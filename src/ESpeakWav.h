// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ESpeakAudio.h"
#include <FS.h>

namespace espeak {

// Write 16-bit mono PCM to an empty, writable, seekable Arduino-Pico file.
// The caller owns the file and closes it after the synthesizer ends.
class WavSink final : public AudioSink {
public:
    explicit WavSink(fs::File &file) : _file(file) {}
    WavSink(const WavSink &) = delete;
    WavSink &operator=(const WavSink &) = delete;

    bool begin(int sampleRate) override;
    void end() override;
    size_t writeSamples(const int16_t *samples, size_t count) override;
    void flush() override;
    bool discard() override;
    bool ready() const override;
    const char *lastErrorText() const override { return _error; }

    bool finalized() const { return _finalized; }
    uint32_t samplesRecorded() const { return _committedBytes / sizeof(int16_t); }

private:
    bool writeHeader(uint32_t length);
    bool fail(const char *message);

    fs::File &_file;
    const char *_error = nullptr;
    uint32_t _sampleRate = 0;
    uint32_t _dataBytes = 0;
    uint32_t _committedBytes = 0;
    bool _active = false;
    bool _finalized = false;
};

} // namespace espeak

using ESpeakWavSink = espeak::WavSink;
