#pragma once

#include "ControlCredentialProvisioner.h"
#include "ControlCredentials.h"
#include "ControlPairing.h"

#include <QObject>

#include <deque>
#include <functional>

namespace AetherSDR::control {

// aetherd's paired-client store: the one writer of remote records to the vault.
// - issue(): a paired client's remote-control credential is written to the
//   vault (read-modify-write-readback), then added to the live verifier without
//   retiring any other identity. Only then is the token handed back.
// - revoke(): a remote record is removed from the vault first, then retired
//   live, which closes its sessions.
// Vault operations run one at a time in arrival order, so a revoke never makes
// a pairing whose code was already used fail as busy.
class VaultPairingIssuer final : public QObject, public PairingIssuer, public RemoteClientAdmin {
public:
    VaultPairingIssuer(ControlCredentialVault& vault, ControlCredentials& credentials,
                       QObject* parent = nullptr);
    void issue(const QString& clientName, Done done) override;
    [[nodiscard]] QList<Client> clients() const override;
    void revoke(const QString& credentialId, RevokeDone done) override;

private:
    using Task = std::function<void(std::function<void()> next)>;
    void enqueue(Task task);
    void runNext();

    ControlCredentialProvisioner m_provisioner;
    ControlCredentials& m_credentials;
    std::deque<Task> m_tasks;
    bool m_running{false};
};

} // namespace AetherSDR::control
