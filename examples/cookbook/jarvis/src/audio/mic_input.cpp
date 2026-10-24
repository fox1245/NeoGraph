// jarvis/src/audio/mic_input.cpp
//
// MicInputNode implementation — text mode (mock) + wav file input mode.
//
// Branches into two paths depending on the shape of each stdin line:
//
//   "wav:/path/to/file.wav"  → load wav file → decode to 16kHz mono PCM
//                              → push { "pcm": [...], "sample_rate": 16000 } to voice_in
//
//   Any other text           → mock path
//                              → push { "pcm": [], "sample_rate": ..., "mock_text": "..." } to voice_in
//
//   EOF                      → emit __shutdown__ signal
//
// WAV loading uses miniaudio's ma_decoder API.
// MA_NO_DEVICE_IO disables audio device I/O when only the decoder is needed.

#include "mic_input.h"

#include <cstdlib>
#include <iostream>
#include <string>

#ifdef JARVIS_HAVE_MINIAUDIO
#  include <vector>
#  include <stdexcept>
// The miniaudio implementation lives in the single TU miniaudio_impl.cpp
// (including device I/O used by TTS playback). Only decoder declarations are needed here.
#  include "miniaudio.h"
#endif

// Live microphone capture + Silero VAD requires both miniaudio (device) and
// ONNX Runtime (VAD).
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
// MicCapture — miniaudio capture device + Silero VAD worker thread.
//
// Capture callback (audio thread): appends input frames to a mutex-guarded buffer
//   (running VAD inside the callback would cause dropouts — buffer only).
// Worker thread: 512-sample (32ms) windows → VAD → speech prob → utterance boundary FSM.
//   Pushes completed utterances to the queue and notifies; run() pops from the queue.
// ─────────────────────────────────────────────────────────────────────────────
class MicCapture {
  public:
    MicCapture(std::string vad_path, float threshold, int max_seconds)
        : vad_path_(std::move(vad_path))
        , threshold_(threshold)
        , max_samples_(max_seconds * 16000) {
        // Env overrides: JARVIS_VAD_THRESHOLD (sensitivity tuning), JARVIS_MIC_DEBUG (diagnostics)
        if (const char* t = std::getenv("JARVIS_VAD_THRESHOLD"); t && t[0])
            threshold_ = std::atof(t);
        if (const char* d = std::getenv("JARVIS_MIC_DEBUG"); d && d[0] && d[0] != '0')
            debug_ = true;
    }

    ~MicCapture() { stop(); }

