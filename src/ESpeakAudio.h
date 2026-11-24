#pragma once

#include <Arduino.h>

#if __has_include(<AudioOutputBase.h>)
#include <AudioOutputBase.h>
#define ESPEAK_ARDUINO_HAS_AUDIOOUTPUTBASE 1
#else
#define ESPEAK_ARDUINO_HAS_AUDIOOUTPUTBASE 0
#endif

#if ESPEAK_ARDUINO_HAS_AUDIOOUTPUTBASE && defined(ARDUINO_ARCH_RP2040)
#include <I2S.h>
#include <PWMAudio.h>
#endif

namespace espeak {

using AudioStopCheck = bool (*)(void *userData);

class AudioSink {
public:
    virtual ~AudioSink() = default;

    virtual bool begin(int sampleRate) = 0;
    virtual void end() = 0;
    // Change the PCM rate between drained streams.
    virtual bool changeSampleRate(int sampleRate)
    {
        end();
        return begin(sampleRate);
    }
    // Accept signed 16-bit mono PCM. Wait for space as needed and return the
    // number of samples accepted. A short write indicates an output error.
    virtual size_t writeSamples(const int16_t *samples, size_t count) = 0;
    virtual void flush() {}
    // On interruption, leave pending audio for discard().
    virtual void flushUntil(AudioStopCheck stop, void *userData)
    {
        if (stop == nullptr || !stop(userData)) {
            flush();
        }
    }
    // Drop pending audio and leave the sink ready for the next utterance.
    virtual bool discard() { return true; }
    virtual bool ready() const = 0;
    // Optional diagnostic, valid until the next non-const sink operation.
    virtual const char *lastErrorText() const { return nullptr; }
};

class NullAudioSink : public AudioSink {
public:
    bool begin(int sampleRate) override;
    void end() override;
    size_t writeSamples(const int16_t *samples, size_t count) override;
    bool ready() const override;

private:
    bool _ready = false;
};

// Convert signed mono PCM to a fixed rate. The downstream sink must outlive
// this adapter and is started at outputSampleRate on begin().
class ResamplingSink final : public AudioSink {
public:
    ResamplingSink(AudioSink &downstream, int outputSampleRate);
    ResamplingSink(const ResamplingSink &) = delete;
    ResamplingSink &operator=(const ResamplingSink &) = delete;

    bool begin(int sampleRate) override;
    void end() override;
    bool changeSampleRate(int sampleRate) override;
    size_t writeSamples(const int16_t *samples, size_t count) override;
    void flush() override;
    void flushUntil(AudioStopCheck stop, void *userData) override;
    bool discard() override;
    bool ready() const override;
    const char *lastErrorText() const override { return _error; }

    int inputSampleRate() const { return _inputRate; }
    int outputSampleRate() const { return _outputRate; }

private:
    bool interpolate(int16_t next, AudioStopCheck stop = nullptr, void *userData = nullptr);
    bool average(int16_t sample);
    bool emit(int16_t sample);
    bool writeBuffered();
    bool failDownstream(const char *fallback);
    void resetStream();

    AudioSink &_downstream;
    const int _outputRate;
    int _inputRate = 0;
    // Interpolation uses ticks per input interval. Averaging uses the same
    // timescale to count ticks accumulated in the current output interval.
    uint32_t _phase = 0;
    int64_t _area = 0;
    int16_t _previous = 0;
    int16_t _buffer[32];
    size_t _buffered = 0;
    char _downstreamError[160] = {};
    const char *_error = nullptr;
    bool _hasPrevious = false;
    bool _needsFlush = false;
    bool _failed = false;
};

bool playTestTone(AudioSink &sink,
                  int sampleRate = 22050,
                  int frequencyHz = 440,
                  int durationMs = 500,
                  int amplitude = 12000);

#if ESPEAK_ARDUINO_HAS_AUDIOOUTPUTBASE
enum class AudioError {
    None,
    NotReady,
    InvalidSampleRate,
    InvalidBuffering,
    InsufficientMemory,
    InvalidPins,
    InvalidFormat,
    DmaChannelsUnavailable,
    DmaTimerUnavailable,
    ConfigurationFailed,
    StartFailed,
    InvalidWrite,
    WriteTimeout,
    DrainTimeout,
};

// Wrap a configured, idle Arduino-Pico output. Use one core for the entire
// audio lifecycle, including any other Arduino-Pico audio devices in the sketch.
class AudioOutputBaseSink : public AudioSink {
public:
    explicit AudioOutputBaseSink(AudioOutputBase &output, bool stereo = false);
    AudioOutputBaseSink(const AudioOutputBaseSink &) = delete;
    AudioOutputBaseSink &operator=(const AudioOutputBaseSink &) = delete;

