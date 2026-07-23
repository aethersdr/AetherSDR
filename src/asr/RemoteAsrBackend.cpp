#include "asr/RemoteAsrBackend.h"

#include <QDataStream>
#include <QEventLoop>
#include <QHttpMultiPart>
#include <QHttpPart>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <cmath>

namespace AetherSDR {

Q_LOGGING_CATEGORY(lcAsrRemote, "aether.asr.remote")

RemoteAsrBackend::RemoteAsrBackend(RemoteAsrConfig config)
    : m_config(std::move(config))
{
}

RemoteAsrBackend::~RemoteAsrBackend() = default;

bool RemoteAsrBackend::load(const QString& /*modelPath*/, QString* error)
{
    if (m_config.url.isEmpty()) {
        if (error != nullptr) {
            *error = QStringLiteral("No remote ASR endpoint configured.");
        }
        return false;
    }
    if (!m_nam) {
        m_nam = std::make_unique<QNetworkAccessManager>();
    }
    m_loaded = true;
    qCInfo(lcAsrRemote) << "Remote ASR endpoint:" << m_config.url << "model:" << m_config.model;
    return true;
}

QByteArray RemoteAsrBackend::encodeWav16(const std::vector<float>& pcm16k, int sampleRate)
{
    const int n = static_cast<int>(pcm16k.size());
    const int bytesPerSample = 2; // 16-bit
    const quint32 dataSize = static_cast<quint32>(n) * bytesPerSample;

    QByteArray wav;
    QDataStream ds(&wav, QIODevice::WriteOnly);
    ds.setByteOrder(QDataStream::LittleEndian);

    ds.writeRawData("RIFF", 4);
    ds << quint32(36 + dataSize);
    ds.writeRawData("WAVE", 4);
    ds.writeRawData("fmt ", 4);
    ds << quint32(16);                                       // PCM fmt chunk size
    ds << quint16(1);                                        // audioFormat = PCM
    ds << quint16(1);                                        // mono
    ds << quint32(sampleRate);
    ds << quint32(static_cast<quint32>(sampleRate) * bytesPerSample); // byte rate
    ds << quint16(bytesPerSample);                           // block align
    ds << quint16(16);                                       // bits per sample
    ds.writeRawData("data", 4);
    ds << dataSize;

    for (const float f : pcm16k) {
        const float c = std::clamp(f, -1.0f, 1.0f);
        ds << static_cast<qint16>(std::lround(c * 32767.0f));
    }
    return wav;
}

AsrTranscript RemoteAsrBackend::transcribe(const std::vector<float>& pcm16k, QString* error)
{
    if (!m_loaded || m_nam == nullptr) {
        if (error != nullptr) {
            *error = QStringLiteral("Remote backend not loaded.");
        }
        return {};
    }
    if (pcm16k.empty()) {
        return {};
    }

    auto* multi = new QHttpMultiPart(QHttpMultiPart::FormDataType);
    auto addText = [multi](const QString& name, const QString& value) {
        QHttpPart part;
        part.setHeader(QNetworkRequest::ContentDispositionHeader,
                       QStringLiteral("form-data; name=\"%1\"").arg(name));
        part.setBody(value.toUtf8());
        multi->append(part);
    };

    QHttpPart filePart;
    filePart.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("audio/wav"));
    filePart.setHeader(QNetworkRequest::ContentDispositionHeader,
                       QByteArrayLiteral("form-data; name=\"file\"; filename=\"audio.wav\""));
    filePart.setBody(encodeWav16(pcm16k));
    multi->append(filePart);

    addText(QStringLiteral("model"), m_config.model);
    if (!m_config.language.isEmpty()) {
        addText(QStringLiteral("language"), m_config.language);
    }
    addText(QStringLiteral("response_format"), QStringLiteral("verbose_json"));

    QNetworkRequest request{QUrl(m_config.url)};
    if (!m_config.apiKey.isEmpty()) {
        request.setRawHeader("Authorization", "Bearer " + m_config.apiKey.toUtf8());
    }

    QNetworkReply* reply = m_nam->post(request, multi);
    multi->setParent(reply); // freed with the reply

    // Synchronous round-trip on this (worker) thread, with a timeout.
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timeout.start(m_config.timeoutMs);
    loop.exec();

    if (reply->isRunning()) {
        reply->abort();
        if (error != nullptr) {
            *error = QStringLiteral("Remote ASR request timed out.");
        }
        reply->deleteLater();
        return {};
    }
    if (reply->error() != QNetworkReply::NoError) {
        if (error != nullptr) {
            *error = QStringLiteral("Remote ASR request failed: %1").arg(reply->errorString());
        }
        reply->deleteLater();
        return {};
    }

    const QByteArray body = reply->readAll();
    reply->deleteLater();

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error != nullptr) {
            *error = QStringLiteral("Remote ASR returned an unparseable response.");
        }
        return {};
    }

    const QJsonObject obj = doc.object();
    AsrTranscript result;
    result.text = obj.value(QStringLiteral("text")).toString().trimmed();

    // Confidence from verbose_json segment avg_logprob (a log-probability), if the
    // server provides it; otherwise trust the configured server (1.0).
    result.confidence = 1.0f;
    const QJsonValue segs = obj.value(QStringLiteral("segments"));
    if (segs.isArray()) {
        double sum = 0.0;
        int count = 0;
        for (const QJsonValue& v : segs.toArray()) {
            const QJsonObject seg = v.toObject();
            if (seg.contains(QStringLiteral("avg_logprob"))) {
                sum += seg.value(QStringLiteral("avg_logprob")).toDouble();
                ++count;
            }
        }
        if (count > 0) {
            result.confidence = std::clamp(static_cast<float>(std::exp(sum / count)), 0.0f, 1.0f);
        }
    }
    return result;
}

std::function<std::unique_ptr<IAsrBackend>()> remoteAsrBackendFactory(const RemoteAsrConfig& config)
{
    return [config]() -> std::unique_ptr<IAsrBackend> {
        return std::make_unique<RemoteAsrBackend>(config);
    };
}

} // namespace AetherSDR
