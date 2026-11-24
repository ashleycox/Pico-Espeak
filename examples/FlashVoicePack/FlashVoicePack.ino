// SPDX-License-Identifier: GPL-3.0-or-later
#include <ESpeakArduino.h>

espeak::FlashDataPack voices;
ESpeakPWMSink audio(15);
ESpeakSynth speech;

void setup()
{
    Serial.begin(115200);
    const unsigned long started = millis();
    while (!Serial && millis() - started < 4000) {
        delay(10);
    }

    Serial.printf("Voice pack address: 0x%08lx\n",
                  static_cast<unsigned long>(voices.reservedFlashAddress()));
    Serial.printf("Voice pack capacity: %lu bytes\n",
                  static_cast<unsigned long>(voices.reservedFlashCapacity()));

    if (!voices.openReservedFlash()) {
        Serial.println(voices.lastErrorText());
        return;
    }

    ESpeakConfig config(voices.pack());
    config.requiredLanguage = "en";
    if (!speech.begin(audio, config) ||
        !speech.say("This voice pack is stored separately from the sketch.")) {
        Serial.println(speech.lastErrorText());
    }
}

void loop() {}
