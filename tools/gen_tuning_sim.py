#!/usr/bin/env python3
"""
gen_tuning_sim.py — Guitar tuning simulation for ESP32 strobe tuner.

Simulates a guitarist tuning each string: discrete plucks at fixed
deviations from the target pitch, with silence between each pluck
(while the peg is adjusted).  No continuous sweeps — each pluck
pops immediately to a fixed Hz and decays naturally.

Pluck sequence per string (cents from target):
  -50 → -20 → +12 → -5 → 0
  (flat approach, small overshoot sharp, fine correction, locked)

Output: tuning_sim.wav (int16 PCM, 44100 Hz) — copy to SD card root
"""

import numpy as np
from scipy.io import wavfile
import os

FS          = 44100
AMPLITUDE   = 0.70
MAX_HARM    = 12
PLUCK_DUR   = 2.5   # seconds of audible tone per pluck
DECAY_TAU   = 1.6   # exponential decay time constant (seconds)
ATTACK_MS   = 8     # ms fade-in — fast, like a real pluck
SILENCE_DUR = 0.6   # seconds of silence between plucks (peg adjustment)
OUTPUT      = "tuning_sim.wav"

# Deviation sequence per string: flat approach, small sharp overshoot, in tune
PLUCK_CENTS = [-50, -20, +12, -5, 0]

# All open strings across the instrument collection, sorted low to high
STRINGS = [
    ( 41.20, "E1  "),
    ( 55.00, "A1  "),
    ( 61.74, "B1  "),
    ( 73.42, "D2  "),
    ( 82.41, "E2  "),
    ( 92.50, "F#2 "),
    ( 98.00, "G2  "),
    (110.00, "A2  "),
    (123.47, "B2  "),
    (146.83, "D3  "),
    (164.81, "E3  "),
    (185.00, "F#3 "),
    (196.00, "G3  "),
    (207.65, "G#3 "),
    (246.94, "B3  "),
    (277.18, "C#4 "),
    (329.63, "E4  "),
]

# 1/k^2 harmonic rolloff — plucked string approximation
_K    = np.arange(1, MAX_HARM + 1)
_AMPS = 1.0 / (_K ** 2)
_NORM = _AMPS.sum()


def guitar_wave(t, f_hz):
    """Fixed-frequency harmonic series with 1/k^2 rolloff, Nyquist-limited."""
    nyq  = FS / 2.0
    wave = np.zeros(len(t))
    for k, amp in zip(_K, _AMPS):
        if k * f_hz >= nyq:
            break
        wave += amp * np.sin(2.0 * np.pi * k * f_hz * t)
    return wave / _NORM


def make_pluck(f_hz):
    """Single pluck: instant attack, exponential decay, fixed frequency."""
    n  = int(FS * PLUCK_DUR)
    t  = np.arange(n) / FS

    wave     = guitar_wave(t, f_hz)
    envelope = np.exp(-t / DECAY_TAU)

    # Fast attack (few ms linear fade-in) to simulate pick transient
    attack_n = int(FS * ATTACK_MS / 1000)
    envelope[:attack_n] *= np.linspace(0.0, 1.0, attack_n)

    return AMPLITUDE * wave * envelope


def make_string_segment(f_target):
    """Full tuning sequence for one string: 5 plucks at varying deviations."""
    silence = np.zeros(int(FS * SILENCE_DUR))
    parts   = []
    for cents in PLUCK_CENTS:
        f_inst = f_target * (2.0 ** (cents / 1200.0))
        parts.append(make_pluck(f_inst))
        parts.append(silence)
    return np.concatenate(parts)


def main():
    out_path   = os.path.join(os.path.dirname(os.path.abspath(__file__)), OUTPUT)
    slot_dur   = PLUCK_DUR + SILENCE_DUR
    total_dur  = len(STRINGS) * len(PLUCK_CENTS) * slot_dur

    print(f"Generating {OUTPUT}...")
    print(f"  {len(STRINGS)} strings × {len(PLUCK_CENTS)} plucks × {slot_dur:.1f}s = {total_dur:.0f}s total")
    print()

    segments = []
    # Leading silence so the device is ready before audio starts
    segments.append(np.zeros(int(FS * 1.0)))

    for f, label in STRINGS:
        print(f"  {f:7.2f} Hz  {label}  plucks: ", end="")
        for cents in PLUCK_CENTS:
            f_inst = f * (2.0 ** (cents / 1200.0))
            sign = "+" if cents >= 0 else ""
            print(f"{sign}{cents:3d}¢({f_inst:.2f}Hz)", end="  ")
        print()
        segments.append(make_string_segment(f))

    audio = np.concatenate(segments)
    int16 = (audio * 32767).clip(-32767, 32767).astype(np.int16)
    wavfile.write(out_path, FS, int16)

    mb      = os.path.getsize(out_path) / 1_048_576
    dur_min = len(audio) / FS / 60.0
    print(f"\nDone: {out_path}")
    print(f"  {dur_min:.1f} min  {mb:.1f} MB")
    print(f"  Copy to SD card root as sweep.wav")

    # Print ground truth table for test_harness.c (slot index → Hz)
    print()
    print("Ground truth table (for test_harness.c):")
    print(f"  Leading silence: 1.0s")
    t = 1.0
    for fi, (f, label) in enumerate(STRINGS):
        for pi, cents in enumerate(PLUCK_CENTS):
            f_inst = f * (2.0 ** (cents / 1200.0))
            print(f"  t={t:6.1f}s  {label.strip():<5}  {f_inst:7.3f} Hz  ({'+' if cents>=0 else ''}{cents} cents)")
            t += slot_dur
    print(f"  t={t:6.1f}s  end")


if __name__ == "__main__":
    main()
