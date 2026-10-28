"""Measure the audible onset and the requested cue for each scripted press.

This is the in-game companion of the offline catalogue. For every scripted
button press it reports three objective facts:

1. whether the captured audio has a transient right at the press (spectral-flux
   peak inside the window compared with the second before it),
2. which sequence the game requested nearest to the press (from
   `--scsp-requests`), and
3. the level of the window, so a silent window is visible as such.

It also writes one WAV per press so the segment can be listened to directly.
Naming a cue ("this is the shot") comes from the input that triggered it, not
from the audio alone; the report keeps those two facts separate.

Usage:

    python tools/diagnostics/probe_x4_gameplay_onsets.py \
        --audio build-scsp/tmp-test/stage-gameplay.wav \
        --requests build-scsp/tmp-test/stage-requests.csv \
        --presses 160000000:b,164000000:b,174000000:c,178000000:a \
        --vblank-blocks 14000 \
        --out build-scsp/tmp-test/gameplay-onsets
"""

import argparse
import csv
import json
import wave
from pathlib import Path

import numpy as np

SAMPLE_RATE = 44100
SAMPLES_PER_FRAME = 735
FRAME = 512
HOP = 256
BEFORE = 0.25
AFTER = 0.75


def read_wav(path: Path) -> np.ndarray:
    with wave.open(str(path), "rb") as handle:
        channels = handle.getnchannels()
        width = handle.getsampwidth()
        raw = handle.readframes(handle.getnframes())
    if width != 2:
        raise ValueError(f"{path} is not 16-bit PCM")
    data = np.frombuffer(raw, dtype="<i2").astype(np.float32) / 32768.0
    if channels == 2:
        data = data.reshape(-1, 2).mean(axis=1)
    return data


def write_wav(path: Path, samples: np.ndarray) -> None:
    with wave.open(str(path), "wb") as handle:
        handle.setnchannels(1)
        handle.setsampwidth(2)
        handle.setframerate(SAMPLE_RATE)
        handle.writeframes(np.clip(samples * 32767.0, -32768, 32767)
                           .astype("<i2").tobytes())


def flux_curve(samples: np.ndarray) -> np.ndarray:
    """Positive spectral flux per hop."""
    if samples.size < FRAME:
        return np.zeros(0, dtype=np.float64)
    window = np.hanning(FRAME).astype(np.float32)
    values = []
    previous = None
    for start in range(0, samples.size - FRAME + 1, HOP):
        magnitude = np.abs(np.fft.rfft(samples[start:start + FRAME] * window))
        values.append(0.0 if previous is None
                      else float(np.sum(np.maximum(magnitude - previous, 0.0))))
        previous = magnitude
    return np.asarray(values)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--audio", type=Path, required=True)
    parser.add_argument("--requests", type=Path, required=True)
    parser.add_argument("--presses", required=True, help="step:button list, comma separated")
    parser.add_argument("--vblank-blocks", type=int, default=14000)
    parser.add_argument("--out", type=Path, required=True)
    arguments = parser.parse_args()
    arguments.out.mkdir(parents=True, exist_ok=True)
    wav_dir = arguments.out / "windows"
    wav_dir.mkdir(exist_ok=True)

    audio = read_wav(arguments.audio)
    samples_per_step = SAMPLES_PER_FRAME / arguments.vblank_blocks
    requests = []
    with arguments.requests.open(newline="", encoding="utf-8") as handle:
        for row in csv.DictReader(handle):
            requests.append({"step": int(row["step"]), "control": int(row["control"]),
                             "bank": int(row["bank"]), "sequence": int(row["sequence"])})
    requests.sort(key=lambda item: item["step"])

    rows = []
    for token in arguments.presses.split(","):
        block_text, button = token.split(":")
        step = int(block_text)
        time_s = step * samples_per_step / SAMPLE_RATE
        start = int(step * samples_per_step)
        window = audio[max(0, start - int(BEFORE * SAMPLE_RATE)):
                       start + int(AFTER * SAMPLE_RATE)]
        if window.size < FRAME * 2:
            continue
        flux = flux_curve(window)
        before_frames = int(BEFORE * SAMPLE_RATE / HOP)
        onset_region = flux[before_frames:]
        reference = flux[:max(1, before_frames)]
        onset_peak = float(np.max(onset_region)) if onset_region.size else 0.0
        reference_peak = float(np.max(reference)) if reference.size else 0.0
        onset_index = int(np.argmax(onset_region)) if onset_region.size else 0
        onset_ms = 1000.0 * (onset_index * HOP + FRAME / 2) / SAMPLE_RATE
        nearest = min(requests, key=lambda item: abs(item["step"] - step)) if requests else None
        delta_ms = (1000.0 * (nearest["step"] - step) * samples_per_step / SAMPLE_RATE
                    if nearest else None)
        name = f"press-{step}-{button}.wav"
        write_wav(wav_dir / name, window)
        rows.append({
            "step": step, "button": button, "time_s": round(time_s, 3),
            "peak": round(float(np.max(np.abs(window))), 3),
            "rms": round(float(np.sqrt(np.mean(window.astype(np.float64) ** 2))), 4),
            "onset_ms": round(onset_ms, 1),
            "onset_flux": round(onset_peak, 2),
            "reference_flux": round(reference_peak, 2),
            "flux_ratio": round(onset_peak / reference_peak, 2) if reference_peak > 0 else None,
            "nearest_request": (None if nearest is None
                                else f"c{nearest['control']}-b{nearest['bank']}-s{nearest['sequence']}"),
            "request_delta_ms": None if delta_ms is None else round(delta_ms, 1),
            "nearest_step": None if nearest is None else nearest["step"],
            "window": name,
        })

    csv_path = arguments.out / "press-onsets.csv"
    with csv_path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0].keys()) if rows else ["step"])
        writer.writeheader()
        writer.writerows(rows)
    (arguments.out / "press-onsets.json").write_text(
        json.dumps({"presses": len(rows), "rows": rows}, indent=2) + "\n", encoding="utf-8")
    print(f"{len(rows)} presses analysed")
    for row in rows:
        print(f"  {row['button']:<6} t={row['time_s']:>8}s onset={row['onset_ms']:>6}ms "
              f"flux={row['onset_flux']} (pre {row['reference_flux']}, x{row['flux_ratio']}) "
              f"request={row['nearest_request']} at {row['request_delta_ms']}ms")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
