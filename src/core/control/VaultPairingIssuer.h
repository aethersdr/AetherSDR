#pragma once

#include "ControlCredentialProvisioner.h"
#include "ControlCredentials.h"
#include "ControlPairing.h"

#include <QObject>

namespace AetherSDR::control {

// aetherd's PairingIssuer: a paired client's remote-control credential is
// written to the vault (read-modify-write-readback), then added to the live
// verifier without retiring any other identity. Only then is the token
// handed back. One issue at a time; a second concurrent one fails.
class VaultPairingIssuer final : public QObject, public PairingIssuer {
public:
    VaultPairingIssuer(ControlCredentialVault& vault, ControlCredentials& credentials,
                       QObject* parent = nullptr);
    void issue(const QString& clientName, Done done) override;

private:
    ControlCredentialProvisioner m_provisioner;
    ControlCredentials& m_credentials;
    bool m_busy{false};
};

} // namespace AetherSDR::control
