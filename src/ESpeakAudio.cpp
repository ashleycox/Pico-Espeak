#include "ESpeakAudio.h"

#include <algorithm>
#include <climits>
#include <cmath>

#if ESPEAK_ARDUINO_HAS_AUDIOOUTPUTBASE && defined(ARDUINO_ARCH_RP2040)
#include <hardware/clocks.h>
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
    if (_output == nullptr) {
        return fail(AudioError::NotReady);
    }
    if (sampleRate <= 0) {
        return fail(AudioError::InvalidSampleRate);
    }
    if (_buffers < 3 || _bufferWords < 8 || _bufferWords > INT_MAX / dmaWordBytes / _buffers) {
        return fail(AudioError::InvalidBuffering);
    }
#if defined(ARDUINO_ARCH_RP2040)
    // The driver allocates a permanent silence buffer of the same size.
    const size_t pcmBytes = (_buffers + 1) * _bufferWords * dmaWordBytes;
    const int freeHeap = rp2040.getFreeHeap();
    if (freeHeap <= 0 || pcmBytes > static_cast<size_t>(freeHeap)) {
        return fail(AudioError::InsufficientMemory);
    }
#endif

    if (!_output->setBuffers(_buffers, _bufferWords, 0) || !_output->setBitsPerSample(16) ||
        !_output->setFrequency(sampleRate) || !_output->setStereo(_stereo)) {
        return fail(AudioError::ConfigurationFailed);
    }
    if (!_output->begin()) {
        return fail(AudioError::StartFailed);
    }

    _ready = true;
    _sampleRate = sampleRate;
    _writeCapacity = _output->availableForWrite();
    _writeTimeoutMs = bufferTimeoutMs(_bufferWords, sampleRate);
    _drainTimeoutMs = bufferTimeoutMs((_buffers + 2) * _bufferWords, sampleRate);
    if (_writeCapacity < static_cast<int>(dmaWordBytes)) {
        end();
        return fail(AudioError::StartFailed);
    }
    _lastError = AudioError::None;
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
    _primedFrames = 0;
}

size_t AudioOutputBaseSink::writeSamples(const int16_t *samples, size_t count)
{
    if (count == 0) {
        return 0;
    }
    if (!_ready) {
        fail(AudioError::NotReady);
        return 0;
    }
    if (samples == nullptr) {
        fail(AudioError::InvalidWrite);
        return 0;
    }

    _lastError = AudioError::None;
    const size_t primeTarget = 2 * _bufferWords;
    if (_primedFrames == primeTarget && _output->getUnderflow()) {
        _underrun = true;
    }

    const size_t accepted = writePcm(samples, count, nullptr, nullptr);
    if (_primedFrames < primeTarget) {
        _primedFrames += std::min(accepted, primeTarget - _primedFrames);
        if (_primedFrames == primeTarget) {
            // The device also transmits silence before the first utterance.
            _output->getUnderflow();
        }
    }
    return accepted;
}

size_t AudioOutputBaseSink::writePcm(const int16_t *samples, size_t count, AudioStopCheck stop, void *userData)
{
    alignas(uint32_t) int16_t stereoFrames[2 * writeChunk];
    const size_t frameBytes = sizeof(int16_t) * (_stereo ? 2 : 1);
    size_t accepted = 0;
    uint32_t lastProgress = millis();
    while (accepted < count) {
        if (stop != nullptr && stop(userData)) {
            break;
        }
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
                fail(AudioError::InvalidWrite);
                break;
            }
            if (bytes != 0) {
                accepted += bytes / frameBytes;
                lastProgress = millis();
                continue;
            }
        }

        if (millis() - lastProgress >= _writeTimeoutMs) {
            fail(AudioError::WriteTimeout);
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
    flushUntil(nullptr, nullptr);
}

void AudioOutputBaseSink::flushUntil(AudioStopCheck stop, void *userData)
{
    if (!_ready || !_pending) {
        return;
    }

    _lastError = AudioError::None;
    // A partial DMA buffer is never queued by the Pico audio drivers. Pad it
    // first, then wait for all user buffers to return to the free list.
    const int16_t silence[writeChunk] = {};
    while (_bufferOffset != 0) {
        const size_t count = std::min(_bufferWords - _bufferOffset, writeChunk);
        if (writePcm(silence, count, stop, userData) != count) {
            if (_lastError != AudioError::None) {
                end();
            }
            return;
        }
    }

    const uint32_t started = millis();
    while (_output->availableForWrite() < _writeCapacity) {
        if (stop != nullptr && stop(userData)) {
            return;
        }
        if (millis() - started >= _drainTimeoutMs) {
            end();
            fail(AudioError::DrainTimeout);
            return;
        }
        delay(1);
    }

    // The I2S PIO can still hold eight FIFO words and one shifted frame.
    delayMicroseconds((9000000ULL + _sampleRate - 1) / _sampleRate);
    _pending = false;
    _primedFrames = 0;
}

bool AudioOutputBaseSink::discard()
{
    if (!_ready) {
        if (_lastError == AudioError::None) {
            return fail(AudioError::NotReady);
        }
        return false;
    }
    return !_pending || begin(_sampleRate);
}

bool AudioOutputBaseSink::ready() const
{
    return _ready;
}

AudioError AudioOutputBaseSink::lastError() const
{
    return _lastError;
}

