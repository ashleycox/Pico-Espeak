// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace espeak {

struct DataFile {
    const char *path;
    const uint8_t *data;
    size_t size;
};

// Paths are relative to espeak-ng-data and file payloads are aligned to four bytes.
struct DataPack {
    constexpr DataPack(const DataFile *entries = nullptr, size_t entryCount = 0)
        : files(entries), count(entryCount) {}

    DataFile file(size_t index) const;

    const DataFile *files;
    size_t count;

private:
    friend class FlashDataPack;
    const uint8_t *_image = nullptr;
};

// A validated view of an immutable binary voice pack. Both the view and its
// backing memory must stay alive until the synthesizer has ended.
class FlashDataPack {
public:
    bool open(const void *image, size_t capacity);
    bool openReservedFlash();
    const DataPack &pack() const { return _pack; }
    const char *lastErrorText() const { return _error; }

    static uintptr_t reservedFlashAddress();
    static size_t reservedFlashCapacity();

private:
    bool fail(const char *message);
    DataPack _pack;
    const char *_error = nullptr;
};

constexpr char kBuiltinDataPath[] = "/pico-espeak";

const DataPack &englishDataPack();

namespace detail {
void useDataPack(const DataPack *pack);
}

} // namespace espeak
