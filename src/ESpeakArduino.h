// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <Arduino.h>
#include <atomic>
#include <memory>
#include "ESpeakAudio.h"
#include "ESpeakData.h"
#include <espeak-ng/espeak_ng.h>

#if __has_include(<pico/mutex.h>)
#include <pico/mutex.h>
#else
#include <mutex>
#endif

namespace espeak {

struct Config {
    Config() : Config(englishDataPack()) {}
    explicit Config(const DataPack &pack) : dataPack(&pack) {}
    explicit Config(const char *path) : dataPath(path) {}

    const DataPack *dataPack = nullptr;
    const char *dataPath = kBuiltinDataPath;
    const char *requiredLanguage = nullptr;
    int bufferLengthMs = 60;
    int initializeOptions = espeakINITIALIZE_PHONEME_EVENTS;
    bool phonemeEventsUseIpa = false;
    bool validateDataFiles = true;
    unsigned int defaultSynthFlags = espeakCHARS_UTF8 | espeakENDPAUSE;
};

struct Settings {
    int rate = espeakRATE_NORMAL;
    int volume = 100;
    int pitch = 50;
    int range = 50;
    int punctuation = espeakPUNCT_NONE;
    int capitals = 0;
    int wordGap = 0;
    int intonation = 0;
    int ssmlBreakMultiplier = 100;
};

using SpeechTicket = uint32_t;

enum class SpeechState : uint8_t {
    Unknown,
    Queued,
    Speaking,
    Completed,
    Cancelled,
    Failed,
};

struct SpeechResult {
    SpeechState state = SpeechState::Unknown;
    espeak_ng_STATUS status = ENS_OK;

    bool finished() const {
        return state == SpeechState::Completed || state == SpeechState::Cancelled || state == SpeechState::Failed;
    }
};

using EventCallback = void (*)(const espeak_EVENT &event, void *userData);
using PhonemeCallback = void (*)(const char *phoneme, void *userData);
using UriCallback = int (*)(int type, const char *uri, const char *base, void *userData);

class Synth {
public:
    Synth();
    ~Synth();
    Synth(const Synth &) = delete;
    Synth &operator=(const Synth &) = delete;

    // Only one Synth can own the engine. Use the same core for engine and sink calls.
    bool begin(const Config &config = Config());
    bool begin(AudioSink &sink, const Config &config = Config());
    void end();
    bool isInitialized() const;

    bool setAudioSink(AudioSink &sink);
    AudioSink *audioSink() const;

    bool say(const char *text);
    bool say(const String &text) { return say(text.c_str()); }
    bool say(const __FlashStringHelper *text);
    bool synthesize(const char *text,
                    unsigned int flags,
                    unsigned int *identifier = nullptr,
                    void *messageUserData = nullptr);
    bool synthesize(const char *text,
                    unsigned int flags,
                    unsigned int position,
                    espeak_POSITION_TYPE positionType,
                    unsigned int endPosition = 0,
                    unsigned int *identifier = nullptr,
                    void *messageUserData = nullptr);
    // Text must be terminated in the encoding selected by flags (including wide input).
    bool synthesizeText(const void *text,
                        size_t size,
                        unsigned int flags,
                        unsigned int position = 0,
                        espeak_POSITION_TYPE positionType = POS_CHARACTER,
                        unsigned int endPosition = 0,
                        unsigned int *identifier = nullptr,
                        void *messageUserData = nullptr);
    bool synthesizeMarkText(const void *text,
                            size_t size,
                            const char *mark,
                            unsigned int flags,
                            unsigned int endPosition = 0,
                            unsigned int *identifier = nullptr,
                            void *messageUserData = nullptr);
    bool synthesizeMark(const char *text,
                        const char *mark,
                        unsigned int flags,
                        unsigned int endPosition = 0,
                        unsigned int *identifier = nullptr,
                        void *messageUserData = nullptr);
    bool synthesizeMark(const __FlashStringHelper *text,
                        const char *mark,
                        unsigned int flags,
                        unsigned int endPosition = 0,
                        unsigned int *identifier = nullptr,
                        void *messageUserData = nullptr);
    bool sayKey(const char *keyName);
    bool sayKey(const __FlashStringHelper *keyName);
    bool sayCharacter(wchar_t character);
    bool synchronize();
    bool cancel();
    // Can be called from the other core to cancel active speech.
    void requestStop();
    bool stopRequested() const;
    uint32_t lastStopLatencyMicros() const;
    uint32_t stopCount() const;
    bool isPlaying() const;

