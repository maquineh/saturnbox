"""Match in-game SCSP sequence requests to the catalogue of rendered cues.

The runtime now records every `01` sequence-start packet it sees on the host
mailbox, with the scheduler step. Because the run is deterministic, a step maps
to an exact audio sample: with `--vblank-blocks 14000` a frame is 14000 steps
and 735 samples, so `samples = step * 735 / 14000`.

For each request this script cuts the captured audio just before the request,
measures the onset and level, and scores the window against every WAV the
offline scan produced. The best matches are written next to the CSV; a low
score is a legitimate answer ("the cue is not in the catalogue" or "it is buried
under music"), and the script says so instead of forcing a match.

Usage:

    python tools/diagnostics/analyze_x4_gameplay_audio.py \
        --requests build-scsp/tmp-test/stage-requests.csv \
        --audio build-scsp/tmp-test/stage-gameplay.wav \
        --scan build-scsp/tmp-test/sequence-scan/sequence-scan.csv \
        --wav-dir build-scsp/tmp-test/sequence-scan/wav \
        --vblank-blocks 14000 \
        --out build-scsp/tmp-test/gameplay-match
"""

import argparse
import csv
import json
import wave
from pathlib import Path

import numpy as np

SAMPLE_RATE = 44100
SAMPLES_PER_FRAME = 735
FRAME = 1024
HOP = 512
BAND_COUNT = 24
CUE_SECONDS = 0.30
WINDOW_BEFORE = 0.05
WINDOW_AFTER = 0.90


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


def band_edges() -> np.ndarray:
    return np.geomspace(80.0, 16000.0, BAND_COUNT + 1)


def band_profile(samples: np.ndarray) -> np.ndarray:
    """Mean log-energy per log-spaced band over the signal."""
    if samples.size < FRAME:
        return np.zeros(BAND_COUNT, dtype=np.float32)
    window = np.hanning(FRAME).astype(np.float32)
    frequencies = np.fft.rfftfreq(FRAME, 1.0 / SAMPLE_RATE)
    edges = band_edges()
    accumulator = np.zeros(BAND_COUNT, dtype=np.float64)
    frames = 0
    for start in range(0, samples.size - FRAME + 1, HOP):
        spectrum = np.abs(np.fft.rfft(samples[start:start + FRAME] * window))
        for band in range(BAND_COUNT):
            selected = (frequencies >= edges[band]) & (frequencies < edges[band + 1])
            if np.any(selected):
                accumulator[band] += float(np.mean(spectrum[selected] ** 2 + 1e-12))
        frames += 1
    if frames == 0:
        return np.zeros(BAND_COUNT, dtype=np.float32)
    return np.log10(accumulator / frames + 1e-12).astype(np.float32)


def cosine(left: np.ndarray, right: np.ndarray) -> float:
    left = left - left.mean()
    right = right - right.mean()
    denominator = float(np.linalg.norm(left) * np.linalg.norm(right))
    return float(np.dot(left, right) / denominator) if denominator > 0.0 else 0.0


def onset_offset_ms(samples: np.ndarray, start_offset: int) -> float | None:
    """First strong positive spectral-flux peak at or after the request."""
    if samples.size < 2 * FRAME:
        return None
    window = np.hanning(FRAME).astype(np.float32)
    previous = None
    best = (0.0, None)
    for start in range(0, samples.size - FRAME + 1, HOP):
        magnitude = np.abs(np.fft.rfft(samples[start:start + FRAME] * window))
        if previous is not None:
            flux = float(np.sum(np.maximum(magnitude - previous, 0.0)))
            position = start + FRAME / 2 - start_offset
            if position >= 0 and flux > best[0]:
                best = (flux, position)
        previous = magnitude
    if best[1] is None:
        return None
    return 1000.0 * best[1] / SAMPLE_RATE


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--requests", type=Path, required=True)
    parser.add_argument("--audio", type=Path, required=True)
    parser.add_argument("--wav-dir", type=Path, required=True)
    parser.add_argument("--vblank-blocks", type=int, default=14000)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--top", type=int, default=3)
    arguments = parser.parse_args()
    arguments.out.mkdir(parents=True, exist_ok=True)

    audio = read_wav(arguments.audio)
    samples_per_step = SAMPLES_PER_FRAME / arguments.vblank_blocks

    candidates = []
    for path in sorted(arguments.wav_dir.glob("bank-*-sequence-*-control-*.wav")):
        parts = path.stem.split("-")
        bank = int(parts[1])
        sequence = int(parts[3])
        candidates.append({"bank": bank, "sequence": sequence, "path": path,
                           "profile": band_profile(read_wav(path)[:int(CUE_SECONDS * SAMPLE_RATE)])})

    rows = []
    with arguments.requests.open(newline="", encoding="utf-8") as handle:
        for request in csv.DictReader(handle):
            if int(request["command"]) != 0x01:
                continue
            step = int(request["step"])
            start = int(step * samples_per_step)
            before = int(WINDOW_BEFORE * SAMPLE_RATE)
            after = int(WINDOW_AFTER * SAMPLE_RATE)
            window = audio[max(0, start - before):start + after]
            if window.size < FRAME:
                continue
            cue = window[before:before + int(CUE_SECONDS * SAMPLE_RATE)]
            profile = band_profile(cue)
            scores = sorted(((cosine(profile, candidate["profile"]),
                              candidate["bank"], candidate["sequence"])
                             for candidate in candidates), reverse=True)[:arguments.top]
            peak = float(np.max(np.abs(window)))
            rms = float(np.sqrt(np.mean(window.astype(np.float64) ** 2)))
            onset = onset_offset_ms(window, before)
            rows.append({
                "step": step,
                # samples = step * SAMPLES_PER_FRAME / vblank_blocks.
                "time_s": round(start / SAMPLE_RATE, 3),
                "control": request["control"],
                "bank": request["bank"],
                "sequence": request["sequence"],
                "window_peak": round(peak, 3),
                "window_rms": round(rms, 4),
                "onset_ms": None if onset is None else round(onset, 1),
                **{f"match_{index + 1}": f"b{score[1]}-s{score[2]}"
                   for index, score in enumerate(scores)},
                **{f"score_{index + 1}": round(score[0], 4)
                   for index, score in enumerate(scores)},
            })

    csv_path = arguments.out / "gameplay-matches.csv"
    with csv_path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0].keys()) if rows else ["step"])
        writer.writeheader()
        writer.writerows(rows)
    summary = {
        "requests": len(rows),
        "audio_seconds": round(audio.size / SAMPLE_RATE, 2),
        "vblank_blocks": arguments.vblank_blocks,
        "catalog_entries": len(candidates),
        "matches": rows,
    }
    (arguments.out / "gameplay-matches.json").write_text(
        json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(f"{len(rows)} sequence requests in {summary['audio_seconds']}s of audio")
    for row in rows[:20]:
        print(f"  t={row['time_s']:>8}s b{row['bank']}s{row['sequence']:<3} "
              f"onset={row['onset_ms']}ms peak={row['window_peak']} "
              f"best={row['match_1']} ({row['score_1']})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
