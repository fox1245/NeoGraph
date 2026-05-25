// jarvis/src/stt/whisper_node.h
//
// "whisper_stt" 그래프 노드: voice_in PCM → (text, lang) → user_text + user_lang.
//
// 구현 의존: whisper.cpp 의 libwhisper. ggerganov/whisper.cpp 의 examples/whisper.cpp
// 사용법 거의 그대로 — whisper_init_from_file → whisper_full → token 디코드.
//
// language="auto" 가 핵심: 첫 30초 mel 로 99개 언어 토큰 확률 계산 후 가장 높은
// 코드를 ISO 형식(ko/en/ja/...) 으로 출력. 이게 user_lang 채널 값이 되어 TTS 까지
// 전파됨 — 토니가 한국어로 말하면 자비스가 한국어로 답하는 메커니즘.
//
// 등록 (main.cpp):
//   NodeFactory::register_type("whisper_stt", ...);
//
// JSON 설정:
//   { "type": "whisper_stt",
//     "model_path": "assets/whisper-small.bin",
//     "language": "auto",
//     "translate_to_en": false }
//
// 입력 채널: voice_in (pcm + sample_rate)
// 출력 채널: user_text (string), user_lang (string, ISO 2자리)
//
// TODO(impl):
//   - whisper_init_from_file 1회
//   - whisper_full_params 구성 — language="auto", strategy=greedy 면 충분
//   - whisper_full_lang_id() 로 감지된 언어 코드 추출
//   - 빈 발화 / 잡음 처리 (text 비어있으면 그래프 조기 종료)
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
    // TODO: whisper_context*
};

}  // namespace jarvis::stt
