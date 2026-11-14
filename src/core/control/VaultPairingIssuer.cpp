#include "VaultPairingIssuer.h"

#include <QLoggingCategory>
#include <QPointer>

#include <utility>

namespace AetherSDR::control {
namespace {
Q_LOGGING_CATEGORY(lcPairing, "aether.control.pairing")
} // namespace

VaultPairingIssuer::VaultPairingIssuer(ControlCredentialVault& vault,
                                       ControlCredentials& credentials, QObject* parent)
    : QObject(parent), m_provisioner(vault), m_credentials(credentials)
{
}

void VaultPairingIssuer::issue(const QString& clientName, Done done)
{
    if (!done) { return; }
    if (m_busy) { done(std::nullopt); return; }
    QByteArray secret;
    ControlCredentials::Record record =
        ControlCredentials::generateRemote(ControlCredentials::Role::RemoteControl, &secret);
    record.name = clientName;
    if (record.id.isEmpty() || !ControlCredentials::validRecords({record})) {
        done(std::nullopt);
        return;
    }
    m_busy = true;
    const QPointer<VaultPairingIssuer> self(this);
    m_provisioner.add(record, [self, record, token = QString::fromLatin1(secret.toHex()),
                               done = std::move(done)](ControlCredentialProvisioner::Result result) {
        if (!self) { done(std::nullopt); return; }
        self->m_busy = false;
        if (result.error != ControlCredentialProvisioner::Error::None) {
            qCWarning(lcPairing) << "pairing: the vault refused the new credential, code"
                                 << int(result.error) << "storage" << int(result.storageError);
            done(std::nullopt);
            return;
        }
        if (!self->m_credentials.add(record)) {
            // Stored but not live: it works after a restart, but this client
            // never gets its token, so the record is an orphan to remove.
            qCWarning(lcPairing) << "pairing: credential" << record.id
                                 << "was stored but could not be enabled; stop aetherd and remove it with --remove-credential";
            done(std::nullopt);
            return;
        }
        qCInfo(lcPairing) << "pairing: paired" << record.name << "as credential" << record.id;
        done(PairingIssuer::Issued{record.id, token});
    });
}

} // namespace AetherSDR::control
