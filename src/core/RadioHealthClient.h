#pragma once

#include <QHostAddress>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>

#include <optional>

#include "RadioHealthPackage.h"

class QTcpSocket;

namespace AetherSDR {

// One reading from the radio health agent (tools/radio-health/agent.c).
struct RadioHealth {
    struct Volume {
        QString path;
        quint64 freeBytes{0};
        quint64 totalBytes{0};
    };
    QString agentVersion;
    int cpuIdlePct{-1};          // -1 until the agent has two CPU samples
    int cpuCores{0};
    qint64 memTotalKb{-1};
    qint64 memAvailableKb{-1};   // -1 when unknown
    bool memEstimated{false};    // no MemAvailable (FLEX-6000 kernel): estimated
    QList<Volume> storage;       // most useful first: container store, then waveforms
};

namespace radiohealth {

// Parses the agent's JSON line; nullopt for anything else.
std::optional<RadioHealth> parse(const QByteArray& line);

// "Storage 824 MB free", or empty when no volume could be read.
QString storageSummary(const RadioHealth& health);
// "Memory 1.7 GB free · CPU 90% idle", leaving out what wasn't measured.
QString resourceSummary(const RadioHealth& health);
// Both, joined.
QString summary(const RadioHealth& health);

// The agent answers only on the radio's local network: a private or
// link-local IPv4 address.
bool isLanIpv4(const QHostAddress& address);

// Writes the agent package that ships inside AetherSDR (RadioHealthPackage)
// to `dir`/AetherHealth.ssdr_waveform for the installer, reusing a matching
// copy already there. Only bytes matching the pin are written: empty, with
// `error` set, otherwise.
QString writeBundledPackage(const QString& dir, QString* error,
                            const QString& resource = QString::fromLatin1(
                                RadioHealthPackage::kResource));

}  // namespace radiohealth

// Reads one line from the agent at the radio's LAN address. Not SmartSDR
// wire: the agent is AetherSDR's own service on the radio.
class RadioHealthClient : public QObject {
    Q_OBJECT
public:
    explicit RadioHealthClient(QObject* parent = nullptr);

    // A new address retires any read in flight.
    void setRadioAddress(const QHostAddress& address);
    void fetch();
    // Retires the read in flight, if any: it ends without a signal.
    void cancel();

signals:
    void received(const AetherSDR::RadioHealth& health);
    void failed(const QString& message);

protected:
    // The end of read `generation`: signals only while that read is current.
    void deliver(const QByteArray& reply, quint64 generation);
    quint64 generation() const { return m_generation; }

private:
    void finish();

    QHostAddress m_address;
    quint64 m_generation{0};       // bumped by cancel(); a read carries its own
    quint64 m_readGeneration{0};
    QPointer<QTcpSocket> m_socket;   // the read in flight, if any
    QByteArray m_buffer;
};

}  // namespace AetherSDR
