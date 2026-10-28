"""Scan every Rockman X4 sound sequence through the real 68K SCSP driver.

The probe (`saturn-scsp-probe --replay-pending --play-sequence B:S`) boots the
driver from a captured sound-RAM dump, replays the game's pending command batch
(which loads sound map 0) and then queues a single `01` sequence-start packet.
This script enumerates the sequence banks straight from the dump, runs one
probe process per entry so runs cannot contaminate each other, and records what
each sequence actually does: key-ons, slots, sample addresses, noise/LFO/FM/DSP
usage and the rendered audio level.

Usage (from the repository root):

    python tools/diagnostics/scan_x4_sequences.py \
        --ram build-saturnbox/rockman/stage-from-title-capture-20261027/video-diagnostics/scsp_ram.bin \
        --probe build-scsp/tools/saturn-scsp-probe/Release/saturn-scsp-probe.exe \
        --out build-scsp/tmp-test/sequence-scan \
        --cycles 6000000 --controls 3,6,0 --wav

Nothing here writes outside --out. The driver and sound data are not part of
the repository; only the derived CSV/manifest are.
"""

import argparse
import csv
import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path

# Map entries are 8 bytes: type<<4|bank, 24-bit start, size with a loaded bit.
MAP_POINTER_OFFSET = 0x408
MAP_TABLE_OFFSET = 0x500
MAP_ENTRY_BYTES = 8


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ram", type=Path, required=True)
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--cycles", type=int, default=6_000_000)
    parser.add_argument("--controls", default="3,6,0")
    parser.add_argument("--banks", default="", help="comma-separated bank filter")
    parser.add_argument("--limit", type=int, default=0, help="stop after N sequences")
    parser.add_argument("--wav", action="store_true", help="keep a WAV per entry")
    parser.add_argument("--sequence-slot", type=int, default=3)
    return parser.parse_args()


def read_sequence_banks(ram: bytes) -> list[dict]:
    """Recover the sequence banks from the sound map the driver published."""
    table = int.from_bytes(ram[MAP_POINTER_OFFSET:MAP_POINTER_OFFSET + 4], "big")
    if table != MAP_TABLE_OFFSET:
        raise ValueError(f"unexpected sound-map pointer 0x{table:X}")
    banks: list[dict] = []
    offset = table
    while offset + MAP_ENTRY_BYTES <= len(ram) and ram[offset] != 0xFF:
        entry = ram[offset:offset + MAP_ENTRY_BYTES]
        kind = (entry[0] >> 4) & 7
        bank = entry[0] & 15
        start = int.from_bytes(entry[1:4], "big") & 0xFFFFF
        size = int.from_bytes(entry[4:8], "big") & 0xFFFFF
        if kind == 1:
            if start + size > len(ram):
                raise ValueError(f"sequence bank {bank} exceeds sound RAM")
            data = ram[start:start + size]
            count = int.from_bytes(data[0:2], "big")
            table_end = 2 + count * 4
            if count == 0 or table_end > size:
                raise ValueError(f"bank {bank} has an invalid entry table")
            offsets = [int.from_bytes(data[2 + index * 4:6 + index * 4], "big")
                       for index in range(count)]
            if any(left >= right for left, right in zip(offsets, offsets[1:])):
                raise ValueError(f"bank {bank} offsets are not ascending")
            banks.append({"bank": bank, "start": start, "size": size,
                          "count": count, "offsets": offsets, "data": data})
        offset += MAP_ENTRY_BYTES
    return banks


SUMMARY = re.compile(r"sequence_bank=(\d+) sequence_number=(\d+) sequence_control=(\d+)")
FIELDS = ("command_status", "key_ons", "key_offs", "key_on_mask", "sample_addresses",
          "peak", "nonzero_samples", "rendered_samples", "noise_samples",
          "pitch_lfo_samples", "amplitude_lfo_samples", "fm_samples",
          "dsp_send_samples", "dsp_effect_samples", "dsp_steps", "dsp_window_writes",
          "dsp_window_nonzero", "dsp_start_writes", "out_of_range_samples")


def parse_run(stdout: str) -> dict | None:
    for line in stdout.splitlines():
        if not line.startswith("sequence_bank="):
            continue
        result = {}
        for token in line.split():
            if "=" not in token:
                continue
            key, value = token.split("=", 1)
            result[key] = value
        return result
    return None


def run_probe(arguments: argparse.Namespace, ram: Path, bank: int, sequence: int,
              control: int, wav: Path | None) -> tuple[dict | None, str]:
    command = [str(arguments.probe), str(ram), str(arguments.cycles),
               "--replay-pending", "--play-sequence", f"{bank}:{sequence}",
               "--sequence-control", str(control), "--sequence-slot",
               str(arguments.sequence_slot), "--mark-loaded"]
    if wav is not None:
        if wav.exists():
            wav.unlink()
        command += ["--wav", str(wav)]
    completed = subprocess.run(command, capture_output=True, text=True, timeout=600)
    if completed.returncode != 0:
        return None, completed.stderr.strip() or completed.stdout.strip()
    return parse_run(completed.stdout), ""


