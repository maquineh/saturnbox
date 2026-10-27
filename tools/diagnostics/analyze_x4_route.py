"""Correlate X4's recorded pad/SMPC reads with CD PLAYs, without running the game.

Usage: python analyze_x4_route.py DISC.mdf REFERENCE.csv REPLAY.csv
The peripheral reader block is specific to Rockman X4 T-1221G V1.002.
"""

import argparse
from collections import Counter
import csv
import hashlib
import json
from pathlib import Path


def disc_files(path, sector_size):
    with path.open("rb") as disc:
        def sector(lba):
            disc.seek(lba * sector_size + (0 if sector_size == 2048 else 16))
            return disc.read(2048)

        descriptor = sector(16)
        if descriptor[:7] != b"\x01CD001\x01":
            raise ValueError("Expected a MODE1 ISO9660 primary volume descriptor")

        def walk(lba, size, prefix=""):
            data = b"".join(sector(i) for i in range(lba, lba + (size + 2047) // 2048))
            offset = 0
            while offset < size:
                length = data[offset]
                if not length:
                    offset = (offset // 2048 + 1) * 2048
                    continue
                record = data[offset:offset + length]
                offset += length
                name = record[33:33 + record[32]]
                if name in (b"\0", b"\1"):
                    continue
                name = prefix + name.decode("ascii").split(";")[0]
                extent = int.from_bytes(record[2:6], "little")
                size_bytes = int.from_bytes(record[10:14], "little")
                if record[25] & 2:
                    yield from walk(extent, size_bytes, name + "/")
                else:
                    yield {"file": name, "fad": extent + 150,
                           "sectors": (size_bytes + 2047) // 2048}

        root = descriptor[156:]
        return list(walk(int.from_bytes(root[2:6], "little"),
                         int.from_bytes(root[10:14], "little")))


def analyze(path, files):
    plays, pads = [], []
    reads = Counter()
    first_read = None
    dropped = None

    def finish_pad(end_step=None):
        if pads:
            pads[-1].update(end_step=end_step, peripheral_reads=dict(reads),
                            first_peripheral_read=first_read)

    with path.open(newline="", encoding="utf-8-sig") as stream:
        def lines():
            nonlocal dropped
            for line in stream:
                if line.startswith("#"):
                    if "dropped=" in line:
                        dropped = int(line.split("dropped=")[1].split()[0])
                else:
                    yield line

        for row in csv.DictReader(lines()):
            step = int(row["step"])
            if row["kind"] == "pad-input" and row["port"] == "0":
                finish_pad(step)
                pads.append({"step": step, "buttons_hex": row["value_hex"]})
                reads.clear()
                first_read = None
            elif (row["kind"] == "smpc-read" and pads
                  and row["block"] == "0x0603446C"):
                reads[row["value_hex"]] += 1
                if first_read is None:
                    first_read = {"step": step, "value_hex": row["value_hex"]}
            elif row["kind"] == "cd-command" and int(row["cr1_hex"], 16) >> 8 == 0x10:
                # CR1/CR3 bit 7 selects FAD/count addressing; it is not address data.
                fad = ((int(row["cr1_hex"], 16) & 0x7F) << 16) | int(row["cr2_hex"], 16)
                count = ((int(row["cr3_hex"], 16) & 0x7F) << 16) | int(row["cr4_hex"], 16)
                file = next((f for f in files if f["fad"] <= fad < f["fad"] + f["sectors"]), None)
                plays.append({"step": step, "fad": fad, "count": count,
                              "file": file["file"] if file else None,
                              "file_offset": (fad - file["fad"]) * 2048 if file else None})
    finish_pad()
    return {"trace": str(path.resolve()), "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
            "dropped": dropped, "peripheral_reader_block": "0x0603446C",
            "pads": pads, "plays": plays}


def compare(reference, replay):
    common = 0
    for left, right in zip(reference["plays"], replay["plays"]):
        if (left["fad"], left["count"]) != (right["fad"], right["count"]):
            break
        common += 1
    return {"common_play_prefix": common,
            "first_different_reference_play": next(iter(reference["plays"][common:]), None),
            "first_different_replay_play": next(iter(replay["plays"][common:]), None),
            "complete_retained_windows": reference["dropped"] == replay["dropped"] == 0}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("disc", type=Path)
    parser.add_argument("reference", type=Path)
    parser.add_argument("replay", type=Path)
    parser.add_argument("--sector-size", type=int, choices=(2048, 2352, 2448), default=2448)
    args = parser.parse_args()
    files = disc_files(args.disc, args.sector_size)
    reference, replay = (analyze(path, files) for path in (args.reference, args.replay))
    print(json.dumps({"comparison": compare(reference, replay),
                      "reference": reference, "replay": replay}, indent=2))
