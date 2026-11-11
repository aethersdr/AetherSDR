#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVector>

#include <optional>

namespace AetherSDR::kenwood {
inline constexpr qint64 kMinimumFrequencyHz = 30000;
inline constexpr qint64 kDefaultMaximumFrequencyHz = 60000000;
inline constexpr qint64 kEuropeanMaximumFrequencyHz = 74800000;
std::optional<qint64> maximumFrequencyForVariant(const QString& variant);

// Transport-free KNS framing and authentication. The owner feeds only the
// current socket's bytes and monotonic milliseconds; reset retires its tail.
class KnsProtocol final {
public:
    enum class State { Closed, Greeting, Authenticating, Ready, Failed };
    struct Events {
        QList<QByteArray> outgoing;
        QList<QByteArray> messages;
        QString error;
        bool authenticated = false;
    };
    static constexpr qsizetype kMaximumMessageBytes = 4096;
    static bool credentialsValid(const QString& username, const QString& password);
    Events start(const QString& username, const QString& password, bool administrator,
                 qint64 nowMs);
    Events receive(const QByteArray& bytes, qint64 nowMs);
    Events tick(qint64 nowMs);
    void reset();
    State state() const { return m_state; }
    static std::optional<QByteArray> command(const QByteArray& body);

private:
    void fail(Events& events, const QString& error);
    State m_state = State::Closed;
    QByteArray m_tail;
    QByteArray m_login;
    qint64 m_deadlineMs = 0;
    qint64 m_keepaliveMs = 0;
    qint64 m_lastReceiveMs = 0;
};

struct ScopeLine {
    QVector<float> levels;
};
std::optional<ScopeLine> decodeScope(const QByteArray& message);
std::optional<qint64> decodeFrequency(const QByteArray& message);
std::optional<QByteArray> encodeFrequency(char vfo, qint64 hz);
std::optional<QString> decodeMode(char code);
std::optional<char> encodeMode(const QString& mode);
struct VfoState { int vfo; qint64 frequencyHz; QString mode; };
std::optional<VfoState> decodeVfoState(const QByteArray& message);
std::optional<bool> decodeStatusTransmit(const QByteArray& message);

// MS configures the complete front/rear pair separately for SEND and DATA SEND.
// The reset sentinel and the invalid all-off pair are deliberately excluded.
struct TransmitAudioInput { int path; int selection; };
std::optional<TransmitAudioInput> decodeTransmitAudioInput(const QByteArray& message);
std::optional<QByteArray> encodeTransmitAudioInput(const TransmitAudioInput& input);
QStringList transmitAudioInputChoices();

// Audio framing follows the public Open890 references; see the design's
// provenance and hardware checklist. RX and TX have different sample encodings.
struct AudioPacket {
    quint16 sequence = 0;
    quint32 timestamp = 0;
    quint32 source = 0;
    QVector<float> samples;
};
std::optional<AudioPacket> decodeAudio(const QByteArray& packet);
std::optional<QByteArray> encodeTxAudio(const QVector<float>& samples,
                                      quint16 sequence, float gain);

class AudioSequence final {
public:
    enum class Result { First, Continuous, Gap, Restart, Drop };
    Result accept(quint16 sequence, quint32 source, qint64 nowMs);
    void reset();
private:
    bool m_seen = false;
    quint16 m_next = 0;
    quint32 m_source = 0;
    qint64 m_lastMs = 0;
    quint16 m_restartNext = 0;
    int m_restartPackets = 0;
};
} // namespace AetherSDR::kenwood
