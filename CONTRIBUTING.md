# Contributing

For a bug report, include the board, Arduino-Pico version, flash layout, and
a small sketch that reproduces it. For audio problems, include the output
type, GPIOs, and DAC or amplifier model. For pronunciation problems, include
the text, voice, and language pack.

Clone the repository and install Arduino CLI and the same Arduino-Pico
version as the build workflow.

```sh
index=https://github.com/earlephilhower/arduino-pico/releases/download/global/package_rp2040_index.json
arduino-cli core update-index --additional-urls "$index"
arduino-cli core install rp2040:rp2040@6.4.0 --additional-urls "$index"
arduino-cli compile --fqbn rp2040:rp2040:rpipico --library . examples/BasicPico
tests/run-host.sh
```

The host test exercises the bundled engine and flash data without a board.
Use `SANITIZE=1 tests/run-host.sh` with Clang or GCC to check memory use.
If you change playback, try a short and a long announcement over PWM and
I2S. Check that the final syllables play, then interrupt a long announcement
with the menu sketch and confirm the next one starts.

Changes to vendored eSpeak NG files belong in the updater or its patch.
See [NOTICE.md](NOTICE.md) for the source revision and
[the voice pack guide](docs/data-packs.md) for the generator. Include the
verification commands you ran when you submit a change.
