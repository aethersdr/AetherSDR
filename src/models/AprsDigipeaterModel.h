#pragma once

#include "core/aprs/AprsFillInDigipeater.h"
#include "core/aprs/AprsBeacon.h"
#include "core/TxCoordinator.h"
#include <QQueue>

namespace AetherSDR {

// Session-local fill-in policy and the modem's bounded shared transmit queue.
// No transport or audio ownership: the UI consumes admitted frames through the
// existing modem TX path. Disarming removes only this producer's traffic.
class AprsDigipeaterModel : public QObject {
    Q_OBJECT
public:
    explicit AprsDigipeaterModel(QObject* parent = nullptr);
    using Stats = AprsFillInDigipeater::Stats;
    struct PendingFrame { QByteArray raw; bool digi{false}; TxCoordinator::Request input; };
    static constexpr int kMaxQueueDepth = 64;
    bool isEnabled() const { return m_enabled; }
    void setEnabled(bool on);
    void setTransmitProgram(const TxCoordinator::Request& input)
    { m_txProgram = input; m_beacon.setTransmitProgram(input); }
    void setBaud(int baud);
    void receiveFrame(const QByteArray& raw);
    void enqueue(const QByteArray& raw, bool digi = false,
                 const TxCoordinator::Request& input = {});
    bool isEmpty() const { return m_queue.isEmpty(); }
    int size() const { return m_queue.size(); }
    void clear() { m_queue.clear(); }
    PendingFrame dequeue() { return m_queue.dequeue(); }
    Stats stats() const { return m_engine.stats(); }
    void setMyAddress(const ax25::Address& address);
    ax25::Address myAddress() const { return m_engine.myAddress(); }
    void setAlias(const ax25::Address& alias) { m_engine.setAlias(alias); }
    ax25::Address alias() const { return m_engine.alias(); }
    void setAlsoMyCall(bool on) { m_engine.setAlsoMyCall(on); }
    bool alsoMyCall() const { return m_engine.alsoMyCall(); }
    void setAlsoRelay(bool on) { m_engine.setAlsoRelay(on); }
    bool alsoRelay() const { return m_engine.alsoRelay(); }
    void setDupeWindowSecs(int secs) { m_engine.setDupeWindowSecs(secs); }
    int dupeWindowSecs() const { return m_engine.dupeWindowSecs(); }
    void setBeaconEnabled(bool on) { m_beacon.setEnabled(on && m_enabled); }
    bool beaconEnabled() const { return m_beacon.isEnabled(); }
    void setBeaconIntervalMinutes(int n) { m_beacon.setIntervalMinutes(n); }
    int beaconIntervalMinutes() const { return m_beacon.intervalMinutes(); }
    void setBeaconSymbol(char table, char code) { m_beacon.setSymbol(table, code); }
    void setBeaconPath(const QVector<ax25::Address>& path) { m_beacon.setPath(path); }
    void setBeaconStatusText(const QString& text) { m_beacon.setStatusText(text); }
    void setGpsPosition(double lat, double lon, bool valid) { m_beacon.setGpsPosition(lat, lon, valid); }
    void setManualPosition(double lat, double lon, bool valid) { m_beacon.setManualPosition(lat, lon, valid); }
    bool sendBeaconNow() { return m_enabled && m_beacon.sendNow(); }
signals:
    void heard(const QString& source, const QString& line);
    void repeated(const QString& line);
    void dropped();
    void queued();
    void disarmed();
    void activity(const QString& line);
private:
    TxCoordinator::Request m_txProgram;
    AprsFillInDigipeater m_engine;
    AprsBeacon m_beacon;
    QQueue<PendingFrame> m_queue;
    bool m_enabled{false};
    int m_baud{300};
};
} // namespace AetherSDR
