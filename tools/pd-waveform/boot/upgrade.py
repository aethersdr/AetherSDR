"""Encode explicit approval for a release/key transition; grants no authority."""
import approval


def encode(profile, firmware, serial, previous_digest, candidate_digest,
           previous_key, candidate_key):
    if (len(previous_key) != 32 or len(candidate_key) != 32
            or previous_digest == candidate_digest):
        raise ValueError('upgrade requires distinct releases and two public keys')
    return (b'PDUPGR01'
            + approval.encode(profile, firmware, serial, previous_digest)
            + approval.encode(profile, firmware, serial, candidate_digest)
            + previous_key + candidate_key)