    bool setQueueCapacity(size_t capacity);
    size_t queueCapacity() const;
    size_t queued() const;
    bool queueEmpty() const;
    bool queueFull() const;
    bool enqueue(const char *text, unsigned int flags = 0);
    bool enqueue(const __FlashStringHelper *text, unsigned int flags = 0);
    bool speakNow(const char *text, unsigned int flags = 0);
    bool speakNow(const __FlashStringHelper *text, unsigned int flags = 0);
    bool update();
    void clearQueue(bool cancelCurrent = false);

    bool setVoice(const char *name);
    bool setVoiceFile(const char *filename);
    bool setVoiceFile(const __FlashStringHelper *filename);
    bool setLanguage(const char *language);
    bool setVariant(const char *variant);
    bool setVoice(const espeak_VOICE &voice);
    espeak_VOICE *currentVoice() const;
    const espeak_VOICE **listVoices(espeak_VOICE *filter = nullptr) const;
    const espeak_VOICE **listVariants() const;

    bool setParameter(espeak_PARAMETER parameter, int value, bool relative = false);
    int getParameter(espeak_PARAMETER parameter, bool current = true) const;
    bool setRate(int wordsPerMinute, bool relative = false);
    bool setVolume(int volume, bool relative = false);
    bool setPitch(int pitch, bool relative = false);
    bool setRange(int range, bool relative = false);
    bool setWordGap(int gap10ms, bool relative = false);
    bool setCapitals(int modeOrPitchHz, bool relative = false);
    bool setPunctuation(espeak_PUNCT_TYPE mode);
    bool setIntonation(int intonation, bool relative = false);
    bool setSsmlBreakMultiplier(int percent, bool relative = false);
    bool setPunctuationList(const wchar_t *punctuation);
    Settings settings(bool current = true) const;
    bool applySettings(const Settings &settings);

    void enablePhonemeEvents(bool enabled, bool ipa = false);
    void setPhonemeTrace(int mode, FILE *stream = nullptr);
    String textToPhonemes(const char *text,
                          int textMode = espeakCHARS_UTF8,
                          int phonemeMode = espeakPHONEMES_SHOW) const;
    String textToPhonemesWithTerminator(const char *text,
                                        int *lastTerminator = nullptr,
                                        int textMode = espeakCHARS_UTF8,
                                        int phonemeMode = espeakPHONEMES_SHOW) const;
    String textToPhonemeClause(const void **textCursor,
                               int *terminator = nullptr,
                               int textMode = espeakCHARS_UTF8,
                               int phonemeMode = espeakPHONEMES_SHOW) const;
    String textToIpa(const char *text) const;

    void onEvent(EventCallback callback, void *userData = nullptr);
    void onPhoneme(PhonemeCallback callback, void *userData = nullptr);
    void onUri(UriCallback callback, void *userData = nullptr);

    bool compileDictionary(const char *sourcePath,
                           const char *dictionaryName = nullptr,
                           FILE *log = nullptr,
                           int flags = 0,
                           espeak_ng_ERROR_CONTEXT *context = nullptr);
    bool compilePhonemeData(long rate, FILE *log = nullptr, espeak_ng_ERROR_CONTEXT *context = nullptr);
    bool compilePhonemeDataPath(long rate,
                                const char *sourcePath,
                                const char *destinationPath,
                                FILE *log = nullptr,
                                espeak_ng_ERROR_CONTEXT *context = nullptr);
    bool compileIntonation(FILE *log = nullptr, espeak_ng_ERROR_CONTEXT *context = nullptr);
    bool compileIntonationPath(const char *sourcePath,
                               const char *destinationPath,
                               FILE *log = nullptr,
                               espeak_ng_ERROR_CONTEXT *context = nullptr);
    bool setOutputHooks(espeak_ng_OUTPUT_HOOKS *hooks);
    bool setConstF0(int f0);
    bool setRandSeed(long seed);

    int sampleRate() const;
    const char *version() const;
    const char *dataPath() const;
    espeak_ERROR lastError() const;
    const char *lastErrorText() const;
    espeak_ng_STATUS lastStatus() const;
    const char *lastStatusText() const;

    static String statusCodeMessage(espeak_ng_STATUS status);
    static void printStatusCodeMessage(espeak_ng_STATUS status,
                                       FILE *out,
                                       espeak_ng_ERROR_CONTEXT context = nullptr);
    static void clearErrorContext(espeak_ng_ERROR_CONTEXT *context);

private:
    friend class BackgroundSynth;

