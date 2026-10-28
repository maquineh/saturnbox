"""Decode the structure of a Rockman X4 sound sequence from the driver's reads.

`SDDRVS.TSK` is a register-level sequencer: it programs SCSP slot registers
(total level, pitch, sample address, loop points, envelope) and keys the slot
on. The sequence container it consumes is recovered here from two sources:

* the sound map the driver publishes in sound RAM (same layout the extractor
  uses), which gives every entry's bytes;
* the probe's read trace (`--trace-reads BANK_START:BANK_END`), which shows
  which byte the player read and from which PC. Bytes read by the dispatcher
  are opcodes; bytes read by a handler are that opcode's arguments.

Observed container layout (offsets relative to one entry):

    0x00  u16  header word      read by the start-sequence handler (pc 0x4EDC)
    0x02  u16  never read       present in every entry, not consumed
    0x04  u16  header word      (pc 0x5B38)
    0x06  u16  header word      (pc 0x5B46)
    0x08  u32  header long      (pc 0x5B84)
    0x0C  u32  header long      (pc 0x5B86)
    0x10  ...  event stream     single-byte opcodes, some with arguments

For bank 3 sequence 2 (entry 0x29AD4) the event stream dispatcher is pc
0x57B8; opcodes 0x88-0x8B were consumed there with no arguments, 0xB0 and
0xC0 read two arguments, and the 0x00 byte at offset 0x23 switched to a
different handler (pc 0x57C2) that read the family byte at offset 0x24.
These are observations for that family, not a universal table.

Usage:

    python tools/diagnostics/decode_x4_sequences.py \
        --ram build-scsp/tmp-test/sequence-scan/map-ram.bin \
        --scan build-scsp/tmp-test/sequence-scan/sequence-scan.csv \
        --trace build-scsp/tmp-test/seq32-reads.csv \
        --out build-scsp/tmp-test/sequence-decode
"""

import argparse
import csv
import hashlib
import json
from collections import defaultdict
from pathlib import Path

HEADER_FIELDS = (
    ("header_word_0", 0x00, 2),
    ("reserved_0x02", 0x02, 2),
    ("header_word_4", 0x04, 2),
    ("header_word_6", 0x06, 2),
    ("header_long_8", 0x08, 4),
    ("header_long_12", 0x0C, 4),
)
EVENT_START = 0x10
DISPATCHER_PC = 0x57B8
MAP_POINTER_OFFSET = 0x408
MAP_TABLE_OFFSET = 0x500


def read_banks(ram: bytes) -> list[dict]:
    table = int.from_bytes(ram[MAP_POINTER_OFFSET:MAP_POINTER_OFFSET + 4], "big")
    if table != MAP_TABLE_OFFSET:
        raise ValueError(f"unexpected sound-map pointer 0x{table:X}")
    banks = []
    offset = table
    while offset + 8 <= len(ram) and ram[offset] != 0xFF:
        entry = ram[offset:offset + 8]
        kind = (entry[0] >> 4) & 7
        bank = entry[0] & 15
        start = int.from_bytes(entry[1:4], "big") & 0xFFFFF
        size = int.from_bytes(entry[4:8], "big") & 0xFFFFF
        if kind == 1:
            data = ram[start:start + size]
            count = int.from_bytes(data[0:2], "big")
            offsets = [int.from_bytes(data[2 + index * 4:6 + index * 4], "big")
                       for index in range(count)]
            banks.append({"bank": bank, "start": start, "size": size,
                          "count": count, "offsets": offsets, "data": data})
        offset += 8
    return banks


def entry_payload(bank: dict, number: int) -> tuple[int, bytes]:
    offset = bank["offsets"][number]
    end = (bank["offsets"][number + 1] if number + 1 < bank["count"] else bank["size"])
    return bank["start"] + offset, bank["data"][offset:end]


def load_scan(path: Path | None) -> dict:
    rows = {}
    if path is None or not path.exists():
        return rows
    with path.open(newline="", encoding="utf-8") as handle:
        for row in csv.DictReader(handle):
            rows[(int(row["bank"]), int(row["sequence"]))] = row
    return rows


def annotate_trace(trace_path: Path | None, base: int) -> list[dict] | None:
    """Return per-offset read annotations from a probe read trace, if given."""
    if trace_path is None or not trace_path.exists():
        return None
    reads = {}
    with trace_path.open(newline="", encoding="utf-8") as handle:
        for row in csv.DictReader(handle):
            address = int(row["address"], 16)
            if address < base:
                continue
            reads.setdefault(address - base, []).append(
                {"pc": int(row["pc"], 16), "cycles": int(row["cycles"])})
    return [reads.get(offset, []) for offset in range(0, max(reads) + 1)] if reads else None


