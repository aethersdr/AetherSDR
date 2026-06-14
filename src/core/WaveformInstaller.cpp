#include "WaveformInstaller.h"
#include "LogManager.h"
#include "../models/RadioModel.h"

#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QTimer>

namespace AetherSDR {

WaveformInstaller::WaveformInstaller(RadioModel* model, QObject* parent)
    : QObject(parent), m_model(model)
{
    connect(&m_socket, &QTcpSocket::connected,     this, &WaveformInstaller::onConnected);
    connect(&m_socket, &QTcpSocket::bytesWritten,  this, &WaveformInstaller::onBytesWritten);
    connect(&m_socket, &QTcpSocket::errorOccurred, this, [this] { onError(); });
}

void WaveformInstaller::install(const QString& filePath)
{
    if (m_installing) {
        emit finished(false, tr("Install already in progress"));
        return;
    }

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        emit finished(false, tr("Cannot open file: %1").arg(file.errorString()));
        return;
    }

    // Validate file format: accept gzip-compressed tars (magic 0x1F 0x8B) and
    // plain uncompressed tars (POSIX "ustar" magic at byte 257).
    // FlexRadio waveform packages may use either format — newer releases ship
    // Docker OCI image layout tars without gzip compression despite a .tar.gz
    // extension (confirmed with freedv-waveform-2.4.0-dev-7132.tar.gz).
    const QByteArray header = file.read(262);
    const bool isGzip = header.size() >= 2
                        && static_cast<quint8>(header[0]) == 0x1F
                        && static_cast<quint8>(header[1]) == 0x8B;
    const bool isTar  = header.size() >= 262
                        && header.mid(257, 5) == QByteArray("ustar", 5);
    if (!isGzip && !isTar) {
        emit finished(false, tr("Not a valid waveform image (expected a .tar.gz or tar archive)"));
        return;
    }
    file.seek(0);

    m_fileData = file.readAll();
    file.close();

    if (m_fileData.isEmpty()) {
        emit finished(false, tr("File is empty"));
        return;
    }

    if (m_fileData.size() > MAX_FILE_BYTES) {
        emit finished(false, tr("File too large (> 500 MB)"));
        return;
    }

    m_fileName   = QFileInfo(filePath).fileName();
    m_bytesSent  = 0;
    m_uploadPort = -1;
    m_installing = true;
    m_cancelled  = false;

    emit progressChanged(0, tr("Requesting upload port…"));

    // Single command — filename is embedded directly (no separate "file filename" step).
    // Protocol confirmed by pcap 4.2.18-waveform-install.pcapng frame 4496.
    m_model->sendCmdPublic(
        QStringLiteral("file upload %1 waveform_docker_image %2")
            .arg(m_fileData.size()).arg(m_fileName),
        [this](int code, const QString& body) {
            onUploadPortReceived(code, body);
        });
}

void WaveformInstaller::cancel()
{
    if (!m_installing) return;
    m_cancelled  = true;
    m_socket.abort();
    m_installing = false;
    m_fileData.clear();
    emit finished(false, tr("Install cancelled"));
}

void WaveformInstaller::onUploadPortReceived(int code, const QString& body)
{
    if (m_cancelled) return;

    if (code != 0) {
        m_installing = false;
        m_fileData.clear();
        emit finished(false, tr("Radio rejected upload (error 0x%1)").arg(code, 0, 16));
        return;
    }

    bool ok = false;
    int port = body.trimmed().toInt(&ok);
    if (!ok || port <= 0)
        port = FALLBACK_PORT;

    m_uploadPort = port;
    qCDebug(lcWaveform) << "WaveformInstaller: connecting to upload port" << m_uploadPort;
    emit progressChanged(0, tr("Connecting to port %1…").arg(m_uploadPort));

    // Small delay to let the radio set up its TCP server.
    QTimer::singleShot(200, this, [this] {
        if (m_cancelled) return;
        m_socket.connectToHost(m_model->radioAddress(), m_uploadPort);

        // Timeout: try fallback port if no connection after 10 s.
        QTimer::singleShot(10000, this, [this] {
            if (m_installing && m_socket.state() != QAbstractSocket::ConnectedState) {
                if (m_uploadPort != FALLBACK_PORT) {
                    m_uploadPort = FALLBACK_PORT;
                    qCDebug(lcWaveform) << "WaveformInstaller: trying fallback port" << FALLBACK_PORT;
                    emit progressChanged(0, tr("Trying fallback port %1…").arg(FALLBACK_PORT));
                    m_socket.abort();
                    m_socket.connectToHost(m_model->radioAddress(), m_uploadPort);
                } else {
                    m_installing = false;
                    m_fileData.clear();
                    emit finished(false, tr("Cannot connect to upload port"));
                }
            }
        });
    });
}

void WaveformInstaller::onConnected()
{
    if (m_cancelled) return;

    qCDebug(lcWaveform) << "WaveformInstaller: connected, sending" << m_fileData.size() << "bytes";
    emit progressChanged(0, tr("Uploading waveform image…"));

    const qint64 toSend = qMin(static_cast<qint64>(CHUNK_SIZE),
                                m_fileData.size() - m_bytesSent);
    m_socket.write(m_fileData.constData() + m_bytesSent, toSend);
}

void WaveformInstaller::onBytesWritten(qint64 bytes)
{
    if (m_cancelled || !m_installing) return;

    m_bytesSent += bytes;
    const int percent = static_cast<int>(m_bytesSent * 100 / m_fileData.size());
    emit progressChanged(percent,
                         tr("Uploading… %1 / %2 KB")
                             .arg(m_bytesSent / 1024)
                             .arg(m_fileData.size() / 1024));

    if (m_bytesSent >= m_fileData.size()) {
        qCDebug(lcWaveform) << "WaveformInstaller: upload complete";
        m_socket.flush();
        m_socket.disconnectFromHost();
        m_installing = false;
        m_fileData.clear();
        emit progressChanged(100, tr("Upload complete — installing on radio…"));
        emit finished(true, tr("Waveform image uploaded successfully. "
                               "The radio will install it momentarily."));
        return;
    }

    const qint64 toSend = qMin(static_cast<qint64>(CHUNK_SIZE),
                                m_fileData.size() - m_bytesSent);
    m_socket.write(m_fileData.constData() + m_bytesSent, toSend);
}

void WaveformInstaller::onError()
{
    if (m_cancelled || !m_installing) return;

    // First error on the primary port: try the fallback before giving up.
    if (m_bytesSent == 0 && m_uploadPort != FALLBACK_PORT) {
        m_uploadPort = FALLBACK_PORT;
        qCDebug(lcWaveform) << "WaveformInstaller: error on primary port, trying" << FALLBACK_PORT;
        emit progressChanged(0, tr("Retrying on port %1…").arg(FALLBACK_PORT));
        m_socket.abort();
        m_socket.connectToHost(m_model->radioAddress(), m_uploadPort);
        return;
    }

    m_installing = false;
    m_fileData.clear();
    emit finished(false, tr("Upload failed: %1").arg(m_socket.errorString()));
}

} // namespace AetherSDR
