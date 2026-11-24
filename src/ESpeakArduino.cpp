// SPDX-License-Identifier: GPL-3.0-or-later
#include "ESpeakArduino.h"
#include "espeak-ng/core/speech.h"

#include <cerrno>
#include <climits>
#include <new>
#include <utility>

#if __has_include(<pico/multicore.h>)
#include <pico/multicore.h>
#endif

extern "C" int GetFileLength(const char *filename);

namespace espeak {
namespace {

unsigned int currentCore()
{
#if __has_include(<pico/multicore.h>)
    return get_core_num();
#else
    return 0;
#endif
}

espeak_ERROR legacyError(espeak_ng_STATUS status)
{
    switch (static_cast<int>(status)) {
    case ENS_OK:
        return EE_OK;
    case ENS_VOICE_NOT_FOUND:
    case ENOENT:
        return EE_NOT_FOUND;
    case ENS_FIFO_BUFFER_FULL:
        return EE_BUFFER_FULL;
    default:
        return EE_INTERNAL_ERROR;
    }
}

const char *parameterError(espeak_PARAMETER parameter, int value, bool relative)
{
    if (static_cast<unsigned int>(parameter) >= N_SPEECH_PARAM) {
        return "unknown speech parameter";
    }
    int64_t absolute = value;
    if (relative && parameter <= espeakRANGE) {
        int64_t base = espeak_GetParameter(parameter, 0);
        if (base * value < INT_MIN || base * value > INT_MAX) {
            return "relative speech parameter is out of range";
        }
        absolute = base + (base * value) / 100;
    }
    switch (parameter) {
    case espeakRATE:
        if (absolute < espeakRATE_MINIMUM || absolute > espeakRATE_MAXIMUM) {
            return "rate must be between 80 and 450 words per minute";
        }
        break;
    case espeakVOLUME:
        if (absolute < 0 || absolute > 200) {
            return "volume must be between 0 and 200";
        }
        break;
    case espeakPITCH:
    case espeakRANGE:
        if (absolute < 0 || absolute > 100) {
            return "pitch and pitch range must be between 0 and 100";
        }
        break;
    case espeakPUNCTUATION:
        if (absolute < espeakPUNCT_NONE || absolute > espeakPUNCT_SOME) {
            return "unknown punctuation mode";
        }
        break;
    case espeakCAPITALS:
    case espeakINTONATION:
    case espeakSSML_BREAK_MUL:
        if (absolute < 0) {
            return "speech parameter cannot be negative";
        }
        break;
    case espeakWORDGAP:
        if (absolute < 0 || absolute > UINT16_MAX / 14) {
            return "word gap is out of range";
        }
        break;
    default:
        break;
    }
    return nullptr;
}

espeak_ng_STATUS validateText(const void *text, size_t size, unsigned int flags)
{
    if (text == nullptr || size == 0 || size > INT_MAX) {
        return static_cast<espeak_ng_STATUS>(EINVAL);
    }
    switch (flags & 7) {
    case espeakCHARS_AUTO:
    case espeakCHARS_UTF8:
    case espeakCHARS_8BIT:
        if (memchr(text, 0, size) != nullptr) {
            return ENS_OK;
        }
        break;
    case espeakCHARS_WCHAR: {
        if (reinterpret_cast<uintptr_t>(text) % alignof(wchar_t) != 0) {
            return static_cast<espeak_ng_STATUS>(EINVAL);
        }
        const auto *characters = static_cast<const wchar_t *>(text);
        for (size_t i = 0; i < size / sizeof(wchar_t); ++i) {
            if (characters[i] == L'\0') {
                return ENS_OK;
            }
        }
        break;
    }
    case espeakCHARS_16BIT: {
        const auto *bytes = static_cast<const uint8_t *>(text);
        for (size_t i = 0; i + 1 < size; i += 2) {
            if (bytes[i] == 0 && bytes[i + 1] == 0) {
                return ENS_OK;
            }
        }
        break;
    }
    default:
        return ENS_UNKNOWN_TEXT_ENCODING;
    }
    return ENS_UNEXPECTED_EOF;
}

} // namespace

std::atomic<Synth *> Synth::_active{nullptr};

Synth::Synth() = default;

Synth::~Synth()
{
    end();
}

bool Synth::begin(const Config &config)
{
    return begin(_nullSink, config);
}

bool Synth::begin(AudioSink &sink, const Config &config)
{
    if (_active == this && !onOwnerCore()) {
        return false;
    }
    if (isPlaying()) {
        return fail(EE_INTERNAL_ERROR, "the synthesizer is speaking");
    }
    if (config.dataPath == nullptr || config.dataPath[0] != '/' || strlen(config.dataPath) >= sizeof(path_home)) {
        return fail(EE_INTERNAL_ERROR, "dataPath must be a valid absolute path", static_cast<espeak_ng_STATUS>(EINVAL));
    }
    if (config.bufferLengthMs < 0 || config.bufferLengthMs > 1000) {
        return fail(EE_INTERNAL_ERROR, "bufferLengthMs must be between 0 and 1000", static_cast<espeak_ng_STATUS>(EINVAL));
    }
    String path(config.dataPath);
    if (path.length() != strlen(config.dataPath)) {
        return rememberStatus(static_cast<espeak_ng_STATUS>(ENOMEM));
    }
    while (path.length() > 1 && path.endsWith("/")) {
        path.remove(path.length() - 1);
    }

    end();
    Synth *expected = nullptr;
    if (!_active.compare_exchange_strong(expected, this)) {
        return fail(EE_INTERNAL_ERROR, "another Synth is using eSpeak NG");
    }
    _ownerCore = currentCore();
    detail::useDataPack(config.dataPack);
    espeak_ng_InitializePath(path.c_str());
    if (config.validateDataFiles && !validateDataPack(path.c_str())) {
        detail::useDataPack(nullptr);
        _active = nullptr;
        return false;
    }

    espeak_ng_ERROR_CONTEXT context = nullptr;
    const auto initialized = espeak_ng_Initialize(&context);
    espeak_ng_ClearErrorContext(&context);
    if (!rememberStatus(initialized)) {
        end();
        return false;
    }
    _sampleRate = espeak_ng_GetSampleRate();
    if (_sampleRate <= 0 || _sampleRate > (INT_MAX - 1000) / 1000) {
        end();
        return fail(EE_INTERNAL_ERROR, "the data pack has an invalid sample rate");
    }
    if (!rememberStatus(espeak_ng_InitializeOutput(ENOUTPUT_MODE_SYNCHRONOUS, config.bufferLengthMs, nullptr))) {
        end();
        return false;
    }

    espeak_SetSynthCallback(&Synth::handleSynthCallback);
    espeak_SetPhonemeCallback(&Synth::handlePhonemeCallback);
    espeak_SetUriCallback(&Synth::handleUriCallback);
    _defaultFlags = config.defaultSynthFlags;
    _initialized = true;
    enablePhonemeEvents((config.initializeOptions & (espeakINITIALIZE_PHONEME_EVENTS | espeakINITIALIZE_PHONEME_IPA)) != 0,
                        config.phonemeEventsUseIpa || (config.initializeOptions & espeakINITIALIZE_PHONEME_IPA) != 0);
    if (!selectInitialVoice(config.requiredLanguage)) {
        end();
        return false;
    }
    if (!sink.begin(_sampleRate)) {
        sink.end();
        end();
        return fail(EE_INTERNAL_ERROR, "audio output did not start", ENS_AUDIO_ERROR);
    }
    _sink = &sink;
    return rememberStatus(ENS_OK);
}

void Synth::end()
{
    if (_active != this || !onOwnerCore() || isPlaying()) {
        return;
    }
    if (_sink != nullptr) {
        _sink->end();
    }
    espeak_SetSynthCallback(nullptr);
    espeak_SetPhonemeCallback(nullptr);
    espeak_SetUriCallback(nullptr);
    espeak_ng_SetOutputHooks(nullptr);
    espeak_ng_SetConstF0(0);
    espeak_ng_Terminate();
    detail::useDataPack(nullptr);
    _initialized = false;
    _sink = nullptr;
    _sampleRate = 0;
    _stopRequestMicros = 0;
    clearQueue();
    _active = nullptr;
}

bool Synth::isInitialized() const
{
    return _initialized;
}

bool Synth::setAudioSink(AudioSink &sink)
{
    if (!checkIdle()) {
        return false;
    }
    if (&sink == _sink && sink.ready()) {
        return rememberStatus(ENS_OK);
    }
    AudioSink *previous = _sink;
    if (previous != nullptr) {
        previous->flush();
        previous->end();
    }
    if (sink.begin(_sampleRate)) {
        _sink = &sink;
        return rememberStatus(ENS_OK);
    }
    sink.end();
    _sink = previous != nullptr && previous->begin(_sampleRate) ? previous : nullptr;
    return fail(EE_INTERNAL_ERROR, "audio output did not start", ENS_AUDIO_ERROR);
}

AudioSink *Synth::audioSink() const
{
    return _sink;
}

bool Synth::say(const char *text)
{
    return synthesize(text, _defaultFlags);
}

bool Synth::say(const __FlashStringHelper *text)
{
    const String copy(text);
    return say(copy.c_str());
}

bool Synth::synthesize(const char *text, unsigned int flags, unsigned int *identifier, void *messageUserData)
{
    return synthesize(text, flags, 0, POS_CHARACTER, 0, identifier, messageUserData);
}

bool Synth::synthesize(const char *text, unsigned int flags, unsigned int position,
                       espeak_POSITION_TYPE positionType, unsigned int endPosition,
                       unsigned int *identifier, void *messageUserData)
{
    return synthesizeText(text, text != nullptr ? strlen(text) + 1 : 0, flags, position,
                          positionType, endPosition, identifier, messageUserData);
}

bool Synth::synthesizeText(const void *text, size_t size, unsigned int flags, unsigned int position,
                           espeak_POSITION_TYPE positionType, unsigned int endPosition,
                           unsigned int *identifier, void *messageUserData)
{
    if (!rememberStatus(validateText(text, size, flags))) {
        return false;
    }
    if (!startSpeech()) {
        return false;
    }
    return finishSpeech(espeak_ng_Synthesize(text, size, position, positionType, endPosition,
                                              flags, identifier, messageUserData));
}

bool Synth::synthesizeMarkText(const void *text, size_t size, const char *mark, unsigned int flags,
                               unsigned int endPosition, unsigned int *identifier, void *messageUserData)
{
    if (mark == nullptr) {
        return fail(EE_INTERNAL_ERROR, "mark is required", static_cast<espeak_ng_STATUS>(EINVAL));
    }
    if (!rememberStatus(validateText(text, size, flags))) {
        return false;
    }
    if (!startSpeech()) {
        return false;
    }
    return finishSpeech(espeak_ng_SynthesizeMark(text, size, mark, endPosition, flags,
                                                  identifier, messageUserData));
}

bool Synth::synthesizeMark(const char *text, const char *mark, unsigned int flags,
                           unsigned int endPosition, unsigned int *identifier, void *messageUserData)
{
    return synthesizeMarkText(text, text != nullptr ? strlen(text) + 1 : 0, mark, flags,
                              endPosition, identifier, messageUserData);
}

bool Synth::synthesizeMark(const __FlashStringHelper *text, const char *mark, unsigned int flags,
                           unsigned int endPosition, unsigned int *identifier, void *messageUserData)
{
    const String copy(text);
    return synthesizeMark(copy.c_str(), mark, flags, endPosition, identifier, messageUserData);
}

bool Synth::sayKey(const char *keyName)
{
    if (keyName == nullptr || keyName[0] == '\0') {
        return fail(EE_INTERNAL_ERROR, "key name is empty", static_cast<espeak_ng_STATUS>(EINVAL));
    }
    if (!startSpeech()) {
        return false;
    }
    return finishSpeech(espeak_ng_SpeakKeyName(keyName));
}

bool Synth::sayKey(const __FlashStringHelper *keyName)
{
    const String copy(keyName);
    return sayKey(copy.c_str());
}

bool Synth::sayCharacter(wchar_t character)
{
    if (!startSpeech()) {
        return false;
    }
    return finishSpeech(espeak_ng_SpeakCharacter(character));
}

bool Synth::synchronize()
{
    if (!checkIdle()) {
        return false;
    }
    if (_sink != nullptr) {
        _sink->flush();
        if (!_sink->ready()) {
            return fail(EE_INTERNAL_ERROR, "audio output did not drain", ENS_AUDIO_ERROR);
        }
    }
    return rememberStatus(espeak_ng_Synchronize());
}

bool Synth::cancel()
{
    if (isPlaying()) {
        requestStop();
        return true;
    }
    if (!checkIdle()) {
        return false;
    }
    if (_sink != nullptr && !_sink->discard()) {
        return fail(EE_INTERNAL_ERROR, "audio output could not discard pending samples", ENS_AUDIO_ERROR);
    }
    return rememberStatus(ENS_OK);
}

void Synth::requestStop()
{
    if (isPlaying()) {
        auto requested = static_cast<uint32_t>(micros());
        uint32_t pending = 0;
        _stopRequestMicros.compare_exchange_strong(pending, requested != 0 ? requested : UINT32_MAX);
    }
}

bool Synth::stopRequested() const
{
    return _stopRequestMicros.load() != 0;
}

uint32_t Synth::lastStopLatencyMicros() const
{
    return _lastStopLatencyMicros.load();
}

uint32_t Synth::stopCount() const
{
    return _stopCount.load();
}

bool Synth::isPlaying() const
{
    return _playing.load();
}

bool Synth::setQueueCapacity(size_t capacity)
{
    if (capacity == _queueCapacity) {
        return true;
    }
    if (capacity < _queueCount || capacity > SIZE_MAX / sizeof(QueueItem)) {
        return fail(EE_BUFFER_FULL, "queue capacity cannot hold pending utterances", ENS_FIFO_BUFFER_FULL);
    }
    std::unique_ptr<QueueItem[]> queue(capacity != 0 ? new (std::nothrow) QueueItem[capacity] : nullptr);
    if (capacity != 0 && !queue) {
        return rememberStatus(static_cast<espeak_ng_STATUS>(ENOMEM));
    }
    for (size_t i = 0; i < _queueCount; ++i) {
        queue[i] = std::move(_queue[(_queueHead + i) % _queueCapacity]);
    }
    _queue = std::move(queue);
    _queueHead = 0;
    _queueCapacity = capacity;
    return true;
}

size_t Synth::queueCapacity() const { return _queueCapacity; }
size_t Synth::queued() const { return _queueCount; }
bool Synth::queueEmpty() const { return _queueCount == 0; }
bool Synth::queueFull() const { return _queueCapacity > 0 && _queueCount == _queueCapacity; }

bool Synth::enqueue(const char *text, unsigned int flags)
{
    if (text == nullptr || text[0] == '\0') {
        return fail(EE_INTERNAL_ERROR, "queue text is empty", static_cast<espeak_ng_STATUS>(EINVAL));
    }
    if (queueFull()) {
        return rememberStatus(ENS_FIFO_BUFFER_FULL);
    }
    String copy(text);
    if (copy.length() != strlen(text)) {
        return rememberStatus(static_cast<espeak_ng_STATUS>(ENOMEM));
    }
    if (_queueCapacity == 0 && !setQueueCapacity(4)) {
        return false;
    }
    auto &item = _queue[(_queueHead + _queueCount) % _queueCapacity];
    item.text = std::move(copy);
    item.flags = flags;
    ++_queueCount;
    return true;
}

bool Synth::enqueue(const __FlashStringHelper *text, unsigned int flags)
{
    const String copy(text);
    return enqueue(copy.c_str(), flags);
}

bool Synth::speakNow(const char *text, unsigned int flags)
{
    if (text == nullptr || text[0] == '\0') {
        return fail(EE_INTERNAL_ERROR, "queue text is empty", static_cast<espeak_ng_STATUS>(EINVAL));
    }
    String copy(text);
    if (copy.length() != strlen(text)) {
        return rememberStatus(static_cast<espeak_ng_STATUS>(ENOMEM));
    }
    if (_queueCapacity == 0 && !setQueueCapacity(4)) {
        return false;
    }
    clearQueue(true);
    _queue[0].text = std::move(copy);
    _queue[0].flags = flags;
    _queueCount = 1;
    return true;
}

bool Synth::speakNow(const __FlashStringHelper *text, unsigned int flags)
{
    const String copy(text);
    return speakNow(copy.c_str(), flags);
}

bool Synth::update()
{
    if (!checkIdle()) {
        return false;
    }
    if (queueEmpty()) {
        return true;
    }
    if (!startSpeech()) {
        return false;
    }
    auto item = std::move(_queue[_queueHead]);
    _queue[_queueHead] = QueueItem{};
    _queueHead = (_queueHead + 1) % _queueCapacity;
    --_queueCount;
    return synthesizeQueued(item.text.c_str(), item.flags);
}

void Synth::clearQueue(bool cancelCurrent)
{
    if (cancelCurrent) {
        requestStop();
    }
    for (size_t i = 0; i < _queueCount; ++i) {
        _queue[(_queueHead + i) % _queueCapacity] = QueueItem{};
    }
    _queueHead = 0;
    _queueCount = 0;
}

bool Synth::setVoice(const char *name)
{
    if (!checkIdle()) {
        return false;
    }
    return name != nullptr ? rememberStatus(espeak_ng_SetVoiceByName(name))
                           : rememberStatus(static_cast<espeak_ng_STATUS>(EINVAL));
}

bool Synth::setVoiceFile(const char *filename)
{
    if (!checkIdle()) {
        return false;
    }
    return filename != nullptr ? rememberStatus(espeak_ng_SetVoiceByFile(filename))
                               : rememberStatus(static_cast<espeak_ng_STATUS>(EINVAL));
}

bool Synth::setVoiceFile(const __FlashStringHelper *filename)
{
    const String copy(filename);
    return setVoiceFile(copy.c_str());
}

bool Synth::setLanguage(const char *language)
{
    if (language == nullptr || language[0] == '\0') {
        return fail(EE_INTERNAL_ERROR, "language is empty", static_cast<espeak_ng_STATUS>(EINVAL));
    }
    espeak_VOICE voice = {};
    voice.languages = language;
    return setVoice(voice);
}

bool Synth::setVariant(const char *variant)
{
    if (!checkIdle()) {
        return false;
    }
    if (variant == nullptr) {
        return rememberStatus(static_cast<espeak_ng_STATUS>(EINVAL));
    }
    const auto *voice = currentVoice();
    if (voice == nullptr || voice->identifier == nullptr) {
        return fail(EE_NOT_FOUND, "select a voice before selecting a variant", ENS_VOICE_NOT_FOUND);
    }
    String base(voice->identifier);
    const int separator = base.indexOf('+');
    if (separator >= 0) {
        base.remove(separator);
    }
    String value(variant);
    value.trim();
    if (value.length() == 0 || value == "none" || value == "default") {
        return setVoice(base.c_str());
    }
    if (value.startsWith("!v/")) {
        value.remove(0, 3);
    }
    if (value.startsWith("+")) {
        value.remove(0, 1);
    }
    base += '+';
    base += value;
    return setVoice(base.c_str());
}

bool Synth::setVoice(const espeak_VOICE &voice)
{
    if (!checkIdle()) {
        return false;
    }
    espeak_VOICE copy = voice;
    return rememberStatus(espeak_ng_SetVoiceByProperties(&copy));
}

espeak_VOICE *Synth::currentVoice() const
{
    return onOwnerCore() && _initialized && !isPlaying() ? espeak_GetCurrentVoice() : nullptr;
}

const espeak_VOICE **Synth::listVoices(espeak_VOICE *filter) const
{
    return onOwnerCore() && _initialized && !isPlaying() ? espeak_ListVoices(filter) : nullptr;
}

const espeak_VOICE **Synth::listVariants() const
{
    espeak_VOICE filter = {};
    filter.languages = "!v";
    return listVoices(&filter);
}

bool Synth::setParameter(espeak_PARAMETER parameter, int value, bool relative)
{
    if (!checkIdle()) {
        return false;
    }
    if (const char *error = parameterError(parameter, value, relative)) {
        return fail(EE_INTERNAL_ERROR, error, static_cast<espeak_ng_STATUS>(EINVAL));
    }
    return rememberStatus(espeak_ng_SetParameter(parameter, value, relative));
}

int Synth::getParameter(espeak_PARAMETER parameter, bool current) const
{
    if (!onOwnerCore() || !_initialized || isPlaying() || static_cast<unsigned int>(parameter) >= N_SPEECH_PARAM) {
        return 0;
    }
    return espeak_GetParameter(parameter, current);
}

bool Synth::setRate(int wordsPerMinute, bool relative)
{
    return setParameter(espeakRATE, wordsPerMinute, relative);
}

bool Synth::setVolume(int volume, bool relative)
{
    return setParameter(espeakVOLUME, volume, relative);
}

bool Synth::setPitch(int pitch, bool relative)
{
    return setParameter(espeakPITCH, pitch, relative);
}

bool Synth::setRange(int range, bool relative)
{
    return setParameter(espeakRANGE, range, relative);
}

bool Synth::setWordGap(int gap10ms, bool relative)
{
    return setParameter(espeakWORDGAP, gap10ms, relative);
}

bool Synth::setCapitals(int modeOrPitchHz, bool relative)
{
    return setParameter(espeakCAPITALS, modeOrPitchHz, relative);
}

bool Synth::setPunctuation(espeak_PUNCT_TYPE mode)
{
    return setParameter(espeakPUNCTUATION, static_cast<int>(mode), false);
}

bool Synth::setIntonation(int intonation, bool relative)
{
    return setParameter(espeakINTONATION, intonation, relative);
}

bool Synth::setSsmlBreakMultiplier(int percent, bool relative)
{
    return setParameter(espeakSSML_BREAK_MUL, percent, relative);
}

bool Synth::setPunctuationList(const wchar_t *punctuation)
{
    return checkIdle() && rememberStatus(espeak_ng_SetPunctuationList(punctuation));
}

Settings Synth::settings(bool current) const
{
    Settings values;
    values.rate = getParameter(espeakRATE, current);
    values.volume = getParameter(espeakVOLUME, current);
    values.pitch = getParameter(espeakPITCH, current);
    values.range = getParameter(espeakRANGE, current);
    values.punctuation = getParameter(espeakPUNCTUATION, current);
    values.capitals = getParameter(espeakCAPITALS, current);
    values.wordGap = getParameter(espeakWORDGAP, current);
    values.intonation = getParameter(espeakINTONATION, current);
    values.ssmlBreakMultiplier = getParameter(espeakSSML_BREAK_MUL, current);
    return values;
}

bool Synth::applySettings(const Settings &values)
{
    if (!checkIdle()) {
        return false;
    }
    const struct { espeak_PARAMETER parameter; int value; } parameters[] = {
        {espeakRATE, values.rate}, {espeakVOLUME, values.volume}, {espeakPITCH, values.pitch},
        {espeakRANGE, values.range}, {espeakPUNCTUATION, values.punctuation},
        {espeakCAPITALS, values.capitals}, {espeakWORDGAP, values.wordGap},
        {espeakINTONATION, values.intonation}, {espeakSSML_BREAK_MUL, values.ssmlBreakMultiplier},
    };
    for (const auto &entry : parameters) {
        if (const char *error = parameterError(entry.parameter, entry.value, false)) {
            return fail(EE_INTERNAL_ERROR, error, static_cast<espeak_ng_STATUS>(EINVAL));
        }
    }
    for (const auto &entry : parameters) {
        if (!rememberStatus(espeak_ng_SetParameter(entry.parameter, entry.value, 0))) {
            return false;
        }
    }
    return true;
}

void Synth::enablePhonemeEvents(bool enabled, bool ipa)
{
    if (checkIdle()) {
        espeak_ng_SetPhonemeEvents(enabled ? 1 : 0, ipa ? 1 : 0);
    }
}

void Synth::setPhonemeTrace(int mode, FILE *stream)
{
    if (checkIdle()) {
        espeak_SetPhonemeTrace(mode, stream);
    }
}

String Synth::textToPhonemes(const char *text, int textMode, int phonemeMode) const
{
    String result;
    if (!onOwnerCore() || !_initialized || isPlaying() || text == nullptr) {
        return result;
    }

    const void *cursor = text;
    while (cursor != nullptr) {
        const void *previous = cursor;
        const char *clause = espeak_TextToPhonemes(&cursor, textMode, phonemeMode);
        if (clause != nullptr) {
            if (result.length() > 0) {
                result += ' ';
            }
            result += clause;
        }
        if (cursor == previous) {
            break;
        }
    }
    return result;
}

String Synth::textToPhonemesWithTerminator(const char *text,
                                           int *lastTerminator,
                                           int textMode,
                                           int phonemeMode) const
{
    String result;
    if (lastTerminator != nullptr) {
        *lastTerminator = 0;
    }
    if (!onOwnerCore() || !_initialized || isPlaying() || text == nullptr) {
        return result;
    }

    const void *cursor = text;
    while (cursor != nullptr) {
        int terminator = 0;
        String clause = textToPhonemeClause(&cursor, &terminator, textMode, phonemeMode);
        if (clause.length() > 0) {
            if (result.length() > 0) {
                result += ' ';
            }
            result += clause;
        }
        if (lastTerminator != nullptr) {
            *lastTerminator = terminator;
        }
    }
    return result;
}

String Synth::textToPhonemeClause(const void **textCursor,
                                  int *terminator,
                                  int textMode,
                                  int phonemeMode) const
{
    if (terminator != nullptr) {
        *terminator = 0;
    }
    if (!onOwnerCore() || !_initialized || isPlaying() || textCursor == nullptr || *textCursor == nullptr) {
        return String();
    }

    const void *previous = *textCursor;
    int localTerminator = 0;
    const char *clause = espeak_TextToPhonemesWithTerminator(textCursor,
                                                            textMode,
                                                            phonemeMode,
                                                            &localTerminator);
    if (*textCursor == previous) {
        *textCursor = nullptr;
    }
    if (terminator != nullptr) {
        *terminator = localTerminator;
    }
    return clause != nullptr ? String(clause) : String();
}

String Synth::textToIpa(const char *text) const
{
    return textToPhonemes(text, espeakCHARS_UTF8, espeakPHONEMES_IPA);
}

void Synth::onEvent(EventCallback callback, void *userData)
{
    _eventCallback = callback;
    _eventUserData = userData;
}

void Synth::onPhoneme(PhonemeCallback callback, void *userData)
{
    _phonemeCallback = callback;
    _phonemeUserData = userData;
}

void Synth::onUri(UriCallback callback, void *userData)
{
    _uriCallback = callback;
    _uriUserData = userData;
}

bool Synth::compileDictionary(const char *sourcePath,
                              const char *dictionaryName,
                              FILE *log,
                              int flags,
                              espeak_ng_ERROR_CONTEXT *context)
{
    return checkIdle() && rememberStatus(espeak_ng_CompileDictionary(sourcePath, dictionaryName, log, flags, context));
}

bool Synth::compilePhonemeData(long rate, FILE *log, espeak_ng_ERROR_CONTEXT *context)
{
    return checkIdle() && rememberStatus(espeak_ng_CompilePhonemeData(rate, log, context));
}

bool Synth::compilePhonemeDataPath(long rate,
                                   const char *sourcePath,
                                   const char *destinationPath,
                                   FILE *log,
                                   espeak_ng_ERROR_CONTEXT *context)
{
    return checkIdle() && rememberStatus(espeak_ng_CompilePhonemeDataPath(rate, sourcePath, destinationPath, log, context));
}

bool Synth::compileIntonation(FILE *log, espeak_ng_ERROR_CONTEXT *context)
{
    return checkIdle() && rememberStatus(espeak_ng_CompileIntonation(log, context));
}

bool Synth::compileIntonationPath(const char *sourcePath,
                                  const char *destinationPath,
                                  FILE *log,
                                  espeak_ng_ERROR_CONTEXT *context)
{
    return checkIdle() && rememberStatus(espeak_ng_CompileIntonationPath(sourcePath, destinationPath, log, context));
}

bool Synth::setOutputHooks(espeak_ng_OUTPUT_HOOKS *hooks)
{
    if (!checkIdle()) {
        return false;
    }
    if (hooks != nullptr) {
        _outputHooks = *hooks;
    }
    return rememberStatus(espeak_ng_SetOutputHooks(hooks != nullptr ? &_outputHooks : nullptr));
}

bool Synth::setConstF0(int f0)
{
    if (!checkIdle()) {
        return false;
    }
    if (f0 < 0 || f0 > (INT_MAX >> 12)) {
        return fail(EE_INTERNAL_ERROR, "fixed frequency is out of range", static_cast<espeak_ng_STATUS>(EINVAL));
    }
    return rememberStatus(espeak_ng_SetConstF0(f0));
}

bool Synth::setRandSeed(long seed)
{
    return checkIdle() && rememberStatus(espeak_ng_SetRandSeed(seed));
}

int Synth::sampleRate() const
{
    return _sampleRate;
}

const char *Synth::version() const
{
    return espeak_Info(nullptr);
}

const char *Synth::dataPath() const
{
    const char *path = nullptr;
    if (_initialized) {
        espeak_Info(&path);
    }
    return path;
}

espeak_ERROR Synth::lastError() const
{
    return _lastError;
}

const char *Synth::lastErrorText() const
{
    if (_lastErrorMessage[0] != '\0') {
        return _lastErrorMessage;
    }

    switch (_lastError) {
    case EE_OK:
        return "OK";
    case EE_INTERNAL_ERROR:
        return "internal error";
    case EE_BUFFER_FULL:
        return "buffer full";
    case EE_NOT_FOUND:
        return "not found";
    default:
        return "unknown error";
    }
}

espeak_ng_STATUS Synth::lastStatus() const
{
    return _lastStatus;
}

const char *Synth::lastStatusText() const
{
    if (_lastStatusMessage[0] != '\0') {
        return _lastStatusMessage;
    }
    if (_lastStatus == ENS_OK) {
        return "OK";
    }
    return "unknown status";
}

String Synth::statusCodeMessage(espeak_ng_STATUS status)
{
    char buffer[160] = {};
    espeak_ng_GetStatusCodeMessage(status, buffer, sizeof(buffer));
    return String(buffer[0] != '\0' ? buffer : "unknown status");
}

void Synth::printStatusCodeMessage(espeak_ng_STATUS status,
                                   FILE *out,
                                   espeak_ng_ERROR_CONTEXT context)
{
    espeak_ng_PrintStatusCodeMessage(status, out, context);
}

void Synth::clearErrorContext(espeak_ng_ERROR_CONTEXT *context)
{
    espeak_ng_ClearErrorContext(context);
}

int Synth::handleSynthCallback(short *wav, int sampleCount, espeak_EVENT *events)
{
    auto *synth = _active.load();
    return synth != nullptr ? synth->synthCallback(wav, sampleCount, events) : 1;
}

int Synth::handlePhonemeCallback(const char *phoneme)
{
    auto *synth = _active.load();
    if (synth != nullptr && synth->_phonemeCallback != nullptr) {
        synth->_phonemeCallback(phoneme, synth->_phonemeUserData);
    }
    return 0;
}

int Synth::handleUriCallback(int type, const char *uri, const char *base)
{
    auto *synth = _active.load();
    return synth != nullptr && synth->_uriCallback != nullptr
        ? synth->_uriCallback(type, uri, base, synth->_uriUserData) : 1;
}

bool Synth::onOwnerCore() const
{
    return _ownerCore.load() == currentCore();
}

bool Synth::checkIdle()
{
    if (_active == this && !onOwnerCore()) {
        return false;
    }
    if (!_initialized) {
        return fail(EE_INTERNAL_ERROR, "synthesizer is not initialized", ENS_NOT_INITIALIZED);
    }
    return !isPlaying() || fail(EE_INTERNAL_ERROR, "the synthesizer is speaking");
}

bool Synth::startSpeech()
{
    if (!checkIdle()) {
        return false;
    }
    if (_sink == nullptr || !_sink->ready()) {
        return fail(EE_INTERNAL_ERROR, "audio output is unavailable", ENS_AUDIO_ERROR);
    }
    _callbackError = nullptr;
    _stopRequestMicros = 0;
    _playing = true;
    return true;
}

bool Synth::selectInitialVoice(const char *language)
{
    if (language != nullptr && language[0] != '\0') {
        return setLanguage(language);
    }
    if (setVoice("gmw/" ESPEAKNG_DEFAULT_VOICE) || setVoice(ESPEAKNG_DEFAULT_VOICE)) {
        return true;
    }
    const auto *entry = listVoices();
    for (; entry != nullptr && *entry != nullptr; ++entry) {
        if (setVoice((*entry)->identifier)) {
            return true;
        }
    }
    return fail(EE_NOT_FOUND, "data pack contains no usable voice and dictionary", ENS_VOICE_NOT_FOUND);
}

bool Synth::finishSpeech(espeak_ng_STATUS status)
{
    if (status == ENS_OK && !stopRequested()) {
        _sink->flush();
        if (!_sink->ready()) {
            _callbackError = "audio output did not drain";
        }
    }
    if (status != ENS_OK || stopRequested() || _callbackError != nullptr) {
        if (!_sink->discard() && _callbackError == nullptr) {
            _callbackError = "audio output could not discard pending samples";
        }
    }
    _playing = false;
    if (const uint32_t requested = _stopRequestMicros.load()) {
        _lastStopLatencyMicros = static_cast<uint32_t>(micros()) - requested;
        ++_stopCount;
        if (status == ENS_OK) {
            status = ENS_SPEECH_STOPPED;
        }
    }
    if (_callbackError != nullptr) {
        return fail(EE_INTERNAL_ERROR, _callbackError, ENS_AUDIO_ERROR);
    }
    return rememberStatus(status);
}

bool Synth::synthesizeQueued(const char *text, unsigned int flags)
{
    if (stopRequested()) {
        return finishSpeech(ENS_SPEECH_STOPPED);
    }
    return finishSpeech(espeak_ng_Synthesize(text, strlen(text) + 1, 0, POS_CHARACTER, 0,
                                              flags == 0 ? _defaultFlags : flags, nullptr, nullptr));
}

int Synth::synthCallback(short *wav, int sampleCount, espeak_EVENT *events)
{
    if (stopRequested()) {
        return 1;
    }
    for (auto *event = events; event != nullptr && event->type != espeakEVENT_LIST_TERMINATED; ++event) {
        if (event->type == espeakEVENT_SAMPLERATE && event->id.number > 0 && event->id.number != _sampleRate) {
            _sink->flush();
            if (!_sink->ready()) {
                _callbackError = "audio output did not drain before changing sample rate";
                return 1;
            }
            if (stopRequested()) {
                return 1;
            }
            _sink->end();
            if (!_sink->begin(event->id.number)) {
                _callbackError = "audio output could not change sample rate";
                return 1;
            }
            _sampleRate = event->id.number;
        }
        if (_eventCallback != nullptr) {
            _eventCallback(*event, _eventUserData);
        }
        if (stopRequested()) {
            return 1;
        }
    }
    if (wav != nullptr && sampleCount > 0) {
        const auto *samples = reinterpret_cast<const int16_t *>(wav);
        const size_t count = static_cast<size_t>(sampleCount);
        for (size_t offset = 0; offset < count;) {
            if (stopRequested()) {
                return 1;
            }
            const size_t chunk = min(size_t{32}, count - offset);
            if (_sink->writeSamples(samples + offset, chunk) != chunk) {
                _callbackError = "audio output did not accept the samples";
                return 1;
            }
            offset += chunk;
        }
    }
    return 0;
}

bool Synth::validateDataPack(const char *path)
{
    const char *data = nullptr;
    espeak_Info(&data);
    const String expected(path);
    String nested(expected);
    if (!nested.endsWith("/")) {
        nested += '/';
    }
    nested += "espeak-ng-data";
    String resolved(data);
    while (resolved.startsWith("//")) {
        resolved.remove(0, 1);
    }
    while (resolved.length() > 1 && resolved.endsWith("/")) {
        resolved.remove(resolved.length() - 1);
    }
    if ((resolved != expected && resolved != nested) || GetFileLength(data) != -EISDIR) {
        return fail(EE_NOT_FOUND, "data pack directory was not found at dataPath", static_cast<espeak_ng_STATUS>(ENOENT));
    }
    const char *required[] = {"phontab", "phonindex", "phondata", "intonations"};
    for (const char *filename : required) {
        const String fullPath = resolved + (resolved.endsWith("/") ? "" : "/") + filename;
        if (GetFileLength(fullPath.c_str()) <= 0) {
            const String message = String("missing or empty data file: ") + fullPath;
            return fail(EE_NOT_FOUND, message.c_str(), static_cast<espeak_ng_STATUS>(ENOENT));
        }
    }
    return true;
}

bool Synth::fail(espeak_ERROR error, const char *message, espeak_ng_STATUS status)
{
    _lastError = error;
    _lastStatus = status;
    snprintf(_lastErrorMessage, sizeof(_lastErrorMessage), "%s", message);
    snprintf(_lastStatusMessage, sizeof(_lastStatusMessage), "%s", message);
    return false;
}

bool Synth::rememberStatus(espeak_ng_STATUS status)
{
    _lastStatus = status;
    _lastError = legacyError(status);
    _lastErrorMessage[0] = '\0';
    _lastStatusMessage[0] = '\0';
    if (status != ENS_OK) {
        espeak_ng_GetStatusCodeMessage(status, _lastStatusMessage, sizeof(_lastStatusMessage));
        snprintf(_lastErrorMessage, sizeof(_lastErrorMessage), "%s", _lastStatusMessage);
    }
    return status == ENS_OK;
}

class BackgroundSynth::QueueLock {
public:
    explicit QueueLock(const BackgroundSynth &owner) : _owner(owner)
    {
#if __has_include(<pico/mutex.h>)
        mutex_enter_blocking(&_owner._mutex);
#else
        _owner._mutex.lock();
#endif
    }

