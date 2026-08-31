#!/usr/bin/env python3
"""Host-only compression sweep for the SX container.

This tool deliberately does not change the PS1 wire format.  It measures
candidate encoders and reversible MIPS-oriented transforms on a BIOS-sized
input, then verifies every candidate by decoding it again.

The region map is a static-analysis heuristic from the project notes.  The
BIOS is read transiently from --input; no input bytes are written to results.
"""

from __future__ import annotations

import argparse
import bz2
import concurrent.futures
import dataclasses
import json
import lzma
import os
from pathlib import Path
import time
import zlib


INPUT_SIZE = 0x80000
CONTAINER_HEADER = 28
BLOCK_HEADER = 16
HASH_BITS = 12
HASH_SIZE = 1 << HASH_BITS
LZSS_WINDOW = 4096
LZSS_MATCH_MAX = 18
DEFLATE_MATCH_MAX = 258

LENGTH_BASE = (
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258,
)
LENGTH_EXTRA = (
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0,
)
DISTANCE_BASE = (
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193,
    12289, 16385, 24577,
)
DISTANCE_EXTRA = (
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13,
)

# Static-analysis regions from ps1-bios-analysis.md.  The first region also
# contains the resident BIOS and shell code area before the first known TIM.
MIPSISH_END = 0x044C60
UI_TIM_END = 0x053D38
LOGO_TIM_START = 0x05FA40
LOGO_TIM_END = 0x062E30


@dataclasses.dataclass(frozen=True)
class Job:
    name: str
    mode: str
    codec: str
    transform: str
    block_size: int
    level: int = 9
    chain: int = 96
    policy: str = "uniform"
    region_codec: str = ""
    region_transform: str = ""


def _reverse_bits(value: int, count: int) -> int:
    result = 0
    for _ in range(count):
        result = (result << 1) | (value & 1)
        value >>= 1
    return result


class _BitWriter:
    def __init__(self) -> None:
        self.data = bytearray()
        self.bits = 0
        self.count = 0

    def put(self, value: int, count: int) -> None:
        self.bits |= value << self.count
        self.count += count
        while self.count >= 8:
            self.data.append(self.bits & 0xFF)
            self.bits >>= 8
            self.count -= 8

    def finish(self) -> bytes:
        if self.count:
            self.data.append(self.bits & 0xFF)
        return bytes(self.data)


def _fixed_symbol(writer: _BitWriter, symbol: int) -> None:
    if symbol <= 143:
        code, count = 0x30 + symbol, 8
    elif symbol <= 255:
        code, count = 0x190 + symbol - 144, 9
    elif symbol <= 279:
        code, count = symbol - 256, 7
    else:
        code, count = 0xC0 + symbol - 280, 8
    writer.put(_reverse_bits(code, count), count)


def fixed_deflate_encode(data: bytes, chain: int) -> bytes:
    """The same greedy fixed-Huffman encoder used by the PS1 target."""
    if not data:
        return b""
    heads = [-1] * HASH_SIZE
    previous = [-1] * len(data)

    def hash3(position: int) -> int:
        return (data[position] * 251 + data[position + 1] * 31 + data[position + 2]) & (HASH_SIZE - 1)

    def insert(position: int) -> None:
        if position + 2 >= len(data):
            return
        value = hash3(position)
        previous[position] = heads[value]
        heads[value] = position

    def find(position: int) -> tuple[int, int]:
        if position + 2 >= len(data):
            return 0, 0
        candidate = heads[hash3(position)]
        limit = min(len(data) - position, DEFLATE_MATCH_MAX)
        best_length = 0
        best_distance = 0
        for _ in range(chain):
            if candidate < 0:
                break
            distance = position - candidate
            if not distance or distance > 32768:
                break
            length = 0
            while length < limit and data[candidate + length] == data[position + length]:
                length += 1
            if length > best_length and length >= 3:
                best_length, best_distance = length, distance
                if length == limit:
                    break
            candidate = previous[candidate]
        return best_length, best_distance

    writer = _BitWriter()
    writer.put(1, 1)  # BFINAL
    writer.put(1, 2)  # fixed Huffman BTYPE
    position = 0
    while position < len(data):
        length, distance = find(position)
        if length >= 3:
            length_code = 0
            while length_code < 28 and length >= LENGTH_BASE[length_code + 1]:
                length_code += 1
            _fixed_symbol(writer, 257 + length_code)
            extra = LENGTH_EXTRA[length_code]
            if extra:
                writer.put(length - LENGTH_BASE[length_code], extra)

            distance_code = 0
            while distance_code < 29 and distance >= DISTANCE_BASE[distance_code + 1]:
                distance_code += 1
            writer.put(_reverse_bits(distance_code, 5), 5)
            extra = DISTANCE_EXTRA[distance_code]
            if extra:
                writer.put(distance - DISTANCE_BASE[distance_code], extra)
            for offset in range(length):
                insert(position + offset)
            position += length
        else:
            _fixed_symbol(writer, data[position])
            insert(position)
            position += 1
    _fixed_symbol(writer, 256)
    return writer.finish()


