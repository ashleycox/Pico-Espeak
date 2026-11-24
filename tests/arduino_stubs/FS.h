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

    explicit operator bool() const { return true; }
    bool isDirectory() const { return false; }
    size_t size() const { return bytes.size(); }
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

    bool truncate(uint32_t size)
    {
        bytes.resize(size);
        return true;
    }

private:
    size_t _position = 0;
};

} // namespace fs
