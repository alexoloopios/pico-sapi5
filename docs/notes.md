# Implementation notes

Things that were measured rather than assumed, and the reasoning behind the
decisions they led to. Anyone changing this port should read the first two
sections before touching `EscapeText` or the word boundary code.

## Changes to the vendored Pico engine

`third_party/pico/lib` is the upstream Apache-2.0 engine, vendored verbatim apart
from three changes. Each is marked in the source with a comment beginning "Local
change for the SAPI 5 port".

| File | Change | Why |
| --- | --- | --- |
| `picopltf.h` | Define `PICO_ENDIANNESS` as little-endian on Windows instead of including `<endian.h>` | Neither the MSVC nor the mingw-w64 headers have `<endian.h>`. Every architecture Windows runs on is little-endian. |
| `picopal.c` | On Windows, treat the path given to `picopal_fopen` as UTF-8 and open it with `_wfopen` | The narrow `fopen` resolves its path through the active ANSI code page, so lingware under a profile whose name is not representable there cannot be opened at all. The alternative — falling back to 8.3 short names — fails outright on volumes where short name creation is disabled. |
| `picoapi.h` | Make the `dllexport` on every entry point conditional on `PICO_STATIC` | The engine is linked into the SAPI DLL as a static library. Without this, all ~30 `pico_*` symbols are re-exported from it alongside the four COM entry points. |

None of the three changes behaviour on any platform other than Windows.

## Pico markup in text that is being read aloud

Pico takes its prosody from tags embedded in the text — `<speed level="n">`,
`<pitch level="n">` — rather than from an API call. Anything being spoken is
therefore also being parsed for markup, and text arriving from an application is
not necessarily trustworthy: a screen reader reading a web page will hand over
whatever is on it.

Measured with `pico_render --raw`, speaking `alpha bravo charlie delta`
(1.88 s at default settings):

| Input | Result |
| --- | --- |
| `alpha < bravo charlie delta` | 2.40 s — the `<` is spoken as "less than" |
| `alpha <bravo> charlie delta` | 2.98 s — an unrecognised tag is spoken as text |
| `alpha <speed level="20"> bravo charlie delta` | **8.14 s — the tag is obeyed** |
| `alpha < speed level="20"> bravo charlie delta` | 8.14 s — a space after `<` does not prevent it |
| `alpha <spell> bravo charlie delta` | **segmentation fault** |

So Pico is well behaved with an ordinary angle bracket and speaks it correctly,
but text containing one of its own tags can change the listener's speaking rate,
and `<spell>` faults the engine — which, inside a screen reader, takes the
application down with it. `<genfile file="...">` writes a file.

`EscapeText` therefore neutralises a `<` only when it opens something Pico would
read as one of its tags, by substituting the fullwidth `＜`, which the tokenizer
does not recognise as a tag opener. Ordinary text keeps its brackets and their
pronunciation. Both halves of that are covered by the checks in the escaping
section of `pico_sapitest`.

The same fault is why `SPVA_SpellOut` splits the text into characters here rather
than using Pico's `<spell>`, which would have been the idiomatic way to do it.

## Word boundary events without marks

SAPI expects the engine to say where each word falls in the audio, so that a
screen reader can highlight what is being spoken. Pico reports nothing:
`pico_getData` returns `PICO_DATA_PCM_16BIT` unconditionally, and although the
tokenizer understands a `<mark>` tag, the marker item it produces is consumed
inside the pipeline (`picopr.c`) and never reaches the output stage.

What Pico does do is hold a sentence back until it is complete. `SpeakFragment`
uses that as the clock: it feeds one word at a time and drains after each, and
when audio finally appears it belongs to the words fed since the last time any
did — that is, to the sentence just completed. Those words are then spread across
that block in proportion to their length.

The result is exact at every sentence boundary and interpolated within a
sentence, which is the right way round: the error cannot accumulate, and a caret
driven by these events never falls behind or runs backwards. `pico_sapitest`
asserts one event per word, monotonic offsets, and offsets that stay inside the
audio actually written.

