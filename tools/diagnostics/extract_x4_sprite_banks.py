"""Decode the type-2 animation banks in Rockman X4 PL00/PL01/PL02 archives.

The decoder is the same word-oriented codec independently checked against the
slave sprite worker and VDP1 uploads. This tool exports each distinct source
pose once and records every descriptor that references it.

Usage:
  python tools/diagnostics/extract_x4_sprite_banks.py
  python tools/diagnostics/extract_x4_sprite_banks.py build/rockman --output build/rockman/sprite-banks
"""

import argparse
import hashlib
import json
from pathlib import Path
import struct

from validate_x4_sprite_job import decode_words


HEADER_SIZE = 0x800
HEADER_ENTRY_SIZE = 16
CHUNK_SIZE = 0x800
TERMINATOR_KIND = 0xFF
ARCHIVE_NAMES = ("PL00.ARC", "PL01.ARC", "PL02.ARC")


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def checked_u32(data: bytes, offset: int, limit: int, what: str) -> int:
    if offset < 0 or offset + 4 > limit:
        raise ValueError(f"{what} at {offset:#x} is outside its containing payload")
    return struct.unpack_from(">I", data, offset)[0]


def find_type2(data: bytes, path: Path) -> tuple[int, int, int]:
    if len(data) < HEADER_SIZE:
        raise ValueError(f"{path}: shorter than the 0x800-byte type table")
    payload_offset = HEADER_SIZE
    for header_offset in range(0, HEADER_SIZE, HEADER_ENTRY_SIZE):
        kind, count, first, last, mode = struct.unpack_from(
            ">IIHHI", data, header_offset
        )
        if kind == TERMINATOR_KIND:
            break
        if count == 0:
            raise ValueError(f"{path}: type {kind} at {header_offset:#x} has zero units")
        size = first if count == 1 else first + (count - 2) * CHUNK_SIZE + last
        end = payload_offset + size
        if end > len(data):
            raise ValueError(f"{path}: type {kind} ends past EOF")
        if kind == 2:
            if mode != 0:
                raise ValueError(f"{path}: type-2 mode {mode} is not raw payload mode")
            return payload_offset, size, end
        payload_offset = end
    raise ValueError(f"{path}: no type-2 payload before the type-table terminator")


