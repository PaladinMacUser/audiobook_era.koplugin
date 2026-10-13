#!/usr/bin/env python3
"""Host sanity run for Paradee-8M: ids files -> onnxruntime -> RTF + WAVs.

Uses the int8 graph exactly as the device runner will (single thread), so
the RTFs here bound what a desktop can do; the Kobo numbers come from
paradee_runner.c on the device.

  kokoro/tools/py.sh host_sanity.py model/paradee_int8.onnx 'sentences.*.ids'
"""
import glob
import struct
import sys
import time

import numpy as np
import onnxruntime as ort

SR = 24000


def write_wav(path, pcm):
    data = (np.clip(pcm, -1, 1) * 32767).astype("<i2").tobytes()
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVEfmt " +
                struct.pack("<IHHIIHH", 16, 1, 1, SR, SR * 2, 2, 16) +
                b"data" + struct.pack("<I", len(data)) + data)


def main():
    model, pattern = sys.argv[1], sys.argv[2]
    opts = ort.SessionOptions()
    opts.intra_op_num_threads = 1
    opts.inter_op_num_threads = 1
    so = ort.InferenceSession(model, opts, providers=["CPUExecutionProvider"])
    print("inputs:", [(i.name, i.shape, i.type) for i in so.get_inputs()])
    print("outputs:", [(o.name, o.shape, o.type) for o in so.get_outputs()])

    total_synth = total_audio = 0.0
    for path in sorted(glob.glob(pattern)):
        ids = np.fromfile(path, dtype="<i8")
        t0 = time.perf_counter()
        out = so.run(None, {"input_ids": ids[None, :], "speed": np.array([1.0], dtype=np.float32)})
        dt = time.perf_counter() - t0
        pcm = out[0][0]
        audio = len(pcm) / SR
        total_synth += dt
        total_audio += audio
        wav = path.rsplit(".", 1)[0] + ".wav"
        write_wav(wav, pcm)
        print(f"{path}: T={len(ids)} samples={len(pcm)} synth={dt:.3f}s "
              f"audio={audio:.2f}s RTF={dt / audio:.3f} -> {wav}")
    if total_audio:
        print(f"TOTAL: synth={total_synth:.3f}s audio={total_audio:.2f}s "
              f"RTF={total_synth / total_audio:.3f}")


if __name__ == "__main__":
    main()
