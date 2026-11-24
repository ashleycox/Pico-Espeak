# API guide

Include `ESpeakArduino.h`. Initialize one `ESpeakSynth` with an
`ESpeakPWMSink(pin)`, `ESpeakI2SSink(bitClock, dataOut)` or your own
`espeak::AudioSink`. Declare the sink before the synthesizer so it lives
through the synthesizer's teardown. Call `begin()` without a sink when you
only want phonemes or events. Check the return value of `begin()`, the
speech calls and the settings setters. `lastErrorText()` describes a failure.

## Use a menu to change the voice

| Setting | Setter | Units |
| --- | --- | --- |
| Rate | `setRate()` | 80–450 words per minute |
| Volume | `setVolume()` | 0 for silence, 100 for normal volume, up to 200 |
| Pitch | `setPitch()` | 0–100, with 50 as the default |
| Pitch range | `setRange()` | 0–100, with 0 for a monotone |
| Word gap | `setWordGap()` | Units of 10 ms |

Read the current values with `settings()`, edit a copy of `ESpeakSettings`
and send it back with `applySettings()`. `setParameter()` also exposes the
engine's parameter IDs for finer control.

Use `setVoice("en-us")` for US English, `setVoice("en")` for British
English, or `setVoice("en-us+f2")` for an eSpeak variant. `setLanguage()`
selects by language code. `listVoices()` returns a null-terminated array of
`espeak_VOICE` pointers. Use `name` for a menu label and `identifier` with
`setVoice()`. Copy any strings or entries you keep past a later voice-list
call or `end()`. Use `listVariants()` for the variant menu and pass its
`identifier` to `setVariant()`. Add more languages or variants to the
sketch's [voice pack](data-packs.md).

## Keep the user interface responsive

`ESpeakSynth::say()` plays speech synchronously. In a multicore sketch,
declare `bool core1_separate_stack = true;` to give each core a full stack,
even if synthesis stays on core 0. For background speech, call
`ESpeakBackgroundSynth::begin()` from `setup1()` and its `loop()` from
`loop1()`. Submit text and settings from `loop()`. Queue methods
return whether a request was accepted. Use the background object's
`lastErrorText()` to inspect the result of the most recent operation.

Use `say()` on the background object to queue an announcement, `speakNow()`
to interrupt playback and replace waiting announcements, or `stop()` to
cancel. Keep access to the underlying engine, its callbacks and the audio
hardware on the playback core. Keep the sink, configuration and voice data
alive until that core finishes shutdown. After calling `end()`, keep running
the worker loop until `isStopped()` returns true. See [the menu sketch](../examples/BackgroundMenu/BackgroundMenu.ino)
and [Arduino-Pico's multicore guide](https://arduino-pico.readthedocs.io/en/latest/multicore.html).

For a menu that needs to track specific announcements, call `submit(text)` or
`submitNow(text)`. Both return a ticket, or zero if the queue rejects the
request. Call `result(ticket)` from the menu to read its state and native
eSpeak status. `cancel(ticket)` targets one queued or active announcement.
The worker retains the eight most recent ticketed results. Older tickets
return `Unknown`.

```cpp
ESpeakSpeechTicket announcement = speech.submit("Battery low.");
// In loop(), after a successful submission:
const auto result = speech.result(announcement);
if (result.finished()) {
    Serial.println(ESpeakSynth::statusCodeMessage(result.status));
}
```

`setMaxTextBytes(256)` caps the size of future queued text and voice names
before the library copies them. The limit counts bytes, so a UTF-8 code point
can count as more than one. Zero restores the default of unlimited text size.

## Read text in chunks

Use `sayStream(stream, byteCount)` for a finite Arduino `Stream`, such as an
open LittleFS or SD file. Pass the number of text bytes remaining from the
current position. For other sources, pass an `ESpeakTextReader` to
`sayStream(reader, context)`. The reader fills the supplied buffer and returns
the number of bytes read, zero for end of input, or a negative value for an
I/O error.

```cpp
File passage = LittleFS.open("/passage.txt", "r");
if (passage && !speech.sayStream(passage, passage.size())) {
    Serial.println(speech.lastErrorText());
}
```

`sayStream()` reads plain UTF-8 text in chunks of up to 512 bytes and blocks
the playback core until speech finishes. It prefers sentence and word breaks,
preserves complete UTF-8 characters and accepts an optional UTF-8 byte order
mark. Event positions are relative to the current chunk. Use `synthesize()`
for SSML.

Another core can call `requestStop()` to interrupt a streamed passage.
Custom readers should return promptly so the synthesizer can check for
cancellation. A generic Arduino stream uses its configured timeout while
waiting for more bytes. The file overload reports early end of input
immediately.

To try a file, open [ReadTextFile](../examples/ReadTextFile/ReadTextFile.ino).
On a 2 MB Pico, select **Sketch: 1536KB, FS: 512KB**. Upload the example's
`data` folder to a spare or backed-up LittleFS partition, then upload the
sketch. [Arduino-Pico filesystem uploader](https://arduino-pico.readthedocs.io/en/latest/fs.html#uploading-files-to-the-littlefs-file-system-on-ide-2-x-rp2040-and-rp2350)

## Work with speech data

- `synthesize(text, espeakCHARS_UTF8 | espeakSSML)` handles SSML. Include
  `espeakPHONEMES` to accept eSpeak phoneme input. Use `synthesizeMark()`
  to start at a named SSML mark. For `synthesizeText()` and
  `synthesizeMarkText()`, include the string terminator in the buffer size.
  Register `onUri()` to handle SSML audio references in your sketch.
- `textToIpa()` and `textToPhonemes()` return the pronunciation using the
  current voice. `onEvent()` reports word, sentence, mark and phoneme
  positions during synthesis. Event data is valid for the duration of the call.
- `AudioSink::writeSamples()` receives signed 16-bit mono PCM at
  `sampleRate()`. Return the number of samples accepted. The engine checks
  short writes. A custom sink can drain pending audio in `flush()` and drop
  it on interruption in `discard()`. To support interruption during a long
  drain, override `flushUntil()` and co-operate with its stop check.
  `lastErrorText()` can provide the specific cause of a sink failure.
  Override `changeSampleRate()` if a sink can switch rates in place. The
  default ends the sink and opens it again. See the [audio guide](audio.md)
  for fixed-rate PCM, WAV output and DMA buffering.

The C APIs in [`speak_lib.h`](../src/espeak-ng/speak_lib.h) and
[`espeak_ng.h`](../src/espeak-ng/espeak_ng.h) expose the bundled engine.
The engine uses shared global state. Keep its C calls on the same core as
the owning synthesizer, outside its callbacks. Initialize and tear down
through one API, and leave the wrapper's callbacks installed while using it.

This build includes eSpeak's formant synthesis, Klatt and SpeechPlayer
variants, and Sonic rate processing. Desktop audio drivers, eSpeak's desktop
worker thread and the external MBROLA runtime are outside this port.
Use the host-side [voice pack generator](data-packs.md) to compile language data.
