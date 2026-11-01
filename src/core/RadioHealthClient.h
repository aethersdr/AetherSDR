#pragma once

#include <QHostAddress>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>

#include <optional>

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

// "Storage 824 MB free · Memory 1.7 GB free · CPU 90% idle", leaving out
// what the agent couldn't measure.
QString summary(const RadioHealth& health);

}  // namespace radiohealth

// Reads one line from the agent at the radio's LAN address. Not SmartSDR
// wire: the agent is AetherSDR's own service on the radio.
class RadioHealthClient : public QObject {
    Q_OBJECT
public:
    explicit RadioHealthClient(QObject* parent = nullptr);

    void setRadioAddress(const QHostAddress& address) { m_address = address; }
    void fetch();

signals:
    void received(const AetherSDR::RadioHealth& health);
    void failed(const QString& message);

private:
    void finish();

    QHostAddress m_address;
    QPointer<QTcpSocket> m_socket;   // the read in flight, if any
    QByteArray m_buffer;
};

}  // namespace AetherSDR