    static int handleSynthCallback(short *wav, int sampleCount, espeak_EVENT *events);
    static int handlePhonemeCallback(const char *phoneme);
    static int handleUriCallback(int type, const char *uri, const char *base);
    int synthCallback(short *wav, int sampleCount, espeak_EVENT *events);
    bool tryRequestStop();
    bool startSpeech();
    bool finishSpeech(espeak_ng_STATUS status);
    bool synthesizeQueued(const char *text, unsigned int flags);
    bool onOwnerCore() const;
    bool checkIdle();
    bool selectInitialVoice(const char *language);
    bool fail(espeak_ERROR error, const char *message, espeak_ng_STATUS status = ENS_NOT_SUPPORTED);
    bool audioFailure(const AudioSink *sink, const char *fallback);
    bool rememberStatus(espeak_ng_STATUS status);
    bool validateDataPack(const char *path);

    struct QueueItem {
        String text;
        unsigned int flags = 0;
    };

    AudioSink *_sink = nullptr;
    NullAudioSink _nullSink;
    bool _initialized = false;
    unsigned int _defaultFlags = espeakCHARS_UTF8 | espeakENDPAUSE;
    int _sampleRate = 0;
    espeak_ERROR _lastError = EE_OK;
    bool _audioFault = false;
    char _lastErrorMessage[160] = {};
    espeak_ng_STATUS _lastStatus = ENS_OK;
    char _lastStatusMessage[160] = {};
    EventCallback _eventCallback = nullptr;
    void *_eventUserData = nullptr;
    PhonemeCallback _phonemeCallback = nullptr;
    void *_phonemeUserData = nullptr;
    UriCallback _uriCallback = nullptr;
    void *_uriUserData = nullptr;
    espeak_ng_OUTPUT_HOOKS _outputHooks = {};
    std::atomic<unsigned int> _ownerCore{0};
    std::atomic<bool> _playing{false};
    std::atomic<uint32_t> _stopRequestMicros{0};
    std::atomic<uint32_t> _lastStopLatencyMicros{0};
    std::atomic<uint32_t> _stopCount{0};
    std::unique_ptr<QueueItem[]> _queue;
    size_t _queueCapacity = 0;
    size_t _queueHead = 0;
    size_t _queueCount = 0;

    static std::atomic<Synth *> _active;
};

class BackgroundSynth {
public:
    explicit BackgroundSynth(Synth &synth);
    ~BackgroundSynth();
    BackgroundSynth(const BackgroundSynth &) = delete;
    BackgroundSynth &operator=(const BackgroundSynth &) = delete;

    // Call begin and loop on the worker core. Queue requests can come from either core.
    bool begin(AudioSink &sink, const Config &config = Config(), size_t queueCapacity = 4, size_t commandCapacity = 8);
    // Keep calling loop until isStopped() is true before releasing the worker or sink.
    void end();
    void loop();

    bool isRunning() const;
    bool isStopped() const;
    bool isSpeaking() const;
    size_t queued() const;
    size_t commandCapacity() const;
    size_t commandsQueued() const;
    Settings settings() const;
    espeak_ERROR lastError() const;
    String lastErrorText() const;
    espeak_ng_STATUS lastStatus() const;
    bool commandQueueFull() const;
    uint32_t lastStopLatencyMicros() const;
    uint32_t stopCount() const;

    bool say(const char *text, unsigned int flags = 0);
    bool say(const String &text, unsigned int flags = 0) { return say(text.c_str(), flags); }
    bool say(const __FlashStringHelper *text, unsigned int flags = 0);
    bool speakNow(const char *text, unsigned int flags = 0);
    bool speakNow(const __FlashStringHelper *text, unsigned int flags = 0);
    void stop(bool clearPending = true);

    // Returns zero when the queue cannot accept the request.
    SpeechTicket submit(const char *text, unsigned int flags = 0);
    SpeechTicket submit(const String &text, unsigned int flags = 0) { return submit(text.c_str(), flags); }
    SpeechTicket submit(const __FlashStringHelper *text, unsigned int flags = 0);
    SpeechTicket submitNow(const char *text, unsigned int flags = 0);
    SpeechTicket submitNow(const String &text, unsigned int flags = 0) { return submitNow(text.c_str(), flags); }
    SpeechTicket submitNow(const __FlashStringHelper *text, unsigned int flags = 0);
    SpeechResult result(SpeechTicket ticket) const;
    bool cancel(SpeechTicket ticket);
    static constexpr size_t resultHistoryCapacity = 8;

