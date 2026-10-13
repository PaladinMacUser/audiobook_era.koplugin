#!/usr/bin/env python3
"""Dump reference goldens for the Kokoro-family C port (M0).

Runs the author's shipped inference path in PyTorch fp32 on a fixed sentence
set and writes every stage boundary as a .npy plus a manifest with shape and
dtype, then a SHA256SUMS over the whole set. The C port is gated against these
(PyTorch is the golden source; the shipped ONNX is not usable as one because it
contains RandomUniformLike/RandomNormalLike in the sine source).

Reference assembly: the Paradee/ConvSTFT/Lock classes below mirror
github.com/sahilmahendrakar/paradee at commit 9c8b4d75 (Apache-2.0), file
training/scripts/export_paradee.py. They are inlined rather than imported
because that file runs its export at import time.

Usage:
  kokoro/tools/py.sh kokoro/tools/gen_goldens.py \
      --repo kokoro/model/paradee-repo --text kokoro/model/text_side.pt \
      --dec kokoro/model/decoder.pt --sentences dev/paradee/sentences.txt \
      --out kokoro/goldens/paradee-8m --seed 0
"""
import argparse
import hashlib
import json
import os
import sys
import warnings

warnings.filterwarnings("ignore")

import numpy as np
import torch
import torch.nn as nn
import torch.nn.functional as F

# torch 2.6+ defaults torch.load to weights_only=True, which rejects the
# author's checkpoint dicts (they carry optimizer/step state beside the
# weights). These are the published Paradee checkpoints, loaded locally.
_torch_load = torch.load


def _load_compat(*a, **kw):
    kw.setdefault("weights_only", False)
    return _torch_load(*a, **kw)


torch.load = _load_compat

N_FFT, HOP, SR = 1024, 256, 24000
GEN_HOP = 300          # 10 * 6 * 5: samples per generator frame
LOCK_L = 33            # export_paradee.py --L default


