#!/usr/bin/env python3
"""Deterministic IPC fixture; actual RHVoice audio is tested under ARM emulation."""
import argparse
import os
from pathlib import Path
import socket
import time
import wave

p = argparse.ArgumentParser()
p.add_argument('--daemon', required=True)
p.add_argument('--idle-seconds', type=int, default=120)
a = p.parse_args()
endpoint = Path(a.daemon)
endpoint.unlink(missing_ok=True)
s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
s.bind(str(endpoint))
s.settimeout(a.idle_seconds)
Path(str(endpoint) + '.ready').write_text(str(os.getpid()))
try:
    while True:
        try:
            job = Path(s.recv(4096).decode())
        except socket.timeout:
            break
        src, dest, rate, volume, lead = job.read_text().splitlines()
        text = Path(src).read_text()
        if text == 'crash':
            os._exit(7)
        cancel = Path(str(job) + '.cancel')
        if text == 'slow':
            for _ in range(300):
                if cancel.exists():
                    break
                time.sleep(.01)
        if cancel.exists():
            result = 'CANCELLED'
        elif text == 'fail':
            result = 'ERR synthesis failed'
        else:
            with wave.open(dest, 'wb') as f:
                f.setparams((1, 2, 24000, 0, 'NONE', 'not compressed'))
                f.writeframes(bytes(int(4800 / float(rate)) + int(float(lead)) * 48))
            result = 'OK'
        marker = Path(str(job) + '.done')
        temporary = Path(str(marker) + '.new')
        temporary.write_text(result + '\n')
        temporary.replace(marker)
finally:
    s.close()
    endpoint.unlink(missing_ok=True)
    Path(str(endpoint) + '.ready').unlink(missing_ok=True)
