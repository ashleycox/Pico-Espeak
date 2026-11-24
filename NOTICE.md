# Third-party code and data

Pico eSpeak includes eSpeak NG 1.53.0 development sources and speech data from commit `ee9fa7cc4704d683020526f47a9655f14c8d5f4c`. The engine and data use GPL-3.0-or-later. The source includes the language, dictionary and phoneme files used to build the speech data and preserves the original copyright statements. [eSpeak NG source](https://github.com/espeak-ng/espeak-ng/tree/ee9fa7cc4704d683020526f47a9655f14c8d5f4c)

The files `COPYING.APACHE`, `COPYING.BSD2` and `COPYING.UCD` contain additional notices distributed with eSpeak NG. The embedded port changes are in `extras/espeak-ng.patch`.

NV Speech Player is included with eSpeak NG and provides an alternative waveform generator for some Klatt voices. It is copyright 2014 NV Access Limited and uses GPL-3.0-or-later. [NV Speech Player source](https://github.com/espeak-ng/espeak-ng/tree/ee9fa7cc4704d683020526f47a9655f14c8d5f4c/src/speechPlayer)

Sonic handles speech speed changes. It is copyright Bill Cox and uses the Apache License 2.0, included in `COPYING.SONIC`. [Sonic source](https://github.com/waywardgeek/sonic/tree/fbf75c3d6d846bad3bb3d456cbc5d07d9fd8c104)
