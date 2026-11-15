#pragma once

#include "asr/IAsrBackend.h"

#include <QString>

#include <functional>
#include <memory>

class QNetworkAccessManager;

namespace AetherSDR {

// User-configured remote endpoint (RFC #4338, Phase 6). `url` is the full POST
// URL of an OpenAI-compatible transcription endpoint (e.g.
// http://host:8080/v1/audio/transcriptions, or whisper.cpp's whisper-server
// /inference). `apiKey` is optional (sent as a Bearer token when set).
struct RemoteAsrConfig {
    QString url;
    QString apiKey;
    QString model = QStringLiteral("whisper-1");
    QString language = QStringLiteral("en");
    int timeoutMs = 30000;
};

// IAsrBackend that offloads transcription to a remote OpenAI-compatible
// /v1/audio/transcriptions endpoint. Encodes the utterance as a 16 kHz mono WAV
// and POSTs it as multipart/form-data. Runs on the AsrEngine worker thread, so
// the network round-trip blocks only that thread (never the audio/UI thread).
//
// This is the "bring your own server" path — AetherSDR ships no server and no
// default endpoint; bundled whisper.cpp remains the zero-config default.
class RemoteAsrBackend : public IAsrBackend {
public:
    explicit RemoteAsrBackend(RemoteAsrConfig config);
    ~RemoteAsrBackend() override;

    // For the remote backend `modelPath` is ignored (the model is named in the
    // config). Returns false only if no endpoint URL is configured.
    bool load(const QString& modelPath, QString* error) override;
    bool isLoaded() const override { return m_loaded; }
    AsrTranscript transcribe(const std::vector<float>& pcm16k, QString* error) override;
    void unload() override { m_loaded = false; }

    // Encode 16 kHz mono float samples as a 16-bit PCM WAV byte buffer.
    static QByteArray encodeWav16(const std::vector<float>& pcm16k, int sampleRate = 16000);

private:
    RemoteAsrConfig m_config;
    std::unique_ptr<QNetworkAccessManager> m_nam;
    bool m_loaded = false;
};

// Factory for wiring AsrEngine to a remote endpoint (matches AsrBackendFactory).
std::function<std::unique_ptr<IAsrBackend>()> remoteAsrBackendFactory(const RemoteAsrConfig& config);

} // namespace AetherSDR