    // Start device + VAD + worker. Returns false on failure (no device or VAD load error).
    bool start() {
        // Silero VAD session
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

        // Capture device (16kHz mono f32)
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

    // Wait for one utterance — returns completed PCM, or nullopt on shutdown.
    //
    // Backpressure: on entry, discard any audio or utterances accumulated during
    // the previous turn (STT/LLM/TTS) and start listening fresh. This prevents
    // (1) startup noise, (2) stale utterances queued while JARVIS was busy, and
    // (3) TTS playback echo feeding back into the mic. Once an utterance is popped,
    // clear listening_ so the worker queues nothing while the turn runs.
    std::optional<std::vector<float>> next_utterance() {
        { std::lock_guard<std::mutex> bl(buf_mtx_); buf_.clear(); }
        { std::lock_guard<std::mutex> ql(q_mtx_);
          std::queue<std::vector<float>> empty; utt_q_.swap(empty); }
        listening_.store(true);

        std::unique_lock<std::mutex> lk(q_mtx_);
        cv_.wait(lk, [this] { return !utt_q_.empty() || !running_.load(); });
        if (utt_q_.empty()) { listening_.store(false); return std::nullopt; }
        auto u = std::move(utt_q_.front());
        utt_q_.pop();
        listening_.store(false);   // turn started — discard subsequent capture until next call
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
        // Silero v5 contract: input = context(64) ++ chunk(512) = 576 samples.
        // context is the last 64 samples of the previous input (zeros on first call).
        // Passing 512 samples without the 64-sample context produces ~0.0006 output.
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
        // context = last 64 samples of this input (prepended to the next call)
        std::copy(in_buf.end() - CTX, in_buf.end(), context_.begin());
        return prob;
    }

    void worker() {
        constexpr int WIN = 512;                  // Silero v5 required chunk size at 16kHz
        constexpr int PREROLL = 16000 / 5;        // 200ms preroll (avoids clipping speech onset)
        const int min_silence = 16000 / 2;        // 500ms silence → end of utterance
        const int min_speech  = 16000 / 4;        // ignore bursts shorter than 250ms as noise
        std::vector<float> win(WIN);
        std::deque<float> preroll;
        std::vector<float> utt;
        bool triggered = false;
        int silence = 0;

        while (running_.load()) {
            // Pull 512 samples from the buffer
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

            // While a turn is being processed (listening_==false), drop this window —
            // do not accumulate or queue utterances. Prevents TTS playback echo and
            // stale utterances, and resets local state for the next listening window.
            if (!listening_.load()) {
                if (triggered) { triggered = false; utt.clear(); silence = 0; }
                preroll.clear();
                continue;
            }

            float prob = run_vad(win.data());

            // Diagnostic logging — observe audio signal level and VAD probability.
            // When JARVIS_MIC_DEBUG=1, print RMS and prob roughly every 0.5s.
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
                    utt.assign(preroll.begin(), preroll.end());  // prepend preroll
                }
                utt.insert(utt.end(), win.begin(), win.end());
                silence = 0;
            } else if (triggered) {
                utt.insert(utt.end(), win.begin(), win.end());   // keep trailing audio
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
                // Idle — maintain rolling preroll window
                preroll.insert(preroll.end(), win.begin(), win.end());
                while (static_cast<int>(preroll.size()) > PREROLL)
                    preroll.pop_front();
            }
            // Maximum utterance length guard
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
    std::vector<float>               context_;  // Silero 64-sample leading context

    ma_device device_{};
    bool      device_ok_ = false;

    std::mutex          buf_mtx_;
    std::vector<float>  buf_;

    std::mutex                        q_mtx_;
    std::condition_variable           cv_;
    std::queue<std::vector<float>>    utt_q_;
    std::atomic<bool>                 running_{false};
    std::atomic<bool>                 listening_{false};  // queue utterances only while listening
    std::thread                       worker_;
};
#else
// Keep the pimpl type complete when live audio dependencies are absent.
// Mock mode never constructs it, but unique_ptr still instantiates its deleter.
class MicCapture {};
#endif  // JARVIS_LIVE_MIC

// ─────────────────────────────────────────────────────────────────────────────
// Constructor
// ─────────────────────────────────────────────────────────────────────────────

MicInputNode::MicInputNode(std::string name, const neograph::json& cfg)
    : name_(std::move(name))
    , sample_rate_(cfg.value("sample_rate", 16000))
    , vad_threshold_(cfg.value("vad_threshold", 0.5f))
    , max_utterance_seconds_(cfg.value("max_utterance_seconds", 30))
{
    // Live microphone mode: cfg "use_microphone":true or env JARVIS_MIC=1
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
            use_mic_ = false;   // device failure → fall back to stdin
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
// Destructor
// ─────────────────────────────────────────────────────────────────────────────

MicInputNode::~MicInputNode() = default;

// ─────────────────────────────────────────────────────────────────────────────
// Internal helper that reads a WAV file into a 16kHz mono float PCM vector;
// miniaudio's ma_decoder handles resampling and channel conversion automatically.
// ─────────────────────────────────────────────────────────────────────────────

#ifdef JARVIS_HAVE_MINIAUDIO
static std::vector<float> load_wav_as_16k_mono(const std::string& path)
{
    // Pin output format to 16kHz mono float32 — ma_decoder converts automatically
    ma_decoder_config cfg = ma_decoder_config_init(
        ma_format_f32,  // output sample format: 32-bit float
        1,              // output channels: mono
        16000           // output sample rate: 16kHz
    );

    ma_decoder decoder;
    ma_result result = ma_decoder_init_file(path.c_str(), &cfg, &decoder);
    if (result != MA_SUCCESS) {
        throw std::runtime_error("wav 파일 열기 실패: " + path
            + " (ma_result=" + std::to_string(result) + ")");
    }

    // Query total frame count up front (continue dynamically if unavailable)
    ma_uint64 total_frames = 0;
    ma_decoder_get_length_in_pcm_frames(&decoder, &total_frames);

    std::vector<float> pcm;
    if (total_frames > 0) {
        pcm.reserve(static_cast<size_t>(total_frames));
    }

    // Read in chunks of 4096 frames
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
// run() — read one line from stdin → branch by mode → push to voice_in channel
// ─────────────────────────────────────────────────────────────────────────────

asio::awaitable<neograph::graph::NodeOutput>
MicInputNode::run(neograph::graph::NodeInput /*in*/)
{
    neograph::graph::NodeOutput out;

#ifdef JARVIS_LIVE_MIC
    // ── Live microphone mode — wait for one utterance and push PCM to voice_in ──
    if (use_mic_ && capture_) {
        auto utt = capture_->next_utterance();
        if (!utt) {  // shutdown signal
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
        // EOF — emit __shutdown__ signal
        std::cerr << "[MicInputNode] stdin EOF — __shutdown__ 신호 전송\n";
        out.writes.push_back({"__shutdown__", true});
        co_return out;
    }

    // ── WAV mode: "wav:/path/to/file.wav" format ─────────────────────────────
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

            // Convert PCM vector to JSON array
            neograph::json pcm_json = neograph::json::array();
            for (float s : pcm) {
                pcm_json.push_back(s);
            }

            neograph::json voice_packet = {
                {"pcm",         std::move(pcm_json)},
                {"sample_rate", 16000},
                {"mock_text",   std::string{}}  // left empty in wav mode
            };
            out.writes.push_back({"voice_in", voice_packet});
            co_return out;

        } catch (const std::exception& e) {
            std::cerr << "[MicInputNode] wav 읽기 실패 — 텍스트 모드로 폴백: "
                      << e.what() << "\n";
            // Fall through to text mock below
            line = "(wav 읽기 실패: " + wav_path + ")";
        }
#else
        // Without miniaudio, warn and handle as text mock
        std::cerr << "[MicInputNode] miniaudio 없음 — wav 모드 비활성. "
                  << "텍스트 mock 으로 처리: " << wav_path << "\n";
        line = "(wav 모드 비활성: " + wav_path + ")";
#endif
    }

    // ── Text mode (mock behavior) ────────────────────────────────────────────
    neograph::json voice_packet = {
        {"pcm",         neograph::json::array()},
        {"sample_rate", sample_rate_},
        {"mock_text",   line}
    };

    out.writes.push_back({"voice_in", voice_packet});

    co_return out;
}

}  // namespace jarvis::audio
