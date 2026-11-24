# Voice packs

The English pack is linked into the sketch. The engine reads the compiled
phoneme tables and dictionaries directly from flash, which keeps them out of
the Pico's limited RAM. You can use the same approach for other languages.

## Use a different language

Install Python 3.9 or later, Git, CMake and a C compiler on your computer. Run this
command from the library checkout and replace the output directory with your
sketch folder. The generator fetches the matching eSpeak NG source and builds
the pronunciation data on your computer.

```sh
python3 extras/update_espeak_ng.py --languages fr,de --voices f2,m3 \
    --embedded-source /path/to/sketch/ESpeakDataPack.cpp
```

The command writes `ESpeakDataPack.cpp` and `ESpeakDataPack.h`. Keep them
beside the `.ino` file. Pass the pack to `ESpeakConfig` and set an initial
language from the pack.

```cpp
#include <ESpeakArduino.h>
#include "ESpeakDataPack.h"

// In setup(), with your speech and audio objects already declared:
ESpeakConfig config(speechDataPack());
config.requiredLanguage = "fr";
if (speech.begin(audio, config)) {
    speech.say("Bonjour, le Pico est prêt.");
}
```

Use that configuration for each `begin()` call. If the sketch only uses a
custom pack, the linker can leave the built-in English pack out. Include
`en` in the language list if the sketch needs both English and another language.

The generator prints the size of the pack before compilation. The Arduino
build reports the total flash use for your selected board and partition.
Start with a few languages and variants on a 2 MB Pico. The complete eSpeak NG
data set is intended for systems with more storage.

## Rebuild the library's English pack

After updating the pinned revision and port patch, regenerate the library's
engine and its English pack together.

```sh
python3 extras/update_espeak_ng.py --update-core --preset english \
    --include ESpeakData.h --pack-function espeak::englishDataPack \
    --embedded-source src/ESpeakEnglishData.cpp
```

The host tools accept `--ref` when evaluating a particular eSpeak NG revision.
Use the same source revision for the engine and compiled data.

## Keep the voice pack separate from the sketch

The [FlashVoicePack example](../examples/FlashVoicePack/FlashVoicePack.ino)
uses a binary voice pack in a dedicated flash partition. Flashing a new voice
pack at that address leaves the sketch in place. `FlashDataPack` verifies the
pack's revision, length and checksum before eSpeak uses it.

1. Select a flash layout with a 1 MB or larger filesystem partition. This
   example uses that partition exclusively for speech data. Use a board or
   partition whose contents you can replace.
2. Upload `FlashVoicePack` and read the voice pack address and capacity from
   the Serial Monitor. These depend on the flash layout.
3. Generate a binary pack with the language selected in the sketch. For the
   English example and a 1 MB partition, run the command below.
4. Reboot the Pico into BOOTSEL mode. In the `picotool` command, replace
   `ADDRESS` with the exact hex address reported by the example. Reset the
   board when the verified write finishes.

```sh
python3 extras/update_espeak_ng.py --preset minimal-en \
    --format binary --max-data-bytes 1048576 --data-out voice-pack.bin
picotool load --no-overwrite --verify voice-pack.bin -t bin -o ADDRESS
```

Use the reported capacity as the byte limit for other flash layouts. Recheck
the address if you change the board or partition size. To consume a binary
pack from your own memory-mapped storage, call
`FlashDataPack::open(pointer, capacity)` on a 4-byte-aligned image. Keep the
image and the `FlashDataPack` object alive until speech ends.

## Find dialects and fit a smaller pack

- `--list languages` and `--list voices` show language files and variants in
  the eSpeak NG source. `--list presets` lists the preset combinations.
- `--languages en,pt` includes available dialects. To choose specific ones,
  use `--dialects en-US,pt-BR` with `--voices none` or your preferred
  variant list.
- `--compact` leaves out eSpeak NG's optional extra Russian, Mandarin and
  Cantonese dictionary entries. Use it when you need those languages on a
  board with less flash.

Once the source and requested revision are cached, add `--offline` to build
or list voices without fetching them again.
