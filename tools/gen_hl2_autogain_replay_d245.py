#!/usr/bin/env python3
"""Generate tests/Hl2AutoGainReplayD245.h from the recorded d245 legs.

    python3 tools/gen_hl2_autogain_replay_d245.py           # write the header
    python3 tools/gen_hl2_autogain_replay_d245.py --check   # exit 1 if stale

Standard library only. A per-leg summary goes to stderr.

Inputs, tests/fixtures/hl2-autogain-replay/d245/ (see the README there), per leg
  hl2log-<leg>.json        the app log lines of the leg (JSON lines)
  series-<leg>.json.gz     the 4 Hz health poll

Same construction as tools/gen_hl2_autogain_replay_d168.py, with one addition
the +48 legs need: clips the app log does not name. At the floor (offset 24) a
clip moves nothing, so it writes no attack line; it does reset the loop's clean
time and it records the trip. Those come from the poll series' totO rise,
outside the windows already assigned to an attack, one clip per poll at the
tick the poll closes (poll time minus one tick), with the offset the radio held
then.

  changes  every attack/release line inside the leg: ms after arming, offset after
  clips    (tick, overload, offset the radio held). At an attack: the line's
           "window K / N" at the attack's tick and what is left of the series'
           rise around it at the next tick. Elsewhere: the series' rise.
  blocks   every bandscope block: arrival ms after arming (poll time minus
           blockAgeMs; a new arrival is one more than 0.3 s from the last) and
           the peak code round(2048 / 10^(headroom/20)); 2047 when railN rose;
           0 = no reading.

The log's times are the recording host's wall clock, CET (UTC+1); the series'
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
DATA = REPO / 'tests' / 'fixtures' / 'hl2-autogain-replay' / 'd245'
HEADER_PATH = REPO / 'tests' / 'Hl2AutoGainReplayD245.h'
TZ = datetime.timezone(datetime.timedelta(hours=1))   # CET, 2026-11-12/13
TICK_MS = 123
LEG_MS = 305000
LINE = re.compile(r'HL2 auto RF gain: (attack|release) (-?\d+) dB -> offset (\d+) dB, '
                  r'window (\d+) / (\d+) , headroom "([^"]+)"')
ARMED = re.compile(r'HL2 auto RF gain: ARMED at baseline (\d+) dB')


def tod(s):
    h, m, x = s.split(':')
    return int(h) * 3600 + int(m) * 60 + float(x)


def leg(name):
    with gzip.open(DATA / f'series-{name}.json.gz', 'rt', encoding='utf8') as f:
        s = json.load(f)
    d0 = datetime.datetime.fromtimestamp(s[0]['t'], TZ)
    day0 = s[0]['t'] - (d0.hour * 3600 + d0.minute * 60 + d0.second + d0.microsecond / 1e6)
    with open(DATA / f'hl2log-{name}.json', encoding='utf8') as f:
        lines = [json.loads(x) for x in f if x.strip()]
    arm = None
    for d in lines:
        if ARMED.search(d['msg']):
            arm = day0 + tod(d['t'])
            break
    assert arm is not None, name
    if arm - s[0]['t'] > 43200:      # the log's day is the series' previous day
        arm -= 86400
    elif s[0]['t'] - arm > 43200:
        arm += 86400
    end = min(s[-1]['t'], arm + LEG_MS / 1000.0)
    ev = []
    for d in lines:
        m = LINE.search(d['msg'])
        if m:
            t = day0 + tod(d['t'])
            if t - s[0]['t'] > 43200:
                t -= 86400
            elif s[0]['t'] - t > 43200:
                t += 86400
            if arm - 0.001 <= t <= end:
                ev.append((t, m.group(1), int(m.group(3)), int(m.group(4)), int(m.group(5))))
    dt = TICK_MS / 1000.0
    ticks = int((end - arm) / dt)
    changes, clips, offset, used, dens = [], [], 0, set(), []
    for (t, kind, off, k, n) in ev:
        changes.append((int(round((t - arm) * 1000)), off))
        dens.append(n)
        if kind == 'attack':
            i = max(0, math.ceil((t - arm) / dt - 1e-9) - 1) if t > arm + 1e-3 else 0
            near = [j for j in range(1, len(s)) if t - 0.30 < s[j]['t'] <= t + 0.60]
            tot = sum(s[j]['totO'] - s[j - 1]['totO'] for j in near if j not in used)
            used.update(near)
            clips.append((i, k, offset))
            if tot - k > 0:
                clips.append((i + 1, int(tot - k), offset))
        offset = off

    def radio_offset(ms):
        o = 0
        for c in changes:
            if c[0] <= ms:
                o = c[1]
        return o

    extra, extra_below = 0, 0
    for j in range(1, len(s)):
        if j in used or s[j]['t'] < arm or s[j]['t'] > end:
            continue
        rise = int(s[j]['totO'] - s[j - 1]['totO'])
        if rise <= 0:
            continue
        ms = int(round((s[j]['t'] - arm) * 1000)) - TICK_MS
        i = max(0, ms // TICK_MS)
        at = radio_offset(ms)
        clips.append((i, rise, at))
        extra += rise
        if at < 24:
            extra_below += rise
    clips.sort()
    merged = []
    for c in clips:
        if merged and merged[-1][0] == c[0]:
            merged[-1] = (c[0], merged[-1][1] + c[1], min(merged[-1][2], c[2]))
        else:
            merged.append(c)
    # A block is a new ARRIVAL (poll time minus blockAgeMs more than 0.3 s from
    # the last one), not a new value of the `blocks` counter: the counter can
    # step one poll before the headroom and age it belongs to are published.
    blocks, seen, rail = [], None, None
    for p in s:
        a = p['t'] - p['blockAgeMs'] / 1000.0
        if seen is None or abs(a - seen) > 0.3:
            seen = a
            if a > end:
                continue
            hr = p['headroom']
            railed = rail is not None and (p['railN'] or 0) > rail
            if railed:
                peak = 2047
            elif hr is None:
                peak = 0
            else:
                peak = int(round(2048 / 10 ** (hr / 20)))
            blocks.append((int(round((a - arm) * 1000)), peak))
        rail = p['railN'] or 0
    first = max([i for i, b in enumerate(blocks) if b[0] <= 0] or [0])
    spt = sorted(dens)[len(dens) // 2] if dens else None
    print(f'# {name}: arm {datetime.datetime.fromtimestamp(arm, TZ).strftime("%H:%M:%S.%f")[:-3]}, '
          f'{len(changes)} changes, {len(merged)} clip ticks, clipped obs from the log '
          f'{sum(c[1] for c in clips) - extra}, from the series outside attacks {extra} '
          f'({extra_below} of them below 24 dB), series totO rise '
          f'{s[-1]["totO"] - s[0]["totO"]:.0f}, {len(blocks) - first} blocks, window N median {spt if spt is not None else "n/a"}',
          file=sys.stderr)
    return ticks, changes, merged, blocks[first:], spt, dens


def rows(items, fmt, per):
    cells = [fmt(*it) for it in items]
    return ['    ' + ' '.join(cells[i:i + per]) for i in range(0, len(cells), per)]


def emit(tag, ticks, changes, clips, blocks):
    out = [f'inline constexpr int k{tag}Ticks = {ticks};',
           f'inline constexpr Change k{tag}Changes[] = {{']
    out += rows(changes, lambda ms, off: f'{{{ms}, {off}}},', 6) or ['    {-1, 0},']
    out += ['};', f'inline constexpr Clip k{tag}Clips[] = {{']
    out += rows(clips, lambda i, k, o: f'{{{i}, {k}, {o}}},', 6) or ['    {-1, 0, 0},']
    out += ['};', f'inline constexpr Block k{tag}Blocks[] = {{']
    out += rows(blocks, lambda ms, peak: f'{{{ms}, {peak}}},', 6)
    out.append('};')
    return '\n'.join(out)


LEGS = [('R1L40', 'R1-L40-P48'), ('R1L30', 'R1-L30-P48'), ('R1L80', 'R1-L80-P48'),
        ('R2L40', 'R2-L40-P48'), ('R2L30', 'R2-L30-P48'), ('R2L80', 'R2-L80-P48'),
        ('R1L80p20', 'R1-L80-P20'), ('Dum40', 'DUM40-P48')]

HEADER = '''#pragma once

// HL2 auto RF gain inputs recorded on-air, receive only, 2026-11-12/13
// 23:02-00:26 CET, AetherSDR main 50ed5fff2 (lab run d245): the
// bandscope law (floor 24 dB) armed at LNA +48 dB for 305 s on 40 / 30 / 80 m
// in two rounds, at +20 dB on 80 m (round 1), and at +48 dB on the dummy load.
// Changes are the app log's attack/release lines; clip counts are those lines'
// windows plus the poll series' overload rise elsewhere (at the floor a clip
// logs nothing); blocks are the bandscope peaks from the 4 Hz health poll.
// Generated from tests/fixtures/hl2-autogain-replay/d245/ by
// tools/gen_hl2_autogain_replay_d245.py (--check compares); do not hand-edit.

namespace d245 {

inline constexpr int kTickMs = 123;          // one policy step per telemetry publish
inline constexpr int kSamplesPerTick = %d;    // overload-bit observations per step

struct Change { int ms; int offsetDb; };              // ms after arming, offset after
struct Clip   { int tick; int overload; int atOffsetDb; };  // offset the radio held
struct Block  { int arrivalMs; int peakCode; };       // 0 = no reading
'''


# The kSamplesPerTick the committed header was generated with.
EXPECTED_SAMPLES_PER_TICK = 19


def samples_per_tick(legs):
    # The log's "window K / N" alternates between 19 and 20 observations per
    # 123 ms window, so the per-leg medians land on 19 on some legs and 20 on
    # others. The replay steps every leg with one constant, so it takes the
    # median over every logged window of every leg; the first assert keeps that
    # one value inside the per-leg medians' range.
    #
    # The pooled counts are close to a tie (19 x 50 against 20 x 47 in these
    # captures), and a flip to 20 still sits inside that range. The second
    # assert pins the value the committed header was built with, so a capture
    # that tips the median stops here with the counts, and choosing 19 or 20
    # becomes a decision for that change rather than a silent retiming.
    pooled = sorted(n for r in legs for n in r[5])
    assert pooled, 'no attack/release line in any leg'
    spt = pooled[len(pooled) // 2]
    medians = [r[4] for r in legs if r[4] is not None]
    assert min(medians) <= spt <= max(medians), (spt, medians)
    counts = {n: pooled.count(n) for n in sorted(set(pooled))}
    assert spt == EXPECTED_SAMPLES_PER_TICK, (
        f'pooled median moved to {spt} (counts {counts}); decide 19 or 20 and '
        f'update EXPECTED_SAMPLES_PER_TICK')
    print(f'# kSamplesPerTick {spt}: median of {len(pooled)} logged windows, '
          f'per-leg medians {sorted(set(medians))}', file=sys.stderr)
    return spt


def generate():
    legs = [leg(name) for _, name in LEGS]
    out = [emit(tag, *r[:4]) for (tag, _), r in zip(LEGS, legs)]
    return '\n'.join([HEADER % samples_per_tick(legs), '\n\n'.join(out),
                      '\n}  // namespace d245']) + '\n'


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--check', action='store_true',
                    help='compare with the committed header instead of writing it')
    args = ap.parse_args()
    text = generate()
    rel = HEADER_PATH.relative_to(REPO)
    if args.check:
        if HEADER_PATH.read_text(encoding='utf8') != text:
            print(f'::error::{rel} differs from what tools/gen_hl2_autogain_replay_d245.py generates')
            return 1
        print(f'{rel} matches its generator')
        return 0
    HEADER_PATH.write_text(text, encoding='utf8', newline='\n')
    print(f'wrote {rel}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
