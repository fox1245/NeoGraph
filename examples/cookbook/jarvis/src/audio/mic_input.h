// jarvis/src/audio/mic_input.h
//
// "voice_in" 그래프 노드: 마이크 한 발화 → PCM float 청크 → voice_in 채널.
//
// 구현 의존: miniaudio (헤더 하나, 마이크 + 스피커 둘 다), Silero VAD (ONNX).
// miniaudio 가 16kHz mono PCM 으로 콜백, VAD 가 발화 끝 감지 시 청크 종료.
//
// 등록 (main.cpp):
//   NodeFactory::instance().register_type("voice_in",
//       [](const std::string& n, const neograph::json& cfg, const NodeContext&) {
//           return std::make_unique<jarvis::audio::MicInputNode>(n, cfg);
//       });
//
// JSON 설정 (jarvis_graph.json):
//   { "type": "voice_in", "sample_rate": 16000,
//     "vad_threshold": 0.5, "max_utterance_seconds": 30 }
//
// 출력 채널: voice_in = { "pcm": <float vector>, "sample_rate": int }
//
// TODO(impl):
//   - miniaudio device init
//   - Silero VAD ONNX 세션
//   - 콜백 기반 청크 누적 + endpoint 감지 후 awaitable 반환
//   - max_utterance_seconds 초과 시 강제 종료
//   - 백그라운드 트리거가 voice_in 에 직접 쓰는 경우(텍스트 입력) 우회 처리
#pragma once

#include <neograph/neograph.h>

namespace jarvis::audio {

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
    // TODO: miniaudio device handle, Silero VAD session
};

}  // namespace jarvis::audio
