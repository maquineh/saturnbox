"""Extract typed payloads from already-extracted Rockman X4 ARC files.

Payload bytes are copied as stored; this tool does not decompress or interpret
the type-specific data. The manifest records source offsets, header fields and
SHA-256 hashes so later decoders can be checked against the disc files.

Usage:
  python tools/diagnostics/extract_x4_arc.py
  python tools/diagnostics/extract_x4_arc.py build/rockman --output build/rockman/arc-extracted
"""

import argparse
import hashlib
import json
from pathlib import Path
import struct


HEADER_SIZE = 0x800
HEADER_ENTRY_SIZE = 16
CHUNK_SIZE = 0x800
TERMINATOR_KIND = 0xFF


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def parse_archive(path: Path) -> tuple[bytes, list[dict], int]:
    data = path.read_bytes()
    if len(data) < HEADER_SIZE:
        raise ValueError(f"{path}: shorter than the 0x800-byte type table")

    entries = []
    payload_offset = HEADER_SIZE
    terminated = False
    for header_offset in range(0, HEADER_SIZE, HEADER_ENTRY_SIZE):
        kind, count, first_size, last_size, mode = struct.unpack_from(
            ">IIHHI", data, header_offset
        )
        if kind == TERMINATOR_KIND:
            terminated = True
            break
        if count == 0:
            raise ValueError(f"{path}: type {kind} at {header_offset:#x} has zero units")

        payload_size = (first_size if count == 1 else
                        first_size + (count - 2) * CHUNK_SIZE + last_size)
        payload_end = payload_offset + payload_size
        if payload_end > len(data):
            raise ValueError(
                f"{path}: type {kind} at {header_offset:#x} ends at {payload_end:#x}, "
                f"past file size {len(data):#x}"
            )
        entries.append({
            "index": len(entries),
            "header_offset": header_offset,
            "kind": kind,
            "unit_count": count,
            "first_unit_bytes": first_size,
            "last_unit_bytes": last_size,
            "mode": mode,
            "payload_offset": payload_offset,
            "payload_size": payload_size,
            "payload_end_exclusive": payload_end,
            "payload": data[payload_offset:payload_end],
        })
        payload_offset = payload_end

    if not terminated:
        raise ValueError(f"{path}: no type-table terminator in 0x800 bytes")
    return data, entries, payload_offset


def extract(input_dir: Path, output_dir: Path) -> dict:
    archives = sorted(input_dir.glob("*.ARC"), key=lambda p: p.name.casefold())
    if not archives:
        raise ValueError(f"No .ARC files found in {input_dir}")
    if output_dir.exists() and any(output_dir.iterdir()):
        raise FileExistsError(f"Refusing to overwrite non-empty output directory: {output_dir}")
    output_dir.mkdir(parents=True, exist_ok=True)

    records = []
    total_payload_bytes = 0
    total_trailing_bytes = 0
    for archive_path in archives:
        data, entries, trailing_offset = parse_archive(archive_path)
        archive_dir = output_dir / archive_path.stem
        archive_dir.mkdir(parents=True, exist_ok=True)
        entry_records = []
        for entry in entries:
            filename = f"{entry['index']:03d}-type-{entry['kind']:03d}.bin"
            relative_path = Path(archive_path.stem) / filename
            payload = entry.pop("payload")
            (output_dir / relative_path).write_bytes(payload)
            entry_records.append({
                **entry,
                "output": relative_path.as_posix(),
                "sha256": sha256(payload),
            })
            total_payload_bytes += len(payload)

        trailing = data[trailing_offset:]
        trailing_record = None
        if trailing:
            relative_path = Path(archive_path.stem) / "trailing.bin"
            (output_dir / relative_path).write_bytes(trailing)
            trailing_record = {
                "offset": trailing_offset,
                "size": len(trailing),
                "output": relative_path.as_posix(),
                "sha256": sha256(trailing),
            }
            total_trailing_bytes += len(trailing)

        records.append({
            "archive": archive_path.name,
            "size": len(data),
            "sha256": sha256(data),
            "typed_payload_bytes": sum(row["payload_size"] for row in entries),
            "trailing_bytes": trailing_record,
            "entries": entry_records,
        })

    manifest = {
        "format": "Rockman X4 typed ARC payload extraction",
        "schema_version": 1,
        "header_size": HEADER_SIZE,
        "header_entry_size": HEADER_ENTRY_SIZE,
        "unit_size": CHUNK_SIZE,
        "archives": len(records),
        "entries": sum(len(row["entries"]) for row in records),
        "typed_payload_bytes": total_payload_bytes,
        "trailing_bytes": total_trailing_bytes,
        "source_directory": str(input_dir),
        "files": records,
    }
    (output_dir / "manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
    )
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input_dir", nargs="?", type=Path, default=Path("build/rockman"))
    parser.add_argument("--output", type=Path, default=Path("build/rockman/arc-extracted"))
    args = parser.parse_args()
    manifest = extract(args.input_dir, args.output)
    print(
        f"Extracted {manifest['entries']} typed payloads from {manifest['archives']} ARC files "
        f"({manifest['typed_payload_bytes']} payload bytes; "
        f"{manifest['trailing_bytes']} trailing bytes) to {args.output}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