def lzss_encode(data: bytes) -> bytes:
    """The existing 4 KiB/18-byte SX LZSS encoder."""
    last = [-1] * HASH_SIZE
    output = bytearray()
    position = 0

    def hash3(at: int) -> int:
        return (data[at] * 251 + data[at + 1] * 17 + data[at + 2]) & (HASH_SIZE - 1)

    while position < len(data):
        flag_position = len(output)
        output.append(0)
        flags = 0
        for bit in range(8):
            if position >= len(data):
                break
            candidate = -1
            if position + 2 < len(data):
                value = hash3(position)
                candidate = last[value]
                last[value] = position
            match = 0
            if candidate >= 0 and position - candidate <= LZSS_WINDOW:
                limit = min(len(data) - position, LZSS_MATCH_MAX)
                while match < limit and data[candidate + match] == data[position + match]:
                    match += 1
            if match >= 3:
                distance = position - candidate - 1
                output.append(distance & 0xFF)
                output.append((distance >> 8) | ((match - 3) << 4))
                for offset in range(1, match):
                    if position + offset + 2 < len(data):
                        last[hash3(position + offset)] = position + offset
                position += match
            else:
                flags |= 1 << bit
                output.append(data[position])
                position += 1
        output[flag_position] = flags
    return bytes(output)


def lzss_decode(data: bytes, expected: int) -> bytes:
    output = bytearray()
    position = 0
    while position < len(data):
        flags = data[position]
        position += 1
        for bit in range(8):
            if position >= len(data):
                break
            if flags & (1 << bit):
                output.append(data[position])
                position += 1
            else:
                if position + 2 > len(data):
                    raise ValueError("truncated LZSS token")
                lo, hi = data[position], data[position + 1]
                position += 2
                distance = 1 + lo + ((hi & 0x0F) << 8)
                length = 3 + (hi >> 4)
                if distance > len(output):
                    raise ValueError("invalid LZSS distance")
                for _ in range(length):
                    output.append(output[-distance])
    if len(output) != expected:
        raise ValueError(f"LZSS size {len(output)} != {expected}")
    return bytes(output)


def _pack_bits(values: list[int], width: int) -> bytes:
    mask = (1 << width) - 1
    output = bytearray()
    accumulator = 0
    bits = 0
    for value in values:
        accumulator |= (value & mask) << bits
        bits += width
        while bits >= 8:
            output.append(accumulator & 0xFF)
            accumulator >>= 8
            bits -= 8
    if bits:
        output.append(accumulator & 0xFF)
    return bytes(output)


def _unpack_bits(data: bytes, count: int, width: int) -> list[int]:
    mask = (1 << width) - 1
    output: list[int] = []
    accumulator = 0
    bits = 0
    position = 0
    for _ in range(count):
        while bits < width:
            if position >= len(data):
                raise ValueError("truncated bit-plane")
            accumulator |= data[position] << bits
            position += 1
            bits += 8
        output.append(accumulator & mask)
        accumulator >>= width
        bits -= width
    return output


def _words(data: bytes) -> tuple[list[int], bytes]:
    full = len(data) & ~3
    return [int.from_bytes(data[i:i + 4], "little") for i in range(0, full, 4)], data[full:]


def _word_values(values: list[int], operation: str) -> list[int]:
    output: list[int] = []
    previous = 0
    for value in values:
        if operation == "xor":
            output.append(value ^ previous)
        elif operation == "delta":
            output.append((value - previous) & 0xFFFFFFFF)
        else:
            output.append(value)
        previous = value
    return output


