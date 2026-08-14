# Copy Assist — On-device Speech-to-Text (ASR)

Copy Assist transcribes received **voice** (SSB/AM/FM) into a live, scrolling
text panel docked under the waterfall — an assist for weak/noisy copy,
accessibility, and nets/contests. It is **receive-only** and never keys TX.

Design + decision record: RFC **#4333** (accepted). Engine: **whisper.cpp**
(MIT). This document is the user + contributor reference.

## Using it

- Open with the **`ASR`** toggle in the status bar (between `CWX` and `DVK`),
  which shows/hides the panel. The toggle is the inverse of `CWX`:
  **enabled in voice modes** (USB/LSB/AM/SAM/FM/NFM/DFM), **dimmed in CW and
  DIGx/RTTY**.
- That mode test reads the **slice you have selected**, not the transmit slice —
  Copy Assist is a receive-side feature, so it is available with **TX off** and
  on a receive-only setup (#4825). Its two neighbours in the status bar behave
  the opposite way on purpose: `CWX` and `DVK` are transmit keyers and follow the
  TX slice, so the three indicators can legitimately disagree — with a CW
  transmit slice and a USB slice selected, `CWX` and `ASR` are both live.
- Tick **Enable**. On first enable the selected model is downloaded (see below),
  verified, and loaded; a loading indicator shows progress. Then transcription
  of the audio you're hearing streams into the panel.
- Text is **color-coded by recognition confidence**: green (high) → yellow →
  orange → red (low), mirroring the CW decoder.
- **Context** (header toggle, off by default) — carries context across segment
  boundaries: each decode is conditioned on the *previous confident segment's*
  own text (via whisper's `initial_prompt`), for continuity of names, callsigns,
  and topic. Applied live (no model reload) so it can be A/B'd on the fly. It's
  self-protecting: a low-confidence decode is **not** carried (so a garbled over
  can't poison the next), and a real pause, **Clear**, or a retune starts fresh.
  Whisper-only — greyed out on the sherpa-onnx and remote backends, which don't
  implement it.
- Hiding the panel (the status-bar **ASR** toggle / leaving voice mode) turns
  ASR off. "Leaving voice mode" includes **selecting a slice that is not in a
  voice mode** — clicking to a CW slice while Copy Assist is running closes the
  panel and stops transcription, even if the voice slice you were transcribing is
  still audible. To resume, re-select the voice slice, re-open the panel and
  tick **Enable** again — closing the panel unticks it.
- A **band-stack recall is not** leaving voice mode, and neither is a disconnect.
  A recall drops the slice and re-creates it on the new band a moment later, and
  a disconnect clears every slice, so an open panel is left alone in both states
  rather than torn down — transcription survives a band change. That holds
  whether or not another slice survives the recall: with a second slice on the
  pan the client re-selects it while the radio rebuilds, and a surviving CW or
  DIGx slice does not count as you leaving voice mode either.
- Whenever the panel is open the `ASR` toggle is **lit and clickable**, in every
  one of those states, so you can always close it by hand.
- The status line shows a **`Queue: N s`** backlog — seconds of received audio not
  yet transcribed. It stays near 0 when the engine keeps up and climbs
  (amber→red) when it can't (e.g. whisper on a Raspberry Pi), so you can see ASR
  falling behind. If it grows without bound, use a smaller model, the remote
  backend, or faster hardware.

### Settings (⚙)

The **⚙ button** (next to Enabled) opens a small modeless **settings dialog**
holding the **model** and **compute-device** (GPU/CPU) pickers, with room for
more options. It floats over the app and can stay open while you operate.

- **Save transcript to a file** — when ticked, every finished utterance is
  appended as one timestamped line (`2026-07-21T14:30:05<TAB>text`). You name a
  base file; a **per-day date is inserted before the extension**, so
  `net.txt` writes to `net-2026-07-21.txt` and rolls to a new file each day.
  Appends only (never truncates); the base name and on/off state persist across
  sessions.
  - A **frequency marker** (`<TAB>=== 14.074000 MHz ===`, on its own line after a
    blank line) is written when ASR starts, whenever you retune, and at the top
    of each new day's file — so every block of decoded text is labeled with the
    frequency it came from.
- **Use Silero VAD (ONNX)** — replaces the built-in energy voice-activity
  detector with the ~2 MB [Silero VAD](https://huggingface.co/onnx-community/silero-vad)
  neural model, which is far more robust in HF noise (it segments *actual speech*
  rather than anything above an energy threshold). Ticking it **downloads the
  model on demand** (Hugging Face, SHA-256-verified, cached in the models dir) —
  no file to find; **Browse…** overrides with your own `.onnx`. Runs in the ONNX
  Runtime AetherSDR already ships (`HAVE_ONNX`); unset → energy VAD (unchanged).
  Requires an ONNX-Runtime-enabled build.
- **Label speakers (A/B/C…)** — tags each utterance with a speaker label using an
  ONNX **speaker-embedding** model ([WeSpeaker ECAPA-TDNN](https://huggingface.co/Wespeaker/wespeaker-ecapa-tdnn512-LM),
  ~24 MB, auto-downloaded + SHA-verified). Each closed "over" is embedded
  (kaldi-style Fbank → ECAPA → 192-d vector) and **online-clustered** by cosine
  similarity: the nearest known speaker, or a new one. Panel and log lines are
  prefixed `[A] …`, `[B] …`. Half-duplex helps — one transmitter at a time means
  each utterance is a single speaker, so no overlap handling is needed. Labels are
  session-relative (reset on retune/re-enable). A **Match threshold** slider
  (0.00–1.00, cosine; applied live) tunes it — higher = stricter (more, finer
  splits), lower = looser (fewer, merged speakers). RF caveats: narrowband/noise
  and propagation drift can split or merge a voice, so expect to tune it.
  Requires an ONNX-Runtime-enabled build.
- **Boundary overlap** (slider, 0–2000 ms; **0 = Off, the default**) — recovers a
  word chopped in half by the **Buffer** cap. A long "over" that never pauses is
  force-closed at the buffer limit, and that cut can land mid-word; because each
  segment is decoded on its own, the straddling word comes out mangled or
  missing. With overlap set, the last N ms of a *cap-forced* segment are carried
  into the front of the next one so the word is decoded whole, and the repeated
  boundary words are then stripped from the transcript. Applied live, no model
  reload, and it works on every backend (whisper, sherpa-onnx, remote).
  - Costs a little extra decoding — the carried audio is transcribed twice — so
    the practical setting is a few hundred ms, not the maximum. The carry is
    internally capped at half the Buffer, so no combination of the two sliders
    can run away.
  - Only fires on a **buffer-cap** close. A normal close on a silence gap has no
    split word to recover, and is left alone.
  - The de-duplication is word-based, so it does nothing for languages whisper
    transcribes without spaces (Chinese, Japanese, Thai) — on those, leave it
    Off or expect the overlapping words to appear twice.

### Tuning (the control row)

| Control | Range | Effect |
|---|---|---|
| **Buffer** | 1–20 s | Max audio accumulated before a decode is forced without a silence gap. |
| **Sensitivity** | 1–100 % | VAD threshold — higher picks up fainter/weaker speech. |
| **Silence** | 100–2000 ms | Trailing silence that ends an utterance. |

All three, plus the panel height, persist client-side in `AppSettings`.

## Models

Weights are **not bundled** — they download on first enable and cache under
`QStandardPaths::AppDataLocation/models` (e.g. `~/.local/share/AetherSDR/models/`).
Sources are tried in order and each download is **SHA-256-verified** before it
is accepted:

1. **Hugging Face** — `huggingface.co/ggerganov/whisper.cpp` (primary)
2. **GitHub release** — `aethersdr/AetherSDR` tag `asr-models-v1` (mirror fallback)

| Tier | Size | Notes |
|---|---|---|
| tiny | 74 MB | fastest, roughest |
| **base** | 141 MB | default on CPU/ARM (incl. Raspberry Pi 5) |
| small | 465 MB | desktop CPU |
| **large-v3-turbo** | 1.6 GB | default when a GPU is available |

Offline/air-gapped: drop the `ggml-*.bin` file into the models dir manually.

### Bring your own model

To use a model that isn't in the tier list — a fine-tune, a different
quantization, or a manually-downloaded `ggml-*.bin`/`.gguf` — select
**"Custom model…"** in the model picker (⚙ settings) and pick the file. It loads directly
(no download, no checksum), the picker remembers it (shown as `Custom: <name>`),
and it runs on the selected compute device like any built-in tier.

## GPU acceleration

The selected model runs on the **GPU when one is available**, else CPU
(automatic fallback). GPU is auto-detected at build time and used at runtime via
`ggml_backend_dev_by_type(GPU)`:

- **Vulkan** — Linux/Windows (NVIDIA/AMD/Intel). Requires the Vulkan
  loader+headers, `glslc`, and `SPIRV-Headers` at build time (`ENABLE_ASR_VULKAN`,
  auto).
- **Metal** — macOS (native). Uses the Metal framework + `metal` compiler from
  full Xcode (`ENABLE_ASR_METAL`, auto on Apple).

Without the toolchain the build is CPU-only, unchanged. A GPU-enabled binary
still runs on GPU-less hosts.

### Not shipped on the Intel macOS DMG

The **Intel macOS release build has no ASR at all** (`ENABLE_ASR=OFF`, #4719).
Building it from source on an Intel Mac still works; this is about the shipped
artifact.

The reason is reach, not the feature. The ASR stack pulls in an ONNX Runtime
built at `minos 15.5`, and dyld enforces that floor on every Mach-O it loads —
so a DMG carrying it requires macOS 15.5 no matter what it advertises, which
excludes most of the older Intel hardware that artifact exists for (#4713,
#4532). Dropping ASR is what lets the Intel DMG hold a 12.0 floor. Intel Macs
also have no GPU worth running ASR on, so what was left there after the ONNX
and sherpa backends came out was CPU-only whisper.

Apple Silicon is unaffected: Metal 3.1 with bf16, plus the ONNX and sherpa
backends, all required at configure time.

## sherpa-onnx models (non-whisper engines)

The model picker's **"sherpa-onnx model…"** entry (shown when sherpa-onnx is
built in) runs a [sherpa-onnx](https://github.com/k2-fsa/sherpa-onnx) **offline**
model through its C API — transducer (Zipformer), Moonshine, Paraformer, CTC, and
more — via `SherpaOnnxBackend` (`AsrBackendKind::SherpaOnnx`). These are far
faster than whisper on CPU/ARM, so they're the practical path for a Raspberry Pi.

sherpa models ship as multi-file **bundles** (a folder of `.onnx` files +
`tokens.txt`), so the entry opens a **folder picker**; the backend auto-detects
the layout. Grab models from sherpa-onnx's model zoo and point at the extracted
directory. sherpa-onnx bundles its own ONNX Runtime, which the whole app then
shares (single runtime, no version clash).

Stage it with `scripts/setup/setup-sherpa-onnx.sh` (Linux x64/aarch64 + macOS;
`REQUIRE_ASR_SHERPA=ON` makes it a release requirement). The AppImage and DMG
release builds run this and ship sherpa-onnx. k2-fsa publishes no aarch64
shared-lib, so the ARM one is built by AetherSDR's own CI
([`build-sherpa-onnx-aarch64.yml`](../.github/workflows/build-sherpa-onnx-aarch64.yml))
and hosted on the `sherpa-onnx-libs` release. Windows is a follow-up (needs a
`setup-sherpa-onnx.ps1`).

## Remote backend (bring your own server)

Instead of the bundled engine, Copy Assist can offload transcription to a
**user-configured OpenAI-compatible `/v1/audio/transcriptions` endpoint** —
whisper.cpp's `whisper-server`, faster-whisper, or any compatible server — to
run inference on another machine or experiment with different engines.

Select **"Remote server…"** in the model picker (⚙ settings) and enter the endpoint URL
(e.g. `http://host:8080/v1/audio/transcriptions`), an optional API key, and the
model name. AetherSDR ships **no server and no default endpoint**; cloud
endpoints are entirely opt-in and user-configured.

## Architecture (contributors)

ASR lives in its own static library **`aetherasr`** (Qt Core/Network +
whisper) — **not** in `libaethercore`, which stays whisper-free (verified: 0
whisper symbols) so a thin UI / headless engine never links it.

```
AudioEngine (aethercore, 24 kHz post-NR RX)
   └─ AsrAudioTap (gui)  ── mono → ──▶ AsrEngine (aetherasr)
                                          ├─ worker thread: resample 24k→16k (r8brain)
                                          ├─ AsrSegmenter (energy VAD → utterances)
                                          └─ IAsrBackend
                                               ├─ WhisperAsrBackend (local, CPU/Vulkan/Metal)
                                               ├─ SherpaOnnxBackend (non-whisper, ONNX)
                                               └─ RemoteAsrBackend (HTTP endpoint)
   AsrEngine::finalText(text, confidence) ──▶ CopyAssistPanel (gui, color-coded)
```

- **`IAsrBackend`** is the pluggable seam; `WhisperAsrBackend` and
  `RemoteAsrBackend` implement it. `AsrEngine` is backend-agnostic and testable
  with a fake backend.
- **`AsrEngine::finalText`** is the stream seam the UI subscribes to (a signal
  today; over the aetherd wire later — the thin UI never links whisper).
- All inference + resampling + verification run off the audio/UI threads.

### Build flags

| Flag | Default | Effect |
|---|---|---|
| `ENABLE_ASR` | ON | Build ASR (`aetherasr` + Copy Assist). `OFF` = no ASR. |
| `ENABLE_ASR_VULKAN` | ON (auto) | Vulkan GPU backend when the toolchain is present (non-Apple). |
| `ENABLE_ASR_METAL` | ON (Apple) | Metal GPU backend (macOS). |
| `REQUIRE_ASR_ONNX` | OFF | **Release guard** — fail configure if ONNX Runtime is missing (else VAD/speaker/classifier silently compile out). |
| `REQUIRE_ASR_GPU` | OFF | **Release guard** — fail configure if no GPU backend (Vulkan/Metal) is enabled. |
| `USE_SYSTEM_LIBWHISPER` | OFF | Link a distro libwhisper (**≥ 1.8.0**) via pkg-config instead of the vendored snapshot. |

The ONNX features (Silero VAD, speaker labeling) and the signal classifier need
**ONNX Runtime**. Stage a prebuilt with `scripts/setup/setup-onnxruntime.sh`
(Linux/macOS) or `setup-onnxruntime.ps1` (Windows) — it lands under
`third_party/onnxruntime/`, which CMake detects automatically. The release
workflows run this and set `REQUIRE_ASR_ONNX=ON` so these features can't silently
drop out of a shipped build; GPU is guarded the same way where the toolchain is
installed (Linux x86_64 Vulkan, macOS Metal).

Vendored whisper.cpp is pinned; see
[`third_party/whisper.cpp/AETHER_VENDORING.md`](../third_party/whisper.cpp/AETHER_VENDORING.md).

`USE_SYSTEM_LIBWHISPER=ON` is the packager escape hatch from that pin — it drops
the vendored subdirectory and takes `whisper` (plus `ggml`, which
`WhisperAsrBackend` calls directly) from pkg-config. The 1.8.0 floor is the first
release carrying `GGML_BACKEND_DEVICE_TYPE_IGPU`. Since the ggml backends built
into a distro package are the packager's choice and can't be read back at
configure time, this path never reports a GPU backend and is rejected outright
under `REQUIRE_ASR_GPU`: release images build the vendored engine.

### Tests

`ctest --test-dir build -R 'asr_|copy_assist'` — all offline/CI-safe: segmenter,
engine (fake backend), model manager (source failover + hash-mismatch), Copy
Assist panel (confidence coloring), settings dialog (model + GPU pickers),
remote backend (mock endpoint), whisper linkage, speaker clustering
(`asr_speaker_clusterer_test`, pure C++). `asr_silero_vad_test` and
`asr_speaker_embedder_test` (built only with ONNX Runtime; env-gated on a model +
WAV(s)) validate the Silero VAD and the speaker embedder end-to-end. Real GPU/CPU inference is exercised by the env-gated
`asr_whisper_backend_test` (`AETHER_ASR_TEST_MODEL` + `AETHER_ASR_TEST_PCM`).
