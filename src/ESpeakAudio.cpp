#include "ESpeakAudio.h"

#include <algorithm>
#include <climits>
#include <cmath>

#if ESPEAK_ARDUINO_HAS_AUDIOOUTPUTBASE && defined(ARDUINO_ARCH_RP2040)
#include <hardware/dma.h>
#endif

namespace espeak {

bool NullAudioSink::begin(int sampleRate)
{
    _ready = sampleRate > 0;
    return _ready;
}

void NullAudioSink::end()
{
    _ready = false;
}

size_t NullAudioSink::writeSamples(const int16_t *samples, size_t count)
{
    (void)samples;
    return _ready ? count : 0;
}

bool NullAudioSink::ready() const
{
    return _ready;
}

bool playTestTone(AudioSink &sink, int sampleRate, int frequencyHz, int durationMs, int amplitude)
{
    if (sampleRate <= 0 || frequencyHz <= 0 || frequencyHz > sampleRate / 2 || durationMs <= 0 || amplitude <= 0) {
        return false;
    }

    const uint64_t totalSamples = (static_cast<uint64_t>(sampleRate) * durationMs) / 1000;
    if (totalSamples == 0 || !sink.begin(sampleRate)) {
        return false;
    }

    constexpr float twoPi = 6.28318530717958647692f;
    const float step = twoPi * static_cast<float>(frequencyHz) / static_cast<float>(sampleRate);
    amplitude = std::min(amplitude, 32767);
    int16_t buffer[96];
    float phase = 0.0f;
    uint64_t produced = 0;
    bool ok = true;

    while (produced < totalSamples) {
        const size_t count = std::min<uint64_t>(sizeof(buffer) / sizeof(buffer[0]), totalSamples - produced);
        for (size_t index = 0; index < count; ++index) {
            buffer[index] = static_cast<int16_t>(std::sin(phase) * amplitude);
            phase += step;
            if (phase >= twoPi) {
                phase -= twoPi;
            }
        }

        if (sink.writeSamples(buffer, count) != count) {
            ok = false;
            break;
        }
        produced += count;
    }

    if (ok) {
        sink.flush();
        ok = sink.ready();
    }
    sink.end();
    return ok;
}

#if ESPEAK_ARDUINO_HAS_AUDIOOUTPUTBASE

namespace {

constexpr size_t writeChunk = 32;
constexpr size_t dmaWordBytes = sizeof(uint32_t);

uint32_t bufferTimeoutMs(size_t frames, int sampleRate)
{
    const uint64_t bufferTimeMs = (1000ULL * frames + sampleRate - 1) / sampleRate;
    return static_cast<uint32_t>(std::min<uint64_t>(bufferTimeMs + 50, INT_MAX));
}

} // namespace

AudioOutputBaseSink::AudioOutputBaseSink(AudioOutputBase &output, bool stereo)
    : AudioOutputBaseSink(stereo)
{
    attachOutput(output);
}

AudioOutputBaseSink::AudioOutputBaseSink(bool stereo)
    : _stereo(stereo)
{
}

void AudioOutputBaseSink::attachOutput(AudioOutputBase &output)
{
    _output = &output;
}

void AudioOutputBaseSink::setStereo(bool stereo)
{
    if (!_ready) {
        _stereo = stereo;
    }
}

bool AudioOutputBaseSink::stereo() const
{
    return _stereo;
}

void AudioOutputBaseSink::setBuffering(size_t buffers, size_t bufferWords)
{
    if (!_ready) {
        _buffers = buffers;
        _bufferWords = bufferWords;
    }
}

bool AudioOutputBaseSink::begin(int sampleRate)
{
    end();
    if (_output == nullptr || sampleRate <= 0 || _buffers < 3 || _bufferWords < 8 ||
        _bufferWords > INT_MAX / dmaWordBytes / _buffers) {
        return false;
    }

    if (!_output->setBuffers(_buffers, _bufferWords, 0) || !_output->setBitsPerSample(16) ||
        !_output->setFrequency(sampleRate) || !_output->setStereo(_stereo) || !_output->begin()) {
        return false;
    }

    _ready = true;
    _sampleRate = sampleRate;
    _writeCapacity = _output->availableForWrite();
    _writeTimeoutMs = bufferTimeoutMs(_bufferWords, sampleRate);
    _drainTimeoutMs = bufferTimeoutMs((_buffers + 2) * _bufferWords, sampleRate);
    if (_writeCapacity < static_cast<int>(dmaWordBytes)) {
        end();
        return false;
    }
    return true;
}

void AudioOutputBaseSink::end()
{
    if (_ready) {
        _output->end();
    }
    _ready = false;
    _pending = false;
    _bufferOffset = 0;
}

size_t AudioOutputBaseSink::writeSamples(const int16_t *samples, size_t count)
{
    if (!_ready || samples == nullptr) {
        return 0;
    }

    alignas(uint32_t) int16_t stereoFrames[2 * writeChunk];
    const size_t frameBytes = sizeof(int16_t) * (_stereo ? 2 : 1);
    size_t accepted = 0;
    uint32_t lastProgress = millis();
    while (accepted < count) {
        // The Pico drivers both report available bytes in DMA words. One word
        // holds a mono PWM sample or a complete stereo frame.
        const int available = _output->availableForWrite();
        if (available >= static_cast<int>(dmaWordBytes)) {
            const size_t availableFrames = static_cast<size_t>(available) / dmaWordBytes;
            const size_t frames = std::min({count - accepted, availableFrames, writeChunk});
            const int16_t *input = samples + accepted;
            if (_stereo) {
                for (size_t index = 0; index < frames; ++index) {
                    stereoFrames[2 * index] = input[index];
                    stereoFrames[2 * index + 1] = input[index];
                }
                input = stereoFrames;
            }
            const size_t bytes = _output->write(reinterpret_cast<const uint8_t *>(input), frames * frameBytes);
            if (bytes > frames * frameBytes || bytes % frameBytes != 0) {
                break;
            }
            if (bytes != 0) {
                accepted += bytes / frameBytes;
                lastProgress = millis();
                continue;
            }
        }

        if (millis() - lastProgress >= _writeTimeoutMs) {
            break;
        }
        delay(1);
    }

    _bufferOffset = (_bufferOffset + accepted % _bufferWords) % _bufferWords;
    _pending = _pending || accepted != 0;
    return accepted;
}

void AudioOutputBaseSink::flush()
{
    if (!_ready || !_pending) {
        return;
    }

    // A partial DMA buffer is never queued by the Pico audio drivers. Pad it
    // first, then wait for all user buffers to return to the free list.
    const int16_t silence[writeChunk] = {};
    while (_bufferOffset != 0) {
        const size_t count = std::min(_bufferWords - _bufferOffset, writeChunk);
        if (writeSamples(silence, count) != count) {
            end();
            return;
        }
    }

    const uint32_t started = millis();
    while (_output->availableForWrite() < _writeCapacity) {
        if (millis() - started >= _drainTimeoutMs) {
            end();
            return;
        }
        delay(1);
    }

    // The I2S PIO can still hold eight FIFO words and one shifted frame.
    delayMicroseconds((9000000ULL + _sampleRate - 1) / _sampleRate);
    _pending = false;
}

bool AudioOutputBaseSink::discard()
{
    return _ready && (!_pending || begin(_sampleRate));
}

bool AudioOutputBaseSink::ready() const
{
    return _ready;
}
#endif

#if ESPEAK_ARDUINO_HAS_AUDIOOUTPUTBASE && defined(ARDUINO_ARCH_RP2040)

PWMSink::PWMSink(pin_size_t pin)
    : AudioOutputBaseSink(false), _pwm(pin), _pin(pin)
{
    attachOutput(_pwm);
}

bool PWMSink::begin(int sampleRate)
{
    end();
    if (_pin >= __GPIOCNT || (stereo() && (_pin % 2 != 0 || _pin + 1 >= __GPIOCNT))) {
        return false;
    }

    // The Arduino-Pico PWM driver requires one DMA timer and four channels.
    // It can leave resources claimed if it discovers exhaustion during begin().
    unsigned int freeChannels = 0;
    for (unsigned int channel = 0; channel < NUM_DMA_CHANNELS; ++channel) {
        freeChannels += !dma_channel_is_claimed(channel);
    }
    if (freeChannels < 4) {
        return false;
    }
    for (unsigned int timer = 0; timer < NUM_DMA_TIMERS; ++timer) {
        if (!dma_timer_is_claimed(timer)) {
            return AudioOutputBaseSink::begin(sampleRate);
        }
    }
    return false;
}

I2SSink::I2SSink(pin_size_t bitClock, pin_size_t dataOut)
    : AudioOutputBaseSink(true), _i2s(OUTPUT), _bitClock(bitClock), _dataOut(dataOut)
{
    attachOutput(_i2s);
}

bool I2SSink::begin(int sampleRate)
{
    end();
    const int wordSelect = _bitClock + 1;
    return wordSelect < __GPIOCNT && _dataOut != _bitClock && _dataOut != wordSelect &&
           _i2s.setBCLK(_bitClock) && _i2s.setDOUT(_dataOut) && AudioOutputBaseSink::begin(sampleRate);
}

#endif

} // namespace espeak
