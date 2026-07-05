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

#include <cstdlib>
#include <iostream>
#include <string>

#ifdef JARVIS_HAVE_MINIAUDIO
#  include <vector>
#  include <stdexcept>
// miniaudio 구현은 miniaudio_impl.cpp 단일 TU 에 있음 (디바이스 I/O 포함
// — TTS 재생이 사용). 여기서는 디코더 API 선언만 필요.
#  include "miniaudio.h"
#endif

// 라이브 마이크 캡처 + Silero VAD 는 miniaudio(디바이스) + ONNX Runtime(VAD)
// 둘 다 있을 때만.
#if defined(JARVIS_HAVE_MINIAUDIO) && defined(JARVIS_HAVE_ONNXRUNTIME)
#  define JARVIS_LIVE_MIC 1
#  include <onnxruntime_cxx_api.h>
#  include <array>
#  include <atomic>
#  include <cmath>
#  include <condition_variable>
#  include <deque>
#  include <mutex>
#  include <optional>
#  include <queue>
#  include <thread>
#endif

namespace jarvis::audio {

#ifdef JARVIS_LIVE_MIC
// ─────────────────────────────────────────────────────────────────────────────
// MicCapture — miniaudio 캡처 디바이스 + Silero VAD 워커 스레드.
//
// 캡처 콜백(오디오 스레드): 입력 프레임을 mutex 버퍼에 append (VAD 를 콜백에서
//   돌리면 dropout — 버퍼링만).
// 워커 스레드: 512샘플(32ms) 윈도우로 VAD → speech prob → 발화 경계 상태기계.
//   발화 완성 시 발화 큐에 push + notify. run() 이 큐에서 pop.
// ─────────────────────────────────────────────────────────────────────────────
class MicCapture {
  public:
    MicCapture(std::string vad_path, float threshold, int max_seconds)
        : vad_path_(std::move(vad_path))
        , threshold_(threshold)
        , max_samples_(max_seconds * 16000) {
        // env 오버라이드: JARVIS_VAD_THRESHOLD(민감도 튜닝), JARVIS_MIC_DEBUG(관찰)
        if (const char* t = std::getenv("JARVIS_VAD_THRESHOLD"); t && t[0])
            threshold_ = std::atof(t);
        if (const char* d = std::getenv("JARVIS_MIC_DEBUG"); d && d[0] && d[0] != '0')
            debug_ = true;
    }

    ~MicCapture() { stop(); }

    // 디바이스 + VAD + 워커 기동. 실패(디바이스 없음/VAD 로드 실패) 시 false.
    bool start() {
        // Silero VAD 세션
        try {
            env_ = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "jarvis_vad");
            mem_ = std::make_unique<Ort::MemoryInfo>(
                Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault));
            Ort::SessionOptions so;
            so.SetIntraOpNumThreads(1);
            vad_ = std::make_unique<Ort::Session>(*env_, vad_path_.c_str(), so);
        } catch (const std::exception& e) {
            std::cerr << "[mic] Silero VAD 로드 실패: " << e.what() << "\n";
            return false;
        }
        state_.assign(2 * 1 * 128, 0.0f);
        context_.assign(64, 0.0f);