def ensure_map_ram(arguments: argparse.Namespace, out: Path) -> Path:
    """Return a sound-RAM image that already contains the driver's sound map.

    A raw capture keeps 0x500 erased (0xFF); the driver builds the map while it
    processes the game's `08` map-load packet. Replaying that batch once and
    dumping the resulting RAM gives a stable table to enumerate from.
    """
    target = out / "map-ram.bin"
    if target.exists():
        return target
    command = [str(arguments.probe), str(arguments.ram), "4000000",
               "--replay-pending", "--mark-loaded",
               "--dump-final-ram", str(target)]
    completed = subprocess.run(command, capture_output=True, text=True, timeout=600)
    if completed.returncode != 0 or not target.exists():
        raise RuntimeError("could not build the sound map: "
                           + (completed.stderr.strip() or completed.stdout.strip()))
    return target


def main() -> int:
    arguments = parse_arguments()
    ram_bytes = arguments.ram.read_bytes()
    if len(ram_bytes) != 0x80000:
        raise ValueError("expected a complete 512 KiB sound-RAM snapshot")
    arguments.out.mkdir(parents=True, exist_ok=True)
    map_ram = ensure_map_ram(arguments, arguments.out).read_bytes()
    banks = read_sequence_banks(map_ram)
    wanted = {int(value) for value in arguments.banks.split(",") if value.strip()}
    if wanted:
        banks = [bank for bank in banks if bank["bank"] in wanted]
    controls = [int(value) for value in arguments.controls.split(",") if value.strip()]
    wav_dir = arguments.out / "wav"
    if arguments.wav:
        wav_dir.mkdir(exist_ok=True)

    rows: list[dict] = []
    failures: list[str] = []
    examined = 0
    for bank in banks:
        for number, offset in enumerate(bank["offsets"]):
            if arguments.limit and examined >= arguments.limit:
                break
            examined += 1
            end = (bank["offsets"][number + 1] if number + 1 < bank["count"]
                   else bank["size"])
            payload = bank["data"][offset:end]
            chosen: dict | None = None
            chosen_control = controls[0]
            for control in controls:
                wav = wav_dir / f"bank-{bank['bank']:02d}-sequence-{number:03d}-control-{control}.wav" \
                    if arguments.wav else None
                result, error = run_probe(arguments, arguments.ram, bank["bank"], number,
                                          control, wav)
                if result is None:
                    failures.append(f"bank {bank['bank']} sequence {number} control {control}: {error}")
                    continue
                if int(result.get("key_ons", "0")) > 0 or result.get("nonzero_samples") != "0":
                    chosen = result
                    chosen_control = control
                    if wav is not None:
                        for other in controls:
                            if other == control:
                                continue
                            stale = wav_dir / f"bank-{bank['bank']:02d}-sequence-{number:03d}-control-{other}.wav"
                            if stale.exists():
                                stale.unlink()
                    break
                if chosen is None:
                    chosen, chosen_control = result, control
            row = {"bank": bank["bank"], "sequence": number,
                   "address": bank["start"] + offset, "size": len(payload),
                   "sha256": hashlib.sha256(payload).hexdigest(),
                   "control": chosen_control}
            if chosen is None:
                row["status"] = "probe-error"
            else:
                for field in FIELDS:
                    row[field] = chosen.get(field, "")
                row["status"] = "audio" if int(chosen.get("nonzero_samples", "0")) > 0 else "silent"
            rows.append(row)
            print(f"bank {row['bank']:2d} seq {row['sequence']:3d} "
                  f"control {row['control']} status {row['status']:11s} "
                  f"key_ons {row.get('key_ons', '-'):>3} "
                  f"peak {row.get('peak', '-'):>9} "
                  f"noise {row.get('noise_samples', '-'):>7} "
                  f"fm {row.get('fm_samples', '-'):>7}", flush=True)
        else:
            continue
        break

    csv_path = arguments.out / "sequence-scan.csv"
    with csv_path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0].keys()) if rows else ["bank"])
        writer.writeheader()
        writer.writerows(rows)
    audible = sum(1 for row in rows if row["status"] == "audio")
    with_printer = sum(1 for row in rows if int(row.get("noise_samples") or 0) > 0)
    with_lfo = sum(1 for row in rows
                   if int(row.get("pitch_lfo_samples") or 0) > 0
                   or int(row.get("amplitude_lfo_samples") or 0) > 0)
    with_fm = sum(1 for row in rows if int(row.get("fm_samples") or 0) > 0)
    with_dsp = sum(1 for row in rows if int(row.get("dsp_steps") or 0) > 0)
    manifest = {
        "format": "X4 sequence scan through the real 68K SCSP driver",
        "sound_ram": str(arguments.ram),
        "sound_ram_sha256": hashlib.sha256(ram_bytes).hexdigest(),
        "probe": str(arguments.probe),
        "cycles": arguments.cycles,
        "controls": controls,
        "sequence_slot": arguments.sequence_slot,
        "banks": [{"bank": bank["bank"], "start": bank["start"], "size": bank["size"],
                   "count": bank["count"]} for bank in banks],
        "examined": len(rows),
        "audible": audible,
        "with_noise": with_printer,
        "with_lfo": with_lfo,
        "with_fm": with_fm,
        "with_dsp": with_dsp,
        "failures": failures,
    }
    (arguments.out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n",
                                                 encoding="utf-8")
    print(f"scan complete: {len(rows)} sequences, {audible} audible, "
          f"{with_printer} with noise, {with_lfo} with LFO, {with_fm} with FM, "
          f"{with_dsp} with DSP; {len(failures)} probe failures")
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
