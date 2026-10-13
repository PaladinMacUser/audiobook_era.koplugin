#!/usr/bin/env python3
"""Convert Kokoro-family checkpoints into the blobs the C runtime loads.

Emits:
  kok_meta.bin   fixed dims header + one by-NAME weight slot table + name table
  kok_text.bin   fp32 weights for the text half (slot blob_id 0)
  kok_dec.bin    fp32 weights for the decoder half (slot blob_id 1)

The slot table is by name rather than positional because the family spans
Paradee presets (xs..l x decoders A..W2/L), Kokoro-7M-Distill and Kokoro-82M,
with optional blocks; positional slots would not scale. The C parser resolves a
required name set plus optional names and checks every count against the
config-implied shape, failing loudly.

Legacy weight_norm is stored in these checkpoints as weight_g/weight_v and is
folded here as g*v/||v|| over all dimensions but the output channel, so the
runtime never sees either form.

Values that are NOT derivable from tensor shapes (attention head count, ALBERT
depth) are CLI arguments with the family defaults, and the parser validates the
shapes that follow from them.

Usage (through the port venv):
  kokoro/tools/py.sh kokoro/tools/export_kokoro_blobs.py \
      --text kokoro/model/text_side.pt --dec kokoro/model/decoder.pt \
      --out kokoro/model/blobs
"""
import argparse
import json
import struct
import sys
from pathlib import Path

import numpy as np
import torch

MAGIC = 0x4B4F4B4D          # 'KOKM'
VERSION = 1
DT_F32, DT_I8, DT_F16 = 0, 1, 2
QK_NONE, QK_PER_ROW_I8 = 0, 1

# Fixed dims order; extend at the end only (the C header mirrors this list).
DIMS = [
    "n_token", "sr",
    "albert_hidden", "albert_layers", "albert_heads", "albert_intermediate",
    "albert_embed", "albert_max_pos",
    "text_style_dim", "prosody_d_hid", "prosody_nlayers", "prosody_max_dur",
    "textenc_channels", "textenc_depth", "textenc_kernel",
    "asr_dim", "asr_hidden",
    "dec_width", "dec_uic",
    "dec_kernel0", "dec_kernel1", "dec_kernel2",
    "dec_uprate0", "dec_uprate1",
    "dec_upkern0", "dec_upkern1",
    "dec_istft_nfft", "dec_istft_hop", "dec_style_dim",
    "dec_dil0", "dec_dil1", "dec_dil2",
    "lock_l", "lock_nfft", "lock_hop",
    "lock_band_lo_hz", "lock_band_hi_hz", "lock_voiced_hz",
    "gen_harmonics",
]
DI = {name: i for i, name in enumerate(DIMS)}

# Constants of this model family (kokoro's istftnet config and the author's
# phase lock); not derivable from shapes.
LOCK = dict(l=33, nfft=1024, hop=256, band_lo=2000, band_hi=8000, voiced=60)
DILATIONS = (1, 3, 5)


def fold_weight_norm(sd):
    """Legacy weight_norm -> plain weight; name -> ndarray."""
    tensors = {k: v.detach().numpy().astype(np.float32) for k, v in sd.items()
               if hasattr(v, "detach")}
    out = {k: v for k, v in tensors.items()
           if not k.endswith(".weight_g") and not k.endswith(".weight_v")}
    for k, g in tensors.items():
        if not k.endswith(".weight_g"):
            continue
        base = k[:-len(".weight_g")]
        v = tensors[base + ".weight_v"]
        dims = tuple(range(1, v.ndim))
        out[base + ".weight"] = (g * v / np.sqrt((v ** 2).sum(axis=dims, keepdims=True))).astype(np.float32)
    return out


def shape_of(tensors, name):
    if name not in tensors:
        sys.exit(f"missing tensor {name}")
    return tuple(tensors[name].shape)


def require(tensors, name, shape):
    got = shape_of(tensors, name)
    if got != tuple(shape):
        sys.exit(f"{name}: shape {got} != expected {tuple(shape)}")
    return got


# Budgets kept in fp32 even when quantising: everything 1-D (norms, biases,
# Snake alpha, style vectors) plus the small heads the plan calls out. They are
# negligible in size and the most sensitive numerically.
FP32_KEEP = ("duration_proj", "F0_proj", "N_proj", "m_source", "style")


