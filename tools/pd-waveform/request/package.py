"""Assemble an offline/bench fixture; not a production deployment format.

The tested radio retains uploaded native archives as mode0644 and extracts
configuration under a different UID. Do not ship request secrets this way.
See README.md for the observed permission blocker and required replacement.
"""
import hashlib
import io
import os
from pathlib import Path
import stat
import struct
import tempfile
import zipfile
import client

NAME='AetherPDInstall'
CFG=b'''[header]
Name: AetherPDInstall
Version: "0.1.0"
Minimum-SmartSDR-Version: 4.2.18.0
Author: AetherSDR
License: GPL-3.0
Executable: "pd-request"

[setup]

[end]
'''

def assemble(executable,private_config,licenses):
    _,receipt=client.configuration(private_config)
    arch=struct.unpack_from('>I',receipt,8)[0]
    machine=183 if arch==64 else 40
    elf_class=2 if arch==64 else 1
    if (not 64<=len(executable)<=512*1024 or executable[:6]!=b'\x7fELF'+bytes([elf_class,1])
            or struct.unpack_from('<H',executable,18)[0]!=machine):
        raise ValueError('request executable architecture does not match approved receipt')
    if not 0<len(licenses)<=256*1024:raise ValueError('bounded license bundle required')
    output=io.BytesIO()
    files={'AetherPDInstall.cfg':(CFG,0o600),'pd-request':(executable,0o700),'config':(private_config,0o600),'LICENSES':(licenses,0o600)}
    with zipfile.ZipFile(output,'w',zipfile.ZIP_DEFLATED,compresslevel=9) as archive:
        for name,(data,mode) in sorted(files.items()):
            entry=zipfile.ZipInfo(NAME+'/'+name,(2026,1,1,0,0,0));entry.create_system=3
            entry.compress_type=zipfile.ZIP_DEFLATED;entry.external_attr=(stat.S_IFREG|mode)<<16
            archive.writestr(entry,data,compresslevel=9)
    data=output.getvalue()
    return data,{'sha256':hashlib.sha256(data).hexdigest(),'bytes':len(data),'unpacked_bytes':sum(len(x[0]) for x in files.values()),'architecture':arch,'contains_release_payloads':False}

def publish(target,data):
    target=Path(target)
    fd,name=tempfile.mkstemp(prefix='.aetherpd-request-',dir=target.parent)
    try:
        with os.fdopen(fd,'wb') as file:file.write(data);file.flush();os.fsync(file.fileno())
        os.link(name,target)
    finally:os.unlink(name)
