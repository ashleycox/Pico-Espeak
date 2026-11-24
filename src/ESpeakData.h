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

// The generator supplies aligned, read-only files with paths relative to espeak-ng-data.
struct DataPack {
    const DataFile *files;
    size_t count;
};

constexpr char kBuiltinDataPath[] = "/pico-espeak";

const DataPack &englishDataPack();

namespace detail {
void useDataPack(const DataPack *pack);
}

} // namespace espeak
