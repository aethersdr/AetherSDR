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
- Tick **Enable**. On first enable the selected model is downloaded (see below),
  verified, and loaded; a loading indicator shows progress. Then transcription
  of the audio you're hearing streams into the panel.
- Text is **color-coded by recognition confidence**: green (high) → yellow →
  orange → red (low), mirroring the CW decoder.
- Hiding the panel (the status-bar **ASR** toggle / leaving voice mode) turns
  ASR off.
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

The ONNX features (Silero VAD, speaker labeling) and the signal classifier need
**ONNX Runtime**. Stage a prebuilt with `scripts/setup/setup-onnxruntime.sh`
(Linux/macOS) or `setup-onnxruntime.ps1` (Windows) — it lands under
`third_party/onnxruntime/`, which CMake detects automatically. The release
workflows run this and set `REQUIRE_ASR_ONNX=ON` so these features can't silently
drop out of a shipped build; GPU is guarded the same way where the toolchain is
installed (Linux x86_64 Vulkan, macOS Metal).

Vendored whisper.cpp is pinned; see
[`third_party/whisper.cpp/AETHER_VENDORING.md`](../third_party/whisper.cpp/AETHER_VENDORING.md).

### Tests

`ctest --test-dir build -R 'asr_|copy_assist'` — all offline/CI-safe: segmenter,
engine (fake backend), model manager (source failover + hash-mismatch), Copy
Assist panel (confidence coloring), settings dialog (model + GPU pickers),
remote backend (mock endpoint), whisper linkage, speaker clustering
(`asr_speaker_clusterer_test`, pure C++). `asr_silero_vad_test` and
`asr_speaker_embedder_test` (built only with ONNX Runtime; env-gated on a model +
WAV(s)) validate the Silero VAD and the speaker embedder end-to-end. Real GPU/CPU inference is exercised by the env-gated
`asr_whisper_backend_test` (`AETHER_ASR_TEST_MODEL` + `AETHER_ASR_TEST_PCM`).
