// SPDX-License-Identifier: GPL-3.0-or-later
#include <ESpeakData.h>

#include <cassert>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

void updateChecksum(std::vector<uint8_t> &image)
{
    uint32_t crc = UINT32_MAX;
    for (size_t i = 16; i < image.size(); ++i) {
        crc ^= image[i];
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320U : 0);
        }
    }
    crc = ~crc;
    for (size_t byte = 0; byte < 4; ++byte) {
        image[12 + byte] = static_cast<uint8_t>(crc >> (byte * 8));
    }
}

int main(int argc, char *argv[])
{
    assert(argc == 2);
    std::ifstream in(argv[1], std::ios::binary);
    const std::vector<uint8_t> image{std::istreambuf_iterator<char>(in), {}};
    assert(image.size() > 100);

    espeak::FlashDataPack pack;
    assert(!pack.open(nullptr, image.size()));
    assert(!pack.open(image.data() + 1, image.size() - 1));
    assert(!pack.open(image.data(), image.size() - 1));
    assert(pack.open(image.data(), image.size()));
    assert(!pack.lastErrorText());
    assert(pack.pack().count == 3);
    const auto first = pack.pack().file(0);
    assert(std::strcmp(first.path, "en_dict") == 0);
    assert(first.size == 3 && first.data[0] == 'd');
    assert(reinterpret_cast<uintptr_t>(first.data) % 4 == 0);
    assert(std::strcmp(pack.pack().file(2).path, "voices/!v/Mr serious") == 0);
    assert(!pack.pack().file(pack.pack().count).path);

    auto broken = image;
    broken.back() ^= 0xff;
    assert(!pack.open(broken.data(), broken.size()));
    assert(std::strstr(pack.lastErrorText(), "checksum"));
    assert(pack.pack().file(0).data == first.data);

    broken = image;
    broken[20] ^= 0xff;
    updateChecksum(broken);
    assert(!pack.open(broken.data(), broken.size()));
    assert(std::strstr(pack.lastErrorText(), "revision"));

    broken = image;
    broken[44] |= 1; // Make the first payload offset unaligned.
    updateChecksum(broken);
    assert(!pack.open(broken.data(), broken.size()));
    assert(std::strstr(pack.lastErrorText(), "offset"));

    broken = image;
    broken[40] = 1; // A path cannot point into the header.
    updateChecksum(broken);
    assert(!pack.open(broken.data(), broken.size()));

    broken = image;
    broken[76] = '/'; // Three records end at byte 76, where names start.
    updateChecksum(broken);
    assert(!pack.open(broken.data(), broken.size()));
    assert(std::strstr(pack.lastErrorText(), "names"));
}
