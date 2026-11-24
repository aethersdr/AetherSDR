#!/usr/bin/env python3
# Normalize flex_ingress_characterization_test -vs output: model-visible signals per fed line.
# Usage: AETHER_INGRESS_CORPUS=tests/data/flex_ingress_corpus.txt build/flex_ingress_characterization_test -vs -maxwarnings 0 > raw; tools/flex_ingress_trace.py raw
import re, sys
# Normalize a QtTest -vs trace: keep model-visible signals per fed line.
# RadioModel's statusReceived relay is gone; it stays filtered so a trace
# still diffs cleanly against the stage 0 baseline recorded on main.
drop = re.compile(r'QFileDevice|QSaveFile|QFile\(|QEventDispatcher|QThread\(|QTimer\(|QSocketNotifier|objectNameChanged|destroyed \(|'
                  r'AetherSDR::RadioConnection\(|AetherSDR::WanConnection\(|AetherSDR::FlexStatusIngress\(|statusLineAccepted|RadioModel\([0-9a-f]*\) statusReceived|QAbstractEventDispatcher|deleteLater')
out = []
started = False
with open(sys.argv[1], errors='replace') as trace:
    raw_lines = trace.readlines()
for raw in raw_lines:
    line = raw.rstrip('\n')
    m = re.search(r'LINE (.*)$', line)
    if m:
        started = True
        out.append('== ' + m.group(1)); continue
    for tag in ('WRITE ', 'STATE '):
        if started and tag in line and 'Signal:' not in line:
            out.append('  ' + re.sub(r'\d{4}-\d\d-\d\dZ\d\d:\d\d:\d\d', 'TIME', line[line.index(tag):]))
            break
    else:
        tag = None
    if tag:
        continue
    if not started or 'Signal:' not in line or drop.search(line):
        continue
    sig = line.split('Signal: ', 1)[1]
    sig = re.sub(r'\b(0x)?[0-9a-f]{8,16}\b', 'ADDR', sig)
    sig = re.sub(r'\(([A-Za-z_]+ )?ADDR\)', r'(\1)', sig)
    sig = sig.replace('(QMap<QString,QString>())', '()')
    out.append('  ' + sig)
# Writes are logged from the connection's thread, so they interleave with
# the model's signals; each line's writes are sorted to the end of its block.
blocks, cur = [], None
for l in out:
    if l.startswith('== '):
        cur = [l, [], []]; blocks.append(cur)
    elif cur:
        (cur[2] if l.lstrip().startswith('WRITE ') else cur[1]).append(l)
print('\n'.join('\n'.join([b[0]] + b[1] + sorted(b[2])) for b in blocks))
