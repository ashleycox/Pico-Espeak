# Audio output

The two built-in sinks take mono 16-bit audio from eSpeak. They run from the
same core as the synthesizer and use the sample rate supplied by the engine.

- `ESpeakPWMSink audio(15)` sends PWM to GP15. Use a low-pass filter and an
  amplifier. The pin's PWM slice is reserved while the sink is running.
- `ESpeakI2SSink audio(10, 11, 12)` uses GP10 for BCLK, GP11 for word
  select and GP12 for data out. The clock pins can be in either order as
  long as they are adjacent. Use `ESpeakI2SFormat::LeftJustified` as a fourth
  argument when your DAC expects left-justified 16-bit data.

The default DMA buffering is six buffers of 128 words. For a sketch that
sometimes starves the audio output, try setting a larger buffer before
`speech.begin(audio)`.

```cpp
audio.setBuffering(8, 256);
```

Larger buffers use more RAM and can increase playback latency.
`audio.takeUnderrun()` returns and clears a flag if the driver reported an
underrun during speech. Check it on the playback core. It can help diagnose
buffer starvation, but it gives no count or duration.

## Use a fixed sample rate

Wrap an audio sink in `ESpeakResamplingSink` when a DAC or mixer needs a
fixed sample rate such as 44.1 or 48 kHz. Your downstream sink still
receives signed 16-bit mono PCM. A custom mixer sink can use these samples
alongside music or sound effects.

```cpp
ESpeakI2SSink i2s(10, 11, 12);
ESpeakResamplingSink audio(i2s, 48000);
ESpeakSynth speech;

// In setup():
i2s.setBuffering(8, 256);
if (speech.begin(audio)) {
    speech.say("The output runs at forty-eight kilohertz.");
}
```

`speech.sampleRate()` reports the engine rate and `audio.outputSampleRate()`
reports the rate delivered to the downstream sink. The adapter keeps the
output at the chosen rate when the engine changes its rate. Try
[FixedRateI2S](../examples/FixedRateI2S/FixedRateI2S.ino) with a DAC that
accepts 48 kHz I2S.

## Save speech to WAV

Include `ESpeakWav.h` and give an `ESpeakWavSink` an empty, writable file
from an Arduino-Pico filesystem such as LittleFS. Keep the file open until
`speech.end()` returns. A recording uses one sample rate and can contain
several calls to `say()`. Stopping an utterance removes its partial samples
and keeps the earlier completed speech. `speech.end()` writes the final WAV
header. Check `wav.finalized()` before treating the recording as complete.
Wrap `wav` in `ESpeakResamplingSink` to record at a chosen rate.

```cpp
File output = LittleFS.open("/announcement.wav", "w+");
ESpeakWavSink wav(output);
ESpeakSynth speech;
if (speech.begin(wav)) {
    speech.say("Next stop, Central Station.");
    speech.end();
    if (!wav.finalized()) {
        Serial.println(wav.lastErrorText());
    }
}
output.close();
```

Run the [RecordToWav example](../examples/RecordToWav/RecordToWav.ino) on core
0. Use a flash layout with a filesystem partition and enough sketch space for
your voice pack. On a 2 MB Pico, select **Sketch: 1536KB, FS: 512KB**.
At 22.05 kHz, speech uses about 44 KB per second. LittleFS copies the file
when the WAV header is finalized, so allow about twice the recording's size
in free space. The example preserves existing
files and expects formatted LittleFS.
To initialize a newly reserved, empty partition, set
`initializeBlankPartition` to `true` for the first upload. Leave it
`false` when the partition contains files you want to keep.
