"""Confirm which rendered cue is present in a captured gameplay segment.

Spectral profiles cannot isolate one cue from a music bed: every profile looks
alike. The cues here are *synthesized by the same code* from the same sound RAM,
so the in-game waveform should match the offline render sample for sample, up
to where it starts and how it is mixed. This tool therefore correlates the
waveform (normalized, lag-tolerant) and also its amplitude envelope, and prints
the contrast against the best competing cues.

Usage:

    python tools/diagnostics/match_x4_cues.py \
        --requests build-scsp/tmp-test/stage-requests.csv \
        --audio build-scsp/tmp-test/stage-gameplay.wav \
        --wav-dir build-scsp/tmp-test/sequence-scan/wav \
        --vblank-blocks 14000 --from-step 150000000 \
        --out build-scsp/tmp-test/cue-match
"""

import argparse
import csv
import json
import wave
from pathlib import Path

import numpy as np

SAMPLE_RATE = 44100
SAMPLES_PER_FRAME = 735
WINDOW_BEFORE = 0.10
WINDOW_AFTER = 0.90
ENVELOPE_HOP = 256


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


def envelope(samples: np.ndarray) -> np.ndarray:
    frames = samples.size // ENVELOPE_HOP
    if frames == 0:
        return np.zeros(1, dtype=np.float32)
    trimmed = samples[:frames * ENVELOPE_HOP].reshape(frames, ENVELOPE_HOP)
    return np.sqrt(np.mean(trimmed.astype(np.float64) ** 2, axis=1)).astype(np.float32)


def normalized_correlation(cue: np.ndarray, window: np.ndarray) -> tuple[float, int]:
    """Max |correlation| of `cue` inside `window` over 0..len(cue) lag."""
    if cue.size == 0 or window.size < cue.size:
        return 0.0, 0
    size = 1 << int(np.ceil(np.log2(window.size + cue.size)))
    spectrum_window = np.fft.rfft(window.astype(np.float64), size)
    spectrum_cue = np.fft.rfft(cue[::-1].astype(np.float64), size)
    correlation = np.fft.irfft(spectrum_window * spectrum_cue, size)[:window.size - cue.size + 1]
    # Normalize by the local window energy at each lag.
    energy = np.concatenate(([0.0], np.cumsum(window.astype(np.float64) ** 2)))
    cue_norm = float(np.linalg.norm(cue))
    segment = energy[cue.size:] - energy[:-cue.size]
    denominator = np.sqrt(np.maximum(segment, 1e-12)) * cue_norm
    normalized = np.abs(correlation) / denominator
    index = int(np.argmax(normalized))
    return float(normalized[index]), index


def envelope_correlation(cue: np.ndarray, window: np.ndarray) -> tuple[float, int]:
    cue_envelope = envelope(cue)
    window_envelope = envelope(window)
    if cue_envelope.size < 2 or window_envelope.size < cue_envelope.size:
        return 0.0, 0
    size = 1 << int(np.ceil(np.log2(window_envelope.size + cue_envelope.size)))
    left = np.fft.rfft(window_envelope - window_envelope.mean(), size)
    right = np.fft.rfft((cue_envelope - cue_envelope.mean())[::-1], size)
    correlation = np.fft.irfft(left * right, size)[:window_envelope.size - cue_envelope.size + 1]
    energy = np.concatenate(([0.0], np.cumsum((window_envelope - window_envelope.mean()) ** 2)))
    denominator = np.sqrt(np.maximum(
        energy[cue_envelope.size:] - energy[:-cue_envelope.size], 1e-12)) * \
        float(np.linalg.norm(cue_envelope - cue_envelope.mean()))
    normalized = correlation / denominator
    index = int(np.argmax(np.abs(normalized)))
    return float(normalized[index]), index


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--requests", type=Path, required=True)
    parser.add_argument("--audio", type=Path, required=True)
    parser.add_argument("--wav-dir", type=Path, required=True)
    parser.add_argument("--vblank-blocks", type=int, default=14000)
    parser.add_argument("--from-step", type=int, default=0)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--limit", type=int, default=40)
    arguments = parser.parse_args()
    arguments.out.mkdir(parents=True, exist_ok=True)

    audio = read_wav(arguments.audio)
    samples_per_step = SAMPLES_PER_FRAME / arguments.vblank_blocks
    cues = {}
    for path in sorted(arguments.wav_dir.glob("bank-*-sequence-*-control-*.wav")):
        parts = path.stem.split("-")
        cues[(int(parts[1]), int(parts[3]))] = read_wav(path)

    rows = []
    with arguments.requests.open(newline="", encoding="utf-8") as handle:
        for request in csv.DictReader(handle):
            if int(request["command"]) != 0x01:
                continue
            step = int(request["step"])
            if step < arguments.from_step:
                continue
            if len(rows) >= arguments.limit:
                break
            bank, sequence = int(request["bank"]), int(request["sequence"])
            start = int(step * samples_per_step)
            before = int(WINDOW_BEFORE * SAMPLE_RATE)
            window = audio[max(0, start - before):start + int(WINDOW_AFTER * SAMPLE_RATE)]
            if window.size < 4096:
                continue
            scores = []
            for (cue_bank, cue_sequence), cue in cues.items():
                waveform, lag = normalized_correlation(cue, window)
                envelope_score, envelope_lag = envelope_correlation(cue, window)
                scores.append({
                    "bank": cue_bank, "sequence": cue_sequence,
                    "waveform": waveform, "waveform_lag_ms": 1000.0 * lag / SAMPLE_RATE,
                    "envelope": envelope_score,
                    "envelope_lag_ms": 1000.0 * envelope_lag * ENVELOPE_HOP / SAMPLE_RATE,
                })
            requested = next((score for score in scores
                              if score["bank"] == bank and score["sequence"] == sequence), None)
            best_waveform = max(scores, key=lambda score: score["waveform"])
            best_envelope = max(scores, key=lambda score: abs(score["envelope"]))
            rows.append({
                "step": step,
                "time_s": round(start / SAMPLE_RATE, 3),
                "requested": f"b{bank}-s{sequence}",
                "requested_waveform": None if requested is None else round(requested["waveform"], 3),
                "requested_waveform_lag_ms": None if requested is None else round(requested["waveform_lag_ms"], 1),
                "requested_envelope": None if requested is None else round(requested["envelope"], 3),
                "best_waveform": f"b{best_waveform['bank']}-s{best_waveform['sequence']}",
                "best_waveform_score": round(best_waveform["waveform"], 3),
                "best_envelope": f"b{best_envelope['bank']}-s{best_envelope['sequence']}",
                "best_envelope_score": round(best_envelope["envelope"], 3),
                "peak": round(float(np.max(np.abs(window))), 3),
            })

    csv_path = arguments.out / "cue-matches.csv"
    with csv_path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0].keys()) if rows else ["step"])
        writer.writeheader()
        writer.writerows(rows)
    (arguments.out / "cue-matches.json").write_text(
        json.dumps({"requests": len(rows), "rows": rows}, indent=2) + "\n", encoding="utf-8")
    confirmed = sum(1 for row in rows
                    if row["requested_waveform"] is not None
                    and row["requested"] == row["best_waveform"])
    print(f"{len(rows)} requests analysed; requested cue is the best waveform match "
          f"in {confirmed} of them")
    for row in rows[:20]:
        print(f"  t={row['time_s']:>8}s req={row['requested']:<8} "
              f"wave={row['requested_waveform']} (lag {row['requested_waveform_lag_ms']}ms) "
              f"env={row['requested_envelope']} best={row['best_waveform']}"
              f"({row['best_waveform_score']})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
