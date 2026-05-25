#pragma once

#include "SmartCatProtocol.h"

#include <QObject>
#include <QTcpSocket>
#include <QByteArray>

namespace AetherSDR {

class RadioModel;
class SliceModel;

// Per-connection TCP session for the SmartSDR CAT protocol.
//
// Wraps SmartCatProtocol (pure request/response) with TCP socket I/O and
// AI mode — the async push of FA/MD updates when slice state changes.
//
// The flexExtensions flag selects whether ZZ-prefixed commands are accepted;
// false = plain Kenwood TS-2000 dialect, true = FlexRadio CAT (default).
class SmartCatSession : public QObject {
    Q_OBJECT

public:
    explicit SmartCatSession(QTcpSocket* socket, RadioModel* model,
                             int vfoA = 0, int vfoB = -1,
                             bool flexExtensions = true,
                             QObject* parent = nullptr);
    ~SmartCatSession() override;

signals:
    void sessionEnded(SmartCatSession* session);

private slots:
    void onReadyRead();
    void onDisconnected();
    void onVfoAFrequencyChanged(double mhz);
    void onVfoAModeChanged(const QString& mode);

private:
    void setAI(bool on);
    void send(const QString& data);

    SliceModel* vfoA() const;

    SmartCatProtocol m_protocol;
    QTcpSocket*      m_socket;
    RadioModel*      m_model;
    QByteArray       m_buffer;
    bool             m_aiEnabled{false};
};

} // namespace AetherSDR
