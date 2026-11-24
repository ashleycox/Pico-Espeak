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
