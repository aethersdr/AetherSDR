#pragma once

#include "ControlSession.h"
#include "TransmitControlTarget.h"

#include <QJsonArray>
#include <QMap>

namespace AetherSDR::control {

// One bounded entry per authenticated client connection. All authority is in
// opaque engine handles; wire IDs select only entries owned by this session.
class TransmitControlService final : public QObject {
public:
    explicit TransmitControlService(TransmitControlTarget* target);
    ~TransmitControlService() override;
    bool attach(ControlSession* session);
    QJsonObject handle(const ProtocolRequest& request, ControlSession* session);
    QJsonArray methods(const ControlSession& session) const;

private:
    struct Client {
        QString id;
        QPointer<ControlSession> session;
        TxGrantManager::Client client;
        TxGrantManager::Grant grant;
        QString grantId;
        TxGrantManager::Admission admission;
        QString operationId;
        bool started{false};
        bool ended{false};
    };
    void retire(const QString& sessionId);
    void invalidateRadio();
    QJsonObject status(const Client& client) const;
    QPointer<TransmitControlTarget> m_target;
    QPointer<TxGrantManager> m_manager;
    QString m_generation;
    QMap<QString, std::shared_ptr<Client>> m_clients;
    bool m_handling{false};
};

} // namespace AetherSDR::control
