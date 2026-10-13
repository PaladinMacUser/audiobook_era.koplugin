#!/usr/bin/env python3
"""Convert a sanoTTS fp16 self-contained voice package into the f32 blob pair
the plugin's C runtime loads (snt_front_f32.c + snt_piperlite.c).

Input: a package dir from huggingface.co/ampixa/sanoTTS (manifest.json +
weights.fp16.bin, the "standalone runtime" layout: one fp16 blob, tensors
dispatched by offset_bytes).

Output (into --out):
  front_meta.bin         dims + per-slot (offset,size) table, snt_front_f32 layout
  front_weights_f32.bin  duration + acoustic student weights, fp32, exporter order
  dec_meta.bin           snt_piperlite meta.bin layout
  dec_weights_f32.bin    piperlite decoder weights, fp32, exporter order

Slot orders follow upstream tools/export_front_golden.py and
export_piperlite_golden.py. Every slot's shape is validated against the
expected dims before it is written, so a manifest that does not match the
exporter layout fails loudly instead of producing a silent-garbage blob.
"""

import argparse
import json
import struct
import sys


def fail(msg):
    print(f"error: {msg}", file=sys.stderr)
    sys.exit(1)


def expected_front_slots(d_vocab, d_hidden, d_depth, d_kernel,
                         a_vocab, a_hidden, a_token_depth, a_depth,
                         a_kernel, a_out):
    """Return the exporter slot order for the front half as
    (component, name, expected_shape) tuples."""
    slots = []
    # duration student (DurationStudent)
    slots.append(("duration", "embedding.weight", [d_vocab, d_hidden]))
    slots.append(("duration", "input_proj.weight", [d_hidden, d_hidden + 3, 1]))
    slots.append(("duration", "input_proj.bias", [d_hidden]))
    for b in range(d_depth):
        slots.append(("duration", f"blocks.{b}.scale", [1]))
        slots.append(("duration", f"blocks.{b}.net.0.weight", [d_hidden, d_hidden, d_kernel]))
        slots.append(("duration", f"blocks.{b}.net.0.bias", [d_hidden]))
        slots.append(("duration", f"blocks.{b}.net.2.weight", [d_hidden, d_hidden, d_kernel]))
        slots.append(("duration", f"blocks.{b}.net.2.bias", [d_hidden]))
    slots.append(("duration", "output.weight", [1, d_hidden, 1]))
    slots.append(("duration", "output.bias", [1]))
    # acoustic student (ContextualLatentStudent)
    slots.append(("acoustic", "embedding.weight", [a_vocab, a_hidden]))
    slots.append(("acoustic", "token_input_proj.weight", [a_hidden, a_hidden + 2, 1]))
    slots.append(("acoustic", "token_input_proj.bias", [a_hidden]))
    for b in range(a_token_depth):
        slots.append(("acoustic", f"token_blocks.{b}.scale", [1]))
        slots.append(("acoustic", f"token_blocks.{b}.net.0.weight", [a_hidden, a_hidden, a_kernel]))
        slots.append(("acoustic", f"token_blocks.{b}.net.0.bias", [a_hidden]))
        slots.append(("acoustic", f"token_blocks.{b}.net.2.weight", [a_hidden, a_hidden, a_kernel]))
        slots.append(("acoustic", f"token_blocks.{b}.net.2.bias", [a_hidden]))
    slots.append(("acoustic", "frame_input_proj.weight", [a_hidden, a_hidden + 3, 1]))
    slots.append(("acoustic", "frame_input_proj.bias", [a_hidden]))
    for b in range(a_depth):
        slots.append(("acoustic", f"frame_blocks.{b}.scale", [1]))
        slots.append(("acoustic", f"frame_blocks.{b}.net.0.weight", [a_hidden, a_hidden, a_kernel]))
        slots.append(("acoustic", f"frame_blocks.{b}.net.0.bias", [a_hidden]))
        slots.append(("acoustic", f"frame_blocks.{b}.net.2.weight", [a_hidden, a_hidden, a_kernel]))
        slots.append(("acoustic", f"frame_blocks.{b}.net.2.bias", [a_hidden]))
    slots.append(("acoustic", "output.weight", [a_out, a_hidden, 1]))
    slots.append(("acoustic", "output.bias", [a_out]))
    return slots


