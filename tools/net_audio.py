#!/usr/bin/env python3
"""
net_audio.py — stream PC microphone to ESP32 over UDP

Usage:
    python tools/net_audio.py <esp32-ip> [--rate 44100] [--port 1234] [--device N]

The ESP32 expects raw little-endian int16 mono PCM on UDP port 1234.
Each UDP packet carries one chunk of samples (default 512 samples = 1024 bytes).
"""

import argparse
import socket
import struct
import sys

try:
    import sounddevice as sd
    import numpy as np
except ImportError:
    sys.exit("Install dependencies:  pip install sounddevice numpy")


def list_devices():
    print(sd.query_devices())


def stream(host, port, rate, chunk, device):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    dest = (host, port)

    print(f"Streaming mic → {host}:{port}  {rate} Hz  chunk={chunk} samples")
    print("Ctrl-C to stop.")

    def callback(indata, frames, time, status):
        if status:
            print(f"[sounddevice] {status}", file=sys.stderr)
        # indata is float32, shape (frames, channels) — take channel 0, convert to int16
        mono = indata[:, 0]
        pcm = (mono * 32767).astype(np.int16)
        sock.sendto(pcm.tobytes(), dest)

    with sd.InputStream(
        samplerate=rate,
        channels=1,
        dtype="float32",
        blocksize=chunk,
        device=device,
        callback=callback,
    ):
        try:
            while True:
                sd.sleep(1000)
        except KeyboardInterrupt:
            pass

    sock.close()
    print("\nStopped.")


def main():
    parser = argparse.ArgumentParser(description="Stream mic audio to ESP32 over UDP")
    parser.add_argument("host", nargs="?", help="ESP32 IP address")
    parser.add_argument("--port",   type=int, default=1234,  help="UDP port (default 1234)")
    parser.add_argument("--rate",   type=int, default=44100, help="Sample rate (default 44100)")
    parser.add_argument("--chunk",  type=int, default=512,   help="Samples per UDP packet (default 512)")
    parser.add_argument("--device", type=int, default=None,  help="Input device index (omit for default)")
    parser.add_argument("--list",   action="store_true",     help="List audio devices and exit")
    args = parser.parse_args()

    if args.list:
        list_devices()
        return

    if not args.host:
        parser.error("host is required (use --list to find your ESP32's IP)")

    # Accept "ip:port" as a convenience
    host = args.host
    port = args.port
    if ":" in host:
        host, p = host.rsplit(":", 1)
        port = int(p)

    stream(host, port, args.rate, args.chunk, args.device)


if __name__ == "__main__":
    main()
