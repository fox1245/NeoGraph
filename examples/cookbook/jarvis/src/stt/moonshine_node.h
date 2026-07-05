// jarvis/src/stt/moonshine_node.h
//
// "moonshine_stt" 그래프 노드 — Moonshine-tiny(-ko) ONNX STT.
// whisper_stt 의 대체/보완. voice_in PCM → user_text + user_lang.
//
// whisper 대비:
//   - 27M 초경량(int8 ~28MB), ergodic streaming encoder → 저지연/엣지
//   - raw 16kHz 파형 입력(mel 아님), seq2seq(encoder + 2-모델 분리 decoder)
//   - 언어별 flavor 모델 — tiny-ko 는 한국어 전용이라 user_lang 은 config 고정
//
// 런타임: onnxruntime C++ (supertonic TTS 와 동일 의존 재사용).
// 토크나이저: SentencePiece식 BPE — tokenizer.json 을 직접 파싱해 id→piece,
//   디코드는 ▁→space + ByteFallback(<0xHH>) + Fuse + strip 를 C++ 로 구현
//   (별도 토크나이저 라이브러리 불요).
//
// JSON 설정:
//   { "type": "moonshine_stt",
//     "model_dir": "assets/moonshine-tiny-ko",   // encoder/decoder onnx + tokenizer.json
//     "encoder": "onnx/encoder_model_int8.onnx",
//     "decoder": "onnx/decoder_model_int8.onnx",
//     "decoder_with_past": "onnx/decoder_with_past_model_int8.onnx",
//     "tokenizer": "tokenizer.json",
//     "language": "ko",
//     "max_length": 194 }
//
// 입력 채널: voice_in (pcm + sample_rate)  |  출력: user_text, user_lang
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

    // 토크나이저: id → piece (SentencePiece BPE)
    std::vector<std::string> id_to_piece_;
    std::string decode_tokens(const std::vector<int64_t>& ids) const;

#ifdef JARVIS_HAVE_ONNXRUNTIME
    std::unique_ptr<Ort::Env>        env_;
    std::unique_ptr<Ort::Session>    enc_;
    std::unique_ptr<Ort::Session>    dec0_;
    std::unique_ptr<Ort::Session>    decp_;
    std::unique_ptr<Ort::MemoryInfo> mem_;
    // 세션별 입출력 이름 (Run 은 이름 배열 순서 의존 — init 시 1회 조회)
    std::vector<std::string> enc_in_,  enc_out_;
    std::vector<std::string> dec0_in_, dec0_out_;
    std::vector<std::string> decp_in_, decp_out_;
#endif
};

}  // namespace jarvis::stt
