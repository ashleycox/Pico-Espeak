// SPDX-License-Identifier: GPL-3.0-or-later
#include <ESpeakArduino.h>

// Connect GP10 to BCLK, GP11 to LRCLK and GP12 to DIN on an I2S amplifier.
ESpeakI2SSink audio(10, 12);
ESpeakSynth speech;

void setup()
{
    Serial.begin(115200);

    ESpeakSettings settings;
    settings.rate = 150;
    settings.pitch = 45;
    settings.volume = 80;

    if (!speech.begin(audio) ||
        !speech.setVoice("en-us+f2") ||
        !speech.applySettings(settings) ||
        !speech.say("These settings can be changed while your sketch is running.")) {
        Serial.println(speech.lastErrorText());
    }
}

void loop() {}