def quantize_int8(arr, per_row):
    """Symmetric int8, per output channel (dim 0) or per tensor, exactly the
    author's fq() in training/scripts/common.py: scale = max|row| / 127, round
    and clamp. Returns (int8 array, fp32 scales)."""
    if arr.ndim < 2:
        return None, None
    qmax = 127.0
    flat = arr.reshape(arr.shape[0], -1)
    if per_row:
        s = np.maximum(np.abs(flat).max(axis=1), 1e-8) / qmax
        q = np.clip(np.round(flat / s[:, None]), -qmax, qmax).astype(np.int8)
        return q.reshape(arr.shape), s.astype(np.float32)
    s = np.float32(max(float(np.abs(flat).max()), 1e-8) / qmax)
    q = np.clip(np.round(flat / s), -qmax, qmax).astype(np.int8)
    return q.reshape(arr.shape), np.array([s], dtype=np.float32)


def build_slots(tensors, blob_id, int8=False):
    names = sorted(tensors)
    blob = bytearray()
    slots = []
    extra_names = []
    for i, name in enumerate(names):
        arr = np.ascontiguousarray(tensors[name], dtype=np.float32)
        keep = (not int8 or name.startswith(FP32_KEEP) or arr.ndim < 2)
        # ConvTranspose1d weights are [in, out, k], so "dim 0" is not the output
        # channel; those get one scale for the whole tensor instead.
        convtr = (name.endswith(".weight") and arr.ndim == 3 and "ups." in name)
        q = s = None
        if not keep:
            q, s = quantize_int8(arr, per_row=not convtr)
        if q is None:
            payload, dtype, qkind, count = arr.tobytes(), DT_F32, QK_NONE, int(arr.size)
        else:
            payload, dtype, qkind, count = q.tobytes(), DT_I8, QK_PER_ROW_I8, int(q.size)
        pad = (-len(blob)) % 16
        blob.extend(b"\0" * pad)
        slots.append({"name_index": i, "blob_id": blob_id, "dtype": dtype,
                      "qkind": qkind, "count": count, "offset": len(blob),
                      "rows": int(arr.shape[0]) if arr.ndim >= 2 else -1,
                      "row_len": int(np.prod(arr.shape[1:])) if arr.ndim >= 2 else int(arr.size),
                      "scale": 0.0})
        blob.extend(payload)
        if s is not None:
            pad = (-len(blob)) % 16
            blob.extend(b"\0" * pad)
            extra_names.append((f"{name}.scale", len(s), len(blob)))
            blob.extend(s.astype(np.float32).tobytes())
    names = names + [e[0] for e in extra_names]
    for (nm, n, off) in extra_names:
        idx = names.index(nm)
        slots.append({"name_index": idx, "blob_id": blob_id, "dtype": DT_F32,
                      "qkind": QK_NONE, "count": n, "offset": off, "rows": -1,
                      "row_len": n, "scale": 0.0})
    return names, bytes(blob), slots


