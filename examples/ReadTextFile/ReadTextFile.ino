// SPDX-License-Identifier: GPL-3.0-or-later
#include <LittleFS.h>
#include <ESpeakArduino.h>

ESpeakPWMSink audio(15);
ESpeakSynth speech;

void setup()
{
    Serial.begin(115200);
    const unsigned long started = millis();
    while (!Serial && millis() - started < 4000) {
        delay(10);
    }

    LittleFSConfig filesystem;
    filesystem.setAutoFormat(false);
    LittleFS.setConfig(filesystem);
    if (!LittleFS.begin()) {
        Serial.println("Upload the example data folder to LittleFS first.");
        return;
    }

    File passage = LittleFS.open("/passage.txt", "r");
    if (!passage || passage.isDirectory()) {
        Serial.println("Missing /passage.txt. Upload the example data folder.");
        return;
    }

    if (!speech.begin(audio) || !speech.sayStream(passage, passage.size())) {
        Serial.println(speech.lastErrorText());
    }
    passage.close();
    speech.end();
}

void loop() {}
