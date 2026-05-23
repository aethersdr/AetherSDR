#pragma once

#include <QObject>
#include <QUdpSocket>
#include <QFile>
#include <QString>
#include <atomic>
#include "DxClusterClient.h"  // for DxSpot

namespace MasterSDR {

// DXLab SpotCollector UDP listener — receives spot push packets
// in standard "DX de" cluster format on a configurable UDP port (default 9999).
class SpotCollectorClient : public QObject {
    Q_OBJECT

public:
    explicit SpotCollectorClient(QObject* parent = nullptr);
    ~SpotCollectorClient() override;

    void startListening(quint16 port);
    void stopListening();
    bool isListening() const { return m_listening; }

    QString logFilePath() const;

public slots:
    // Defer socket construction to the worker thread (#1929) — see DxClusterClient::initialize().
    void initialize();

signals:
    void listening();
    void stopped();
    void spotReceived(const DxSpot& spot);
    void rawLineReceived(const QString& line);

private slots:
    void onReadyRead();

private:
    bool parseDxSpotLine(const QString& line, DxSpot& spot) const;

    QUdpSocket* m_socket{nullptr};
    QFile       m_logFile;
    quint16     m_port{9999};
    std::atomic<bool> m_listening{false};
};

} // namespace MasterSDR
