# Portuguese (Brazil) speech

Loose WAV files here are mounted as `speech/portugues/` on the next launch (`data.dirs` already points at `data/core`). The audio slider then lists **Português (Brasil)** next to the official voices.

The binaries are not committed (size). Import a complete set of 1999 files that match the official English names:

```powershell
powershell -ExecutionPolicy Bypass -File scripts/import-speech-portugues.ps1 -SourceDir '<folder with the .wav files>'
```

Expected layout after import: `arx/data/core/speech/portugues/*.wav` (lowercase names, no subfolders).
