#!/usr/bin/env python3
"""Build an offline ARM Ito adapter. Voice weights remain user-supplied."""
import argparse
import shutil
import subprocess
import tarfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
REVISION='118a238b3fbb754d3364c52a167f11b9142ca6a2'
def run(*args): subprocess.run([str(a) for a in args],check=True)
def build(source,output,cc,cxx,cmake,jobs):
    if not (source/'.git').exists():
        source.mkdir(parents=True,exist_ok=True)
        run('git','init','-q',source)
        run('git','-C',source,'remote','add','origin','https://github.com/lokutor-ai/ito.git')
        run('git','-C',source,'fetch','-q','--depth','1','origin',REVISION)
        run('git','-C',source,'checkout','-q','--detach','FETCH_HEAD')
    actual=subprocess.check_output(['git','-C',str(source),'rev-parse','HEAD'],text=True).strip()
    if actual!=REVISION: raise ValueError('Unexpected Ito source revision')
    directory=output.parent/(output.name+'-build')
    run(cmake,'-S',ROOT/'ito','-B',directory,'-DITO_SOURCE='+str(source.resolve()),
        '-DCMAKE_BUILD_TYPE=Release','-DCMAKE_SYSTEM_NAME=Linux','-DCMAKE_SYSTEM_PROCESSOR=arm',
        '-DCMAKE_C_COMPILER='+cc,'-DCMAKE_CXX_COMPILER='+cxx,'-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY')
    run(cmake,'--build',directory,'--parallel',jobs)
    output.mkdir(parents=True,exist_ok=True)
    shutil.copyfile(directory/'ito-speak',output/'ito-speak');(output/'ito-speak').chmod(0o755)
    for name in ('LICENSE','NOTICE'): shutil.copyfile(source/name,output/(name+'-Ito'))
    for name in ('LICENSE-WEIGHTS','TERMS.md'): shutil.copyfile(source/'models'/name,output/name)
    (output/'SOURCE-REVISION').write_text(REVISION+'\n')
    with tarfile.open(output/'corresponding-source.tar.gz','w:gz') as archive:
        for name in ('esp32/engine','LICENSE','NOTICE','models/TERMS.md','models/LICENSE-WEIGHTS'):
            archive.add(source/name,'source/ito/'+name)
        archive.add(ROOT/'ito','source/era/ito')
        for name in ('sonic.c','sonic.h','LICENSE'): archive.add(ROOT/'tempo'/name,'source/era/tempo/'+name)
        archive.add(Path(__file__),'source/era/tools/build-ito.py')
def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source-dir',type=Path,default=ROOT/'.cache/ito-source')
    p.add_argument('--output',type=Path,default=ROOT/'.cache/ito-runtime')
    p.add_argument('--cc',default='arm-linux-gnueabihf-gcc');p.add_argument('--cxx',default='arm-linux-gnueabihf-g++')
    p.add_argument('--cmake',default='cmake');p.add_argument('--jobs',type=int,default=2)
    a=p.parse_args();build(a.source_dir,a.output,a.cc,a.cxx,a.cmake,a.jobs)
if __name__=='__main__':main()
