# Kristin speech tempo on PocketBook Era

`wav-tempo` changes the tempo of a completed mono PCM16 WAV with Sonic,
keeping its sample rate and pitch. For the Era's Kristin voice, the plugin
renders at the model's native duration scale and applies the requested rate
to the waveform. This avoids asking the small duration model to generate
very long phonemes when the reader selects a slower speed such as 0.5×.
It cannot improve the underlying voice or turn Kristin into Julie.

Both resident read-along synthesis and one-shot **Read word** use the same
conversion. A failed conversion fails that request instead of playing the
original fast clip. Converted WAVs replace their temporary input atomically.
Native word timestamps are removed after tempo conversion; highlighting
uses estimated timing based on the converted audio duration.

Sonic source (`sonic.c`, `sonic.h`, `LICENSE`) is vendored unchanged from
[waywardgeek/sonic at b93885dcb70aae50c6f76b0fe4e0868f029a077e](https://github.com/waywardgeek/sonic/tree/b93885dcb70aae50c6f76b0fe4e0868f029a077e),
Apache-2.0. `wav-tempo.c` is this fork's command-line adapter.

Build a static ARM binary (Debian/Ubuntu `gcc-arm-linux-gnueabihf`):

```sh
arm-linux-gnueabihf-gcc -std=c11 -O2 -static -Itempo \
  tempo/wav-tempo.c tempo/sonic.c -lm -o /tmp/wav-tempo
python3 tools/package-era.py --tempo-binary /tmp/wav-tempo
```

Without `--tempo-binary`, the packaging tool invokes this cross-compiler.
A static musl build is also supported, for example with Zig 0.13.0:

```sh
zig cc -target arm-linux-musleabihf -std=c11 -O2 -static -Itempo \
  tempo/wav-tempo.c tempo/sonic.c -lm -o /tmp/wav-tempo
```
