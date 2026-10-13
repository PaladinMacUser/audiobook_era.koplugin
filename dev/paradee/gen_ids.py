#!/usr/bin/env python3
"""Generate Paradee-8M phoneme id files from plain text (host side).

Paradee expects misaki phoneme ids: int64, [1,T] with 0 pad tokens at both
ends, max 512 total. The committed .ids files let the device side run with
no Python and no G2P on the Kobo.

misaki is not in nixpkgs, so this runs through the Kokoro-port toolchain
(kokoro/tools/setup-env.sh once, then the wrapper below), which carries misaki
plus the espeak-ng libraries its fallback G2P needs:
  kokoro/tools/py.sh gen_ids.py --model-dir <hf checkout> sentences.txt

Writes <stem>.ids next to each input line (one sentence per line), plus
prints the phoneme string for eyeballing.
"""
import argparse
import struct
import sys
from pathlib import Path


def write_ids(path: Path, ids, max_token):
    le = struct.Struct("<q")
    with open(path, "wb") as f:
        for tok in ids:
            if not 0 <= tok < max_token:
                sys.exit(f"token id {tok} outside Paradee vocab (n_token {max_token})")
            f.write(le.pack(tok))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--model-dir", required=True,
                    help="HF repo checkout (contains tokenizer.json)")
    ap.add_argument("inputs", nargs="+")
    ap.add_argument("--speed", type=float, default=1.0)
    args = ap.parse_args()

    model_dir = Path(args.model_dir)
    from misaki.en import G2P
    from misaki.espeak import EspeakFallback
    fallback = EspeakFallback(british=False)
    g2p = G2P(fallback=fallback)

    import json
    # Paradee's config.json vocab maps misaki phoneme char -> id (plain int).
    cfg = json.loads((model_dir / "config.json").read_text())
    vocab = cfg.get("vocab") or cfg.get("phoneme_id_map")
    if vocab is None:
        sys.exit("no vocab in config.json; inspect the HF checkout")

    for inp in args.inputs:
        text = Path(inp).read_text() if Path(inp).is_file() else inp
        for i, sent in enumerate(text.splitlines()):
            sent = sent.strip()
            if not sent:
                continue
            ps, _ = g2p(sent)
            # Misaki phoneme ids in Paradee's map are per-character; build
            # the id sequence from the phoneme string.
            ids = []
            for ch in ps:
                key = ch if ch in vocab else ch.lower()
                if key not in vocab:
                    sys.exit(f"char {ch!r} not in Paradee vocab (phonemes: {ps!r})")
                ids.append(int(vocab[key]))
            if len(ids) + 2 > 512:
                print(f"SKIP (too long): {sent[:60]}...", file=sys.stderr)
                continue
            ids = [0] + ids + [0]
            out = Path(inp).with_suffix(f".{i}.ids")
            write_ids(out, ids, int(cfg.get("n_token", 178)))
            print(f"{out}: {len(ids)} ids  {ps}")


if __name__ == "__main__":
    main()
