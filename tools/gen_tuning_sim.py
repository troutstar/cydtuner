#!/usr/bin/env python3
"""
gen_tuning_sim.py — Continuous-sweep guitar tuning simulation.

Per note: continuous tone whose pitch sweeps like a guitarist turning a peg.
Starts -30 cents flat, approaches fast, decelerates near the note, overshoots
slightly sharp, pulls back under ~5 cents flat, then slow final tune to 0 and holds.

All 17 strings. Each note ~8 seconds. Total ~2.5 min.
Output: tuning_sim.wav (int16 PCM, 44100 Hz) — copy to SD card root.
"""

import numpy as np
from scipy.interpolate import CubicSpline
from scipy.io import wavfile
import os

FS        = 44100
AMPLITUDE = 0.70
MAX_HARM  = 12
OUTPUT    = "tuning_sim.wav"

# Realistic tuning trajectory: waypoints (time_s, cents_deviation).
# Fast approach from -30, overshoot sharp, pull back flat, fine tune to 0.
TUNING_WAYPOINTS = [
    (0.0,  -30.0),   # flat, peg turning fast
    (1.0,  -18.0),   # still moving quickly
    (2.0,   -5.0),   # slowing near the note
    (2.8,  +12.0),   # overshoot sharp
    (3.5,   +3.0),   # catching the overshoot
    (4.2,   -5.0),   # pull back slightly flat
    (5.5,   -1.0),   # very close, creeping up
    (7.0,    0.0),   # locked on pitch
    (8.0,    0.0),   # hold locked
]

NOTE_DUR    = TUNING_WAYPOINTS[-1][0]   # seconds per note
SILENCE_DUR = 0.5                       # gap between notes

# All strings from the instrument collection, low to high
STRINGS = [
    ( 41.20, "E1"),
    ( 55.00, "A1"),
    ( 61.74, "B1"),
    ( 73.42, "D2"),
    ( 82.41, "E2"),
    ( 92.50, "F#2"),
    ( 98.00, "G2"),
    (110.00, "A2"),
    (123.47, "B2"),
    (146.83, "D3"),
    (164.81, "E3"),
    (185.00, "F#3"),
    (196.00, "G3"),
    (207.65, "G#3"),
    (246.94, "B3"),
    (277.18, "C#4"),
    (329.63, "E4"),
]

# Cubic spline through waypoints — smooth acceleration/deceleration
_wp_t = np.array([w[0] for w in TUNING_WAYPOINTS])
_wp_c = np.array([w[1] for w in TUNING_WAYPOINTS])
_cents_spline = CubicSpline(_wp_t, _wp_c, bc_type='clamped')

# Harmonic amplitudes: 1/k^2 rolloff (plucked string approximation)
_K    = np.arange(1, MAX_HARM + 1)
_AMPS = 1.0 / (_K ** 2)
_NORM = _AMPS.sum()


def make_note_segment(f_target):
    """Continuous tone: pitch follows the tuning spline, harmonics preserved."""
    n      = int(FS * NOTE_DUR)
    t      = np.arange(n) / FS
    cents  = _cents_spline(t)
    f_inst = f_target * 2.0 ** (cents / 1200.0)

    # Phase accumulation — keeps waveform continuous as frequency varies
    fund_phase = np.cumsum(2.0 * np.pi * f_inst / FS)

    wave = np.zeros(n)
    for k, amp in zip(_K, _AMPS):
        if k * f_target >= FS / 2.0:
            break
        wave += amp * np.sin(k * fund_phase)
    wave /= _NORM

    # Short attack and tail fade; otherwise constant amplitude
    env      = np.ones(n)
    atk_n    = int(FS * 0.012)
    fade_n   = int(FS * 0.05)
    env[:atk_n]  = np.linspace(0.0, 1.0, atk_n)
    env[-fade_n:] = np.linspace(1.0, 0.0, fade_n)

    return AMPLITUDE * wave * env


def main():
    out_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), OUTPUT)
    silence  = np.zeros(int(FS * SILENCE_DUR))
    segments = [np.zeros(int(FS * 1.0))]   # 1s lead for device ready

    print(f"Generating {OUTPUT}  ({len(STRINGS)} notes × {NOTE_DUR:.0f}s each)")
    print()

    for f, label in STRINGS:
        print(f"  {f:7.2f} Hz  {label:<5}  "
              f"-30 → +12 overshoot → -5 → 0 cents")
        segments.append(make_note_segment(f))
        segments.append(silence)

    audio = np.concatenate(segments)
    int16 = (audio * 32767).clip(-32767, 32767).astype(np.int16)
    wavfile.write(out_path, FS, int16)

    dur = len(audio) / FS
    mb  = os.path.getsize(out_path) / 1_048_576
    print(f"\nDone: {out_path}")
    print(f"  {dur:.0f}s  ({dur / 60:.1f} min)  {mb:.1f} MB")
    print(f"  Copy to SD card root as tuning_sim.wav")


if __name__ == "__main__":
    main()