Feeding word by word is safe because the spans reproduce the original text byte
for byte — the tokenizer sees exactly what it would have seen had the text
arrived in one piece.

## Switching voices tears down the whole Pico system

`pico_initialize` is handed a fixed arena — 3 MB here — and everything Pico
allocates thereafter, lingware included, is suballocated from it. Releasing a
voice with `pico_disposeEngine`, `pico_releaseVoiceDefinition` and
`pico_unloadResource` returns its memory, but not in a form that can necessarily
be reused: the arena ends up fragmented, and the next `pico_newEngine` can fail
with `PICO_EXC_OUT_OF_MEM` while most of the arena is free.

It is not hypothetical. The voices differ substantially in size:

| Voice | Lingware |
| --- | --- |
| en-US | 1394 KB |
| fr-FR | 1186 KB |
| de-DE | 1050 KB |
| en-GB | 973 KB |
| it-IT | 859 KB |
| es-ES | 841 KB |

Speaking a few utterances with French and then selecting American English —
entirely ordinary behaviour in any application with a voice list — failed every
time, and the symptom was silence rather than an error the user would see.

`Engine::Open` therefore calls `Close()`, which runs `pico_terminate` and starts
the next voice from a fresh arena, rather than releasing only the previous
voice's resources. The extra cost is a memset; the expensive part of a switch is
reading a megabyte of lingware, which happens either way.

The condition needs the outgoing voice to have been *used*, not merely loaded —
the working allocations of synthesis are what leave the holes — and it does not
arise from every starting voice. That is why `build_all.ps1` runs the whole test
suite once per voice for each architecture rather than once: which voice the
engine starts on is part of the state under test. A single run from the default
voice does not reproduce it.

## SAPI does not enumerate per-user voices

A per-user installation would have avoided needing administrative rights, and
`regsvr32 /n /i:user` is the usual way to offer one. It does not work here, and
this was tested rather than inferred: with six voice tokens written to
`HKCU\SOFTWARE\Microsoft\Speech\Voices\Tokens` and the class registered under
`HKCU\Software\Classes`, `ISpObjectTokenCategory::EnumTokens` returned 592 voices
on the test machine and none of them were the Pico ones.

`SPCAT_VOICES` is a registry path beginning with `HKEY_LOCAL_MACHINE`, and SAPI
enumerates a category only from the hive its identifier names. So machine-wide
registration is the only kind that works, and the code no longer offers the
alternative.

## Why there is no helper process

The obvious model for this port was the Infovox 330 SAPI 5 wrapper, which runs a
32-bit helper process and ships audio back over a named pipe. It has to: the
engine it wraps only exists as a 32-bit binary, so a 64-bit application cannot
load it in process.

Pico is portable C with no such constraint. Compiling it for both architectures
removes the helper process, the pipe protocol, the wire format and the registry
API hooking in one go, and leaves a single self-contained DLL per architecture
whose only dependency is the lingware beside it.

## Volume is a gain, not a tag

Rate and pitch are rendered into the text as Pico tags. Volume is not: SAPI lets
an application move the volume while an utterance is already playing, and
re-synthesising to honour that would be both slow and audible. `ApplyGain` scales
the samples on the way to the site instead, so a change takes effect in the next
block. It saturates rather than wrapping, so a clipped peak stays a peak.

## What is not implemented

`SPVA_Pronounce` speaks the fragment's text rather than its phonemes. SAPI
supplies pronunciations in its own universal phone set and Pico's `<phoneme>` tag
takes SAMPA; the mapping is per-language and large, and given that a malformed
phoneme string is exactly the kind of input that faulted the engine above, the
documented fallback of speaking the text is the better trade. Everything else in
`SPVACTIONS` is handled.

A fragment's `LangID` is not used to switch voice mid-utterance. SAPI normally
resolves `<lang>` by choosing a different voice itself, and switching would mean
reloading several megabytes of lingware inside a `Speak` call.
