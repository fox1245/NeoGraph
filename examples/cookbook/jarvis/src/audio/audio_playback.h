// jarvis/src/audio/audio_playback.h
//
// Thin helper that plays PCM on the OS default output device.
// miniaudio selects the platform playback API (PulseAudio/ALSA/WASAPI/CoreAudio)
// at runtime — on WSL2 it plays through the WSLg PulseAudio bridge.

#pragma once

#include <vector>

namespace jarvis::audio {

/// Play float32 mono PCM on the default output device (blocking).
///
/// @return true when playback completes; false if device initialization fails
///         (e.g. headless environment) or when built without miniaudio, in
///         which case the caller can proceed with the saved wav file alone.
bool play_pcm_blocking(const std::vector<float>& pcm, int sample_rate);

}  // namespace jarvis::audio
