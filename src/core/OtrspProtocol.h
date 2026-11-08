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
    enum class Focus { None, Receive, Transmit };

    QByteArray drainCommands();
    quint64 beginFocus(Focus focus, const QByteArray& command, SliceModel* target, int number);
    void finishFocus(quint64 revision);
    void observeFocus();
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
    QTimer m_focusDeadline;
    QPointer<SliceModel> m_focusTarget;
    QList<QMetaObject::Connection> m_focusConnections;
    quint64 m_focusRevision{0};
    int m_focusNumber{0};
    Focus m_waitingFor{Focus::None};
    QByteArray m_expectedFocus;
    bool m_processing{false};
    bool m_discarding{false};
    bool m_closed{false};
};

} // namespace AetherSDR
