# Flex TUNE RCA output suppression

Protocol evidence for #6521, following #6437. The implementation uses public
Flex material and observed TCP API behavior; no proprietary binary was inspected.

## Sources and exact spelling

[FlexRadio employee Mike VA3MW's confirmation](https://community.flexradio.com/discussion/8034395/smartsdr-4-3-16-suppression-of-ptt-during-tune)
describes suppressing TX1, TX2 and TX3 RCA keying outputs during TUNE. It gives
the key as `tx_tune_suppress`. The published FlexLib 4.3.16 source contains
neither that key nor `tune_tx_suppress`.

Serialized, no-RF TCP probes on a **FLEX-6700 running 4.3.16.42034** established
the following wire behavior:

| Request | Reply and readback |
|---|---|
| `interlock tx_tune_suppress=1` | Error `0x5000002D`; no change |
| `interlock bandset 5 tx_tune_suppress=1` | Success reply, but no change |
| `interlock tune_tx_suppress=1` | Success; only active band 9 (20 m) changed |
| `interlock bandset 5 tune_tx_suppress=1` | Success; band 5 (40 m) changed independently |

The general command is **active-band scoped**, rather than a request to set
every band. The TX Band Settings column uses `interlock bandset <id> tune_tx_suppress=<0|1>`
with each reported row ID, rather than inferring IDs from client frequency ranges.
Successful replies do not prove adoption: the incorrect bandset key also
returns success. Only a radio status observation confirms the displayed value.

Readback uses:

```text
interlock band 9 band_name=20 ... tune_tx_suppress=1
interlock ... tune_tx_suppress=1
```

The first form identifies a band's saved value. The general form follows the
active band's value and carries no band identity, so it cannot populate a row.
Each row uses only its matching per-band observation. Band IDs are not
contiguous: the observed table includes 24 (GEN), 25 (2200 m) and 26 (630 m).
A `sub tx all` refresh returns current general and per-band readback. No
single Transmit-page checkbox or client-side copy of this setting is used.

Both edited band values survived closing the GUI API session and reconnecting
with its GUI client ID. This establishes radio ownership across client sessions;
no power-cycle persistence test was performed. The probes captured all 16
band values, changed only 20 m and 40 m, restored both to their captured values,
and verified all 16 band values afterward. No TUNE, MOX, ATU or other keying
command was sent. Physical RCA-output behavior was not measured.

## Client ownership and compatibility

On fully parsed SmartSDR 4.3-or-later firmware, the radio owns RCA suppression
and restoration, whether the checkbox is checked or unchecked. The legacy
client sequence must not independently toggle TX1/TX2/TX3 enable registers.
The separate ACC TX preference keeps its existing behavior. Firmware 4.2 and
older, and unknown or malformed versions, retain the legacy path. The new
checkbox is unavailable on unqualified firmware.

RCA ownership is captured when a TUNE cycle applies inhibition. If firmware
identification changes during that cycle, cleanup retains the captured ownership;
the next cycle uses the new capability. This prevents late identification from
skipping restoration of RCA outputs the legacy path temporarily disabled.

The TX subscription can report band values before the `info` reply identifies
the firmware, especially after reconnect or a manual-IP connection. Validated
per-band observations are retained for that session, but controls and writes
remain unavailable until the firmware qualifies. The identity notification then
updates an open table without another radio event. Disconnect clears these
observations, including those received before firmware qualification.

The radio's per-band value is not stored in AppSettings and is not initialized
from the legacy global RCA preferences. Those preferences are retained for older
firmware. While firmware owns RCA suppression, the submenu contains ACC TX
and a Configure RCA suppression shortcut to TX Band Settings. The legacy RCA
actions are detached without clearing their saved values; they return for older
firmware. No automatic migration or upgrade notice is shown. TX REQ safety
inputs, keying authorization and other amplifier-keying mechanisms are separate
from this setting.

Injected production-path regressions cover encoding, parsing, ownership,
readback, rejection, missing confirmation and session replacement. These checks
do not establish physical RF or RCA-output behavior.
