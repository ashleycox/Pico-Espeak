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
