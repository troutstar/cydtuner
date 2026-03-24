import struct, math, wave, os

sample_rate = 44100
amplitude   = 16000

# Standard guitar open-string frequencies (E2 to E4)
strings = [
    ('E2',  82.407),
    ('A2', 110.000),
    ('D3', 146.832),
    ('G3', 195.998),
    ('B3', 246.942),
    ('E4', 329.628),
]

HOLD_S  = 3.0   # seconds to hold each open-string pitch
SWEEP_S = 2.0   # seconds to glide between adjacent strings

# Up: E2 → A2 → D3 → G3 → B3 → E4
segments = []
for i in range(len(strings)):
    f = strings[i][1]
    segments.append((HOLD_S, f, f))
    if i < len(strings) - 1:
        segments.append((SWEEP_S, f, strings[i + 1][1]))

# Down: E4 → B3 → G3 → D3 → A2 → E2  (skip second E4 hold)
for i in range(len(strings) - 1, 0, -1):
    segments.append((SWEEP_S, strings[i][1], strings[i - 1][1]))
    segments.append((HOLD_S, strings[i - 1][1], strings[i - 1][1]))

out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "sweep.wav")
with wave.open(out, "w") as wf:
    wf.setnchannels(1)
    wf.setsampwidth(2)
    wf.setframerate(sample_rate)
    phase = 0.0
    for (dur, f_start, f_end) in segments:
        n = int(sample_rate * dur)
        for i in range(n):
            t = i / n
            freq  = f_start + (f_end - f_start) * t
            phase += 2.0 * math.pi * freq / sample_rate
            wf.writeframes(struct.pack("<h", int(amplitude * math.sin(phase))))

total_s = sum(d for d, _, _ in segments)
print(f"Generated: {out}  ({total_s:.0f}s, {os.path.getsize(out)//1024}KB)")
