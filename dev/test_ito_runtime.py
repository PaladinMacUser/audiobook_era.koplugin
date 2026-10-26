#!/usr/bin/env python3
"""Real Ito worker with a locally supplied voice; never uploads the model."""
import argparse,array,json,os,socket,subprocess,tempfile,time,wave
from pathlib import Path

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--binary',required=True);p.add_argument('--model',required=True)
    p.add_argument('--phonemizer',required=True);p.add_argument('--qemu')
    a=p.parse_args()
    prefix=[a.qemu] if a.qemu else []
    checks=0
    with tempfile.TemporaryDirectory(prefix='ito-check-') as directory:
        root=Path(directory);endpoint=root/'worker.sock';ready=Path(str(endpoint)+'.ready')
        command=prefix+[a.binary,'--daemon',str(endpoint),'--data',a.model,'--phonemizer',a.phonemizer,'--idle-seconds','2']
        def wait(predicate,seconds=45):
            until=time.monotonic()+seconds
            while not predicate():
                assert time.monotonic()<until,'Ito timed out'
                time.sleep(.02)
        sender=socket.socket(socket.AF_UNIX,socket.SOCK_DGRAM)
        with (root/'worker.log').open('w') as log:
            worker=subprocess.Popen(command,stdout=log,stderr=log)
            try:
                wait(ready.exists);pid=worker.pid
                def request(index,text='Hello, world!',speed=1,volume=1,voice='ItoFemale'):
                    src=root/f'{index}.txt';src.write_text(text)
                    dest=root/f'{index}.wav';job=root/f'{index}.req'
                    job.write_text(f'{src}\n{dest}\n{speed}\n{volume}\n350\n{voice}\n1\ntext\n')
                    sender.sendto(str(job).encode(),str(endpoint));return job,dest
                def finish(job):
                    done=Path(str(job)+'.done');wait(done.exists)
                    assert worker.poll() is None and int(ready.read_text())==pid,'reuse one loaded model'
                    return done.read_text().strip()
                durations=[];energies=[]
                for index,speed,volume in ((0,1,1),(1,.5,1),(2,1,.25),(3,1,0)):
                    job,audio=request(index,speed=speed,volume=volume)
                    assert finish(job)=='OK'
                    with wave.open(str(audio)) as w:
                        assert w.getframerate()==24000 and w.getsampwidth()==2 and w.getnchannels()==1
                        assert w.readframes(8400)==bytes(16800),'keep 350 ms speaker guard'
                        pcm=array.array('h',w.readframes(w.getnframes()))
                        durations.append(len(pcm));energies.append(sum(v*v for v in pcm)/len(pcm))
                    checks+=1
                assert 1.8<durations[1]/durations[0]<2.2,'speed changes actual audio duration'
                assert .05<energies[2]/energies[0]<.08 and energies[3]==0,'volume and true mute'
                checks+=2
                job,audio=request(4,'reader');assert finish(job)=='OK' and audio.stat().st_size>16844;checks+=1
                job,audio=request(5,voice='ItoMale');assert finish(job).startswith('ERR') and not audio.exists();checks+=1
                job,audio=request(6,'reader '*100);Path(str(job)+'.cancel').write_text('cancel')
                assert finish(job)=='CANCELLED' and not audio.exists();checks+=1
                job,audio=request(7,'another word');assert finish(job)=='OK';checks+=1
                # Actual Lua normalization -> actual ARM phonemizer -> model.
                lua=os.environ.get('LUAJIT','luajit')
                script='local S=dofile("rhvoicesettings.lua");io.write(dofile("itosettings.lua").prepare("Dr. Brown paid $1,250.75 for a ﬁne book.",S.options(nil)))'
                normalized=subprocess.check_output([lua,'-e',script],text=True)
                assert '1250 dollars and 75 cents' in normalized and 'fine' in normalized
                job,audio=request(8,normalized);assert finish(job)=='OK';checks+=1
                # More than 400 phonemes must split safely and preserve the tail.
                job,audio=request(9,'reader '*90+'Goodbye.')
                assert finish(job)=='OK'
                with wave.open(str(audio)) as w: assert w.getnframes()/24000>20
                checks+=1
                wait(lambda:worker.poll() is not None,5);assert worker.returncode==0 and not endpoint.exists();checks+=1
                worker=subprocess.Popen(command,stdout=log,stderr=log);wait(ready.exists)
                worker.terminate();worker.wait(timeout=5);assert worker.returncode==0 and not ready.exists();checks+=1
            finally:
                sender.close()
                if worker.poll() is None:worker.kill();worker.wait()
    print(f'ok - {checks} real Ito ARM speech, resident model, frontend, rate, volume, guard, long text, cancellation and lifecycle checks')
if __name__=='__main__':main()
