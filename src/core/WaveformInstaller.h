#pragma once

#include <QObject>
#include <QTcpSocket>
#include <QByteArray>
#include <QString>

namespace AetherSDR {

class RadioModel;

// Installs a Docker waveform container image on the radio via the file upload
// protocol introduced in firmware 4.2.18.
//
// Protocol (confirmed by pcap 4.2.18-waveform-install.pcapng, frame 4496):
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

    // Begin installing the .tar.gz waveform image at filePath.
    // Emits progressChanged() during transfer and finished() on completion.
    void install(const QString& filePath);

    // Abort an in-progress install.
    void cancel();

    bool isInstalling() const { return m_installing; }

signals:
    void progressChanged(int percent, const QString& status);
    void finished(bool success, const QString& message);

private:
    void onUploadPortReceived(int code, const QString& body);
    void onConnected();
    void onBytesWritten(qint64 bytes);
    void onError();

    RadioModel* m_model{nullptr};
    QTcpSocket  m_socket;
    QByteArray  m_fileData;
    QString     m_fileName;
    qint64      m_bytesSent{0};
    int         m_uploadPort{-1};
    bool        m_installing{false};
    bool        m_cancelled{false};

    static constexpr qint64 MAX_FILE_BYTES = 500LL * 1024 * 1024;  // 500 MB
    static constexpr int    CHUNK_SIZE     = 65536;                 // 64 KB
    static constexpr int    FALLBACK_PORT  = 42607;  // fw 4.2.18 observed default
};

} // namespace AetherSDR
