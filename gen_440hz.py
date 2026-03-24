import struct, math, wave, os

sample_rate = 44100
duration_s  = 5
frequency   = 440.0
amplitude   = 16000

out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "440hz_sine.wav")
with wave.open(out, "w") as wf:
    wf.setnchannels(1)
    wf.setsampwidth(2)
    wf.setframerate(sample_rate)
    for i in range(sample_rate * duration_s):
        s = int(amplitude * math.sin(2.0 * math.pi * frequency * i / sample_rate))
        wf.writeframes(struct.pack("<h", s))
print(f"Generated: {out}")
