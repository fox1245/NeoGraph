// jarvis/src/audio/miniaudio_impl.cpp
//
// miniaudio 구현 단일 TU.
//
// 이전에는 mic_input.cpp 가 MA_NO_DEVICE_IO 로 디코더만 활성화했지만,
// TTS 스피커 재생(audio_playback.cpp)이 디바이스 I/O 를 쓰므로 여기서
// 전체 구현을 켠다. miniaudio 는 백엔드(PulseAudio/ALSA/WASAPI/CoreAudio)
// 를 런타임 dlopen 으로 찾으므로 추가 링크 의존성은 없다 — 디바이스가
// 없는 환경(헤드리스 CI 등)에서는 ma_device_init 이 실패할 뿐이고,
// 호출부가 파일 저장 폴백으로 처리한다.

#ifdef JARVIS_HAVE_MINIAUDIO
#  define MINIAUDIO_IMPLEMENTATION
#  include "miniaudio.h"
#endif
