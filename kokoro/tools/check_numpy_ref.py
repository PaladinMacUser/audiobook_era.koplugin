#!/usr/bin/env python3
"""Gate the numpy reference against the torch goldens, stage by stage (M1).

Usage (through the port venv):
  kokoro/tools/py.sh kokoro/tools/check_numpy_ref.py \
      --goldens kokoro/goldens/paradee-8m --text kokoro/model/text_side.pt \
      --dec kokoro/model/decoder.pt [--sentences 0,1] [--limit 1]

torch is used here only to read the checkpoints into numpy arrays; the model
under test (numpy_model.py) never imports it. The generator's stochastic source
is fed from the golden tensor, which isolates the deterministic kernels from
RNG parity.
"""
import argparse
import json

import numpy as np
import torch

from numpy_model import Paradee

# stage -> (golden file suffix, comparison note)
STAGES = ["dur_raw", "durs", "aln", "d", "en", "F0", "N", "asr_tok", "asr",
          "src", "wave_pre_lock", "waveform"]


def load_sd(path):
    """Checkpoint -> flat numpy dict with legacy weight_norm (weight_g/weight_v)
    folded into a plain .weight, so the model code never sees either form."""
    raw = torch.load(path, map_location="cpu", weights_only=False)["model"]
    sd = {k: v.detach().numpy().astype(np.float32) for k, v in raw.items()
          if hasattr(v, "detach")}
    out = {k: v for k, v in sd.items()
           if not k.endswith(".weight_g") and not k.endswith(".weight_v")}
    for k, g in sd.items():
        if not k.endswith(".weight_g"):
            continue
        base = k[:-len(".weight_g")]
        v = sd[base + ".weight_v"]
        dims = tuple(range(1, v.ndim))          # norm over everything but dim 0
        out[base + ".weight"] = (g * v / np.sqrt((v ** 2).sum(axis=dims, keepdims=True))).astype(np.float32)
    return out


def cmp(name, mine, gold):
    a = np.asarray(mine, dtype=np.float64).ravel()
    b = np.asarray(gold, dtype=np.float64).ravel()
    if a.shape != b.shape:
        return {"stage": name, "shape": f"{a.shape} vs {b.shape}", "maxerr": float("nan"),
                "rms": float("nan"), "corr": float("nan")}
    err = np.abs(a - b).max() if a.size else 0.0
    rms = float(np.sqrt(((a - b) ** 2).mean())) if a.size else 0.0
    refmax = float(np.abs(b).max()) if a.size else 0.0
    # stages have very different scales (F0 runs to ~300, the waveform to ~0.5),
    # so the gate is relative to the reference magnitude
    rel = float(err) / max(refmax, 1e-9)
    corr = float(np.corrcoef(a, b)[0, 1]) if a.std() > 0 and b.std() > 0 else float("nan")
    return {"stage": name, "shape": str(a.shape), "maxerr": float(err), "rms": rms,
            "refmax": refmax, "rel": rel, "corr": corr}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--goldens", required=True)
    ap.add_argument("--text", required=True)
    ap.add_argument("--dec", required=True)
    ap.add_argument("--sentences", default="")
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--tol", type=float, default=1e-4)
    args = ap.parse_args()

    man = json.load(open(args.goldens + "/manifest.json"))
    which = [int(x) for x in args.sentences.split(",") if x != ""] if args.sentences else list(range(len(man["sentences"])))
    if args.limit:
        which = which[:args.limit]

    model = Paradee(load_sd(args.text), load_sd(args.dec))
    worst = {}
    for i in which:
        g = lambda name: np.load(f"{args.goldens}/s{i}_{name}.npy")
        ids = [int(x) for x in g("ids")]
        tap = {}
        src = g("src")
        out = model(ids, 1.0, src_override=src, tap=tap)
        rows = [cmp("durs", tap["durs"], g("durs")),
                cmp("dur_raw", tap["dur_raw"], g("dur_raw")),
                cmp("aln", tap["aln"], g("aln")),
                cmp("d", tap["d"].T, g("d")),
                cmp("en", tap["en"], g("en")),
                cmp("F0", tap["F0"][:len(g("F0"))], g("F0")),
                cmp("N", tap["N"][:len(g("N"))], g("N")),
                cmp("asr_tok", tap["asr_tok"], g("asr_tok")),
                cmp("asr", tap["asr"], g("asr")),
                cmp("wave_pre_lock", tap["wave_pre_lock"], g("wave_pre_lock")),
                cmp("waveform", out[0], g("waveform"))]
        print(f"--- sentence {i}: {len(ids)} ids, F={tap['aln'].shape[1]} "
              f"(golden {g('aln').shape[1]}), sample diff "
              f"{tap['wave_pre_lock'].shape[0] - g('wave_pre_lock').shape[0]:+d}")
        for r in rows:
            flag = ""
            if r["stage"] in ("durs", "aln"):
                flag = " OK" if r["maxerr"] == 0 else " MISMATCH"
            else:
                flag = " OK" if r["rel"] < args.tol else " CHECK"
            print(f"    {r['stage']:14s} {r['shape']:>16s} maxerr {r['maxerr']:10.3e} "
                  f"rel {r['rel']:9.2e} corr {r['corr']:8.5f}{flag}")
            w = worst.get(r["stage"])
            if w is None or (r["maxerr"] == r["maxerr"] and r["maxerr"] > w):
                worst[r["stage"]] = r["maxerr"]
    print("\n=== worst absolute error per stage (gate is relative; see rel above)")
    for k, v in worst.items():
        print(f"    {k:14s} {v:10.3e}")


if __name__ == "__main__":
    main()