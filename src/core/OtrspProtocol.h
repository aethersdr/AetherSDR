#pragma once

#include <QByteArray>
#include <QList>
#include <QPointer>
#include <QObject>
#include <QQueue>
#include <QTimer>

#include <memory>

namespace AetherSDR {

class RadioModel;
class SliceModel;

// Focus subset of K1XM's OTRSP v0.9: https://www.k1xm.org/OTRSP/
// Each transport/client owns its framing buffer; focus itself belongs to the radio.
class OtrspProtocol : public QObject, public std::enable_shared_from_this<OtrspProtocol> {
    Q_OBJECT

public:
    OtrspProtocol(RadioModel* model, int radio1Slice, int radio2Slice);

    QByteArray processData(const QByteArray& data);
    void close();

signals:
    void responseReady(const QByteArray& response);
    void transportClosed();

private:
    QByteArray drainCommands();
    void finishTx(quint64 revision);
    void observeTx();
    void cancelPending();
    QByteArray processCommand(const QByteArray& command);
    SliceModel* radioSlice(int number) const;
    QByteArray receiveFocus() const;
    QByteArray transmitFocus() const;

    QPointer<RadioModel> m_model;
    int m_radio1Slice;
    int m_radio2Slice;
    QByteArray m_buffer;
    QQueue<QByteArray> m_commands;
    qsizetype m_queuedBytes{0};
    QTimer m_txDeadline;
    QPointer<SliceModel> m_txTarget;
    QList<QMetaObject::Connection> m_txConnections;
    quint64 m_txRevision{0};
    int m_txNumber{0};
    bool m_waitingForTx{false};
    bool m_processing{false};
    bool m_discarding{false};
    bool m_closed{false};
};

} // namespace AetherSDR