const char *AudioOutputBaseSink::lastErrorText() const
{
    switch (_lastError) {
        case AudioError::None: return nullptr;
        case AudioError::NotReady: return "audio output is not initialized";
        case AudioError::InvalidSampleRate: return "sample rate is outside the audio hardware's range";
        case AudioError::InvalidBuffering: return "audio buffering requires at least 3 buffers of 8 DMA words";
        case AudioError::InsufficientMemory: return "audio DMA buffers exceed available heap memory";
        case AudioError::InvalidPins: return "audio GPIO assignment is invalid";
        case AudioError::InvalidFormat: return "audio format is unsupported";
        case AudioError::DmaChannelsUnavailable: return "audio output needs 4 free DMA channels";
        case AudioError::DmaTimerUnavailable: return "PWM audio needs a free DMA timer";
        case AudioError::ConfigurationFailed: return "audio driver rejected configuration";
        case AudioError::StartFailed: return "audio driver could not start. Check available hardware resources.";
        case AudioError::InvalidWrite: return "audio output rejected the PCM frame";
        case AudioError::WriteTimeout: return "audio DMA stopped accepting samples";
        case AudioError::DrainTimeout: return "audio DMA did not finish playback";
    }
    return nullptr;
}

bool AudioOutputBaseSink::takeUnderrun()
{
    const bool underrun = _underrun;
    _underrun = false;
    return underrun;
}

bool AudioOutputBaseSink::fail(AudioError error)
{
    _lastError = error;
    return false;
}
#endif

#if ESPEAK_ARDUINO_HAS_AUDIOOUTPUTBASE && defined(ARDUINO_ARCH_RP2040)

namespace {

bool hasAudioDmaChannels()
{
    unsigned int freeChannels = 0;
    for (unsigned int channel = 0; channel < NUM_DMA_CHANNELS; ++channel) {
        if (!dma_channel_is_claimed(channel) && ++freeChannels == 4) {
            return true;
        }
    }
    return false;
}

} // namespace

PWMSink::PWMSink(pin_size_t pin)
    : AudioOutputBaseSink(false), _pwm(pin), _pin(pin)
{
    attachOutput(_pwm);
}

bool PWMSink::begin(int sampleRate)
{
    end();
    if (_pin >= __GPIOCNT || (stereo() && (_pin % 2 != 0 || _pin + 1 >= __GPIOCNT))) {
        return fail(AudioError::InvalidPins);
    }
    const uint32_t systemClock = clock_get_hz(clk_sys);
    if (sampleRate <= 0 || static_cast<uint32_t>(sampleRate) > systemClock || 65535ULL * sampleRate < systemClock) {
        return fail(AudioError::InvalidSampleRate);
    }

    // The Arduino-Pico PWM driver requires one DMA timer and four channels.
    // It can leave resources claimed if it discovers exhaustion during begin().
    if (!hasAudioDmaChannels()) {
        return fail(AudioError::DmaChannelsUnavailable);
    }
    for (unsigned int timer = 0; timer < NUM_DMA_TIMERS; ++timer) {
        if (!dma_timer_is_claimed(timer)) {
            return AudioOutputBaseSink::begin(sampleRate);
        }
    }
    return fail(AudioError::DmaTimerUnavailable);
}

I2SSink::I2SSink(pin_size_t bitClock, pin_size_t dataOut)
    : I2SSink(bitClock, static_cast<pin_size_t>(bitClock + 1), dataOut)
{
}

I2SSink::I2SSink(pin_size_t bitClock, pin_size_t wordSelect, pin_size_t dataOut, I2SFormat format)
    : AudioOutputBaseSink(true), _i2s(OUTPUT), _bitClock(bitClock), _wordSelect(wordSelect), _dataOut(dataOut), _format(format)
{
    attachOutput(_i2s);
}

bool I2SSink::begin(int sampleRate)
{
    end();
    const pin_size_t firstClock = std::min(_bitClock, _wordSelect);
    const pin_size_t lastClock = std::max(_bitClock, _wordSelect);
    if (lastClock >= __GPIOCNT || lastClock - firstClock != 1 || _dataOut >= __GPIOCNT ||
        _dataOut == _bitClock || _dataOut == _wordSelect) {
        return fail(AudioError::InvalidPins);
    }
    if (_format != I2SFormat::Standard && _format != I2SFormat::LeftJustified) {
        return fail(AudioError::InvalidFormat);
    }

    // A stereo 16-bit frame takes 64 PIO cycles. Its divider spans 1 to 65536.
    const uint32_t systemClock = clock_get_hz(clk_sys);
    if (sampleRate <= 0 || 64ULL * sampleRate > systemClock || 64ULL * 65536 * sampleRate < systemClock) {
        return fail(AudioError::InvalidSampleRate);
    }
    if (!hasAudioDmaChannels()) {
        return fail(AudioError::DmaChannelsUnavailable);
    }
    if (!_i2s.setBCLK(firstClock) || !_i2s.setDOUT(_dataOut) ||
        (_wordSelect < _bitClock && !_i2s.swapClocks()) ||
        (_format == I2SFormat::LeftJustified && !_i2s.setLSBJFormat())) {
        return fail(AudioError::ConfigurationFailed);
    }
    return AudioOutputBaseSink::begin(sampleRate);
}

#endif

} // namespace espeak
