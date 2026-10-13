#!/usr/bin/env python3
"""Bisect the numpy generator against torch via hooks on torch's own modules.

Both sides run the real model code (the model's StudentDecoder/Generator with
taps, and the author's torch StudentDecoder), so the probe cannot drift from
the model. Inputs come from the goldens (ids, and the stochastic source), so
any divergence is inside the deterministic kernels.

Usage: kokoro/tools/py.sh kokoro/tools/debug_gen.py --repo kokoro/model/paradee-repo \
         --text kokoro/model/text_side.pt --dec kokoro/model/decoder.pt \
         --goldens kokoro/goldens/paradee-8m --sentence 0
"""
import argparse
import sys

import numpy as np
import torch

sys.path.insert(0, "kokoro/tools")
from check_numpy_ref import load_sd  # noqa: E402
from numpy_model import Paradee  # noqa: E402


def err(name, mine, ref):
    a = np.asarray(mine, dtype=np.float64).reshape(-1)
    b = np.asarray(ref.detach().numpy() if hasattr(ref, "detach") else ref,
                   dtype=np.float64).reshape(-1)
    if a.size != b.size:
        print(f"  {name:22s} SIZE {a.size} vs {b.size}")
        return
    corr = np.corrcoef(a, b)[0, 1] if a.std() and b.std() else float("nan")
    print(f"  {name:22s} n={a.size:8d} maxerr {np.abs(a - b).max():10.3e} corr {corr:8.5f} "
          f"|mine| {np.abs(a).max():9.3e} |ref| {np.abs(b).max():9.3e}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", required=True)
    ap.add_argument("--text", required=True)
    ap.add_argument("--dec", required=True)
    ap.add_argument("--goldens", required=True)
    ap.add_argument("--sentence", type=int, default=0)
    args = ap.parse_args()
    sys.path.insert(0, args.repo + "/training/scripts")

    from student_decoder import load_student_decoder
    g = args.goldens
    model = Paradee(load_sd(args.text), load_sd(args.dec))
    ids = [int(v) for v in np.load(f"{g}/s{args.sentence}_ids.npy")]
    src = np.load(f"{g}/s{args.sentence}_src.npy").reshape(-1)

    tap, gtap = {}, {}
    model(ids, 1.0, src_override=src, tap=tap, gen_tap=gtap)
    ds = load_student_decoder("A", args.dec, disable_complex=True)
    style_dec = torch.tensor(model.style_dec, dtype=torch.float32)[None]

    caps = {}
    gen = ds.generator
    handles = []
    for i, m in enumerate(gen.ups):
        handles.append(m.register_forward_hook(lambda mod, inp, out, i=i: caps.__setitem__(f"gen_ups{i}", out)))
    handles.append(gen.conv_post.register_forward_hook(lambda mod, inp, out: caps.__setitem__("gen_conv_post", out)))

    with torch.no_grad():
        wave_t = ds(torch.tensor(tap["asr"], dtype=torch.float32)[None],
                    torch.tensor(tap["F0"], dtype=torch.float32)[None],
                    torch.tensor(tap["N"], dtype=torch.float32)[None])
    for h in handles:
        h.remove()

    print("\n--- generator stages (numpy vs torch hooks)")
    for k in ("gen_ups0", "gen_ups1", "gen_conv_post"):
        if k in caps:
            err(k, gtap.get(k), caps[k])
    err("wave_pre_lock", gtap.get("gen_out"), wave_t)


if __name__ == "__main__":
    main()