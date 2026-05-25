// jarvis/src/audio/mic_input.cpp
//
// MicInputNode 구현 — 텍스트 모드(기존 mock 유지) + wav 파일 입력 모드(신규).
//
// stdin 한 줄의 형태에 따라 두 경로로 분기한다:
//
//   "wav:/path/to/file.wav"  → wav 파일 로드 → 16kHz mono PCM 으로 디코딩
//                              → voice_in 채널에 { "pcm": [...], "sample_rate": 16000 }
//
//   그 외 텍스트              → 기존 mock 경로 유지
//                              → voice_in 채널에 { "pcm": [], "sample_rate": ..., "mock_text": "..." }
//
//   EOF                       → __shutdown__ 신호 전송
//
// wav 로딩은 miniaudio 의 ma_decoder API 를 사용한다.
// MA_NO_DEVICE_IO 로 오디오 장치 부분을 제외하고 디코더만 활성화.

#include "mic_input.h"

#include <iostream>
#include <string>

#ifdef JARVIS_HAVE_MINIAUDIO
#  include <vector>
#  include <stdexcept>
// miniaudio — 장치 입출력 비활성화, 디코더(파일 읽기)만 사용
#  define MINIAUDIO_IMPLEMENTATION
#  define MA_NO_DEVICE_IO
#  include "miniaudio.h"
#endif

namespace jarvis::audio {

// ─────────────────────────────────────────────────────────────────────────────
// 생성자
// ─────────────────────────────────────────────────────────────────────────────

MicInputNode::MicInputNode(std::string name, const neograph::json& cfg)
    : name_(std::move(name))
    , sample_rate_(cfg.value("sample_rate", 16000))
    , vad_threshold_(cfg.value("vad_threshold", 0.5f))
    , max_utterance_seconds_(cfg.value("max_utterance_seconds", 30))
{
    // 실제 마이크 디바이스 사용 안 함 (WSL2 환경)
}

// ─────────────────────────────────────────────────────────────────────────────
// 소멸자
// ─────────────────────────────────────────────────────────────────────────────

MicInputNode::~MicInputNode() {
    // 현재 별도 리소스 없음
}

// ─────────────────────────────────────────────────────────────────────────────
// wav 파일을 읽어 16kHz mono float PCM 벡터로 반환하는 내부 함수
// miniaudio ma_decoder 가 자동으로 리샘플링 + 채널 변환 수행
// ─────────────────────────────────────────────────────────────────────────────

#ifdef JARVIS_HAVE_MINIAUDIO
static std::vector<float> load_wav_as_16k_mono(const std::string& path)
{
    // 출력 포맷을 16kHz mono float32 로 고정 — ma_decoder 가 자동 변환
    ma_decoder_config cfg = ma_decoder_config_init(
        ma_format_f32,  // 출력 샘플 형식: 32비트 부동소수점
        1,              // 출력 채널 수: mono
        16000           // 출력 샘플레이트: 16kHz
    );

    ma_decoder decoder;
    ma_result result = ma_decoder_init_file(path.c_str(), &cfg, &decoder);
    if (result != MA_SUCCESS) {
        throw std::runtime_error("wav 파일 열기 실패: " + path
            + " (ma_result=" + std::to_string(result) + ")");
    }

    // 전체 프레임 수 미리 파악 (실패해도 계속 — 동적으로 읽기)
    ma_uint64 total_frames = 0;
    ma_decoder_get_length_in_pcm_frames(&decoder, &total_frames);

    std::vector<float> pcm;
    if (total_frames > 0) {
        pcm.reserve(static_cast<size_t>(total_frames));
    }

    // 청크 단위로 읽기 — 4096 프레임씩
    constexpr ma_uint64 CHUNK = 4096;
    std::vector<float> chunk(CHUNK);
    ma_uint64 frames_read = 0;
    do {
        result = ma_decoder_read_pcm_frames(&decoder, chunk.data(), CHUNK, &frames_read);
        if (frames_read > 0) {
            pcm.insert(pcm.end(), chunk.begin(), chunk.begin() + frames_read);
        }
    } while (frames_read == CHUNK && result == MA_SUCCESS);

    ma_decoder_uninit(&decoder);

    if (pcm.empty()) {
        throw std::runtime_error("wav 파일에서 PCM 데이터를 읽지 못했습니다: " + path);
    }

    return pcm;
}
#endif  // JARVIS_HAVE_MINIAUDIO

// ─────────────────────────────────────────────────────────────────────────────
// run() — stdin 한 줄 읽기 → 모드 분기 → voice_in 채널 push
// ─────────────────────────────────────────────────────────────────────────────

asio::awaitable<neograph::graph::NodeOutput>
MicInputNode::run(neograph::graph::NodeInput /*in*/)
{
    neograph::graph::NodeOutput out;

    std::string line;
    if (!std::getline(std::cin, line)) {
        // EOF — __shutdown__ 신호 전송
        std::cerr << "[MicInputNode] stdin EOF — __shutdown__ 신호 전송\n";
        out.writes.push_back({"__shutdown__", true});
        co_return out;
    }

    // ── wav 모드: "wav:/path/to/file.wav" 형식 ──────────────────────────────
    constexpr std::string_view WAV_PREFIX = "wav:";
    if (line.size() > WAV_PREFIX.size()
        && line.substr(0, WAV_PREFIX.size()) == WAV_PREFIX)
    {
        std::string wav_path = line.substr(WAV_PREFIX.size());

#ifdef JARVIS_HAVE_MINIAUDIO
        try {
            auto pcm = load_wav_as_16k_mono(wav_path);

            std::cerr << "[MicInputNode] wav 로드 완료 — "
                      << wav_path << " → "
                      << pcm.size() << " 샘플 (16kHz mono)\n";

            // PCM 벡터를 JSON 배열로 변환
            neograph::json pcm_json = neograph::json::array();
            for (float s : pcm) {
                pcm_json.push_back(s);
            }

            neograph::json voice_packet = {
                {"pcm",         std::move(pcm_json)},
                {"sample_rate", 16000},
                {"mock_text",   std::string{}}  // wav 모드에서는 비워 둠
            };
            out.writes.push_back({"voice_in", voice_packet});
            co_return out;

        } catch (const std::exception& e) {
            std::cerr << "[MicInputNode] wav 읽기 실패 — 텍스트 모드로 폴백: "
                      << e.what() << "\n";
            // 아래 텍스트 mock 으로 이어서 처리
            line = "(wav 읽기 실패: " + wav_path + ")";
        }
#else
        // miniaudio 없으면 경고 후 텍스트 처리
        std::cerr << "[MicInputNode] miniaudio 없음 — wav 모드 비활성. "
                  << "텍스트 mock 으로 처리: " << wav_path << "\n";
        line = "(wav 모드 비활성: " + wav_path + ")";
#endif
    }

    // ── 텍스트 모드 (기존 mock 동작 그대로) ─────────────────────────────────
    neograph::json voice_packet = {
        {"pcm",         neograph::json::array()},
        {"sample_rate", sample_rate_},
        {"mock_text",   line}
    };

    out.writes.push_back({"voice_in", voice_packet});

    co_return out;
}

}  // namespace jarvis::audio
