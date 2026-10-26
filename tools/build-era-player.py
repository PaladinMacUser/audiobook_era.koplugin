#!/usr/bin/env python3
"""Build the persistent PocketBook firmware PCM player (public ALSA ABI)."""
import argparse
import subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--cc',default='arm-linux-gnueabihf-gcc')
p.add_argument('--output',type=Path,default=ROOT/'.cache/rhvoice-runtime/era-player')
a=p.parse_args();a.output.parent.mkdir(parents=True,exist_ok=True)
subprocess.run([a.cc,'-std=c99','-O2','-fPIE','-pie','-Wall','-Wextra',str(ROOT/'rhvoice/era-player.c'),'-ldl','-o',str(a.output)],check=True)
a.output.chmod(0o755)
