#!/usr/bin/env python3
"""Torch-free numpy assembly of the Paradee/Kokoro-family pipeline (M1).

Uses kokoro/tools/numpy_ref.py primitives. Verified stage-by-stage against the
torch goldens before any C is written; see check_numpy_ref.py.

Weights arrive as a flat dict of numpy arrays keyed by the checkpoint names
(torch weight_norm is already folded out by the caller: the Paradee checkpoints
and the goldens were produced after remove_weight_norm, so names carry no
'parametrizations' segments).

The sine source is stochastic (torch.rand/randn draws). Generator.forward takes
an optional src_override, which the checker feeds from the golden tensor so the
deterministic kernels can be validated without RNG parity; with no override the
module computes the source from f0 plus a supplied RNG callback.
"""
import math

import numpy as np

from numpy_ref import (ConvSTFT, Lock, SR, ada_layer_norm, adain_res_blk1d,
                       adain_res_block1, bilstm, conv1d, convtr1d, gelu,
                       gelu_new, interp_linear_time, kokoro_layer_norm,
                       layer_norm, leaky_relu, linear, sigmoid, softmax,
                       upsample_nearest)


def kq(p, name):
    """Checkpoint key for module path p: the decoder checkpoint carries no
    module prefix, the text checkpoint's submodules do."""
    return name if not p else f"{p}.{name}"


class Albert:
    """HF AlbertModel, embeddings plus one shared layer applied n_layers times
    (the shipped student reuses a single albert_layers.0 block)."""

    def __init__(self, sd, n_layers):
        self.sd, self.n_layers = sd, n_layers

    def __call__(self, ids):
        sd = self.sd
        T = len(ids)
        x = (sd["bert.embeddings.word_embeddings.weight"][ids]
             + sd["bert.embeddings.position_embeddings.weight"][:T]
             + sd["bert.embeddings.token_type_embeddings.weight"][0]).T
        x = layer_norm(x, sd["bert.embeddings.LayerNorm.weight"],
                       sd["bert.embeddings.LayerNorm.bias"], 1e-12)
        x = linear(x, sd["bert.encoder.embedding_hidden_mapping_in.weight"],
                   sd["bert.encoder.embedding_hidden_mapping_in.bias"])
        for _ in range(self.n_layers):
            x = self._layer(x)
        return x

    def attention_block(self, x):
        """AlbertAttention on [C, T] -> [C, T]. Head layout matches HF:
        [heads, T, head_size], scores contract the head_size axis."""
        sd = self.sd
        p = "bert.encoder.albert_layer_groups.0.albert_layers.0"
        C, T = x.shape
        heads = 4
        hs = C // heads
        q = linear(x, sd[p + ".attention.query.weight"], sd[p + ".attention.query.bias"]).reshape(heads, hs, T)
        k = linear(x, sd[p + ".attention.key.weight"], sd[p + ".attention.key.bias"]).reshape(heads, hs, T)
        v = linear(x, sd[p + ".attention.value.weight"], sd[p + ".attention.value.bias"]).reshape(heads, hs, T)
        # to [heads, T, hs] (torch's layout) before scoring
        q, k, v = (a.transpose(0, 2, 1) for a in (q, k, v))
        scores = np.einsum("hid,hjd->hij", q, k, optimize=True) / np.float32(math.sqrt(hs))
        probs = softmax(scores, axis=-1)
        ctx = np.einsum("hij,hjd->hid", probs, v, optimize=True)   # [heads, T, hs]
        ctx = ctx.transpose(1, 0, 2).reshape(T, C).T               # [C, T], heads-major
        att = linear(ctx, sd[p + ".attention.dense.weight"], sd[p + ".attention.dense.bias"])
        return layer_norm(att + x, sd[p + ".attention.LayerNorm.weight"],
                          sd[p + ".attention.LayerNorm.bias"], 1e-12)

    def ffn_block(self, x):
        """AlbertLayer FFN half: ffn -> gelu_new -> ffn_output (no residual)."""
        sd = self.sd
        p = "bert.encoder.albert_layer_groups.0.albert_layers.0"
        h = gelu_new(linear(x, sd[p + ".ffn.weight"], sd[p + ".ffn.bias"]))
        return linear(h, sd[p + ".ffn_output.weight"], sd[p + ".ffn_output.bias"])

    def _layer(self, x):
        a = self.attention_block(x)
        f = self.ffn_block(a)
        sd = self.sd
        p = "bert.encoder.albert_layer_groups.0.albert_layers.0"
        return layer_norm(f + a, sd[p + ".full_layer_layer_norm.weight"],
                          sd[p + ".full_layer_layer_norm.bias"], 1e-12)


class TextEncoder:
    """kokoro TextEncoder: embedding, depth conv blocks with channel LayerNorm
    and leaky 0.2, then a bidirectional LSTM."""

    def __init__(self, sd, p, depth, kernel):
        self.sd, self.p, self.depth, self.kernel = sd, p, depth, kernel

    def __call__(self, ids):
        sd, p = self.sd, self.p
        x = sd[p + ".embedding.weight"][ids].T
        pad = (self.kernel - 1) // 2
        for i in range(self.depth):
            q = f"{p}.cnn.{i}"
            x = conv1d(x, sd[q + ".0.weight"], sd[q + ".0.bias"], pad=pad)
            x = kokoro_layer_norm(x, sd, q + ".1")
            x = leaky_relu(x, 0.2)
        return bilstm(x, sd, p + ".lstm")


