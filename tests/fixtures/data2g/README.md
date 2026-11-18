# Data2G replay recording

`session-0.4.3.wav` is 23 s of 8 kHz, 16-bit mono audio: what a third station
would hear while two Data2G stations work each other. `data2g_replay_test`
plays it through `Data2gMonitor`.

It was made by two upstream `data2g-host` 0.4.3 processes, N0CALL and W1AW,
connected to each other through named pipes. No radio or sound card was
involved and nothing was transmitted. It is **not** an off-air recording: the
channel is perfect, with no noise, fading, tuning offset or receiver filter.
The test adds level changes and white noise itself.

What is in it, in order:

1. a KISS broadcast from N0CALL (an AX.25 UI frame, `!beacon`)
2. N0CALL connects to W1AW
3. one line of text each way, in 256-QAM data bursts
4. N0CALL disconnects
5. both stations send their ID frame

`session-0.4.3.monitor.txt` is upstream's own `data2g-monitor` 0.4.3 reading
of the recording. The expectations in `tests/data2g_replay_test.cpp` are taken
from it, so the test holds AetherSDR's adapter to the reference receiver.

## What it is for

- The other Data2G tests transmit with the vendored core inside the test. This
  recording comes from the upstream host application, so it covers the frames
  of a connected session, which those tests do not produce.
- When the vendored core is moved to a newer upstream version, this recording
  shows whether the new core still reads what 0.4.3 put on the air. Upstream
  says the protocol may change, so a failure here after a re-pin may be
  expected: record a new session with the new version and keep or drop this
  one deliberately.

## Recording a new one

Build upstream's `data2g-host` and `data2g-monitor` at the version wanted, then:

```sh
tests/fixtures/data2g/record_session.py /path/to/data2g-host /tmp/take
```

Each take has its own session nonce and key. Rewrite the table in
`tests/data2g_replay_test.cpp` from `data2g-monitor`'s reading of the new file
(it takes raw float32 at 8 kHz with `--input-file`).

The call signs are placeholders: N0CALL is the conventional "no call sign"
value, and W1AW is the one the other Data2G tests already use.
