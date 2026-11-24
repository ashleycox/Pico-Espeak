#pragma once

#define HAVE_MKSTEMP 0
#define HAVE_NBTOOL_CONFIG_H 0
#define HAVE_DECL_OPTRESET 0
#define HAVE_SYS_ENDIAN_H 0
#define HAVE_ISWBLANK 1

#define USE_ASYNC 0
#define USE_KLATT 1
#define USE_LIBPCAUDIO 0
#define USE_LIBSONIC 1
#define USE_MBROLA 0
#define USE_SPEECHPLAYER 1

#define PACKAGE_VERSION "1.53.0"
#define ESPEAK_ARDUINO_UPSTREAM_COMMIT "ee9fa7cc4704d683020526f47a9655f14c8d5f4c"

// Avoid a link conflict with sketches that declare their own global voice.
#define voice pico_espeak_engine_voice

#ifndef PICO_ESPEAK_MAPPED_DATA
#  if defined(ARDUINO)
#    define PICO_ESPEAK_MAPPED_DATA 1
#  else
#    define PICO_ESPEAK_MAPPED_DATA 0
#  endif
#endif

#if PICO_ESPEAK_MAPPED_DATA
#  define N_PATH_BUF 256
#endif
