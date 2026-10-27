"""Extract bounded frame/piece records from X4 type-1 ARC payloads.

The output preserves raw 16-bit piece fields. It does not assign tile, palette,
or signed-coordinate semantics to those fields.

Usage:
  python tools/diagnostics/extract_x4_type1_frames.py
  python tools/diagnostics/extract_x4_type1_frames.py build/rockman --output build/rockman/arc-decoded-all
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


def find_type1(data: bytes, path: Path) -> tuple[int, int]:
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
        if kind == 1:
            if mode != 0:
                raise ValueError(f"{path}: type-1 mode {mode} is not raw payload mode")
            return payload_offset, size
        payload_offset = end
    raise ValueError(f"{path}: no type-1 payload before the type-table terminator")


def parse_archive(path: Path) -> dict:
    data = path.read_bytes()
    payload_offset, payload_size = find_type1(data, path)
    payload = data[payload_offset:payload_offset + payload_size]
    if len(payload) < 4:
        raise ValueError(f"{path}: type-1 payload has no directory header")

    directory_bytes = struct.unpack_from(">I", payload, 0)[0]
    if directory_bytes == 0 or directory_bytes % 4:
        raise ValueError(f"{path}: invalid directory byte count {directory_bytes:#x}")
    bank_count = directory_bytes // 4
    # The first word is also the first absolute bank offset. Its value gives
    # the byte length of the entire BE32 offset table (including that word).
    directory_end = directory_bytes
    if directory_end > len(payload):
        raise ValueError(f"{path}: bank directory extends past type-1 payload")
    bank_offsets = [
        struct.unpack_from(">I", payload, index * 4)[0]
        for index in range(bank_count)
    ]
    if any(offset < directory_end or offset >= len(payload) for offset in bank_offsets):
        raise ValueError(f"{path}: bank offset is outside the type-1 payload")

    unique_offsets = sorted(set(bank_offsets))
    bank_ends = {
        start: next((offset for offset in unique_offsets if offset > start), len(payload))
        for start in unique_offsets
    }
    parsed_banks = {}
    for bank_index, start in enumerate(bank_offsets):
        if start in parsed_banks:
            continue
        end = bank_ends[start]
        if start + 4 > end:
            raise ValueError(f"{path}: bank {bank_index} has no first frame descriptor")
        # The second halfword of the first frame descriptor is also the number
        # of frame descriptors in this bank.
        frame_count = struct.unpack_from(">H", payload, start + 2)[0]
        table_end = start + frame_count * 4
        if frame_count == 0 or table_end > end:
            raise ValueError(f"{path}: bank {bank_index} has invalid frame table bounds")

        frames = []
        piece_cursor = table_end
        total_pieces = 0
        for frame_index in range(frame_count):
            pair_offset = start + frame_index * 4
            piece_count, offset_units = struct.unpack_from(">HH", payload, pair_offset)
            piece_offset = start + offset_units * 4
            if piece_offset != piece_cursor:
                raise ValueError(
                    f"{path}: bank {bank_index} frame {frame_index} begins at "
                    f"{piece_offset:#x}, expected contiguous offset {piece_cursor:#x}"
                )
            piece_end = piece_offset + piece_count * 4
            if piece_end > end:
                raise ValueError(f"{path}: bank {bank_index} frame {frame_index} exceeds bank end")

            pieces = []
            for piece_index in range(piece_count):
                offset = piece_offset + piece_index * 4
                code, xy = struct.unpack_from(">HH", payload, offset)
                pieces.append({
                    "offset_in_type1": offset,
                    "raw_code_be16": f"0x{code:04X}",
                    "raw_xy_be16": f"0x{xy:04X}",
                    "raw_hex": payload[offset:offset + 4].hex(),
                })
            frame_bytes = payload[piece_offset:piece_end]
            frames.append({
                "index": frame_index,
                "record_offset_in_type1": pair_offset,
                "piece_count": piece_count,
                "piece_offset_units_of_4": offset_units,
                "piece_offset_in_type1": piece_offset,
                "piece_end_exclusive_in_type1": piece_end,
                "raw_sha256": sha256(frame_bytes),
                "pieces": pieces,
            })
            piece_cursor = piece_end
            total_pieces += piece_count

        if piece_cursor != end:
            raise ValueError(
                f"{path}: bank {bank_index} ends at {piece_cursor:#x}, "
                f"but next bank/payload begins at {end:#x}"
            )
        parsed_banks[start] = {
            "offset_in_type1": start,
            "end_exclusive_in_type1": end,
            "first_frame_piece_count": struct.unpack_from(">H", payload, start)[0],
            "frame_count": frame_count,
            "piece_count_total": total_pieces,
            "frames": frames,
        }

    banks = []
    for index, start in enumerate(bank_offsets):
        banks.append({
            "index": index,
            "offset_in_type1": start,
            "alias_of_index": next(
                (prior for prior in range(index) if bank_offsets[prior] == start), None
            ),
            **parsed_banks[start],
        })
    return {
        "archive": path.name,
        "archive_sha256": sha256(data),
        "type1_payload_offset_in_archive": payload_offset,
        "type1_payload_size": payload_size,
        "type1_payload_sha256": sha256(payload),
        "directory_bytes": directory_bytes,
        "bank_count": bank_count,
        "banks": banks,
        "unique_bank_count": len(unique_offsets),
        "frame_count_total_including_aliases": sum(bank["frame_count"] for bank in banks),
        "piece_count_total_including_aliases": sum(bank["piece_count_total"] for bank in banks),
    }


def extract(input_dir: Path, output_dir: Path) -> dict:
    if output_dir.exists() and any(output_dir.iterdir()):
        raise FileExistsError(f"Refusing to overwrite non-empty output directory: {output_dir}")
    sources = sorted(input_dir.glob("*.ARC"), key=lambda path: path.name.casefold())
    if not sources:
        raise FileNotFoundError(f"No .ARC files found in {input_dir}")

    # Parse every type-1 archive before creating the destination, so malformed
    # data cannot leave a partial extraction behind. Unknown type-1 layouts are
    # preserved byte-for-byte with their parser error instead of being lost.
    archives = []
    for path in sources:
        data = path.read_bytes()
        try:
            payload_offset, payload_size = find_type1(data, path)
        except ValueError as error:
            if "no type-1 payload" in str(error):
                continue
            archives.append({
                "archive": path.name,
                "status": "header-error",
                "error": str(error),
                "archive_sha256": sha256(data),
            })
            continue
        try:
            record = parse_archive(path)
            record["status"] = "parsed"
            archives.append(record)
        except (ValueError, struct.error) as error:
            payload = data[payload_offset:payload_offset + payload_size]
            archives.append({
                "archive": path.name,
                "status": "raw-only",
                "error": str(error),
                "archive_sha256": sha256(data),
                "type1_payload_offset_in_archive": payload_offset,
                "type1_payload_size": payload_size,
                "type1_payload_sha256": sha256(payload),
                "raw_output": f"{path.stem}/type001-raw.bin",
            })

    output_dir.mkdir(parents=True, exist_ok=True)
    for archive in archives:
        archive_dir = output_dir / Path(archive["archive"]).stem
        archive_dir.mkdir()
        if archive["status"] == "parsed":
            (archive_dir / "type001-frames.json").write_text(
                json.dumps(archive, indent=2) + "\n", encoding="utf-8"
            )
        elif archive["status"] == "raw-only":
            source = input_dir / archive["archive"]
            data = source.read_bytes()
            payload_start = archive["type1_payload_offset_in_archive"]
            payload_end = payload_start + archive["type1_payload_size"]
            (archive_dir / "type001-raw.bin").write_bytes(data[payload_start:payload_end])
            (archive_dir / "type001-status.json").write_text(
                json.dumps(archive, indent=2) + "\n", encoding="utf-8"
            )
        else:
            (archive_dir / "type001-status.json").write_text(
                json.dumps(archive, indent=2) + "\n", encoding="utf-8"
            )

    parsed = [row for row in archives if row["status"] == "parsed"]
    raw_only = [row for row in archives if row["status"] == "raw-only"]
    total_banks = sum(row["bank_count"] for row in parsed)
    total_unique_banks = sum(row["unique_bank_count"] for row in parsed)
    total_frames = sum(row["frame_count_total_including_aliases"] for row in parsed)
    total_pieces = sum(row["piece_count_total_including_aliases"] for row in parsed)

    result = {
        "format": "Rockman X4 type-1 frame and piece tables",
        "schema_version": 1,
        "type1_archive_count": len(archives),
        "parsed_archive_count": len(parsed),
        "raw_only_archive_count": len(raw_only),
        "total_banks_including_aliases": total_banks,
        "total_unique_banks": total_unique_banks,
        "total_frames_including_aliases": total_frames,
        "total_pieces_including_aliases": total_pieces,
        "archives": archives,
    }
    (output_dir / "manifest.json").write_text(
        json.dumps(result, indent=2) + "\n", encoding="utf-8"
    )
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input_dir", nargs="?", type=Path, default=Path("build/rockman"))
    parser.add_argument("--output", type=Path, default=Path("build/rockman/arc-decoded-all"))
    args = parser.parse_args()
    manifest = extract(args.input_dir, args.output)
    for archive in manifest["archives"]:
        if archive["status"] == "parsed":
            print(
                f"{archive['archive']}: {archive['bank_count']} banks "
                f"({archive['unique_bank_count']} unique), "
                f"{archive['frame_count_total_including_aliases']} frames, "
                f"{archive['piece_count_total_including_aliases']} pieces"
            )
        else:
            print(f"{archive['archive']}: preserved raw ({archive['status']}: {archive['error']})")
    print(
        f"Parsed {manifest['parsed_archive_count']} type-1 payloads; "
        f"preserved {manifest['raw_only_archive_count']} unparsed payloads at {args.output}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
