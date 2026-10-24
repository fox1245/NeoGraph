// jarvis/src/stt/moonshine_node.h
//
// "moonshine_stt" graph node — Moonshine-tiny(-ko) ONNX STT.
// Alternative/complement to whisper_stt: voice_in PCM → user_text + user_lang.
//
// Compared to whisper:
//   - Compact 27M model (int8 ~28MB) with an ergodic streaming encoder for low-latency edge use
//   - Takes raw 16kHz waveform input (not mel) with a seq2seq encoder + split 2-model decoder
//   - Language-specific model flavor — tiny-ko is Korean-only, so user_lang is pinned in config
//
// Runtime: ONNX Runtime C++ (reuses the same dependency as supertonic TTS).
// Tokenizer: SentencePiece-style BPE — parses tokenizer.json directly for id→piece,
//   and implements ▁→space + ByteFallback(<0xHH>) + Fuse + strip decoding in C++
//   (no external tokenizer library required).
//
// JSON configuration:
//   { "type": "moonshine_stt",
//     "model_dir": "assets/moonshine-tiny-ko",   // encoder/decoder onnx + tokenizer.json
//     "encoder": "onnx/encoder_model_int8.onnx",
//     "decoder": "onnx/decoder_model_int8.onnx",
//     "decoder_with_past": "onnx/decoder_with_past_model_int8.onnx",
//     "tokenizer": "tokenizer.json",
//     "language": "ko",
//     "max_length": 194 }
//
// Input channel: voice_in (pcm + sample_rate)  |  Outputs: user_text, user_lang
#pragma once

#include <neograph/neograph.h>

#include <memory>
#include <string>
#include <vector>

#ifdef JARVIS_HAVE_ONNXRUNTIME
#  include <onnxruntime_cxx_api.h>
#endif

namespace jarvis::stt {

class MoonshineSttNode : public neograph::graph::GraphNode {
  public:
    MoonshineSttNode(std::string name, const neograph::json& cfg);
    ~MoonshineSttNode() override;

    asio::awaitable<neograph::graph::NodeOutput>
    run(neograph::graph::NodeInput in) override;

    std::string get_name() const override { return name_; }

  private:
    std::string name_;
    std::string language_;
    int         max_length_ = 194;
    bool        ready_ = false;

    // Tokenizer: id → piece (SentencePiece BPE)
    std::vector<std::string> id_to_piece_;
    std::string decode_tokens(const std::vector<int64_t>& ids) const;

#ifdef JARVIS_HAVE_ONNXRUNTIME
    std::unique_ptr<Ort::Env>        env_;
    std::unique_ptr<Ort::Session>    enc_;
    std::unique_ptr<Ort::Session>    dec0_;
    std::unique_ptr<Ort::Session>    decp_;
    std::unique_ptr<Ort::MemoryInfo> mem_;
    // Per-session I/O names (Run depends on name array order — queried once at init)
    std::vector<std::string> enc_in_,  enc_out_;
    std::vector<std::string> dec0_in_, dec0_out_;
    std::vector<std::string> decp_in_, decp_out_;
#endif
};

}  // namespace jarvis::stt
