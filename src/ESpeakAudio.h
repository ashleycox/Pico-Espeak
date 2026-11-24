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

class AudioSink {
public:
    virtual ~AudioSink() = default;

    virtual bool begin(int sampleRate) = 0;
    virtual void end() = 0;
    // Accept signed 16-bit mono PCM. Wait for space as needed and return the
    // number of samples accepted. A short write indicates an output error.
    virtual size_t writeSamples(const int16_t *samples, size_t count) = 0;
    virtual void flush() {}
    // Drop pending audio and leave the sink ready for the next utterance.
    virtual bool discard() { return true; }
    virtual bool ready() const = 0;
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

bool playTestTone(AudioSink &sink,
                  int sampleRate = 22050,
                  int frequencyHz = 440,
                  int durationMs = 500,
                  int amplitude = 12000);

#if ESPEAK_ARDUINO_HAS_AUDIOOUTPUTBASE
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
    bool discard() override;
    bool ready() const override;

protected:
    explicit AudioOutputBaseSink(bool stereo);
    void attachOutput(AudioOutputBase &output);

private:
    AudioOutputBase *_output = nullptr;
    bool _ready = false;
    bool _stereo;
    bool _pending = false;
    size_t _buffers = 4;
    size_t _bufferWords = 64;
    size_t _bufferOffset = 0;
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

class I2SSink final : public AudioOutputBaseSink {
public:
    // The word-select GPIO is bitClock + 1. Both channels carry the same PCM.
    I2SSink(pin_size_t bitClock, pin_size_t dataOut);
    bool begin(int sampleRate) override;

private:
    using AudioOutputBaseSink::setStereo;

    I2S _i2s;
    pin_size_t _bitClock;
    pin_size_t _dataOut;
};
#endif

} // namespace espeak

using ESpeakAudioSink = espeak::AudioSink;
using ESpeakNullAudioSink = espeak::NullAudioSink;
using espeak::playTestTone;

#if ESPEAK_ARDUINO_HAS_AUDIOOUTPUTBASE
using ESpeakAudioOutputBaseSink = espeak::AudioOutputBaseSink;
#endif

#if ESPEAK_ARDUINO_HAS_AUDIOOUTPUTBASE && defined(ARDUINO_ARCH_RP2040)
using ESpeakPWMSink = espeak::PWMSink;
using ESpeakI2SSink = espeak::I2SSink;
#endif
