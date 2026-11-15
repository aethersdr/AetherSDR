"""Encode explicit approval for a release/key transition; grants no authority."""
import approval


def encode(profile, firmware, serial, previous_digest, candidate_digest,
           previous_key, candidate_key, *, attempt=None):
    if (len(previous_key) != 32 or len(candidate_key) != 32
            or previous_digest == candidate_digest):
        raise ValueError('upgrade requires distinct releases and two public keys')
    plan = (b'PDUPGR01'
            + approval.encode(profile, firmware, serial, previous_digest)
            + approval.encode(profile, firmware, serial, candidate_digest)
            + previous_key + candidate_key)
    if attempt is None:
        return plan
    if len(attempt) != 32 or not any(attempt):
        raise ValueError('attempt must be a nonzero 32-byte nonce')
    return b'PDUPGR02' + plan[8:56] + attempt + bytes(60) + plan[148:]
