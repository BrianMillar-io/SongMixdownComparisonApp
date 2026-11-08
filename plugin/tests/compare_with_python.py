"""Check the plugin's live measurements against the Python analyzer.

    python plugin/tests/compare_with_python.py <AnalyzerSelfTest> <song> [profile.json] [--rate 48000]

The song is converted to WAV with ffmpeg (at 44.1 kHz, or --rate to test
another session rate), measured by both, and every metric is printed side by
side with its allowed difference. Exits 1 if any metric is off by more than
that.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import tempfile
from pathlib import Path

from analyzer_app import compare as C
from analyzer_app import profiles as P
from analyzer_app.analyze import analyze_file

# How far the live numbers may differ. The live version streams in blocks,
# so loudness gating and window edges differ a little from the file version.
TOLERANCE = {
    "loudness.integrated_lufs": 0.2, "loudness.max_short_term_lufs": 0.3,
    "loudness.true_peak_dbtp": 0.3, "dynamics.crest_db": 0.3,
    "dynamics.plr_db": 0.4, "dynamics.lra_lu": 0.6,
    "dynamics.section_contrast_lu": 0.6, "tonal.tilt_db_per_oct": 0.1,
    "tonal.centroid_hz": 60, "stereo.width": 0.01, "stereo.correlation": 0.01,
    "stereo.low_correlation": 0.01, "stereo.balance_db": 0.05,
    "low_end.sub_to_bass_db": 0.3, "low_end.low_width": 0.005,
}
BAND_TOLERANCE = 0.3
WIDTH_TOLERANCE = 0.01


def tolerance(key: str) -> float:
    if key in TOLERANCE:
        return TOLERANCE[key]
    if key.startswith("tonal.bands_db."):
        return BAND_TOLERANCE
    return WIDTH_TOLERANCE


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("selftest", type=Path)
    ap.add_argument("song", type=Path)
    ap.add_argument("profile", type=Path, nargs="?")
    ap.add_argument("--rate", type=int, default=44100)
    args = ap.parse_args()

    with tempfile.TemporaryDirectory() as tmp:
        wav = Path(tmp) / "song.wav"
        subprocess.run(["ffmpeg", "-v", "error", "-i", str(args.song), "-ar",
                        str(args.rate), "-c:a", "pcm_f32le", str(wav)], check=True)
        live = json.loads(subprocess.run(
            [str(args.selftest), str(wav)] + ([str(args.profile)] if args.profile else []),
            check=True, capture_output=True, text=True).stdout)
        analysis = analyze_file(wav, musical=False)

    row = P.flatten(analysis)
    if analysis["stereo"]:
        row["stereo.balance_db"] = analysis["stereo"]["balance_db"]
    row["loudness.true_peak_dbtp"] = analysis["loudness"]["true_peak_dbtp"]
    bad = 0
    print(f"{'metric':38} {'python':>10} {'plugin':>10} {'diff':>8}")
    for key in sorted(set(row) | set(live["values"])):
        a, b = row.get(key), live["values"].get(key)
        if a is None or b is None:
            print(f"{key:38} {a!s:>10} {b!s:>10}   missing")
            if key != "musical.bpm":
                bad += 1
            continue
        diff = b - a
        flag = "" if abs(diff) <= tolerance(key) else "   TOO FAR"
        bad += bool(flag)
        print(f"{key:38} {a:10.3f} {b:10.3f} {diff:8.3f}{flag}")

    if args.profile:
        profile = json.loads(args.profile.read_text())
        py = C.compare(analysis, profile)
        rep = live["report"]
        print(f"\nscore: python {py['score']['overall']} plugin {rep['overall']}")
        print(f"areas: python {py['score']['areas']}\n       plugin {rep['areas']}")
        print("next steps (plugin):")
        for s in rep["next_steps"]:
            print("  -", s)
        if abs(py["score"]["overall"] - rep["overall"]) > 3:
            print("   SCORE TOO FAR")
            bad += 1
    print("\nOK" if not bad else f"\n{bad} problem(s)")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
