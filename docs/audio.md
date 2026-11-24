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

## Save speech to WAV

Include `ESpeakWav.h` and give an `ESpeakWavSink` an empty, writable file
from an Arduino-Pico filesystem such as LittleFS. Keep the file open until
`speech.end()` returns. A recording uses one sample rate and can contain
several calls to `say()`. Each successful utterance updates the WAV header.
Stopping an utterance removes its partial samples and keeps the earlier
completed speech.

```cpp
File output = LittleFS.open("/announcement.wav", "w+");
ESpeakWavSink wav(output);
ESpeakSynth speech;
if (speech.begin(wav)) {
    speech.say("Next stop, Central Station.");
    speech.end();
}
output.close();
```

Run the [RecordToWav example](../examples/RecordToWav/RecordToWav.ino) on core
0. Use a flash layout with a filesystem partition and enough sketch space for
your voice pack. For a 2 MB Pico, the example fits in **Sketch: 1792KB, FS:
256KB**. The example preserves existing files and expects formatted LittleFS.
To initialize a newly reserved, empty partition, set
`initializeBlankPartition` to `true` for the first upload. Leave it
`false` when the partition contains files you want to keep.
