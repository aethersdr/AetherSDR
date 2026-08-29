"""HPSDR Protocol 1 (Metis) primitives for the HL2 spike — grounded, not guessed.

Register map sourced from the Hermes-Lite 2 wiki "Protocol" page and cross-checked
against the pihpsdr reference client's C&C construction (src/old_protocol.c),
consulted clean-room for wire-protocol facts only — see ../../THIRD_PARTY_LICENSES
(Principle I). Verified live against a real HL2 (WWV 10 MHz carrier lands exactly
at baseband DC; see README.md phase 0.3/0.4).

  C0 byte:  bits [6:1] = register ADDR[5:0], bit [0] = MOX (1=TX). So the C0 byte
            for a register = (addr << 1) | mox. Keep C0 EVEN → MOX=0 → never keys.
  addr 0x00 → C0 0x00 : config. C1 = speed(bits[1:0]) | CONFIG_MERCURY(0x40);
                        C4 = duplex(0x04) | ((#RX-1) & 0x7) << 3.
                        *** C1 bit6 (CONFIG_MERCURY) selects the ADC as the RX
                        source — WITHOUT it the DDC gets no input and the stream
                        is dead ADC-floor noise. This is the non-obvious must-set. ***
  addr 0x01 → C0 0x02 : TX1 NCO frequency (Hz, 32-bit)  ***DO NOT SET for RX***
  addr 0x02 → C0 0x04 : RX1 NCO frequency (Hz, 32-bit, big-endian in C1..C4)
  addr 0x0a → C0 0x14 : ADC gain. HL2 extended-range LNA: C4 = 0x40 | gain,
                        gain 0..60 = -12..+48 dB (i.e. code = dB + 12).

Register value is 32-bit; C1=bits[31:24], C2=[23:16], C3=[15:8], C4=[7:0].

Wire framing:
  metis command : EF FE 04 <cmd>  (pad 64)   cmd bit0 = IQ on/off
  EP2 (→radio)  : EF FE 01 02 | seq[4] | frame512 | frame512
  EP6 (←radio)  : EF FE 01 06 | seq[4] | frame512 | frame512
  each 512-B frame: 7F 7F 7F | C0 C1 C2 C3 C4 | 504 B payload
  RX sample (1 RX): I[3] Q[3] mic[2] = 8 B; I/Q are 24-bit signed big-endian.

Minimal working RX = round-robin three registers: config (with CONFIG_MERCURY),
RX1 freq, and ADC gain. Sending only freq (no Mercury bit) yields flat noise.
"""

import struct

METIS_PORT = 1024
SYNC = b"\x7f\x7f\x7f"
FULL_SCALE = 1 << 23  # 24-bit signed full scale

# C0 register-address bytes (address << 1, MOX=0).
C0_CONFIG = 0x00
C0_TX1_FREQ = 0x02   # avoid — transmit
C0_RX1_FREQ = 0x04
C0_ADC_GAIN = 0x14   # register 0x0a

CONFIG_MERCURY = 0x40   # C1 bit6: select ADC as RX source (mandatory for signal)
CONFIG_DUPLEX = 0x04    # C4 bit2: pihpsdr sets this on unconditionally


def metis_command(cmd: int) -> bytes:
    """EF FE 04 <cmd> padded to 64 B. cmd 0x01 = start IQ, 0x00 = stop."""
    return bytes([0xEF, 0xFE, 0x04, cmd]) + bytes(60)


def cc_config(speed: int = 0, n_rx: int = 1) -> bytes:
    """5-byte C&C for register 0x00: sample rate + receiver count + ADC select.
    C1 carries the speed AND CONFIG_MERCURY (without which there is no RX signal).
    C4 carries the duplex bit and the receiver count. MOX stays 0."""
    c1 = (speed & 0x3) | CONFIG_MERCURY
    c4 = CONFIG_DUPLEX | (((n_rx - 1) & 0x7) << 3)
    return bytes([C0_CONFIG, c1, 0x00, 0x00, c4])


def cc_rx1_freq(hz: int) -> bytes:
    """5-byte C&C for register 0x02: RX1 NCO frequency in Hz (32-bit BE). MOX 0."""
    return bytes([C0_RX1_FREQ]) + struct.pack(">I", hz & 0xFFFFFFFF)


def cc_rx_gain(db: int = 20) -> bytes:
    """5-byte C&C for register 0x0a: HL2 extended-range LNA gain, -12..+48 dB.
    C4 = 0x40 (enable direct AD9866 gain) | code, code = clamp(dB+12, 0, 60)."""
    code = max(0, min(60, db + 12))
    return bytes([C0_ADC_GAIN, 0x00, 0x00, 0x00, 0x40 | code])


def ep2_packet(seq: int, cc_a: bytes, cc_b: bytes) -> bytes:
    """EP2 host→radio packet: two frames, each carrying one C&C register.
    TX payload is zero (RX-only). cc_* must be exactly 5 bytes (C0..C4)."""
    assert len(cc_a) == 5 and len(cc_b) == 5
    frame_a = SYNC + cc_a + bytes(504)
    frame_b = SYNC + cc_b + bytes(504)
    return bytes([0xEF, 0xFE, 0x01, 0x02]) + struct.pack(">I", seq) + frame_a + frame_b


def parse_ep6(pkt: bytes):
    """Return (seq, n_samples, peak_abs, sumsq, sync_ok) or None if not an EP6 packet.
    Accumulates level stats rather than materializing all samples."""
    if len(pkt) < 1032 or pkt[0] != 0xEF or pkt[1] != 0xFE or pkt[2] != 0x01 or pkt[3] != 0x06:
        return None
    seq = struct.unpack(">I", pkt[4:8])[0]
    n, peak, sumsq, sync_ok = 0, 0, 0.0, True
    for fstart in (8, 520):
        frame = pkt[fstart:fstart + 512]
        if frame[0:3] != SYNC:
            sync_ok = False
            continue
        payload = frame[8:512]
        for k in range(0, 504, 8):
            i = int.from_bytes(payload[k:k + 3], "big", signed=True)
            q = int.from_bytes(payload[k + 3:k + 6], "big", signed=True)
            a = abs(i) if abs(i) > abs(q) else abs(q)
            if a > peak:
                peak = a
            sumsq += float(i) * i + float(q) * q
            n += 1
    return seq, n, peak, sumsq, sync_ok


def ep6_seq(pkt: bytes):
    """Sequence number of an EP6 packet, or None if it isn't one. Cheap — reads
    only the header, no per-sample decode (use when you want seq + iq_samples()
    without paying for parse_ep6's discarded level stats)."""
    if len(pkt) < 8 or pkt[0] != 0xEF or pkt[1] != 0xFE or pkt[2] != 0x01 or pkt[3] != 0x06:
        return None
    return struct.unpack(">I", pkt[4:8])[0]


def iq_samples(pkt: bytes):
    """Yield (I, Q) tuples from an EP6 packet — for FFT/spectrum use."""
    if len(pkt) < 1032 or pkt[0] != 0xEF or pkt[1] != 0xFE or pkt[2] != 0x01 or pkt[3] != 0x06:
        return
    for fstart in (8, 520):
        frame = pkt[fstart:fstart + 512]
        if frame[0:3] != SYNC:
            continue
        payload = frame[8:512]
        for k in range(0, 504, 8):
            i = int.from_bytes(payload[k:k + 3], "big", signed=True)
            q = int.from_bytes(payload[k + 3:k + 6], "big", signed=True)
            yield i, q
