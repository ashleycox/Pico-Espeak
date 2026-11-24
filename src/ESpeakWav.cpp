// SPDX-License-Identifier: GPL-3.0-or-later
#include "ESpeakWav.h"

#include <algorithm>

namespace {

constexpr uint32_t headerBytes = 44;
// Keep the physical file size and the seek offsets within uint32_t.
constexpr uint32_t maxDataBytes = (UINT32_MAX - headerBytes) & ~uint32_t{1};

void write32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = static_cast<uint8_t>(value);
    bytes[1] = static_cast<uint8_t>(value >> 8);
    bytes[2] = static_cast<uint8_t>(value >> 16);
    bytes[3] = static_cast<uint8_t>(value >> 24);
}

} // namespace

bool espeak::WavSink::begin(int sampleRate)
{
    if (_active) {
        return fail("End the current WAV recording before starting another");
    }
    if (sampleRate <= 0 || !_file || _file.isDirectory() || _file.size() != 0) {
        return fail("WAV output needs an empty writable file and a positive sample rate");
    }
    _error = nullptr;
    _finalized = false;
    _sampleRate = static_cast<uint32_t>(sampleRate);
    _dataBytes = _committedBytes = 0;
    _active = writeHeader(0);
    return _active;
}

void espeak::WavSink::end()
{
    if (!_active) {
        return;
    }
    flush();
    if (_dataBytes != _committedBytes) {
        discard();
    }
    if (_dataBytes == _committedBytes && writeHeader(_committedBytes)) {
        _file.flush();
        _finalized = true;
    }
    _active = false;
}

size_t espeak::WavSink::writeSamples(const int16_t *samples, size_t count)
{
    if (!ready() || !samples) {
        return 0;
    }
    if (count > (maxDataBytes - _dataBytes) / sizeof(int16_t)) {
        fail("Recording exceeds the WAV size limit");
        return 0;
    }

    uint8_t bytes[64];
    size_t written = 0;
    while (written < count) {
        const size_t chunk = std::min(count - written, sizeof(bytes) / sizeof(int16_t));
        for (size_t index = 0; index < chunk; ++index) {
            const auto sample = static_cast<uint16_t>(samples[written + index]);
            bytes[index * 2] = static_cast<uint8_t>(sample);
            bytes[index * 2 + 1] = static_cast<uint8_t>(sample >> 8);
        }

        const size_t accepted = _file.write(bytes, chunk * sizeof(int16_t));
        _dataBytes += accepted;
        written += accepted / sizeof(int16_t);
        if (accepted != chunk * sizeof(int16_t)) {
            fail("Could not write all WAV samples");
            break;
        }
    }
    return written;
}

void espeak::WavSink::flush()
{
    if (!ready() || _dataBytes == _committedBytes) {
        return;
    }
    // LittleFS copies the whole file when bytes near the front are
    // overwritten. Commit the samples now and write their sizes in end().
    _file.flush();
    _committedBytes = _dataBytes;
}

bool espeak::WavSink::discard()
{
    if (!_active || !_file) {
        return false;
    }
    if (_dataBytes == _committedBytes && !_error) {
        return true;
    }
    const uint32_t length = headerBytes + _committedBytes;
    if (!_file.truncate(length) || !_file.seek(length)) {
        return fail("Could not restore the WAV file to the last completed utterance");
    }
    _file.flush();
    _dataBytes = _committedBytes;
    // Preserve any write error for the caller. Stop requests with no write
    // error leave the sink usable for the next utterance.
    return true;
}

bool espeak::WavSink::ready() const
{
    return _active && !_error && bool(_file);
}

bool espeak::WavSink::writeHeader(uint32_t length)
{
    uint8_t header[headerBytes] = {
        'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E',
        'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 1, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 16, 0,
        'd', 'a', 't', 'a', 0, 0, 0, 0,
    };
    write32(header + 4, length + 36);
    write32(header + 24, _sampleRate);
    write32(header + 28, _sampleRate * 2);
    write32(header + 40, length);

    if (!_file.seek(0) ||
        _file.write(header, sizeof(header)) != sizeof(header) ||
        !_file.seek(headerBytes + length)) {
        return fail("Could not write the WAV header");
    }
    return true;
}

bool espeak::WavSink::fail(const char *message)
{
    _error = message;
    return false;
}
