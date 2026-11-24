#!/usr/bin/env python3
"""Generate tests/Hl2AutoGainReplayD168.h from the recorded d168 legs.

    python3 tools/gen_hl2_autogain_replay_d168.py           # write the header
    python3 tools/gen_hl2_autogain_replay_d168.py --check   # exit 1 if stale

Standard library only. A per-leg summary goes to stderr.

Inputs, tests/fixtures/hl2-autogain-replay/d168/ (see the README there):
  series-L80-B1.json.gz, series-L30-B1.json.gz   the 4 Hz health poll
  hl2-auto-rf-gain.txt                           the app log's auto-gain lines

What it writes, per leg:
  changes  every "attack"/"release" line of the app log inside the leg: ms after
           arming and the offset after the change.
  clips    the clipped observations per 123 ms policy tick. The count at each
           attack is the app log's "window K / N"; what is left of the series'
           totO rise around that attack goes into the next tick (it cannot be
           earlier: the loop would have attacked sooner; nor later: it would
           have attacked again). Each clip carries the offset the radio held
           BEFORE that attack, for both ticks: the second tick is the window in
           flight when the gain write went out.
  blocks   every bandscope block in the series: arrival in ms after arming
           (poll time minus blockAgeMs) and the peak code recomputed from the
           two-decimal headroom, round(2048 / 10^(headroom/20)). 0 = no reading.

The log's times are the recording host's wall clock, CEST (UTC+2); the series'
are Unix time. The offset is fixed here so the output does not depend on the
time zone of the machine that runs this.
"""
import argparse
import datetime
import gzip
import json
import math
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
DATA = REPO / 'tests' / 'fixtures' / 'hl2-autogain-replay' / 'd168'
LOG = DATA / 'hl2-auto-rf-gain.txt'
HEADER_PATH = REPO / 'tests' / 'Hl2AutoGainReplayD168.h'
TZ = datetime.timezone(datetime.timedelta(hours=2))   # CEST, 2026-10-01
TICK_MS = 123
LINE = re.compile(r'\[(\d\d:\d\d:\d\d\.\d+)\] DBG aether\.hl2: HL2 auto RF gain: (attack|release) (-?\d+) dB '
                  r'-> offset (\d+) dB, window (\d+) / (\d+) , headroom "([^"]+)"')


def tod(s):
    h, m, x = s.split(':')
    return int(h) * 3600 + int(m) * 60 + float(x)


def leg(name, arm_s, end_s):
    with gzip.open(DATA / f'series-{name}.json.gz', 'rt', encoding='utf8') as f:
        s = json.load(f)
    d0 = datetime.datetime.fromtimestamp(s[0]['t'], TZ)
    day0 = s[0]['t'] - (d0.hour * 3600 + d0.minute * 60 + d0.second + d0.microsecond / 1e6)
    arm = day0 + tod(arm_s)
    end = min(s[-1]['t'], day0 + tod(end_s))
    ev = []
    with open(LOG, encoding='utf8', errors='replace') as f:
        for line in f:
            m = LINE.match(line)
            if m:
                t = day0 + tod(m.group(1))
                if arm - 0.001 <= t <= end:
                    ev.append((t, m.group(2), int(m.group(4)), int(m.group(5))))
    dt = TICK_MS / 1000.0
    ticks = int((end - arm) / dt)
    changes, clips, offset = [], [], 0
    for (t, kind, off, k) in ev:
        changes.append((int(round((t - arm) * 1000)), off))
        if kind == 'attack':
            i = max(0, math.ceil((t - arm) / dt - 1e-9) - 1) if t > arm + 1e-3 else 0
            tot = sum(b['totO'] - a['totO'] for a, b in zip(s, s[1:]) if t - 0.30 < b['t'] <= t + 0.60)
            if t <= arm + 1e-3:
                tot = k + (s[1]['totO'] - s[0]['totO'])
            clips.append((i, k, offset))
            if tot - k > 0:
                clips.append((i + 1, int(tot - k), offset))
        offset = off
    assigned = sum(c[1] for c in clips)
    rise = s[-1]['totO'] - s[0]['totO']
    print(f'# {name}: {len(changes)} changes, clipped obs assigned {assigned}, series totO rise {rise:.0f}',
          file=sys.stderr)
    blocks, seen = [], None
    for p in s:
        if p['blocks'] != seen:
            seen = p['blocks']
            a = p['t'] - p['blockAgeMs'] / 1000.0
            if a > end:
                continue
            hr = p['headroom']
            if (p['railN'] or 0) > 0:
                peak = 2047
            elif hr is None:
                peak = 0
            else:
                peak = int(round(2048 / 10 ** (hr / 20)))
            blocks.append((int(round((a - arm) * 1000)), peak))
    # Keep the last block before arming (it is the reading at the first ticks).
    first = max([i for i, b in enumerate(blocks) if b[0] <= 0] or [0])
    return ticks, changes, clips, blocks[first:]


