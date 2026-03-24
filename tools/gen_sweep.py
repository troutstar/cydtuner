#!/usr/bin/env python3
"""
gen_sweep.py — Generate sweep.wav for ESP32 strobe tuner pitch validation.

Each of the 49 chromatic notes from E1 (41.2 Hz) to E5 (659.3 Hz) is rendered as:
  - 3 seconds: frequency sweeps from +100 cents above the note down to exact pitch
  - 2 seconds: holds at exact pitch

Output: sweep.wav (16-bit mono PCM, 44100 Hz) in the same directory as this script.
Copy to SD card root as sweep.wav before booting the tuner.
"""

import wave
import math
import array
import os

SAMPLE_RATE   = 44100
AMPLITUDE     = int(32767 * 0.8)   # headroom to avoid clipping
SWEEP_SECS    = 3.0
HOLD_SECS     = 2.0
CENTS_START   = 100.0              # start this many cents sharp
MIDI_LOW      = 28                 # E1 = 41.2 Hz
MIDI_HIGH     = 76                 # E5 = 659.3 Hz
N_HARMONICS   = 8                  # sawtooth-like harmonic series (guitar-like)

NOTE_NAMES = ['C','C#','D','D#','E','F','F#','G','G#','A','A#','B']

# Normalization factor: sum(1/k for k=1..N_HARMONICS)
_NORM = sum(1.0 / k for k in range(1, N_HARMONICS + 1))

def sawtooth(phase, f):
    """Sum of harmonics 1..N_HARMONICS at 1/k amplitude, normalised to [-1, 1].
    Harmonics above Nyquist are skipped. phase is the fundamental phase."""
    s = 0.0
    nyquist = SAMPLE_RATE / 2.0
    for k in range(1, N_HARMONICS + 1):
        if k * f >= nyquist:
            break
        s += math.sin(k * phase) / k
    return s / _NORM

def midi_to_hz(midi):
    return 440.0 * (2.0 ** ((midi - 69) / 12.0))

def midi_to_name(midi):
    return f"{NOTE_NAMES[midi % 12]}{(midi // 12) - 1}"

def main():
    script_dir  = os.path.dirname(os.path.abspath(__file__))
    output_path = os.path.join(script_dir, 'sweep.wav')

    sweep_n = int(SWEEP_SECS * SAMPLE_RATE)
    hold_n  = int(HOLD_SECS  * SAMPLE_RATE)
    two_pi  = 2.0 * math.pi
    phase   = 0.0

    note_count = MIDI_HIGH - MIDI_LOW + 1
    print(f"Generating sweep.wav — {note_count} notes, E1–E5, ~{note_count * (SWEEP_SECS + HOLD_SECS) / 60:.1f} min")

    with wave.open(output_path, 'wb') as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(SAMPLE_RATE)

        for midi in range(MIDI_LOW, MIDI_HIGH + 1):
            f_note = midi_to_hz(midi)
            print(f"  {midi_to_name(midi):4s}  {f_note:7.2f} Hz")

            buf = array.array('h')

            # Sweep: +100 cents → 0 cents (linear in cents = exponential in Hz)
            for i in range(sweep_n):
                cents = CENTS_START * (1.0 - i / sweep_n)
                f     = f_note * (2.0 ** (cents / 1200.0))
                phase += two_pi * f / SAMPLE_RATE
                phase %= two_pi
                buf.append(int(sawtooth(phase, f) * AMPLITUDE))

            # Hold: exact pitch
            for _ in range(hold_n):
                phase += two_pi * f_note / SAMPLE_RATE
                phase %= two_pi
                buf.append(int(sawtooth(phase, f_note) * AMPLITUDE))

            wf.writeframes(buf.tobytes())

    size_mb = os.path.getsize(output_path) / (1024 * 1024)
    print(f"\nDone: {output_path}  ({size_mb:.1f} MB)")
    print("Copy sweep.wav to the SD card root before booting the tuner.")

if __name__ == '__main__':
    main()
