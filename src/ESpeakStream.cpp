// SPDX-License-Identifier: GPL-3.0-or-later
#include "ESpeakArduino.h"
#include "espeak-ng/core/common.h"

#include <cerrno>
#include <cstring>
#include <new>

#if __has_include(<FS.h>)
#include <FS.h>
#endif

namespace espeak {
namespace {

constexpr size_t chunkBytes = 512;
constexpr size_t readCapacity = chunkBytes + 1; // Probe EOF before deciding which chunk gets the end pause.

struct CodePoint {
    uint32_t value = 0;
    size_t bytes = 0;
};

// A zero byte count marks an incomplete sequence. A zero value marks invalid UTF-8 or NUL.
CodePoint readCodePoint(const char *text, size_t size)
{
    const auto lead = static_cast<uint8_t>(text[0]);
    if (lead < 0x80) {
        return {lead, 1};
    }
    size_t width;
    uint32_t value;
    uint32_t minimum;
    if (lead >= 0xc2 && lead <= 0xdf) {
        width = 2;
        value = lead & 0x1f;
        minimum = 0x80;
    } else if (lead >= 0xe0 && lead <= 0xef) {
        width = 3;
        value = lead & 0x0f;
        minimum = 0x800;
    } else if (lead >= 0xf0 && lead <= 0xf4) {
        width = 4;
        value = lead & 0x07;
        minimum = 0x10000;
    } else {
        return {0, 1};
    }
    if (size < width) {
        return {};
    }
    for (size_t i = 1; i < width; ++i) {
        const auto continuation = static_cast<uint8_t>(text[i]);
        if ((continuation & 0xc0) != 0x80) {
            return {0, width};
        }
        value = (value << 6) | (continuation & 0x3f);
    }
    if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) {
        return {0, width};
    }
    return {value, width};
}

struct ChunkBoundary {
    size_t bytes = 0;
    bool truncated = false;
};

// Keep a word intact when a space is available. Unspaced writing still advances at code-point boundaries.
ChunkBoundary chooseChunk(const char *text, size_t length, bool atEnd)
{
    const size_t limit = min(length, chunkBytes);
    size_t end = 0;
    size_t space = 0;
    size_t sentence = 0;
    bool afterPunctuation = false;
    while (end < limit) {
        const CodePoint point = readCodePoint(text + end, length - end);
        if (point.bytes == 0) {
            if (atEnd) {
                return {0, true};
            }
            break;
        }
        if (point.value == 0) {
            return {};
        }
        if (point.bytes > limit - end) {
            break;
        }
        end += point.bytes;
        if (IsSpace(point.value)) {
            space = end;
            if (afterPunctuation || point.value == '\n' || point.value == 0x2028 || point.value == 0x2029) {
                sentence = end;
            }
        } else {
            afterPunctuation = point.value == '.' || point.value == '!' || point.value == '?';
            if (point.value == 0x3002 || point.value == 0xff01 || point.value == 0xff1f) {
                sentence = end;
            }
        }
    }
    if (atEnd) {
        return {end, false};
    }
    if (sentence >= chunkBytes / 2) {
        return {sentence, false};
    }
    return {space != 0 ? space : end, false};
}

struct StreamInput {
    Synth &synth;
    Stream &source;
    size_t remaining;
    bool timedOut = false;
};

int readArduinoStream(char *text, size_t capacity, void *userData)
{
    auto &input = *static_cast<StreamInput *>(userData);
    const size_t limit = min(capacity, input.remaining);
    const unsigned long started = millis();
    const unsigned long timeout = input.source.getTimeout();
    size_t count = 0;
    while (count < limit && !input.synth.stopRequested()) {
        const int value = input.source.read();
        if (value >= 0 && value <= UINT8_MAX) {
            text[count++] = static_cast<char>(value);
        } else if (value != -1) {
            return -1;
        } else if (count != 0) {
            break;
        } else if (millis() - started >= timeout) {
            input.timedOut = true;
            return -1;
        } else {
            delay(1);
        }
    }
    input.remaining -= count;
    return static_cast<int>(count);
}

#if __has_include(<FS.h>)
struct FileInput {
    fs::File &source;
    size_t remaining;
    bool truncated = false;
};

int readArduinoFile(char *text, size_t capacity, void *userData)
{
    auto &input = *static_cast<FileInput *>(userData);
    if (input.remaining == 0) {
        return 0;
    }
    const size_t limit = min(capacity, input.remaining);
    const int count = input.source.read(reinterpret_cast<uint8_t *>(text), limit);
    if (count == 0) {
        input.truncated = true;
        return -1;
    }
    if (count < 0 || static_cast<size_t>(count) > limit) {
        return -1;
    }
    input.remaining -= static_cast<size_t>(count);
    return count;
}
#endif

} // namespace

