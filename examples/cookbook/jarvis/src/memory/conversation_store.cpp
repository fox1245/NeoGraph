// jarvis/src/memory/conversation_store.cpp
//
// MemoryLookupNode  — at the start of each turn, reads the last N turns, user
//   preferences, and last topic from the Store and pushes them to memory_context.
//
// MemoryCommitNode  — at the end of each turn, appends user_text, final_text,
//   and the names of tools used to the Store's turns list.
//
// Store namespace layout (default "jarvis.tony"):
//   <ns>.turns      — append-only list of conversation turns (stored as one JSON array)
//   <ns>.prefs      — user preferences key-value object
//   <ns>.last_topic — short topic summary string

#include "conversation_store.h"

#include <chrono>
#include <iostream>
#include <string>
#include <vector>

namespace jarvis::memory {

// ─────────────────────────────────────────────────────────────────────────────
// Internal helpers — convert a dotted namespace string to a Store Namespace vector
// ─────────────────────────────────────────────────────────────────────────────

namespace {

/// Split a dot-separated string such as "jarvis.tony" into {"jarvis", "tony"}.
/// Store uses a hierarchical std::vector<std::string> Namespace.
neograph::graph::Namespace split_ns(const std::string& ns_str) {
    neograph::graph::Namespace parts;
    std::string                buf;
    for (char c : ns_str) {
        if (c == '.') {
            if (!buf.empty()) { parts.push_back(buf); buf.clear(); }
        } else {
            buf += c;
        }
    }
    if (!buf.empty()) parts.push_back(buf);
    return parts;
}

/// Return the current time as Unix epoch seconds.
int64_t now_epoch_seconds() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

/// Trim leading and trailing whitespace — used for verbatim repeat comparisons.
std::string trim_ws(const std::string& s) {
    const char* ws = " \t\r\n";
    auto b = s.find_first_not_of(ws);
    if (b == std::string::npos) return "";
    auto e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}

/// Extract tool names from the tool_results channel value.
/// Handles both array ([{"tool":"x",...},...]) and object ({"tool":"x",...})
/// shapes.
std::vector<std::string> extract_tool_names(const neograph::json& tool_results) {
    std::vector<std::string> names;
    if (tool_results.is_null()) return names;

    auto try_name = [&](const neograph::json& item) {
        // Try keys in order: "tool", "tool_name", "name"
        for (const char* key : {"tool", "tool_name", "name"}) {
            if (item.contains(key) && item[key].is_string()) {
                names.push_back(item[key].get<std::string>());
                return;
            }
        }
    };

    if (tool_results.is_array()) {
        for (const auto& item : tool_results) {
            if (item.is_object()) try_name(item);
        }
    } else if (tool_results.is_object()) {
        try_name(tool_results);
    }

    return names;
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// MemoryLookupNode — constructor
// ─────────────────────────────────────────────────────────────────────────────

MemoryLookupNode::MemoryLookupNode(std::string name, const neograph::json& cfg)
    : name_(std::move(name))
    , namespace_(cfg.value("store_namespace", std::string("jarvis.tony")))
    , recent_turns_(cfg.value("recent_turns", 6))
{}

// ─────────────────────────────────────────────────────────────────────────────
// MemoryLookupNode::run  — fetch memory from the Store
// ─────────────────────────────────────────────────────────────────────────────

asio::awaitable<neograph::graph::NodeOutput>
MemoryLookupNode::run(neograph::graph::NodeInput in) {
    neograph::graph::NodeOutput out;

    // Return an empty context quietly on a shutdown cycle
    {
        const auto& sd = in.state.get("__shutdown__");
        if (sd.is_boolean() && sd.get<bool>()) {
            out.writes.push_back({
                "memory_context",
                neograph::json{
                    {"recent_turns", neograph::json::array()},
                    {"prefs",        neograph::json::object()},
                    {"last_topic",   ""}
                }
            });
            co_return out;
        }
    }

    // If no Store is attached (ctx.store == nullptr), return an empty memory_context
    if (!in.ctx.store) {
        std::cerr << "[MemoryLookupNode] Store 미설정 — 빈 memory_context 반환\n";
        out.writes.push_back({
            "memory_context",
            neograph::json{
                {"recent_turns", neograph::json::array()},
                {"prefs",        neograph::json::object()},
                {"last_topic",   ""}
            }
        });
        co_return out;
    }

    auto& store     = *in.ctx.store;
    auto  base_ns   = split_ns(namespace_);

    // ── 1) Read the last N turns ────────────────────────────────────────────
    // All turns are stored as a single JSON array under the key "turns".
    // (Store can hold arbitrary JSON per key — see 43_store_personalization.cpp)
    neograph::json recent_turns = neograph::json::array();
    {
        auto turns_ns  = base_ns;           // {"jarvis", "tony"}
        auto item      = store.get(turns_ns, "turns");   // key = "turns"
        if (item && item->value.is_array()) {
            const auto& all = item->value;
            // Collect up to N turns from the end, skipping turns marked with
            // repeat_flag (suspected verbatim repeats) so they do not re-enter
            // context and reinforce a repetition loop.
            std::vector<neograph::json> picked;
            for (int i = static_cast<int>(all.size()) - 1;
                 i >= 0 && static_cast<int>(picked.size()) < recent_turns_; --i) {
                const auto& t = all[i];
                if (t.is_object() && t.value("repeat_flag", false)) continue;
                picked.push_back(t);
            }
            for (auto it = picked.rbegin(); it != picked.rend(); ++it) {
                recent_turns.push_back(*it);   // restore chronological order
            }
        }
    }

    // ── 2) Read user preferences (prefs) ────────────────────────────────────
    neograph::json prefs = neograph::json::object();
    {
        auto item = store.get(base_ns, "prefs");
        if (item && item->value.is_object()) {
            prefs = item->value;
        }
    }

    // ── 3) Read last topic ──────────────────────────────────────────────────
    std::string last_topic;
    {
        auto item = store.get(base_ns, "last_topic");
        if (item && item->value.is_string()) {
            last_topic = item->value.get<std::string>();
        }
    }

    // ── 4) Push to the memory_context channel ───────────────────────────────
    out.writes.push_back({
        "memory_context",
        neograph::json{
            {"recent_turns", recent_turns},
            {"prefs",        prefs},
            {"last_topic",   last_topic}
        }
    });

    co_return out;
}

// ─────────────────────────────────────────────────────────────────────────────
// MemoryCommitNode — constructor
// ─────────────────────────────────────────────────────────────────────────────

MemoryCommitNode::MemoryCommitNode(std::string name, const neograph::json& cfg)
    : name_(std::move(name))
    , namespace_(cfg.value("store_namespace", std::string("jarvis.tony")))
{}

// ─────────────────────────────────────────────────────────────────────────────
// MemoryCommitNode::run  — append the current turn to Store turns
// ─────────────────────────────────────────────────────────────────────────────

asio::awaitable<neograph::graph::NodeOutput>
MemoryCommitNode::run(neograph::graph::NodeInput in) {
    neograph::graph::NodeOutput out;  // side effect only — no channel writes

    // Skip quietly on a shutdown cycle
    {
        const auto& sd = in.state.get("__shutdown__");
        if (sd.is_boolean() && sd.get<bool>()) {
            co_return out;
        }
    }

    // No-op if no Store is attached (do not fail the graph)
    if (!in.ctx.store) {
        std::cerr << "[MemoryCommitNode] Store 미설정 — 턴 저장 건너뜀\n";
        co_return out;
    }

    auto& store   = *in.ctx.store;
    auto  base_ns = split_ns(namespace_);

    // ── Read current turn data from state channels ──────────────────────────
    auto user_text_v  = in.state.get("user_text");
    auto user_lang_v  = in.state.get("user_lang");
    auto final_text_v = in.state.get("final_text");
    auto tool_res_v   = in.state.get("tool_results");

    std::string user_text  = user_text_v.is_string()
                             ? user_text_v.get<std::string>()  : "";
    std::string user_lang  = user_lang_v.is_string()
                             ? user_lang_v.get<std::string>()  : "";
    std::string final_text = final_text_v.is_string()
                             ? final_text_v.get<std::string>() : "";

    // Empty-turn commit guard — do not store turns where user_text or final_text
    // is empty. (Prevents empty turns caused by mic noise, STT failure, or tool
    // errors from polluting memory and filling the 24-turn cap, which would evict
    // real conversation history.)
    {
        auto trim = [](const std::string& s) {
            auto b = s.find_first_not_of(" \t\r\n");
            return b == std::string::npos ? std::string() : s.substr(b);
        };
        if (trim(user_text).empty() || trim(final_text).empty()) {
            std::cerr << "[MemoryCommitNode] 빈 턴 — 저장 건너뜀 "
                         "(user_text 또는 final_text 비어있음)\n";
            co_return out;
        }
    }

    // Extract names of tools used in this turn
    auto tool_names = extract_tool_names(tool_res_v);
    neograph::json tools_json = neograph::json::array();
    for (const auto& t : tool_names) tools_json.push_back(t);

    // ── Build new turn object ───────────────────────────────────────────────
    neograph::json new_turn = {
        {"user_text",  user_text},
        {"lang",       user_lang},
        {"final_text", final_text},
        {"tools_used", tools_json},
        {"ts",         now_epoch_seconds()}
    };

    // ── Read existing turns, append new turn, and write back ────────────────
    // Trim from the front when the list exceeds recent_turns (default 6) * 4 = 24
    // to bound Store size and drop stale history.
    {
        neograph::json all_turns = neograph::json::array();

        auto existing = store.get(base_ns, "turns");
        if (existing && existing->value.is_array()) {
            all_turns = existing->value;
        }

        // Flag suspected verbatim repeats — if final_text matches an earlier turn
        // verbatim for a different user_text, set repeat_flag=true. The turn is
        // still stored, but MemoryLookupNode skips it on recall to break self-reinforcing
        // repetition loops (identical answers to identical questions are not flagged).
        {
            const std::string ft = trim_ws(final_text);
            if (!ft.empty()) {
                for (const auto& t : all_turns) {
                    if (!t.is_object()) continue;
                    if (trim_ws(t.value("final_text", std::string(""))) == ft &&
                        trim_ws(t.value("user_text",  std::string(""))) !=
                            trim_ws(user_text)) {
                        new_turn["repeat_flag"] = true;
                        std::cerr << "[MemoryCommitNode] 복창 의심 턴 — "
                                     "repeat_flag=true (회상 제외 대상)\n";
                        break;
                    }
                }
            }
        }

        all_turns.push_back(new_turn);

        // Heuristic: max retained turns = recent_turns_ * 4 (default 24).
        // MemoryLookupNode reads recent_turns_ items, so keeping 4x is sufficient;
        // pinned to 24 so MemoryCommitNode works standalone as well.
        constexpr int kMaxKeep = 24;
        int total = static_cast<int>(all_turns.size());
        if (total > kMaxKeep) {
            // Drop oldest entries from the front — keep only the last kMaxKeep
            neograph::json trimmed = neograph::json::array();
            for (int i = total - kMaxKeep; i < total; ++i) {
                trimmed.push_back(all_turns[i]);
            }
            all_turns = std::move(trimmed);
        }

        store.put(base_ns, "turns", all_turns);
    }

    co_return out;
}

}  // namespace jarvis::memory
