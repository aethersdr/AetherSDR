#pragma once

#include "asr/IAsrBackend.h"

#include <QString>

#include <functional>
#include <memory>
#include <vector>

struct whisper_context;

namespace AetherSDR {

// whisper.cpp implementation of IAsrBackend. CPU inference (the vendored ggml
// is CPU-only for now); language defaults to English but is configurable.
// Lives entirely on AsrEngine's worker thread.
class WhisperAsrBackend : public IAsrBackend {
public:
    WhisperAsrBackend();
    // gpuDevice: which GPU to run on (index among GPU devices; see asrGpuDevices),
    // or -1 to force CPU.
    explicit WhisperAsrBackend(QString language, int gpuDevice = 0);
    ~WhisperAsrBackend() override;

    bool load(const QString& modelPath, QString* error) override;
    bool isLoaded() const override { return m_ctx != nullptr; }
    AsrTranscript transcribe(const std::vector<float>& pcm16k, QString* error) override;
    void unload() override;

    void setLanguage(const QString& language) { m_language = language; }

private:
    whisper_context* m_ctx = nullptr;
    QString m_language;
    int m_threads = 0;
    int m_gpuDevice = 0;
};

// A selectable GPU: `index` is the value to pass as gpuDevice (its position among
// GPU/IGPU devices in ggml's enumeration order); `name` is a human description.
struct AsrGpuDevice {
    int index = 0;
    QString name;
};

// Factory for wiring AsrEngine to the production whisper backend. Kept here so
// AsrEngine.cpp never references whisper (keeping the engine — and its unit
// test — independent of the vendored library). Matches AsrBackendFactory.
std::function<std::unique_ptr<IAsrBackend>()>
whisperAsrBackendFactory(const QString& language = QStringLiteral("en"), int gpuDevice = 0);

// True when a GPU ggml backend (Vulkan/Metal) is compiled in and a GPU device is
// present. Used to default the model tier and enable GPU inference.
bool asrGpuAvailable();

// All selectable GPU devices (discrete + integrated), in the order whisper's
// gpu_device indexes them. Empty on CPU-only builds / GPU-less hosts.
std::vector<AsrGpuDevice> asrGpuDevices();

} // namespace AetherSDR
