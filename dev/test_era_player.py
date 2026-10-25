#!/usr/bin/env python3
"""Test persistent firmware PCM lifetime, exact audio, cancellation and pause."""
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import time
import wave
ROOT=Path(__file__).resolve().parents[1]

def wait(predicate,seconds=6):
    deadline=time.monotonic()+seconds
    while not predicate():
        assert time.monotonic()<deadline,'player timed out'
        time.sleep(.01)

def check_player(command,env,root,fixture=False):
    endpoint=root/'player.sock'
    audio=root/'speech.wav'
    speech=struct.pack('<h',1234)*12000
    with wave.open(str(audio),'wb') as f:
        f.setparams((1,2,24000,0,'NONE','not compressed'));f.writeframes(bytes(16800)+speech)
    worker=subprocess.Popen(command+['--daemon',str(endpoint),'--idle-seconds','1'],env=env,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
    sender=socket.socket(socket.AF_UNIX,socket.SOCK_DGRAM)
    try:
        wait(lambda:Path(str(endpoint)+'.ready').exists())
        index=0
        def submit(wav=audio):
            nonlocal index
            index+=1
            job=root/f'{index}.req';src=root/f'{index}.txt';out=root/f'{index}.unused'
            src.write_text(str(wav));job.write_text(f'{src}\n{out}\n1\n1\n350\n')
            sender.sendto(str(job).encode(),str(endpoint));return job
        def result(job):
            done=Path(str(job)+'.done');wait(done.exists);return done.read_text().strip()
        def playing(job):
            p=Path(str(job)+'.playing');wait(p.exists);return int(p.read_text())
        first=submit();assert playing(first)==350 and result(first)=='OK'
        if fixture:
            pcm=(root/'capture.pcm').read_bytes()
            assert pcm[:350*48*4]==bytes(350*48*4),'fresh speaker retains full lead-in'
            assert pcm.count(struct.pack('<hh',1234,1234))==24000,'every speech frame survives mono->stereo conversion'
            assert (root/'events').read_text()=='open\n','natural completion keeps exactly one PCM open'
            offset=len(pcm)
        second=submit();assert playing(second)==0,'warm speaker skips guard instead of fading speech'
        pause=Path(str(second)+'.pause');pause.write_text('pause');time.sleep(.12)
        assert not Path(str(second)+'.done').exists(),'pause cannot finish or advance a clip'
        if fixture:
            start=(root/'capture.pcm').stat().st_size;time.sleep(.05)
            assert not any((root/'capture.pcm').read_bytes()[start:]),'paused player feeds silence'
        pause.unlink();assert result(second)=='OK'
        if fixture:
            pcm=(root/'capture.pcm').read_bytes()[offset:]
            assert pcm.count(struct.pack('<hh',1234,1234))==24000,'warm playback preserves complete speech'
            assert (root/'events').read_text()=='open\n','two sentences reuse one PCM handle'
        cancelled=submit();playing(cancelled);Path(str(cancelled)+'.cancel').write_text('cancel')
        assert result(cancelled)=='CANCELLED'
        if fixture:wait(lambda:'close' in (root/'events').read_text())
        fresh=submit();assert playing(fresh)==350 and result(fresh)=='OK','new stream after stop restores wakeup protection'
        bad=root/'bad.wav';bad.write_bytes(b'bad')
        assert result(submit(bad)).startswith('ERR'),'invalid WAV cannot play or crash the service'
        worker.wait(timeout=4)
        assert worker.returncode==0 and not endpoint.exists() and not Path(str(endpoint)+'.ready').exists(),'idle exit releases socket and PCM'
        if fixture:
            events=(root/'events').read_text();assert events.count('open')==2 and events.count('close')==2,'all opened PCMs close'
    except Exception as error:
        if worker.poll() is None: worker.terminate();worker.wait(timeout=3)
        raise AssertionError(str(error)+"; player stderr: "+worker.stderr.read().decode(errors="replace")) from error
    finally:
        sender.close()
        if worker.poll() is None:worker.terminate();worker.wait(timeout=3)
    print('ok - persistent Era PCM: guarded start, warm reuse, exact speech, pause/resume, cancel, restart, invalid input and idle release')

def main():
    with tempfile.TemporaryDirectory(prefix='audiobook-pcm-') as directory:
        root=Path(directory)
        subprocess.run(['cc','-std=c99','-O2','-Wall','-Wextra','-Werror',str(ROOT/'rhvoice/era-player.c'),'-ldl','-o',str(root/'player')],check=True)
        subprocess.run(['cc','-shared','-fPIC','-Wall','-Wextra','-Werror',str(ROOT/'dev/fixtures/alsa_pcm.c'),'-o',str(root/'libasound.so.2')],check=True)
        env=os.environ.copy();env.update(LD_LIBRARY_PATH=str(root),ERA_PCM_EVENTS=str(root/'events'),ERA_PCM_CAPTURE=str(root/'capture.pcm'))
        check_player([str(root/'player')],env,root,fixture=True)
if __name__=='__main__':main()
