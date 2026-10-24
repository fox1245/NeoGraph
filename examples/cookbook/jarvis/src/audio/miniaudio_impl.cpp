// jarvis/src/audio/miniaudio_impl.cpp
//
// Single translation unit for the miniaudio implementation.
//
// mic_input.cpp previously enabled only the decoder with MA_NO_DEVICE_IO, but
// TTS speaker playback (audio_playback.cpp) uses device I/O, so the full
// implementation is compiled here. miniaudio loads audio backends
// (PulseAudio/ALSA/WASAPI/CoreAudio) at runtime via dlopen with no extra link
// dependencies; in headless environments without a device, ma_device_init
// simply fails and callers fall back to saving the wav file.

#ifdef JARVIS_HAVE_MINIAUDIO
#  define MINIAUDIO_IMPLEMENTATION
#  include "miniaudio.h"
#endif
