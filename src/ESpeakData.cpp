// SPDX-License-Identifier: GPL-3.0-or-later
#include "ESpeakData.h"
#include "espeak-ng/core/config.h"

#include <algorithm>

#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>

#if defined(ARDUINO_ARCH_RP2040)
extern "C" {
extern const uint8_t _FS_start[];
extern const uint8_t _FS_end[];
}
#endif

namespace {

constexpr char imageMagic[] = "PESPACK1";
constexpr size_t imageHeaderBytes = 40;
constexpr size_t imageRecordBytes = 12;
constexpr size_t maxImageFiles = 1024;
constexpr size_t maxPathBytes = 119;
constexpr char revision[] = ESPEAK_ARDUINO_UPSTREAM_COMMIT;
static_assert(sizeof(revision) == 41, "Expected a full eSpeak NG revision");

uint32_t readLittleEndian(const uint8_t *data)
{
    return uint32_t(data[0]) | (uint32_t(data[1]) << 8) |
           (uint32_t(data[2]) << 16) | (uint32_t(data[3]) << 24);
}

size_t alignFour(size_t size)
{
    return (size + 3) & ~size_t{3};
}

uint8_t hexDigit(char digit)
{
    return digit >= 'a' ? digit - 'a' + 10 : digit - '0';
}

bool matchingRevision(const uint8_t *image)
{
    for (size_t byte = 0; byte < 20; ++byte) {
        const uint8_t expected = (hexDigit(revision[byte * 2]) << 4) | hexDigit(revision[byte * 2 + 1]);
        if (image[20 + byte] != expected) {
            return false;
        }
    }
    return true;
}

uint32_t crc32(const uint8_t *data, size_t length)
{
    // Four bits per lookup keeps the table small and the flash check quick.
    constexpr uint32_t table[] = {
        0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac,
        0x76dc4190, 0x6b6b51f4, 0x4db26158, 0x5005713c,
        0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c,
        0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c,
    };
    uint32_t crc = UINT32_MAX;
    for (size_t index = 0; index < length; ++index) {
        crc ^= data[index];
        crc = table[crc & 0x0f] ^ (crc >> 4);
        crc = table[crc & 0x0f] ^ (crc >> 4);
    }
    return ~crc;
}

bool validPath(const char *path, const char *end)
{
    const char *component = path;
    for (auto *character = path; character != end; ++character) {
        if (*character == '/') {
            const size_t length = character - component;
            if (length == 0 || (length <= 2 && std::memcmp(component, "..", length) == 0)) {
                return false;
            }
            component = character + 1;
        } else if (*character < ' ' || *character > '~' || *character == '\\') {
            return false;
        }
    }
    const size_t length = end - component;
    return length && (length > 2 || std::memcmp(component, "..", length) != 0);
}

constexpr char dataDirectory[] = "/espeak-ng-data";
const espeak::DataPack *activePack = nullptr;

bool startsWithComponent(const char *path, const char *prefix, size_t length)
{
    return std::strncmp(path, prefix, length) == 0 &&
           (path[length] == '\0' || path[length] == '/');
}

struct PackedPath {
    espeak::DataFile file{};
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
        const espeak::DataFile file = activePack->file(index);
        if (!file.path) {
            continue;
        }
        if (std::strcmp(path, file.path) == 0) {
            if (file.size > INT_MAX) {
                result.length = -EFBIG;
            } else {
                result.file = file;
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

espeak::DataFile espeak::DataPack::file(size_t index) const
{
    if (index >= count || (!_image && !files)) {
        return {};
    }
    if (!_image) {
        return files[index];
    }

    const auto *record = _image + imageHeaderBytes + index * imageRecordBytes;
    return {
        reinterpret_cast<const char *>(_image + readLittleEndian(record)),
        _image + readLittleEndian(record + 4),
        readLittleEndian(record + 8),
    };
}

bool espeak::FlashDataPack::open(const void *storage, size_t capacity)
{
    const auto *image = static_cast<const uint8_t *>(storage);
    if (!image || reinterpret_cast<uintptr_t>(image) % 4 || capacity < imageHeaderBytes) {
        return fail("Voice pack needs aligned storage and a complete header");
    }
    if (std::memcmp(image, imageMagic, sizeof(imageMagic) - 1) != 0) {
        return fail("No Pico eSpeak binary voice pack found");
    }

    const size_t length = readLittleEndian(image + 8);
    const size_t count = readLittleEndian(image + 16);
    const size_t tableEnd = imageHeaderBytes + imageRecordBytes * std::min(count, maxImageFiles);
    if (!count || count > maxImageFiles || length < tableEnd || length > capacity || length > INT_MAX) {
        return fail("Voice pack length or file count is invalid");
    }
    if (!matchingRevision(image)) {
        return fail("Voice pack was built for a different eSpeak NG revision");
    }
    if (crc32(image + 16, length - 16) != readLittleEndian(image + 12)) {
        return fail("Voice pack checksum does not match");
    }

    const uint8_t *record = image + imageHeaderBytes;
    size_t nextPath = tableEnd;
    const char *previous = nullptr;
    for (size_t index = 0; index < count; ++index, record += imageRecordBytes) {
        if (readLittleEndian(record) != nextPath || nextPath >= length) {
            return fail("Voice pack file name offset is invalid");
        }
        const auto *path = reinterpret_cast<const char *>(image + nextPath);
        const auto *end = static_cast<const char *>(
            std::memchr(path, 0, std::min(length - nextPath, maxPathBytes + 1)));
        if (!end || !validPath(path, end) || (previous && std::strcmp(previous, path) >= 0)) {
            return fail("Voice pack file names are invalid or out of order");
        }
        previous = path;
        nextPath += end - path + 1;
    }

    record = image + imageHeaderBytes;
    size_t nextFile = alignFour(nextPath);
    for (size_t index = 0; index < count; ++index, record += imageRecordBytes) {
        const size_t bytes = readLittleEndian(record + 8);
        if (nextFile > length || readLittleEndian(record + 4) != nextFile || bytes > length - nextFile) {
            return fail("Voice pack file offset or length is invalid");
        }
        nextFile = alignFour(nextFile + bytes);
    }
    if (nextFile != length) {
        return fail("Voice pack length does not match its files");
    }

    _pack = {nullptr, count};
    _pack._image = image;
    _error = nullptr;
    return true;
}

bool espeak::FlashDataPack::openReservedFlash()
{
    const size_t capacity = reservedFlashCapacity();
    if (capacity == 0) {
        return fail("Choose an Arduino-Pico flash layout with a reserved data partition");
    }
    return open(reinterpret_cast<const void *>(reservedFlashAddress()), capacity);
}

uintptr_t espeak::FlashDataPack::reservedFlashAddress()
{
#if defined(ARDUINO_ARCH_RP2040)
    return reinterpret_cast<uintptr_t>(_FS_start);
#else
    return 0;
#endif
}

size_t espeak::FlashDataPack::reservedFlashCapacity()
{
#if defined(ARDUINO_ARCH_RP2040)
    const uintptr_t end = reinterpret_cast<uintptr_t>(_FS_end);
    const uintptr_t start = reservedFlashAddress();
    return end > start ? end - start : 0;
#else
    return 0;
#endif
}

bool espeak::FlashDataPack::fail(const char *message)
{
    _error = message;
    return false;
}

void espeak::detail::useDataPack(const DataPack *pack)
{
    activePack = pack;
}

extern "C" const void *pico_espeak_map_file(const char *path, size_t *size)
{
    PackedPath result;
    if (!resolve(path, result) || !result.file.path) {
        return nullptr;
    }
    *size = result.file.size;
    return result.file.data;
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
    if (!result.file.path) {
        errno = -result.length;
        return nullptr;
    }
    if (std::strcmp(mode, "r") != 0 && std::strcmp(mode, "rb") != 0) {
        errno = EROFS;
        return nullptr;
    }

    // fmemopen accepts a mutable pointer but never writes in read mode.
    return fmemopen(const_cast<uint8_t *>(result.file.data), result.file.size, "r");
}