def expected_decoder_slots(in_ch, c0, c1, c2, c3):
    """Exporter slot order for the piperlite decoder (46 slots, no
    post-filter)."""
    bk = [3, 5, 7]           # residual-bank branch kernels
    up_k = [16, 16, 8]       # upsample kernels
    slots = []
    slots.append(("decoder", "pre.weight", [c0, in_ch, 7]))
    slots.append(("decoder", "pre.bias", [c0]))
    for s, (ic, oc) in enumerate([(c0, c1), (c1, c2), (c2, c3)]):
        # ConvTranspose1d weight layout is (in_channels, out_channels, kernel)
        slots.append(("decoder", f"up{s}.weight", [ic, oc, up_k[s]]))
        slots.append(("decoder", f"up{s}.bias", [oc]))
        for br in range(3):
            for part, shape in enumerate([
                ([oc, oc, bk[br]], f"res{s}.0.blocks.{br}.conv1.weight"),
                ([oc], f"res{s}.0.blocks.{br}.conv1.bias"),
                ([oc, oc, bk[br]], f"res{s}.0.blocks.{br}.conv2.weight"),
                ([oc], f"res{s}.0.blocks.{br}.conv2.bias"),
            ]):
                slots.append(("decoder", shape[1], list(shape[0])))
    slots.append(("decoder", "post.weight", [1, c3, 7]))
    slots.append(("decoder", "post.bias", [1]))
    return slots


