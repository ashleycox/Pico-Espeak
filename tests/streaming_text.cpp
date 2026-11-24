// SPDX-License-Identifier: GPL-3.0-or-later
#include <ESpeakArduino.h>
#include <FS.h>

#include <atomic>
#include <cassert>
#include <cerrno>
#include <cstring>
#include <string>
#include <thread>

namespace {

class CountingSink final : public espeak::AudioSink {
public:
    bool begin(int rate) override { return _ready = rate > 0; }
    void end() override { _ready = false; }
    bool ready() const override { return _ready; }
    size_t writeSamples(const int16_t *, size_t count) override
    {
        samples += count;
        return count;
    }
    void flush() override { ++flushes; }
    bool discard() override
    {
        ++discards;
        return true;
    }

    size_t samples = 0;
    size_t flushes = 0;
    size_t discards = 0;

private:
    bool _ready = false;
};

struct Reader {
    espeak::Synth &synth;
    const CountingSink &sink;
    const std::string &text;
    size_t offset = 0;
    size_t failAt = SIZE_MAX;
    size_t cancelAt = SIZE_MAX;
    size_t firstSpeech = SIZE_MAX;
    size_t samplesAtStart = sink.samples;
};

int readSmallPieces(char *destination, size_t capacity, void *context)
{
    auto &reader = *static_cast<Reader *>(context);
    assert(capacity <= 513);
    if (reader.sink.samples > reader.samplesAtStart && reader.firstSpeech == SIZE_MAX) {
        reader.firstSpeech = reader.offset;
    }
    if (reader.offset >= reader.cancelAt) {
        reader.synth.requestStop();
        return -1; // Cancellation takes precedence over a callback's read error.
    }
    if (reader.offset >= reader.failAt) {
        return -1;
    }
    const size_t count = std::min({capacity, size_t{7}, reader.text.size() - reader.offset});
    std::memcpy(destination, reader.text.data() + reader.offset, count);
    reader.offset += count;
    return static_cast<int>(count);
}

int reportExcessRead(char *, size_t capacity, void *)
{
    return static_cast<int>(capacity + 1);
}

void countWords(const espeak_EVENT &event, void *context)
{
    if (event.type == espeakEVENT_WORD) {
        ++*static_cast<size_t *>(context);
    }
}

class MemoryStream final : public Stream {
public:
    explicit MemoryStream(const char *text) : _text(text) {}
    int read() override
    {
        ++reads;
        if (_position == _text.size()) {
            return -1;
        }
        return static_cast<unsigned char>(_text[_position++]);
    }
    size_t position() const { return _position; }

    std::atomic<size_t> reads{0};

private:
    std::string _text;
    size_t _position = 0;
};

void checkStreaming(espeak::Synth &synth, CountingSink &sink)
{
    std::string text;
    for (int line = 0; line < 100; ++line) {
        text += "One two three café. ";
    }
    size_t expectedWords = 0;
    synth.onEvent(countWords, &expectedWords);
    assert(synth.say(text.c_str()) && expectedWords > 0);

    size_t streamedWords = 0;
    synth.onEvent(countWords, &streamedWords);
    Reader reader{synth, sink, text};
    const auto flushes = sink.flushes;
    assert(synth.sayStream(readSmallPieces, &reader));
    assert(reader.offset == text.size() && reader.firstSpeech < text.size());
    assert(streamedWords == expectedWords);
    assert(sink.flushes == flushes + 1);
    synth.onEvent(nullptr);

    // The four-byte character straddles the first buffer's one-byte lookahead.
    const std::string boundary = std::string(511, ' ') + "\xf0\x9f\x92\xa1 light.";
    Reader split{synth, sink, boundary};
    assert(synth.sayStream(readSmallPieces, &split) && split.offset == boundary.size());

    const std::string bom = "\xef\xbb\xbf";
    Reader onlyBom{synth, sink, bom};
    const auto samples = sink.samples;
    assert(synth.sayStream(readSmallPieces, &onlyBom));
    assert(sink.samples == samples);

    Reader failure{synth, sink, text};
    failure.failAt = 700;
    const auto discards = sink.discards;
    assert(!synth.sayStream(readSmallPieces, &failure));
    assert(failure.firstSpeech < failure.offset && sink.discards == discards + 1);
    assert(synth.lastStatus() == static_cast<espeak_ng_STATUS>(EIO));
    assert(std::strcmp(synth.lastErrorText(), "text reader failed") == 0);

    Reader stopped{synth, sink, text};
    stopped.cancelAt = 700;
    assert(!synth.sayStream(readSmallPieces, &stopped));
    assert(stopped.firstSpeech < stopped.offset && synth.lastStatus() == ENS_SPEECH_STOPPED);
    assert(!synth.isPlaying());
}

void checkEncoding(espeak::Synth &synth, CountingSink &sink)
{
    const std::string truncated = "incomplete \xf0\x9f";
    Reader shortRead{synth, sink, truncated};
    assert(!synth.sayStream(readSmallPieces, &shortRead));
    assert(synth.lastStatus() == ENS_UNEXPECTED_EOF);

    const std::string badText[] = {"invalid \xc0\x80", "surrogate \xed\xa0\x80", std::string("zero\0byte", 9)};
    for (const auto &text : badText) {
        Reader invalid{synth, sink, text};
        assert(!synth.sayStream(readSmallPieces, &invalid));
        assert(synth.lastStatus() == static_cast<espeak_ng_STATUS>(EILSEQ));
    }

    assert(!synth.sayStream(reportExcessRead));
    assert(synth.lastStatus() == static_cast<espeak_ng_STATUS>(EIO));
    assert(!synth.sayStream(nullptr));
    assert(synth.lastStatus() == static_cast<espeak_ng_STATUS>(EINVAL));
}

void checkArduinoInputs(espeak::Synth &synth)
{
    MemoryStream serial("first passage!more data");
    assert(synth.sayStream(serial, 14));
    assert(serial.position() == 14);
    serial.setTimeout(0);
    assert(!synth.sayStream(serial, 100));
    assert(synth.lastStatus() == static_cast<espeak_ng_STATUS>(ETIMEDOUT));

    MemoryStream waiting("");
    waiting.setTimeout(5000);
    std::thread worker([&] { assert(!synth.sayStream(waiting, 12)); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (waiting.reads == 0) {
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
    synth.requestStop();
    worker.join();
    assert(synth.lastStatus() == ENS_SPEECH_STOPPED && !synth.isPlaying());

    fs::File file;
    const std::string contents = "skip this. Read only the rest.";
    file.bytes.assign(contents.begin(), contents.end());
    file.readLimit = 3;
    assert(file.seek(11));
    assert(synth.sayStream(file, file.size() - file.position()));
    assert(file.position() == file.size());
    assert(!synth.sayStream(file, 1) && synth.lastStatus() == ENS_UNEXPECTED_EOF);
    file.failReads = true;
    assert(!synth.sayStream(file, 1) && synth.lastStatus() == static_cast<espeak_ng_STATUS>(EIO));
}

} // namespace

int main()
{
    CountingSink sink;
    espeak::Synth synth;
    assert(synth.begin(sink));
    checkStreaming(synth, sink);
    checkEncoding(synth, sink);
    checkArduinoInputs(synth);
    assert(synth.say("Speech resumes."));
    synth.end();
    std::puts("Streaming text checks passed");
}
