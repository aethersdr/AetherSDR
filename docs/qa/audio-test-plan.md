# AetherSDR Audio Pipeline Test Plan

**Version:** v0.8.15.1
**Last updated:** 2026-04-15
**Purpose:** Comprehensive validation of all audio paths — RX, TX, TCI, DAX, DSP, and recovery mechanisms.

---

## Prerequisites

- FLEX radio connected, at least one slice active on a band with signals
- Headphones or speakers with stereo capability (for pan testing)
- WSJT-X or JTDX installed (for TCI testing)
- Protocol logging enabled (Support & Diagnostics → Commands/Status)
- A second slice available (for multi-slice tests)

---

## A. Basic RX Audio (Radio → Speaker)

### A1. Audio plays on connect
1. Launch AetherSDR, connect to radio
2. Tune to an active frequency (e.g., WWV 10 MHz, or a busy band)
3. Audio should play immediately — no silence, no stuttering

### A2. Volume slider full range
1. Drag master volume slider from 0 to 100
2. Volume should scale smoothly from silence to full
3. No clicks, pops, or distortion at any position

### A3. Mute/unmute
1. Click speaker icon to mute
2. Audio stops immediately
3. Click again to unmute
4. Audio resumes immediately — no restart delay

### A4. AF gain per-slice
1. Adjust the AF gain slider in the applet panel
2. Volume should change for the active slice only
3. Smooth transition, no clicks

### A5. Audio survives idle (15+ minutes)
1. Leave AetherSDR connected and idle for 15+ minutes
2. Return — audio should still be playing
3. Check log for "no audio data received" warnings — should NOT appear during normal operation
4. If liveness watchdog fires, note the circumstances

### A6. Audio survives screensaver/sleep wake
1. Let the system go to screensaver or sleep
2. Wake the system
3. Audio should resume within a few seconds
4. Check log for restart messages — one restart is acceptable, repeated restarts are a bug

---

## B. Audio Format Integrity

### B1. No buffer cap drops under normal load
1. Connect, listen to audio for 2 minutes
2. Open Support & Diagnostics → Network tab
3. Check RX buffer stats — underrun count should be near zero
4. Peak buffer size should be well under the 200ms cap

### B2. 48kHz output device
1. If your system uses a 48kHz output device (most do)
2. Audio should play without pitch shift or speed change
3. Verify in log: "RX stream started at 48000 Hz" or "24000 Hz"

### B3. Bandwidth-reduced audio (PCC 0x0123)
1. Narrow the slice bandwidth significantly (e.g., CW narrow filter)
2. Audio should still play — no silence from reduced-bandwidth packets
3. The int16 mono → float32 stereo conversion should be transparent

---

## C. DSP Paths

### C1. No DSP baseline
1. Disable all noise reduction (NR2, NR4, BNR, DFNR all off)
2. Audio should play cleanly — this is the reference

### C2. RN2 (RNNoise)
1. Enable RN2
2. Audio should play with noise reduction applied
3. **Pan test**: set pan control fully left, then fully right
4. Audio should pan correctly (RN2 is true stereo)