def write_meta(path, dims, names, slots):
    name_blob = bytearray()
    entries = []
    for n in names:
        b = n.encode()
        entries.append((len(name_blob), len(b)))
        name_blob.extend(b)
    with open(path, "wb") as fh:
        fh.write(struct.pack("<III", MAGIC, VERSION, len(dims)))
        fh.write(struct.pack(f"<{len(dims)}i", *dims))
        fh.write(struct.pack("<I", len(entries)))
        for off, ln in entries:
            fh.write(struct.pack("<II", off, ln))
        fh.write(struct.pack("<I", len(name_blob)))
        fh.write(bytes(name_blob))
        fh.write(struct.pack("<I", len(slots)))
        for s in slots:
            fh.write(struct.pack("<IIiiiIii", s["name_index"], s["blob_id"], s["dtype"],
                                 s["qkind"], s["count"], s["offset"], s["rows"],
                                 s["row_len"]))
            fh.write(struct.pack("<f", s["scale"]))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--text", required=True)
    ap.add_argument("--dec", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--int8", action="store_true",
                    help="per-output-channel symmetric int8 weights with an fp32 "
                         "keep-list (the shipped ONNX's own scheme)")
    ap.add_argument("--albert-layers", type=int, default=6,
                    help="num_hidden_layers: a config value, not in the checkpoint")
    ap.add_argument("--albert-heads", type=int, default=4,
                    help="num_attention_heads: a config value, not in the checkpoint")
    ap.add_argument("--dec-kernels", type=int, nargs=3, default=[3, 7, 11])
    ap.add_argument("--dec-uprates", type=int, nargs=2, default=[10, 6])
    ap.add_argument("--istft", type=int, nargs=2, default=[20, 5], metavar=("NFFT", "HOP"))
    args = ap.parse_args()
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    ts = fold_weight_norm(torch.load(args.text, map_location="cpu", weights_only=False)["model"])
    ds = fold_weight_norm(torch.load(args.dec, map_location="cpu", weights_only=False)["model"])

    n_token, a_embed = shape_of(ts, "bert.embeddings.word_embeddings.weight")
    a_hidden = shape_of(ts, "bert.encoder.embedding_hidden_mapping_in.weight")[0]
    a_inter = shape_of(ts, "bert.encoder.albert_layer_groups.0.albert_layers.0.ffn.weight")[0]
    a_maxpos = shape_of(ts, "bert.embeddings.position_embeddings.weight")[0]
    style_dim = shape_of(ts, "style")[1]
    d_hid = shape_of(ts, "predictor.duration_proj.linear_layer.weight")[1]
    max_dur = shape_of(ts, "predictor.duration_proj.linear_layer.weight")[0]
    textenc_ch = shape_of(ts, "text_encoder.embedding.weight")[1]
    textenc_kernel = shape_of(ts, "text_encoder.cnn.0.0.weight")[2]
    textenc_depth = len({k.split(".")[2] for k in ts if k.startswith("text_encoder.cnn.")})
    asr_dim = shape_of(ts, "asr_proj.0.weight")[0]
    asr_hid = shape_of(ts, "asr_proj.2.weight")[0]
    prosody_layers = len({k.split(".")[3] for k in ts if k.startswith("predictor.text_encoder.lstms.")}) // 2
    dec_w = shape_of(ds, "encode.conv1.weight")[0]        # AdaIN width (norm1 acts on dim_in)
    dec_uic = shape_of(ds, "decode.3.conv2.weight")[0]
    dec_style = shape_of(ds, "style")[1]

    # Shape checks that follow from the config values: a checkpoint exported
    # with different widths fails here, not at load time on the device.
    require(ts, "bert.encoder.albert_layer_groups.0.albert_layers.0.attention.query.weight",
            (a_hidden, a_hidden))
    require(ts, "bert.encoder.albert_layer_groups.0.albert_layers.0.ffn.weight",
            (a_inter, a_hidden))
    require(ts, "bert_encoder.weight", (d_hid, a_hidden))
    require(ts, "predictor.lstm.weight_ih_l0", (2 * d_hid, d_hid + style_dim))
    require(ts, "predictor.shared.weight_ih_l0", (2 * d_hid, d_hid + style_dim))
    require(ts, "predictor.F0.1.pool.weight", (d_hid, 1, 3))
    require(ts, "predictor.F0_proj.weight", (1, d_hid // 2, 1))
    require(ts, "predictor.text_encoder.lstms.0.weight_ih_l0", (2 * d_hid, d_hid + style_dim))
    require(ts, "text_encoder.lstm.weight_ih_l0", (2 * textenc_ch, textenc_ch))
    require(ts, "text_encoder.cnn.0.0.weight", (textenc_ch, textenc_ch, textenc_kernel))
    require(ts, "asr_proj.0.weight", (asr_dim, textenc_ch))
    require(ts, "asr_proj.2.weight", (asr_hid, asr_dim))
    require(ds, "encode.conv1.weight", (dec_w, asr_dim + 2, 3))
    require(ds, "encode.norm1.fc.weight", (2 * (asr_dim + 2), dec_style))   # Linear(style, 2C)
    require(ds, "decode.3.conv1.weight", (dec_uic, dec_w + 2 + 64, 3))
    require(ds, "decode.0.conv1.weight", (dec_w, dec_w + 2 + 64, 3))
    require(ds, "decode.0.conv1x1.weight", (dec_w, dec_w + 2 + 64, 1))
    require(ds, "F0_conv.weight", (1, 1, 3))
    require(ds, "asr_res.0.weight", (64, asr_dim, 1))
    require(ds, "generator.ups.0.weight", (dec_uic, dec_uic // 2, shape_of(ds, "generator.ups.0.weight")[2]))
    require(ds, "generator.ups.1.weight", (dec_uic // 2, dec_uic // 4, shape_of(ds, "generator.ups.1.weight")[2]))
    require(ds, "generator.conv_post.weight", (args.istft[0] + 2, dec_uic // 4, 7))
    require(ds, "generator.noise_convs.0.weight", (dec_uic // 2, args.istft[0] + 2, 12))
    require(ds, "generator.noise_convs.1.weight", (dec_uic // 4, args.istft[0] + 2, 1))
    require(ds, "generator.m_source.l_linear.weight", (1, 9))

    names_t, blob_t, slots_t = build_slots(ts, 0, int8=args.int8)
    names_d, blob_d, slots_d = build_slots(ds, 1, int8=args.int8)
    # one name table shared by both halves: re-index the decoder slots
    names = names_t + [n for n in names_d]
    for s in slots_d:
        s["name_index"] += len(names_t)

    dims = [0] * len(DIMS)

    def setdim(name, value):
        dims[DI[name]] = int(value)

    setdim("n_token", n_token)
    setdim("sr", 24000)
    setdim("albert_hidden", a_hidden)
    setdim("albert_layers", args.albert_layers)
    setdim("albert_heads", args.albert_heads)
    setdim("albert_intermediate", a_inter)
    setdim("albert_embed", a_embed)
    setdim("albert_max_pos", a_maxpos)
    setdim("text_style_dim", style_dim)
    setdim("prosody_d_hid", d_hid)
    setdim("prosody_nlayers", prosody_layers)
    setdim("prosody_max_dur", max_dur)
    setdim("textenc_channels", textenc_ch)
    setdim("textenc_depth", textenc_depth)
    setdim("textenc_kernel", textenc_kernel)
    setdim("asr_dim", asr_dim)
    setdim("asr_hidden", asr_hid)
    setdim("dec_width", dec_w)
    setdim("dec_uic", dec_uic)
    for i, k in enumerate(args.dec_kernels):
        setdim(f"dec_kernel{i}", k)
    setdim("dec_uprate0", args.dec_uprates[0])
    setdim("dec_uprate1", args.dec_uprates[1])
    setdim("dec_upkern0", shape_of(ds, "generator.ups.0.weight")[2])
    setdim("dec_upkern1", shape_of(ds, "generator.ups.1.weight")[2])
    setdim("dec_istft_nfft", args.istft[0])
    setdim("dec_istft_hop", args.istft[1])
    setdim("dec_style_dim", dec_style)
    for i, d in enumerate(DILATIONS):
        setdim(f"dec_dil{i}", d)
    setdim("lock_l", LOCK["l"])
    setdim("lock_nfft", LOCK["nfft"])
    setdim("lock_hop", LOCK["hop"])
    setdim("lock_band_lo_hz", LOCK["band_lo"])
    setdim("lock_band_hi_hz", LOCK["band_hi"])
    setdim("lock_voiced_hz", LOCK["voiced"])
    setdim("gen_harmonics", shape_of(ds, "generator.m_source.l_linear.weight")[1])

    write_meta(out / "kok_meta.bin", dims, names, slots_t + slots_d)
    (out / "kok_text.bin").write_bytes(blob_t)
    (out / "kok_dec.bin").write_bytes(blob_d)

    summary = {"dims": {n: dims[i] for i, n in enumerate(DIMS)},
               "text": {"slots": len(slots_t), "bytes": len(blob_t)},
               "decoder": {"slots": len(slots_d), "bytes": len(blob_d)},
               "assumptions": {"albert_layers": args.albert_layers,
                               "albert_heads": args.albert_heads}}
    (out / "kok_blobs.json").write_text(json.dumps(summary, indent=1))
    print(f"layers: albert {args.albert_layers}@{args.albert_heads}h, prosody {prosody_layers}, "
          f"textenc depth {textenc_depth} k{textenc_kernel} ch{textenc_ch}; decoder width {dec_w}, "
          f"uic {dec_uic}, kernels {args.dec_kernels}, uprates {args.dec_uprates}, istft {args.istft}")
    print(f"wrote kok_meta.bin ({len(slots_t) + len(slots_d)} slots, {len(names)} names), "
          f"kok_text.bin {len(blob_t)} bytes, kok_dec.bin {len(blob_d)} bytes -> {out}")


if __name__ == "__main__":
    main()