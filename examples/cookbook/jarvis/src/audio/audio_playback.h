// jarvis/src/audio/audio_playback.h
//
// OS 기본 재생 디바이스로 PCM 을 내보내는 얇은 헬퍼.
// miniaudio 가 플랫폼별 재생 API(PulseAudio/ALSA/WASAPI/CoreAudio)를
// 런타임에 자동 선택한다 — WSL2 는 WSLg PulseAudio 브리지로 재생됨.

#pragma once

#include <vector>

namespace jarvis::audio {

/// float32 mono PCM 을 기본 재생 디바이스로 블로킹 재생.
///
/// @return 재생 완료 시 true. 디바이스 초기화 실패(헤드리스 환경 등)나
///         miniaudio 미포함 빌드에서는 false — 호출자는 wav 파일 저장만으로
///         계속 진행하면 된다.
bool play_pcm_blocking(const std::vector<float>& pcm, int sample_rate);

}  // namespace jarvis::audio
