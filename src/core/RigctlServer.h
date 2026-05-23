#pragma once

#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QList>

namespace MasterSDR {

class RadioModel;
class RigctlProtocol;

// TCP server implementing the Hamlib rigctld protocol.
// Supports multiple concurrent clients, each with their own protocol state.
class RigctlServer : public QObject {
    Q_OBJECT

public:
    explicit RigctlServer(RadioModel* model, QObject* parent = nullptr);
    ~RigctlServer() override;

    bool start(quint16 port = 4532);
    void stop();

    bool isRunning() const;
    quint16 port() const;
    int clientCount() const { return m_clients.size(); }

    // Which slice index this server's connections will control.
    void setSliceIndex(int idx) { m_sliceIndex = idx; }
    int  sliceIndex() const     { return m_sliceIndex; }

signals:
    void clientCountChanged(int count);

private slots:
    void onNewConnection();
    void onClientData();
    void onClientDisconnected();

private:
    struct ClientState {
        QTcpSocket*     socket{nullptr};
        RigctlProtocol* protocol{nullptr};
        QByteArray      buffer;         // line accumulation buffer
    };

    RadioModel*      m_model;
    QTcpServer*      m_server{nullptr};
    QList<ClientState> m_clients;
    int              m_sliceIndex{0};
};

} // namespace MasterSDR
