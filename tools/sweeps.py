import numpy as np
from scipy.io import wavfile

# Configuration
FS = 44100
STRING_DURATION = 12.0  # Duration per string
SILENCE_DURATION = 1.5  # Silence between strings
AMPLITUDE = 0.6
FILENAME = "sweep.wav"

# Frequencies for Dropped A 7-String (A1, E2, A2, D3, G3, B3, E4)
TARGET_NOTES = [55.00, 82.41, 110.00, 146.83, 196.00, 246.94, 329.63]

def generate_tuning_segment(f_target, duration, fs):
    t = np.linspace(0, duration, int(fs * duration), endpoint=False)
    f_curve = np.zeros_like(t)

    # 0-4s: Rise from 3.5Hz flat
    m1 = (t >= 0) & (t < 4)
    f_curve[m1] = (f_target - 3.5) + (3.2 * (t[m1] / 4))

    # 4-9s: Hysteresis/Hunting (Oscillates and settles)
    m2 = (t >= 4) & (t < 9)
    t_m2 = t[m2] - 4
    # Simulates peg turning past target and correcting
    f_curve[m2] = f_target + (0.9 * np.sin(1.5 * np.pi * (t_m2 / 5)) * np.exp(-t_m2 / 2.5))

    # 9-12s: Final Lock
    m3 = (t >= 9)
    f_curve[m3] = f_target

    # Continuous phase accumulation to prevent waveform jumps
    phase = 2 * np.pi * np.cumsum(f_curve) / fs
    audio = AMPLITUDE * np.sin(phase)

    # 100ms Fade in/out to prevent clicks between strings
    fade = int(fs * 0.1)
    audio[:fade] *= np.linspace(0, 1, fade)
    audio[-fade:] *= np.linspace(1, 0, fade)

    return audio

def main():
    full_audio = []
    silence = np.zeros(int(FS * SILENCE_DURATION))

    for freq in TARGET_NOTES:
        print(f"Synthesizing {freq}Hz simulation...")
        full_audio.append(generate_tuning_segment(freq, STRING_DURATION, FS))
        full_audio.append(silence)

    output = np.concatenate(full_audio)
    int16  = (output * 32767).clip(-32767, 32767).astype(np.int16)
    wavfile.write(FILENAME, FS, int16)
    print(f"Generated: {FILENAME}")

if __name__ == "__main__":
    main()
