// SPDX-License-Identifier: GPL-3.0-or-later
#include <ESpeakArduino.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

using espeak::SpeechState;

namespace {

template <typename Check>
void waitUntil(Check check)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!check()) {
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
}

class TestSink final : public espeak::AudioSink {
public:
    bool begin(int rate) override { return _running = rate > 0; }
    void end() override { _running = false; }
    bool ready() const override { return _running; }
    const char *lastErrorText() const override { return _error; }

    size_t writeSamples(const int16_t *, size_t count) override
    {
        if (rejectWrites) {
            _error = "sample write rejected";
            return 0;
        }
        samples += count;
        return count;
    }

    void flushUntil(espeak::AudioStopCheck stopped, void *context) override
    {
        if (holdDrain) {
            draining = true;
            waitUntil([&] { return stopped(context); });
        }
    }

    bool discard() override
    {
        _error = "discard completed";
        if (holdDiscard) {
            discarding = true;
            waitUntil([&] { return resumeDiscard.load(); });
        }
        return true;
    }

    bool rejectWrites = false;
    bool holdDrain = false;
    bool holdDiscard = false;
    std::atomic<bool> draining{false};
    std::atomic<bool> discarding{false};
    std::atomic<bool> resumeDiscard{false};
    size_t samples = 0;

private:
    bool _running = false;
    const char *_error = nullptr;
};

void testPendingRequests(espeak::BackgroundSynth &queue, const TestSink &sink)
{
    queue.setMaxTextBytes(12);
    assert(queue.maxTextBytes() == 12);
    const auto original = queue.submit(F("hello world"));
    assert(original != 0 && queue.result(original).state == SpeechState::Queued);
    assert(!queue.submitNow(F("too much text to replace the original")));
    assert(!queue.setVoice(F("overlong-voice")));
    assert(queue.result(original).state == SpeechState::Queued);

    const auto discarded = queue.submit("queued too");
    const auto replacement = queue.submitNow(String("new request!"));
    assert(discarded != 0 && replacement != 0 && discarded != replacement);
    assert(queue.result(original).status == ENS_SPEECH_STOPPED);
    assert(queue.result(discarded).state == SpeechState::Cancelled && queue.result(discarded).finished());
    assert(queue.result(replacement).state == SpeechState::Queued);
    const size_t oldSamples = sink.samples;
    queue.loop();
    assert(sink.samples > oldSamples);
    assert(queue.result(replacement).state == SpeechState::Completed && queue.result(replacement).finished());
    assert(!queue.cancel(replacement));

    const auto first = queue.submit("first");
    assert(queue.setPitch(40));
    const auto middle = queue.submit("middle");
    const auto last = queue.submit("last");
    assert(first != 0 && middle != 0 && last != 0 && queue.submit("queue full") == 0);
    assert(queue.cancel(middle));
    assert(queue.result(middle).state == SpeechState::Cancelled && queue.result(last).state == SpeechState::Queued);
    queue.loop();
    assert(queue.result(first).state == SpeechState::Completed && queue.result(last).state == SpeechState::Queued);
    queue.loop();
    assert(queue.settings().pitch == 40 && queue.result(last).state == SpeechState::Queued);
    queue.loop();
    assert(queue.result(last).state == SpeechState::Completed);
}

void testActiveRequests(espeak::BackgroundSynth &queue, TestSink &sink)
{
    sink.holdDrain = true;
    const auto playing = queue.submit("stop this");
    assert(playing != 0);
    std::thread worker([&] { queue.loop(); });
    waitUntil([&] { return sink.draining.load(); });
    assert(queue.result(playing).state == SpeechState::Speaking);
    assert(queue.cancel(playing));
    worker.join();
    assert(queue.result(playing).state == SpeechState::Cancelled);
    assert(queue.result(playing).status == ENS_SPEECH_STOPPED);
    sink.holdDrain = false;

    sink.rejectWrites = true;
    sink.holdDiscard = true;
    const auto failing = queue.submit("write error");
    assert(failing != 0);
    std::thread failingWorker([&] { queue.loop(); });
    waitUntil([&] { return sink.discarding.load(); });
    assert(queue.result(failing).state == SpeechState::Speaking);
    assert(!queue.cancel(failing));
    sink.resumeDiscard = true;
    failingWorker.join();
    assert(queue.result(failing).state == SpeechState::Failed);
    assert(queue.result(failing).status == ENS_AUDIO_ERROR);
    assert(std::strcmp(queue.lastErrorText().c_str(), "sample write rejected") == 0);
    sink.holdDiscard = false;
    sink.rejectWrites = false;

    for (size_t index = 0; index < espeak::BackgroundSynth::resultHistoryCapacity; ++index) {
        const auto next = queue.submit("archive");
        assert(next != 0 && queue.cancel(next));
    }
    assert(queue.result(playing).state == SpeechState::Unknown);
    assert(queue.result(failing).state == SpeechState::Unknown);
}

} // namespace

int main()
{
    TestSink sink;
    espeak::Synth synth;
    espeak::BackgroundSynth queue(synth);
    assert(queue.submit("before begin") == 0);
    assert(queue.begin(sink, espeak::Config(), 3));
    assert(queue.result(0).state == SpeechState::Unknown);
    testPendingRequests(queue, sink);
    testActiveRequests(queue, sink);

    const auto stopped = queue.submit("stop pending");
    queue.stop();
    assert(stopped != 0 && queue.result(stopped).state == SpeechState::Cancelled);
    const auto shutdown = queue.submit("end pending");
    queue.end();
    assert(shutdown != 0 && queue.result(shutdown).state == SpeechState::Cancelled);
    assert(!queue.isRunning() && !queue.isStopped());
    queue.loop();
    assert(queue.isStopped() && !sink.ready());
    std::puts("Speech queue checks passed");
}