def emit_meta(path, magic, version, header_i32s, header_f32s, n_slots, slots_meta):
    """meta.bin: i32 magic, i32 version, header fields, i32 n_tensors,
    then per-slot i32 offset (float units) + i32 size (floats)."""
    with open(path, "wb") as fh:
        fh.write(struct.pack("<i", magic))
        fh.write(struct.pack("<i", version))
        for v in header_i32s:
            fh.write(struct.pack("<i", v))
        for v in header_f32s:
            fh.write(struct.pack("<f", v))
        fh.write(struct.pack("<i", n_slots))
        for off, size in slots_meta:
            fh.write(struct.pack("<ii", off, size))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("package_dir", help="fp16 voice package (manifest.json + weights.fp16.bin)")
    ap.add_argument("--out", required=True, help="output dir for the four f32 blobs")
    args = ap.parse_args()

    import os
    os.makedirs(args.out, exist_ok=True)

    with open(os.path.join(args.package_dir, "manifest.json")) as fh:
        man = json.load(fh)

    weights_path = os.path.join(args.package_dir, "weights.fp16.bin")
    with open(weights_path, "rb") as fh:
        blob = fh.read()
    if man.get("weights_size_bytes") not in (None, len(blob)):
        fail(f"weights_size_bytes {man.get('weights_size_bytes')} != file size {len(blob)}")

    # name -> (shape, offset_bytes) per component
    tables = {}
    for comp in ("duration", "acoustic", "decoder"):
        tables[comp] = {t["name"]: (t["shape"], t["offset_bytes"], t["nbytes"])
                        for t in man["components"][comp]["tensors"]}

    def take(comp, name, want_shape, out):
        shape, off, nbytes = tables[comp][name]
        if shape != want_shape:
            fail(f"{comp}/{name}: shape {shape} != expected {want_shape}")
        want_bytes = 2
        for d in want_shape:
            want_bytes *= d
        if nbytes != want_bytes:
            fail(f"{comp}/{name}: nbytes {nbytes} != {want_bytes}")
        raw = blob[off:off + nbytes]
        vals = struct.unpack(f"<{want_bytes // 2}e", raw)
        out.extend(struct.pack(f"<{len(vals)}f", *vals))
        return len(vals)

    # ---- derive front dims from the manifest shapes ----
    d_vocab, d_hidden = tables["duration"]["embedding.weight"][0]
    d_depth = len({n.split(".")[1] for n in tables["duration"] if n.startswith("blocks.")})
    d_kernel = tables["duration"]["blocks.0.net.0.weight"][0][2]
    d_max_tokens = man["components"]["duration"].get("max_tokens")
    d_max_duration = man["components"]["duration"].get("max_duration")
    a_vocab, a_hidden = tables["acoustic"]["embedding.weight"][0]
    a_token_depth = len({n.split(".")[1] for n in tables["acoustic"] if n.startswith("token_blocks.")})
    a_depth = len({n.split(".")[1] for n in tables["acoustic"] if n.startswith("frame_blocks.")})
    a_kernel = tables["acoustic"]["frame_blocks.0.net.0.weight"][0][2]
    a_out = tables["acoustic"]["output.weight"][0][0]
    if d_max_tokens is None or d_max_duration is None:
        # fall back to the runtime-kernels documented values
        d_max_tokens, d_max_duration = 525, 80

    if len(tables["duration"]) != 5 + 5 * d_depth:
        fail(f"duration: {len(tables['duration'])} tensors, expected {5 + 5 * d_depth}")
    if len(tables["acoustic"]) != 7 + 5 * a_token_depth + 5 * a_depth:
        fail(f"acoustic: {len(tables['acoustic'])} tensors, unexpected adapter or shape")

    # ---- front blob ----
    front_w = bytearray()
    slots_meta = []
    for comp, name, shape in expected_front_slots(
            d_vocab, d_hidden, d_depth, d_kernel,
            a_vocab, a_hidden, a_token_depth, a_depth, a_kernel, a_out):
        off = len(front_w) // 4
        n = take(comp, name, shape, front_w)
        slots_meta.append((off, n))
    if len(slots_meta) != (5 + 5 * d_depth) + (7 + 5 * a_token_depth + 5 * a_depth):
        fail("front slot count mismatch")
    emit_meta(os.path.join(args.out, "front_meta.bin"), 0x534E4652, 1,
              [d_vocab, d_hidden, d_depth, d_kernel, d_max_tokens, d_max_duration,
               a_vocab, a_hidden, a_token_depth, a_depth, a_kernel, a_out,
               0, 0, 0],  # adapter_mode NONE, kernel, rank
              [], len(slots_meta), slots_meta)
    with open(os.path.join(args.out, "front_weights_f32.bin"), "wb") as fh:
        fh.write(front_w)

    # ---- decoder blob ----
    in_ch = tables["decoder"]["pre.weight"][0][1]
    c0 = tables["decoder"]["pre.weight"][0][0]
    c1 = tables["decoder"]["up0.weight"][0][1]
    c2 = tables["decoder"]["up1.weight"][0][1]
    c3 = tables["decoder"]["up2.weight"][0][1]
    if len(tables["decoder"]) != 46:
        fail(f"decoder: {len(tables['decoder'])} tensors; post-filter voices are not supported")
    dec_w = bytearray()
    slots_meta = []
    for comp, name, shape in expected_decoder_slots(in_ch, c0, c1, c2, c3):
        off = len(dec_w) // 4
        n = take(comp, name, shape, dec_w)
        slots_meta.append((off, n))
    emit_meta(os.path.join(args.out, "dec_meta.bin"), 0x534E504C, 1,
              [in_ch, c0, c1, c2, c3, 0, 0, 0],  # pf_channels/layers/kernel = 0
              [0.0],  # pf_scale
              len(slots_meta), slots_meta)
    with open(os.path.join(args.out, "dec_weights_f32.bin"), "wb") as fh:
        fh.write(dec_w)

    print(f"front: {len(front_w)} bytes f32, d_vocab={d_vocab} d_hidden={d_hidden} "
          f"d_depth={d_depth} d_kernel={d_kernel} "
          f"a_vocab={a_vocab} a_hidden={a_hidden} a_out={a_out}")
    print(f"decoder: {len(dec_w)} bytes f32, in_ch={in_ch} c={c0}/{c1}/{c2}/{c3}")
    print(f"sample_rate={man.get('sample_rate')} hop={man.get('hop_length')}")


if __name__ == "__main__":
    main()
