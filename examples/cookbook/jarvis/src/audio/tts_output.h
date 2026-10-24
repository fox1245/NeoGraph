// jarvis/src/audio/tts_output.h
//
// "supertonic_tts" graph node: final_text + user_lang → 44.1kHz PCM → speaker.
//
// Embeds supertonic's cpp/helper.{h,cpp} directly and runs inference on ONNX
// Runtime. CPU-only, 99M-parameter model, MIT license.
//
// Uses the detected language code on the user_lang channel (ko/en/...) as the
// lang argument when present, falling back to "na" (language-agnostic).
//
// Registration (main.cpp):
//   NodeFactory::register_type("supertonic_tts", ...);
//
// JSON configuration:
//   { "type": "supertonic_tts",
//     "onnx_dir": "assets/onnx",
//     "voice_style": "assets/voices/M1.json",
//     "total_steps": 8, "speed": 1.05,
//     "play_audio": true }        // speaker playback (default true; forced off by JARVIS_NO_PLAYBACK=1)
//
// Input channels: final_text (string), user_lang (string?, optional)
// Output channel: tts_audio (playback completion timestamp or empty — speaker output is a side effect)
//
// Implementation notes:
//   - Embedded supertonic helper.cpp (loads models from assets/onnx)
//   - PCM playback via miniaudio playback device
//   - Optional wav file output alongside blocking speaker playback
#pragma once

#include <neograph/neograph.h>

#ifdef JARVIS_HAVE_SUPERTONIC
#  include <memory>
#  include "helper.h"        // supertonic: TextToSpeech, Style, Ort::Env, Ort::MemoryInfo
#  include <onnxruntime_cxx_api.h>
#endif

namespace jarvis::audio {

class SupertonicTtsNode : public neograph::graph::GraphNode {
  public:
    SupertonicTtsNode(std::string name, const neograph::json& cfg);
    ~SupertonicTtsNode() override;

    asio::awaitable<neograph::graph::NodeOutput>
    run(neograph::graph::NodeInput in) override;

    std::string get_name() const override { return name_; }

  private:
    std::string name_;
    std::string onnx_dir_;
    std::string voice_style_path_;
    int total_steps_;
    float speed_;
    bool play_audio_ = true;  // play on the default output device after synthesis
    int turn_counter_ = 0;  // sequential counter for output wav files

#ifdef JARVIS_HAVE_SUPERTONIC
    // ONNX environment — kept alive for the lifetime of the node (no reloading)
    std::unique_ptr<Ort::Env>        ort_env_;
    std::unique_ptr<Ort::MemoryInfo> ort_meminfo_;
    // supertonic TextToSpeech instance (holds 4 ONNX sessions internally)
    std::unique_ptr<TextToSpeech>    tts_;
    // Voice style (e.g. M1.json)
    std::unique_ptr<Style>           style_;
#endif
};

}  // namespace jarvis::audio
