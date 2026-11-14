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

void VaultPairingIssuer::enqueue(Task task)
{
    m_tasks.push_back(std::move(task));
    if (!m_running) { runNext(); }
}

void VaultPairingIssuer::runNext()
{
    if (m_tasks.empty()) { m_running = false; return; }
    m_running = true;
    Task task = std::move(m_tasks.front());
    m_tasks.pop_front();
    const QPointer<VaultPairingIssuer> self(this);
    // next() may run inside task() (an inline refusal); the queue survives that.
    task([self] { if (self) { self->runNext(); } });
}

void VaultPairingIssuer::issue(const QString& clientName, Done done)
{
    if (!done) { return; }
    QByteArray secret;
    ControlCredentials::Record record =
        ControlCredentials::generateRemote(ControlCredentials::Role::RemoteControl, &secret);
    record.name = clientName;
    if (record.id.isEmpty() || !ControlCredentials::validRecords({record})) {
        done(std::nullopt);
        return;
    }
    const QPointer<VaultPairingIssuer> self(this);
    enqueue([self, record, token = QString::fromLatin1(secret.toHex()),
             done = std::move(done)](std::function<void()> next) mutable {
        if (!self) { done(std::nullopt); return; }
        self->m_provisioner.add(record, [self, record, token, done = std::move(done), next]
                                        (ControlCredentialProvisioner::Result result) {
            if (!self) { done(std::nullopt); return; }
            if (result.error != ControlCredentialProvisioner::Error::None) {
                qCWarning(lcPairing) << "pairing: the vault refused the new credential, code"
                                     << int(result.error) << "storage" << int(result.storageError);
                done(std::nullopt);
            } else if (!self->m_credentials.add(record)) {
                // Stored but not live: it works after a restart, but this client
                // never gets its token, so the record is an orphan to remove.
                qCWarning(lcPairing) << "pairing: credential" << record.id
                                     << "was stored but could not be enabled; it appears in the radio's paired clients after aetherd restarts, remove it there";
                done(std::nullopt);
            } else {
                qCInfo(lcPairing) << "pairing: paired" << record.name << "as credential" << record.id;
                done(PairingIssuer::Issued{record.id, token});
            }
            next();
        });
    });
}

QList<RemoteClientAdmin::Client> VaultPairingIssuer::clients() const
{
    QList<Client> out;
    for (const auto& r : m_credentials.remoteRecords()) {
        out.append({r.id, r.name, r.role == ControlCredentials::Role::RemoteObserve
                                      ? QStringLiteral("remote-observe") : QStringLiteral("remote-control")});
    }
    return out;
}

void VaultPairingIssuer::revoke(const QString& credentialId, RevokeDone done)
{
    if (!done) { return; }
    const QPointer<VaultPairingIssuer> self(this);
    enqueue([self, credentialId, done = std::move(done)](std::function<void()> next) mutable {
        if (!self) { done(RevokeResult::Failed); return; }
        // Only a live remote record: a local identity (a TX client or the
        // grant-admin) is never removable from here, and reads as unknown.
        bool remote = false;
        for (const auto& r : self->m_credentials.remoteRecords()) { remote |= r.id == credentialId; }
        if (!remote) { done(RevokeResult::NotFound); next(); return; }
        self->m_provisioner.run(ControlCredentialProvisioner::Operation::Remove, credentialId,
            [self, credentialId, done = std::move(done), next](ControlCredentialProvisioner::Result result) {
                using Error = ControlCredentialProvisioner::Error;
                if (!self) { done(RevokeResult::Failed); return; }
                // Already absent from the vault is gone after a restart too.
                if (result.error == Error::None || result.error == Error::MissingCredential) {
                    (void)self->m_credentials.revoke(credentialId);   // closes its sessions
                    qCInfo(lcPairing) << "pairing: removed paired client" << credentialId;
                    done(RevokeResult::Revoked);
                } else {
                    qCWarning(lcPairing) << "pairing: the vault refused removing" << credentialId
                                         << "code" << int(result.error) << "storage" << int(result.storageError);
                    done(RevokeResult::Failed);
                }
                next();
            });
    });
}

} // namespace AetherSDR::control
