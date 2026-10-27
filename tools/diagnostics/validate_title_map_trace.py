"""Validate Rockman X4's title-map stores against a user-supplied STE_0_0.ARC.

Checks data provenance and cell-mode availability, not title-screen appearance.
Usage: python validate_title_map_trace.py CAPTURE_DIRECTORY STE_0_0.ARC
"""

import argparse
import csv
import hashlib
import json
import struct
from pathlib import Path


def validate(directory: Path, archive_path: Path) -> dict:
    archive = archive_path.read_bytes()
    payload = 0x800
    definitions = None
    definition_offset = None
    for header in range(0, 0x800, 16):
        kind, count, first, last, mode = struct.unpack_from(">IIHHI", archive, header)
        if kind == 255:
            break
        size = first if count == 1 else first + (count - 2) * 0x800 + last
        if kind == 3:
            assert mode == 0 and size % 4 == 0
            definitions = {value & 0xFFFFFF for (value,) in
                           struct.iter_unpack(">I", archive[payload:payload + size])}
            definition_offset = payload
            break
        payload += size
    assert definitions is not None, "Archive has no pattern-name definitions"

    def rows(name):
        with (directory / name).open(newline="") as stream:
            return list(csv.DictReader(line for line in stream if not line.startswith("#")))

    writes = [row for row in rows("vdp2-map-producer.csv")
              if row["block"] in ("0x0601A182", "0x0601A284")
              and int(row["value"], 16) != 0]
    assert writes, "Guest title-map expansion was not reached"
    for row in writes:
        assert 0x70000 <= int(row["offset"], 16) < 0x72000
        assert int(row["width"]) == 4
        # The guest combines the definition's low 24 bits with tile flip bits.
        assert int(row["value"], 16) & 0xFFFFFF in definitions, row
    states = rows("vdp2-map-state.csv")
    populated = [row for row in states
                 if int(row["step"]) >= int(writes[0]["step"])
                 and int(row["chctla"], 16) == 0x0101
                 and int(row["expected_nonzero_words"]) > 0]
    assert populated, "No populated title map captured in cell mode"
    reads = [row for row in rows("vdp2-map-read.csv")
             if int(row["step"]) >= int(writes[0]["step"])]
    for row in reads:
        assert 0x70000 <= int(row["renderer_min_address"], 16) < 0x72000
        assert 0x70000 <= int(row["renderer_max_address"], 16) < 0x72000
    return {
        "archive_sha256": hashlib.sha256(archive).hexdigest(),
        "definition_file_offset": hex(definition_offset),
        "nonzero_title_stores": len(writes),
        "all_stores_match_archive_definitions": True,
        "first_store": {key: writes[0][key] for key in ("step", "block", "address", "value")},
        "first_populated_cell_frame": populated[0],
        "final_state": states[-1],
        "later_renderer_samples": len(reads),
        "later_renderer_nonzero_samples": sum(int(row["renderer_nonzero_reads"]) > 0
                                              for row in reads),
        "limit": "Map availability does not prove NBG0 is enabled or its populated cells are visible.",
    }


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("archive", type=Path)
    args = parser.parse_args()
    print(json.dumps(validate(args.directory, args.archive), indent=2))
