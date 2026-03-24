# Pitch Test Sweep Generator — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Create a Python script that generates `sweep.wav` — a 4-minute test signal covering every chromatic note E1–E5, each sweeping from 100 cents sharp into exact pitch, for validating ESP32 pitch detection accuracy.

**Architecture:** Single Python script using stdlib only (`wave`, `math`, `array`). Writes WAV samples note-by-note directly to disk to avoid large in-memory buffers. Phase accumulator is continuous across the entire file.

**Tech Stack:** Python 3 (stdlib only — no pip dependencies)

---

## File Structure

- Create: `tools/gen_sweep.py` — the generator script
- Output: `tools/sweep.wav` — generated file (copy to SD card root as `sweep.wav`)

---

### Task 1: Create `tools/gen_sweep.py`

**Files:**
- Create: `tools/gen_sweep.py`

- [ ] **Step 1: Create the `tools/` directory and write `gen_sweep.py`**

```python
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

NOTE_NAMES = ['C','C#','D','D#','E','F','F#','G','G#','A','A#','B']

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
                buf.append(int(math.sin(phase) * AMPLITUDE))

            # Hold: exact pitch
            for _ in range(hold_n):
                phase += two_pi * f_note / SAMPLE_RATE
                phase %= two_pi
                buf.append(int(math.sin(phase) * AMPLITUDE))

            wf.writeframes(buf.tobytes())

    size_mb = os.path.getsize(output_path) / (1024 * 1024)
    print(f"\nDone → {output_path}  ({size_mb:.1f} MB)")
    print("Copy sweep.wav to the SD card root before booting the tuner.")

if __name__ == '__main__':
    main()
```

- [ ] **Step 2: Verify the script runs and produces the expected output**

```bash
cd tools
python gen_sweep.py
```

Expected output (last few lines):
```
  E5   659.26 Hz
Done → .../tools/sweep.wav  (~20.6 MB)
Copy sweep.wav to the SD card root before booting the tuner.
```

Verify:
- `sweep.wav` exists in `tools/`
- File size is approximately 20–21 MB
- Can be opened in Audacity or VLC to confirm audio content (optional)

- [ ] **Step 3: Copy to SD card and test on hardware**

Copy `tools/sweep.wav` to the SD card root (must be named exactly `sweep.wav`).

Boot the tuner. Expected behaviour:
- Strobe ring spins white at start of each note (100 cents sharp)
- Ring slows and locks as frequency dials in
- Ring turns green and holds at exact pitch
- Serial log (idf.py monitor) shows Hz converging to note frequency then stabilising
- Sequence runs through all 49 notes over ~4 minutes, then loops
