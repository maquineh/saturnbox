"""Find candidate player-health bytes in periodic WRAM snapshots.

`saturn-sh2 --watch-memory ADDR:LEN:PERIOD --watch-dir DIR` writes one binary
snapshot per period. This tool diffs them byte by byte and ranks offsets by
"looks like a small counter that goes down and comes back", which is how health,
lives and timers appear. It is a discovery aid, not proof: the report prints
the raw series so the shape can be judged.

Usage:

    python tools/diagnostics/find_x4_health.py \
        --watch-dir build-scsp/tmp-test/watch \
        --address 0x06050000 \
        --out build-scsp/tmp-test/health-candidates
"""

import argparse
import csv
import json
import re
from pathlib import Path

PATTERN = re.compile(r"watch-(\d+)\.bin$")


def load_series(directory: Path) -> tuple[list[int], dict[int, bytes]]:
    steps = []
    snapshots = {}
    for path in sorted(directory.glob("watch-*.bin")):
        match = PATTERN.search(path.name)
        if not match:
            continue
        step = int(match.group(1))
        steps.append(step)
        snapshots[step] = path.read_bytes()
    steps.sort()
    return steps, snapshots


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--watch-dir", type=Path, required=True)
    parser.add_argument("--address", type=lambda value: int(value, 0), default=0)
    parser.add_argument("--minimum-drop", type=int, default=1)
    parser.add_argument("--maximum-value", type=int, default=64)
    parser.add_argument("--out", type=Path, required=True)
    arguments = parser.parse_args()
    arguments.out.mkdir(parents=True, exist_ok=True)

    steps, snapshots = load_series(arguments.watch_dir)
    if len(steps) < 3:
        raise SystemExit(f"need at least three snapshots, found {len(steps)}")
    length = min(len(snapshots[step]) for step in steps)
    rows = []
    for offset in range(length):
        series = [snapshots[step][offset] for step in steps]
        if max(series) > arguments.maximum_value or max(series) == 0:
            continue
        drops = 0
        biggest_drop = 0
        rises = 0
        for previous, current in zip(series, series[1:]):
            delta = current - previous
            if delta < 0 and -delta >= arguments.minimum_drop:
                drops += 1
                biggest_drop = max(biggest_drop, -delta)
            elif delta > 0:
                rises += 1
        if drops == 0:
            continue
        # A health byte decreases a few times, never goes below zero, and either
        # stays down or resets once (death). Penalise pure noise (many rises).
        score = drops * 2 + rises - biggest_drop
        rows.append({
            "offset": offset,
            "address": hex(arguments.address + offset),
            "drops": drops,
            "rises": rises,
            "biggest_drop": biggest_drop,
            "score": score,
            "first": series[0],
            "min": min(series),
            "max": max(series),
            "series": ",".join(str(value) for value in series),
        })
    rows.sort(key=lambda row: (-row["score"], row["offset"]))
    csv_path = arguments.out / "health-candidates.csv"
    with csv_path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0].keys()) if rows else
                                ["offset", "address"])
        writer.writeheader()
        writer.writerows(rows)
    (arguments.out / "health-candidates.json").write_text(
        json.dumps({"snapshots": len(steps), "steps": steps,
                    "candidates": rows[:200]}, indent=2) + "\n", encoding="utf-8")
    print(f"{len(steps)} snapshots of {length} bytes; {len(rows)} candidates")
    for row in rows[:15]:
        print(f"  {row['address']} drops={row['drops']} rises={row['rises']} "
              f"biggest={row['biggest_drop']} {row['series'][:110]}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
