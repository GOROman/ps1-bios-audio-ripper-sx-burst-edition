#!/usr/bin/env python3
"""Generate SX's 300-baud level sequence and nostalgic ASCII FSK header."""

import argparse
import binascii
import math
import struct
import wave

RATE = 44100
BAUD = 300
SYMBOL_SAMPLES = RATE // BAUD
SPACE_HZ = 1200
MARK_HZ = 2400
LEVEL_SECONDS = 3
SYNC_BITS = 96


def header_text(stored_size=524288, image_crc=0):
    body = (
        "PS1SX,1\r\n"
        f"NAME=PS1BIOS\r\nSIZE=524288\r\nSTORED={stored_size}\r\n"
        f"CRC32={image_crc:08X}\r\nBLOCK=16384\r\nBLOCKS=32\r\nCODEC=MIXED\r\n"
    ).encode("ascii")
    return body + f"HDRCRC={binascii.crc_hqx(body, 0xffff):04X}\r\nREADY\r\n".encode("ascii")


def byte_bits(value):
    return [0] + [(value >> bit) & 1 for bit in range(8)] + [1]


def make_pcm(header):
    bits = [1] * (BAUD * LEVEL_SECONDS)
    bits += [index & 1 for index in range(SYNC_BITS)]
    bits += [1] * 30
    for value in header:
        bits.extend(byte_bits(value))
    phase = 0.0
    frames = bytearray()
    for bit in bits:
        frequency = MARK_HZ if bit else SPACE_HZ
        step = 2.0 * math.pi * frequency / RATE
        for _ in range(SYMBOL_SAMPLES):
            sample = int(math.sin(phase) * 0.45 * 32767)
            frames += struct.pack("<hh", sample, sample)
            phase = (phase + step) % (2.0 * math.pi)
    return bytes(frames), len(bits)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("output")
    parser.add_argument("--stored-size", type=int, default=524288)
    parser.add_argument("--image-crc", type=lambda value: int(value, 16), default=0)
    args = parser.parse_args()
    header = header_text(args.stored_size, args.image_crc)
    pcm, symbols = make_pcm(header)
    with wave.open(args.output, "wb") as wav:
        wav.setparams((2, 2, RATE, symbols * SYMBOL_SAMPLES, "NONE", "not compressed"))
        wav.writeframes(pcm)
    print(header.decode("ascii"), end="")
    print(f"wrote {args.output}: {symbols / BAUD:.2f}s, {symbols} symbols")


if __name__ == "__main__":
    main()
