#!/usr/bin/env python3
"""Embed verified candidate management bytes in an independently trusted worker.

The native verifier authenticates a private snapshot before any C output is
published. Recovery then needs neither the caller's files nor the rejected DSP
payload to restore these exact management files. This tool installs nothing.
"""
import argparse
import os
from pathlib import Path
import stat
import subprocess
import tempfile
import bundle
import release


def array(data):
    return '\n'.join(','.join(str(x) for x in data[i:i+24])+',' for i in range(0,len(data),24))


def emit(artifact, public_key, arch, output, verifier):
    if arch not in bundle.ARCH or len(public_key)!=32:
        raise ValueError('invalid management architecture or trust key')
    fd=os.open(artifact,os.O_RDONLY|os.O_NOFOLLOW|os.O_NONBLOCK)
    with os.fdopen(fd,'rb') as source:
        info=os.fstat(source.fileno())
        if not stat.S_ISREG(info.st_mode) or not release.PREFIX<=info.st_size<=release.MAX_BYTES:
            raise ValueError('invalid bounded release input')
        data=source.read(release.MAX_BYTES+1)
    if len(data)!=info.st_size or data[:8] not in (b'AEPDR003',b'AEPDR004',b'AEPDR005'):
        raise ValueError('a complete managed-startup release is required')
    firmware=data[52:80].split(b'\0',1)[0].decode('ascii')
    digest=bundle.blake(data)
    output.parent.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='pd-management-verify-',dir=output.parent) as directory:
        root=Path(directory);snapshot=root/'release';snapshot.write_bytes(data)
        key=root/'key';key.write_bytes(public_key);approved=root/'approved';approved.write_bytes(digest)
        extracted=root/'extracted';extracted.mkdir()
        result=subprocess.run([str(verifier),str(snapshot),str(key),str(bundle.ARCH[arch]),
                               '2' if arch=='armv7' else '1',firmware,str(approved),str(extracted)],
                              capture_output=True,timeout=30)
        if result.returncode:
            raise ValueError('native release authentication/extraction refused')
        boot=(extracted/'pd-boot').read_bytes();licenses=(extracted/'BOOT-LICENSES.txt').read_bytes()
    text=('/* Generated from a fully verified release; no private key material. */\n'
          '#include "pd_management.h"\n'
          'static const unsigned char pd_management_boot_bytes[]={\n'+array(boot)+'\n};\n'
          'static const unsigned char pd_management_license_bytes[]={\n'+array(licenses)+'\n};\n'
          'static const struct pd_management_blob pd_management_embedded={\n'
          '.boot=pd_management_boot_bytes,.boot_size=sizeof(pd_management_boot_bytes),\n'
          '.licenses=pd_management_license_bytes,.license_size=sizeof(pd_management_license_bytes),\n'
          '.header={\n'+array(data[:release.PREFIX])+'\n},\n'
          '.release_digest={'+','.join(str(x) for x in digest)+'}\n};\n')
    bundle.atomic_write(output,text.encode())
    return {'release_blake2b_256':digest.hex(),'boot_bytes':len(boot),'license_bytes':len(licenses),'header_bytes':release.PREFIX}


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--release',type=Path,required=True)
    p.add_argument('--public-key',type=Path,required=True)
    p.add_argument('--arch',choices=bundle.ARCH,required=True)
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--verifier',type=Path,required=True,help='current locally built native release-check')
    a=p.parse_args()
    try:emit(a.release,a.public_key.read_bytes(),a.arch,a.output,a.verifier.resolve())
    except (OSError,ValueError,subprocess.TimeoutExpired) as error:p.exit(1,str(error)+'\n')

if __name__=='__main__':main()
