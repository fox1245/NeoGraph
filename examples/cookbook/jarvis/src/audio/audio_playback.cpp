// jarvis/src/audio/audio_playback.cpp
//
// play_pcm_blocking implementation — the ma_device callback consumes the PCM
// buffer while the calling thread polls the completion flag.
//
// Why blocking: the graph runs one turn per cycle, so waiting for TTS playback
// to finish before returning to microphone capture keeps turns sequential and
// avoids feeding speaker output back into the microphone.

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

// Audio thread callback — copy up to frame_count samples; zero-pad and set done when exhausted
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

    // `done` marks when the last frame was copied into the callback buffer, not
    // when speaker output finishes. Short utterances fit into the first few
    // callbacks and set `done` almost immediately, so uninitializing right away
    // would cut off audio still buffered in the device/server (e.g. PulseAudio).
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