bool Synth::sayStream(TextReader reader, void *userData)
{
    if (reader == nullptr) {
        return fail(EE_INTERNAL_ERROR, "text reader is missing", static_cast<espeak_ng_STATUS>(EINVAL));
    }
    if (!startSpeech()) {
        return false;
    }
    std::unique_ptr<char[]> text(new (std::nothrow) char[readCapacity]);
    if (!text) {
        return finishSpeech(static_cast<espeak_ng_STATUS>(ENOMEM));
    }
    size_t length = 0;
    bool atEnd = false;
    bool first = true;
    while (!stopRequested()) {
        while (length < readCapacity && !atEnd && !stopRequested()) {
            const size_t capacity = readCapacity - length;
            const int count = reader(text.get() + length, capacity, userData);
            if (stopRequested()) {
                return finishSpeech(ENS_SPEECH_STOPPED);
            }
            if (count < 0) {
                return finishSpeech(static_cast<espeak_ng_STATUS>(EIO), "text reader failed");
            }
            if (static_cast<size_t>(count) > capacity) {
                return finishSpeech(static_cast<espeak_ng_STATUS>(EIO), "text reader exceeded its buffer capacity");
            }
            atEnd = count == 0;
            length += static_cast<size_t>(count);
        }
        if (stopRequested()) {
            break;
        }
        if (length == 0) {
            return finishSpeech(ENS_OK);
        }
        if (first) {
            first = false;
            // Skip an optional UTF-8 byte order mark at the start of a file.
            if (length >= 3 && memcmp(text.get(), "\xef\xbb\xbf", 3) == 0) {
                length -= 3;
                memmove(text.get(), text.get() + 3, length);
                continue;
            }
        }
        const auto boundary = chooseChunk(text.get(), length, atEnd);
        if (boundary.bytes == 0) {
            if (boundary.truncated) {
                return finishSpeech(ENS_UNEXPECTED_EOF, "text ended within a UTF-8 character");
            }
            return finishSpeech(static_cast<espeak_ng_STATUS>(EILSEQ), "text contains invalid UTF-8 or a NUL byte");
        }
        const bool last = atEnd && boundary.bytes == length;
        const unsigned int flags = espeakCHARS_UTF8 | (last ? (_defaultFlags & espeakENDPAUSE) : 0);
        const char next = boundary.bytes < length ? text[boundary.bytes] : '\0';
        text[boundary.bytes] = '\0';
        const auto status = espeak_ng_Synthesize(text.get(), boundary.bytes + 1, 0, POS_CHARACTER, 0,
                                                 flags, nullptr, nullptr);
        if (status != ENS_OK || last) {
            return finishSpeech(status);
        }
        text[boundary.bytes] = next;
        length -= boundary.bytes;
        memmove(text.get(), text.get() + boundary.bytes, length);
    }
    return finishSpeech(ENS_SPEECH_STOPPED);
}

bool Synth::sayStream(Stream &source, size_t byteCount)
{
    StreamInput input{*this, source, byteCount};
    const bool success = sayStream(readArduinoStream, &input);
    if (!success && input.timedOut && _lastStatus == static_cast<espeak_ng_STATUS>(EIO)) {
        return fail(EE_INTERNAL_ERROR, "text stream timed out before the requested byte count",
                    static_cast<espeak_ng_STATUS>(ETIMEDOUT));
    }
    return success;
}

#if __has_include(<FS.h>)
bool Synth::sayStream(fs::File &source, size_t byteCount)
{
    if (!source || source.isDirectory()) {
        return fail(EE_INTERNAL_ERROR, "text file is not open for reading", static_cast<espeak_ng_STATUS>(EBADF));
    }
    FileInput input{source, byteCount};
    const bool success = sayStream(readArduinoFile, &input);
    if (!success && input.truncated && _lastStatus == static_cast<espeak_ng_STATUS>(EIO)) {
        return fail(EE_INTERNAL_ERROR, "text file ended before the requested byte count", ENS_UNEXPECTED_EOF);
    }
    return success;
}
#endif

} // namespace espeak
