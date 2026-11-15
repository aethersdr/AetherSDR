#!/usr/bin/env python3
"""Build the native installer request or its test adapter; no upload or signing."""
import argparse
import os
from pathlib import Path
import subprocess
HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[2]

def build(output,arch='host',sanitize=False,request=False,fixture=False,inherited_fixture=False):
    output.mkdir(parents=True,exist_ok=True)
    compiler=['cc'] if sanitize else ['zig','cc']
    targets={'host':[],'arm64':['-target','aarch64-linux-musl'],'armv7':['-target','arm-linux-musleabihf','-mcpu=cortex_a8']}
    flags=['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'] if sanitize else ['-O2','-static','-s','-Wl,--build-id=none']
    target=output/('pd-request' if request else 'upload-check')
    entries=[HERE/('pd_request.c' if request else 'upload_check.c'),HERE/'pd_upload.c',ROOT/'third_party/monocypher/monocypher.c']
    if request:entries += [HERE.parent/'boot/pd_transport.c']
    defines=[] if request and not fixture else ['-DPD_UPLOAD_TEST']
    if fixture:defines += ['-DPD_REQUEST_TEST']
    if inherited_fixture:defines += ['-DPD_REQUEST_IDENTITY_TEST']
    subprocess.run(compiler+targets[arch]+flags+defines+['-Wall','-Wextra','-Werror','-I',str(HERE.parent/'boot'),'-I',str(ROOT/'third_party/monocypher'),*map(str,entries),'-o',str(target)],check=True,env=dict(os.environ,ZIG_GLOBAL_CACHE_DIR=str(output/'cache'),ZIG_LOCAL_CACHE_DIR=str(output/'cache')))
    return target

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);p.add_argument('--arch',choices=['host','arm64','armv7'],default='host');p.add_argument('--sanitize',action='store_true');p.add_argument('--request',action='store_true');p.add_argument('--fixture',action='store_true');p.add_argument('--inherited-fixture',action='store_true');a=p.parse_args()
    if a.sanitize and a.arch!='host':p.error('sanitizers are host-only')
    if a.fixture and (a.arch!='host' or not a.request):p.error('fixture requires host request build')
    if a.inherited_fixture and (a.arch!='host' or not a.request or a.fixture):p.error('inherited fixture requires host request without fixture listener')
    print(build(a.output.resolve(),a.arch,a.sanitize,a.request,a.fixture,a.inherited_fixture))