    // Configure before begin(). Both supported drivers require at least three
    // buffers of at least eight 32-bit words each.
    void setStereo(bool stereo);
    bool stereo() const;
    void setBuffering(size_t buffers, size_t bufferWords);
    bool begin(int sampleRate) override;
    void end() override;
    size_t writeSamples(const int16_t *samples, size_t count) override;
    void flush() override;
    void flushUntil(AudioStopCheck stop, void *userData) override;
    bool discard() override;
    bool ready() const override;
    AudioError lastError() const;
    const char *lastErrorText() const override;
    // Return and clear the sticky underrun observed during writes. This samples
    // the driver's flag after the first two buffers, excluding idle and drain.
    bool takeUnderrun();

protected:
    explicit AudioOutputBaseSink(bool stereo);
    void attachOutput(AudioOutputBase &output);
    bool fail(AudioError error);

private:
    size_t writePcm(const int16_t *samples, size_t count, AudioStopCheck stop, void *userData);

    AudioOutputBase *_output = nullptr;
    bool _ready = false;
    bool _stereo;
    bool _pending = false;
    bool _underrun = false;
    AudioError _lastError = AudioError::None;
    size_t _buffers = 6;
    size_t _bufferWords = 128;
    size_t _bufferOffset = 0;
    size_t _primedFrames = 0;
    int _sampleRate = 0;
    int _writeCapacity = 0;
    uint32_t _writeTimeoutMs = 0;
    uint32_t _drainTimeoutMs = 0;
};
#endif

#if ESPEAK_ARDUINO_HAS_AUDIOOUTPUTBASE && defined(ARDUINO_ARCH_RP2040)
class PWMSink final : public AudioOutputBaseSink {
public:
    explicit PWMSink(pin_size_t pin);
    bool begin(int sampleRate) override;

private:
    PWMAudio _pwm;
    pin_size_t _pin;
};

enum class I2SFormat {
    Standard,
    LeftJustified,
};

class I2SSink final : public AudioOutputBaseSink {
public:
    // The word-select GPIO is bitClock + 1. Both channels carry the same PCM.
    I2SSink(pin_size_t bitClock, pin_size_t dataOut);
    // The clock GPIOs must be adjacent. Specify their actual signal order.
    I2SSink(pin_size_t bitClock, pin_size_t wordSelect, pin_size_t dataOut,
            I2SFormat format = I2SFormat::Standard);
    bool begin(int sampleRate) override;

private:
    using AudioOutputBaseSink::setStereo;

    I2S _i2s;
    pin_size_t _bitClock;
    pin_size_t _wordSelect;
    pin_size_t _dataOut;
    I2SFormat _format;
};
#endif

} // namespace espeak

using ESpeakAudioSink = espeak::AudioSink;
using ESpeakNullAudioSink = espeak::NullAudioSink;
using ESpeakResamplingSink = espeak::ResamplingSink;
using espeak::playTestTone;

#if ESPEAK_ARDUINO_HAS_AUDIOOUTPUTBASE
using ESpeakAudioOutputBaseSink = espeak::AudioOutputBaseSink;
using ESpeakAudioError = espeak::AudioError;
#endif

#if ESPEAK_ARDUINO_HAS_AUDIOOUTPUTBASE && defined(ARDUINO_ARCH_RP2040)
using ESpeakPWMSink = espeak::PWMSink;
using ESpeakI2SSink = espeak::I2SSink;
using ESpeakI2SFormat = espeak::I2SFormat;
#endif
