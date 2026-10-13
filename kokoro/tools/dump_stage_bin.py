#!/usr/bin/env python3
"""Dump golden stages as flat binaries for the C gates.

Writes, per sentence, the stages the C runtime is checked against, as raw
little-endian arrays plus a manifest with shapes (the fixture contract style
used by sanotts/mcu: regenerate the whole set, never edit one file).

Usage: kokoro/tools/py.sh kokoro/tools/dump_stage_bin.py \
         --goldens kokoro/goldens/paradee-8m --out kokoro/test/fixtures/paradee-8m
"""
import argparse
import json
from pathlib import Path

import numpy as np

# stage name -> (dtype, note)
STAGES = {
    "ids": ("<i4", "phoneme ids, padded with 0 at both ends"),
    "dur_raw": ("<f4", "per-token pre-round durations"),
    "durs": ("<i4", "rounded frame counts"),
    "d": ("<f4", "prosody features, time-major [T, d_hid+style]"),
    "asr_tok": ("<f4", "asr projection, [asr_dim, T]"),
    "en": ("<f4", "aligned prosody features, [d_hid+style, F]"),
    "asr": ("<f4", "aligned asr, [asr_dim, F]"),
    "F0": ("<f4", "[2F]"),
    "N": ("<f4", "[2F]"),
    "src": ("<f4", "harmonic source, 6 * F * F0S samples"),
    "wave_pre_lock": ("<f4", "generator output before the phase lock"),
    "waveform": ("<f4", "final waveform"),
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--goldens", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--sentences", default="")
    args = ap.parse_args()
    g = Path(args.goldens)
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    man = json.loads((g / "manifest.json").read_text())
    which = [int(x) for x in args.sentences.split(",") if x != ""] or list(range(len(man["sentences"])))

    index = {"sr": man["sr"], "sentences": []}
    for i in which:
        entry = {"index": i, "text": man["sentences"][i], "files": {}}
        for stage, (dt, note) in STAGES.items():
            arr = np.load(g / f"s{i}_{stage}.npy")
            arr = np.ascontiguousarray(arr.astype(np.dtype(dt)))
            name = f"s{i}_{stage}.bin"
            (out / name).write_bytes(arr.tobytes())
            entry["files"][stage] = {"name": name, "dtype": dt, "shape": list(arr.shape), "note": note}
        index["sentences"].append(entry)
        print(f"s{i}: " + " ".join(f"{k}{tuple(v['shape'])}" for k, v in entry["files"].items()))

    (out / "index.json").write_text(json.dumps(index, indent=1))
    print("wrote", out)


if __name__ == "__main__":
    main()