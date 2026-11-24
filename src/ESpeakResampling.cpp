// SPDX-License-Identifier: GPL-3.0-or-later
#include "ESpeakAudio.h"

#include <algorithm>
#include <cstring>

namespace espeak {
namespace {

int16_t roundedPcm(int64_t weightedSample, uint32_t ticks)
{
    const int64_t half = ticks / 2;
    const int64_t rounded = weightedSample >= 0 ? weightedSample + half : weightedSample - half;
    // At normal audio rates the rounded value fits a signed 32-bit divide.
    return ticks < 65536
        ? static_cast<int16_t>(static_cast<int32_t>(rounded) / static_cast<int32_t>(ticks))
        : static_cast<int16_t>(rounded / ticks);
}

bool stopRequested(AudioStopCheck stop, void *userData)
{
    return stop != nullptr && stop(userData);
}

} // namespace

ResamplingSink::ResamplingSink(AudioSink &downstream, int outputSampleRate)
    : _downstream(downstream), _outputRate(outputSampleRate)
{
}

bool ResamplingSink::begin(int sampleRate)
{
    end();
    _error = nullptr;
    _failed = false;
    if (sampleRate <= 0 || _outputRate <= 0) {
        _error = "PCM sample rates must be positive";
        return false;
    }
    if (!_downstream.begin(_outputRate) || !_downstream.ready()) {
        failDownstream("fixed-rate output could not start");
        _downstream.end();
        return false;
    }
    _inputRate = sampleRate;
    return true;
}

void ResamplingSink::end()
{
    if (_inputRate > 0) {
        _downstream.end();
        _inputRate = 0;
    }
    resetStream();
}

bool ResamplingSink::changeSampleRate(int sampleRate)
{
    if (_inputRate == 0) {
        return begin(sampleRate);
    }
    if (_failed || !_downstream.ready()) {
        return _failed ? false : failDownstream("fixed-rate output is unavailable");
    }
    if (sampleRate <= 0) {
        _error = "PCM sample rates must be positive";
        return false;
    }
    if (sampleRate != _inputRate && _needsFlush) {
        _error = "drain the PCM stream before changing its input rate";
        return false;
    }
    _inputRate = sampleRate;
    _error = nullptr;
    return true;
}

size_t ResamplingSink::writeSamples(const int16_t *samples, size_t count)
{
    if (count == 0 || _failed) {
        return 0;
    }
    if (_inputRate == 0) {
        _error = "resampling sink is not initialized";
        return 0;
    }
    if (!_downstream.ready()) {
        failDownstream("fixed-rate output is unavailable");
        return 0;
    }
    if (samples == nullptr) {
        _error = "resampling requires a PCM buffer";
        return 0;
    }

    _error = nullptr;
    _needsFlush = true;
    if (_inputRate == _outputRate) {
        const size_t written = _downstream.writeSamples(samples, count);
        if (written != count) {
            failDownstream("fixed-rate output did not accept the PCM samples");
        }
        return written <= count ? written : 0;
    }

    const bool upsampling = _outputRate > _inputRate;
    size_t consumed = 0;
    for (; consumed < count; ++consumed) {
        if (!(upsampling ? interpolate(samples[consumed]) : average(samples[consumed]))) {
            break;
        }
    }
    return consumed;
}

void ResamplingSink::flush()
{
    flushUntil(nullptr, nullptr);
}

void ResamplingSink::flushUntil(AudioStopCheck stop, void *userData)
{
    if (_inputRate == 0 || _failed || stopRequested(stop, userData)) {
        return;
    }
    if (!_downstream.ready()) {
        failDownstream("fixed-rate output is unavailable");
        return;
    }

    _error = nullptr;
    if (_hasPrevious) {
        // Hold the final input sample to the end of its interval.
        if (!interpolate(_previous, stop, userData)) {
            return;
        }
        _hasPrevious = false;
    } else if (_inputRate > _outputRate && _phase != 0) {
        // Average the part of the last output interval that the stream filled.
        if (!emit(roundedPcm(_area, _phase))) {
            return;
        }
        _phase = 0;
        _area = 0;
    }

    if (stopRequested(stop, userData) || !writeBuffered()) {
        return;
    }
    _downstream.flushUntil(stop, userData);
    if (!_downstream.ready()) {
        failDownstream("fixed-rate output did not finish playback");
    } else if (!stopRequested(stop, userData)) {
        resetStream();
    }
}

bool ResamplingSink::discard()
{
    if (_inputRate == 0) {
        if (_error == nullptr) {
            _error = "resampling sink is not initialized";
        }
        return false;
    }
    resetStream();
    if (!_downstream.discard() || !_downstream.ready()) {
        return failDownstream("fixed-rate output could not discard pending samples");
    }
    _failed = false;
    _error = nullptr;
    return true;
}

bool ResamplingSink::ready() const
{
    return _inputRate > 0 && !_failed && _downstream.ready();
}

bool ResamplingSink::interpolate(int16_t next, AudioStopCheck stop, void *userData)
{
    if (_hasPrevious) {
        const uint32_t interval = static_cast<uint32_t>(_outputRate);
        const int32_t amplitudeChange = static_cast<int32_t>(next) - _previous;
        const int64_t start = static_cast<int64_t>(_previous) * interval;
        while (_phase < interval) {
            if (stopRequested(stop, userData)) {
                return false;
            }
            const int16_t frame = _phase == 0 || amplitudeChange == 0
                ? _previous
                : roundedPcm(start + static_cast<int64_t>(amplitudeChange) * _phase, interval);
            if (!emit(frame)) {
                return false;
            }
            _phase += static_cast<uint32_t>(_inputRate);
        }
        _phase -= interval;
    }
    _previous = next;
    _hasPrevious = true;
    return true;
}

bool ResamplingSink::average(int16_t sample)
{
    const uint32_t interval = static_cast<uint32_t>(_inputRate);
    const uint32_t width = static_cast<uint32_t>(_outputRate);
    const uint32_t ticks = std::min(width, interval - _phase);
    _area += static_cast<int64_t>(sample) * ticks;
    _phase += ticks;
    if (_phase == interval) {
        if (!emit(roundedPcm(_area, interval))) {
            return false;
        }
        _phase = width - ticks;
        _area = static_cast<int64_t>(sample) * _phase;
    }
    return true;
}

bool ResamplingSink::emit(int16_t sample)
{
    _buffer[_buffered++] = sample;
    return _buffered < sizeof(_buffer) / sizeof(_buffer[0]) || writeBuffered();
}

bool ResamplingSink::writeBuffered()
{
    if (_buffered != 0 && _downstream.writeSamples(_buffer, _buffered) != _buffered) {
        return failDownstream("fixed-rate output did not accept the PCM samples");
    }
    _buffered = 0;
    return true;
}

bool ResamplingSink::failDownstream(const char *fallback)
{
    const char *detail = _downstream.lastErrorText();
    if (detail != nullptr && detail[0] != '\0') {
        std::strncpy(_downstreamError, detail, sizeof(_downstreamError) - 1);
        _downstreamError[sizeof(_downstreamError) - 1] = '\0';
        _error = _downstreamError;
    } else {
        _error = fallback;
    }
    _failed = true;
    return false;
}

void ResamplingSink::resetStream()
{
    _phase = 0;
    _area = 0;
    _buffered = 0;
    _hasPrevious = false;
    _needsFlush = false;
}

} // namespace espeak
