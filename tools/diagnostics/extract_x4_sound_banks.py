"""Extract raw sequence entries from an X4 sound-RAM snapshot and area map.

The entries are preserved as bytes; their event encoding and associated tone
banks are not decoded. The manifest records the source hashes and addresses.
"""

import argparse
import hashlib
import json
from pathlib import Path
import struct


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def read_map(data: bytes, requested: int) -> list[dict]:
    offset = 0
    index = 0
    while offset < len(data):
        entries = []
        while offset < len(data) and data[offset] != 0xFF:
            if offset + 8 > len(data):
                raise ValueError("truncated sound-map entry")
            entry = data[offset:offset + 8]
            entries.append({
                "type": (entry[0] >> 4) & 7,
                "bank": entry[0] & 15,
                "address": int.from_bytes(entry[1:4], "big") & 0xFFFFF,
                "size": int.from_bytes(entry[4:8], "big") & 0xFFFFF,
            })
            offset += 8
        if offset >= len(data):
            raise ValueError("sound map lacks an end marker")
        offset += 1
        if index == requested:
            return entries
        index += 1
        if offset >= len(data) or data[offset] == 0xFF:
            break
    raise ValueError(f"sound map {requested} not found")


def extract(map_path: Path, ram_path: Path, output: Path, map_index: int) -> dict:
    map_data = map_path.read_bytes()
    ram = ram_path.read_bytes()
    if len(ram) != 0x80000:
        raise ValueError("expected a complete 512 KiB sound-RAM snapshot")
    if output.exists() and any(output.iterdir()):
        raise FileExistsError(f"refusing to overwrite non-empty directory: {output}")

    banks = []
    seen_banks = set()
    for entry in read_map(map_data, map_index):
        if entry["type"] != 1:
            continue
        if entry["bank"] in seen_banks:
            raise ValueError(f"duplicate sequence bank {entry['bank']}")
        seen_banks.add(entry["bank"])
        start, size = entry["address"], entry["size"]
        if size < 2 or start + size > len(ram):
            raise ValueError(f"sequence bank {entry['bank']} outside sound RAM")
        data = ram[start:start + size]
        count = struct.unpack_from(">H", data)[0]
        table_end = 2 + count * 4
        if count > 128 or table_end > size:
            raise ValueError(f"invalid sequence table in bank {entry['bank']}")
        offsets = [struct.unpack_from(">I", data, 2 + i * 4)[0] for i in range(count)]
        if any(value < table_end or value >= size for value in offsets) or any(
            left >= right for left, right in zip(offsets, offsets[1:])
        ):
            raise ValueError(f"unexpected sequence offsets in bank {entry['bank']}")
        slices = [data[offset:(offsets[i + 1] if i + 1 < count else size)]
                  for i, offset in enumerate(offsets)]
        banks.append((entry, data, offsets, slices))
    if not banks:
        raise ValueError("selected map contains no sequence banks")

    output.mkdir(parents=True, exist_ok=True)
    records = []
    for entry, data, offsets, slices in banks:
        bank_dir = output / f"bank-{entry['bank']:02d}"
        bank_dir.mkdir()
        (bank_dir / "bank.bin").write_bytes(data)
        sequences = []
        for number, (offset, item) in enumerate(zip(offsets, slices)):
            name = f"sequence-{number:03d}.bin"
            (bank_dir / name).write_bytes(item)
            sequences.append({"number": number, "offset": offset, "size": len(item),
                              "sha256": sha256(item), "file": name})
        records.append({**entry, "sha256": sha256(data), "count": len(slices),
                        "sequences": sequences})
    manifest = {"format": "X4 sound-RAM sequence-bank extraction", "map_index": map_index,
                "map_file": str(map_path), "map_sha256": sha256(map_data),
                "sound_ram": str(ram_path), "sound_ram_sha256": sha256(ram),
                "banks": records}
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("map_file", type=Path)
    parser.add_argument("sound_ram", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--map-index", type=int, default=0)
    args = parser.parse_args()
    if args.map_index < 0:
        parser.error("map index must be nonnegative")
    manifest = extract(args.map_file, args.sound_ram, args.output, args.map_index)
    count = sum(bank["count"] for bank in manifest["banks"])
    print(f"Extracted {count} raw sequences in {len(manifest['banks'])} banks to {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