    // Limits future queue entries that carry text. Zero allows any length.
    void setMaxTextBytes(size_t bytes);
    size_t maxTextBytes() const;

    bool setCommandCapacity(size_t capacity);
    bool setVoice(const char *name);
    bool setVoice(const __FlashStringHelper *name);
    bool setVoiceFile(const char *filename);
    bool setVoiceFile(const __FlashStringHelper *filename);
    bool setLanguage(const char *language);
    bool setLanguage(const __FlashStringHelper *language);
    bool setVariant(const char *variant);
    bool setVariant(const __FlashStringHelper *variant);
    bool setParameter(espeak_PARAMETER parameter, int value, bool relative = false);
    bool setRate(int wordsPerMinute, bool relative = false);
    bool setVolume(int volume, bool relative = false);
    bool setPitch(int pitch, bool relative = false);
    bool setRange(int range, bool relative = false);
    bool setWordGap(int gap10ms, bool relative = false);
    bool setCapitals(int modeOrPitchHz, bool relative = false);
    bool setPunctuation(espeak_PUNCT_TYPE mode);
    bool setIntonation(int intonation, bool relative = false);
    bool setSsmlBreakMultiplier(int percent, bool relative = false);
    bool setOutputHooks(espeak_ng_OUTPUT_HOOKS *hooks);
    bool setConstF0(int f0);
    bool setRandSeed(long seed);
    bool applySettings(const Settings &settings);

    Synth &synth();

private:
    class QueueLock;

    enum State { Idle, Starting, Running, Stopping };
    enum CommandType : uint8_t {
        CommandSpeak,
        CommandSetVoice,
        CommandSetVoiceFile,
        CommandSetLanguage,
        CommandSetVariant,
        CommandSetParameter,
        CommandSetOutputHooks,
        CommandSetConstF0,
        CommandSetRandSeed,
        CommandApplySettings,
    };

    struct Command {
        CommandType type = CommandSpeak;
        String text;
        SpeechTicket ticket = 0;
        espeak_PARAMETER parameter = espeakRATE;
        long value = 0;
        bool relative = false;
        unsigned int flags = 0;
        Settings settings;
        bool useHooks = false;
        espeak_ng_OUTPUT_HOOKS hooks = {};
    };

    struct Completion {
        SpeechTicket ticket = 0;
        SpeechResult result;
    };

    bool resizeQueue(size_t speechCapacity, size_t commandCapacity);
    bool push(Command &&command);
    bool enqueueSpeech(const char *text, unsigned int flags, bool replace, SpeechTicket *ticket = nullptr);
    bool queueTextCommand(CommandType type, const char *text);
    static bool copyQueuedText(String &destination, const char *text, size_t limit);
    SpeechResult ticketResult(SpeechTicket ticket) const;
    void addCompletion(SpeechTicket ticket, SpeechResult result);
    void cancelPending(const Command &command);
    void removePendingSpeech();
    void clear();
    void recordResult();
    void applyCommand(Command &command);

    Synth &_synth;
    State _state = Idle;
    unsigned int _workerCore = 0;
    bool _workerBusy = false;
    std::unique_ptr<Command[]> _queue;
    size_t _speechCapacity = 0;
    size_t _commandCapacity = 0;
    size_t _head = 0;
    size_t _count = 0;
    size_t _speechCount = 0;
    size_t _maxTextBytes = 0;
    SpeechTicket _nextTicket = 0;
    SpeechTicket _activeTicket = 0;
    Completion _completions[resultHistoryCapacity];
    size_t _nextCompletion = 0;
    Settings _settings;
    espeak_ERROR _lastError = EE_OK;
    espeak_ng_STATUS _lastStatus = ENS_OK;
    char _lastErrorMessage[160] = "OK";
#if __has_include(<pico/mutex.h>)
    mutable mutex_t _mutex;
#else
    mutable std::mutex _mutex;
#endif
};

} // namespace espeak

using ESpeakConfig = espeak::Config;
using ESpeakSettings = espeak::Settings;
using ESpeakSpeechState = espeak::SpeechState;
using ESpeakSpeechTicket = espeak::SpeechTicket;
using ESpeakSpeechResult = espeak::SpeechResult;
using ESpeakSynth = espeak::Synth;
using ESpeakBackgroundSynth = espeak::BackgroundSynth;
using ESpeakEventCallback = espeak::EventCallback;
using ESpeakPhonemeCallback = espeak::PhonemeCallback;
using ESpeakUriCallback = espeak::UriCallback;
