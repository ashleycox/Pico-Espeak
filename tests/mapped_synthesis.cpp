// SPDX-License-Identifier: GPL-3.0-or-later
#include <ESpeakData.h>
#include <espeak-ng/espeak_ng.h>

#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr char dictionaryPath[] = "/pico-espeak/espeak-ng-data/en_dict";

size_t samples;
size_t words;
size_t phonemes;
size_t marks;
bool heardSignal;
bool finished;
bool interruptSpeech;

int collect(short *pcm, int count, espeak_EVENT *events)
{
    if (pcm) {
        samples += count;
        for (int index = 0; index < count; ++index) {
            heardSignal = heardSignal || pcm[index] != 0;
        }
    } else {
        finished = true;
    }
    for (const espeak_EVENT *event = events; event && event->type; ++event) {
        words += event->type == espeakEVENT_WORD;
        phonemes += event->type == espeakEVENT_PHONEME;
        marks += event->type == espeakEVENT_MARK;
    }
    return interruptSpeech && count > 0;
}

espeak_ng_STATUS speak(const char *text, unsigned int flags = espeakCHARS_UTF8)
{
    samples = words = phonemes = marks = 0;
    heardSignal = finished = false;
    return espeak_ng_Synthesize(text, std::strlen(text) + 1, 0, POS_CHARACTER, 0,
                               flags, nullptr, nullptr);
}

void start(const espeak::DataPack &pack)
{
    espeak::detail::useDataPack(&pack);
    espeak_ng_InitializePath(espeak::kBuiltinDataPath);
    assert(espeak_ng_Initialize(nullptr) == ENS_OK);
    assert(espeak_ng_InitializeOutput(ENOUTPUT_MODE_SYNCHRONOUS, 50, nullptr) == ENS_OK);
    assert(espeak_ng_GetSampleRate() == 22050);
    assert(espeak_ng_SetPhonemeEvents(1, 1) == ENS_OK);
    espeak_SetSynthCallback(collect);
}

} // namespace

extern "C" int pico_espeak_file_length(const char *path, int *result);
extern "C" const void *pico_espeak_map_file(const char *path, size_t *size);
extern "C" FILE *pico_espeak_open_file(const char *path, const char *mode);

int main()
{
    const espeak::DataPack &english = espeak::englishDataPack();
    espeak::detail::useDataPack(&english);

    int length = 0;
    assert(pico_espeak_file_length("/pico-espeak/espeak-ng-data/lang/gmw", &length));
    assert(length == -EISDIR);
    assert(pico_espeak_file_length("/pico-espeak/espeak-ng-data/missing", &length));
    assert(length == -ENOENT);
    assert(!pico_espeak_file_length("/pico-espeak-example", &length));

    size_t size = 0;
    const auto *dictionary = static_cast<const uint8_t *>(pico_espeak_map_file(dictionaryPath, &size));
    assert(dictionary && size > 1024);
    FILE *file = pico_espeak_open_file(dictionaryPath, "rb");
    assert(file);
    assert(std::fseek(file, -1, SEEK_END) == 0);
    assert(std::fgetc(file) == dictionary[size - 1]);
    assert(std::fgetc(file) == EOF);
    std::fclose(file);
    assert(pico_espeak_open_file(dictionaryPath, "wb") == nullptr && errno == EROFS);

    for (int run = 0; run < 2; ++run) {
        start(english);
        const espeak_VOICE **voices = espeak_ListVoices(nullptr);
        assert(voices && voices[0] && voices[1]);
        for (const char *voice : {"en", "en-us", "en-us+f2", "en-us+klatt", "en-us+klatt6"}) {
            assert(espeak_ng_SetVoiceByName(voice) == ENS_OK);
            assert(speak("Check the pronunciation of a quiet voice.") == ENS_OK);
            assert(samples > 1000 && heardSignal && finished && words > 1 && phonemes > 1);
        }
        assert(espeak_ng_SetVoiceByName("en-us+missing-variant") == ENS_VOICE_NOT_FOUND);
        assert(espeak_ng_SetVoiceByName("en-us") == ENS_OK);
        assert(espeak_ng_SetParameter(espeakPITCH, 42, 0) == ENS_OK);
        assert(espeak_ng_SetParameter(espeakVOLUME, 70, 0) == ENS_OK);
        assert(espeak_ng_SetParameter(espeakRATE, 200, 0) == ENS_OK);
        assert(espeak_GetParameter(espeakPITCH, 1) == 42);
        assert(espeak_GetParameter(espeakVOLUME, 1) == 70);
        assert(espeak_GetParameter(espeakRATE, 1) == 200);

        const void *text = "English phonemes.";
        const char *ipa = espeak_TextToPhonemes(&text, espeakCHARS_UTF8, espeakPHONEMES_IPA);
        assert(ipa && *ipa);
        assert(speak("<speak>First <mark name=\"point\"/> second.</speak>",
                     espeakCHARS_UTF8 | espeakSSML) == ENS_OK);
        assert(marks == 1 && finished);
        interruptSpeech = true;
        assert(speak("Stop this sentence while it is still being spoken.") == ENS_SPEECH_STOPPED);
        assert(!finished);
        interruptSpeech = false;
        assert(speak("Playback can start again.") == ENS_OK && finished);
        assert(speak("The total is 1,234 dollars and 56 cents.") == ENS_OK && finished);
        std::string longClause;
        for (int i = 0; i < 150; ++i) {
            longClause += "more words ";
        }
        assert(speak(longClause.c_str()) == ENS_OK && finished);
        assert(words == 300);
        assert(espeak_ng_Terminate() == ENS_OK);
    }

    std::vector<espeak::DataFile> entries(english.files, english.files + english.count);
    alignas(4) static const uint8_t wrongVersion[8] = {};
    for (auto &entry : entries) {
        if (std::strcmp(entry.path, "phondata") == 0) {
            entry.data = wrongVersion;
            entry.size = sizeof(wrongVersion);
        }
    }

    const espeak::DataPack badPack{entries.data(), entries.size()};
    espeak::detail::useDataPack(&badPack);
    espeak_ng_InitializePath(espeak::kBuiltinDataPath);
    assert(espeak_ng_Initialize(nullptr) == ENS_VERSION_MISMATCH);
    assert(espeak_ng_Terminate() == ENS_OK);

    espeak::detail::useDataPack(nullptr);
    assert(!pico_espeak_file_length(dictionaryPath, &length));
    std::puts("Mapped flash, voices, PCM, SSML, phonemes, settings, cancellation and reinitialization passed.");
}