    ~QueueLock()
    {
#if __has_include(<pico/mutex.h>)
        mutex_exit(&_owner._mutex);
#else
        _owner._mutex.unlock();
#endif
    }

    QueueLock(const QueueLock &) = delete;
    QueueLock &operator=(const QueueLock &) = delete;

private:
    const BackgroundSynth &_owner;
};

BackgroundSynth::BackgroundSynth(Synth &synth) : _synth(synth)
{
#if __has_include(<pico/mutex.h>)
    mutex_init(&_mutex);
#endif
}

BackgroundSynth::~BackgroundSynth() = default;

bool BackgroundSynth::begin(AudioSink &sink, const Config &config, size_t queueCapacity, size_t commandCapacity)
{
    {
        QueueLock lock(*this);
        if (_state != Idle) {
            return false;
        }
        if (!resizeQueue(queueCapacity, commandCapacity)) {
            _lastError = EE_INTERNAL_ERROR;
            _lastStatus = static_cast<espeak_ng_STATUS>(ENOMEM);
            snprintf(_lastErrorMessage, sizeof(_lastErrorMessage), "background queue allocation failed");
            return false;
        }
        _workerCore = currentCore();
        _state = Starting;
    }

    const bool started = _synth.begin(sink, config);
    {
        QueueLock lock(*this);
        recordResult();
        if (!started) {
            _state = Idle;
            return false;
        }
        if (_state == Starting) {
            _state = Running;
            return true;
        }
    }
    _synth.end();
    QueueLock lock(*this);
    _state = Idle;
    return false;
}

void BackgroundSynth::end()
{
    QueueLock lock(*this);
    if (_state == Starting || _state == Running) {
        _state = Stopping;
        _synth.requestStop();
        clear();
    }
}

void BackgroundSynth::loop()
{
    Command command;
    bool stopping = false;
    bool hasWork = false;
    bool prepared = false;
    {
        QueueLock lock(*this);
        if (_workerCore == currentCore() && !_workerBusy) {
            stopping = _state == Stopping;
            hasWork = _state == Running && _count > 0;
            if (hasWork) {
                command = std::move(_queue[_head]);
                _queue[_head] = Command{};
                _head = (_head + 1) % (_speechCapacity + _commandCapacity);
                --_count;
                if (command.type == CommandSpeak) {
                    --_speechCount;
                    prepared = _synth.startSpeech();
                }
            }
            _workerBusy = stopping || hasWork;
        }
    }

    if (stopping) {
        _synth.end();
        QueueLock lock(*this);
        _workerBusy = false;
        _state = Idle;
        return;
    }
    if (!hasWork) {
        delay(1);
        return;
    }
    if (command.type == CommandSpeak) {
        if (prepared) {
            _synth.synthesizeQueued(command.text.c_str(), command.flags);
        }
    } else {
        applyCommand(command);
    }
    QueueLock lock(*this);
    recordResult();
    _workerBusy = false;
}

bool BackgroundSynth::isRunning() const
{
    QueueLock lock(*this);
    return _state == Running;
}

bool BackgroundSynth::isStopped() const
{
    QueueLock lock(*this);
    return _state == Idle;
}

bool BackgroundSynth::isSpeaking() const { return _synth.isPlaying(); }

size_t BackgroundSynth::queued() const
{
    QueueLock lock(*this);
    return _speechCount;
}

size_t BackgroundSynth::commandCapacity() const
{
    QueueLock lock(*this);
    return _commandCapacity;
}

size_t BackgroundSynth::commandsQueued() const
{
    QueueLock lock(*this);
    return _count - _speechCount;
}

bool BackgroundSynth::commandQueueFull() const
{
    QueueLock lock(*this);
    return _count - _speechCount == _commandCapacity;
}

Settings BackgroundSynth::settings() const
{
    QueueLock lock(*this);
    return _settings;
}

espeak_ERROR BackgroundSynth::lastError() const
{
    QueueLock lock(*this);
    return _lastError;
}

String BackgroundSynth::lastErrorText() const
{
    QueueLock lock(*this);
    return String(_lastErrorMessage);
}

espeak_ng_STATUS BackgroundSynth::lastStatus() const
{
    QueueLock lock(*this);
    return _lastStatus;
}

uint32_t BackgroundSynth::lastStopLatencyMicros() const { return _synth.lastStopLatencyMicros(); }
uint32_t BackgroundSynth::stopCount() const { return _synth.stopCount(); }

bool BackgroundSynth::say(const char *text, unsigned int flags)
{
    return enqueueSpeech(text, flags, false);
}

bool BackgroundSynth::say(const __FlashStringHelper *text, unsigned int flags)
{
    const String copy(text);
    return say(copy.c_str(), flags);
}

bool BackgroundSynth::speakNow(const char *text, unsigned int flags)
{
    return enqueueSpeech(text, flags, true);
}

bool BackgroundSynth::speakNow(const __FlashStringHelper *text, unsigned int flags)
{
    const String copy(text);
    return speakNow(copy.c_str(), flags);
}

bool BackgroundSynth::setCommandCapacity(size_t capacity)
{
    QueueLock lock(*this);
    return resizeQueue(_speechCapacity, capacity);
}

bool BackgroundSynth::setVoice(const char *name)
{
    return queueTextCommand(CommandSetVoice, name);
}

bool BackgroundSynth::setVoice(const __FlashStringHelper *name)
{
    if (name == nullptr) {
        return false;
    }
    String copy(name);
    return setVoice(copy.c_str());
}

bool BackgroundSynth::setVoiceFile(const char *filename)
{
    return queueTextCommand(CommandSetVoiceFile, filename);
}

bool BackgroundSynth::setVoiceFile(const __FlashStringHelper *filename)
{
    if (filename == nullptr) {
        return false;
    }
    String copy(filename);
    return setVoiceFile(copy.c_str());
}

bool BackgroundSynth::setLanguage(const char *language)
{
    return queueTextCommand(CommandSetLanguage, language);
}

bool BackgroundSynth::setLanguage(const __FlashStringHelper *language)
{
    if (language == nullptr) {
        return false;
    }
    String copy(language);
    return setLanguage(copy.c_str());
}

bool BackgroundSynth::setVariant(const char *variant)
{
    return queueTextCommand(CommandSetVariant, variant);
}

bool BackgroundSynth::setVariant(const __FlashStringHelper *variant)
{
    if (variant == nullptr) {
        return false;
    }
    String copy(variant);
    return setVariant(copy.c_str());
}

bool BackgroundSynth::setParameter(espeak_PARAMETER parameter, int value, bool relative)
{
    Command command;
    command.type = CommandSetParameter;
    command.parameter = parameter;
    command.value = value;
    command.relative = relative;
    QueueLock lock(*this);
    return push(std::move(command));
}

bool BackgroundSynth::setRate(int wordsPerMinute, bool relative)
{
    return setParameter(espeakRATE, wordsPerMinute, relative);
}

bool BackgroundSynth::setVolume(int volume, bool relative)
{
    return setParameter(espeakVOLUME, volume, relative);
}

bool BackgroundSynth::setPitch(int pitch, bool relative)
{
    return setParameter(espeakPITCH, pitch, relative);
}

bool BackgroundSynth::setRange(int range, bool relative)
{
    return setParameter(espeakRANGE, range, relative);
}

bool BackgroundSynth::setWordGap(int gap10ms, bool relative)
{
    return setParameter(espeakWORDGAP, gap10ms, relative);
}

bool BackgroundSynth::setCapitals(int modeOrPitchHz, bool relative)
{
    return setParameter(espeakCAPITALS, modeOrPitchHz, relative);
}

bool BackgroundSynth::setPunctuation(espeak_PUNCT_TYPE mode)
{
    return setParameter(espeakPUNCTUATION, static_cast<int>(mode), false);
}

bool BackgroundSynth::setIntonation(int intonation, bool relative)
{
    return setParameter(espeakINTONATION, intonation, relative);
}

bool BackgroundSynth::setSsmlBreakMultiplier(int percent, bool relative)
{
    return setParameter(espeakSSML_BREAK_MUL, percent, relative);
}

bool BackgroundSynth::setOutputHooks(espeak_ng_OUTPUT_HOOKS *hooks)
{
    Command command;
    command.type = CommandSetOutputHooks;
    command.useHooks = hooks != nullptr;
    if (hooks != nullptr) {
        command.hooks = *hooks;
    }
    QueueLock lock(*this);
    return push(std::move(command));
}

bool BackgroundSynth::setConstF0(int f0)
{
    Command command;
    command.type = CommandSetConstF0;
    command.value = f0;
    QueueLock lock(*this);
    return push(std::move(command));
}

bool BackgroundSynth::setRandSeed(long seed)
{
    Command command;
    command.type = CommandSetRandSeed;
    command.value = seed;
    QueueLock lock(*this);
    return push(std::move(command));
}

bool BackgroundSynth::applySettings(const Settings &settings)
{
    Command command;
    command.type = CommandApplySettings;
    command.settings = settings;
    QueueLock lock(*this);
    return push(std::move(command));
}

void BackgroundSynth::stop(bool clearPending)
{
    QueueLock lock(*this);
    _synth.requestStop();
    if (clearPending) {
        removePendingSpeech();
    }
}

Synth &BackgroundSynth::synth() { return _synth; }

bool BackgroundSynth::resizeQueue(size_t speechCapacity, size_t commandCapacity)
{
    if (speechCapacity == _speechCapacity && commandCapacity == _commandCapacity) {
        return true;
    }
    constexpr size_t limit = SIZE_MAX / sizeof(Command);
    if (commandCapacity > limit || speechCapacity > limit - commandCapacity ||
        speechCapacity < _speechCount || commandCapacity < _count - _speechCount) {
        return false;
    }
    const size_t capacity = speechCapacity + commandCapacity;
    std::unique_ptr<Command[]> queue(capacity > 0 ? new (std::nothrow) Command[capacity] : nullptr);
    if (capacity > 0 && !queue) {
        return false;
    }
    for (size_t i = 0; i < _count; ++i) {
        queue[i] = std::move(_queue[(_head + i) % (_speechCapacity + _commandCapacity)]);
    }
    _queue = std::move(queue);
    _head = 0;
    _speechCapacity = speechCapacity;
    _commandCapacity = commandCapacity;
    return true;
}

bool BackgroundSynth::push(Command &&command)
{
    if (_state != Running) {
        return false;
    }
    if (command.type == CommandSpeak) {
        if (_speechCount == _speechCapacity) {
            return false;
        }
        ++_speechCount;
    } else if (_count - _speechCount == _commandCapacity) {
        return false;
    }
    _queue[(_head + _count) % (_speechCapacity + _commandCapacity)] = std::move(command);
    ++_count;
    return true;
}

bool BackgroundSynth::enqueueSpeech(const char *text, unsigned int flags, bool replace)
{
    if (text == nullptr || text[0] == '\0') {
        return false;
    }
    Command command;
    command.text = text;
    command.flags = flags;
    if (command.text.length() != strlen(text)) {
        return false;
    }
    QueueLock lock(*this);
    if (_state != Running || _speechCapacity == 0) {
        return false;
    }
    if (replace) {
        removePendingSpeech();
        _synth.requestStop();
    }
    return push(std::move(command));
}

bool BackgroundSynth::queueTextCommand(CommandType type, const char *text)
{
    if (text == nullptr || (text[0] == '\0' && type != CommandSetVariant)) {
        return false;
    }
    Command command;
    command.type = type;
    command.text = text;
    if (command.text.length() != strlen(text)) {
        return false;
    }
    QueueLock lock(*this);
    return push(std::move(command));
}

void BackgroundSynth::removePendingSpeech()
{
    if (_speechCount == 0) {
        return;
    }
    const size_t capacity = _speechCapacity + _commandCapacity;
    size_t retained = 0;
    for (size_t i = 0; i < _count; ++i) {
        const size_t index = (_head + i) % capacity;
        if (_queue[index].type != CommandSpeak) {
            if (retained != i) {
                _queue[(_head + retained) % capacity] = std::move(_queue[index]);
            }
            ++retained;
        }
    }
    for (size_t i = retained; i < _count; ++i) {
        _queue[(_head + i) % capacity] = Command{};
    }
    _count = retained;
    _speechCount = 0;
}

void BackgroundSynth::clear()
{
    for (size_t i = 0; i < _count; ++i) {
        _queue[(_head + i) % (_speechCapacity + _commandCapacity)] = Command{};
    }
    _head = 0;
    _count = 0;
    _speechCount = 0;
}

void BackgroundSynth::recordResult()
{
    _lastError = _synth.lastError();
    _lastStatus = _synth.lastStatus();
    snprintf(_lastErrorMessage, sizeof(_lastErrorMessage), "%s", _synth.lastErrorText());
    if (_synth.isInitialized()) {
        _settings = _synth.settings();
    }
}

void BackgroundSynth::applyCommand(Command &command)
{
    switch (command.type) {
    case CommandSpeak:
        break;
    case CommandSetVoice:
        _synth.setVoice(command.text.c_str());
        break;
    case CommandSetVoiceFile:
        _synth.setVoiceFile(command.text.c_str());
        break;
    case CommandSetLanguage:
        _synth.setLanguage(command.text.c_str());
        break;
    case CommandSetVariant:
        _synth.setVariant(command.text.c_str());
        break;
    case CommandSetParameter:
        _synth.setParameter(command.parameter, static_cast<int>(command.value), command.relative);
        break;
    case CommandSetOutputHooks:
        _synth.setOutputHooks(command.useHooks ? &command.hooks : nullptr);
        break;
    case CommandSetConstF0:
        _synth.setConstF0(static_cast<int>(command.value));
        break;
    case CommandSetRandSeed:
        _synth.setRandSeed(command.value);
        break;
    case CommandApplySettings:
        _synth.applySettings(command.settings);
        break;
    }
}

} // namespace espeak