class DurationEncoder:
    """kokoro DurationEncoder: alternating biLSTM and AdaLayerNorm, with the
    style vector concatenated before each block."""

    def __init__(self, sd, p, nlayers, style_dim):
        self.sd, self.p, self.nlayers, self.style_dim = sd, p, nlayers, style_dim

    def __call__(self, x, s):
        """x [C, T], s [style] -> [C+style, T]."""
        sd = self.sd
        x = np.concatenate([x, np.repeat(s[:, None], x.shape[1], axis=1)], axis=0)
        for i in range(self.nlayers):
            x = bilstm(x, sd, f"{self.p}.lstms.{2 * i}")
            x = ada_layer_norm(x, s, sd, f"{self.p}.lstms.{2 * i + 1}")
            x = np.concatenate([x, np.repeat(s[:, None], x.shape[1], axis=1)], axis=0)
        return x


class ProsodyPredictor:
    """kokoro ProsodyPredictor: durations from a biLSTM plus LinearNorm and a
    sigmoid sum, then F0/N heads on the aligned features."""

    def __init__(self, sd, p, d_hid, style_dim, max_dur, upsample=True):
        self.sd, self.p, self.d_hid, self.style_dim, self.max_dur = sd, p, d_hid, style_dim, max_dur
        self.text_encoder = DurationEncoder(sd, p + ".text_encoder", 3, style_dim)

    def durations(self, d, s):
        """d [C+style, T] -> durations [T] (rounded, min 1)."""
        sd, p = self.sd, self.p
        x = bilstm(d, sd, p + ".lstm")
        x = linear(x, sd[p + ".duration_proj.linear_layer.weight"],
                   sd[p + ".duration_proj.linear_layer.bias"])
        dur_raw = sigmoid(x).sum(axis=0)
        return dur_raw, np.maximum(np.round(dur_raw).astype(np.int64), 1)

    def f0_n(self, en, s):
        """en [C+style, F], s [style] -> F0 [2F], N [2F]."""
        sd, p = self.sd, self.p
        x = bilstm(en, sd, p + ".shared")
        out = []
        for head in ("F0", "N"):
            h = x
            for i in range(3):
                h = adain_res_blk1d(h, s, sd, f"{p}.{head}.{i}", upsample=(i == 1))
            out.append(linear(h, sd[f"{p}.{head}_proj.weight"].reshape(1, -1),
                              sd[f"{p}.{head}_proj.bias"])[0].astype(np.float32))
        return out[0], out[1]


class StudentDecoder:
    """Paradee StudentDecoder (preset A): AdaIN encode/decode stack, F0/N stride
    convs, the asr skip, then the istftnet generator."""

    def __init__(self, sd, p, preset):
        self.sd, self.p = sd, p
        self.width, self.uic, self.kernels = preset
        self.style_dim = sd[kq(p, "style")].shape[1]

    def __call__(self, asr, f0_curve, n_curve, style, src_override=None, tap=None):
        sd, p = self.sd, self.p
        s = style
        F0 = conv1d(f0_curve[None, :], sd[kq(p, "F0_conv.weight")], sd[kq(p, "F0_conv.bias")],
                    stride=2, pad=1)
        N = conv1d(n_curve[None, :], sd[kq(p, "N_conv.weight")], sd[kq(p, "N_conv.bias")],
                   stride=2, pad=1)
        x = np.concatenate([asr, F0, N], axis=0)
        x = adain_res_blk1d(x, s, sd, kq(p, "encode"))
        asr_res = conv1d(asr, sd[kq(p, "asr_res.0.weight")], sd[kq(p, "asr_res.0.bias")])
        keep_res = True
        for i in range(4):
            if keep_res:
                x = np.concatenate([x, asr_res, F0, N], axis=0)
            upsample = (i == 3)
            x = adain_res_blk1d(x, s, sd, kq(p, f"decode.{i}"), upsample=upsample)
            if upsample:
                keep_res = False
        gen = Generator(sd, kq(p, "generator"), self.kernels, self.uic)
        return gen(x, s, f0_curve, src_override, tap)


