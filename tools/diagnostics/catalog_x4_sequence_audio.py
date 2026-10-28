"""Describe the audio each X4 sequence produces, so clips can be identified.

The scan harness renders one WAV per audible sequence through the real 68K
driver. This script measures each file and writes a catalogue: activity
window, peak, RMS, spectral centroid, zero-crossing rate, decay time and the
strongest partial. The labels are descriptive only ("short percussive",
"sustained tonal"...): they say what the audio looks like, not which game
action triggered it.

Usage:

    python tools/diagnostics/catalog_x4_sequence_audio.py \
        --scan build-scsp/tmp-test/sequence-scan/sequence-scan.csv \
        --wav-dir build-scsp/tmp-test/sequence-scan/wav \
        --out build-scsp/tmp-test/sequence-catalog
"""

import argparse
import csv
import json
import wave
from pathlib import Path

import numpy as np

SAMPLE_RATE = 44100
FRAME = 1024
HOP = 512


def read_wav(path: Path) -> np.ndarray:
    with wave.open(str(path), "rb") as handle:
        channels = handle.getnchannels()
        width = handle.getsampwidth()
        frames = handle.getnframes()
        raw = handle.readframes(frames)
    if width != 2:
        raise ValueError(f"{path} is not 16-bit PCM")
    data = np.frombuffer(raw, dtype="<i2").astype(np.float32) / 32768.0
    if channels == 2:
        data = data.reshape(-1, 2).mean(axis=1)
    return data


def measure(samples: np.ndarray) -> dict:
    if samples.size == 0:
        return {}
    peak = float(np.max(np.abs(samples)))
    rms = float(np.sqrt(np.mean(samples.astype(np.float64) ** 2)))
    threshold = max(peak * 0.01, 1e-4)
    active = np.nonzero(np.abs(samples) > threshold)[0]
    if active.size == 0:
        return {"peak": peak, "rms": rms, "active_samples": 0,
                "active_seconds": 0.0, "decay_seconds": None,
                "centroid_hz": None, "zcr": None, "strongest_hz": None}
    first, last = int(active[0]), int(active[-1])
    active_samples = last - first + 1
    # Decay: from the peak envelope to -20 dB of it.
    envelope = np.abs(samples[first:last + 1])
    peak_index = int(np.argmax(envelope))
    peak_value = envelope[peak_index]
    target = peak_value * 0.1
    below = np.nonzero(envelope[peak_index:] < target)[0]
    decay_seconds = (below[0] / SAMPLE_RATE) if below.size else None
    # Spectral features over frames that carry signal.
    window = np.hanning(FRAME).astype(np.float32)
    centroids = []
    strongest = []
    for start in range(first, max(first + 1, last - FRAME + 1), HOP):
        frame = samples[start:start + FRAME]
        if frame.size < FRAME:
            break
        if float(np.sqrt(np.mean(frame ** 2))) < threshold:
            continue
        spectrum = np.abs(np.fft.rfft(frame * window))
        frequencies = np.fft.rfftfreq(FRAME, 1.0 / SAMPLE_RATE)
        total = float(np.sum(spectrum))
        if total <= 0.0:
            continue
        centroids.append(float(np.sum(frequencies * spectrum) / total))
        strongest.append(float(frequencies[int(np.argmax(spectrum))]))
    sign_changes = int(np.count_nonzero(np.diff(np.signbit(samples[first:last + 1]))))
    zcr = sign_changes / max(1.0, active_samples / SAMPLE_RATE) / SAMPLE_RATE
    return {
        "peak": peak,
        "rms": rms,
        "active_samples": active_samples,
        "active_seconds": active_samples / SAMPLE_RATE,
        "decay_seconds": decay_seconds,
        "centroid_hz": float(np.mean(centroids)) if centroids else None,
        "zcr": zcr,
        "strongest_hz": float(np.median(strongest)) if strongest else None,
    }


def classify(row: dict, features: dict) -> str:
    key_ons = int(row.get("key_ons") or 0)
    if features.get("active_seconds") is None:
        return "silent"
    if key_ons >= 6:
        return "polyphonic-mix"
    if features["active_seconds"] < 0.35:
        centroid = features.get("centroid_hz") or 0.0
        if centroid > 1500.0 or (features.get("zcr") or 0.0) > 0.12:
            return "short-bright"
        return "short-tonal"
    return "sustained"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scan", type=Path, required=True)
    parser.add_argument("--wav-dir", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    arguments = parser.parse_args()
    arguments.out.mkdir(parents=True, exist_ok=True)
    rows = []
    with arguments.scan.open(newline="", encoding="utf-8") as handle:
        for row in csv.DictReader(handle):
            candidate = (arguments.wav_dir /
                         f"bank-{int(row['bank']):02d}-sequence-{int(row['sequence']):03d}"
                         f"-control-{row['control']}.wav")
            if row.get("status") != "audio" or not candidate.exists():
                continue
            features = measure(read_wav(candidate))
            rows.append({
                "bank": row["bank"], "sequence": row["sequence"],
                "control": row["control"], "key_ons": row["key_ons"],
                "wav": candidate.name,
                "character": classify(row, features),
                **{key: (round(value, 4) if isinstance(value, float) else value)
                   for key, value in features.items()},
            })
    csv_path = arguments.out / "sequence-audio-catalog.csv"
    with csv_path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)
    counts = {}
    for row in rows:
        counts[row["character"]] = counts.get(row["character"], 0) + 1
    (arguments.out / "catalog.json").write_text(
        json.dumps({"sequences": len(rows), "characters": counts}, indent=2) + "\n",
        encoding="utf-8")
    print(f"catalogued {len(rows)} audible sequences: {counts}")
    for row in sorted(rows, key=lambda item: (item["character"], item["bank"],
                                              item["sequence"]))[:12]:
        print(f"  b{row['bank']} s{row['sequence']:>3} {row['character']:14s} "
              f"active={row['active_seconds']:.3f}s peak={row['peak']:.3f} "
              f"centroid={row['centroid_hz']}hz")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
