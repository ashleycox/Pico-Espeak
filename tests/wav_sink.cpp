// SPDX-License-Identifier: GPL-3.0-or-later
#include <ESpeakWav.h>

#include <cassert>
#include <cstring>

int main()
{
    fs::File file;
    ESpeakWavSink wav(file);
    const int16_t samples[] = {0, 32767, -32768, 1234, -15};
    assert(wav.begin(22050));
    assert(file.size() == 44);
    assert(std::memcmp(file.bytes.data(), "RIFF", 4) == 0);
    assert(std::memcmp(file.bytes.data() + 8, "WAVE", 4) == 0);
    assert(file.bytes[24] == 0x22 && file.bytes[25] == 0x56);

    assert(wav.writeSamples(samples, 5) == 5);
    wav.flush();
    const auto completed = file.bytes;
    assert(wav.samplesRecorded() == 5 && file.size() == 54);
    assert(!wav.finalized() && file.bytes[4] == 36 && file.bytes[40] == 0);
    assert(file.bytes[46] == 0xff && file.bytes[47] == 0x7f);
    assert(file.bytes[48] == 0x00 && file.bytes[49] == 0x80);

    assert(wav.writeSamples(samples, 3) == 3);
    assert(wav.discard());
    assert(file.bytes == completed && wav.ready());

    assert(wav.writeSamples(samples, 1) == 1);
    wav.flush();
    assert(file.size() == 56 && wav.samplesRecorded() == 6);
    const auto saved = file.bytes;

    file.writeLimit = 1; // A partial PCM sample must not corrupt completed speech.
    assert(wav.writeSamples(samples, 1) == 0);
    assert(!wav.ready() && wav.lastErrorText());
    file.writeLimit = SIZE_MAX;
    assert(wav.discard());
    assert(file.bytes == saved);
    wav.end();
    assert(wav.finalized());
    assert(file.size() == 56 && file.bytes[4] == 48 && file.bytes[40] == 12);
    assert(std::memcmp(file.bytes.data() + 44, saved.data() + 44, 12) == 0);
    const auto finalized = file.bytes;
    assert(!wav.begin(44100)); // The caller must supply an empty file.
    assert(file.bytes == finalized);

    fs::File fixedRateFile;
    ESpeakWavSink fixedRateWav(fixedRateFile);
    ESpeakResamplingSink fixedRate(fixedRateWav, 48000);
    assert(fixedRate.begin(24000));
    assert(fixedRate.writeSamples(samples, 5) == 5);
    fixedRate.flush();
    assert(fixedRateWav.samplesRecorded() == 10);

    assert(fixedRate.changeSampleRate(16000));
    assert(fixedRate.writeSamples(samples, 5) == 5);
    fixedRate.flush();
    fixedRate.end();
    assert(fixedRateWav.finalized() && fixedRateWav.samplesRecorded() == 25);
    assert(fixedRateFile.size() == 94 && fixedRateFile.bytes[40] == 50);
    assert(fixedRateFile.bytes[24] == 0x80 && fixedRateFile.bytes[25] == 0xbb);
}
