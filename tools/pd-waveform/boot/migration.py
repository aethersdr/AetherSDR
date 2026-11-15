"""Bind a fixed migration plan to a target approval and observed predecessor."""
import re
import struct
import approval


def encode(target, container, image, executable):
    if len(target)!=140 or target[:8]!=b'PDAPRV02':
        raise ValueError('invalid target approval')
    profile,reserved=struct.unpack('>II',target[8:16])
    try:
        canonical=approval.encode(profile,target[48:76].rstrip(b'\0').decode('ascii'),
                                  target[76:140].rstrip(b'\0').decode('ascii'),target[16:48])
    except (UnicodeError,ValueError) as error:
        raise ValueError('invalid target approval') from error
    if profile!=1 or reserved or canonical!=target:
        raise ValueError('container migration requires a canonical profile1 approval')
    for value in [container,image]:
        if not re.fullmatch('[0-9a-f]{64}',value) or value=='0'*64:
            raise ValueError('invalid predecessor identifier')
    if len(executable)!=32 or executable==bytes(32):
        raise ValueError('invalid predecessor executable digest')
    return b'PDMIG001'+target+container.encode()+image.encode()+executable
