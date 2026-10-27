"""Validate the NBG0 map address of the Rockman X4 frame-2469 capture.

Independent calculation from Sega VDP2 manual table 4.8; no emulator mutation.
Deliberately limited to this 4-bpp, 16x16, two-word, 2x1-page, unscrolled case.
"""

import argparse
import csv
import hashlib
import json
from collections import Counter
from pathlib import Path


def validate(directory: Path) -> dict:
    prefix = directory / "frame_002469"
    paths = {name: Path(f"{prefix}_{name}.bin")
             for name in ("vram", "cram", "registers")}
    vram, cram, registers = (paths[name].read_bytes() for name in paths)
    assert (len(vram), len(cram), len(registers)) == (0x80000, 0x1000, 0x200)

    def register(offset):
        return int.from_bytes(registers[offset:offset + 2], "big")

    expected = {0x006: 0, 0x028: 0x0101, 0x030: 0, 0x03A: 0x0015,
                0x03C: 0x0111, 0x040: 0x3030, 0x042: 0x3030,
                0x070: 0, 0x072: 0, 0x074: 0, 0x076: 0}
    for offset, value in expected.items():
        assert register(offset) == value, f"Unsupported register {offset:#x}"

    # Table 4.8: two-word, 2x2-cell character, 2x1-page plane:
    # bits 7:1 of the combined register, times 0x2000. At 4 Mbit,
    # ignore its most significant used bit (bit 7), hence mask 0x7e.
    combined = ((register(0x03C) & 7) << 6) | (register(0x040) & 0x3f)
    correct_base = ((combined & 0x7e) >> 1) * 0x2000
    # The old renderer applies the 8x8 multiplier to 16x16 characters.
    old_base_unmasked = (combined >> 1) * 0x8000
    old_base = old_base_unmasked & 0x7ffff
    assert (correct_base, old_base) == (0x70000, 0x40000)

    with Path(f"{prefix}_cells.csv").open(newline="") as stream:
        cells = list(csv.DictReader(stream))
    assert len(cells) == 320 * 224
    names = set()
    for i, cell in enumerate(cells):
        x, y = i % 320, i // 320
        assert (int(cell["x"]), int(cell["y"])) == (x, y)
        # One page is 512x512 pixels, or 32x32 names of 16x16 characters.
        entry_offset = ((y // 16) * 32 + x // 16) * 4
        assert int(cell["map_hex"], 16) == old_base_unmasked + entry_offset
        address = correct_base + entry_offset
        names.add(address)
        # Both the original and corrected visible names are zero.
        assert vram[address:address + 4] == b"\0" * 4
        assert int(cell["name_upper_hex"], 16) == 0
        assert int(cell["name_lower_hex"], 16) == 0
        assert int(cell["character_hex"], 16) == 0
        assert int(cell["palette_hex"], 16) == 0
        assert int(cell["decoded_argb_hex"], 16) == 0
    assert not any(vram[correct_base:correct_base + 0x2000])
    assert not any(vram[:0x80])
    assert cram[:2] == b"\0\0"

    with Path(f"{prefix}_writes.csv").open(newline="") as stream:
        writes = [r for r in csv.DictReader(stream) if r["memory"] == "vram"
                  and correct_base <= int(r["offset_hex"], 16) < correct_base + 0x2000]
    assert len(writes) == 256
    assert all(int(r["writes"]) > 0 for r in writes)
    frames = Counter(int(r["last_frame"]) for r in writes)
    return {
        "frame": 2469,
        "sources": [
            "https://www.infochunk.com/saturn/segahtml_en/hard/vdp2/hon/p04_80.htm",
            "https://www.infochunk.com/saturn/segahtml_en/hard/vdp2/hon/p04_60.htm",
            "https://raw.githubusercontent.com/Yabause/yabause/master/yabause/src/vidshared.h",
        ],
        "sha256": {name: hashlib.sha256(path.read_bytes()).hexdigest()
                   for name, path in paths.items()},
        "combined_map_register": hex(combined),
        "manual_formula": "((0x70 & 0x7e) >> 1) * 0x2000 = 0x70000",
        "old_formula": "(0x70 >> 1) * 0x8000 = 0x1c0000; & 0x7ffff = 0x40000",
        "correct_plane_range": [hex(correct_base), hex(correct_base + 0x1fff)],
        "correct_visible_names": {"count": len(names), "first": hex(min(names)),
                                  "last": hex(max(names)), "nonzero": 0},
        "correct_plane_nonzero_bytes": 0,
        "correct_plane_write_regions": len(writes),
        "correct_plane_last_write_frames": dict(frames),
        "correct_plane_total_region_write_events": sum(int(r["writes"]) for r in writes),
        "conclusion": "Plane-base calculation is wrong, but the correct map is also zero in this capture.",
        "counterfactual": "Changing only the plane base leaves all NBG0 visible names zero and its decoded pixels black for this fixed snapshot.",
        "limits": ["No new boot or functional renderer change performed.",
                   "Write metadata is per 32-byte region, not per-byte history or source PC.",
                   "Cannot tell whether a populated map was cleared or has not yet been populated.",
                   "Does not certify all other character, palette or page-navigation formulas."],
    }


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    result = validate(args.directory)
    text = json.dumps(result, indent=2, ensure_ascii=False)
    (args.directory / "map-address-validation.json").write_text(text + "\n", encoding="utf-8")
    print(text)
