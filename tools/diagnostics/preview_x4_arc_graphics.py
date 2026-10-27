"""Render tiled analysis previews for selected X4 ARC graphics payloads.

The PNGs are grayscale index views, not palette-accurate game renders. All
selected payloads are previewed as 16x16, packed 4bpp tiles in tile-major order
(high nibble first). This layout matches the dimensions of the existing type-14
and type-35 contact sheets; types 5, 17, and 27 remain exploratory.
Original payloads and source offsets remain in the ARC extraction manifest.

Usage:
  python tools/diagnostics/preview_x4_arc_graphics.py
  python tools/diagnostics/preview_x4_arc_graphics.py build/rockman/arc-extracted --output build/rockman/arc-previews-expanded
"""

import argparse
import binascii
import hashlib
import json
from pathlib import Path
import struct
import zlib


PREVIEW_FORMATS = {
    5: {
        "confidence": "exploratory 16x16 tiled 4bpp view; palette and type semantics unknown",
    },
    14: {
        "confidence": "16x16 tiled 4bpp view consistent with prior visual inspection; palette unknown",
    },
    17: {
        "confidence": "exploratory 16x16 tiled 4bpp view; payload also matches captured VDP1 VRAM",
    },
    27: {
        "confidence": "exploratory 16x16 tiled 4bpp view of a UI payload; palette unknown",
    },
    35: {
        "confidence": "16x16 tiled 4bpp view consistent with prior visual inspection; palette unknown",
    },
}


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def png_chunk(kind: bytes, payload: bytes) -> bytes:
    checksum = binascii.crc32(kind + payload) & 0xFFFFFFFF
    return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", checksum)


def tiled_4bpp_pixels(data: bytes, tile_size: int = 16, tiles_per_row: int = 16) -> tuple[bytes, int, int]:
    pixels_per_tile = tile_size * tile_size
    bytes_per_tile = pixels_per_tile // 2
    tile_count = (len(data) + bytes_per_tile - 1) // bytes_per_tile
    tile_rows = max(1, (tile_count + tiles_per_row - 1) // tiles_per_row)
    width = tile_size * tiles_per_row
    height = tile_size * tile_rows
    canvas = bytearray(width * height)

    for tile_index in range(tile_count):
        source_start = tile_index * bytes_per_tile
        tile_data = data[source_start:source_start + bytes_per_tile]
        tile_x = (tile_index % tiles_per_row) * tile_size
        tile_y = (tile_index // tiles_per_row) * tile_size
        for byte_index, value in enumerate(tile_data):
            pixel_index = byte_index * 2
            for within, nibble in enumerate((value >> 4, value & 0x0F)):
                x = tile_x + (pixel_index + within) % tile_size
                y = tile_y + (pixel_index + within) // tile_size
                canvas[y * width + x] = nibble * 17
    return bytes(canvas), width, height


def write_grayscale_png(path: Path, pixels: bytes, width: int) -> tuple[int, int, int]:
    if width <= 0:
        raise ValueError("PNG width must be positive")
    height = max(1, (len(pixels) + width - 1) // width)
    padded = pixels + bytes(width * height - len(pixels))
    scanlines = bytearray()
    for row in range(height):
        scanlines.append(0)  # PNG filter: None
        start = row * width
        scanlines.extend(padded[start:start + width])
    image_data = zlib.compress(bytes(scanlines), level=9)
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 0, 0, 0, 0)
    png = (
        b"\x89PNG\r\n\x1a\n"
        + png_chunk(b"IHDR", ihdr)
        + png_chunk(b"IDAT", image_data)
        + png_chunk(b"IEND", b"")
    )
    path.write_bytes(png)
    return width, height, len(padded) - len(pixels)


def render(extraction_dir: Path, output_dir: Path) -> dict:
    manifest_path = extraction_dir / "manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if output_dir.exists() and any(output_dir.iterdir()):
        raise FileExistsError(f"Refusing to overwrite non-empty output directory: {output_dir}")

    selections = []
    for archive in manifest["files"]:
        for entry in archive["entries"]:
            spec = PREVIEW_FORMATS.get(entry["kind"])
            if spec is None:
                continue
            raw_path = extraction_dir / entry["output"]
            raw = raw_path.read_bytes()
            observed_hash = sha256(raw)
            if len(raw) != entry["payload_size"] or observed_hash != entry["sha256"]:
                raise ValueError(f"Payload does not match extraction manifest: {raw_path}")
            selections.append((archive, entry, spec, raw))

    if not selections:
        raise ValueError("No supported graphics payloads found in extraction manifest")

    output_dir.mkdir(parents=True, exist_ok=True)
    previews = []
    for archive, entry, spec, raw in selections:
        pixels, width, height = tiled_4bpp_pixels(raw)
        preview_path = output_dir / Path(entry["output"]).with_suffix(".png")
        preview_path.parent.mkdir(parents=True, exist_ok=True)
        write_grayscale_png(preview_path, pixels, width)
        preview = {
            "archive": archive["archive"],
            "kind": entry["kind"],
            "payload_index": entry["index"],
            "source_file_offset": entry["payload_offset"],
            "source_size": entry["payload_size"],
            "source_sha256": entry["sha256"],
            "source_payload": entry["output"],
            "preview_png": Path(entry["output"]).with_suffix(".png").as_posix(),
            "bits_per_pixel_assumption": 4,
            "tile_size_pixels": 16,
            "tiles_per_row": 16,
            "tile_count": (len(raw) + 0x7F) // 0x80,
            "pixel_width": width,
            "pixel_height": height,
            "padding_pixels": width * height - len(raw) * 2,
            "interpretation_confidence": spec["confidence"],
        }
        previews.append(preview)

    result = {
        "format": "Rockman X4 ARC grayscale graphics index previews",
        "schema_version": 1,
        "source_manifest": str(manifest_path),
        "preview_count": len(previews),
        "type_counts": {
            str(kind): sum(1 for item in previews if item["kind"] == kind)
            for kind in sorted(PREVIEW_FORMATS)
        },
        "previews": previews,
    }
    (output_dir / "preview-index.json").write_text(
        json.dumps(result, indent=2) + "\n", encoding="utf-8"
    )
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "extraction_dir", nargs="?", type=Path, default=Path("build/rockman/arc-extracted")
    )
    parser.add_argument("--output", type=Path, default=Path("build/rockman/arc-previews-expanded"))
    args = parser.parse_args()
    result = render(args.extraction_dir, args.output)
    counts = ", ".join(f"type {kind}: {count}" for kind, count in result["type_counts"].items())
    print(f"Rendered {result['preview_count']} grayscale index previews ({counts}) to {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
