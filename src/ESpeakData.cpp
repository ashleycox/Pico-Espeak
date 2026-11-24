// SPDX-License-Identifier: GPL-3.0-or-later
#include "ESpeakData.h"

#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>

namespace {

constexpr char dataDirectory[] = "/espeak-ng-data";
const espeak::DataPack *activePack = nullptr;

bool startsWithComponent(const char *path, const char *prefix, size_t length)
{
    return std::strncmp(path, prefix, length) == 0 &&
           (path[length] == '\0' || path[length] == '/');
}

struct PackedPath {
    const espeak::DataFile *file = nullptr;
    int length = 0;
};

bool resolve(const char *path, PackedPath &result)
{
    constexpr size_t mountLength = sizeof(espeak::kBuiltinDataPath) - 1;
    constexpr size_t dataLength = sizeof(dataDirectory) - 1;
    if (!activePack || !startsWithComponent(path, espeak::kBuiltinDataPath, mountLength)) {
        return false;
    }
    path += mountLength;
    if (!*path || std::strcmp(path, "/") == 0) {
        result.length = -EISDIR;
        return true;
    }
    if (!startsWithComponent(path, dataDirectory, dataLength)) {
        result.length = -ENOENT;
        return true;
    }
    path += dataLength;
    while (*path == '/') {
        ++path;
    }

    // eSpeak checks that these directories exist even when the pack has no variants.
    if (!*path || std::strcmp(path, "lang") == 0 || std::strcmp(path, "voices") == 0) {
        result.length = -EISDIR;
        return true;
    }

    size_t length = std::strlen(path);
    while (length && path[length - 1] == '/') {
        --length;
    }
    for (size_t index = 0; index < activePack->count; ++index) {
        const espeak::DataFile &file = activePack->files[index];
        if (std::strcmp(path, file.path) == 0) {
            if (file.size > INT_MAX) {
                result.length = -EFBIG;
            } else {
                result.file = &file;
                result.length = static_cast<int>(file.size);
            }
            return true;
        }
        if (std::strncmp(path, file.path, length) == 0 && file.path[length] == '/') {
            result.length = -EISDIR;
            return true;
        }
    }
    result.length = -ENOENT;
    return true;
}

} // namespace

void espeak::detail::useDataPack(const DataPack *pack)
{
    activePack = pack;
}

extern "C" const void *pico_espeak_map_file(const char *path, size_t *size)
{
    PackedPath result;
    if (!resolve(path, result) || !result.file) {
        return nullptr;
    }
    *size = result.file->size;
    return result.file->data;
}

extern "C" int pico_espeak_file_length(const char *path, int *result)
{
    PackedPath packedPath;
    if (!resolve(path, packedPath)) {
        return 0;
    }
    *result = packedPath.length;
    return 1;
}

extern "C" FILE *pico_espeak_open_file(const char *path, const char *mode)
{
    PackedPath result;
    if (!resolve(path, result)) {
        return std::fopen(path, mode);
    }
    if (!result.file) {
        errno = -result.length;
        return nullptr;
    }
    if (std::strcmp(mode, "r") != 0 && std::strcmp(mode, "rb") != 0) {
        errno = EROFS;
        return nullptr;
    }

    // fmemopen accepts a mutable pointer but never writes in read mode.
    return fmemopen(const_cast<uint8_t *>(result.file->data), result.file->size, "r");
}
