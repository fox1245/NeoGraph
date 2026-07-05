// jarvis/src/audio/audio_playback.cpp
//
// play_pcm_blocking 구현 — ma_device 콜백이 PCM 버퍼를 소비하고,
// 호출 스레드는 완료 플래그를 폴링하며 대기한다.
//
// 블로킹인 이유: 현재 그래프는 한 턴 = 한 사이클이라 TTS 재생이 끝나야
// 다음 마이크 대기로 넘어가는 게 자연스럽다. barge-in(재생 중 끼어들기)은
// architecture.md 대로 v2 에서 cancel token 으로 도입.

#include "audio_playback.h"

#include <iostream>

#ifdef JARVIS_HAVE_MINIAUDIO

#include "miniaudio.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>

namespace jarvis::audio {

namespace {

struct PlaybackState {
    const float*           data         = nullptr;
    ma_uint64              total_frames = 0;
    std::atomic<ma_uint64> cursor{0};
    std::atomic<bool>      done{false};
};

// 오디오 스레드 콜백 — 버퍼에서 frame_count 만큼 복사, 소진 시 무음 패딩 + done
void data_callback(ma_device* dev, void* out, const void* /*in*/,
                   ma_uint32 frame_count) {
    auto*  st    = static_cast<PlaybackState*>(dev->pUserData);
    float* out_f = static_cast<float*>(out);

    const ma_uint64 cur    = st->cursor.load(std::memory_order_relaxed);
    const ma_uint64 remain = (cur < st->total_frames) ? st->total_frames - cur : 0;
    const ma_uint64 n      = (frame_count < remain) ? frame_count : remain;

    if (n > 0) {
        std::memcpy(out_f, st->data + cur, static_cast<size_t>(n) * sizeof(float));
    }
    if (n < frame_count) {
        std::memset(out_f + n, 0,
                    static_cast<size_t>(frame_count - n) * sizeof(float));
    }

    st->cursor.store(cur + n, std::memory_order_relaxed);
    if (cur + n >= st->total_frames) {
        st->done.store(true, std::memory_order_release);
    }
}

}  // namespace

bool play_pcm_blocking(const std::vector<float>& pcm, int sample_rate) {
    if (pcm.empty() || sample_rate <= 0) return false;

    PlaybackState st;
    st.data         = pcm.data();
    st.total_frames = pcm.size();

    ma_device_config cfg  = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format   = ma_format_f32;
    cfg.playback.channels = 1;
    cfg.sampleRate        = static_cast<ma_uint32>(sample_rate);
    cfg.dataCallback      = data_callback;
    cfg.pUserData         = &st;

    ma_device dev;
    if (ma_device_init(nullptr, &cfg, &dev) != MA_SUCCESS) {
        std::cerr << "[jarvis:audio] 재생 디바이스 초기화 실패 — 파일 저장만 유지\n";
        return false;
    }

    const auto start = std::chrono::steady_clock::now();
    if (ma_device_start(&dev) != MA_SUCCESS) {
        std::cerr << "[jarvis:audio] 재생 시작 실패 — 파일 저장만 유지\n";
        ma_device_uninit(&dev);
        return false;
    }

    while (!st.done.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    // done 은 마지막 프레임이 '콜백 버퍼에 복사'된 시점일 뿐 스피커 출력
    // 완료가 아니다. 짧은 발화는 전체가 첫 콜백들에 다 들어가서 done 이
    // 즉시 서므로, 여기서 바로 uninit 하면 디바이스/서버(PulseAudio) 내부
    // 버퍼에 남은 소리가 잘린다. 전체 재생 시간 + 마진까지 벽시계로 대기.
    const auto expected = std::chrono::milliseconds(
        static_cast<std::int64_t>(1000.0 * static_cast<double>(pcm.size())
                                  / sample_rate) + 350);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    if (elapsed < expected) {
        std::this_thread::sleep_for(expected - elapsed);
    }

    ma_device_uninit(&dev);
    return true;
}

}  // namespace jarvis::audio

#else  // !JARVIS_HAVE_MINIAUDIO

namespace jarvis::audio {

bool play_pcm_blocking(const std::vector<float>& /*pcm*/, int /*sample_rate*/) {
    std::cerr << "[jarvis:audio] miniaudio 미포함 빌드 — 재생 생략\n";
    return false;
}

}  // namespace jarvis::audio

#endif  // JARVIS_HAVE_MINIAUDIO
