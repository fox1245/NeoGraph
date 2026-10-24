// jarvis/src/audio/tts_output.cpp
//
// SupertonicTtsNode implementation (guarded by JARVIS_HAVE_SUPERTONIC).
//
// Flow:
//   Constructor → load 4 ONNX models + voice style once
//   run()       → supertonic TextToSpeech::call() → write wav files
//                  /tmp/jarvis-tts-<turn>.wav  (incrementing turn index)
//                  /tmp/jarvis-tts-latest.wav  (always overwritten)
//
// Falls back to the mock output path when JARVIS_HAVE_SUPERTONIC is not defined.

#include "tts_output.h"
#include "audio_playback.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>

#ifdef JARVIS_HAVE_SUPERTONIC
#  include <algorithm>
#  include <stdexcept>
#  include <filesystem>
#  include "helper.h"          // supertonic: loadTextToSpeech, loadVoiceStyle, writeWavFile, AVAILABLE_LANGS
#endif

namespace jarvis::audio {

// ─────────────────────────────────────────────────────────────────────────────
// Constructor — parse cfg + load models (when supertonic is enabled)
// ─────────────────────────────────────────────────────────────────────────────

SupertonicTtsNode::SupertonicTtsNode(std::string name, const neograph::json& cfg)
    : name_(std::move(name))
    , onnx_dir_(cfg.value("onnx_dir",       std::string("assets/onnx")))
    , voice_style_path_(cfg.value("voice_style", std::string("assets/voices/M1.json")))
    , total_steps_(cfg.value("total_steps", 8))
    , speed_(cfg.value("speed",             1.05f))
    , play_audio_(cfg.value("play_audio",   true))
{
    // Force-disable via env — useful for headless CI and scripted runs
    if (const char* no_play = std::getenv("JARVIS_NO_PLAYBACK");
        no_play && no_play[0] == '1') {
        play_audio_ = false;
    }
#ifdef JARVIS_HAVE_SUPERTONIC
    try {
        // Initialize ONNX Env + MemoryInfo — reused for the lifetime of the node
        ort_env_   = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "jarvis_tts");
        ort_meminfo_ = std::make_unique<Ort::MemoryInfo>(
            Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault));

        // Load TextToSpeech instance (opens 4 ONNX sessions internally)
        tts_ = loadTextToSpeech(*ort_env_, onnx_dir_, /*use_gpu=*/false);

        // Load voice style
        style_ = std::make_unique<Style>(
            loadVoiceStyle({voice_style_path_}, /*verbose=*/false));

        std::cerr << "[jarvis:tts] 모델 로드 완료 — onnx_dir=" << onnx_dir_
                  << ", 음성=" << voice_style_path_
                  << ", 샘플레이트=" << tts_->getSampleRate() << "Hz\n";
    } catch (const std::exception& e) {
        std::cerr << "[jarvis:tts] 모델 로드 실패 — mock 으로 폴백: " << e.what() << "\n";
        tts_.reset();
        style_.reset();
    }
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
// Destructor
// ─────────────────────────────────────────────────────────────────────────────

SupertonicTtsNode::~SupertonicTtsNode() {
    // unique_ptr members clean up automatically
}

// ─────────────────────────────────────────────────────────────────────────────
// run() — synthesize TTS from final_text + user_lang and write wav files
// ─────────────────────────────────────────────────────────────────────────────

asio::awaitable<neograph::graph::NodeOutput>
SupertonicTtsNode::run(neograph::graph::NodeInput in)
{
    neograph::graph::NodeOutput out;

    // Read reply text from the final_text channel
    const auto& ft_val = in.state.get("final_text");
    std::string final_text = ft_val.is_string() ? ft_val.get<std::string>() : ft_val.dump();

    // user_lang channel — fall back to "na" (language-agnostic) if empty
    const auto& lang_val = in.state.get("user_lang");
    std::string lang = (lang_val.is_string() && !lang_val.get<std::string>().empty())
                       ? lang_val.get<std::string>()
                       : "na";

    // Return immediately on empty text — avoids spurious output on the trailing empty EOF cycle
    if (final_text.empty()) {
        co_return out;
    }

#ifdef JARVIS_HAVE_SUPERTONIC
    // Run live TTS only when the supertonic instance loaded cleanly
    if (tts_ && style_) {
        try {
            // Fall back to "na" if user_lang is not in supertonic's supported language list
            std::string tts_lang = lang;
            bool lang_supported = std::find(
                AVAILABLE_LANGS.begin(), AVAILABLE_LANGS.end(), tts_lang
            ) != AVAILABLE_LANGS.end();
            if (!lang_supported) {
                tts_lang = "na";
            }

            // Synthesize speech
            auto result = tts_->call(*ort_meminfo_, final_text, tts_lang,
                                     *style_, total_steps_, speed_);

            // Output wav paths — write both the numbered file and the latest alias
            int turn = turn_counter_++;
            std::string path_turn   = "/tmp/jarvis-tts-" + std::to_string(turn) + ".wav";
            std::string path_latest = "/tmp/jarvis-tts-latest.wav";

            writeWavFile(path_turn,   result.wav, tts_->getSampleRate());
            writeWavFile(path_latest, result.wav, tts_->getSampleRate());

            // Audio duration — use the first element of duration if present, otherwise compute from sample count
            float duration_sec = result.duration.empty()
                ? static_cast<float>(result.wav.size()) / tts_->getSampleRate()
                : result.duration[0];

            std::cout << "[jarvis:tts][" << lang << "] "
                      << "(생성: " << path_turn << ", "
                      << std::fixed << std::setprecision(1) << duration_sec << "초)\n";
            std::cout.flush();

            // Play on the default playback device — wav files are already saved even if playback fails
            if (play_audio_) {
                if (play_pcm_blocking(result.wav, tts_->getSampleRate())) {
                    std::cerr << "[jarvis:tts] 재생 완료 ("
                              << std::fixed << std::setprecision(1)
                              << duration_sec << "초)\n";
                }
            }

            // Playback completion timestamp
            using namespace std::chrono;
            int64_t ts = duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
            out.writes.push_back({"tts_audio", neograph::json{
                {"played_at",    ts},
                {"wav_path",     path_turn},
                {"duration_sec", duration_sec}
            }});

            co_return out;

        } catch (const std::exception& e) {
            std::cerr << "[jarvis:tts] 합성 오류 — mock 출력으로 폴백: " << e.what() << "\n";
            // Fall through to the mock output below
        }
    }
#endif

    // Mock fallback — when supertonic is unavailable or synthesis failed
    std::cout << "[jarvis:tts][" << lang << "] " << final_text << "\n";
    std::cout.flush();

    using namespace std::chrono;
    int64_t ts = duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
    out.writes.push_back({"tts_audio", neograph::json{{"played_at", ts}}});

    co_return out;
}

}  // namespace jarvis::audio
