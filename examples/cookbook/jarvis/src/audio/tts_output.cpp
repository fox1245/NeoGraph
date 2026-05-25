// jarvis/src/audio/tts_output.cpp
//
// SupertonicTtsNode 진짜 구현 (JARVIS_HAVE_SUPERTONIC 가드 안).
//
// 흐름:
//   생성자 → ONNX 모델 4개 + 음성 스타일 로드 (1회)
//   run()  → supertonic TextToSpeech::call() → wav 파일 저장
//             /tmp/jarvis-tts-<turn>.wav  (순번 누적)
//             /tmp/jarvis-tts-latest.wav  (항상 덮어씀)
//
// JARVIS_HAVE_SUPERTONIC 가 없으면 기존 mock 으로 폴백.

#include "tts_output.h"

#include <chrono>
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
// 생성자 — cfg 파싱 + (supertonic 있으면) 모델 로드
// ─────────────────────────────────────────────────────────────────────────────

SupertonicTtsNode::SupertonicTtsNode(std::string name, const neograph::json& cfg)
    : name_(std::move(name))
    , onnx_dir_(cfg.value("onnx_dir",       std::string("assets/onnx")))
    , voice_style_path_(cfg.value("voice_style", std::string("assets/voices/M1.json")))
    , total_steps_(cfg.value("total_steps", 8))
    , speed_(cfg.value("speed",             1.05f))
{
#ifdef JARVIS_HAVE_SUPERTONIC
    try {
        // ONNX 환경 + MemoryInfo 초기화 — 수명 동안 재사용
        ort_env_   = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "jarvis_tts");
        ort_meminfo_ = std::make_unique<Ort::MemoryInfo>(
            Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault));

        // TextToSpeech 인스턴스 로드 (내부에서 4개 ONNX 세션 열림)
        tts_ = loadTextToSpeech(*ort_env_, onnx_dir_, /*use_gpu=*/false);

        // 음성 스타일 로드
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
// 소멸자
// ─────────────────────────────────────────────────────────────────────────────

SupertonicTtsNode::~SupertonicTtsNode() {
    // unique_ptr 멤버들이 알아서 해제함
}

// ─────────────────────────────────────────────────────────────────────────────
// run() — final_text + user_lang 받아 TTS 합성 → wav 저장
// ─────────────────────────────────────────────────────────────────────────────

asio::awaitable<neograph::graph::NodeOutput>
SupertonicTtsNode::run(neograph::graph::NodeInput in)
{
    neograph::graph::NodeOutput out;

    // final_text 채널에서 응답 텍스트 읽기
    const auto& ft_val = in.state.get("final_text");
    std::string final_text = ft_val.is_string() ? ft_val.get<std::string>() : ft_val.dump();

    // user_lang 채널 — 없으면 "na" (language-agnostic) 로 폴백
    const auto& lang_val = in.state.get("user_lang");
    std::string lang = (lang_val.is_string() && !lang_val.get<std::string>().empty())
                       ? lang_val.get<std::string>()
                       : "na";

    // 빈 텍스트면 즉시 반환 — EOF 후 마지막 빈 사이클에서 불필요한 출력 방지
    if (final_text.empty()) {
        co_return out;
    }

#ifdef JARVIS_HAVE_SUPERTONIC
    // supertonic 인스턴스가 정상 로드된 경우에만 진짜 TTS
    if (tts_ && style_) {
        try {
            // user_lang 이 supertonic 지원 언어 목록 안에 없으면 "na" 로 교체
            std::string tts_lang = lang;
            bool lang_supported = std::find(
                AVAILABLE_LANGS.begin(), AVAILABLE_LANGS.end(), tts_lang
            ) != AVAILABLE_LANGS.end();
            if (!lang_supported) {
                tts_lang = "na";
            }

            // 실제 TTS 합성
            auto result = tts_->call(*ort_meminfo_, final_text, tts_lang,
                                     *style_, total_steps_, speed_);

            // wav 파일 경로 — 순번 파일 + latest 덮어쓰기 둘 다 저장
            int turn = turn_counter_++;
            std::string path_turn   = "/tmp/jarvis-tts-" + std::to_string(turn) + ".wav";
            std::string path_latest = "/tmp/jarvis-tts-latest.wav";

            writeWavFile(path_turn,   result.wav, tts_->getSampleRate());
            writeWavFile(path_latest, result.wav, tts_->getSampleRate());

            // 재생 시간 — duration 벡터가 있으면 첫 원소, 없으면 샘플 수로 계산
            float duration_sec = result.duration.empty()
                ? static_cast<float>(result.wav.size()) / tts_->getSampleRate()
                : result.duration[0];

            std::cout << "[jarvis:tts][" << lang << "] "
                      << "(생성: " << path_turn << ", "
                      << std::fixed << std::setprecision(1) << duration_sec << "초)\n";
            std::cout.flush();

            // 재생 완료 타임스탬프
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
            // 아래 mock 출력으로 계속 진행
        }
    }
#endif

    // mock 폴백 — supertonic 없거나 실패 시
    std::cout << "[jarvis:tts][" << lang << "] " << final_text << "\n";
    std::cout.flush();

    using namespace std::chrono;
    int64_t ts = duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
    out.writes.push_back({"tts_audio", neograph::json{{"played_at", ts}}});

    co_return out;
}

}  // namespace jarvis::audio
