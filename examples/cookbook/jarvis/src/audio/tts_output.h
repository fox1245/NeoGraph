// jarvis/src/audio/tts_output.h
//
// "supertonic_tts" 그래프 노드: final_text + user_lang → 44.1kHz PCM → 스피커.
//
// supertonic 의 cpp/helper.{h,cpp} 를 그대로 임베드해서 ONNX Runtime 위에서
// 추론. CPU only, 99M 파라미터 모델, 라이선스 MIT.
//
// user_lang 채널에 감지된 코드(ko/en/...)가 와 있으면 그대로 lang 인자로 사용,
// 없으면 "na"(language-agnostic) 로 fallback.
//
// 등록 (main.cpp):
//   NodeFactory::register_type("supertonic_tts", ...);
//
// JSON 설정:
//   { "type": "supertonic_tts",
//     "onnx_dir": "assets/onnx",
//     "voice_style": "assets/voices/M1.json",
//     "total_steps": 8, "speed": 1.05 }
//
// 입력 채널: final_text (string), user_lang (string?, optional)
// 출력 채널: tts_audio (재생 완료 timestamp 또는 빈 값 — 부수 효과로 스피커 출력)
//
// TODO(impl):
//   - supertonic helper.cpp 임베드 (assets/onnx 에서 모델 로드)
//   - miniaudio playback device 로 PCM 송출
//   - barge-in 대응: 재생 중 voice_in 활성화 감지 시 cancel token 으로 중단
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
    int turn_counter_ = 0;  // wav 파일 순번 카운터

#ifdef JARVIS_HAVE_SUPERTONIC
    // ONNX 환경 — 수명 동안 유지 (재로드 X)
    std::unique_ptr<Ort::Env>        ort_env_;
    std::unique_ptr<Ort::MemoryInfo> ort_meminfo_;
    // supertonic TextToSpeech 인스턴스 (ONNX 세션 4개 내장)
    std::unique_ptr<TextToSpeech>    tts_;
    // 음성 스타일 (M1.json 등)
    std::unique_ptr<Style>           style_;
#endif
};

}  // namespace jarvis::audio