def family_groups(bank: dict) -> list[dict]:
    """Group same-length entries and report the offsets where they differ.

    A "family" here means every entry of one bank that shares a length: bank 3
    is one 42-byte template whose entries differ in a single byte. This is a
    structural observation, not proof that the player treats them as variants.
    """
    payloads = [entry_payload(bank, number)[1] for number in range(bank["count"])]
    by_length = defaultdict(list)
    for number, payload in enumerate(payloads):
        by_length[len(payload)].append(number)
    families = []
    for size, numbers in sorted(by_length.items()):
        if len(numbers) < 2:
            continue
        differing = [offset for offset in range(size)
                     if len({payloads[number][offset] for number in numbers}) > 1]
        families.append({
            "size": size,
            "differing_offsets": differing,
            "sequences": numbers,
            "values": {f"0x{offset:02X}": sorted(
                payloads[number][offset] for number in numbers)
                for offset in differing},
        })
    return families


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ram", type=Path, required=True)
    parser.add_argument("--scan", type=Path)
    parser.add_argument("--trace", type=Path)
    parser.add_argument("--trace-bank", type=int, default=3)
    parser.add_argument("--trace-sequence", type=int, default=2)
    parser.add_argument("--out", type=Path, required=True)
    arguments = parser.parse_args()

    ram = arguments.ram.read_bytes()
    scan = load_scan(arguments.scan)
    arguments.out.mkdir(parents=True, exist_ok=True)

    rows = []
    families = []
    for bank in read_banks(ram):
        for number in range(bank["count"]):
            address, payload = entry_payload(bank, number)
            header = {}
            for name, position, width in HEADER_FIELDS:
                header[name] = int.from_bytes(payload[position:position + width], "big")
            observed = scan.get((bank["bank"], number), {})
            rows.append({
                "bank": bank["bank"], "sequence": number, "address": address,
                "size": len(payload), "sha256": hashlib.sha256(payload).hexdigest(),
                **header,
                "event_stream": payload[EVENT_START:].hex(),
                "observed_status": observed.get("status", ""),
                "observed_control": observed.get("control", ""),
                "observed_key_ons": observed.get("key_ons", ""),
                "observed_peak": observed.get("peak", ""),
                "observed_noise": observed.get("noise_samples", ""),
                "observed_lfo": observed.get("pitch_lfo_samples", ""),
                "observed_fm": observed.get("fm_samples", ""),
                "observed_dsp_steps": observed.get("dsp_steps", ""),
            })
        for family in family_groups(bank):
            families.append({"bank": bank["bank"], **family})
            print(f"bank {bank['bank']} family: {len(family['sequences'])} entries of "
                  f"{family['size']} bytes differing at "
                  f"{[hex(value) for value in family['differing_offsets']]}")

    csv_path = arguments.out / "sequence-structure.csv"
    with csv_path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)

    # Annotate one entry with the probe's read trace: opcode vs operand.
    annotation = None
    for bank in read_banks(ram):
        if bank["bank"] != arguments.trace_bank:
            continue
        address, payload = entry_payload(bank, arguments.trace_sequence)
        reads = annotate_trace(arguments.trace, address)
        if reads is None:
            continue
        annotation = {"bank": bank["bank"], "sequence": arguments.trace_sequence,
                      "address": address, "size": len(payload), "bytes": []}
        for offset in range(len(payload)):
            entries = reads[offset] if offset < len(reads) else []
            role = "unread"
            if entries:
                role = "opcode" if any(entry["pc"] == DISPATCHER_PC for entry in entries) else "operand"
            annotation["bytes"].append({
                "offset": offset, "value": payload[offset],
                "role": role,
                "pcs": sorted({f"0x{entry['pc']:04X}" for entry in entries}),
                "first_cycle": entries[0]["cycles"] if entries else None,
            })
        (arguments.out / "trace-annotation.json").write_text(
            json.dumps(annotation, indent=2) + "\n", encoding="utf-8")
        opcodes = [item for item in annotation["bytes"] if item["role"] == "opcode"]
        print(f"trace annotation: {len(opcodes)} opcode bytes, "
              f"{len([item for item in annotation['bytes'] if item['role'] == 'operand'])} operands")

    summary = {
        "format": "X4 sequence container decode",
        "sound_ram": str(arguments.ram),
        "header_fields": [name for name, _, _ in HEADER_FIELDS],
        "event_start": EVENT_START,
        "dispatcher_pc": f"0x{DISPATCHER_PC:04X}",
        "entries": len(rows),
        "sizes": sorted({row["size"] for row in rows}),
        "families": families,
        "annotation_written": annotation is not None,
    }
    (arguments.out / "structure.json").write_text(
        json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(f"decoded {len(rows)} entries; sizes {summary['sizes']}; "
          f"{len(families)} template families")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
