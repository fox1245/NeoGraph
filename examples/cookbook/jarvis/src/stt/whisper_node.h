// jarvis/src/stt/whisper_node.h
//
// "whisper_stt" graph node: voice_in PCM → (text, lang) → user_text + user_lang.
//
// Dependency: libwhisper from whisper.cpp. Follows the standard whisper.cpp flow:
// whisper_init_from_file_with_params → whisper_full → segment text decoding.
//
// language="auto" computes language token probabilities from the first 30s mel
// spectrogram across 99 languages and outputs the top ISO code (ko/en/ja/...).
// That code is written to the user_lang channel and propagated through to TTS.
//
// Registration (main.cpp):
//   NodeFactory::register_type("whisper_stt", ...);
//
// JSON configuration:
//   { "type": "whisper_stt",
//     "model_path": "assets/whisper-small.bin",
//     "language": "auto",
//     "translate_to_en": false }
//
// Input channel: voice_in (pcm + sample_rate)
// Output channels: user_text (string), user_lang (string, 2-letter ISO code)
//
// Implementation notes:
//   - Load model once via whisper_init_from_file_with_params
//   - Configure whisper_full_params with language="auto" and greedy decoding
//   - Extract detected language code via whisper_full_lang_id()
//   - Return empty text on silent/noisy clips so downstream nodes short-circuit
#pragma once

#include <neograph/neograph.h>

namespace jarvis::stt {

class WhisperSttNode : public neograph::graph::GraphNode {
  public:
    WhisperSttNode(std::string name, const neograph::json& cfg);
    ~WhisperSttNode() override;

    asio::awaitable<neograph::graph::NodeOutput>
    run(neograph::graph::NodeInput in) override;

    std::string get_name() const override { return name_; }

  private:
    std::string name_;
    std::string model_path_;
    std::string language_;  // "auto" or ISO code
    bool translate_to_en_;
    // whisper_context* is associated via a file-static map in whisper_node.cpp
};

}  // namespace jarvis::stt
