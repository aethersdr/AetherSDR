#!/usr/bin/env python3
# Normalize flex_ingress_characterization_test -vs output: model-visible signals per fed line.
# Usage: AETHER_INGRESS_CORPUS=tests/data/flex_ingress_corpus.txt build/flex_ingress_characterization_test -vs > raw; tools/flex_ingress_trace.py raw
import re, sys
# Normalize a QtTest -vs trace: keep model-visible signals per fed line.
# RadioModel's statusReceived relay is gone; it stays filtered so a trace
# still diffs cleanly against the stage 0 baseline recorded on main.
drop = re.compile(r'QFileDevice|QSaveFile|QFile\(|QEventDispatcher|QThread\(|QTimer\(|QSocketNotifier|objectNameChanged|destroyed \(|'
                  r'AetherSDR::RadioConnection\(|AetherSDR::WanConnection\(|AetherSDR::FlexStatusIngress\(|statusLineAccepted|RadioModel\([0-9a-f]*\) statusReceived|QAbstractEventDispatcher|deleteLater')
out = []
started = False
for raw in open(sys.argv[1], errors='replace'):
    line = raw.rstrip('\n')
    m = re.search(r'LINE (.*)$', line)
    if m:
        started = True
        out.append('== ' + m.group(1)); continue
    if not started or 'Signal:' not in line or drop.search(line):
        continue
    sig = line.split('Signal: ', 1)[1]
    sig = re.sub(r'\b(0x)?[0-9a-f]{8,16}\b', 'ADDR', sig)
    sig = re.sub(r'\(([A-Za-z_]+ )?ADDR\)', r'(\1)', sig)
    sig = sig.replace('(QMap<QString,QString>())', '()')
    out.append('  ' + sig)
print('\n'.join(out))
