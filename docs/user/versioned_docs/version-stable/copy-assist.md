---
title: "Copy Assist"
slug: "/copy-assist"
description: "Copy Assist is on-device speech-to-text for received voice."
status: "Supported"
applies_to: ["All radios"]
platforms: ["Linux", "Windows", "macOS (Apple Silicon)"]
---

:::info[Status]

**Status:** Supported · **Applies to:** All radios · **Platforms:** Linux, Windows, macOS (Apple Silicon)

:::

Copy Assist is on-device speech-to-text for received voice. It transcribes what you are hearing on SSB, AM or FM into a live, scrolling text panel docked under the waterfall — an aid for weak or noisy copy, for accessibility, and for following nets and contests. It runs [whisper.cpp](https://github.com/ggml-org/whisper.cpp) on your own computer, is **receive-only**, and never keys the transmitter.

Copy Assist is host-side, so it works with every supported radio family, including receive-only setups and sessions with TX disabled.

## Requirements

- **Platform:** the Linux, Windows and Apple Silicon macOS builds include Copy Assist. The Intel macOS DMG ships without speech-to-text; building from source on an Intel Mac still works.
- **Mode:** a voice mode on the selected slice (see [When it is available](#when-it-is-available)).
- **Model download:** the first time you enable it, a speech model downloads (see [Models](#models)).
- **ONNX Runtime** for Silero VAD and speaker labels. The release builds include it.

## Setup

### Models

Model weights are **not bundled**. The selected model downloads the first time you enable Copy Assist — from Hugging Face, with AetherSDR's GitHub release as a fallback — and is SHA-256 verified before use. Models are cached in the app data folder (on Linux, `~/.local/share/AetherSDR/models/`).

| Model | Size | Notes |
|---|---|---|
| tiny | 74 MB | Fastest, roughest |
| **base** | 141 MB | Default on CPU and ARM, including the Raspberry Pi 5 |
| small | 465 MB | Desktop CPU |
| **large-v3-turbo** | 1.6 GB | Default when a usable GPU with enough free memory is found |

**Offline or air-gapped:** copy a `ggml-*.bin` model file into the models folder by hand.

**Custom model…** loads any whisper `ggml-*.bin` or `.gguf` file you choose — a fine-tune or a different quantization. It loads directly, with no download or checksum, and is remembered as `Custom: <name>`.

**sherpa-onnx model…** (Linux and macOS builds that include sherpa-onnx) runs a non-whisper offline model such as Zipformer, Moonshine or Paraformer. Point it at the extracted model folder. These are much faster than whisper on CPU and ARM, so they are the practical choice on a Raspberry Pi.

### GPU acceleration

The model runs on the GPU when one is available, otherwise on the CPU:

- **Vulkan** on Linux and Windows (NVIDIA, AMD or Intel).
- **Metal** on macOS (Apple Silicon).

If a GPU cannot be used — not enough free memory, or a driver fault — Copy Assist falls back to the CPU rather than crashing. The compute picker marks unusable devices *(unavailable)* and says when a fallback happened, without overwriting your saved choice.

A few faults inside the speech engine close the app outright. Copy Assist records which step it was in, so on the **next** launch:

- a GPU that failed while loading a model is left out, and the model runs on the next device;
- a failure while starting the engine or loading on the CPU keeps local Copy Assist off for that session (a remote server still works).

The settings window shows the reason under **Compute** with a **Try again next launch** button — use it after a driver update, or if something else closed the app. Updating AetherSDR clears the note automatically.

### Remote server

Instead of the built-in engine, Copy Assist can send audio to **your own OpenAI-compatible `/v1/audio/transcriptions` endpoint** — whisper.cpp's `whisper-server`, faster-whisper, or any compatible server — so inference runs on another machine.

Choose **Remote server…** in the model picker and enter the endpoint URL (for example `http://host:8080/v1/audio/transcriptions`), an optional API key, and the model name. AetherSDR ships **no server and no default endpoint**; nothing leaves your computer unless you configure this. The API key is stored in the OS keychain, not in the settings database.

## Using Copy Assist

### Opening Copy Assist

- Click the **ASR** toggle in the status bar (between **CWX** and **DVK**), or
- **Tools → Copy Assist**.

Either one shows or hides the panel, which docks under the waterfall of the active panadapter. Drag the panel's grip to resize it; the height is remembered.

### When it is available

- **Voice modes only:** USB, LSB, AM, SAM, FM, NFM and DFM. In CW, DIGU/DIGL and RTTY the toggle is dimmed.
- The mode check follows the **slice you have selected**, not the transmit slice. The CWX and DVK indicators follow the TX slice instead, so the three can legitimately disagree — with a CW transmit slice and a USB slice selected, CWX and ASR are both live.
- Selecting a slice that is not in a voice mode closes the panel and stops transcription. To resume, select the voice slice, reopen the panel and enable it again.
- A band-stack recall or a disconnect does **not** close an open panel, so transcription survives a band change.
- Whenever the panel is open, the ASR toggle is lit and clickable, so you can always close it by hand.

### Transcribing

1. Open the panel and click **Disabled** so it reads **Enabled**.
2. The first time, the selected model downloads, is verified and loads. A loading indicator shows progress.
3. Transcribed text streams into the panel.

Text is **coloured by recognition confidence**, the same way as the [CW Decoder](./cw-decoder.md):

| Colour | Confidence |
|---|---|
| Green | High |
| Yellow | Medium |
| Orange | Questionable |
| Red | Low |

Copy Assist hears the audio after client-side noise reduction, so a suitable [DSP Noise Mitigation](./dsp-noise-mitigation.md) setting can improve the transcript as well as your own listening.

### Context

**Context** conditions each decode on the text of the previous confident segment, which helps with continuity of names, callsigns and topic. It applies live, so you can A/B it on the fly.

- A low-confidence segment is not carried forward, so one garbled over cannot poison the next.
- A real pause, **Clear**, or a retune starts fresh.
- Context works with the whisper models only. It is greyed out on the sherpa-onnx and remote backends.

### The Queue readout

The status line shows **Queue: N s** — seconds of received audio not yet transcribed. It stays near zero when the engine keeps up and climbs (amber, then red) when it cannot, for example a large model on a Raspberry Pi.

The backlog cannot grow without limit. Once it reaches twice the Buffer setting (never less than 10 s), Copy Assist drops incoming audio until it has drained to half that, and the readout changes to **Queue: N s · dropped M s** in red. Those seconds were never transcribed, so the transcript has gaps. A retune or Disable resets both numbers. If you see drops, use a smaller model, the remote backend, a longer Buffer, or faster hardware.

## Reference

### Panel controls

| Control | Range | What it does |
|---|---|---|
| **Enabled / Disabled** | — | Starts and stops transcription. Closing the panel turns it off. |
| **⚙** | — | Opens Copy Assist Settings (below). |
| **Buffer** | 1–20 s | The longest stretch of audio collected before a decode is forced, even without a pause. |
| **Sens** | 1–100 % | Voice-activity sensitivity. Higher picks up fainter speech. |
| **Silence** | 100–2000 ms | How much trailing silence ends an utterance. |
| **A− / A+** | — | Transcript text size. |
| **↵** | — | Start a new line after each pause. |
| **Context** | Off by default | Carries context across segments (see below). |
| **Clear** | — | Clears the transcript. Also on the text area's right-click menu. |

Buffer, Sens, Silence and the panel height are remembered.

### Copy Assist Settings (⚙)

The ⚙ button opens a small modeless settings window that can stay open while you operate.

| Setting | What it does |
|---|---|
| **Model** | Which speech model runs (see [Models](#models)). Also **Custom model…**, **sherpa-onnx model…** and **Remote server…**. |
| **Compute** | Which device runs the model: a GPU or the CPU. Shows **Detecting…** while GPU discovery runs in the background. |
| **Language** | The spoken language to transcribe (multilingual models only). |
| **Save transcript to a file** | Appends each finished utterance as a timestamped line. A date is inserted before the extension, so `net.txt` writes to `net-2026-07-21.txt` and rolls over daily. A frequency marker line is written when ASR starts, on every retune, and at the top of each day's file. |
| **Use Silero VAD (ONNX)** | Replaces the built-in energy detector with the small Silero neural voice-activity model, which segments real speech far better in HF noise. Downloads on demand; **Browse…** lets you use your own `.onnx`. |
| **Label speakers (A/B/C…)** | Tags each utterance with a speaker label (`[A] …`, `[B] …`) using a downloaded speaker-embedding model. **Match threshold** (0.00–1.00) sets how strict the matching is: higher splits voices more finely, lower merges them. Labels reset on retune or re-enable. |
| **Boundary overlap** | 0–2000 ms, **Off** by default. When a long over is cut at the Buffer limit, the last N ms carry into the next segment so a word split at the cut is decoded whole, and the repeated words are removed. A few hundred ms is the practical setting. It does nothing useful for languages written without spaces (Chinese, Japanese, Thai). |

Silero VAD and speaker labels need an ONNX Runtime build; the release builds include it.

## Known issues

- On Apple Silicon, transcribing in heavy noise can freeze the whole app for several seconds at a time, worse with MNR on ([#5107](https://github.com/aethersdr/AetherSDR/issues/5107)).

## Troubleshooting

### The ASR toggle is dimmed

The selected slice is not in a voice mode. Copy Assist follows the slice you have selected, not the transmit slice.

1. Select a slice in USB, LSB, AM, SAM, FM, NFM or DFM.
2. Reopen the panel and click **Disabled** so it reads **Enabled**.

### The panel closed and transcription stopped

You selected a slice that is not in a voice mode.

1. Select the voice slice again.
2. Reopen the panel and enable it again.

### The transcript falls behind, or shows "dropped" in red

The engine cannot keep up with the audio. Once the backlog reaches twice the Buffer setting (never less than 10 s), Copy Assist drops audio, so the transcript has gaps.

1. Choose a smaller model in **⚙ → Model**, or a sherpa-onnx model on CPU or ARM.
2. Or use a **Remote server…** on a faster machine.
3. Or set a longer **Buffer**.

### Copy Assist runs on the CPU instead of the GPU

The GPU could not be used, because it lacks free memory or the driver failed, so Copy Assist fell back to the CPU. If the GPU failed during a crash, it is left out on the next launch.

1. Open **⚙** and read the reason under **Compute**. Unusable devices are marked *(unavailable)*.
2. After a driver update, or if something else closed the app, click **Try again next launch** and restart AetherSDR.

### Copy Assist stays off after the app closed unexpectedly

The speech engine faulted while starting or while loading on the CPU, so local Copy Assist is kept off for that session.

1. Open **⚙** and read the reason under **Compute**.
2. Click **Try again next launch** and restart AetherSDR.
3. In the meantime, a **Remote server…** still works.

### The model will not download

The computer has no internet access, for example in an air-gapped shack.

1. Copy a `ggml-*.bin` model file into the models folder by hand (on Linux, `~/.local/share/AetherSDR/models/`).
2. Or choose **Custom model…** and point it at a whisper `ggml-*.bin` or `.gguf` file.

## See also

- [DSP Noise Mitigation](./dsp-noise-mitigation.md) — clean up the audio Copy Assist hears
- [CW Decoder](./cw-decoder.md)
- [Accessibility](./accessibility.md)
- Primary reference: [docs/asr-copy-assist.md](https://github.com/aethersdr/AetherSDR/blob/main/docs/asr-copy-assist.md)
