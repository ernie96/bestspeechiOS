# BestSpeech for Android

An Android text-to-speech engine that runs the 1995 **BestSpeech / Keynote Gold**
`B32_TTS.DLL` — a 32-bit Windows formant synthesizer — inside the
[Unicorn](https://www.unicorn-engine.org/) CPU emulator. All 14 voices work, at
around 177x real time on a phone.

No Windows API is executed natively. Every one of the DLL's imports is serviced by
`c/b32emu.c`, which is what makes it portable to arm64 at all.

Ships for `arm64-v8a`, `armeabi-v7a`, `x86_64` and `x86`. Confirmed working on a
Galaxy S23+ (Android 16, arm64-v8a) as the system TTS engine. Audio produced through Android's `TextToSpeech` API is **byte-identical**
to the same engine running on desktop x86_64.

Groundwork, the voice tables, and the `bstRelBuf` insight come from
[samtupy/b32tts_wrapper](https://github.com/samtupy/b32tts_wrapper) (and
`@rommix0`'s `bst.h`), which solves the same capture problem on Windows using
MinHook.

## Layout

| Path | Purpose |
| --- | --- |
| `c/b32emu.{c,h}` | The engine: PE loader, 55 Win32/WinMM shims, trampolines |
| `c/b32jni.c` | JNI bridge, streaming PCM to Java as it is produced |
| `c/sonic/` | [sonic](https://github.com/waywardgeek/sonic) time-stretch (Apache 2.0, Bill Cox) |
| `c/build-android.sh` | Builds libunicorn and `libb32tts.so` per ABI |
| `android/src/.../B32Native.java` | Native bindings and the audio `Sink` |
| `android/src/.../B32TtsService.java` | The `TextToSpeechService` |
| `android/src/.../B32SettingsActivity.java` | Engine settings, including DLL import |
| `android/src/.../CheckVoiceDataActivity.java` | Answers `CHECK_TTS_DATA` |
| `android/build-apk.sh` | Signed APK with no Gradle and no network |
| `third_party/shims/` | `pkg-config`/`strings` stubs qemu's configure needs |

`B32_TTS.DLL` is proprietary (Berkeley Speech Technologies) and **not
redistributable**, so it is neither in this repo nor in the APK. Users import
their own copy from the settings screen.

## Building

Needs the Android NDK, the SDK's cmake/ninja/build-tools, and a JDK — all of which
the Android command-line tools provide. Clone Unicorn first:

```
git clone --depth 1 --branch 2.1.3 https://github.com/unicorn-engine/unicorn.git third_party/unicorn

# One libunicorn + libb32tts.so per ABI, then one APK containing all of them.
c/build-android.sh   arm64-v8a armeabi-v7a x86_64 x86
android/build-apk.sh arm64-v8a armeabi-v7a x86_64 x86
adb install -r android/build/bestspeech-debug.apk
```

Either script takes a subset if you only need one ABI, but ship all four: Android
reports an APK as "incompatible with your device" when it contains native code and
none of it matches the device, so an arm64-only build simply refuses to install on
32-bit ARM phones, Chromebooks, and x86 emulators.

Libraries are linked with `-Wl,-z,max-page-size=16384`, since Android 15+ runs on
devices with 16 KB memory pages where a 4 KB-aligned library will not load.

Only the i386 guest target is compiled into libunicorn, which is all this needs
and keeps it to roughly a quarter of the full library.

Three things about the Unicorn build are worth knowing, because none of them are
obvious from the error messages:

- qemu's `configure` calls `error_exit` when no `pkg-config` binary exists, and
  mis-detects endianness without `strings`. Windows has neither, and the result is
  that `config-host.h` is never generated and every source file fails on a missing
  include. `third_party/shims/` supplies both.
- `UNICORN_LEGACY_STATIC_ARCHIVE` must be **off**: bundling the all-in-one archive
  ends in a `create_symlink` that fails with "A required privilege is not held".
  The three archives are linked separately instead.
- The NDK's `clang` reports its target as `x86_64-w64-windows-gnu` but ships no
  host sysroot, so it cannot build host binaries. It is a cross-compiler only.

## Using it

Install the APK, then open **Settings → Accessibility → Text-to-speech output**,
pick BestSpeech, and tap the gear beside it. Use **Choose B32_TTS.DLL file…** to
import your copy of the engine; it is copied into the app's private storage, so
the original can be moved or deleted afterwards. The picker accepts the file from
anywhere — Downloads, Drive, USB — and rejects anything that is not a 32-bit x86
DLL exporting `bstCreate`.

The same screen sets the default voice and everything else the engine exposes —
see below. Speech rate is the system slider, since Android sends it with every
request.

All 14 voices appear individually in Android's voice picker: Fred, Sara, Hary,
Wendy, Dexter, Alien, Kit, Bruno, Ghost, Peeper, Dracula, Granny, Martha, Tim.

## The parameter set

Every parameter is delivered as a text code on each utterance, so all of them
work on every utterance rather than only the first — see `BST_RATE_SETTING`
below for why that matters.

| Setting | Code | Range | Notes |
| --- | --- | --- | --- |
| Pitch | `~f` | 43–600 Hz | as a percentage of the voice's own baseline |
| Volume | — | -70..20 dB | `bstSetParams(BST_GAIN_SETTING)`; this one does keep working |
| Inflection | `~h` | -300..100 | pitch range; saturates below about -150 |
| Head size | `~v` | 0..6 | 0 and 1 are the same |
| Excitation | `~e` | 1..6 | 1 breathy, 2 whispery, 3 normal |
| Unvoiced volume | `~u` | -70..20 dB | turned down, the speaker sounds blocked up |
| Phrase prediction | `~~2` | on/off | look ahead for punctuation before setting the phrase contour |
| Expand abbreviations | `~n10` | on/off | Dr. → doctor |
| Times of day | `~n9` | on/off | 8:00 → eight o'clock |
| Numbers in full | `~n6` | on/off | 1995 → one nine nine five |
| Digits individually | `~n2` | on/off | |
| Capital groups as words | `~n7` | on/off | NASA as a word |
| Spell every letter | `~n1` | on/off | |
| Speak punctuation | `~n3` | on/off | |
| Speak spaces and line breaks | `~n4` | on/off | |

The `~n` options are all **off** by default in this DLL revision, whatever the
Keynote GOLD manual says about `~n9` and `~n10`, and each one above was confirmed
to change the audio. `~n5` (mathematics) and `~n8` (control characters) are
documented but do nothing here, so they are not offered. Since these are sticky
engine state, each is stated outright on every utterance — omitting one would
leave a previous setting in force instead of turning it off.

The user pronunciation dictionary (`~x]` dictionary-entry mode) is not
implemented.

**`~f` silently discards a `~h` set before it.** The voice tables everyone
inherited from `@rommix0`'s `bst.h` list the codes in the order
`~v ~e ~h ~u ~f`, which means every voice's own pitch range has been thrown away
— in `b32tts_wrapper` and BeSTspeak too, both of which append `~f` after the
prefix. Emitting `~h` after `~f` is the whole of the fix, and it is why voices
other than Fred, Dexter and Peeper (whose `~h` is 0) sound slightly more
expressive or flatter here than in 0.2. Forcing `Inflection` to 0 reproduces the
old output byte for byte.

## Pauses

The engine pauses about **420 ms** at a sentence, 180 ms at a comma, and appends
**477 ms of exact digital silence** to the end of every utterance. At
screen-reader speeds that trailing silence is dead air between one utterance and
the next, so it is trimmed to 60 ms by default; the *Pauses* setting additionally
caps the pauses inside an utterance (300/150/60 ms), and *Authentic* turns all
trimming off.

Trimming happens on the engine's own stream, ahead of sonic, so the caps are in
engine time and shrink further with the rate multiplier. Silence is detected at
-42 dBFS, comfortably above the handful of stray samples under 200 that the
engine's pauses contain and far below anything audible as speech.

Text is also mapped to printable ASCII before synthesis, with curly quotes, en
and em dashes and ellipses converted rather than blanked — otherwise iOS's curly
apostrophe turns "don't" into "don t", which the engine reads as two words. `~`
is dropped from the text, since it is the engine's own lead-in character and
would otherwise let text switch the parser into another mode mid-utterance.

## Two things that cost real time

**`TtsWav` never returns unless you pump messages.** The engine runs its own
message loop and dispatches to a window proc. If `bstRelBuf` isn't called once per
`waveOutWrite` *from that loop*, synthesis hangs forever. Calling it from the host
would re-enter the engine from inside its own `waveOutWrite`, so the fix is a few
bytes of generated x86 that run inside the guest:

```
; wndproc for our private WM_REL_BUF
push [G_TTS]     ; engine handle
call [G_RELBUF]  ; bstRelBuf, __cdecl
add  esp, 4
ret  16          ; wndproc convention
```

Because `waveOutWrite` queues that message *before* checking the abort flag,
stopping mid-utterance still unwinds cleanly and engine state fully recovers.

**`BST_RATE_SETTING` only works once.** `bstSetParams(BST_RATE_SETTING, …)` affects
only the first `TtsWav` call after `bstCreate`. Every utterance after it reverts to
normal speed no matter how often the parameter is re-set. A one-shot CLI never
notices; a TTS service that holds one engine for its lifetime speaks the first
phrase at the right rate and everything after it at 1x. Rate is therefore
delivered as the engine's `~r` text code, which is honoured every time — see
`b32_set_rate`.

## Speed, and its ceiling

Both of the engine's rate controls — the parameter and the `~r` code — saturate at
the same floor, about **2.75x** normal. Measured, relative to rate 0:

| rate | -200 | -100 | -50 | -25 | 0 | 25 | 50 | 80 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| speed | 0.36x | 0.53x | 0.70x | 0.81x | 1.00x | 1.28x | 1.75x | 2.75x |

Screen-reader users routinely run 400–500%, which is past what the engine can do
at all, so `B32TtsService` interpolates that table for the engine and hands the
leftover factor to sonic, which time-stretches without changing pitch. The result
tracks the request closely across the whole range:

| requested | 50% | 100% | 200% | 300% | 400% | 480% | 600% |
| --- | --- | --- | --- | --- | --- | --- | --- |
| delivered | 0.50x | 1.00x | 2.00x | 3.01x | 4.02x | 4.83x | 6.07x |

Note that `onStop()` is bounded rather than instant: aborting stops capture
immediately, but the engine still finishes synthesizing the utterance. At ~177x
real time a 30 second utterance costs about 170 ms, which is cheaper than the risk
of tearing the emulator down mid-call.

## Getting a file onto a device, if you ever do it by hand

The settings screen import is the supported path. If you bypass it, note that
`adb push` into `/sdcard/Android/data/<pkg>/files/` **silently reports success and
leaves no file** on Android 11+. Push to `/data/local/tmp` and `cp` from there,
then `chmod 666` — the copy lands owned by `shell` with mode 660, which the app
cannot read.

And never retrieve a WAV with `adb shell cat > file`: the pty inserts `\r` before
every `\n`, and since the RIFF header still declares the original length, tools
read a shifted buffer and report plausible-looking garbage — correct duration,
zero correlation, inflated RMS. Use `adb pull`.

## Licensing

The combined work is **GPL-2.0**, because it statically links
[Unicorn](https://github.com/unicorn-engine/unicorn), which is GPL-2.0. `LICENSE`
holds that text, and this repository is the corresponding source.

Per component:

| Component | Licence |
| --- | --- |
| Everything under `c/` and `android/` written for this project | Public domain |
| `c/sonic/` | Apache-2.0, Copyright 2010 Bill Cox — see `c/sonic/LICENSE-APACHE-2.0.txt` |
| Unicorn (linked, not vendored) | GPL-2.0 |
| `B32_TTS.DLL` | Proprietary, Berkeley Speech Technologies — **not** included |

**A caveat worth knowing before redistributing the binary:** Apache-2.0 and
GPL-2.0 are generally held to be incompatible, because Apache's patent-termination
clause adds a restriction GPL-2.0 does not permit. `libb32tts.so` statically links
both sonic and Unicorn, so the prebuilt APK sits on that conflict. The upstream
`b32tts_wrapper` does not have this problem: it bundles sonic but links MinHook
(MIT) rather than Unicorn. If this matters for your use, replacing sonic with a
GPL-compatible or public-domain time-stretcher removes the conflict entirely — it
is used only for the speed multiplier past the engine's 2.75x ceiling.

## Still open

- **A user pronunciation dictionary.** The engine has one (`~x]` puts it in
  dictionary-entry mode, associating a spelling with a phoneme string in RAM),
  and nothing here uses it.
- **Licensing** caps this at "bring your own DLL" regardless of how well it works.
- **Sample-exact equivalence with native Windows** is unverified. This engine
  agreed byte-for-byte with a Python reference implementation and across
  architectures, but nothing here has been diffed against `b32tts_wrapper` running
  on real Windows.
- The Python reference harness and the parity, validation, and JNI-signature test
  suites were removed from this tree once the engine was working. They are worth
  restoring if the shims are ever modified.
