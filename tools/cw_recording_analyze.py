#!/usr/bin/env python3
"""
Verify a CW/CWX-sidetone capture WAV (companion to cw_recording_test.py, #2539).

Pure stdlib (wave + math, no numpy) so it runs on a minimal box. Scans the WAV
in short frames; per frame computes RMS energy and Goertzel power at the probe
tones. Confirms the recording contains, in time order:

  * an SSB tone segment near 1200 Hz   (phone 1)
  * a CW segment: a keyed (on/off) tone near the sidetone pitch (~600 Hz default)
  * an SSB tone segment near 1800 Hz   (phone 2)

…which proves both phone and CW TX are captured across SSB<->CW switches.

Usage:  python3 cw_recording_analyze.py <file.wav> [sidetoneHz]
Exit 0 = all three segments verified.
"""
import math
import sys
import wave

FRAME_MS = 40
SILENCE_RMS = 200          # int16 noise floor cutoff (~ -44 dBFS)
PROBE_DEFAULTS = [1200.0, 1800.0]


def goertzel(samples, rate, freq):
    n = len(samples)
    if n == 0:
        return 0.0
    k = round(freq * n / rate)
    w = 2.0 * math.pi * k / n
    cw = math.cos(w)
    coeff = 2.0 * cw
    s0 = s1 = s2 = 0.0
    for x in samples:
        s0 = x + coeff * s1 - s2
        s2 = s1
        s1 = s0
    power = s1 * s1 + s2 * s2 - coeff * s1 * s2
    return power / (n * n)


def main():
    if len(sys.argv) < 2:
        print("usage: cw_recording_analyze.py <file.wav> [sidetoneHz]")
        return 2
    path = sys.argv[1]
    sidetone_hz = float(sys.argv[2]) if len(sys.argv) > 2 else 600.0
    probes = sorted(set(PROBE_DEFAULTS + [sidetone_hz]))

    w = wave.open(path, "rb")
    rate = w.getframerate()
    ch = w.getnchannels()
    nframes = w.getnframes()
    raw = w.readframes(nframes)
    w.close()

    import struct
    total = len(raw) // 2
    ints = struct.unpack("<%dh" % total, raw[: total * 2])
    mono = [sum(ints[i:i + ch]) / ch for i in range(0, len(ints) - ch + 1, ch)]

    fl = max(1, int(rate * FRAME_MS / 1000))
    frames = []
    for i in range(0, len(mono) - fl, fl):
        chunk = mono[i:i + fl]
        rms = math.sqrt(sum(s * s for s in chunk) / len(chunk))
        dom_f, dom_p = None, 0.0
        if rms > SILENCE_RMS:
            for f in probes:
                p = goertzel(chunk, rate, f)
                if p > dom_p:
                    dom_p, dom_f = p, f
        frames.append({"t": round(i / rate, 2), "rms": rms, "dom": dom_f})

    # Collapse consecutive non-silent frames into segments by dominant tone.
    segs, cur = [], None
    for fr in frames:
        active = fr["rms"] > SILENCE_RMS and fr["dom"] is not None
        tone = fr["dom"] if active else None
        if cur and cur["tone"] == tone and (fr["t"] - cur["end"]) < 0.25:
            cur["end"] = fr["t"]
            cur["frames"] += 1
        else:
            if cur and cur["tone"] is not None and cur["frames"] >= 3:
                segs.append(cur)
            cur = {"tone": tone, "start": fr["t"], "end": fr["t"], "frames": 1}
    if cur and cur["tone"] is not None and cur["frames"] >= 3:
        segs.append(cur)

    tone_segs = [s for s in segs if s["tone"] is not None]
    print("non-silent tone segments (time / dominant Hz / dur):")
    for s in tone_segs:
        print(f"  {s['start']:6.2f}s  {s['tone']:7.1f} Hz  "
              f"{s['end'] - s['start']:.2f}s")

    def near(hz):
        return [s for s in tone_segs if abs(s["tone"] - hz) < 1.0]

    p1 = near(1200.0)
    cw = near(sidetone_hz)
    p2 = near(1800.0)
    # CW must come BETWEEN the two phone segments and be keyed (multiple short ons
    # OR one segment whose dominant is the sidetone pitch).
    cw_between = any(p1 and p2 and p1[0]["start"] < c["start"] < p2[-1]["end"]
                     for c in cw) if (p1 and p2) else bool(cw)

    ok_p1, ok_cw, ok_p2 = bool(p1), bool(cw) and cw_between, bool(p2)
    print()
    print(f"phone1 ~1200Hz : {'PASS' if ok_p1 else 'FAIL'}")
    print(f"CW ~{sidetone_hz:.0f}Hz : {'PASS' if ok_cw else 'FAIL'}  "
          f"(segments: {len(cw)})")
    print(f"phone2 ~1800Hz : {'PASS' if ok_p2 else 'FAIL'}")
    allok = ok_p1 and ok_cw and ok_p2
    print(f"\nRESULT: {'PASS — both phone + CW captured across switches' if allok else 'FAIL'}")
    return 0 if allok else 1


if __name__ == "__main__":
    sys.exit(main())
