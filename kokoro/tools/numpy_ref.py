#!/usr/bin/env python3
"""Torch-free numpy primitives for the Kokoro-family pipeline (M1).

Pins the exact semantics the C port must reproduce. No torch import in this
module or in numpy_model.py: the caller loads checkpoints with torch and hands
over plain dicts of numpy arrays.

Layout convention: channel-major [C, T], matching the C runtime. Module maths
mirror kokoro 0.9.4 (modules.py, istftnet.py) and the Paradee student wrappers
(github.com/sahilmahendrakar/paradee at 9c8b4d75; Apache-2.0).
"""
import math

import numpy as np

SR = 24000


# --------------------------------------------------------------------------
# primitives
# --------------------------------------------------------------------------
def conv1d(x, w, b=None, stride=1, pad=0, dilation=1, groups=1):
    """x [Cin, T] -> [Cout, T']; w [Cout, Cin/groups, K]. Zero padding, like
    PyTorch with an explicit padding argument."""
    Cin, T = x.shape
    Cout, Cg, K = w.shape
    if Cin != Cg * groups:
        raise ValueError(f"conv1d: x channels {Cin}, w groups {groups}x{Cg}")
    if pad:
        x = np.pad(x, ((0, 0), (pad, pad)))
    Tp = (x.shape[1] - dilation * (K - 1) - 1) // stride + 1
    out = np.zeros((Cout, Tp), dtype=np.float32)
    xg = x.reshape(groups, Cin // groups, x.shape[1])
    wg = w.reshape(groups, Cout // groups, Cg, K)
    og = out.reshape(groups, Cout // groups, Tp)
    for g in range(groups):
        for k in range(K):
            sl = xg[g][:, k * dilation:k * dilation + stride * Tp:stride]
            og[g] += np.einsum("ct,oc->ot", np.ascontiguousarray(sl[:, :Tp]), wg[g][:, :, k], optimize=True)
    if b is not None:
        out += b[:, None]
    return out


def convtr1d(x, w, b=None, stride=1, pad=0, out_pad=0, groups=1):
    """ConvTranspose1d, scatter form; w [Cin, Cout/groups, K]."""
    Cin, T = x.shape
    Cg, Cgout, K = w.shape
    Cout = Cgout * groups
    Tp = (T - 1) * stride - 2 * pad + K + out_pad
    out = np.zeros((Cout, Tp), dtype=np.float32)
    cing = Cin // groups
    for g in range(groups):
        for k in range(K):
            idx = np.arange(T) * stride + (k - pad)
            m = (idx >= 0) & (idx < Tp)
            if not m.any():
                continue
            src = np.ascontiguousarray(x[g * cing:(g + 1) * cing][:, m])
            wk = w[g * cing:(g + 1) * cing][:, :, k]   # [cing, Cgout]
            contrib = np.einsum("it,io->ot", src, wk, optimize=True)
            out[g * Cgout:(g + 1) * Cgout][:, idx[m]] += contrib
    if b is not None:
        out += b[:, None]
    return out


def linear(x, w, b=None):
    """x [Cin, T] -> [Cout, T]; w [Cout, Cin]."""
    y = w @ x
    return y + b[:, None] if b is not None else y


def layer_norm(x, gamma, beta, eps=1e-5):
    """Over channels, [C, T]."""
    mu = x.mean(axis=0, keepdims=True)
    var = x.var(axis=0, keepdims=True)
    return (x - mu) / np.sqrt(var + eps) * gamma[:, None] + beta[:, None]


def inst_norm(x, gamma, beta, eps=1e-5):
    """InstanceNorm1d(affine=True): stats per channel over time."""
    mu = x.mean(axis=1, keepdims=True)
    var = x.var(axis=1, keepdims=True)
    return (x - mu) / np.sqrt(var + eps) * gamma[:, None] + beta[:, None]


def adain(x, s, sd, p, eps=1e-5):
    """AdaIN1d at checkpoint prefix p: (1+g)*InstanceNorm(x)+b, g/b from Linear(s)."""
    h = sd[p + ".fc.weight"] @ s + sd[p + ".fc.bias"]
    g, b = np.split(h, 2)
    return (1.0 + g[:, None]) * inst_norm(x, sd[p + ".norm.weight"], sd[p + ".norm.bias"], eps) + b[:, None]


def ada_layer_norm(x, s, sd, p, eps=1e-5):
    """AdaLayerNorm on [C, T]: plain layer_norm over channels, then affine."""
    h = sd[p + ".fc.weight"] @ s + sd[p + ".fc.bias"]
    g, b = np.split(h, 2)
    xn = layer_norm(x, np.ones_like(g), np.zeros_like(b), eps)
    return (1.0 + g[:, None]) * xn + b[:, None]


def snake(x, alpha):
    """x + (1/alpha)*sin(alpha*x)^2 with alpha per channel ([C])."""
    a = alpha[:, None]
    return x + (1.0 / a) * np.sin(a * x) ** 2


def leaky_relu(x, slope=0.2):
    return np.where(x >= 0, x, x * slope)


def _erf(x):
    from math import erf
    return np.vectorize(erf, otypes=[np.float32])(x)


def gelu(x):
    """Exact erf form (HF 'gelu', nn.GELU default): the asr projection's."""
    return (0.5 * x * (1.0 + _erf(x * np.float32(1.0 / math.sqrt(2.0))))).astype(np.float32)


def gelu_new(x):
    """Tanh approximation (HF 'gelu_new'): what AlbertConfig selects for the
    ALBERT layer's FFN. Not interchangeable with the erf form."""
    return (0.5 * x * (1.0 + np.tanh(np.float32(math.sqrt(2.0 / math.pi))
                                     * (x + np.float32(0.044715) * x ** 3)))).astype(np.float32)


def softmax(x, axis=-1):
    m = x.max(axis=axis, keepdims=True)
    e = np.exp(x - m)
    return e / e.sum(axis=axis, keepdims=True)


def sigmoid(x):
    return 1.0 / (1.0 + np.exp(-x))


def upsample_nearest(x, factor):
    """F.interpolate(scale_factor=factor, mode='nearest') on the time axis."""
    return np.repeat(x, factor, axis=-1)


def interp_linear_time(x, scale):
    """F.interpolate(mode='linear', align_corners=False) on the time axis:
    output[j] samples the input at (j + 0.5)/scale - 0.5, edge-clamped."""
    C, T = x.shape
    T_o = int(math.floor(T * scale))
    pos = np.clip((np.arange(T_o) + 0.5) / scale - 0.5, 0, T - 1)
    i0 = np.floor(pos).astype(int)
    i1 = np.minimum(i0 + 1, T - 1)
    w = (pos - i0).astype(np.float32)
    return (x[:, i0] * (1 - w) + x[:, i1] * w).astype(np.float32)


def smooth_rows(z, kernel, pad):
    """F.conv1d(z[:, None], kernel[None, None], padding=pad)[:, 0]: each row of
    z [K, T] smoothed on its own by the same 1-tap kernel."""
    k = np.asarray(kernel).reshape(-1)
    T = z.shape[1]
    zp = np.pad(z, ((0, 0), (pad, pad)))
    out = np.zeros((z.shape[0], T), dtype=np.float32)
    for j in range(k.size):
        out += k[j] * zp[:, j:j + T]
    return out


def lstm(x, w_ih, w_hh, b_ih, b_hh, reverse=False):
    """One direction: x [T, Cin] -> h [T, H]. PyTorch gate order i, f, g, o."""
    T = x.shape[0]
    H = w_hh.shape[1]
    h = np.zeros(H, dtype=np.float32)
    c = np.zeros(H, dtype=np.float32)
    out = np.zeros((T, H), dtype=np.float32)
    order = range(T - 1, -1, -1) if reverse else range(T)
    for n, t in enumerate(order):
        gates = w_ih @ x[t] + b_ih + w_hh @ h + b_hh
        i, f, g, o = np.split(gates, 4)
        c = sigmoid(f) * c + sigmoid(i) * np.tanh(g)
        h = sigmoid(o) * np.tanh(c)
        out[n] = h
    # PyTorch aligns the backward output to the input index (out[t] is the
    # state after consuming inputs T-1..t), so it is the reverse of the order
    # in which the states are produced.
    return out[::-1] if reverse else out


def bilstm(x, sd, p):
    """x [Cin, T] -> [2H, T]: forward output then backward output."""
    xT = np.ascontiguousarray(x.T)
    f = lstm(xT, sd[p + ".weight_ih_l0"], sd[p + ".weight_hh_l0"],
             sd[p + ".bias_ih_l0"], sd[p + ".bias_hh_l0"])
    b = lstm(xT, sd[p + ".weight_ih_l0_reverse"], sd[p + ".weight_hh_l0_reverse"],
             sd[p + ".bias_ih_l0_reverse"], sd[p + ".bias_hh_l0_reverse"], reverse=True)
    return np.concatenate([f, b], axis=1).T


# --------------------------------------------------------------------------
# spectral
# --------------------------------------------------------------------------
def _hann_periodic(n):
    """torch.hann_window(n) (periodic=True)."""
    return np.hanning(n + 1)[:n].astype(np.float32)


class ConvSTFT:
    """The author's ONNX-friendly STFT (export_paradee.py): reflect padding,
    DC/Nyquist imaginary parts zeroed, inverse scaled by 1/n and divided by the
    window-overlap envelope."""

    def __init__(self, n, hop):
        self.n, self.hop = n, hop
        w = _hann_periodic(n)
        k = np.arange(n // 2 + 1, dtype=np.float32)[:, None]
        ang = (2 * np.pi * k * np.arange(n, dtype=np.float32) / n).astype(np.float32)
        re, im = np.cos(ang) * w, -np.sin(ang) * w
        im[0] = 0
        im[-1] = 0
        self.fwd = np.concatenate([re, im], axis=0)[:, None, :].astype(np.float32)
        c = np.full((n // 2 + 1, 1), 2.0, dtype=np.float32)
        c[0] = c[-1] = 1
        self.inv = (np.concatenate([np.cos(ang) * c, -np.sin(ang) * c], axis=0) / n * w)[:, None, :].astype(np.float32)
        self.w2 = (w ** 2)[None, None, :].astype(np.float32)

    def transform(self, x):
        """x [1, T] -> (mag [K, F], phase [K, F])."""
        xp = np.pad(x, ((0, 0), (self.n // 2, self.n // 2)), mode="reflect")
        y = conv1d(xp, self.fwd, stride=self.hop)
        K = self.n // 2 + 1
        re, im = y[:K], y[K:]
        ph = np.where((im == 0) & (re < 0), np.float32(np.pi), np.arctan2(im, re)).astype(np.float32)
        return np.sqrt(re ** 2 + im ** 2), ph

    def inverse(self, mag, phase):
        """mag, phase [K, F] -> [1, S]."""
        z = np.concatenate([mag * np.cos(phase), mag * np.sin(phase)], axis=0)
        y = convtr1d(z, self.inv, stride=self.hop)[0]
        env = convtr1d(np.ones((1, mag.shape[1]), dtype=np.float32), self.w2, stride=self.hop)[0]
        return (y / np.maximum(env, 1e-8))[self.n // 2: -(self.n // 2)][None, :]


class Lock:
    """Phase-lock post-filter (export_paradee.py Lock, L=33): repair the 2-8 kHz
    band of voiced frames toward the source phase, keep the magnitude."""

    def __init__(self, L=33, n=1024, hop=256, sr=SR):
        fr = np.arange(n // 2 + 1) * sr / n
        self.bins = np.where((fr >= 2000) & (fr < 8000))[0]
        self.n, self.hop, self.pad, self.L = n, hop, L // 2, L
        w = _hann_periodic(n)
        nn = np.arange(n, dtype=np.float32)
        k = self.bins.astype(np.float32)[:, None]
        ang = (2 * np.pi * k * nn / n).astype(np.float32)
        self.fwd = np.concatenate([np.cos(ang) * w, -np.sin(ang) * w], axis=0)[:, None, :].astype(np.float32)
        self.inv = (np.concatenate([np.cos(ang), -np.sin(ang)], axis=0) * (2 / n) * w)[:, None, :].astype(np.float32)
        self.w2 = (w ** 2)[None, None, :].astype(np.float32)
        # torch.hann_window(L + 2)[1:-1]: periodic window of L+2 points, ends dropped
        sk = _hann_periodic(L + 2)[1:-1].astype(np.float32)
        self.sk = (sk / sk.sum())[None, None, :].astype(np.float32)

    def stft(self, x):
        xp = np.pad(x, ((0, 0), (self.n // 2, self.n // 2)), mode="reflect")
        y = conv1d(xp, self.fwd, stride=self.hop)
        K = len(self.bins)
        return y[:K], y[K:]

    def __call__(self, s, src, f0, gen_hop=300):
        """s [1, S], src [S], f0 [2F] -> [1, S]."""
        k = min(s.shape[1], src.shape[0])
        s, src = s[:, :k], src[:k][None, :]
        Sr, Si = self.stft(s)
        Er, Ei = self.stft(src)
        j = np.clip(np.arange(Sr.shape[1]) * self.hop // gen_hop, 0, len(f0) - 1)
        v = (f0[j] > 60).astype(np.float32)[None, :]
        Em = np.maximum(np.sqrt(Er ** 2 + Ei ** 2), 1e-6)
        Rr, Ri = (Sr * Er + Si * Ei) / Em, (Si * Er - Sr * Ei) / Em
        Rr, Ri = smooth_rows(Rr, self.sk[0, 0], self.pad), smooth_rows(Ri, self.sk[0, 0], self.pad)
        Rm = np.maximum(np.sqrt(Rr ** 2 + Ri ** 2), 1e-12)
        Sm = np.sqrt(Sr ** 2 + Si ** 2)
        ur, ui = (Er * Rr - Ei * Ri) / (Em * Rm), (Er * Ri + Ei * Rr) / (Em * Rm)
        Dr, Di = (Sm * ur - Sr) * v, (Sm * ui - Si) * v
        ola = convtr1d(np.concatenate([Dr, Di], axis=0), self.inv, stride=self.hop)[0]
        env = convtr1d(np.ones((1, Sr.shape[1]), dtype=np.float32), self.w2, stride=self.hop)[0]
        d = (ola / np.maximum(env, 1e-8))[self.n // 2: self.n // 2 + k]
        return s + d[None, :]


# --------------------------------------------------------------------------
# blocks shared by the two halves
# --------------------------------------------------------------------------
def adain_res_blk1d(x, s, sd, p, upsample=False):
    """AdainResBlk1d: AdaIN -> leaky0.2 -> [learned depthwise ConvT upsample] ->
    conv1 -> AdaIN -> leaky0.2 -> conv2, plus a nearest-upsampled (and 1x1
    projected) shortcut, scaled by 1/sqrt(2)."""
    r = adain(x, s, sd, p + ".norm1")
    r = leaky_relu(r, 0.2)
    if upsample:
        r = convtr1d(r, sd[p + ".pool.weight"], sd[p + ".pool.bias"], stride=2, pad=1, out_pad=1,
                     groups=r.shape[0])
    r = conv1d(r, sd[p + ".conv1.weight"], sd[p + ".conv1.bias"], pad=1)
    r = adain(r, s, sd, p + ".norm2")
    r = leaky_relu(r, 0.2)
    r = conv1d(r, sd[p + ".conv2.weight"], sd[p + ".conv2.bias"], pad=1)
    sc = upsample_nearest(x, 2) if upsample else x
    if p + ".conv1x1.weight" in sd:
        sc = conv1d(sc, sd[p + ".conv1x1.weight"])
    return (r + sc) * np.float32(1.0 / math.sqrt(2.0))


def adain_res_block1(x, s, sd, p, kernel):
    """AdaINResBlock1 (generator resblocks): 3 branches with dilations 1/3/5 and
    Snake activations, residual outside."""
    for i in range(3):
        d1 = (1, 3, 5)[i]
        xt = adain(x, s, sd, f"{p}.adain1.{i}")
        xt = snake(xt, sd[f"{p}.alpha1.{i}"][0][:, 0])
        xt = conv1d(xt, sd[f"{p}.convs1.{i}.weight"], sd[f"{p}.convs1.{i}.bias"],
                    pad=(kernel - 1) * d1 // 2, dilation=d1)
        xt = adain(xt, s, sd, f"{p}.adain2.{i}")
        xt = snake(xt, sd[f"{p}.alpha2.{i}"][0][:, 0])
        xt = conv1d(xt, sd[f"{p}.convs2.{i}.weight"], sd[f"{p}.convs2.{i}.bias"], pad=kernel // 2)
        x = xt + x
    return x


def kokoro_layer_norm(x, sd, p, eps=1e-5):
    """kokoro.modules.LayerNorm: layer norm over channels."""
    return layer_norm(x, sd[p + ".gamma"], sd[p + ".beta"], eps)