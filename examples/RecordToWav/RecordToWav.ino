// SPDX-License-Identifier: GPL-3.0-or-later
#include <LittleFS.h>
#include <ESpeakArduino.h>
#include <ESpeakWav.h>

// Set true to initialize a new LittleFS partition. This can erase its contents.
constexpr bool initializeBlankPartition = false;

void setup()
{
    Serial.begin(115200);
    const unsigned long started = millis();
    while (!Serial && millis() - started < 4000) {
        delay(10);
    }

    LittleFSConfig filesystem;
    filesystem.setAutoFormat(initializeBlankPartition);
    LittleFS.setConfig(filesystem);
    if (!LittleFS.begin()) {
        Serial.println("Mount or initialize LittleFS before recording. See the WAV example guide.");
        return;
    }

    // Use a new filename to keep any existing recording.
    if (LittleFS.exists("/speech.wav")) {
        Serial.println("/speech.wav already exists.");
        return;
    }
    File file = LittleFS.open("/speech.wav", "w+");
    ESpeakWavSink wav(file);
    ESpeakSynth speech;
    if (!speech.begin(wav) ||
        !speech.say("Save this announcement to play again later.")) {
        Serial.println(speech.lastErrorText());
        return;
    }

    speech.end();
    file.close();
    if (!wav.finalized()) {
        Serial.println(wav.lastErrorText());
        return;
    }
    Serial.printf("Saved /speech.wav with %lu samples.\n",
                  static_cast<unsigned long>(wav.samplesRecorded()));
}

void loop() {}
