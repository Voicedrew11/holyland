"""Inspect captured native SPU2 stereo PCM without external Python packages."""

import argparse
import array
import json
import math
import sys
import wave
from pathlib import Path


def inspect(path, window_seconds):
    rows = []
    total_samples = nonzero_samples = clipped_samples = 0
    square_sum = signed_sum = 0
    peak = longest_silence = current_silence = 0
    with wave.open(str(path), "rb") as stream:
        if stream.getnchannels() != 2 or stream.getsampwidth() != 2:
            raise ValueError("Expected signed 16-bit stereo PCM")
        sample_rate = stream.getframerate()
        frames = stream.getnframes()
        offset = 0
        window = max(1, round(sample_rate * window_seconds))
        while raw := stream.readframes(window):
            values = array.array("h", raw)
            if sys.byteorder != "little":
                values.byteswap()
            count = len(values)
            local_nonzero = sum(value != 0 for value in values)
            local_clipped = sum(abs(value) >= 32767 for value in values)
            local_squares = sum(value * value for value in values)
            local_sum = sum(values)
            local_peak = max(map(abs, values), default=0)
            different = sum(values[i] != values[i + 1] for i in range(0, count, 2))
            for i in range(0, count, 2):
                if values[i] == 0 and values[i + 1] == 0:
                    current_silence += 1
                    longest_silence = max(longest_silence, current_silence)
                else:
                    current_silence = 0
            rows.append({
                "start_seconds": round(offset / sample_rate, 6),
                "duration_seconds": round(count / (2 * sample_rate), 6),
                "peak": local_peak,
                "rms": round(math.sqrt(local_squares / count), 3),
                "dc": round(local_sum / count, 3),
                "nonzero_fraction": round(local_nonzero / count, 6),
                "clipped_samples": local_clipped,
                "stereo_difference_fraction": round(different / (count / 2), 6),
            })
            offset += count // 2
            total_samples += count
            nonzero_samples += local_nonzero
            clipped_samples += local_clipped
            square_sum += local_squares
            signed_sum += local_sum
            peak = max(peak, local_peak)
    return {
        "file": str(path.resolve()), "sample_rate": sample_rate,
        "channels": 2, "frames": frames,
        "duration_seconds": frames / sample_rate,
        "decoded_frames": total_samples // 2, "peak": peak,
        "rms": math.sqrt(square_sum / total_samples) if total_samples else 0,
        "dc": signed_sum / total_samples if total_samples else 0,
        "nonzero_samples": nonzero_samples,
        "clipped_samples": clipped_samples,
        "longest_digital_silence_seconds": longest_silence / sample_rate,
        "windows": rows,
    }


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("wav", type=Path)
    parser.add_argument("--window", type=float, default=1.0)
    parser.add_argument("--json", type=Path)
    args = parser.parse_args()
    result = inspect(args.wav, args.window)
    if args.json:
        args.json.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({key: value for key, value in result.items() if key != "windows"}, indent=2))
