#!/usr/bin/env python3
"""Check the imported PT-BR speech against the subtitles and, optionally, the English voices.

Reads the loose WAVs that scripts/import-speech-portugues.ps1 places in
arx/data/core/speech/portugues/ and the subtitles in utext_portugues.ini, then reports:

  * WAV files that are empty, truncated or not RIFF/WAVE;
  * subtitles whose speech file is missing (speech keys are the WAV names);
  * subtitles too long to read while the line plays (more characters per second than
    --max-cps, 20 by default: about what a player reads comfortably);
  * with --english-dir (a folder of the official English WAVs, e.g. unpacked from
    speech.pak with arxunpak): files missing on either side and PT lines much shorter or
    longer than the English take.

Nothing is changed. Exit code 1 when a WAV is broken or missing, 0 otherwise.

  python scripts/check-speech-portugues.py
  python scripts/check-speech-portugues.py --english-dir D:/arx-unpacked/speech/english --report speech-report.md
"""

import argparse
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# A RIFF header plus fmt and data chunk headers: anything this small holds no audio.
MIN_WAV_BYTES = 44


def wav_duration(path):
    """Seconds of audio in a RIFF/WAVE file, or None when the file is broken.

    Reads the chunks directly instead of using the wave module, which only accepts PCM
    and rejects the ADPCM files the game also ships.
    """
    data = path.read_bytes()
    if len(data) <= MIN_WAV_BYTES or data[0:4] != b'RIFF' or data[8:12] != b'WAVE':
        return None
    byte_rate = None
    data_size = None
    pos = 12
    while pos + 8 <= len(data):
        chunk_id = data[pos:pos + 4]
        (size,) = struct.unpack_from('<I', data, pos + 4)
        body = pos + 8
        if chunk_id == b'fmt ' and size >= 16:
            (byte_rate,) = struct.unpack_from('<I', data, body + 8)
        elif chunk_id == b'data':
            data_size = min(size, len(data) - body)
        pos = body + size + (size & 1)
    if not byte_rate or not data_size:
        return None
    return data_size / byte_rate


SECTION = re.compile(r'^\[([^\]]+)\]')
STRING = re.compile(r'^string\s*=\s*"(.*)"\s*$', re.IGNORECASE)


def read_utext(path):
    """{lowercase key: text} for every section with a string= line."""
    texts = {}
    key = None
    for line in path.read_text(encoding='utf-8').splitlines():
        line = line.strip()
        match = SECTION.match(line)
        if match:
            key = match.group(1).strip().lower()
            continue
        match = STRING.match(line)
        if match and key:
            texts[key] = match.group(1)
            key = None
    return texts


def spoken_length(text):
    """Characters a player reads: no markup, no repeated spaces."""
    text = re.sub(r'\{[^}]*\}|\[[^\]]*\]|<[^>]*>', '', text)
    return len(' '.join(text.split()))


def wav_index(folder):
    return {p.stem.lower(): p for p in folder.glob('*.wav')} if folder else {}


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('--speech-dir', type=Path, default=ROOT / 'arx/data/core/speech/portugues')
    parser.add_argument('--utext', type=Path, default=ROOT / 'arx/data/core/localisation/utext_portugues.ini')
    parser.add_argument('--english-dir', type=Path, help='folder with the official English speech WAVs')
    parser.add_argument('--max-cps', type=float, default=20.0, help='characters per second to flag')
    parser.add_argument('--report', type=Path, help='also write the findings to this Markdown file')
    args = parser.parse_args()

    pt = wav_index(args.speech_dir)
    if not pt:
        print(f'No WAV files in {args.speech_dir}. Import them first with scripts/import-speech-portugues.ps1.')
        return 1
    en = wav_index(args.english_dir)
    texts = read_utext(args.utext)

    lines = [f'# PT-BR speech check', '',
             f'- WAV files: {len(pt)} in `{args.speech_dir}`',
             f'- Subtitles: {len(texts)} keys in `{args.utext.name}`']

    durations = {}
    broken = []
    for name, path in sorted(pt.items()):
        seconds = wav_duration(path)
        if seconds is None or seconds < 0.05:
            broken.append(path.name)
        else:
            durations[name] = seconds

    # Speech keys are the subtitle sections that have a WAV under that name on either side.
    speech_keys = sorted(k for k in texts if k in pt or k in en)
    missing_pt = sorted(k for k in speech_keys if k not in pt)

    too_fast = []
    for key in speech_keys:
        seconds = durations.get(key)
        if not seconds:
            continue
        chars = spoken_length(texts[key])
        cps = chars / seconds
        if cps > args.max_cps:
            too_fast.append((cps, key, chars, seconds))
    too_fast.sort(reverse=True)

    def section(title, rows):
        lines.extend(['', f'## {title} ({len(rows)})', ''])
        lines.extend(rows if rows else ['None.'])

    section('Broken or empty WAV files', [f'- `{n}`' for n in broken])
    section('Subtitles whose PT-BR speech file is missing', [f'- `{k}.wav`' for k in missing_pt])
    section(f'Subtitles faster than {args.max_cps:g} characters per second',
            [f'- `{k}`: {c} characters in {s:.1f} s ({cps:.0f}/s)' for cps, k, c, s in too_fast])

    if en:
        only_en = sorted(set(en) - set(pt))
        only_pt = sorted(set(pt) - set(en))
        section('Official English files with no PT-BR file', [f'- `{n}.wav`' for n in only_en])
        section('PT-BR files with no English counterpart', [f'- `{n}.wav`' for n in only_pt])
        ratio_rows = []
        for name, seconds in sorted(durations.items()):
            if name not in en:
                continue
            reference = wav_duration(en[name])
            if reference and (seconds < 0.5 * reference or seconds > 2.0 * reference):
                ratio_rows.append(f'- `{name}.wav`: PT {seconds:.1f} s, EN {reference:.1f} s')
        section('Lines much shorter or longer than the English take', ratio_rows)

    report = '\n'.join(lines) + '\n'
    print(report)
    if args.report:
        args.report.write_text(report, encoding='utf-8')
    return 1 if broken or missing_pt or (en and set(en) - set(pt)) else 0


if __name__ == '__main__':
    sys.exit(main())
