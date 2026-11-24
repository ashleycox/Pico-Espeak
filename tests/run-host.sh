#!/bin/sh
set -eu

cd "$(dirname "$0")/.."
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
flags=
if [ "${SANITIZE:-0}" = 1 ]; then
    flags="-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
fi

for source in src/espeak-ng/core/*.c src/ucd/*.c; do
    "${CC:-cc}" $flags -std=gnu11 -DARDUINO -Isrc -c "$source" -o "$build/$(basename "$source").o"
done

"${CXX:-c++}" $flags -std=c++17 -Isrc tests/mapped_synthesis.cpp \
    src/ESpeakData.cpp src/ESpeakEnglishData.cpp src/espeak-ng/speechPlayer/src/*.cpp \
    "$build"/*.o -lm -o "$build/test"
"$build/test"

python3 - "$build/voice-pack.bin" <<'PY'
import sys
from pathlib import Path
from extras.update_espeak_ng import make_binary_pack, UPSTREAM_REF

files = {"en_dict": b"dic", "lang/gmw/en": b"language en\n", "voices/!v/Mr serious": b"variant"}
Path(sys.argv[1]).write_bytes(make_binary_pack(files, UPSTREAM_REF))
PY
"${CXX:-c++}" $flags -std=c++17 -Isrc tests/flash_pack.cpp src/ESpeakData.cpp -o "$build/flash-pack"
"$build/flash-pack" "$build/voice-pack.bin"

"${CXX:-c++}" $flags -std=c++17 -Itests/arduino_stubs -Isrc tests/wav_sink.cpp src/ESpeakWav.cpp -o "$build/wav-sink"
"$build/wav-sink"

"${CXX:-c++}" $flags -std=c++17 -pthread -Itests/arduino_stubs -Isrc \
    tests/speech_queue.cpp src/ESpeakArduino.cpp src/ESpeakAudio.cpp \
    src/ESpeakData.cpp src/ESpeakEnglishData.cpp src/espeak-ng/speechPlayer/src/*.cpp \
    "$build"/*.o -lm -o "$build/speech-queue"
"$build/speech-queue"
