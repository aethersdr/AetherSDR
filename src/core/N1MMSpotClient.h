#pragma once

#include "N1MMSpotParser.h"

#include <QObject>
#include <QUdpSocket>
#include <QFile>
#include <QSet>
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
    // The port actually bound, which is not necessarily what the tab's
    // spinbox currently reads — the operator can spin it without restarting.
    quint16 port() const { return m_port; }

    QString logFilePath() const;

public slots:
    // Defer socket construction to the worker thread (#1929) — see
    // SpotCollectorClient::initialize().
    void initialize();

signals:
    void listening();
    void stopped();
    // Bind failed — port already taken by SmartSDR CAT or another N1MM
    // consumer is the common case, and without this the Start button would
    // just silently stay "Start" with only a log line to explain it.
    void bindFailed(const QString& error);
    // action == "add": create-or-update the spot keyed by N1MMSpotParser::spotKey(...).
    void spotAdded(const N1mmSpot& spot);
    // action == "delete": remove the spot keyed by spotKey(dxCall, freqMhz).
    void spotDeleted(const QString& dxCall, double freqMhz);
    // One compact console line per accepted spot, matching the other SpotHub
    // clients. Deliberately not the raw XML: measured against a live contest
    // feed, 333 spot datagrams produced 4146 lines of XML in ~15 minutes,
    // turning the console's 2000-block buffer over twice. Full documents go
    // to the log file, which is what it's for.
    void rawLineReceived(const QString& line);

private slots:
    void onReadyRead();

private:
    QUdpSocket* m_socket{nullptr};
    QFile       m_logFile;
    quint16     m_port{12060};
    std::atomic<bool> m_listening{false};
    // Root elements of non-spot documents already noted this session, so the
    // console says "RadioInfo is arriving here" once instead of every packet.
    QSet<QString> m_notedNonSpotRoots;
};

} // namespace AetherSDR
