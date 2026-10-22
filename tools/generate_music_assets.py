#!/usr/bin/env python3
"""Generate and verify the original PCM tracks embedded by the music player."""

from __future__ import annotations

import argparse
import hashlib
import math
import struct
import sys
from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
OUTPUT_DIR = ROOT / "assets" / "music" / "pcm"
SAMPLE_RATE = 16_000
BITS_PER_SAMPLE = 16
CHANNELS = 1
AMPLITUDE = 0.26


@dataclass(frozen=True)
class Note:
    """One note or rest in a compact score; duration is measured in beats."""

    name: str
    beats: float


@dataclass(frozen=True)
class Track:
    """A deterministic original composition emitted as signed 16-bit mono PCM."""

    slug: str
    title: str
    bpm: int
    notes: tuple[Note, ...]


# Frequencies are equal-tempered A4=440 Hz, rounded only when synthesized.
FREQUENCIES = {
    "C4": 261.625565,
    "D4": 293.664768,
    "E4": 329.627557,
    "F4": 349.228231,
    "G4": 391.995436,
    "A4": 440.0,
    "B4": 493.883301,
    "C5": 523.251131,
    "D5": 587.329536,
    "E5": 659.255114,
    "F5": 698.456463,
    "G5": 783.990872,
    "A5": 880.0,
    "B5": 987.766603,
    "C6": 1046.502261,
}

TRACKS = (
    Track(
        slug="morning_postcard",
        title="Morning Postcard",
        bpm=112,
        notes=(
            Note("E5", 0.5), Note("D5", 0.5), Note("C5", 1.0),
            Note("G4", 0.5), Note("A4", 0.5), Note("C5", 1.0),
            Note("D5", 0.5), Note("E5", 0.5), Note("G5", 1.5),
            Note("E5", 0.5), Note("D5", 0.5), Note("C5", 1.5),
            Note("A4", 0.5), Note("G4", 0.5), Note("E4", 1.0),
            Note("G4", 1.0), Note("C5", 2.0),
            Note("REST", 0.5),
            Note("G4", 0.5), Note("C5", 0.5), Note("E5", 0.5),
            Note("D5", 0.5), Note("C5", 0.5), Note("A4", 1.5),
            Note("G4", 0.5), Note("A4", 0.5), Note("C5", 1.0),
            Note("D5", 1.0), Note("C5", 2.0),
            Note("REST", 1.0),
        ),
    ),
    Track(
        slug="blue_hour",
        title="Blue Hour",
        bpm=88,
        notes=(
            Note("A4", 0.5), Note("C5", 0.5), Note("E5", 1.0),
            Note("D5", 0.5), Note("C5", 0.5), Note("A4", 1.0),
            Note("G4", 0.5), Note("A4", 0.5), Note("C5", 1.5),
            Note("E5", 0.5), Note("D5", 1.0),
            Note("C5", 0.5), Note("A4", 0.5), Note("G4", 1.0),
            Note("E4", 0.5), Note("G4", 0.5), Note("A4", 1.0),
            Note("C5", 0.5), Note("D5", 0.5), Note("E5", 1.5),
            Note("G5", 0.5), Note("E5", 1.0),
            Note("D5", 0.5), Note("C5", 0.5), Note("A4", 1.0),
            Note("G4", 0.5), Note("E4", 0.5), Note("D4", 1.0),
            Note("E4", 0.5), Note("G4", 0.5), Note("A4", 1.0),
            Note("C5", 0.5), Note("A4", 0.5), Note("G4", 2.0),
            Note("REST", 1.0),
        ),
    ),
    Track(
        slug="little_engine",
        title="Little Engine",
        bpm=136,
        notes=(
            Note("C5", 0.5), Note("E5", 0.5), Note("G5", 0.5),
            Note("E5", 0.5), Note("C5", 0.5), Note("E5", 0.5),
            Note("G5", 1.0), Note("REST", 0.5),
            Note("D5", 0.5), Note("F5", 0.5), Note("A5", 0.5),
            Note("F5", 0.5), Note("D5", 0.5), Note("F5", 0.5),
            Note("A5", 1.0), Note("REST", 0.5),
            Note("E5", 0.5), Note("G5", 0.5), Note("C6", 0.5),
            Note("G5", 0.5), Note("E5", 0.5), Note("G5", 0.5),
            Note("C6", 1.0), Note("REST", 0.5),
            Note("F5", 0.5), Note("E5", 0.5), Note("D5", 0.5),
            Note("C5", 0.5), Note("G4", 0.5), Note("A4", 0.5),
            Note("C5", 1.0), Note("REST", 0.5),
            Note("G4", 0.5), Note("C5", 0.5), Note("E5", 0.5),
            Note("G5", 0.5), Note("E5", 0.5), Note("C5", 0.5),
            Note("G4", 1.0), Note("REST", 0.5),
            Note("E5", 0.5), Note("D5", 0.5), Note("C5", 0.5),
            Note("G4", 0.5), Note("A4", 0.5), Note("G4", 0.5),
            Note("C5", 2.0),
        ),
    ),
)


