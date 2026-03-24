#!/usr/bin/env python3
"""
claudesweeps.py — Pitch validation signals for ESP32 strobe tuner.

Covers all open strings across the instrument collection:
  4-str bass standard, 7-str drop A, 6-str standard, drop D, drop A, C-flat

Three-phase tuning simulation per string (same behaviour as sweeps.py):
  0-4s:  approach from 100 cents flat, overshoot to ~50 cents sharp
  4-9s:  damped oscillation — peg hunting and correcting
  9-12s: locked at exact pitch

Waveform: 1/k^2 harmonic series (plucked string), cents-based deviation
so the sweep feels equal across the full frequency range.

Output: sweep.wav (int16 PCM, 44100 Hz) — copy to SD card root
"""

import numpy as np
from scipy.io import wavfile
import os

FS          = 44100
NOTE_DUR    = 12.0
SILENCE_DUR = 1.5
AMPLITUDE   = 0.6
MAX_HARM    = 12
OUTPUT      = "sweep.wav"

# 1/k^2 harmonic rolloff — plucked string approximation
_K    = np.arange(1, MAX_HARM + 1)
_AMPS = 1.0 / (_K ** 2)
_NORM = _AMPS.sum()

# All open strings across the instrument collection, sorted low to high
STRINGS = [
    ( 41.20, "E1   bass standard"),
    ( 55.00, "A1   bass standard / drop A"),
    ( 61.74, "B1   7-str / C-flat guitar"),
    ( 73.42, "D2   bass / drop D"),
    ( 82.41, "E2   guitar standard low"),
    ( 92.50, "F#2  C-flat guitar"),
    ( 98.00, "G2   bass standard"),
    (110.00, "A2   guitar standard"),
    (123.47, "B2   C-flat guitar"),
    (146.83, "D3   guitar standard"),
    (164.81, "E3   C-flat guitar"),
    (185.00, "F#3  drop A guitar"),
    (196.00, "G3   guitar standard"),
    (207.65, "G#3  C-flat guitar"),
    (246.94, "B3   guitar standard"),
    (277.18, "C#4  C-flat guitar"),
    (329.63, "E4   guitar standard high"),
]


def guitar_wave(phase, f_fund):
    """Harmonic series, 1/k^2 amplitude, Nyquist-limited."""
    nyq  = FS / 2.0
    wave = np.zeros_like(phase)
    for k, amp in zip(_K, _AMPS):
        if k * f_fund >= nyq:
            break
        wave += amp * np.sin(k * phase)
    return wave / _NORM


def cents_curve(n):
    """
    Phase 1 (0-4s):  -100 cents -> +50 cents  (flat approach, overshoot)
    Phase 2 (4-9s):  damped cosine from +50 -> ~0  (hunt and correct)
    Phase 3 (9-12s): 0 cents  (locked)
    """
    t     = np.arange(n) / FS
    cents = np.zeros(n)

    m1 = t < 4.0
    cents[m1] = -100.0 + 150.0 * (t[m1] / 4.0)   # -100 -> +50

    m2 = (t >= 4.0) & (t < 9.0)
    t2 = t[m2] - 4.0
    cents[m2] = 50.0 * np.cos(np.pi * t2 / 5.0) * np.exp(-t2 / 1.5)

    return cents


def generate_string(f_target):
    n      = int(FS * NOTE_DUR)
    cents  = cents_curve(n)
    f_inst = f_target * 2.0 ** (cents / 1200.0)

    phase  = 2.0 * np.pi * np.cumsum(f_inst) / FS
    audio  = AMPLITUDE * guitar_wave(phase, f_target)

    fade = int(FS * 0.1)
    audio[:fade]  *= np.linspace(0, 1, fade)
    audio[-fade:] *= np.linspace(1, 0, fade)

    return audio


def main():
    out_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), OUTPUT)
    silence  = np.zeros(int(FS * SILENCE_DUR))
    segments = []

    for f, label in STRINGS:
        print(f"  {f:7.2f} Hz  {label}")
        segments.append(generate_string(f))
        segments.append(silence)

    audio = np.concatenate(segments)
    int16 = (audio * 32767).clip(-32767, 32767).astype(np.int16)
    wavfile.write(out_path, FS, int16)

    mb      = os.path.getsize(out_path) / 1_048_576
    dur_min = len(audio) / FS / 60.0
    print(f"\nDone: {out_path}")
    print(f"  {len(STRINGS)} strings  {dur_min:.1f} min  {mb:.1f} MB")
    print(f"  Copy to SD card root as sweep.wav")


if __name__ == "__main__":
    print(f"Generating {OUTPUT} ({len(STRINGS)} strings)...")
    main()
