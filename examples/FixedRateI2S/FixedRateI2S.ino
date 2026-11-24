// SPDX-License-Identifier: GPL-3.0-or-later
#include <ESpeakArduino.h>

// Connect GP10 to BCLK, GP11 to LRCLK and GP12 to DIN.
ESpeakI2SSink i2s(10, 11, 12);
ESpeakResamplingSink audio(i2s, 48000);
ESpeakSynth speech;

void setup()
{
    Serial.begin(115200);
    i2s.setBuffering(8, 256);
    if (!speech.begin(audio) ||
        !speech.say("The audio output is running at forty-eight kilohertz.")) {
        Serial.println(speech.lastErrorText());
    }
}

void loop() {}
