# Changelog

## 1.0.0

First release.

- A SAPI 5 engine exposing all six SVOX Pico voices: American English, British
  English, German, Spanish, French and Italian.
- 32-bit and 64-bit engines, each registering itself for applications of its own
  bitness, sharing one copy of the lingware.
- Rate, pitch and volume, both as the voice's own settings and as per-fragment
  overrides. Rate and volume changes are picked up mid-utterance.
- Word boundary, sentence boundary and bookmark events. Word positions are exact
  at sentence boundaries and interpolated within a sentence, since Pico reports
  no marks of its own.
- `SPVA_Speak`, `SPVA_Silence`, `SPVA_Bookmark`, `SPVA_SpellOut` and
  `SPVA_ParseUnknownTag`; `SPVA_Pronounce` falls back to speaking the text.
- Aborting and forward sentence skipping, both leaving the engine clean for the
  next utterance.
- Text that would otherwise be read as Pico markup is neutralised. Besides
  letting a document change the listener's speaking rate, `<spell>` faults the
  engine, so this is what keeps a screen reader from being taken down by the page
  it is reading.
- Three test programs — `pico_render`, `pico_sapitest`, `pico_speak` — and an
  Inno Setup installer. The suite runs once per voice for each architecture,
  because which voice the engine starts on turned out to matter.
- A voice change reinitialises the Pico system rather than releasing just the
  previous voice. Pico suballocates from a fixed arena, and releasing a voice
  leaves it fragmented: selecting American English after speaking with French
  failed to load, silently, every time.

Changes to the vendored Pico engine, all Windows-only and all marked in the
source: little-endian definition without `<endian.h>`, UTF-8 resource paths
opened with `_wfopen`, and `dllexport` made conditional so the engine's symbols
are not re-exported from the SAPI DLL.