def note_frames(beats: float, bpm: int) -> int:
    """Convert a score duration to an integer sample count for one track."""
    return round(beats * 60.0 / bpm * SAMPLE_RATE)


def synthesize(track: Track) -> bytes:
    """Render a gentle additive waveform without any third-party samples."""
    frames = bytearray()
    for note in track.notes:
        count = note_frames(note.beats, track.bpm)
        if note.name == "REST":
            frames.extend(b"\0\0" * count)
            continue

        frequency = FREQUENCIES[note.name]
        duration = count / SAMPLE_RATE
        for index in range(count):
            time = index / SAMPLE_RATE
            attack = min(1.0, time / 0.012)
            release = min(1.0, max(0.0, (duration - time) / 0.055))
            envelope = min(attack, release)
            # A quiet fundamental plus two overtones reads clearly on the tiny
            # speaker without the harshness of a full square wave.
            wave = (
                math.sin(2.0 * math.pi * frequency * time)
                + 0.20 * math.sin(4.0 * math.pi * frequency * time)
                + 0.06 * math.sin(6.0 * math.pi * frequency * time)
            )
            sample = int(max(-1.0, min(1.0, wave * AMPLITUDE * envelope)) * 32767)
            frames.extend(struct.pack("<h", sample))

        # A short silence prevents adjacent repeated notes from sounding slurred.
        frames.extend(b"\0\0" * round(SAMPLE_RATE * 0.032))
    return bytes(frames)


def write_if_changed(path: Path, content: bytes) -> None:
    """Avoid timestamp churn when regenerating identical assets."""
    if path.exists() and path.read_bytes() == content:
        return
    path.write_bytes(content)


def expected_assets() -> dict[str, bytes]:
    """Return all generated filenames and their exact deterministic contents."""
    return {f"{track.slug}.pcm": synthesize(track) for track in TRACKS}


def generate() -> None:
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    for name, content in expected_assets().items():
        write_if_changed(OUTPUT_DIR / name, content)
        digest = hashlib.sha256(content).hexdigest()
        print(f"{name}: {len(content)} bytes sha256={digest}")


def verify() -> int:
    """Require checked-in assets to match the reproducible generator."""
    failures = 0
    for name, content in expected_assets().items():
        path = OUTPUT_DIR / name
        if not path.is_file():
            print(f"MISSING: {path}", file=sys.stderr)
            failures += 1
            continue
        actual = path.read_bytes()
        if actual != content:
            print(f"MISMATCH: {path}", file=sys.stderr)
            failures += 1
            continue
        digest = hashlib.sha256(actual).hexdigest()
        print(f"{name}: PASS {len(actual)} bytes sha256={digest}")
    if failures:
        print(f"Music asset verification failed: {failures} error(s)", file=sys.stderr)
        return 1
    print(f"Music assets: PASS ({len(TRACKS)} tracks)")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="verify without rewriting")
    args = parser.parse_args()
    if args.check:
        return verify()
    generate()
    return verify()


if __name__ == "__main__":
    raise SystemExit(main())
