// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stddef.h>
#include <stdio.h>

#if PICO_ESPEAK_MAPPED_DATA
#ifdef __cplusplus
extern "C" {
#endif

// An aligned, read-only view which remains valid for the life of the engine.
const void *pico_espeak_map_file(const char *path, size_t *size);
FILE *pico_espeak_open_file(const char *path, const char *mode);
int pico_espeak_file_length(const char *path, int *result);

#ifdef __cplusplus
}
#endif

// Used only in engine source files, after all system headers.
#define fopen pico_espeak_open_file
#endif
