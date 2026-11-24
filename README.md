# Pico eSpeak

Add offline speech to an Arduino sketch on a Raspberry Pi Pico. Connect a PWM
audio circuit or an I2S amplifier, then call `say()`. English accents and
eSpeak NG voice variants are included in the sketch upload. The library uses
Arduino-Pico 6.4.0 and eSpeak NG 1.53.0 development sources. [Source revision](NOTICE.md)

## Install

1. Install the [Arduino-Pico board package](https://arduino-pico.readthedocs.io/en/latest/install.html)
   by Earle Philhower. Select your Pico, Pico W, Pico 2 or Pico 2 W in **Tools → Board**.
2. Download this repository as a ZIP using **Code → Download ZIP**. In Arduino
   IDE choose **Sketch → Include Library → Add .ZIP Library** and open it.
3. Open **File → Examples → Pico eSpeak → BasicPico**. On a 2 MB Pico with the
   stock flash layout, use **Tools → Flash Size → 2MB (no FS)**. Upload the sketch.

## Connect audio

| Sketch | Pico GPIO | Connect to |
| --- | --- | --- |
| `BasicPico` | GP15 | Low-pass filter, then an amplifier input |
| `I2SVoiceAndSettings` | GP10 | Bit clock (BCLK) |
| | GP11 | Word select (LRCLK or WS) |
| | GP12 | Data input (DIN) |

Connect the Pico's ground to the audio circuit's ground. Use an amplifier for
a speaker. For I2S, choose a DAC or amplifier that accepts 16-bit I2S at
22.05 kHz with BCLK and LRCLK. The word-select GPIO is always one higher
than BCLK. Both channels carry the same speech.

[Arduino-Pico PWM audio](https://arduino-pico.readthedocs.io/en/latest/pwm.html) ·
[Arduino-Pico I2S audio](https://arduino-pico.readthedocs.io/en/latest/i2s.html)

## Speak from a sketch

```cpp
#include <ESpeakArduino.h>

ESpeakPWMSink audio(15);
// For I2S, use ESpeakI2SSink audio(10, 12);
ESpeakSynth speech;

void setup() {
    Serial.begin(115200);
    if (!speech.begin(audio) ||
        !speech.setVoice("en-us+f2") ||
        !speech.setRate(160) ||
        !speech.say("The battery is fully charged.")) {
        Serial.println(speech.lastErrorText());
    }
}

void loop() {}
```

`say()` waits for speech to finish. If your sketch needs to read buttons or
refresh a display during playback, use `ESpeakBackgroundSynth` on the second
core. The `BackgroundMenu` example changes rate, volume, pitch and voice from
the serial port and can interrupt an announcement.

The [API guide](docs/api.md) covers voice menus, speech settings, custom PCM
sinks, SSML and phoneme output. To speak another language, [build a voice pack](docs/data-packs.md)
with the languages you need.

## License

Pico eSpeak and the bundled eSpeak NG code are licensed under
[GPL-3.0-or-later](LICENSE). See [NOTICE.md](NOTICE.md) for source revisions
and bundled component licenses. [CONTRIBUTING.md](CONTRIBUTING.md) describes
the build and test commands.