        // 캡처 디바이스 (16kHz mono f32)
        ma_device_config cfg = ma_device_config_init(ma_device_type_capture);
        cfg.capture.format   = ma_format_f32;
        cfg.capture.channels = 1;
        cfg.sampleRate       = 16000;
        cfg.dataCallback     = &MicCapture::ma_cb;
        cfg.pUserData        = this;
        if (ma_device_init(nullptr, &cfg, &device_) != MA_SUCCESS) {
            std::cerr << "[mic] 캡처 디바이스 초기화 실패 (WSL2 마이크 미연결?) "
                         "— stdin 폴백\n";
            return false;
        }
        if (ma_device_start(&device_) != MA_SUCCESS) {
            std::cerr << "[mic] 캡처 시작 실패 — stdin 폴백\n";
            ma_device_uninit(&device_);
            return false;
        }
        device_ok_ = true;
        running_.store(true);
        worker_ = std::thread([this] { worker(); });
        std::cerr << "[mic] 라이브 마이크 켜짐 — 말하면 발화 끝에서 자동 인식\n";
        return true;
    }

    void stop() {
        running_.store(false);
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
        if (device_ok_) { ma_device_uninit(&device_); device_ok_ = false; }
    }

    // 발화 하나를 기다림 — 완성된 PCM 반환, 종료 신호면 nullopt.
    std::optional<std::vector<float>> next_utterance() {
        std::unique_lock<std::mutex> lk(q_mtx_);
        cv_.wait(lk, [this] { return !utt_q_.empty() || !running_.load(); });
        if (!utt_available()) return std::nullopt;
        auto u = std::move(utt_q_.front());
        utt_q_.pop();
        return u;
    }

  private:
    bool utt_available() { return !utt_q_.empty(); }

    static void ma_cb(ma_device* d, void* /*out*/, const void* in, ma_uint32 n) {
        auto* self = static_cast<MicCapture*>(d->pUserData);
        const float* f = static_cast<const float*>(in);
        std::lock_guard<std::mutex> lk(self->buf_mtx_);
        self->buf_.insert(self->buf_.end(), f, f + n);
    }

    float run_vad(const float* chunk /*512*/) {
        // Silero v5 계약: 입력 = context(64) ++ chunk(512) = 576 샘플.
        // context 는 직전 입력의 마지막 64 (첫 호출은 0). 이걸 안 붙이고 512 만
        // 넣으면 모델이 무의미한 ~0.0006 을 뱉는다(실측 — 이게 원래 버그였음).
        constexpr int CTX = 64;
        std::array<int64_t, 2> in_shape{1, CTX + 512};
        std::array<int64_t, 3> st_shape{2, 1, 128};
        int64_t sr = 16000;
        std::array<int64_t, 1> sr_shape{};  // scalar
        std::vector<float> in_buf(CTX + 512);
        std::copy(context_.begin(), context_.end(), in_buf.begin());
        std::copy(chunk, chunk + 512, in_buf.begin() + CTX);
        std::array<Ort::Value, 3> ins{
            Ort::Value::CreateTensor<float>(*mem_, in_buf.data(), in_buf.size(),
                                            in_shape.data(), in_shape.size()),
            Ort::Value::CreateTensor<float>(*mem_, state_.data(), state_.size(),
                                            st_shape.data(), st_shape.size()),
            Ort::Value::CreateTensor<int64_t>(*mem_, &sr, 1,
                                              sr_shape.data(), 0)};
        const char* in_names[]  = {"input", "state", "sr"};
        const char* out_names[] = {"output", "stateN"};
        auto outs = vad_->Run(Ort::RunOptions{nullptr}, in_names, ins.data(), 3,
                              out_names, 2);
        float prob = outs[0].GetTensorData<float>()[0];
        const float* ns = outs[1].GetTensorData<float>();
        std::copy(ns, ns + state_.size(), state_.begin());
        // context = 이번 입력의 마지막 64 (다음 호출에 선행)
        std::copy(in_buf.end() - CTX, in_buf.end(), context_.begin());
        return prob;
    }

    void worker() {
        constexpr int WIN = 512;                  // Silero v5 16kHz 필수 청크
        constexpr int PREROLL = 16000 / 5;        // 200ms 프리롤(말 시작 안 잘리게)
        const int min_silence = 16000 / 2;        // 500ms 무음 → 발화 끝
        const int min_speech  = 16000 / 4;        // 250ms 미만은 잡음 무시
        std::vector<float> win(WIN);
        std::deque<float> preroll;
        std::vector<float> utt;
        bool triggered = false;
        int silence = 0;

        while (running_.load()) {
            // 버퍼에서 512 확보
            bool have = false;
            {
                std::lock_guard<std::mutex> lk(buf_mtx_);
                if (buf_.size() >= WIN) {
                    std::copy(buf_.begin(), buf_.begin() + WIN, win.begin());
                    buf_.erase(buf_.begin(), buf_.begin() + WIN);
                    have = true;
                }
            }
            if (!have) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }

            float prob = run_vad(win.data());

            // 진단 로깅 — 오디오가 흐르는지 + VAD 가 반응하는지 관찰.
            // JARVIS_MIC_DEBUG=1 이면 ~0.5초마다 RMS(신호 세기)+prob 출력.
            if (debug_) {
                double sum = 0.0;
                for (int i = 0; i < WIN; ++i) sum += win[i] * win[i];
                const float rms = static_cast<float>(std::sqrt(sum / WIN));
                if (++dbg_ctr_ % 15 == 0) {
                    std::cerr << "[mic][debug] rms=" << rms << " vad_prob=" << prob
                              << " thr=" << threshold_
                              << (triggered ? " [녹음중]" : "") << "\n";
                }
            }

            if (prob >= threshold_) {
                if (!triggered) {
                    triggered = true;
                    utt.assign(preroll.begin(), preroll.end());  // 프리롤 선행
                }
                utt.insert(utt.end(), win.begin(), win.end());
                silence = 0;
            } else if (triggered) {
                utt.insert(utt.end(), win.begin(), win.end());   // 트레일링 유지
                silence += WIN;
                if (silence >= min_silence) {
                    if (static_cast<int>(utt.size()) >= min_speech) {
                        std::lock_guard<std::mutex> lk(q_mtx_);
                        utt_q_.push(std::move(utt));
                        cv_.notify_one();
                    }
                    utt.clear(); triggered = false; silence = 0;
                    std::fill(state_.begin(), state_.end(), 0.0f);
                    std::fill(context_.begin(), context_.end(), 0.0f);
                }
            } else {
                // 유휴 — 프리롤 롤링 유지
                preroll.insert(preroll.end(), win.begin(), win.end());
                while (static_cast<int>(preroll.size()) > PREROLL)
                    preroll.pop_front();
            }
            // 최대 발화 길이 가드
            if (triggered && static_cast<int>(utt.size()) >= max_samples_) {
                std::lock_guard<std::mutex> lk(q_mtx_);
                utt_q_.push(std::move(utt));
                cv_.notify_one();
                utt.clear(); triggered = false; silence = 0;
                std::fill(state_.begin(), state_.end(), 0.0f);
                    std::fill(context_.begin(), context_.end(), 0.0f);
            }
        }
    }

    std::string vad_path_;
    float       threshold_;
    int         max_samples_;
    bool        debug_ = false;
    long        dbg_ctr_ = 0;

    std::unique_ptr<Ort::Env>        env_;
    std::unique_ptr<Ort::MemoryInfo> mem_;
    std::unique_ptr<Ort::Session>    vad_;
    std::vector<float>               state_;
    std::vector<float>               context_;  // Silero 64샘플 선행 컨텍스트

    ma_device device_{};
    bool      device_ok_ = false;

    std::mutex          buf_mtx_;
    std::vector<float>  buf_;

    std::mutex                        q_mtx_;
    std::condition_variable           cv_;
    std::queue<std::vector<float>>    utt_q_;
    std::atomic<bool>                 running_{false};
    std::thread                       worker_;
};
#endif  // JARVIS_LIVE_MIC