def _word_inverse(values: list[int], operation: str) -> list[int]:
    output: list[int] = []
    previous = 0
    for value in values:
        if operation == "xor":
            restored = value ^ previous
        elif operation == "delta":
            restored = (value + previous) & 0xFFFFFFFF
        else:
            restored = value
        output.append(restored)
        previous = restored
    return output


def transform(data: bytes, name: str) -> bytes:
    if name == "plain":
        return data
    if name == "byte-delta":
        output = bytearray()
        previous = 0
        for value in data:
            output.append((value - previous) & 0xFF)
            previous = value
        return bytes(output)

    values, tail = _words(data)
    if name in ("mips-byteplane", "mips-xor-byteplane", "mips-delta-byteplane"):
        operation = "xor" if name == "mips-xor-byteplane" else "delta" if name == "mips-delta-byteplane" else "plain"
        mapped = _word_values(values, operation)
        count = len(mapped)
        output = bytearray(count * 4)
        for plane in range(4):
            for index, value in enumerate(mapped):
                output[plane * count + index] = (value >> (plane * 8)) & 0xFF
        return bytes(output) + tail
    if name in ("word-xor", "word-delta"):
        operation = "xor" if name == "word-xor" else "delta"
        mapped = _word_values(values, operation)
        return b"".join(value.to_bytes(4, "little") for value in mapped) + tail
    if name == "mips-bitplane":
        plane_bytes = (len(values) + 7) // 8
        output = bytearray(32 * plane_bytes)
        for bit in range(32):
            base = bit * plane_bytes
            for index, value in enumerate(values):
                if value & (1 << bit):
                    output[base + index // 8] |= 1 << (index & 7)
        return bytes(output) + tail
    if name == "mips-fields":
        opcodes = [value >> 26 for value in values]
        rest = [value & 0x03FFFFFF for value in values]
        return _pack_bits(opcodes, 6) + _pack_bits(rest, 26) + tail
    raise ValueError(f"unknown transform: {name}")


def inverse_transform(data: bytes, original_size: int, name: str) -> bytes:
    if name == "plain":
        return data
    if name == "byte-delta":
        output = bytearray()
        previous = 0
        for value in data[:original_size]:
            restored = (value + previous) & 0xFF
            output.append(restored)
            previous = restored
        return bytes(output)

    full = original_size & ~3
    count = full // 4
    tail_size = original_size - full
    tail = data[-tail_size:] if tail_size else b""
    core = data[:-tail_size] if tail_size else data
    if name in ("mips-byteplane", "mips-xor-byteplane", "mips-delta-byteplane"):
        if len(core) != count * 4:
            raise ValueError("invalid byte-plane size")
        mapped = []
        for index in range(count):
            value = 0
            for plane in range(4):
                value |= core[plane * count + index] << (plane * 8)
            mapped.append(value)
        operation = "xor" if name == "mips-xor-byteplane" else "delta" if name == "mips-delta-byteplane" else "plain"
        restored = _word_inverse(mapped, operation)
        return b"".join(value.to_bytes(4, "little") for value in restored) + tail
    if name in ("word-xor", "word-delta"):
        if len(core) != count * 4:
            raise ValueError("invalid word transform size")
        mapped = [int.from_bytes(core[i:i + 4], "little") for i in range(0, len(core), 4)]
        operation = "xor" if name == "word-xor" else "delta"
        restored = _word_inverse(mapped, operation)
        return b"".join(value.to_bytes(4, "little") for value in restored) + tail
    if name == "mips-bitplane":
        plane_bytes = (count + 7) // 8
        expected = 32 * plane_bytes
        if len(core) != expected:
            raise ValueError("invalid bit-plane size")
        restored = [0] * count
        for bit in range(32):
            base = bit * plane_bytes
            for index in range(count):
                if core[base + index // 8] & (1 << (index & 7)):
                    restored[index] |= 1 << bit
        return b"".join(value.to_bytes(4, "little") for value in restored) + tail
    if name == "mips-fields":
        op_len = (count * 6 + 7) // 8
        rest_len = (count * 26 + 7) // 8
        if len(core) != op_len + rest_len:
            raise ValueError("invalid field transform size")
        opcodes = _unpack_bits(core[:op_len], count, 6)
        rest = _unpack_bits(core[op_len:], count, 26)
        return b"".join(((op << 26) | low).to_bytes(4, "little") for op, low in zip(opcodes, rest)) + tail
    raise ValueError(f"unknown transform: {name}")


_LZMA_DICTS = {
    "lzma64": 1 << 16,
    "lzma128": 1 << 17,
    "lzma256": 1 << 18,
    "lzma512": 1 << 19,
    "lzma1024": 1 << 20,
    "lzma-mips64": 1 << 16,
    "lzma-mips128": 1 << 17,
    "lzma-mips256": 1 << 18,
    "lzma-mips512": 1 << 19,
    "lzma-mips1024": 1 << 20,
}


def _lzma_filters(codec: str, level: int) -> list[dict]:
    mips_aligned = codec.startswith("lzma-mips")
    return [{
        "id": lzma.FILTER_LZMA2,
        "dict_size": _LZMA_DICTS[codec],
        # MIPS code is four-byte aligned.  lp=2 models the literal position
        # modulo four; lc is reduced so lc+lp stays within LZMA's limit.
        "lc": 2 if mips_aligned else 3,
        "lp": 2 if mips_aligned else 0,
        "pb": 2,
        "mode": lzma.MODE_NORMAL,
        "nice_len": 64 if level <= 6 else 273,
        "mf": lzma.MF_BT4,
    }]


def encode_payload(data: bytes, codec: str, level: int, chain: int) -> bytes:
    if codec == "raw":
        return data
    if codec == "lzss":
        return lzss_encode(data)
    if codec == "fixed-custom":
        return fixed_deflate_encode(data, chain)
    if codec.startswith("zlib-"):
        strategy_name = codec[5:]
        strategy = {
            "dynamic": zlib.Z_DEFAULT_STRATEGY,
            "fixed": zlib.Z_FIXED,
            "rle": zlib.Z_RLE,
            "huffman": zlib.Z_HUFFMAN_ONLY,
        }[strategy_name]
        compressor = zlib.compressobj(level, zlib.DEFLATED, -15, 8, strategy)
        return compressor.compress(data) + compressor.flush()
    if codec == "bz2":
        return bz2.compress(data, compresslevel=level)
    if codec in _LZMA_DICTS:
        return lzma.compress(data, format=lzma.FORMAT_RAW, filters=_lzma_filters(codec, level))
    raise ValueError(f"unknown codec: {codec}")


def decode_payload(data: bytes, codec: str, expected: int) -> bytes:
    if codec == "raw":
        restored = data
    elif codec == "lzss":
        return lzss_decode(data, expected)
    elif codec in ("fixed-custom", "zlib-dynamic", "zlib-fixed", "zlib-rle", "zlib-huffman"):
        restored = zlib.decompress(data, -15)
    elif codec == "bz2":
        restored = bz2.decompress(data)
    elif codec in _LZMA_DICTS:
        restored = lzma.decompress(data, format=lzma.FORMAT_RAW, filters=_lzma_filters(codec, 9))
    else:
        raise ValueError(f"unknown codec: {codec}")
    if len(restored) != expected:
        raise ValueError(f"decoded size {len(restored)} != {expected}")
    return restored


def _codec_label(codec: str) -> str:
    if codec in _LZMA_DICTS:
        if codec.startswith("lzma-mips"):
            return f"LZMA2-MIPS-{codec[9:].upper()}K"
        return f"LZMA2-{codec[4:].upper()}K"
    return {
        "raw": "RAW",
        "lzss": "LZSS",
        "fixed-custom": "DFL-FIX",
        "zlib-dynamic": "DFL-DYN",
        "zlib-fixed": "Z-FIX",
        "zlib-rle": "Z-RLE",
        "zlib-huffman": "Z-HUFF",
        "bz2": "BZ2",
        "mixed-current": "MIXED",
    }[codec]


def _segments(data: bytes, policy: str) -> list[tuple[str, bytes]]:
    if policy != "regions":
        return [("uniform", data)]
    cuts = [0, MIPSISH_END, UI_TIM_END, LOGO_TIM_START, LOGO_TIM_END, len(data)]
    cuts = sorted(set(value for value in cuts if 0 <= value <= len(data)))
    segments: list[tuple[str, bytes]] = []
    for start, end in zip(cuts, cuts[1:]):
        if start >= end:
            continue
        if end <= MIPSISH_END or start < MIPSISH_END:
            region = "mips-ish"
        elif MIPSISH_END <= start < UI_TIM_END:
            region = "ui-tim"
        elif LOGO_TIM_START <= start < LOGO_TIM_END:
            region = "logo-tim"
        else:
            region = "resource-ish"
        segments.append((region, data[start:end]))
    return segments


def _area_segments(data: bytes) -> list[tuple[str, bytes]]:
    """Two large areas for a region-specific container candidate."""
    split = min(MIPSISH_END, len(data))
    return [("mips-area", data[:split]), ("resource-area", data[split:])]


def _region_codec(region: str, job: Job) -> tuple[str, str, int]:
    """Return codec, transform and level for a region job."""
    if job.policy not in ("regions", "areas"):
        return job.codec, job.transform, job.level
    if region in ("mips-ish", "mips-area"):
        return job.region_codec or "zlib-dynamic", job.region_transform or job.transform, job.level
    if region in ("ui-tim", "logo-tim", "resource-ish", "resource-area"):
        return job.codec, "plain", job.level
    return job.codec, "plain", job.level


def _verify_one(original: bytes, payload: bytes, codec: str, transform_name: str, chain: int) -> None:
    transformed = decode_payload(payload, codec, len(transform(original, transform_name)))
    restored = inverse_transform(transformed, len(original), transform_name)
    if restored != original:
        raise ValueError("round-trip mismatch")


def _run_job(data: bytes, job: Job) -> dict:
    started = time.perf_counter()
    payload_bytes = 0
    segments = 0
    codec_counts: dict[str, int] = {}

    try:
        if job.mode == "independent":
            for offset in range(0, len(data), job.block_size):
                original = data[offset:offset + job.block_size]
                region = "uniform"
                codec, transform_name, level = _region_codec(region, job)
                if job.codec == "mixed-current":
                    candidates = [
                        ("raw", original),
                        ("lzss", encode_payload(original, "lzss", level, job.chain)),
                        ("fixed-custom", encode_payload(original, "fixed-custom", level, job.chain)),
                    ]
                    codec, payload = min(candidates, key=lambda candidate: len(candidate[1]))
                    transform_name = "plain"
                else:
                    transformed = transform(original, transform_name)
                    payload = encode_payload(transformed, codec, level, job.chain)
                _verify_one(original, payload, codec, transform_name, job.chain)
                payload_bytes += len(payload)
                segments += 1
                label = _codec_label(codec)
                codec_counts[label] = codec_counts.get(label, 0) + 1
            total_bytes = CONTAINER_HEADER + segments * BLOCK_HEADER + payload_bytes
        elif job.mode == "stream":
            transformed = transform(data, job.transform)
            payload = encode_payload(transformed, job.codec, job.level, job.chain)
            _verify_one(data, payload, job.codec, job.transform, job.chain)
            payload_bytes = len(payload)
            segments = 1
            codec_counts[_codec_label(job.codec)] = 1
            # This is a lower-bound stream estimate; it is not current SX
            # independent-block framing.
            total_bytes = CONTAINER_HEADER + payload_bytes
        elif job.mode == "regions":
            for region, original in _segments(data, "regions"):
                codec, transform_name, level = _region_codec(region, job)
                transformed = transform(original, transform_name)
                payload = encode_payload(transformed, codec, level, job.chain)
                _verify_one(original, payload, codec, transform_name, job.chain)
                payload_bytes += len(payload)
                segments += 1
                label = f"{region}:{_codec_label(codec)}"
                codec_counts[label] = codec_counts.get(label, 0) + 1
            total_bytes = CONTAINER_HEADER + segments * BLOCK_HEADER + payload_bytes
        elif job.mode == "areas":
            for region, original in _area_segments(data):
                codec, transform_name, level = _region_codec(region, job)
                transformed = transform(original, transform_name)
                payload = encode_payload(transformed, codec, level, job.chain)
                _verify_one(original, payload, codec, transform_name, job.chain)
                payload_bytes += len(payload)
                segments += 1
                label = f"{region}:{_codec_label(codec)}"
                codec_counts[label] = codec_counts.get(label, 0) + 1
            total_bytes = CONTAINER_HEADER + segments * BLOCK_HEADER + payload_bytes
        else:
            raise ValueError(f"unknown mode {job.mode}")
        return {
            "name": job.name,
            "mode": job.mode,
            "policy": job.policy,
            "block_size": job.block_size,
            "codec": job.codec,
            "transform": job.transform,
            "level": job.level,
            "chain": job.chain,
            "region_codec": job.region_codec,
            "region_transform": job.region_transform,
            "segments": segments,
            "payload_bytes": payload_bytes,
            "total_bytes": total_bytes,
            "ratio": total_bytes / len(data),
            "encode_ms": (time.perf_counter() - started) * 1000.0,
            "verify": "PASS",
            "codec_counts": codec_counts,
        }
    except Exception as error:  # Keep all jobs visible in a parallel sweep.
        return {
            "name": job.name,
            "mode": job.mode,
            "policy": job.policy,
            "block_size": job.block_size,
            "codec": job.codec,
            "transform": job.transform,
            "level": job.level,
            "chain": job.chain,
            "region_codec": job.region_codec,
            "region_transform": job.region_transform,
            "verify": "FAIL",
            "error": str(error),
            "encode_ms": (time.perf_counter() - started) * 1000.0,
        }


def _run_path_job(path: str, job: Job) -> dict:
    return _run_job(Path(path).read_bytes(), job)


def make_jobs() -> list[Job]:
    jobs: list[Job] = []
    for block_size in (4096, 8192, 16384, 32768):
        jobs.append(Job(f"current-mixed-{block_size // 1024}K", "independent", "mixed-current", "plain", block_size, chain=96))
    for block_size in (16384, 32768):
        for chain in (24, 96, 192):
            jobs.append(Job(f"fixed-custom-{block_size // 1024}K-c{chain}", "independent", "fixed-custom", "plain", block_size, chain=chain))
        for codec in ("zlib-fixed", "zlib-dynamic", "zlib-rle", "zlib-huffman"):
            levels = (1, 6, 9) if codec == "zlib-dynamic" else (9,)
            for level in levels:
                jobs.append(Job(f"{codec}-plain-{block_size // 1024}K-l{level}", "independent", codec, "plain", block_size, level=level))
        for transform_name in (
            "mips-byteplane", "mips-xor-byteplane", "mips-delta-byteplane",
            "mips-bitplane", "mips-fields", "word-xor", "word-delta", "byte-delta",
        ):
            jobs.append(Job(f"zlib-dynamic-{transform_name}-{block_size // 1024}K-l9", "independent", "zlib-dynamic", transform_name, block_size, level=9))

    for transform_name in (
        "plain", "mips-byteplane", "mips-xor-byteplane", "mips-delta-byteplane",
        "mips-bitplane", "mips-fields", "word-xor", "word-delta", "byte-delta",
    ):
        for level in (6, 9):
            jobs.append(Job(f"zlib-dynamic-stream-{transform_name}-l{level}", "stream", "zlib-dynamic", transform_name, len(data_placeholder()), level=level))
    for codec, level in (("bz2", 9), ("lzma64", 6), ("lzma64", 9), ("lzma128", 9), ("lzma256", 9), ("lzma512", 9), ("lzma1024", 9)):
        jobs.append(Job(f"{codec}-stream-l{level}", "stream", codec, "plain", len(data_placeholder()), level=level))

    # Exact static-analysis region segmentation.  The MIPS-ish region gets a
    # reversible transform; TIM/font/table regions stay byte order and use the
    # selected resource backend.
    for codec, level in (("zlib-dynamic", 6), ("zlib-dynamic", 9), ("zlib-rle", 9), ("bz2", 9), ("lzma64", 9)):
        for transform_name in ("mips-byteplane", "mips-xor-byteplane", "mips-fields", "plain"):
            jobs.append(Job(f"regions-mips-{transform_name}-resource-{codec}-l{level}", "regions", codec, transform_name, len(data_placeholder()), level=level, policy="regions"))

    # Two-area candidates keep one MIPS stream and one resource stream.  This
    # makes the cost of area-specific codecs explicit and is closer to a
    # future versioned container than the stream-only lower bounds above.
    for mips_codec in ("zlib-dynamic", "bz2", "lzma64", "lzma128", "lzma256", "lzma512", "lzma1024", "lzma-mips64", "lzma-mips128", "lzma-mips256", "lzma-mips512", "lzma-mips1024"):
        for resource_codec in ("zlib-dynamic", "bz2", "lzma64", "lzma128", "lzma256", "lzma512", "lzma1024"):
            jobs.append(Job(
                f"areas-mips-{mips_codec}-resource-{resource_codec}-plain",
                "areas", resource_codec, "plain", len(data_placeholder()), level=9,
                policy="areas", region_codec=mips_codec,
            ))
    for transform_name in ("mips-byteplane", "mips-xor-byteplane", "mips-fields"):
        for mips_codec in ("zlib-dynamic", "lzma64", "lzma256"):
            jobs.append(Job(
                f"areas-mips-{mips_codec}-{transform_name}-resource-lzma256",
                "areas", "lzma256", "plain", len(data_placeholder()), level=9,
                policy="areas", region_codec=mips_codec, region_transform=transform_name,
            ))
    return jobs


def data_placeholder() -> bytes:
    # Job carries a block-size-like field even for stream/region modes.  The
    # value is replaced by the input size in main; keeping construction pure
    # makes jobs easy to pickle for ProcessPoolExecutor.
    return b""


def _replace_stream_sizes(jobs: list[Job], size: int) -> list[Job]:
    return [dataclasses.replace(job, block_size=size) if job.mode != "independent" else job for job in jobs]


def _print_table(summary: dict) -> None:
    print(f"input={summary['input_bytes']} bytes jobs={summary['job_count']} workers={summary['workers']}")
    print("total_bytes  encode_ms  verify  mode         name")
    for result in summary["results"]:
        if result["verify"] == "PASS":
            print(f"{result['total_bytes']:11d} {result['encode_ms']:10.1f}  {result['verify']:6s}  {result['mode']:12s} {result['name']}")
        else:
            print(f"{'-':>11s} {result['encode_ms']:10.1f}  {result['verify']:6s}  {result['mode']:12s} {result['name']} ({result['error']})")
    passed = [result for result in summary["results"] if result["verify"] == "PASS"]
    if passed:
        best = min(passed, key=lambda result: result["total_bytes"])
        print(f"BEST {best['total_bytes']} bytes {best['name']}")


def run_self_test() -> None:
    data = bytearray()
    for index in range(32768):
        opcode = (index % 16) << 26
        registers = ((index * 3) & 0x1F) << 21
        immediate = (index * 37) & 0xFFFF
        data.extend((opcode | registers | immediate).to_bytes(4, "little"))
    source = bytes(data)
    transforms = ("plain", "mips-byteplane", "mips-xor-byteplane", "mips-delta-byteplane", "mips-bitplane", "mips-fields", "word-xor", "word-delta", "byte-delta")
    for transform_name in transforms:
        transformed = transform(source, transform_name)
        assert inverse_transform(transformed, len(source), transform_name) == source, transform_name
        for codec in ("raw", "lzss", "fixed-custom", "zlib-dynamic", "zlib-fixed", "zlib-rle", "zlib-huffman", "bz2", "lzma64", "lzma128", "lzma256", "lzma-mips256"):
            payload = encode_payload(transformed, codec, 9, 96)
            restored = decode_payload(payload, codec, len(transformed))
            assert restored == transformed, (transform_name, codec)
    for job in make_jobs()[:12]:
        result = _run_job(source, dataclasses.replace(job, block_size=16384 if job.mode == "independent" else len(source)))
        assert result["verify"] == "PASS", result
    print("PASS codec_sweep self-test")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", help="transient input file; expected to be 512 KiB for a BIOS run")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 1)
    parser.add_argument("--json-out", help="write aggregate results to this path")
    parser.add_argument("--format", choices=("table", "json"), default="table")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()

    if args.self_test:
        run_self_test()
        return 0
    if not args.input:
        parser.error("--input is required unless --self-test is used")
    input_path = Path(args.input)
    input_size = input_path.stat().st_size
    if input_size != INPUT_SIZE:
        parser.error(f"input must be 524288 bytes, got {input_size}")
    jobs = _replace_stream_sizes(make_jobs(), input_size)
    worker_count = max(1, min(args.jobs, len(jobs)))
    with concurrent.futures.ProcessPoolExecutor(max_workers=worker_count) as pool:
        futures = [pool.submit(_run_path_job, str(input_path), job) for job in jobs]
        results = [future.result() for future in concurrent.futures.as_completed(futures)]
    results.sort(key=lambda result: (result.get("verify") != "PASS", result.get("total_bytes", 1 << 60), result["name"]))
    summary = {
        "input_bytes": input_size,
        "job_count": len(jobs),
        "workers": worker_count,
        "results": results,
    }
    if args.json_out:
        Path(args.json_out).write_text(json.dumps(summary, ensure_ascii=False, indent=2) + "\n")
    if args.format == "json":
        print(json.dumps(summary, ensure_ascii=False, indent=2))
    else:
        _print_table(summary)
    return 0 if all(result["verify"] == "PASS" for result in results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
