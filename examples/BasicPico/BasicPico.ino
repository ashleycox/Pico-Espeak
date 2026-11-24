// SPDX-License-Identifier: GPL-3.0-or-later
#include <ESpeakArduino.h>

ESpeakPWMSink audio(15);  // GP15 to a filtered amplifier input
ESpeakSynth speech;

void setup()
{
    Serial.begin(115200);
    if (!speech.begin(audio)) {
        Serial.println(speech.lastErrorText());
        return;
    }

    if (!speech.say("Hello from the Raspberry Pi Pico.")) {
        Serial.println(speech.lastErrorText());
    }
}

void loop() {}
