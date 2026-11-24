// SPDX-License-Identifier: GPL-3.0-or-later
#include "ESpeakAudio.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

namespace {

class CaptureSink final : public espeak::AudioSink {
public:
    bool begin(int sampleRate) override
    {
        ++starts;
        rate = sampleRate;
        active = allowStart;
        return active;
    }

    void end() override
    {
        ++ends;
        active = false;
        if (errorClearedOnEnd != nullptr) {
            *errorClearedOnEnd = '\0';
        }
    }

    size_t writeSamples(const int16_t *samples, size_t count) override
    {
        const size_t accepted = std::min(maxWrite, count);
        frames.insert(frames.end(), samples, samples + accepted);
        return accepted;
    }

    void flush() override
    {
        ++flushes;
        active = allowFlush;
    }

    bool discard() override
    {
        ++discards;
        if (allowDiscard) {
            frames.clear();
            active = true;
        }
        return allowDiscard;
    }

    bool ready() const override { return active; }
    const char *lastErrorText() const override { return error; }

    std::vector<int16_t> frames;
    size_t maxWrite = std::numeric_limits<size_t>::max();
    const char *error = nullptr;
    char *errorClearedOnEnd = nullptr;
    unsigned int starts = 0;
    unsigned int ends = 0;
    unsigned int flushes = 0;
    unsigned int discards = 0;
    int rate = 0;
    bool allowStart = true;
    bool allowFlush = true;
    bool allowDiscard = true;
    bool active = false;
};

std::vector<int16_t> convert(const std::vector<int16_t> &input, int from, int to, bool split = false)
{
    CaptureSink output;
    espeak::ResamplingSink sink(output, to);
    assert(sink.begin(from));
    assert(sink.inputSampleRate() == from && sink.outputSampleRate() == to && output.rate == to);
    assert(sink.writeSamples(nullptr, 0) == 0);
    if (split) {
        constexpr size_t chunks[] = {1, 7, 31, 32, 67, 2};
        size_t chunk = 0;
        for (size_t position = 0; position < input.size();) {
            const size_t count = std::min(chunks[chunk], input.size() - position);
            assert(sink.writeSamples(input.data() + position, count) == count);
            position += count;
            chunk = (chunk + 1) % (sizeof(chunks) / sizeof(chunks[0]));
        }
    } else {
        assert(sink.writeSamples(input.data(), input.size()) == input.size());
    }
    sink.flush();
    assert(sink.ready());
    const size_t count = output.frames.size();
    assert(count == (uint64_t{input.size()} * to + from - 1) / from);
    sink.flush();
    assert(output.frames.size() == count);
    sink.end();
    assert(!sink.ready() && sink.inputSampleRate() == 0 && output.ends == 1);
    return output.frames;
}

void conversion()
{
    assert(convert({}, 22050, 48000).empty());
    assert((convert({-32768}, 22050, 48000) == std::vector<int16_t>{-32768, -32768, -32768}));
    assert((convert({-32768, 32767}, 22050, 44100) == std::vector<int16_t>{-32768, -1, 32767, 32767}));
    assert((convert({0, 3000, 6000, 9000}, 3, 2) == std::vector<int16_t>{1000, 5000, 9000}));
    assert((convert({-32768, -32768}, 98000, 71000) == std::vector<int16_t>{-32768, -32768}));
    assert((convert({32767, 32767}, 65001, 65000) == std::vector<int16_t>{32767, 32767}));

    std::vector<int16_t> tone(22050);
    constexpr double twoPi = 6.28318530717958647692;
    for (size_t i = 0; i < tone.size(); ++i) {
        tone[i] = static_cast<int16_t>(std::lround(20000 * std::sin(twoPi * 1000 * i / 22050)));
    }
    assert(convert(tone, 22050, 22050, true) == tone);
    for (int rate : {8000, 16000, 44100, 48000, 96000}) {
        const auto continuous = convert(tone, 22050, rate);
        assert(convert(tone, 22050, rate, true) == continuous);
        if (rate >= 44100) {
            for (size_t i = 0; i + 5 < continuous.size(); ++i) {
                const double ideal = 20000 * std::sin(twoPi * 1000 * i / rate);
                assert(std::abs(continuous[i] - ideal) < 205);
            }
        }
    }

    std::vector<int16_t> input(200);
    for (size_t i = 0; i < input.size(); ++i) {
        input[i] = i % 2 == 0 ? -20000 : 20000;
    }
    assert(convert(input, 48000, 24000, true) == std::vector<int16_t>(100, 0));
}

void lifecycle()
{
    CaptureSink output;
    espeak::ResamplingSink sink(output, 48000);
    assert(!sink.begin(0) && output.starts == 0 && sink.lastErrorText() != nullptr);
    assert(sink.begin(24000));
    const int16_t first = -1000;
    assert(sink.writeSamples(&first, 1) == 1);
    assert(!sink.changeSampleRate(16000) && sink.ready());
    sink.flush();
    assert(sink.changeSampleRate(16000));
    const int16_t second = 2000;
    assert(sink.writeSamples(&second, 1) == 1);
    sink.flush();
    assert((output.frames == std::vector<int16_t>{-1000, -1000, 2000, 2000, 2000}));
    assert(output.starts == 1 && output.ends == 0 && output.rate == 48000);

    assert(sink.writeSamples(&second, 1) == 1);
    const unsigned int previousFlushes = output.flushes;
    unsigned int stopChecks = 0;
    sink.flushUntil([](void *context) { return ++*static_cast<unsigned int *>(context) >= 3; }, &stopChecks);
    assert(output.flushes == previousFlushes && output.frames.size() == 5 && sink.ready());
    assert(sink.discard() && output.discards == 1 && output.frames.empty());
    assert(sink.writeSamples(&first, 1) == 1);
    sink.flush();
    assert((output.frames == std::vector<int16_t>{-1000, -1000, -1000}));

    sink.end();
    sink.end();
    assert(output.ends == 1);
}

void downstreamFailures()
{
    CaptureSink output;
    espeak::ResamplingSink sink(output, 48000);
    char startupError[] = "device refused startup";
    output.error = startupError;
    output.errorClearedOnEnd = startupError;
    output.allowStart = false;
    assert(!sink.begin(24000) && startupError[0] == '\0');
    assert(std::strcmp(sink.lastErrorText(), "device refused startup") == 0);
    output.error = "device refused playback";
    output.errorClearedOnEnd = nullptr;
    output.allowStart = true;
    assert(sink.begin(24000));
    output.maxWrite = 6;
    int16_t input[48] = {};
    assert(sink.writeSamples(input, 48) < 48);
    assert(output.frames.size() == 6 && !sink.ready() && sink.writeSamples(input, 48) == 0);
    assert(std::strcmp(sink.lastErrorText(), output.error) == 0);
    assert(sink.discard() && sink.ready() && sink.lastErrorText() == nullptr);

    output.maxWrite = std::numeric_limits<size_t>::max();
    assert(sink.writeSamples(input, 1) == 1);
    output.allowFlush = false;
    sink.flush();
    assert(!sink.ready() && std::strcmp(sink.lastErrorText(), output.error) == 0);
    assert(sink.discard() && sink.ready());
    output.allowFlush = true;

    assert(sink.changeSampleRate(48000));
    output.maxWrite = 5;
    assert(sink.writeSamples(input, 48) == 5 && !sink.ready());
    sink.end();
}

} // namespace

int main()
{
    conversion();
    lifecycle();
    downstreamFailures();
}
