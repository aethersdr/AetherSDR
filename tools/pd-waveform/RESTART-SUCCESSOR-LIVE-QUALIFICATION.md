# Installed restart and failure qualification

The signed successor from `e0563d1e285d36bba21da3f791364dd27642e26a`
passed four receive-only process-failure cases on the FLEX-8600. Installation
through the ordinary desktop workflow committed in 51.794 seconds and removed
its temporary installer. The existing diagnostics bridge was used only for
fault injection and independent host inspection.

| Injected failure | Workers retired | Explicit GUI Start ready |
| --- | ---: | ---: |
| DSP process killed | 1.560 s | 1.751 s |
| Private controller killed | 0.454 s | 1.758 s |
| Both ARP cleanup processes killed | 1.856 s | 1.756 s |
| IQ broker killed | 1.488 s | 1.775 s |

No RF or automatic restart occurred. Each new session required an explicit
Start; no extra Stop was needed to clear terminal status. Losing both ARP
owners retained the exact entry and ownership journal. Explicit restart
reclaimed it; subsequent teardown removed it and cleared the journal.

The final pre-reboot audit verified the selected generation and every runtime
payload hash, unchanged identity, no DSP/IQ workers or setup sockets, no ARP
entry, `accept_local=0`, and no temporary installer container/image or units.
Three retained control processes ran as UID 78666 with all capabilities zero
and `NoNewPrivs=1`. The normal stopped status was confirmed in 1.548 seconds.
This is not a single-process idle baseline.

The next single commanded reboot did **not** qualify: both services went down,
and the exact committed installation did not return within the observation
window (191.836 seconds total). The API, shim and SSH were unreachable and the
local ARP entry was incomplete. No second reboot was requested. This does not
identify whether the cause is boot, radio firmware, AetherPD or networking;
physical state and restored connectivity are needed before continuing live
qualification. Identity persistence after this reboot is not established.

A local syntax error in the first cleanup-audit script was rejected before
radio execution; the corrected audit passed. That failed observation and the
reboot timeout are retained. The DSP case did not collect the extra original
PID/zombie scan used by the other three cases. Capture teardown still records
one downstream output error/loss marker; no loss-free scheduling claim is made.

Earlier matched 5 W/10 W setting RF evidence belongs to the previous candidate.
The DSP binary is unchanged, but that does not qualify the entire successor for
RF or reboot. Keyed failure/unkey tests, partial-startup/API/credential cases,
alignment/cold-start diagnosis, sustained RF and stock APD comparison, actual
FLEX-6300 hardware and independent release gates remain. The full P1–P10 goal
is active; this is a development checkpoint, not a production-ready release.

[Structured report and artifact hashes](RESTART-SUCCESSOR-LIVE-QUALIFICATION.json)
retain exact timings and observations. The earlier defect and component tests
remain in [the original failure report](INSTALLED-FAILURE-QUALIFICATION.json).