def rows(items, fmt, per):
    cells = [fmt(*it) for it in items]
    return ['    ' + ' '.join(cells[i:i + per]) for i in range(0, len(cells), per)]


def emit(tag, ticks, changes, clips, blocks):
    out = [f'inline constexpr int k{tag}Ticks = {ticks};',
           f'inline constexpr Change k{tag}Changes[] = {{']
    out += rows(changes, lambda ms, off: f'{{{ms}, {off}}},', 6)
    out += ['};', f'inline constexpr Clip k{tag}Clips[] = {{']
    out += rows(clips, lambda i, k, o: f'{{{i}, {k}, {o}}},', 6)
    out += ['};', f'inline constexpr Block k{tag}Blocks[] = {{']
    out += rows(blocks, lambda ms, peak: f'{{{ms}, {peak}}},', 6)
    out.append('};')
    return '\n'.join(out)


HEADER = '''#pragma once

// HL2 auto RF gain inputs recorded on-air, receive only, 2026-10-01 22:32-22:50
// CEST: bandscope law armed at LNA +20 dB for 305 s on 80 m and on 30 m.
// Changes and clip counts are the app log's attack/release lines; blocks are
// the bandscope peaks from the 4 Hz health poll. Generated from
// tests/fixtures/hl2-autogain-replay/d168/ by
// tools/gen_hl2_autogain_replay_d168.py (--check compares); do not hand-edit.

namespace d168 {

inline constexpr int kTickMs = 123;          // one policy step per telemetry publish
inline constexpr int kSamplesPerTick = 152;  // overload-bit observations per step

struct Change { int ms; int offsetDb; };              // ms after arming, offset after
struct Clip   { int tick; int overload; int atOffsetDb; };  // offset the radio held
struct Block  { int arrivalMs; int peakCode; };       // 0 = no reading
'''


def generate():
    # Arm times are the log's "ARMED at baseline 20 dB" lines; the ends are
    # where the recording runner closed each 305 s leg.
    return '\n'.join([
        HEADER,
        emit('80m', *leg('L80-B1', '22:44:46.790', '22:49:51.5')),
        '',
        emit('30m', *leg('L30-B1', '22:32:37.278', '22:37:42.2')),
        '\n}  // namespace d168',
    ]) + '\n'


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--check', action='store_true',
                    help='compare with the committed header instead of writing it')
    args = ap.parse_args()
    text = generate()
    rel = HEADER_PATH.relative_to(REPO)
    if args.check:
        if HEADER_PATH.read_text(encoding='utf8') != text:
            print(f'::error::{rel} differs from what tools/gen_hl2_autogain_replay_d168.py generates')
            return 1
        print(f'{rel} matches its generator')
        return 0
    HEADER_PATH.write_text(text, encoding='utf8', newline='\n')
    print(f'wrote {rel}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