### C3. NR2 (Spectral)
1. Enable NR2
2. Audio should play with noise reduction applied
3. **Pan test**: set pan fully left, then fully right
4. **KNOWN BUG**: pan will be centered regardless of setting (#1460)
5. Note: this is expected until #1461 is merged

### C4. NR4 (Specbleach)
1. Enable NR4
2. Audio should play with noise reduction applied
3. **Pan test**: same as C3 — **KNOWN BUG**: pan lost (#1460)

### C5. BNR (NVIDIA)
1. Enable BNR (requires NVIDIA BNR service running)
2. Audio should play after ~50ms priming delay
3. **Pan test**: same as C3 — **KNOWN BUG**: pan lost (#1460)
4. Note the ~50ms onset delay — this is expected

### C6. DFNR (DeepFilter)
1. Enable DFNR
2. Audio should play with noise reduction applied
3. **Pan test**: set pan fully left, then fully right
4. Pan should work correctly (DFNR is true stereo)

### C7. DSP toggle rapid switching
1. Rapidly toggle NR2 on/off 5 times
2. Audio should not cut out, stutter, or require restart
3. Repeat with NR4, BNR, DFNR

### C8. DSP during TX
1. Enable NR2 (or any DSP)
2. Key TX (PTT or MOX)
3. DSP should bypass during TX (no processing of silence/sidetone)
4. Unkey — DSP should resume immediately

---

## D. TCI Audio (WSJT-X / JTDX)

### D1. TCI audio starts on audio_start
1. Launch WSJT-X configured for TCI (localhost:50001)
2. WSJT-X should receive audio and show waterfall activity
3. Check log for "TCI: creating DAX RX stream" and "TCI: registered DAX RX stream"

### D2. TCI audio format negotiation
1. WSJT-X typically requests int16 48kHz
2. Audio should decode properly — FT8/FT4 decodes appearing
3. No silence, no garbled decodes

### D3. TCI survives PC audio mute
1. While WSJT-X is decoding via TCI
2. Mute PC audio (speaker icon)
3. WSJT-X should continue decoding — TCI audio is independent of PC speaker

### D4. TCI client disconnect/reconnect
1. Close WSJT-X
2. Check log for "TCI: removed DAX RX stream" and "TCI: releasing DAX channel"
3. Reopen WSJT-X — TCI should reconnect and audio should flow again

### D5. TCI with DAX already running
1. Enable DAX in the DAX Audio tile (Autostart DAX)
2. Then connect WSJT-X via TCI
3. Both should work simultaneously
4. Disconnect WSJT-X — user's DAX should remain active

### D6. TCI control-only (no audio)
1. Connect a TCI client that only sends control commands (e.g., StreamDeck)
2. No `audio_start` sent
3. Check log — no DAX RX streams should be created
4. No impact on PC audio

---

## E. Multi-Slice Audio

### E1. Switch active slice
1. Create two slices on different bands
2. Switch between them by clicking slice tabs
3. Audio should switch to the new active slice immediately

### E2. TX Follows Active / Active Follows TX
1. Enable "Active Slice Follows TX" in Radio Setup → TX
2. Have WSJT-X move TX to slice B
3. Applet panel should switch to slice B
4. Disable the toggle — behavior should stop

### E3. Two slices simultaneous audio
1. Create two slices
2. Both should contribute audio to the speaker (mixed)
3. Mute one slice — only the other should play

---

## F. TX Audio (Mic → Radio)

### F1. Voice TX
1. Select a voice mode (SSB)
2. Key PTT, speak into mic
3. Monitor on the radio or a second receiver — voice should be clean
4. No distortion, no clipping, correct sideband

### F2. TX audio level
1. Adjust mic gain in Radio Setup → Audio
2. ALC meter should respond proportionally
3. No hard clipping at high gain

### F3. DAX TX (digital modes)
1. Configure WSJT-X for TCI TX
2. Initiate a TX cycle (FT8 call)
3. TX audio should flow from WSJT-X → TCI → radio
4. Monitor — should hear clean FT8 tones, correct timing

---

## G. Digital Modes — DAX, DAX IQ, TCI Audio

### DAX RX Audio

#### G1. DAX RX via DAX Audio tile (Autostart DAX)
1. Open the DAX Audio tile (click `DAX` in the applet tray), check "Autostart DAX"
2. Check log for `stream create type=dax_rx dax_channel=1`
3. Open WSJT-X configured to use DAX audio device (not TCI)
4. WSJT-X should show waterfall activity and produce FT8 decodes
5. Disable Autostart DAX — streams should be removed, WSJT-X loses audio

#### G2. DAX RX channel assignment
1. With two slices active, assign DAX channel 1 to slice A, channel 2 to slice B
2. WSJT-X on DAX channel 1 should decode slice A's frequency
3. Switch DAX assignment — verify WSJT-X follows the correct slice

#### G3. DAX RX survives slice mode change
1. DAX running on slice A in USB mode, WSJT-X decoding
2. Change slice A to CW, then back to USB
3. DAX audio should resume without manual intervention

#### G4. DAX RX audio format
1. DAX delivers float32 stereo 24kHz to the virtual audio bridge
2. Verify in an external recorder (Audacity) — no pitch shift, correct sample rate
3. Record 10 seconds, inspect: should be clean, no clicks or gaps

### DAX TX Audio

#### G5. DAX TX via WSJT-X
1. Configure WSJT-X for DAX TX (audio routed through DAX virtual device)
2. Initiate FT8 TX cycle
3. Monitor on the radio or second receiver — clean FT8 tones, correct timing
4. Check that `transmit set dax=1` was sent (log)

#### G6. DAX TX does not conflict with mic
1. DAX TX active (WSJT-X transmitting FT8)
2. DAX TX should route digital audio, not mic input
3. After WSJT-X TX cycle ends, switch to SSB and key PTT
4. Mic audio should work normally — DAX TX does not permanently steal the TX path

#### G7. DAX TX with SmartSDR DAX coexistence (Windows)
1. On Windows with SmartSDR DAX 4.1.5 installed
2. Launch AetherSDR — should not lock up SmartSDR DAX TX
3. Close AetherSDR — SmartSDR DAX should recover without restart
4. This was the #1394 bug — verify streams are cleaned up on disconnect

### DAX IQ Streaming

#### G8. DAX IQ stream creation
1. Enable DAX IQ on a slice (DAX IQ tile → IQ channel selector)
2. Check log for `stream create type=dax_iq dax_channel=<ch>`
3. An IQ-capable application (e.g., SDR++) should receive IQ data

#### G9. DAX IQ sample rates
1. Set DAX IQ to 24kHz, 48kHz, 96kHz, 192kHz in sequence
2. Each rate change should create a new stream at the requested rate
3. Receiving application should show correct bandwidth for each rate

#### G10. DAX IQ cleanup on disconnect
1. DAX IQ stream running
2. Disconnect from radio
3. Reconnect — no orphaned IQ streams, clean state

### TCI Audio — Format Negotiation

#### G11. TCI float32 stereo (default)
1. Connect a TCI client that requests float32 stereo (default negotiation)
2. Audio should stream correctly
3. Verify with the test script (`/tmp/tci-audio-test.py`): frames should be 64-byte header + float32 payload

#### G12. TCI int16 stereo
1. Connect a TCI client that sends `audio_stream_sample_type:int16;`
2. Audio should stream as int16 — no silence, no garbled data
3. WSJT-X/JTDX typically request this — verify FT8 decodes work

#### G13. TCI int16 mono
1. Connect a TCI client that sends `audio_stream_sample_type:int16;` and `audio_stream_channels:1;`
2. Audio should be mono int16 — half the payload size of stereo
3. Verify decodes still work

#### G14. TCI float32 mono
1. Connect a TCI client that sends `audio_stream_channels:1;` (format stays float32)
2. Audio should be mono float32 — L+R averaged to single channel

#### G15. TCI sample rate negotiation
1. Connect a client that sends `audio_samplerate:48000;`
2. TCI server should resample 24kHz → 48kHz before sending
3. Verify correct pitch and timing (no chipmunk effect)
4. Repeat with `audio_samplerate:12000;` and `audio_samplerate:8000;`

### TCI Audio — Lifecycle

#### G16. TCI audio_start creates DAX RX streams
1. DAX Autostart OFF in the DAX Audio tile
2. Connect TCI client, send `audio_start;`
3. Check log for:
   - `slice set <id> dax=<ch>` (channel assignment)
   - `stream create type=dax_rx dax_channel=<ch>` (stream creation)
   - `TCI: registered DAX RX stream` (VITA-49 routing)
4. Audio frames should arrive at the TCI client

#### G17. TCI audio_stop tears down DAX RX streams
1. Send `audio_stop;` (or disconnect the TCI client)
2. Check log for:
   - `stream remove 0x<streamId>` (stream teardown)
   - `TCI: releasing DAX channel from slice` (channel release)
3. No orphaned DAX streams

#### G18. TCI control-only does not create DAX
1. Connect a TCI client, send only control commands (no `audio_start`)
2. Check log — no `stream create type=dax_rx` should appear
3. No DAX channel assignments made

#### G19. TCI multiple clients
1. Connect two TCI clients simultaneously
2. Both send `audio_start;`
3. Both should receive audio
4. Disconnect one — the other should continue receiving
5. Disconnect the last — DAX streams torn down

#### G20. TCI audio independent of PC speaker
1. TCI client connected and receiving audio
2. Mute PC audio (speaker icon)
3. TCI audio continues — FT8 decodes unaffected
4. Disable PC Audio entirely in settings
5. TCI audio should still work (feeds from DAX, not PC audio path)

### Cross-Path Interactions

#### G21. DAX bridge + TCI simultaneous
1. Enable DAX Autostart in the DAX Audio tile (creates DAX bridge streams)
2. Connect WSJT-X via TCI (creates TCI's own DAX streams or piggybacks)
3. Both should receive audio simultaneously
4. Disable DAX Autostart — TCI should continue working (owns its own streams)
5. Disconnect TCI — DAX bridge should be unaffected

#### G22. TCI TX + DAX TX mutual exclusion
1. WSJT-X connected via TCI, transmitting FT8
2. Simultaneously attempt DAX TX from another app
3. Only one TX path should be active — radio enforces this
4. Verify no audio corruption or lockup

#### G23. Mode change during active digital session
1. WSJT-X decoding via TCI on slice A (DIGU mode)
2. Change slice A to USB
3. TCI audio should continue flowing (DAX is mode-independent)
4. Change back to DIGU — decodes should resume

---

## H. Recovery Mechanisms

### H1. USB audio device disconnect
1. If using USB audio, unplug the device during playback
2. AetherSDR should detect the change and attempt restart
3. Plug device back in — audio should resume
4. Check log for "audio output device list changed, restarting RX"

### H2. No false restarts during normal operation
1. Listen to audio for 10 minutes with no interaction
2. Check log — should see zero "restarting RX" messages
3. Any restart during normal operation is a bug

### H3. Zombie watchdog does not trigger falsely
1. Listen to audio for 5 minutes
2. Check log for "sink appears zombie" — should not appear
3. If it does, note the audio device and OS

### H4. Liveness watchdog does not trigger during TX
1. Key TX for 20+ seconds (CW or voice)
2. Unkey
3. Audio should resume immediately
4. Check log — liveness watchdog should NOT fire (TX suppresses audio feed but watchdog should not restart during this gap)

---

## I. QSO Recorder

### I1. Record and playback
1. Enable QSO recorder
2. Listen to a signal for 10 seconds
3. Stop recording, play back
4. Playback should sound identical to live audio — no pitch shift, no noise

### I2. Recording format
1. Find the recorded WAV file
2. Verify: 24kHz, stereo, 16-bit int (WAV header)
3. Play in an external player — should sound correct

---

## J. CW Decoder

### J1. CW decode from audio tap
1. Tune to a CW signal, switch to CW mode
2. CW decode panel should appear and show decoded text
3. Decoded text should match the signal

### J2. CW decoder does not affect speaker audio
1. While CW decoder is active
2. Speaker audio should be unaffected — no volume change, no artifacts

---

## Pass/Fail Criteria

| Area | Pass | Known Fail (Expected) |
|------|------|-----------------------|
| Basic RX (A1-A6) | Clean audio, no silence, no stuttering | — |
| Buffer integrity (B1-B3) | No drops under normal load | — |
| RN2 pan (C2) | Pan works | — |
| NR2/NR4/BNR pan (C3-C5) | — | Pan lost (#1460, fix pending) |
| DFNR pan (C6) | Pan works | — |
| DSP switching (C7) | No audio dropout | — |
| TCI int16 (D2) | WSJT-X decodes | — |
| TCI + mute (D3) | TCI survives PC mute | — |
| TCI lifecycle (D4) | Clean connect/disconnect | — |
| Multi-slice (E1-E3) | Audio switches with slice | — |
| Voice TX (F1-F2) | Clean TX audio | — |
| DAX TX (F3) | Clean FT8 tones | — |
| DAX RX Autostart (G1-G4) | DAX streams created/removed, audio flows | — |
| DAX TX coexistence (G7) | No SmartSDR DAX lockup on Windows | — |
| DAX IQ (G8-G10) | IQ streams at all rates, clean disconnect | — |
| TCI format negotiation (G11-G15) | All 4 format combos produce correct audio | — |
| TCI DAX lifecycle (G16-G19) | Streams created on audio_start, removed on stop | — |
| TCI independence (G20) | TCI audio survives PC mute and PC Audio disable | — |
| Cross-path (G21-G23) | DAX + TCI simultaneous, mode changes | — |
| Recovery (H1-H4) | Zero false restarts in 10 min | Possible H4 if TX > 15s |
| QSO recorder (I1-I2) | Clean playback | — |
| CW decoder (J1-J2) | Decodes without artifacts | — |

---

## Audio Pipeline Reference

```
FlexRadio (VITA-49 UDP)
  │
  ├─ PCC 0x03E3: float32 stereo BE → byte-swap → float32 stereo 24kHz
  ├─ PCC 0x0123: int16 mono BE → float32 stereo 24kHz (mono duplicated)
  │
  ▼
PanadapterStream
  │
  ├──▶ audioDataReady ──▶ AudioEngine::feedAudioData()
  │                         ├─ DSP branch (NR2/NR4/BNR/DFNR/none)
  │                         ├─ Optional resample 24k→48k
  │                         ├─ m_rxBuffer (200ms cap)
  │                         └─ QAudioSink → Speaker
  │
  ├──▶ audioDataReady ──▶ CwDecoder::feedAudio() (pre-DSP tap)
  ├──▶ audioDataReady ──▶ QsoRecorder::feedRxAudio() (pre-DSP tap)
  │
  └──▶ daxAudioReady ──▶ TciServer::onDaxAudioReady()
                           ├─ Per-client resample
                           ├─ Format conversion (float32/int16 × stereo/mono)
                           └─ WebSocket → TCI clients
```

### Active Recovery Mechanisms (v0.8.15.1)

| Mechanism | Trigger | Action |
|-----------|---------|--------|
| StoppedState handler (#1303) | QAudioSink stops unexpectedly | Restart RX |
| Device change monitor (#1361) | USB device list changes (Windows only) | Restart RX |
| Zombie watchdog (#1361) | bytesFree stuck at 0 for 2s | Restart RX |
| Liveness watchdog (#1411) | No feedAudioData() calls for 15s | Restart RX |
