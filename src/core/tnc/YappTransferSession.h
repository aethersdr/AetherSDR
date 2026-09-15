#pragma once

#include "core/tnc/YappFileStore.h"
#include <QElapsedTimer>
#include <QJsonObject>
#include <QObject>
#include <QQueue>

namespace AetherSDR {

// WA7MBL YAPP 1.1 + FC1EBN YAPP-C (1992). Qt Core only: the caller owns
// connected-stream admission and pulls output ONLY when its link has capacity.
// No timers, RF side effects, text conversions or whole-file buffering here.
class YappTransferSession : public QObject {
    Q_OBJECT
public:
    explicit YappTransferSession(QObject* parent = nullptr);
    bool startSend(const QString& path, const QString& peer, QString& error);
    bool startReceive(const QString& directory, const QString& peer, bool resume, QString& error);
    bool active() const;
    void receive(const QByteArray& bytes);
    QByteArray takeOutbound();
    QByteArray takeTrailingText();
    void cancel();
    void stop(const QString& reason); // transport gone; emits no protocol bytes
    void checkTimeout(int timeoutMs);
    QJsonObject snapshot() const;
    QString summary() const;
    bool succeeded() const { return m_success; }
    static quint8 checksum(const QByteArray& data);
signals:
    void changed();
    void finished(bool success, const QString& summary);
private:
    enum class State { Idle, SendReady, SendHeader, Sending, SendEof, SendEot,
                       ReceiveInit, ReceiveHeader, Receiving, ReceiveEot, Cancelling, Finishing };
    void initialize(const QString& peer, bool sending);
    void handle(quint8 type, quint8 code, const QByteArray& payload);
    void queue(quint8 type, quint8 code);
    void queuePayload(quint8 type, const QByteArray& payload);
    void fail(const QString& reason);
    void finish(bool success, const QString& reason);
    QString phase() const;
    State m_state{State::Idle};
    QQueue<QByteArray> m_outbound;
    QByteArray m_input;
    QByteArray m_trailing;
    QFile m_source;
    YappFileStore m_store;
    QString m_peer;
    QString m_name;
    QString m_directory;
    QByteArray m_stamp;
    QString m_reason;
    qint64 m_size{0};
    qint64 m_position{0};
    qint64 m_resumeOffset{0};
    qint64 m_elapsedMs{0};
    bool m_sending{false};
    bool m_resume{false};
    bool m_success{false};
    bool m_fileAccepted{false};
    QElapsedTimer m_clock;
    QElapsedTimer m_idle;
};
} // namespace AetherSDR
