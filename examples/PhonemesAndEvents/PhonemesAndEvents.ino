// SPDX-License-Identifier: GPL-3.0-or-later
#include <ESpeakArduino.h>

ESpeakSynth speech;

void showEvent(const espeak_EVENT &event, void *)
{
    if (event.type == espeakEVENT_WORD) {
        Serial.printf("Word at text position %d, audio time %d ms\n",
                      event.text_position, event.audio_position);
    }
}

void setup()
{
    Serial.begin(115200);
    const unsigned long start = millis();
    while (!Serial && millis() - start < 3000) {
        delay(10);
    }

    if (!speech.begin()) {
        Serial.println(speech.lastErrorText());
        return;
    }

    speech.onEvent(showEvent);
    Serial.println(speech.textToIpa("Where are you going?"));
    speech.synthesize("Read this <break time=\"200ms\"/> in two parts.", espeakCHARS_UTF8 | espeakSSML);
}

void loop() {}
