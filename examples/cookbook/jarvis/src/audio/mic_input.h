// jarvis/src/audio/mic_input.h
//
// "voice_in" graph node: one microphone utterance → PCM float chunk → voice_in channel.
//
// Dependencies: miniaudio (single-header mic + speaker I/O) and Silero VAD (ONNX).
// miniaudio delivers 16kHz mono PCM via callback; VAD ends the chunk on end-of-speech.
//
// Registration (main.cpp):
//   NodeFactory::instance().register_type("voice_in",
//       [](const std::string& n, const neograph::json& cfg, const NodeContext&) {
//           return std::make_unique<jarvis::audio::MicInputNode>(n, cfg);
//       });
//
// JSON configuration (jarvis_graph.json):
//   { "type": "voice_in", "sample_rate": 16000,
//     "vad_threshold": 0.5, "max_utterance_seconds": 30 }
//
// Output channel: voice_in = { "pcm": <float vector>, "sample_rate": int }
//
// Implementation notes:
//   - miniaudio capture device initialization
//   - Silero VAD ONNX session
//   - Callback-driven chunk accumulation + endpoint detection before returning
//   - Hard cutoff when max_utterance_seconds is exceeded
//   - Passthrough when a background trigger writes text directly to voice_in
#pragma once

#include <neograph/neograph.h>

#include <memory>

namespace jarvis::audio {

// Live microphone capture + Silero VAD (pimpl). The miniaudio capture device
// streams 16kHz mono frames via callback, and the VAD worker thread detects
// speech start/end and pushes completed utterance PCM to a queue. Defined in mic_input.cpp.
class MicCapture;

class MicInputNode : public neograph::graph::GraphNode {
  public:
    MicInputNode(std::string name, const neograph::json& cfg);
    ~MicInputNode() override;

    asio::awaitable<neograph::graph::NodeOutput>
    run(neograph::graph::NodeInput in) override;

    std::string get_name() const override { return name_; }

  private:
    std::string name_;
    int sample_rate_;
    float vad_threshold_;
    int max_utterance_seconds_;

    // Live microphone mode — cfg "use_microphone":true or env JARVIS_MIC=1.
    // Automatically falls back to stdin if device init fails (e.g. no mic on WSL2).
    bool use_mic_ = false;
    std::unique_ptr<MicCapture> capture_;
};

}  // namespace jarvis::audio
