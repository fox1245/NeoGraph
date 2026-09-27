// Schema-configured SSE decoding, independent of HTTP/WebSocket ownership.
#include <neograph/llm/schema_provider.h>

#include <stdexcept>
#include <utility>

namespace neograph::llm {

bool SchemaProvider::consume_stream_line(StreamParseState& state,
                                         const std::string& line,
                                         const StreamCallback& on_chunk) const {
    auto& completion = state.completion;
    auto& full_content = state.full_content;
    auto& tc_map = state.tc_map;
    auto& observed_stop_reason = state.observed_stop_reason;
    auto& event_blocks = state.event_blocks;
    auto& event_block_index = state.event_block_index;
    auto& gemini_tc_index = state.gemini_tc_index;
    auto& current_event_type = state.current_event_type;
    auto& terminal_event_seen = state.terminal_event_seen;
    using EventBlock = StreamParseState::EventBlock;

    // `continue` in this block skips a fixture line, not a network read.
    do {
        // --- SSE_DATA format (OpenAI / Gemini) ---
        if (stream_.format == StreamFormat::SSE_DATA) {
            if (line.rfind(stream_.prefix, 0) != 0) continue;
            std::string payload = line.substr(stream_.prefix.size());

            if (!stream_.done_signal.empty() && payload == stream_.done_signal) {
                terminal_event_seen = true;
                return false;
            }
            if (payload.empty()) continue;

            try {
                auto j = json::parse(payload);
                if (const auto reason = parse_stream_stop_reason(j); !reason.empty()) {
                    observed_stop_reason = reason;
                }

                // Usage capture: OpenAI-compatible endpoints emit
                // `usage` on the FINAL chunk (with empty choices,
                // enabled by include_usage). Gemini emits
                // `usageMetadata` on EVERY chunk. Either way,
                // overwrite-on-seen gives us the latest numbers.
                if (!resp_.usage_path.empty()) {
                    auto u = json_path::at_path(j, resp_.usage_path);
                    if (u && u->is_object()) {
                        int p = u->value(resp_.prompt_tokens_field, 0);
                        int c = u->value(resp_.completion_tokens_field, 0);
                        if (p > 0) completion.usage.prompt_tokens = p;
                        if (c > 0) completion.usage.completion_tokens = c;
                        if (!resp_.total_tokens_field.empty()) {
                            int t = u->value(resp_.total_tokens_field, 0);
                            if (t > 0) completion.usage.total_tokens = t;
                        }
                    }
                }

                if (stream_.delta_strategy == "candidates_parts") {
                    // Gemini streaming: candidates[0].content.parts[]
                    auto parts = json_path::at_path(j, stream_.delta_parts_path);
                    if (parts && parts->is_array()) {
                        for (const auto& part : *parts) {
                            if (part.contains(stream_.delta_text_field) &&
                                part[stream_.delta_text_field].is_string()) {
                                std::string token = part[stream_.delta_text_field].get<std::string>();
                                full_content += token;
                                if (on_chunk) on_chunk(token);
                            }
                            if (part.contains(stream_.delta_function_call_field)) {
                                const auto& fc = part[stream_.delta_function_call_field];
                                ToolCall call;
                                call.id = generate_tool_call_id();
                                call.name = fc.value(stream_.delta_tool_call_name_field, "");
                                if (fc.contains(stream_.delta_tool_call_args_field)) {
                                    call.arguments = fc[stream_.delta_tool_call_args_field].dump();
                                }
                                tc_map[gemini_tc_index++] = call;
                            }
                        }
                    }
                } else {
                    // OpenAI streaming: choices[0].delta
                    auto delta = json_path::at_path(j, stream_.delta_path);
                    if (!delta) continue;

                    // Content token
                    if (delta->contains(stream_.content_field) &&
                        !(*delta)[stream_.content_field].is_null()) {
                        std::string token = (*delta)[stream_.content_field].get<std::string>();
                        full_content += token;
                        if (on_chunk) on_chunk(token);
                    }

                    // Reasoning deltas are part of the returned
                    // message, never public content/on_chunk output.
                    for (const auto& field : stream_.reasoning_fields) {
                        if (delta->contains(field) && (*delta)[field].is_string()) {
                            completion.message.reasoning +=
                                (*delta)[field].get<std::string>();
                            break;
                        }
                    }

                    // Tool calls (streamed incrementally)
                    if (delta->contains(stream_.tool_calls_field)) {
                        for (const auto& tc : (*delta)[stream_.tool_calls_field]) {
                            int idx = tc.value(stream_.tool_call_index_field, 0);
                            if (tc.contains(stream_.tool_call_id_field)) {
                                tc_map[idx].id = tc[stream_.tool_call_id_field].get<std::string>();
                            }
                            auto name_node = json_path::at_path(tc, stream_.tool_call_name_path);
                            if (name_node && name_node->is_string()) {
                                tc_map[idx].name += name_node->get<std::string>();
                            }
                            auto args_node = json_path::at_path(tc, stream_.tool_call_args_path);
                            if (args_node && args_node->is_string()) {
                                tc_map[idx].arguments += args_node->get<std::string>();
                            }
                        }
                    }
                }
            } catch (...) {
                // Skip malformed chunks
            }
        }

        // --- SSE_EVENTS format (Claude) ---
        else if (stream_.format == StreamFormat::SSE_EVENTS) {
            if (line.empty()) {
                current_event_type.clear();
                continue;
            }

            // Parse "event: <type>" lines
            if (line.rfind("event: ", 0) == 0) {
                current_event_type = line.substr(7);
                continue;
            }

            // Parse "data: <json>" lines
            if (line.rfind("data: ", 0) != 0) continue;
            std::string payload = line.substr(6);
            if (payload.empty()) continue;
            if (!stream_.done_signal.empty() && payload == stream_.done_signal) {
                terminal_event_seen = true;
                return false;
            }

            try {
                auto j = json::parse(payload);
                if (const auto reason = parse_stream_stop_reason(j); !reason.empty()) {
                    observed_stop_reason = reason;
                }

                // Prefer the explicit SSE event field. OpenRouter's
                // Responses API sends data-only records, so fall back to
                // the JSON type only when no event field was supplied.
                std::string event_type = current_event_type;
                current_event_type.clear();
                if (event_type.empty() && j.contains("type") &&
                    j["type"].is_string()) {
                    event_type = j["type"].get<std::string>();
                }
                if (!stream_.events_config.contains(event_type)) continue;
                auto event_cfg = stream_.events_config[event_type];
                std::string action = event_cfg.value("action", "ignore");

                if (action == "ignore") {
                    // noop
                }
                else if (action == "usage") {
                    // SSE event dedicated to emitting usage numbers.
                    // Schema declares `prompt_path` / `completion_path`
                    // / `total_path` relative to the event's JSON
                    // payload; any that resolves to a non-zero integer
                    // overwrites the cumulative usage.
                    auto read_int = [&](const std::string& p) -> int {
                        if (p.empty()) return 0;
                        auto v = json_path::at_path(j, p);
                        if (!v || !v->is_number_integer()) return 0;
                        return v->template get<int>();
                    };
                    int p = read_int(event_cfg.value("prompt_path", ""));
                    int c = read_int(event_cfg.value("completion_path", ""));
                    int t = read_int(event_cfg.value("total_path", ""));
                    if (p > 0) completion.usage.prompt_tokens = p;
                    if (c > 0) completion.usage.completion_tokens = c;
                    if (t > 0) completion.usage.total_tokens = t;
                }
                else if (action == "block_start") {
                    // New content block / output item starting.
                    // Claude default: block_path="content_block", tool_type="tool_use", id in "id".
                    // Responses:      block_path="item",          tool_type="function_call", id in "call_id".
                    std::string block_path = event_cfg.value("block_path", "content_block");
                    std::string type_field = event_cfg.value("type_field", "type");
                    std::string tool_type   = event_cfg.value("tool_call_type", "tool_use");
                    std::string id_fld      = event_cfg.value("id_field", "id");
                    std::string name_fld    = event_cfg.value("name_field", "name");

                    auto block = json_path::at_path(j, block_path);
                    if (block) {
                        EventBlock cb;
                        cb.type = block->value(type_field, "");
                        cb.index = static_cast<int>(event_blocks.size());
                        if (cb.type == tool_type) {
                            cb.id = block->value(id_fld, "");
                            cb.name = block->value(name_fld, "");
                        }
                        event_blocks.push_back(cb);
                        event_block_index = cb.index;
                    }
                }
                else if (action == "delta") {
                    // Claude-style delta: single event carries both text and tool args,
                    // discriminated by a nested "type" field inside the delta object.
                    if (event_block_index < 0 ||
                        event_block_index >= static_cast<int>(event_blocks.size())) continue;

                    auto& cur_block = event_blocks[event_block_index];
                    std::string delta_path = event_cfg.value("delta_path", "delta");
                    auto delta = json_path::at_path(j, delta_path);
                    if (!delta) continue;

                    std::string delta_type = delta->value("type", "");
                    std::string text_delta_type = event_cfg.value("text_delta_type", "text_delta");
                    std::string tool_delta_type = event_cfg.value("tool_delta_type", "input_json_delta");

                    if (delta_type == text_delta_type) {
                        std::string text_fld = event_cfg.value("text_field", "text");
                        std::string token = delta->value(text_fld, "");
                        full_content += token;
                        if (on_chunk) on_chunk(token);
                    }
                    else if (delta_type == tool_delta_type) {
                        std::string args_fld = event_cfg.value("tool_args_field", "partial_json");
                        std::string chunk = delta->value(args_fld, "");
                        cur_block.args += chunk;
                    }
                }
                else if (action == "text_delta") {
                    // Responses-style: dedicated text-delta event.
                    // Reads a single field directly from the event payload.
                    if (event_block_index < 0 ||
                        event_block_index >= static_cast<int>(event_blocks.size())) continue;
                    std::string delta_fld = event_cfg.value("delta_field", "delta");
                    std::string token = j.value(delta_fld, "");
                    full_content += token;
                    if (on_chunk) on_chunk(token);
                }
                else if (action == "tool_args_delta") {
                    // Responses-style: dedicated function-call-arguments delta event.
                    if (event_block_index < 0 ||
                        event_block_index >= static_cast<int>(event_blocks.size())) continue;
                    auto& cur_block = event_blocks[event_block_index];
                    std::string delta_fld = event_cfg.value("delta_field", "delta");
                    cur_block.args += j.value(delta_fld, "");
                }
                else if (action == "block_stop") {
                    // Block / item finished.
                    std::string tool_type = event_cfg.value("tool_call_type", "tool_use");
                    if (event_block_index >= 0 &&
                        event_block_index < static_cast<int>(event_blocks.size())) {
                        auto& cb = event_blocks[event_block_index];
                        if (cb.type == tool_type) {
                            ToolCall call;
                            call.id = cb.id;
                            call.name = cb.name;
                            call.arguments = cb.args;
                            tc_map[cb.index] = call;
                        }
                    }
                    event_block_index = -1;
                }
                else if (action == "done") {
                    // OpenAI Responses' `response.completed` event
                    // carries the full response object (including
                    // `usage`) under the "response" key. Reuse
                    // parse_usage so the streaming and non-stream
                    // paths report identical numbers — without this,
                    // every streamed call returned usage={0,0,0}
                    // even though the server sent input/output
                    // token counts on the final event.
                    if (j.contains("response") &&
                        j["response"].is_object()) {
                        completion.usage =
                            parse_usage(j["response"]);
                    }
                    terminal_event_seen = true;
                }
            } catch (...) {
                // Skip malformed
            }
            if (terminal_event_seen) return false;
        }
    } while (false);
    return true;
}

void SchemaProvider::consume_ws_event(StreamParseState& state, const json& j,
                                      const StreamCallback& on_chunk) const {
    auto& completion = state.completion;
    auto& full_content = state.full_content;
    auto& tc_map = state.tc_map;
    auto& observed_stop_reason = state.observed_stop_reason;
    auto& event_blocks = state.event_blocks;
    auto& event_block_index = state.event_block_index;
    const auto& events_config = stream_.events_config;
    using EventBlock = StreamParseState::EventBlock;

    auto read_usage_into = [&](const json& container) {
        if (resp_.usage_path.empty()) return;
        auto u = json_path::at_path(container, resp_.usage_path);
        if (!u || !u->is_object()) return;
        int p = u->value(resp_.prompt_tokens_field, 0);
        int c = u->value(resp_.completion_tokens_field, 0);
        if (p > 0) completion.usage.prompt_tokens = p;
        if (c > 0) completion.usage.completion_tokens = c;
        if (!resp_.total_tokens_field.empty()) {
            int t = u->value(resp_.total_tokens_field, 0);
            if (t > 0) completion.usage.total_tokens = t;
        }
        if (completion.usage.total_tokens == 0) {
            completion.usage.total_tokens =
                completion.usage.prompt_tokens +
                completion.usage.completion_tokens;
        }
    };

    // A single frame is a value-level event; `continue` skips that frame.
    do {
        std::string event_type = j.value("type", "");
        if (event_type.empty()) continue;
        if (const auto reason = parse_stream_stop_reason(j); !reason.empty()) {
            observed_stop_reason = reason;
        }

        // Server-side error frames terminate the stream with detail.
        if (event_type == "error") {
            std::string err_msg = j.value("message", "");
            if (err_msg.empty() && j.contains("error") && j["error"].is_object()) {
                err_msg = j["error"].value("message", "unknown");
            }
            throw std::runtime_error(
                "openai-responses ws error: " + err_msg);
        }

        if (!events_config.contains(event_type)) continue;
        const auto& event_cfg = events_config[event_type];
        const std::string action = event_cfg.value("action", "ignore");

        if (action == "ignore") {
            continue;
        } else if (action == "block_start") {
            // Mirrors complete_stream's block_start: openai-responses
            // schema sets block_path="item", tool_call_type="function_call",
            // id_field="call_id".
            std::string block_path = event_cfg.value("block_path", "item");
            std::string type_field = event_cfg.value("type_field", "type");
            std::string tool_type   = event_cfg.value("tool_call_type", "function_call");
            std::string id_fld      = event_cfg.value("id_field", "call_id");
            std::string name_fld    = event_cfg.value("name_field", "name");

            auto block = json_path::at_path(j, block_path);
            if (block) {
                EventBlock cb;
                cb.type = block->value(type_field, "");
                cb.index = static_cast<int>(event_blocks.size());
                if (cb.type == tool_type) {
                    cb.id   = block->value(id_fld, "");
                    cb.name = block->value(name_fld, "");
                }
                event_blocks.push_back(cb);
                event_block_index = cb.index;
            }
        } else if (action == "text_delta") {
            if (event_block_index < 0 ||
                event_block_index >= static_cast<int>(event_blocks.size())) continue;
            std::string fld = event_cfg.value("delta_field", "delta");
            std::string token = j.value(fld, "");
            full_content += token;
            if (on_chunk) on_chunk(token);
        } else if (action == "tool_args_delta") {
            if (event_block_index < 0 ||
                event_block_index >= static_cast<int>(event_blocks.size())) continue;
            auto& cur = event_blocks[event_block_index];
            std::string fld = event_cfg.value("delta_field", "delta");
            cur.args += j.value(fld, "");
        } else if (action == "block_stop") {
            std::string tool_type = event_cfg.value("tool_call_type", "function_call");
            if (event_block_index >= 0 &&
                event_block_index < static_cast<int>(event_blocks.size())) {
                auto& cb = event_blocks[event_block_index];
                if (cb.type == tool_type) {
                    ToolCall call;
                    call.id        = cb.id;
                    call.name      = cb.name;
                    call.arguments = cb.args;
                    tc_map[cb.index] = call;
                }
            }
            event_block_index = -1;
        } else if (action == "done") {
            // response.completed carries the full response object
            // (including usage) under "response". Same shape as the
            // HTTP path so we reuse the usage extractor.
            if (j.contains("response") && j["response"].is_object()) {
                read_usage_into(j["response"]);
            }
            state.terminal_event_seen = true;
        }
        // Other action values (delta/usage etc.) are Anthropic shapes
        // that the openai-responses schema doesn't emit — ignore.
    } while (false);
}

ChatCompletion SchemaProvider::finish_stream(StreamParseState& state) const {
    state.completion.message.content = std::move(state.full_content);
    for (auto& [_, tc] : state.tc_map) {
        state.completion.message.tool_calls.push_back(std::move(tc));
    }
    state.completion.stop_reason = state.observed_stop_reason.empty()
        ? (state.completion.message.tool_calls.empty() ? resp_.default_stop_reason : "tool_use")
        : state.observed_stop_reason;
    return std::move(state.completion);
}

} // namespace neograph::llm
