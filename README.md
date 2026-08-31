# Pico SAPI5

A SAPI 5 speech engine for Windows built on the [SVOX Pico](https://android.googlesource.com/platform/external/svox/)
synthesizer, so its six voices can be used by any Windows application that speaks
— screen readers, Word, Balabolka, anything that lists SAPI 5 voices.

Pico is small, fast and very intelligible, and until now on Windows it was
reachable only from NVDA, through the
[PicoTTS-NVDA](https://github.com/alexoloopios/picotts-nvda) add-on. This makes
the same engine available everywhere else.

| Voice | Language | Lingware |
| --- | --- | --- |
| Pico American English | en-US | `en-US_ta.bin` + `en-US_lh0_sg.bin` |
| Pico British English | en-GB | `en-GB_ta.bin` + `en-GB_kh0_sg.bin` |
| Pico German | de-DE | `de-DE_ta.bin` + `de-DE_gl0_sg.bin` |
| Pico Spanish | es-ES | `es-ES_ta.bin` + `es-ES_zl0_sg.bin` |
| Pico French | fr-FR | `fr-FR_ta.bin` + `fr-FR_nk0_sg.bin` |
| Pico Italian | it-IT | `it-IT_ta.bin` + `it-IT_cm0_sg.bin` |

All six ship with the engine; there is nothing further to download. Pico provides
one adult female speaker per language, which is the whole of its lingware.

## Installing

Run `PicoSAPI5-1.0.0-setup.exe` and accept the elevation prompt.

Or, from a staged build, in an elevated command prompt:

```
regsvr32 "C:\Program Files\Pico SAPI5\PicoSAPI5.dll"
regsvr32 "C:\Program Files\Pico SAPI5\PicoSAPI5_x86.dll"
```

Both are worth registering: the first serves 64-bit applications and the second
32-bit ones, and each lands in the registry view its own applications read. They
share one copy of the lingware, so the second costs about half a megabyte.

Administrative rights are required. There is no per-user installation, because
SAPI only enumerates voices from `HKEY_LOCAL_MACHINE` — see
[docs/notes.md](docs/notes.md).

To remove, use Add/Remove Programs, or `regsvr32 /u` on each DLL.

Applications that are already running — screen readers especially — usually need
restarting before a newly installed voice appears in their list.

## Checking that it worked

```
pico_speak --list                 every SAPI 5 voice, with ours marked [pico]
pico_speak --all-pico             speak a sample with each of the six
pico_speak --voice "Pico German" --rate 3 "Guten Tag."
pico_speak --events "One two three."     word and sentence events as they fire
```

## What is supported

**Rate, pitch and volume** all behave as they do with any other SAPI 5 voice.
SAPI's −10…+10 range maps onto a factor of three either way for rate and an
octave either way for pitch; the latter lines up exactly with Pico's own 50–200
range, so neither end clips. Volume is applied to the samples rather than as
markup, which is what lets it follow a slider being moved while an utterance is
already playing.

**Events.** Word and sentence boundaries, and bookmarks. Pico reports no marks of
its own, so the word positions are derived — exactly at each sentence boundary,
interpolated within a sentence. The method is described in
[docs/notes.md](docs/notes.md); the practical effect is that a screen reader's
caret tracks the speech and never drifts or runs backwards.

**Actions.** `SPVA_Speak`, `SPVA_Silence`, `SPVA_Bookmark`, `SPVA_SpellOut` and
`SPVA_ParseUnknownTag`. `SPVA_Pronounce` speaks the fragment's text rather than
its phonemes — see the last section of the notes.

**Aborting, skipping, and changes in flight.** An abort is noticed within a
sentence and leaves the engine clean for the next utterance. Forward sentence
skips are honoured without synthesising the text being skipped. Rate and volume
changes are picked up mid-utterance.

Output is 16 kHz 16-bit mono, which is what Pico's lingware is built for; SAPI
converts it if an application asks for something else.

## Building

Needs Visual Studio 2022 with the C++ workload, CMake 3.20+, and — for the
installer — Inno Setup 6.

```
.\tools\build_all.ps1               build both architectures, stage, run the tests
.\tools\build_all.ps1 -Installer    also compile the installer
.\tools\build_all.ps1 -Clean        start from scratch
```

The result lands in `dist\`. Both builds are statically linked against the CRT,
so an installation carries no redistributable.

Pico is portable C, so it is simply compiled for each architecture and linked
into the engine DLL. There is no helper process and no interprocess audio — a
wrapper around an engine that only exists as a 32-bit binary needs both, and this
one does not.

## Layout

```
src/                the SAPI 5 engine
  pico_tts_engine   ISpTTSEngine and ISpObjectWithToken -- the heart of it
  pico_engine       C++ wrapper over pico's incremental C API
  pico_markup       SAPI parameters to pico markup, escaping, word splitting
  pico_voices       the voice catalogue
  pico_registry     self registration and the voice tokens
  pico_main         DLL entry points and the class factory
  pico_paths        where the lingware, settings and log live
  pico_log          optional file logging
tools/              the three test programs and the build scripts
third_party/pico/   the vendored Apache-2.0 engine
lang/               the vendored lingware
installer/          Inno Setup script
docs/notes.md       findings, decisions, and the changes made to Pico
```

## Testing

Three programs, each one layer further from the engine, so a fault can be placed
quickly:

```
pico_render     text to a WAV, driving pico directly -- no COM, no SAPI
pico_sapitest   the full ISpTTSEngine contract against a site written for the
                purpose; needs no registration and no administrative rights
pico_speak      through the registered SAPI stack, out loud
```

`build_all.ps1` runs `pico_sapitest` for both architectures. It checks the output
format, voice selection through a token, one word event per word with monotonic
in-range offsets, rate and volume actually changing the audio, aborting and
recovering, bookmarks, silence and spelling.

## Diagnostics

Logging is off by default. To turn it on, either set `PICO_SAPI5_LOG` to 1–3
before starting the application, or create
`%LOCALAPPDATA%\Pico SAPI5\config.ini`:

```ini
[diagnostics]
logLevel=2
```

The log is written to `%LOCALAPPDATA%\Pico SAPI5\pico_sapi5.log`.

## Licence

Apache License 2.0, the same as SVOX Pico itself. See [LICENSE](LICENSE) and
[NOTICE](NOTICE). The Pico engine and lingware are © 2008–2009 SVOX AG; the three
small changes needed to build them for Windows are listed in
[docs/notes.md](docs/notes.md).
