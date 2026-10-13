# fr-siwis voice (French, fp32 piperlite pair)

The first non-English sanoTTS voice, distilled upstream from Piper's
fr_FR-siwis-medium teacher. Four blobs, loaded by the sanoTTS server's `f32`
engine (`snt_front_f32` + `snt_piperlite`), 22050 Hz, about 6.0 MB.

| file | contents |
|---|---|
| `front_meta.bin` | duration + acoustic student dims and slot table |
| `front_weights_f32.bin` | front-half weights, fp32, exporter order |
| `dec_meta.bin` | piperlite decoder dims and slot table |
| `dec_weights_f32.bin` | decoder weights, fp32, exporter order |

Source: the `fr-siwis-1p57m` fp16 package on
`huggingface.co/ampixa/sanoTTS` (voice data and training by the sanoTTS
project). The runtime kernels are MIT; the voice package is redistributed
here under the same terms the other bundled sanoTTS voices use.

Regenerate with:

```sh
python3 tools/convert-sanotts-voice.py <fr-siwis-1p57m package dir> --out sanotts/voice-fr
sha256sum sanotts/voice-fr/*.bin > sanotts/voice-fr/SHA256SUMS
```

The phoneme id map for this voice is `sanotts/phoneme_id_map_fr.h`, generated
from the `piper-phoneme-config.json` shipped inside the same package. The
server is told which map to use with `--phonemes fr` (see `piperqueue.lua`).
