# Experimental native HD FM receiver

This is a separate draft amendment to RFC #5468, stacked on the analog WFM
applet work. Maintainer architecture approval and native qualification are
pending. The approved analog WFM work does not approve these dependencies,
worker, or digital receiver. No merge or release readiness is implied.

## Build and dependency boundary

`ENABLE_HD_FM` defaults OFF. Its initial opt-in build supports Linux with GNU C
and the existing enabled RTL/FFTW3f dependencies; other toolchains fail configure
when explicitly opted in. Default Mac/Windows builds retain analog WFM.

The embedded sources are pinned to nrsc5
`0225922b6f68109df39d07391f4d855464598ab8` and FAAD2 2.11.2
`673a22a3c7c33e96e2ff7aae7c4d2bc190dfbf92` with the pinned nrsc5 HDC patch.
`third_party/nrsc5` and `third_party/faad_hdc` contain per-file provenance,
licenses, and notices. The explicit source lists in `cmake/AetherHdFm.cmake`
build static libraries. Neither upstream build is evaluated: there is no
network download, CLI, external decoder process, or install hook.

Aether owns the RTL device. The wrapper exposes only the synchronous
`nrsc5_open_pipe` / `nrsc5_pipe_samples_cf32` path. Native open/close operations
share Aether's process-wide single-precision FFTW planner lock.

## Capture, worker, and audio contracts

The initial receiver admits one active wide HD receiver. It does not expand
receiver capacity. A complete digital footprint of ±225 kHz plus at least
3 kHz transition guards must fit the real usable capture, independently of
stored analog filter edges. Invalid original filter geometry remains invalid.
Selecting HD uses the existing prepare/adopt/rollback transaction; a rejected
recipe does not publish or persist a selection.

`HdFmIqAdapter` consumes original capture IQ before the analog filter or
FM discriminator. It translates phase-continuously and uses paired resamplers
to produce exactly **744187.5 complex samples/s**, without integer rounding.
Supported achieved input rates are integral 900001 through 3000000 samples/s.
A capture gap or nonfinite input withdraws that adapter epoch.

The registry prepares and retires a dedicated decoder worker off acquisition.
A process-wide reservation allows at most two resident workers for the old/new
handoff. IQ submission uses 32 fixed slots of at most 8192 complex samples;
a queued source distance above 250 ms or a full queue fails the receiver.
The acquisition path does not wait for decoding. Private decoder callbacks copy
borrowed data immediately into bounded queues. No upstream device worker starts.

The selected program must be synchronized and emit an unflagged, bounded,
even-length native stereo callback before audio is valid. Valid digital silence
is accepted. Unavailable or concealed frames do not establish valid audio.
Program discovery is separate and remains selectable through a concealed frame.

The independent slice tap is true **44100 Hz stereo**, before monitor gain,
pan, or mute. It uses decoded-frame positions and original source identity.
A paired explicit conversion supplies **48000 Hz** to the existing speaker
mixer; speaker/auxiliary PCM domains and global defaults remain 24/48 kHz.
Decoder consumers explicitly convert the new slice rate to their fixed domain.

The initial worker has 32768-frame PCM queues and an 8192-frame 48 kHz prefill.
Acquisition publishes intentional speaker zeros while acquiring; it never
fabricates decoded slice PCM. Post-ready underrun retires the audio epoch.
These queue and prefill choices require recorded and live decoder-burst
qualification; they are not a demonstrated guarantee for arbitrary RF input.
The existing mixer deadline is unchanged.

Receiver, capture, session, revision, selected program, audio epoch, and original
production time travel with data. Buffered speaker contributions retain their
original token across control revisions. Readiness and withdrawal have a separate
monotonic publication sequence; they do not invent decoder measurements or
refresh their timestamps. Backend acceptance rejects malformed observations
before mutating accepted identity or PCM. Original observation/audio ages expire
at 500 ms, independently of delayed owner-thread receipt. Epoch changes reset
prefill before tap publication. Health exposes observed IQ drops, PCM drops,
and playout underruns cumulatively since connection.

## Presentation and persistence

The WFM applet cycles Mono → Stereo → HD Stereo only when the backend declares
the implemented HD capability. Accepted selection, sync, and selected audio
validity are separate states. The selector lists actual discovered audio
programs as HD1 through HD8. SIG channel/port numbers are not guessed to be
program numbers, so program names remain a truthful fallback for now.

HD diagnostics show actual MER, CBER, frequency offset, and sync history.
The API's lower/upper MER labels use the NRSC-5 spectral convention, inverted
relative to RF sidebands. HD does not reuse the analog pilot trace or invent
constellation, SNR, or quality metrics.

One bounded owner-thread snapshot supplies station-name and selected-program
ID3 text to the applet and passive local panadapter overlay. Complete text
updates replace the same slice/program record. Retune, selection, loss, park,
and disconnect clear mismatched information. There is no cluster spot creation
or external publication.

SpotHub → Display contains one ordinary **WFM RDS** on/off button made by the
same factory as Memory. It defaults ON for a fresh profile, preserves saved
OFF, and controls only this local overlay. The applet and decoding continue
while it is off. `WfmPresentationSettings` owns its AppSettings document,
preserving sibling settings. Actual displayed source is HD Radio;
analog RDS/RBDS remains a follow-up. No automatic analog/digital blending exists.

## Validation boundaries

The socket-free tests cover fractional IQ conversion and partition invariance,
full-footprint admission, documented event reduction, bounded worker queues,
loss during queue drains, readiness, identity, persistence, and UI updates.
`nrsc5_fm_decoder_test` and `hd_fm_receiver_test` run only with `ENABLE_HD_FM=ON`;
they do not silently join the frozen per-PR gate. Injected API callbacks prove
client lifecycle behavior, not over-the-air decoder correctness.

The explicit `hd_fm_recording_probe` target is excluded from the default build
and CTest. Supply an already-decompressed unsigned-IQ recording:

```sh
cmake -S . -B build -DENABLE_HD_FM=ON
cmake --build build --parallel 4 --target hd_fm_recording_probe
./build/hd_fm_recording_probe sample.cu8 --program 0 --compressed-source sample.xz
```

It drives the production adapter and native decoder, reports source/executable
hashes, actual metadata, distinct-channel statistics, native callback bursts,
and gaps measured on the source sample clock. It opens no USB device or socket
and does not play or export audio. Recorded decoding is separate from the
bounded worker, speaker playout, GUI, live 104.7 MHz reception, or listening.
A separate explicit `hd_fm_worker_recording_probe` target replays the same CU8
recording at absolute 1× source-clock deadlines through the production pipeline,
registry, native decoder worker and shared mixer:

```sh
cmake --build build --parallel 4 --target hd_fm_worker_recording_probe
./build/hd_fm_worker_recording_probe sample.cu8 --program 0 --compressed-source sample.xz
```

It reports delivered 44.1 kHz slice and 48 kHz speaker PCM, original time/identity
checks, readiness transitions, queue-drop and mixer-fault counters, scheduling
lateness, process CPU and measured teardown. Acquiring speaker silence is
separate from source-produced PCM. Rejected real PCM, expiry or observed faults
cannot be hidden by a short successful prefix. It drains immediately on the
same thread and applies owner-like acceptance checks; it does not instantiate
the backend, GUI, audio device or USB conversion/DC suppression path. End of
file adds no RF padding or decoder flush, so totals describe the delivered
prefix, with a pending tail discarded at stop. Queue occupancy is not measured.
The process has a 120-second watchdog; teardown is measured for this input,
not guaranteed for arbitrary decoder behavior.

Native Windows/Mac/ARM and current sanitizer qualification remain outstanding.