def decode_archive(path: Path) -> tuple[dict, list[tuple[str, bytes]]]:
    data = path.read_bytes()
    payload_offset, payload_size, payload_end = find_type2(data, path)
    payload = data[payload_offset:payload_end]

    directory_offset = checked_u32(payload, 0, len(payload), "animation directory pointer")
    if directory_offset >= len(payload):
        raise ValueError(f"{path}: animation directory pointer {directory_offset:#x} is outside type 2")
    bank_offset = directory_offset + checked_u32(
        payload, directory_offset, len(payload), "animation bank pointer"
    )
    if bank_offset < directory_offset or bank_offset + 4 > len(payload):
        raise ValueError(f"{path}: animation bank pointer is outside type 2")
    table_length = checked_u32(payload, bank_offset, len(payload), "animation table length") & 0xFFFFF
    table_end = bank_offset + table_length
    if table_end < bank_offset + 4 or table_end > len(payload) or (table_end - bank_offset) % 4:
        raise ValueError(f"{path}: invalid animation descriptor table bounds")

    descriptors = []
    decoded_by_source = {}
    for entry_offset in range(bank_offset, table_end, 4):
        descriptor = struct.unpack_from(">I", payload, entry_offset)[0]
        index = (entry_offset - bank_offset) // 4
        expected_size = (descriptor & 0xFFF00000) >> 13
        relative_source = descriptor & 0xFFFFF
        source_offset = bank_offset + relative_source
        if source_offset < bank_offset or source_offset >= len(payload):
            raise ValueError(f"{path}: descriptor {index} source {source_offset:#x} is outside type 2")
        if source_offset not in decoded_by_source:
            try:
                decoded, consumed = decode_words(payload, source_offset)
            except (struct.error, ValueError) as error:
                raise ValueError(f"{path}: invalid pose stream for descriptor {index}: {error}") from error
            if consumed > len(payload):
                raise ValueError(f"{path}: pose stream for descriptor {index} crosses type-2 boundary")
            decoded_by_source[source_offset] = (decoded, consumed)
        decoded, consumed = decoded_by_source[source_offset]
        if expected_size != len(decoded):
            raise ValueError(
                f"{path}: descriptor {index} declares {expected_size:#x} bytes, "
                f"but source {source_offset:#x} decodes to {len(decoded):#x}"
            )
        descriptors.append({
            "index": index,
            "descriptor": f"0x{descriptor:08X}",
            "decoded_bytes": len(decoded),
            "source_offset_in_type2": source_offset,
            "source_offset_in_archive": payload_offset + source_offset,
            "compressed_bytes": consumed - source_offset,
            "decoded_sha256": sha256(decoded),
        })

    exports = []
    for ordinal, source_offset in enumerate(sorted(decoded_by_source)):
        decoded, consumed = decoded_by_source[source_offset]
        name = f"pose_{ordinal:03d}-src-{source_offset:06X}.bin"
        exports.append((name, decoded))
    manifest = {
        "archive": path.name,
        "archive_sha256": sha256(data),
        "type2_payload_offset": payload_offset,
        "type2_payload_size": payload_size,
        "animation_directory_offset_in_type2": directory_offset,
        "descriptor_table_offset_in_type2": bank_offset,
        "descriptor_table_end_in_type2": table_end,
        "descriptor_count": len(descriptors),
        "unique_source_count": len(exports),
        "decoded_bytes": sum(len(blob) for _, blob in exports),
        "descriptors": descriptors,
        "poses": [
            {
                "file": name,
                "source_offset_in_type2": source_offset,
                "decoded_bytes": len(blob),
                "sha256": sha256(blob),
                "descriptor_indices": [
                    row["index"] for row in descriptors
                    if row["source_offset_in_type2"] == source_offset
                ],
            }
            for (name, blob), source_offset in zip(exports, sorted(decoded_by_source))
        ],
    }
    return manifest, exports


def extract(input_dir: Path, output_dir: Path) -> dict:
    if output_dir.exists() and any(output_dir.iterdir()):
        raise FileExistsError(f"Refusing to overwrite non-empty output directory: {output_dir}")
    sources = [input_dir / name for name in ARCHIVE_NAMES]
    missing = [path.name for path in sources if not path.is_file()]
    if missing:
        raise FileNotFoundError(f"Missing required archives in {input_dir}: {', '.join(missing)}")

    # Parse/decode everything before creating the destination, avoiding a partial result.
    results = [(path, *decode_archive(path)) for path in sources]
    output_dir.mkdir(parents=True, exist_ok=True)
    archives = []
    for path, manifest, exports in results:
        archive_dir = output_dir / path.stem
        archive_dir.mkdir()
        for filename, blob in exports:
            (archive_dir / filename).write_bytes(blob)
        manifest["poses"] = [
            {**pose, "file": f"{path.stem}/{pose['file']}"}
            for pose in manifest["poses"]
        ]
        archives.append(manifest)

    result = {
        "format": "Rockman X4 decoded PL animation banks",
        "schema_version": 1,
        "archives": archives,
        "descriptor_count": sum(row["descriptor_count"] for row in archives),
        "unique_pose_count": sum(row["unique_source_count"] for row in archives),
        "decoded_bytes": sum(row["decoded_bytes"] for row in archives),
    }
    (output_dir / "manifest.json").write_text(
        json.dumps(result, indent=2) + "\n", encoding="utf-8"
    )
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input_dir", nargs="?", type=Path, default=Path("build/rockman"))
    parser.add_argument("--output", type=Path, default=Path("build/rockman/sprite-banks"))
    args = parser.parse_args()
    manifest = extract(args.input_dir, args.output)
    print(
        f"Decoded {manifest['descriptor_count']} descriptors into "
        f"{manifest['unique_pose_count']} unique poses "
        f"({manifest['decoded_bytes']} bytes) at {args.output}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
