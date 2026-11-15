"""Encode target-bound approval after operator consent; confers no authority."""
import re
import struct


def encode(profile, firmware, serial, digest):
    if (profile not in (1, 2) or len(digest) != 32
            or not re.fullmatch(r'[0-9]+(?:\.[0-9]+){3}', firmware)
            or len(firmware) >= 28
            or not re.fullmatch(r'[A-Za-z0-9.-]{1,63}', serial)):
        raise ValueError('invalid approval target or digest')
    return (b'PDAPRV02' + struct.pack('>II', profile, 0) + digest
            + firmware.encode().ljust(28, b'\0') + serial.encode().ljust(64, b'\0'))