class Generator:
    """kokoro istftnet Generator (upsample rates 10 and 6, istft n_fft 20, hop 5)
    with the author's ConvSTFT inverse."""

    def __init__(self, sd, p, kernels, uic):
        self.sd, self.p, self.kernels, self.uic = sd, p, kernels, uic
        self.stft = ConvSTFT(20, 5)
        self.total = 10 * 6 * 5  # samples per input frame

    def __call__(self, x, s, f0_curve, src_override=None, tap=None):
        sd, p = self.sd, self.p
        if src_override is None:
            raise NotImplementedError("source module RNG path not implemented yet")
        har_source = np.asarray(src_override).reshape(1, -1)
        mag, phase = self.stft.transform(har_source)
        har = np.concatenate([mag, phase], axis=0)
        if tap is not None:
            tap["gen_har"] = har
        for i in range(2):
            x = leaky_relu(x, 0.1)
            if i == 0:
                stride = 6
                xs_ = conv1d(har, sd[kq(p, f"noise_convs.{i}.weight")], sd[kq(p, f"noise_convs.{i}.bias")],
                             stride=stride, pad=(stride + 1) // 2)
            else:
                xs_ = conv1d(har, sd[kq(p, f"noise_convs.{i}.weight")], sd[kq(p, f"noise_convs.{i}.bias")])
            xs_ = adain_res_block1(xs_, s, sd, kq(p, f"noise_res.{i}"), 7 if i == 0 else 11)
            if i == 0:
                x = convtr1d(x, sd[kq(p, f"ups.{i}.weight")], sd[kq(p, f"ups.{i}.bias")], stride=10, pad=5)
            else:
                x = convtr1d(x, sd[kq(p, f"ups.{i}.weight")], sd[kq(p, f"ups.{i}.bias")], stride=6, pad=3)
                x = np.pad(x, ((0, 0), (1, 0)), mode="reflect")  # ReflectionPad1d((1,0))
            if tap is not None:
                tap[f"gen_ups{i}"] = x
            x = x + xs_
            if tap is not None:
                tap[f"gen_stage{i}_pre_res"] = x
            acc = None
            for j in range(3):
                v = adain_res_block1(x, s, sd, kq(p, f"resblocks.{i * 3 + j}"), self.kernels[j])
                acc = v if acc is None else acc + v
            x = acc / np.float32(3.0)
            if tap is not None:
                tap[f"gen_stage{i}"] = x
        x = leaky_relu(x, 0.01)
        x = conv1d(x, sd[kq(p, "conv_post.weight")], sd[kq(p, "conv_post.bias")], pad=3)
        if tap is not None:
            tap["gen_conv_post"] = x
        nb = 20 // 2 + 1
        spec = np.exp(x[:nb])
        ph = np.sin(x[nb:])
        if tap is not None:
            tap["gen_spec"] = spec
            tap["gen_phase"] = ph
        out = self.stft.inverse(spec, ph)
        if tap is not None:
            tap["gen_out"] = out[0]
        return out


class Paradee:
    """The shipped assembly: text side, alignment expansion, F0/N, decoder, lock."""

    def __init__(self, sd_text, sd_dec, n_albert_layers=6, lock_l=33):
        self.ts = sd_text
        self.ds = sd_dec
        self.albert = Albert(sd_text, n_albert_layers)
        self.prosody = ProsodyPredictor(sd_text, "predictor", 192, 32, 50)
        self.text_encoder = TextEncoder(sd_text, "text_encoder", 3, 5)
        self.decoder = StudentDecoder(sd_dec, "", (256, 128, [3, 7, 11]))
        self.lock = Lock(lock_l)
        self.style_text = sd_text["style"][0]
        self.style_dec = sd_dec["style"][0]

    def text(self, ids):
        """ids -> (d [224, T], durations [T], asr_tok [512, T])."""
        sd = self.ts
        b = self.albert(np.asarray(ids, dtype=np.int64))
        d_en = linear(b, sd["bert_encoder.weight"], sd["bert_encoder.bias"])
        d = self.prosody.text_encoder(d_en, self.style_text)          # [224, T]
        dur_raw, durs = self.prosody.durations(d, self.style_text)
        t_en = self.text_encoder(np.asarray(ids, dtype=np.int64))     # [192, T]
        asr = linear(t_en, sd["asr_proj.0.weight"], sd["asr_proj.0.bias"])
        asr = gelu(asr)
        asr_tok = linear(asr, sd["asr_proj.2.weight"], sd["asr_proj.2.bias"])
        return d, dur_raw, durs, asr_tok

    def __call__(self, ids, speed=1.0, src_override=None, tap=None, gen_tap=None):
        d, dur_raw, durs, asr_tok = self.text(ids)
        n = np.maximum(np.round(dur_raw / speed).astype(np.int64), 1)
        F = int(n.sum())
        aln = np.zeros((len(ids), F), dtype=np.float32)
        pos = 0
        for t, k in enumerate(n):
            aln[t, pos:pos + k] = 1.0
            pos += k
        en = d @ aln                         # [224, F] (d is already channel-major)
        f0, nc = self.prosody.f0_n(en, self.style_text)
        asr = asr_tok @ aln                  # [512, F]
        F0c, Nc = f0[:2 * F], nc[:2 * F]
        if tap is not None:
            tap.update(d=d, dur_raw=dur_raw, durs=n, aln=aln, en=en, F0=F0c, N=Nc,
                       asr_tok=asr_tok, asr=asr)
        src_flat = None if src_override is None else np.asarray(src_override).reshape(-1)
        wave = self.decoder(asr, F0c, Nc, self.style_dec, src_flat, gen_tap)
        s = wave[0]
        if tap is not None:
            tap["wave_pre_lock"] = s
        if src_flat is None:
            return s[None, :]
        return self.lock(s[None, :], src_flat, F0c)