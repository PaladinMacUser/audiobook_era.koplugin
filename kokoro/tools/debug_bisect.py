#!/usr/bin/env python3
"""Bisect the numpy reference against torch, module by module (M1 debugging).

Runs the author's own PyTorch modules and the numpy equivalents on the same
inputs and prints the max error per module, so a divergence is localised to one
implementation rather than inferred from the whole-pipeline goldens.

Usage: kokoro/tools/py.sh kokoro/tools/debug_bisect.py --repo kokoro/model/paradee-repo \
         --text kokoro/model/text_side.pt --dec kokoro/model/decoder.pt --ids kokoro/goldens/paradee-8m/s0_ids.npy
"""
import argparse
import sys

import numpy as np
import torch

sys.path.insert(0, "kokoro/tools")
from check_numpy_ref import load_sd  # noqa: E402
from numpy_model import Albert, TextEncoder  # noqa: E402


def err(name, mine, ref):
    a = np.asarray(mine, dtype=np.float64).reshape(-1)
    b = np.asarray(ref.detach().numpy() if hasattr(ref, "detach") else ref, dtype=np.float64).reshape(-1)
    if a.size != b.size:
        print(f"  {name:24s} SIZE {a.size} vs {b.size}")
        return
    corr = np.corrcoef(a, b)[0, 1] if a.std() and b.std() else float("nan")
    print(f"  {name:24s} n={a.size:7d} maxerr {np.abs(a - b).max():10.3e}  corr {corr:8.5f}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", required=True)
    ap.add_argument("--text", required=True)
    ap.add_argument("--dec", required=True)
    ap.add_argument("--ids", required=True)
    args = ap.parse_args()
    sys.path.insert(0, args.repo + "/training/scripts")

    from student import TextStudent
    ts = TextStudent("s+mlp").eval()
    ts.load_state_dict(torch.load(args.text, map_location="cpu", weights_only=False)["model"])
    # fold legacy weight_norm into plain weights for the numpy side
    sd = load_sd(args.text)

    ids = np.load(args.ids)
    idsb = torch.tensor([ids], dtype=torch.long)
    style = sd["style"][0]

    with torch.no_grad():
        print("ALBERT")
        t_bert = ts.bert(idsb, attention_mask=torch.ones_like(idsb))
        mine = Albert(sd, 6)(ids)
        err("bert.last_hidden_state", mine.T, t_bert)

        print("BERT -> d_en")
        t_en = ts.bert_encoder(t_bert).transpose(-1, -2)
        from numpy_ref import linear
        mine_en = linear(mine, sd["bert_encoder.weight"], sd["bert_encoder.bias"])
        err("d_en", mine_en, t_en)

        print("DurationEncoder")
        t_d = ts.predictor.text_encoder(t_en, ts.style, torch.tensor([len(ids)]),
                                        torch.zeros(1, len(ids), dtype=torch.bool))
        from numpy_model import DurationEncoder
        mine_d = DurationEncoder(sd, "predictor.text_encoder", 3, 32)(mine_en, style)
        err("duration_encoder", mine_d.T, t_d)

        print("duration path (stage by stage)")
        from numpy_ref import ada_layer_norm as _adaln, bilstm as _bilstm
        x_np = np.concatenate([mine_en, np.repeat(style[:, None], mine_en.shape[1], axis=1)], axis=0)
        x_t = torch.cat([t_en, ts.style[:, :, None].expand(-1, -1, t_en.shape[2])], 1)
        for i in range(3):
            x_np = _bilstm(x_np, sd, f"predictor.text_encoder.lstms.{2 * i}")
            x_t = ts.predictor.text_encoder.lstms[2 * i](x_t.transpose(-1, -2))[0].transpose(-1, -2)
            err(f"  de.lstm{i}", x_np, x_t)
            x_np = _adaln(x_np, style, sd, f"predictor.text_encoder.lstms.{2 * i + 1}")
            x_t = ts.predictor.text_encoder.lstms[2 * i + 1](x_t.transpose(-1, -2), ts.style).transpose(-1, -2)
            err(f"  de.adaln{i}", x_np, x_t)
            x_np = np.concatenate([x_np, np.repeat(style[:, None], x_np.shape[1], axis=1)], axis=0)
            x_t = torch.cat([x_t, ts.style[:, :, None].expand(-1, -1, x_t.shape[2])], 1)
        print("duration path")
        t_lstm = ts.predictor.lstm(t_d)[0]
        from numpy_ref import bilstm, linear as lin2, sigmoid
        mine_l = bilstm(mine_d, sd, "predictor.lstm")
        err("lstm(d)", mine_l, t_lstm.transpose(-1, -2))
        t_dur = ts.predictor.duration_proj(t_lstm)
        mine_proj = lin2(mine_l, sd["predictor.duration_proj.linear_layer.weight"],
                         sd["predictor.duration_proj.linear_layer.bias"])
        err("duration_proj", mine_proj.T, t_dur)
        err("dur_raw", sigmoid(mine_proj).sum(0), torch.sigmoid(t_dur).sum(-1)[0])

        print("ALBERT layer by layer")
        emb = ts.bert.embeddings(idsb, token_type_ids=torch.zeros_like(idsb))
        err("embeddings", Albert(sd, 0)(ids).T, emb)
        h = ts.bert.encoder.embedding_hidden_mapping_in(emb)
        err("hidden_mapping", Albert(sd, 0)(ids).T, h)
        mask4 = torch.zeros(1, 1, 1, len(ids))
        layer = ts.bert.encoder.albert_layer_groups[0].albert_layers[0]
        for n in range(1, 7):
            h = layer(h, attention_mask=mask4)
            err(f"after layer {n}", Albert(sd, n)(ids).T, h)

        print("layer internals (config.hidden_act = "
              f"{ts.bert.config.hidden_act}, eps {ts.bert.config.layer_norm_eps})")
        from numpy_ref import gelu, layer_norm, leaky_relu, linear as _lin, softmax
        h0 = ts.bert.encoder.embedding_hidden_mapping_in(emb)
        att_ref = layer.attention(h0, attention_mask=mask4)[0]
        mine_att = Albert(sd, 0).attention_block(h0.detach().numpy()[0].T)
        err("attention block", mine_att.T, att_ref)
        ffn_ref = layer.ffn_output(layer.activation(layer.ffn(att_ref)))
        mine_ffn = Albert(sd, 0).ffn_block(mine_att)
        err("ffn block", mine_ffn.T, ffn_ref)


if __name__ == "__main__":
    main()