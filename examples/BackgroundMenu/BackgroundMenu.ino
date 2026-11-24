// SPDX-License-Identifier: GPL-3.0-or-later
#include <ESpeakArduino.h>

ESpeakPWMSink audio(15);
ESpeakSynth engine;
ESpeakBackgroundSynth speech(engine);

// Arduino-Pico otherwise splits one 8 KB stack between the two cores.
bool core1_separate_stack = true;

static const char *voices[] = {"en", "en-us+f2", "en-us+m3"};
static size_t voice = 0;
static ESpeakSettings settings;

void setup()
{
    Serial.begin(115200);
}

void loop()
{
    static bool showedMenu = false;
    if (!speech.isRunning()) {
        static bool showedError = false;
        if (Serial && !showedError && speech.lastStatus() != ENS_OK) {
            showedError = true;
            Serial.println(speech.lastErrorText());
        }
        delay(10);
        return;
    }

    if (Serial && !showedMenu) {
        showedMenu = true;
        Serial.println("R/r: rate up/down | V/v: volume up/down | P/p: pitch up/down");
        Serial.println("N: next voice | Space: speak | X: stop");
    }
    if (!Serial.available()) {
        return;
    }

    ESpeakSettings next = settings;
    switch (Serial.read()) {
        case 'R': next.rate = min(next.rate + 10, 400); break;
        case 'r': next.rate = max(next.rate - 10, 80); break;
        case 'V': next.volume = min(next.volume + 10, 200); break;
        case 'v': next.volume = max(next.volume - 10, 0); break;
        case 'P': next.pitch = min(next.pitch + 5, 100); break;
        case 'p': next.pitch = max(next.pitch - 5, 0); break;
        case 'N':
        case 'n': {
            const size_t nextVoice = (voice + 1) % (sizeof(voices) / sizeof(voices[0]));
            if (speech.setVoice(voices[nextVoice])) {
                voice = nextVoice;
                Serial.println(voices[voice]);
            }
            return;
        }
        case ' ':
            speech.speakNow("You can still use the menu while this sentence is playing.");
            return;
        case 'X':
        case 'x':
            speech.stop();
            return;
        default:
            return;
    }

    if (speech.applySettings(next)) {
        settings = next;
        Serial.printf("Rate %d, volume %d, pitch %d\n", settings.rate, settings.volume, settings.pitch);
    }
}

void setup1()
{
    speech.begin(audio);
}

void loop1()
{
    speech.loop();
}