// ─────────────────────────────────────────────────────────────────────────────
// 생성자
// ─────────────────────────────────────────────────────────────────────────────

MicInputNode::MicInputNode(std::string name, const neograph::json& cfg)
    : name_(std::move(name))
    , sample_rate_(cfg.value("sample_rate", 16000))
    , vad_threshold_(cfg.value("vad_threshold", 0.5f))
    , max_utterance_seconds_(cfg.value("max_utterance_seconds", 30))
{
    // 라이브 마이크 모드: cfg "use_microphone":true 또는 env JARVIS_MIC=1
    const char* env = std::getenv("JARVIS_MIC");
    use_mic_ = cfg.value("use_microphone", false)
               || (env && env[0] && env[0] != '0');
#ifdef JARVIS_LIVE_MIC
    if (use_mic_) {
        const std::string vad_path = cfg.value("vad_model",
                                               std::string("assets/silero_vad.onnx"));
        capture_ = std::make_unique<MicCapture>(vad_path, vad_threshold_,
                                                max_utterance_seconds_);
        if (!capture_->start()) {
            capture_.reset();
            use_mic_ = false;   // 디바이스 실패 → stdin 폴백
        }
    }
#else
    if (use_mic_) {
        std::cerr << "[mic] JARVIS_MIC 요청됐으나 miniaudio/ONNX 빌드 아님 "
                     "— stdin 폴백\n";
        use_mic_ = false;
    }
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
// 소멸자
// ─────────────────────────────────────────────────────────────────────────────

MicInputNode::~MicInputNode() = default;

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

#ifdef JARVIS_LIVE_MIC
    // ── 라이브 마이크 모드 — 발화 하나를 기다렸다가 voice_in 에 PCM push ──
    if (use_mic_ && capture_) {
        auto utt = capture_->next_utterance();
        if (!utt) {  // 종료 신호
            out.writes.push_back({"__shutdown__", true});
            co_return out;
        }
        std::cerr << "[mic] 발화 감지 — " << utt->size() << " 샘플 (16kHz)\n";
        neograph::json pcm_json = neograph::json::array();
        for (float s : *utt) pcm_json.push_back(s);
        out.writes.push_back({"voice_in", neograph::json{
            {"pcm", std::move(pcm_json)}, {"sample_rate", 16000},
            {"mock_text", std::string{}}}});
        co_return out;
    }
#endif

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
