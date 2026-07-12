#pragma once

#include "WaveformUploadState.h"

#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QString>

QT_BEGIN_NAMESPACE
class QTcpSocket;
QT_END_NAMESPACE

namespace AetherSDR {

class RadioModel;

// Installs radio waveform packages via the Flex file upload protocol.
//
// Legacy protocol (FlexLib Radio.cs SendSSDRWaveformFile):
//   1. Client -> Radio: "file filename <filename>"
//   2. Client -> Radio: "file upload <size> new_waveform"
//   3. Radio  -> Client: R<seq>|0|<port>
//   4. Client connects TCP to radio:<port> and streams raw .ssdr_waveform bytes
//
// Docker protocol (confirmed by pcap 4.2.18-waveform-install.pcapng, frame 4496):
//   1. Client → Radio: "file upload <size> waveform_docker_image <filename>"
//   2. Radio  → Client: R<seq>|0|<port>   (radio opens TCP server on <port>)
//   3. Radio  → Client: S0|file server active
//   4. Client connects TCP to radio:<port> and streams raw .tar.gz bytes
//   5. Radio  → Client: S0|waveform container name=<name> version=<ver>
//      (handled by FlexWaveformModel::handleContainerStatus — no action needed here)
//
// Note: unlike FirmwareUploader, there is NO "file filename <name>" step.
// The filename is embedded directly in the "file upload" command (per pcap).
//
// Mirrors the FirmwareUploader class structure.
class WaveformInstaller : public QObject {
    Q_OBJECT

public:
    explicit WaveformInstaller(RadioModel* model, QObject* parent = nullptr);

    // Backward-compatible Docker install entry point.
    void install(const QString& filePath);

    // Begin installing a legacy .ssdr_waveform package at filePath.
    // Emits progressChanged() during transfer and finished() on completion.
    void installLegacyWaveform(const QString& filePath);

    // Begin installing a Docker .tar/.tar.gz/.tgz waveform image at filePath.
    // Emits progressChanged() during transfer and finished() on completion.
    void installDockerWaveform(const QString& filePath);

    // Abort an in-progress install.
    void cancel();

    bool isInstalling() const { return m_uploadState.isCurrent(m_activeGeneration); }

signals:
    void progressChanged(int percent, const QString& status);
    void finished(bool success, const QString& message);

private:
    enum class PackageKind {
        Legacy,
        Docker
    };

    struct PackagePreparationResult {
        QByteArray data;
        QString error;
    };

    using Generation = WaveformUploadState::Generation;

    void beginInstall(const QString& filePath, PackageKind kind);
    static PackagePreparationResult preparePackage(const QString& filePath,
                                                    PackageKind kind,
                                                    qint64 expectedSize);
    void requestUploadPort(Generation generation);
    void onUploadPortReceived(Generation generation, int code, const QString& body);
    void connectUploadSocket(Generation generation, quint16 port);
    void tryFallbackPort(Generation generation);
    void onConnected(Generation generation, QTcpSocket* socket);
    void queueNextChunk(Generation generation, QTcpSocket* socket);
    void onBytesWritten(Generation generation, QTcpSocket* socket, qint64 bytes);
    void onDisconnected(Generation generation, QTcpSocket* socket);
    void onError(Generation generation, QTcpSocket* socket);
    void finishOperation(Generation generation, bool success, const QString& message);
    void destroySocket(bool abortConnection = true);

    QPointer<RadioModel> m_model;
    QTcpSocket* m_socket{nullptr};
    QByteArray m_fileData;
    QString m_fileName;
    PackageKind m_packageKind{PackageKind::Docker};
    WaveformUploadState m_uploadState;
    Generation m_activeGeneration{0};
    quint16 m_uploadPort{0};

    static constexpr qint64 kMaxFileBytes = 500LL * 1024LL * 1024LL;
    static constexpr qint64 kChunkSize = 64LL * 1024LL;
    static constexpr quint16 kFallbackPort = 42607;
    static constexpr int kConnectDelayMs = 200;
    static constexpr int kConnectTimeoutMs = 10000;
};

} // namespace AetherSDR
