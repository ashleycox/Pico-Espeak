// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cstring>
#include <vector>

namespace fs {

class File {
public:
    std::vector<uint8_t> bytes;
    size_t writeLimit = SIZE_MAX;
    size_t readLimit = SIZE_MAX;
    bool failReads = false;

    explicit operator bool() const { return true; }
    bool isDirectory() const { return false; }
    size_t size() const { return bytes.size(); }
    size_t position() const { return _position; }
    void flush() {}

    bool seek(uint32_t position)
    {
        if (position > size()) {
            return false;
        }
        _position = position;
        return true;
    }

    size_t write(const uint8_t *data, size_t count)
    {
        count = std::min(count, writeLimit);
        if (_position + count > size()) {
            bytes.resize(_position + count);
        }
        std::memcpy(bytes.data() + _position, data, count);
        _position += count;
        return count;
    }

    int read(uint8_t *data, size_t count)
    {
        if (failReads) {
            return -1;
        }
        if (_position >= size()) {
            return 0;
        }
        count = std::min({count, readLimit, size() - _position});
        std::memcpy(data, bytes.data() + _position, count);
        _position += count;
        return static_cast<int>(count);
    }

    bool truncate(uint32_t size)
    {
        bytes.resize(size);
        return true;
    }

private:
    size_t _position = 0;
};

} // namespace fs
