# Pitch Test Sweep Generator — Design Spec

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Generate a WAV file containing a sequence of chromatic notes that each sweep from 100 cents sharp into exact pitch, allowing visual and log-based validation of the ESP32 pitch detector across the full instrument frequency range.

**Architecture:** A single Python script (`tools/gen_sweep.py`) generates `sweep.wav`. No firmware changes required — the file is copied to the SD card root and read by the existing `AUDIO_SOURCE_WAV_FILE` path. The script synthesizes phase-continuous sine waves, integrating instantaneous frequency at each sample to eliminate clicks between notes.

**Tech Stack:** Python 3, `wave` stdlib module (no external dependencies)

---

## Signal Structure

- **Note range:** E1 (41.2 Hz) to E5 (659.3 Hz) — every chromatic semitone, 49 notes total
- **Per-note behaviour:**
  - 3 seconds: frequency sweeps linearly (in cents) from +100 cents above the note down to exact pitch
  - 2 seconds: holds at exact pitch
- **Sweep direction:** sharp → in-tune (strobe spins, then locks and turns green)
- **Transitions:** phase-continuous — phase accumulator carries over between notes, no discontinuities
- **Waveform:** pure sine wave, amplitude 0.8 × INT16_MAX (headroom to avoid clipping)

## Output File

| Property | Value |
|---|---|
| Format | PCM WAV, 16-bit signed, mono |
| Sample rate | 44100 Hz |
| Duration | ~245 seconds (~4 min 5 sec) |
| File size | ~21 MB |
| Path on SD card | `/sdcard/sweep.wav` |

## Frequency Computation

Each note's exact frequency:

```
f_note = 440.0 * 2^((midi - 69) / 12)
```

During the 3-second sweep, instantaneous frequency at time `t` (0 → 3s):

```
cents_offset = 100.0 * (1.0 - t / 3.0)
f(t) = f_note * 2^(cents_offset / 1200.0)
```

During the 2-second hold:

```
f(t) = f_note
```

Phase integration per sample:

```
phase += 2π * f(t) / sample_rate   # sample_rate = 44100
phase  = fmod(phase, 2π)          # wrap to maintain float precision
sample = sin(phase) * amplitude
```

## File Layout

```
tools/
  gen_sweep.py     — script to generate sweep.wav
  sweep.wav        — generated output (copy to SD card root)
```

`tools/` is a new directory under the project root. `sweep.wav` is regeneratable and does not need to be checked in.

## Usage

```bash
cd tools
python gen_sweep.py
# outputs sweep.wav in the tools/ directory
# copy sweep.wav to the SD card root as sweep.wav
```

## Validation

After copying to SD card and booting the device:
- Strobe ring should spin (white) at start of each note — 100 cents sharp
- Ring should slow and lock as frequency dials in
- Ring turns green and holds when exact pitch is reached
- Serial log should show Hz readings converging to the note frequency
- Repeat for all 49 notes across the ~4-minute file
