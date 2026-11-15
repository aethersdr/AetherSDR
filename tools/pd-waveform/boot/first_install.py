"""Public exact-target first-install intent. Contains no admin bearer token.

The caller authenticates release artifacts and approves this exact record before
uploading a bootstrap. Encoding alone grants no installation authority.
"""
import re
import struct
import approval


def encode(target, token_hash, hostname, nonce):
    if len(target) != 140 or target[:8] != b'PDAPRV02':
        raise ValueError('invalid target approval')
    profile, reserved = struct.unpack('>II', target[8:16])
    canonical = approval.encode(profile, target[48:76].rstrip(b'\0').decode('ascii'),
                                target[76:140].rstrip(b'\0').decode('ascii'), target[16:48])
    if profile != 1 or reserved or canonical != target:
        raise ValueError('first install requires canonical profile1 approval')
    if len(token_hash) != 32 or token_hash == bytes(32) or len(nonce) != 32 or nonce == bytes(32):
        raise ValueError('nonzero token hash and fresh attempt nonce required')
    if not re.fullmatch(r'[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?', hostname):
        raise ValueError('invalid local identity hostname')
    return b'PDFST001' + target + token_hash + hostname.encode().ljust(64, b'\0') + nonce + bytes(32)
