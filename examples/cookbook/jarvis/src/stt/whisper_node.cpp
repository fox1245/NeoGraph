// jarvis/src/stt/whisper_node.cpp
//
// WhisperSttNode implementation.
//
// Calls whisper.cpp when JARVIS_HAVE_WHISPER=1; otherwise runs in mock mode
// (text passthrough + Hangul codepoint language heuristic).
//
// Two input modes (voice_in channel):
//   Text mode: {"mock_text": "...", "pcm": [], "sample_rate": 0}
//              — skips STT and copies mock_text directly to user_text
//   WAV mode : {"mock_text": "", "pcm": [float...], "sample_rate": 16000}
//              — runs speech recognition via whisper.cpp
//
// To avoid exposing whisper_context* in whisper_node.h, a file-static map
// associates each node instance with its whisper_context*.

#include "whisper_node.h"

#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// whisper.cpp integration — guarded by JARVIS_HAVE_WHISPER
// ─────────────────────────────────────────────────────────────────────────────

#ifdef JARVIS_HAVE_WHISPER
#  include <whisper.h>
#endif

namespace jarvis::stt {

// ─────────────────────────────────────────────────────────────────────────────
// File-static map: node instance → whisper_context*
//
// Keeps whisper_context* out of the public header; entries are erased when the
// node is destroyed.
// ─────────────────────────────────────────────────────────────────────────────

#ifdef JARVIS_HAVE_WHISPER
namespace {
std::unordered_map<WhisperSttNode*, whisper_context*> g_ctx_map;
}  // namespace
#endif

// ─────────────────────────────────────────────────────────────────────────────
// Constructor — store cfg settings and load the model once
// ─────────────────────────────────────────────────────────────────────────────

WhisperSttNode::WhisperSttNode(std::string name, const neograph::json& cfg)
    : name_(std::move(name))
    , model_path_(cfg.value("model_path", std::string("assets/whisper-small.bin")))
    , language_(cfg.value("language",     std::string("auto")))
    , translate_to_en_(cfg.value("translate_to_en", false))
{
#ifdef JARVIS_HAVE_WHISPER
    // Load the model once in the constructor (expensive initialization)
    whisper_context_params cparams = whisper_context_default_params();
    whisper_context* ctx = whisper_init_from_file_with_params(model_path_.c_str(), cparams);
    if (ctx) {
        g_ctx_map[this] = ctx;
        std::cout << "[whisper] 모델 로드 완료: " << model_path_ << "\n";
    } else {
        std::cerr << "[whisper] 모델 로드 실패: " << model_path_
                  << " — mock 모드로 동작\n";
        // ctx == nullptr → not registered in g_ctx_map → run() falls back to mock path
    }
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
// Destructor — free whisper_context and remove from map
// ─────────────────────────────────────────────────────────────────────────────

WhisperSttNode::~WhisperSttNode() {
#ifdef JARVIS_HAVE_WHISPER
    auto it = g_ctx_map.find(this);
    if (it != g_ctx_map.end()) {
        whisper_free(it->second);
        g_ctx_map.erase(it);
    }
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
// Language detection heuristic (mock only) — returns "ko" if Hangul syllables
// (U+AC00–U+D7A3) are present; whisper_full_lang_id() is used when whisper is active.
// ─────────────────────────────────────────────────────────────────────────────

namespace {

std::string detect_language_heuristic(const std::string& text) {
    if (text.empty()) return "en";

    // UTF-8 Hangul syllable range: 0xEA 0xB0 0x80 (U+AC00) .. 0xED 0x9E 0xA3 (U+D7A3),
    // 3-byte sequences whose lead byte is in 0xEA..0xED.
    for (size_t i = 0; i + 2 < text.size(); ) {
        unsigned char b0 = static_cast<unsigned char>(text[i]);
        if (b0 >= 0xEA && b0 <= 0xED) {
            unsigned char b1 = static_cast<unsigned char>(text[i + 1]);
            unsigned char b2 = static_cast<unsigned char>(text[i + 2]);
            // U+AC00 = EA B0 80, U+D7A3 = ED 9E A3
            uint32_t cp = ((b0 & 0x0F) << 12) | ((b1 & 0x3F) << 6) | (b2 & 0x3F);
            if (cp >= 0xAC00 && cp <= 0xD7A3) {
                return "ko";
            }
            i += 3;
        } else if (b0 < 0x80) {
            // ASCII — 1 byte
            ++i;
        } else if (b0 < 0xC0) {
            // UTF-8 continuation byte — advance to next byte
            ++i;
        } else if (b0 < 0xE0) {
            i += 2;
        } else if (b0 < 0xF0) {
            i += 3;
        } else {
            i += 4;
        }
    }
    return "en";
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// run() — read voice_in packet and write user_text + user_lang channels
//
// Branches:
//   1) Non-empty PCM + JARVIS_HAVE_WHISPER → run whisper.cpp (WAV mode)
//   2) Empty PCM + mock_text present       → pass text through (text mode)
//   3) Both empty                          → pass empty user_text + "en" (EOF)
// ─────────────────────────────────────────────────────────────────────────────

asio::awaitable<neograph::graph::NodeOutput>
WhisperSttNode::run(neograph::graph::NodeInput in)
{
    neograph::graph::NodeOutput out;

    // Read packet from the voice_in channel
    const auto& voice_packet = in.state.get("voice_in");

    // ── Extract PCM data ───────────────────────────────────────────────────
    std::vector<float> pcm;
    int sample_rate = 0;

    if (voice_packet.is_object()) {
        if (voice_packet.contains("pcm") && voice_packet["pcm"].is_array()) {
            const auto& pcm_json = voice_packet["pcm"];
            pcm.reserve(pcm_json.size());
            for (const auto& v : pcm_json) {
                pcm.push_back(v.get<float>());
            }
        }
        if (voice_packet.contains("sample_rate") && voice_packet["sample_rate"].is_number()) {
            sample_rate = voice_packet["sample_rate"].get<int>();
        }
    }

    // ── Dispatch by input mode ─────────────────────────────────────────────

    std::string text;
    std::string lang;

#ifdef JARVIS_HAVE_WHISPER
    auto ctx_it = g_ctx_map.find(this);
    whisper_context* ctx = (ctx_it != g_ctx_map.end()) ? ctx_it->second : nullptr;

    if (!pcm.empty() && ctx) {
        // ── WAV mode: whisper.cpp speech recognition ──────────────────────

        std::cout << "[whisper] STT 시작 (PCM " << pcm.size()
                  << " samples, lang=" << language_ << ")\n";

        // Configure recognition parameters — greedy decoding for low latency
        whisper_full_params wparams =
            whisper_full_default_params(WHISPER_SAMPLING_GREEDY);

        // Language setting: auto-detect when "auto", otherwise pin ISO code.
        // Note: in whisper.cpp, detect_language=true means "detect language only
        // and exit without transcribing", which leaves user_text empty. To both
        // auto-detect language and transcribe, set language="auto" and
        // detect_language=false.
        if (language_ == "auto") {
            wparams.language        = "auto";
            wparams.detect_language = false;
        } else {
            wparams.language        = language_.c_str();
            wparams.detect_language = false;
        }

        // Whether to translate into English
        wparams.translate = translate_to_en_;

        // Suppress whisper console progress — keep only [whisper] logs
        wparams.print_progress  = false;
        wparams.print_realtime  = false;
        wparams.print_timestamps = false;

        // Run speech recognition
        int ret = whisper_full(ctx, wparams, pcm.data(), static_cast<int>(pcm.size()));
        if (ret != 0) {
            std::cerr << "[whisper] whisper_full 실패 (code=" << ret << ") — 빈 결과 반환\n";
            // On failure, return empty text + "en"
            lang = "en";
        } else {
            // Concatenate text across all segments
            int n_seg = whisper_full_n_segments(ctx);
            for (int i = 0; i < n_seg; ++i) {
                const char* seg_text = whisper_full_get_segment_text(ctx, i);
                if (seg_text) {
                    text += seg_text;
                }
            }

            // Extract detected language code — int id → ISO string ("ko", "en", ...)
            int lang_id = whisper_full_lang_id(ctx);
            const char* lang_str = (lang_id >= 0) ? whisper_lang_str(lang_id) : nullptr;
            lang = (lang_str && lang_str[0] != '\0') ? lang_str : "en";

            std::cout << "[whisper] 인식: \"" << text
                      << "\" (감지된 언어: " << lang << ")\n";
        }
    } else if (ctx == nullptr && !pcm.empty()) {
        // PCM arrived without a whisper_context (model failed to load) —
        // fall back to mock_text if present, otherwise empty text
        if (voice_packet.is_object() && voice_packet.contains("mock_text")) {
            const auto& mt = voice_packet["mock_text"];
            if (mt.is_string()) text = mt.get<std::string>();
        }
        lang = detect_language_heuristic(text);
    } else {
        // No PCM → text mode or EOF
        if (voice_packet.is_object() && voice_packet.contains("mock_text")) {
            const auto& mt = voice_packet["mock_text"];
            if (mt.is_string()) text = mt.get<std::string>();
        }
        lang = detect_language_heuristic(text);
    }
#else
    // ── Mock behavior when built without JARVIS_HAVE_WHISPER ──────────────
    // Extract mock_text field — same behavior as text mode
    if (voice_packet.is_object() && voice_packet.contains("mock_text")) {
        const auto& mt = voice_packet["mock_text"];
        if (mt.is_string()) {
            text = mt.get<std::string>();
        }
    }
    lang = detect_language_heuristic(text);
#endif

    // ── Language hysteresis — stick to the inferred native language ─────────
    // Whisper detects language per utterance, so a short or noisy clip can be
    // misclassified and flip the reply/TTS language. Track native_lang and a
    // consecutive streak in store.prefs: increment streak on a match, decrement
    // on a mismatch, and switch only when streak reaches 0. Single misdetections
    // are ignored; sustained switches follow the new language. Empty turns are ignored.
    auto trim = [](const std::string& s) {
        auto b = s.find_first_not_of(" \t\r\n");
        return b == std::string::npos ? std::string() : s.substr(b);
    };
    if (in.ctx.store && !trim(text).empty()) {
        constexpr int kStreakCap = 4;   // hysteresis inertia cap
        neograph::graph::Namespace ns{"jarvis", "tony"};
        auto& store = *in.ctx.store;
        auto item = store.get(ns, "prefs");
        neograph::json prefs = (item && item->value.is_object())
                               ? item->value : neograph::json::object();
        std::string native = prefs.value("native_lang", std::string(""));
        int streak = prefs.value("lang_streak", 0);
        std::string effective;
        if (native.empty()) {
            native = lang; streak = 1; effective = lang;
        } else if (lang == native) {
            streak = std::min(streak + 1, kStreakCap); effective = native;
        } else {
            if (--streak <= 0) { native = lang; streak = 1; effective = lang; }
            else               { effective = native; }  // ignore single misdetection, keep native
        }
        if (effective != lang) {
            std::cerr << "[whisper] 언어 관성 — 감지='" << lang
                      << "' 이지만 네이티브='" << native << "' 고수 (streak "
                      << streak << ")\n";
        }
        prefs["native_lang"] = native;
        prefs["lang_streak"] = streak;
        store.put(ns, "prefs", prefs);
        lang = effective;
    }

    // ── Write user_text + user_lang channels ───────────────────────────────
    out.writes.push_back({"user_text", text});
    out.writes.push_back({"user_lang", lang});

    co_return out;
}

}  // namespace jarvis::stt