# --------------------------------------------------------------------------
# Reference assembly (mirrors export_paradee.py; see module docstring).
# --------------------------------------------------------------------------
class ConvSTFT(nn.Module):
    """The ONNX-friendly stand-in for kokoro's CustomSTFT that matches
    torch.stft/torch.istft: reflect padding, zeroed DC/Nyquist imaginary
    parts, and an inverse that halves DC/Nyquist and divides by the window
    overlap envelope."""

    def __init__(self, n, hop):
        super().__init__()
        self.n, self.hop = n, hop
        w = torch.hann_window(n)
        k = torch.arange(n // 2 + 1).float()[:, None]
        ang = 2 * torch.pi * k * torch.arange(n).float() / n
        re, im = torch.cos(ang) * w, -torch.sin(ang) * w
        im[0] = 0
        im[-1] = 0
        self.register_buffer("fwd", torch.cat([re, im])[:, None])
        c = torch.full((n // 2 + 1, 1), 2.0)
        c[0] = c[-1] = 1
        self.register_buffer("inv", (torch.cat([torch.cos(ang) * c, -torch.sin(ang) * c]) / n * w)[:, None])
        self.register_buffer("w2", (w ** 2)[None, None])

    def transform(self, x):
        y = F.conv1d(F.pad(x[:, None], (self.n // 2, self.n // 2), mode="reflect"), self.fwd, stride=self.hop)
        K = self.n // 2 + 1
        re, im = y[:, :K], y[:, K:]
        ph = torch.where((im == 0) & (re < 0), torch.full_like(re, torch.pi), torch.atan2(im, re))
        return torch.sqrt(re ** 2 + im ** 2), ph

    def inverse(self, mag, phase):
        z = torch.cat([mag * torch.cos(phase), mag * torch.sin(phase)], 1)
        y = F.conv_transpose1d(z, self.inv, stride=self.hop)[:, 0]
        env = F.conv_transpose1d(torch.ones(1, 1, mag.shape[-1]), self.w2, stride=self.hop)[:, 0]
        return (y / env.clamp_min(1e-8))[:, self.n // 2: -(self.n // 2)][:, None]


class Lock(nn.Module):
    """Phase-lock filter without complex numbers: repair the 2-8 kHz band of
    voiced frames toward the harmonic source's phase, keep the magnitude."""

    def __init__(self, L):
        super().__init__()
        from phase_lock import BAND
        self.bins = torch.nonzero(BAND[:, 0]).squeeze(1)
        w = torch.hann_window(N_FFT)
        n = torch.arange(N_FFT).float()
        k = self.bins.float()[:, None]
        ang = 2 * torch.pi * k * n / N_FFT
        self.register_buffer("fwd", torch.cat([torch.cos(ang) * w, -torch.sin(ang) * w])[:, None])
        self.register_buffer("inv", (torch.cat([torch.cos(ang), -torch.sin(ang)]) * (2 / N_FFT) * w)[:, None])
        self.register_buffer("w2", (w ** 2)[None, None])
        sk = torch.hann_window(L + 2)[1:-1]
        self.register_buffer("sk", (sk / sk.sum())[None, None])
        self.register_buffer("sk_pad", torch.tensor(L // 2))
        self.pad = L // 2

    def stft(self, x):
        y = F.conv1d(F.pad(x[:, None], (N_FFT // 2, N_FFT // 2), mode="reflect"), self.fwd, stride=HOP)[0]
        return y[:len(self.bins)], y[len(self.bins):]

    def forward(self, s, src, f0):
        k = min(s.shape[1], src.shape[1])
        s, src = s[:, :k], src[:, :k]
        Sr, Si = self.stft(s)
        Er, Ei = self.stft(src)
        nf = Sr.shape[1]
        j = torch.clamp(torch.arange(nf) * HOP // GEN_HOP, max=f0.shape[1] - 1)
        v = (f0[0, j] > 60).float()[None]
        Em = torch.sqrt(Er ** 2 + Ei ** 2).clamp_min(1e-6)
        Rr, Ri = (Sr * Er + Si * Ei) / Em, (Si * Er - Sr * Ei) / Em
        sm = lambda z: F.conv1d(z[:, None], self.sk, padding=self.pad)[:, 0]
        Rr, Ri = sm(Rr), sm(Ri)
        Rm = torch.sqrt(Rr ** 2 + Ri ** 2).clamp_min(1e-12)
        Sm = torch.sqrt(Sr ** 2 + Si ** 2)
        ur, ui = (Er * Rr - Ei * Ri) / (Em * Rm), (Er * Ri + Ei * Rr) / (Em * Rm)
        Dr, Di = (Sm * ur - Sr) * v, (Sm * ui - Si) * v
        ola = F.conv_transpose1d(torch.cat([Dr, Di])[None], self.inv, stride=HOP)[0]
        env = F.conv_transpose1d(torch.ones(1, 1, nf), self.w2, stride=HOP)[0]
        d = (ola / env.clamp_min(1e-8))[:, N_FFT // 2: N_FFT // 2 + k]
        return s + d


def strip_weight_norm(m):
    from torch.nn.utils import remove_weight_norm
    from torch.nn.utils.parametrize import is_parametrized, remove_parametrizations
    for x in m.modules():
        try:
            remove_weight_norm(x)
        except Exception:
            pass
        if is_parametrized(x, "weight"):
            try:
                remove_parametrizations(x, "weight", leave_parametrized=True)
            except Exception:
                pass


class Paradee(nn.Module):
    def __init__(self, ts, ds, L):
        super().__init__()
        self.ts, self.ds, self.lock = ts, ds, Lock(L)
        self.cap = {}
        ds.generator.m_source.register_forward_hook(lambda m, i, o: self.cap.__setitem__("src", o[0]))

    def text(self, ids):
        t = self.ts
        p = t.predictor
        s = t.style
        d_en = t.bert_encoder(t.bert(ids, attention_mask=torch.ones_like(ids))).transpose(-1, -2)
        x = d_en
        T = ids.shape[1]
        sx = s[:, :, None].expand(-1, -1, T)
        x = torch.cat([x, sx], 1)
        for b in p.text_encoder.lstms:
            if isinstance(b, nn.LSTM):
                x = b(x.transpose(-1, -2))[0].transpose(-1, -2)
            else:
                x = torch.cat([b(x.transpose(-1, -2), s).transpose(-1, -2), sx], 1)
        d = x.transpose(-1, -2)
        dur = torch.sigmoid(p.duration_proj(p.lstm(d)[0])).sum(-1)[0]
        e = t.text_encoder
        h = e.embedding(ids).transpose(1, 2)
        for c in e.cnn:
            h = c(h)
        t_en = e.lstm(h.transpose(1, 2))[0]
        asr_tok = t.asr_proj(t_en).transpose(-1, -2)
        return d, dur, asr_tok

    def forward(self, input_ids, speed=1.0, tap=None):
        d, dur, asr_tok = self.text(input_ids)
        n = torch.clamp(torch.round(dur / speed), min=1).long()
        end = torch.cumsum(n, 0)
        start = end - n
        f = torch.arange(end[-1])[None]
        aln = ((f >= start[:, None]) & (f < end[:, None])).float()[None]
        en = d.transpose(-1, -2) @ aln
        F0, N = self.ts.predictor.F0Ntrain(en, self.ts.style)
        asr = asr_tok @ aln
        F0i, Ni = F0[:, :2 * aln.shape[1]], N[:, :2 * aln.shape[1]]
        if tap is not None:
            tap.update(d=d, dur=dur, n=n, aln=aln, en=en, F0=F0, N=N,
                       asr_tok=asr_tok, asr=asr, F0_in=F0i, N_in=Ni)
        s = self.ds(asr, F0, N).squeeze(1)
        if tap is not None:
            tap.update(wave_pre_lock=s)
        return self.lock(s, self.cap["src"].squeeze(-1), F0)


def load_reference(repo, text_pt, dec_pt):
    sys.path.insert(0, os.path.join(repo, "training", "scripts"))
    from student import TextStudent
    from student_decoder import load_student_decoder
    ts = TextStudent("s+mlp").eval()
    ts.load_state_dict(torch.load(text_pt, map_location="cpu")["model"])
    ds = load_student_decoder("A", dec_pt, disable_complex=True)
    strip_weight_norm(ts)
    strip_weight_norm(ds)
    ds.generator.stft = ConvSTFT(ds.generator.stft.filter_length, ds.generator.stft.hop_length)
    return Paradee(ts, ds, LOCK_L).eval()


def phonemize(text):
    from misaki.en import G2P
    from misaki.espeak import EspeakFallback
    g2p = G2P(fallback=EspeakFallback(british=False))
    ps, _ = g2p(text)
    return ps


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", required=True)
    ap.add_argument("--text", required=True)
    ap.add_argument("--dec", required=True)
    ap.add_argument("--config", required=True, help="Paradee config.json (carries the id vocab)")
    ap.add_argument("--sentences", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--limit", type=int, default=0)
    args = ap.parse_args()

    cfg = json.load(open(args.config))
    vocab = cfg.get("vocab")
    if vocab is None:
        sys.exit("no vocab in config.json")

    model = load_reference(args.repo, args.text, args.dec)
    lines = [l.strip() for l in open(args.sentences) if l.strip()]
    if args.limit:
        lines = lines[:args.limit]
    os.makedirs(args.out, exist_ok=True)

    manifest = []
    for i, line in enumerate(lines):
        ps = phonemize(line)
        ids = [0] + [vocab[c] for c in ps if c in vocab][:510] + [0]
        t = torch.tensor([ids], dtype=torch.long)
        tap = {}
        torch.manual_seed(args.seed + i)      # the source module is stochastic
        with torch.no_grad():
            wave = model(t, 1.0, tap)
        stages = {"ids": np.array(ids, dtype=np.int64),
                  "phonemes": np.array(list(ps), dtype="U1"),
                  "dur_raw": tap["dur"].numpy(), "durs": tap["n"].numpy(),
                  "aln": tap["aln"][0].numpy(), "d": tap["d"][0].numpy(),
                  "en": tap["en"][0].numpy(), "F0": tap["F0"][0].numpy(), "N": tap["N"][0].numpy(),
                  "asr_tok": tap["asr_tok"][0].numpy(), "asr": tap["asr"][0].numpy(),
                  "F0_in": tap["F0_in"][0].numpy(), "N_in": tap["N_in"][0].numpy(),
                  "src": model.cap["src"].squeeze(-1).numpy(),
                  "wave_pre_lock": tap["wave_pre_lock"].squeeze().numpy(),
                  "waveform": wave.squeeze().numpy()}
        for name, arr in stages.items():
            p = os.path.join(args.out, f"s{i}_{name}.npy")
            np.save(p, arr)
            manifest.append({"sentence": i, "stage": name, "file": os.path.basename(p),
                             "shape": list(arr.shape), "dtype": str(arr.dtype)})
        print(f"s{i}: {len(ids)} ids, F={stages['aln'].shape[1]}, "
              f"samples={stages['waveform'].shape[0]} ({stages['waveform'].shape[0] / SR:.2f}s)")

    with open(os.path.join(args.out, "manifest.json"), "w") as fh:
        json.dump({"sentences": lines, "seed": args.seed, "lock_l": LOCK_L,
                   "sr": SR, "gen_hop": GEN_HOP, "stages": manifest}, fh, indent=1)
    os.system(f"cd '{args.out}' && sha256sum *.npy manifest.json > SHA256SUMS")
    print("goldens in", args.out)


if __name__ == "__main__":
    main()