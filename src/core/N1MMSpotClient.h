#pragma once

#include "N1MMSpotParser.h"

#include <QObject>
#include <QUdpSocket>
#include <QFile>
#include <QString>
#include <atomic>

namespace AetherSDR {

// SmartSDR-CAT-compatible N1MMSpot UDP XML listener. Receives contest logger
// bandmap spots (N1MM+, DXLog) pushed as one <spot>...</spot> XML document per
// UDP datagram and emits add/delete events. Unlike the other spot sources,
// N1MM explicitly tells us when a spot should be created/updated vs removed,
// so callers key spots by (dxcall, band) rather than relying on lifetime
// expiry alone — see N1MMSpotParser::spotKey().
class N1MMSpotClient : public QObject {
    Q_OBJECT

public:
    explicit N1MMSpotClient(QObject* parent = nullptr);
    ~N1MMSpotClient() override;

    void startListening(quint16 port);
    void stopListening();
    bool isListening() const { return m_listening; }

    QString logFilePath() const;

public slots:
    // Defer socket construction to the worker thread (#1929) — see
    // SpotCollectorClient::initialize().
    void initialize();

signals:
    void listening();
    void stopped();
    // action == "add": create-or-update the spot keyed by N1MMSpotParser::spotKey(...).
    void spotAdded(const N1mmSpot& spot);
    // action == "delete": remove the spot keyed by spotKey(dxCall, freqMhz).
    void spotDeleted(const QString& dxCall, double freqMhz);
    void rawPacketReceived(const QString& xml);

private slots:
    void onReadyRead();

private:
    QUdpSocket* m_socket{nullptr};
    QFile       m_logFile;
    quint16     m_port{12060};
    std::atomic<bool> m_listening{false};
};

} // namespace AetherSDR
