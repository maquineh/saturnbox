"""Validate the iteration-41 slave sprite job against its captured RAM and PL00.ARC.

Usage: python validate_x4_sprite_job.py ram_high.bin PL00.ARC decoded.bin
Decoding here is independent of the SH-2 interpreter/JIT and uses only file bytes.
"""

import argparse
import hashlib
import json
from pathlib import Path
import struct


def decode_words(data, offset):
    words = []

    def word():
        nonlocal offset
        value, = struct.unpack_from(">H", data, offset)
        offset += 2
        return value

    while len(words) < 0x8000:
        flags = word()
        for bit in range(15, -1, -1):
            if not flags & (1 << bit):
                words.append(word())
                continue
            token = word()
            count, distance = token >> 11, token & 0x7FF
            if count == 0:
                count = word()
            if count == distance == 0:
                return b"".join(struct.pack(">H", w) for w in words), offset
            if distance > len(words) or len(words) + count > 0x8000:
                raise ValueError("Invalid backreference or oversized sprite job")
            for _ in range(count):
                words.append(words[-distance] if distance else 0)
    raise ValueError("Sprite job has no end marker within 64 KiB")


def resident_type2(ram, archive):
    payload = 0x800
    for header in range(0, 0x800, 16):
        kind, count, first, last, mode = struct.unpack_from(">IIHHI", archive, header)
        if kind == 255:
            raise ValueError("Archive has no type-2 payload")
        size = first if count == 1 else first + (count - 2) * 0x800 + last
        if kind == 2:
            if mode != 0:
                raise ValueError("Expected an untransformed type-2 payload")
            break
        payload += size
    else:
        raise ValueError("Archive has no terminator or type-2 payload")
    block = archive[payload:payload + size]
    base = ram.find(block)
    if len(block) != size or base < 0 or ram.find(block, base + 1) != -1:
        raise ValueError("Type-2 payload is not uniquely resident in captured RAM")
    return payload, size, base


def validate(ram_path, archive_path, output_path):
    ram, archive, actual = (p.read_bytes() for p in (ram_path, archive_path, output_path))
    if len(ram) != 0x100000:
        raise ValueError("Expected a 1 MiB high-RAM snapshot")
    callback, = struct.unpack_from(">I", ram, 0x604D0)
    source, destination = struct.unpack_from(">II", ram, 0x604D8)
    if callback != 0x06016F98 or destination != 0x06003000:
        raise ValueError("Snapshot does not contain the expected pending sprite job")
    payload, size, base = resident_type2(ram, archive)
    relative = source - (0x06000000 + base)
    if not 0 <= relative < size:
        raise ValueError("Job source is outside the resident type-2 payload")
    start = payload + relative
    expected, end = decode_words(archive, start)
    if end > payload + size:
        raise ValueError("Compressed job crosses the type-2 payload boundary")
    source_offset = source - 0x06000000
    if ram[source_offset:source_offset + end - start] != archive[start:end]:
        raise ValueError("Compressed source bytes differ from the archive")
    if actual != expected:
        raise ValueError("Slave output differs from independent file decoding")
    return {
        "callback": hex(callback), "archive_type": 2,
        "resident_payload_base": hex(0x06000000 + base),
        "compressed_source": hex(source), "archive_start": hex(start),
        "archive_end_exclusive": hex(end), "compressed_bytes": end - start,
        "destination": hex(destination), "output_bytes": len(actual),
        "all_source_bytes_match": True, "all_output_bytes_match": True,
        "output_sha256": hashlib.sha256(actual).hexdigest(),
        "archive_sha256": hashlib.sha256(archive).hexdigest(),
        "snapshot_sha256": hashlib.sha256(ram).hexdigest(),
    }


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("ram", type=Path)
    parser.add_argument("archive", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    print(json.dumps(validate(args.ram, args.archive, args.output), indent=2))
