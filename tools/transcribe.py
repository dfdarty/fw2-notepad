#!/usr/bin/env python3
"""Turn the notepad's voice memos into text.

The notepad saves each memo as /notes/voice/NNN.wav on the FREE-WILi 2's SD
card and puts a marker like  [voice 7 0:18]  in /notes/notes.txt. This
script transcribes every memo that still has such a marker, with Whisper
(faster-whisper, on your own computer, no internet needed after the first
run), and replaces the marker with the words:

    [voice 7] Apogee eight hundred forty feet, drifted east.

It also writes voice/NNN.txt next to each recording, keeps the audio, and
backs notes.txt up to notes.txt.bak before changing it. Markers that were
already transcribed are left alone, so it's safe to run again.

    pip install faster-whisper
    python transcribe.py E:\\            # the SD card (Windows)
    python transcribe.py /media/sd       # or its /notes folder, or notes.txt
    python transcribe.py E:\\ --model small.en    # slower, more accurate

The first run downloads the model (base.en: about 150 MB).
"""

import argparse
import os
import re
import shutil
import sys
import unicodedata

MARKER = re.compile(r"\[voice (\d+) (\d+:\d\d)\]")

# The FREE-WILi 2's font is plain ASCII: turn typographic characters into
# their ASCII look-alikes and drop anything else it can't show.
ASCII = {"\u2018": "'", "\u2019": "'", "\u201c": '"', "\u201d": '"', "\u2013": "-", "\u2014": "-",
         "\u2026": "...", "\u00b0": " deg", "\u00bd": " 1/2", "\u00bc": " 1/4", "\u00be": " 3/4"}


def to_ascii(text):
    for k, v in ASCII.items():
        text = text.replace(k, v)
    text = unicodedata.normalize("NFKD", text)
    return "".join(c for c in text if 32 <= ord(c) < 127)


def find_notes(path):
    """Accept the card's root, its notes folder, or notes.txt itself."""
    for cand in (path, os.path.join(path, "notes.txt"), os.path.join(path, "notes", "notes.txt")):
        if os.path.isfile(cand) and os.path.basename(cand).lower() == "notes.txt":
            return cand
    sys.exit(f"no notes.txt found at {path} (give the SD card, its notes folder, or notes.txt)")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("path", help="the SD card, its notes folder, or notes.txt")
    ap.add_argument("--model", default="base.en",
                    help="Whisper model: tiny.en, base.en (default), small.en, medium.en ... bigger is slower and better")
    ap.add_argument("--prompt", default="Rocket launch field notes: motor, apogee, altitude in feet, "
                                        "parachute, drogue, recovery, wind, launch rail.",
                    help="words to expect, which helps with jargon (default: model-rocketry terms)")
    ap.add_argument("--dry-run", action="store_true", help="print the transcripts, change no files")
    a = ap.parse_args()

    notes_path = find_notes(a.path)
    voice_dir = os.path.join(os.path.dirname(notes_path), "voice")
    with open(notes_path, encoding="ascii", errors="replace", newline="") as f:
        notes = f.read()

    todo = [(m, os.path.join(voice_dir, f"{int(m.group(1)):03d}.wav")) for m in MARKER.finditer(notes)]
    if not todo:
        print("nothing to do: no untranscribed [voice N m:ss] markers in", notes_path)
        return 0
    missing = [w for _, w in todo if not os.path.isfile(w)]
    for w in missing:
        print("missing recording:", w, "(its marker is left as it is)")

    try:
        from faster_whisper import WhisperModel
    except ImportError:
        sys.exit("needs faster-whisper: pip install faster-whisper")
    print(f"loading Whisper {a.model} (the first time, this downloads it) ...")
    model = WhisperModel(a.model, device="cpu", compute_type="int8")

    out, last, done = [], 0, 0
    for m, wav in todo:
        out.append(notes[last:m.start()])
        last = m.end()
        if not os.path.isfile(wav):
            out.append(m.group(0))
            continue
        segments, _ = model.transcribe(wav, beam_size=5, vad_filter=True, initial_prompt=a.prompt)
        text = to_ascii(" ".join(s.text.strip() for s in segments)).strip() or "(nothing heard)"
        print(f"voice {m.group(1)} ({m.group(2)}): {text}")
        out.append(f"[voice {m.group(1)}] {text}")
        done += 1
        if not a.dry_run:
            with open(wav[:-4] + ".txt", "w", encoding="ascii", newline="\n") as f:
                f.write(text + "\n")
    out.append(notes[last:])
    new = "".join(out)

    if a.dry_run or not done:
        return 0
    shutil.copyfile(notes_path, notes_path + ".bak")
    with open(notes_path, "w", encoding="ascii", newline="") as f:
        f.write(new)
    print(f"transcribed {done} memo(s) into {notes_path} (previous version: notes.txt.bak)")
    if len(new) > 32767:
        print("note: notes.txt is now over 32 KB; the notepad will open it read-only")
    return 0


if __name__ == "__main__":
    sys.exit(main())
