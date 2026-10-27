"""Verify X4's captured RAM and last VDP1 sprite upload against PL00.ARC.

Usage: python validate_x4_sprite_capture.py CAPTURE_DIRECTORY PL00.ARC
RAM and VDP1 may contain different poses; each must match a complete frame.
"""

import argparse
import csv
import hashlib
import json
from pathlib import Path
import re
import struct

from validate_x4_sprite_job import decode_words, resident_type2


def validate(directory, archive_path):
    diagnostics = directory / "video-diagnostics"
    snapshots = list(diagnostics.glob("frame_*_vdp1_vram.bin"))
    if len(snapshots) != 1:
        raise ValueError("Expected one synchronized VDP1 snapshot")
    prefix = snapshots[0].name.removesuffix("vdp1_vram.bin")
    ram = (diagnostics / "ram_high.bin").read_bytes()
    archive = archive_path.read_bytes()
    vram = snapshots[0].read_bytes()
    payload, size, base = resident_type2(ram, archive)
    callback, = struct.unpack_from(">I", ram, 0x604D0)
    source, destination = struct.unpack_from(">II", ram, 0x604D8)
    if callback != 0 or destination != 0x06003000:
        raise ValueError("Sprite worker is not idle with the expected destination")
    source_offset = payload + source - (0x06000000 + base)
    if not payload <= source_offset < payload + size:
        raise ValueError("RAM job source is outside PL00 type 2")
    decoded, end = decode_words(archive, source_offset)
    if ram[0x3000:0x3000 + len(decoded)] != decoded:
        raise ValueError("RAM sprite differs from independent archive decoding")
    if ram[source - 0x06000000:source - 0x06000000 + end - source_offset] != archive[source_offset:end]:
        raise ValueError("Compressed RAM source differs from the archive")

    with (diagnostics / (prefix + "vdp1-memory-writes.csv")).open(newline="") as stream:
        writes = [r for r in csv.DictReader(stream)
                  if 0x60000 <= int(r["offset_hex"], 16) < 0x61000]
    last_step = max(int(r["step"]) for r in writes)
    uploaded = {}
    for row in writes:
        if int(row["step"]) != last_step:
            continue
        offset = int(row["offset_hex"], 16)
        value = int(row["value_hex"], 16).to_bytes(int(row["width"]), "big")
        uploaded.update((offset + i, byte) for i, byte in enumerate(value))
    first, last = min(uploaded), max(uploaded) + 1
    if first != 0x60000 or len(uploaded) != last - first:
        raise ValueError("Last sprite upload is not completely retained")
    bus_prefix = bytes(uploaded[a] for a in range(first, last))
    if vram[first:last] != bus_prefix:
        raise ValueError("Captured VDP1 bytes differ from recorded bus writes")
    # The video observer records only 0x60000..0x602FF. Obtain the complete
    # transfer size from the matching SCU event, not from that trace window.
    transfers = []
    for line in (directory / "scu-dma-vdp1-texture-transfers.txt").read_text().splitlines():
        fields = dict(re.findall(r"(\w+)=([^\s]+)", line))
        if (fields.get("step") == str(last_step)
                and int(fields["source"], 16) == 0x06003000
                and int(fields["destination"], 16) & 0x1FFFFFFF == 0x05C60000):
            transfers.append(fields)
    if len(transfers) != 1 or transfers[0].get("complete") != "1":
        raise ValueError("Last sprite bus writes have no unique completed SCU transfer")
    count = int(transfers[0]["count"], 16)
    if not len(bus_prefix) <= count <= 0x1000:
        raise ValueError("Unexpected complete sprite upload size")
    actual = vram[first:first + count]

    # PL00's first animation directory and bank, observed by the guest builder.
    # Its packed descriptors encode output size in bits 31:20 (128-byte tiles)
    # and a source offset relative to the bank in bits 19:0 (0x0601B0D0..E2).
    directory_base = payload + struct.unpack_from(">I", archive, payload)[0]
    bank = directory_base + struct.unpack_from(">I", archive, directory_base)[0]
    table_end = bank + (struct.unpack_from(">I", archive, bank)[0] & 0xFFFFF)
    if not payload <= bank < table_end < payload + size or (table_end - bank) % 4:
        raise ValueError("Invalid animation table bounds")
    matches = []
    for entry in range(bank, table_end, 4):
        descriptor, = struct.unpack_from(">I", archive, entry)
        if (descriptor & 0xFFF00000) >> 13 != len(actual):
            continue
        offset = bank + (descriptor & 0xFFFFF)
        expected, consumed = decode_words(archive, offset)
        if expected == actual:
            matches.append({"index": (entry - bank) // 4,
                            "archive_offset": hex(offset),
                            "compressed_bytes": consumed - offset})
    if not matches:
        raise ValueError("VDP1 upload does not match any complete PL00 sprite pose")
    return {"frame_prefix": prefix, "mailbox_idle": True,
            "ram_source": hex(source), "ram_archive_offset": hex(source_offset),
            "ram_decoded_bytes": len(decoded), "ram_matches_independent_decode": True,
            "last_upload_step": last_step, "vdp1_offset": hex(first),
            "uploaded_bytes": len(actual), "recorded_bus_prefix_bytes": len(bus_prefix),
            "vdp1_matches_recorded_bus_prefix": True,
            "vdp1_matches_current_ram": actual == decoded,
            "vdp1_complete_pose_matches": matches,
            "texture_sha256": hashlib.sha256(actual).hexdigest()}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    parser.add_argument("archive", type=Path)
    args = parser.parse_args()
    print(json.dumps(validate(args.capture, args.archive), indent=2))
