#include "ControlCredentialProvisioner.h"

#include <QThread>

#include <utility>

namespace AetherSDR::control {
namespace {
ControlCredentialProvisioner::Result describe(const QList<ControlCredentials::Record>& records)
{
    ControlCredentialProvisioner::Result result;
    result.error = ControlCredentialProvisioner::Error::None;
    for (const auto& record : records) { result.records.append({record.id, record.role}); }
    return result;
}
} // namespace

ControlCredentialProvisioner::ControlCredentialProvisioner(ControlCredentialVault& vault, QObject* parent)
    : QObject(parent), m_vault(&vault)
{
    connect(&vault, &QObject::destroyed, this, [this] {
        if (m_busy) { finish({Error::Storage, ControlCredentialVault::Error::Unavailable, {}}); }
    });
}

void ControlCredentialProvisioner::finish(Result result)
{
    Callback completed = std::move(m_completed);
    m_busy = false;
    if (completed) { completed(std::move(result)); } // may destroy this; no access after
}

void ControlCredentialProvisioner::run(Operation operation, const QString& removeId, Callback completed)
{
    if (!completed) { return; }
    if (QThread::currentThread() != thread() || !m_vault || m_vault->thread() != thread()) {
        completed({Error::Storage, ControlCredentialVault::Error::Unavailable, {}});
        return;
    }
    if (m_busy) { completed({Error::Busy, {}, {}}); return; }
    if ((operation != Operation::Initialize && operation != Operation::AddClient
         && operation != Operation::AddAdmin && operation != Operation::Remove && operation != Operation::List)
        || (operation == Operation::Remove ? !ControlCredentialVault::validAuthorityId(removeId) : !removeId.isEmpty())) {
        completed({Error::InvalidInput, {}, {}});
        return;
    }
    m_busy = true;
    m_completed = std::move(completed);
    const QPointer<ControlCredentialProvisioner> self(this);
    m_vault->load([self, operation, removeId](ControlCredentialVault::Result loaded) {
        if (!self) { return; }
        using VaultError = ControlCredentialVault::Error;
        if (operation == Operation::Initialize && loaded.error == VaultError::None) {
            self->finish({Error::AlreadyExists, {}, {}});
            return;
        }
        if (loaded.error != VaultError::None
            && !(operation == Operation::Initialize && loaded.error == VaultError::NotFound)) {
            self->finish({Error::Storage, loaded.error, {}});
            return;
        }
        if (operation == Operation::List) { self->finish(describe(loaded.records)); return; }
        if (operation == Operation::Remove) {
            const qsizetype removed = loaded.records.removeIf([&](const auto& record) { return record.id == removeId; });
            if (removed != 1) { self->finish({Error::MissingCredential, {}, {}}); return; }
        } else {
            if (loaded.records.size() >= ControlCredentials::kMaximumRecords) {
                self->finish({Error::Capacity, {}, {}});
                return;
            }
            loaded.records.append(ControlCredentials::generate(operation == Operation::AddClient
                ? ControlCredentials::Role::Client : ControlCredentials::Role::GrantAdmin));
        }
        const auto expected = ControlCredentialVault::encode(loaded.records);
        if (!expected) { self->finish({Error::InvalidInput, {}, {}}); return; }
        // A write completion alone is not provisioning success. A denied or
        // mismatching readback reports uncertainty; never roll back blindly
        // over a potentially newer record. The daemon remains disarmed.
        self->m_vault->store(loaded.records, [self, expected = *expected](ControlCredentialVault::Result stored) {
            if (!self) { return; }
            if (stored.error != VaultError::None) { self->finish({Error::Storage, stored.error, {}}); return; }
            self->m_vault->load([self, expected](ControlCredentialVault::Result verified) {
                if (!self) { return; }
                if (verified.error != VaultError::None) { self->finish({Error::Storage, verified.error, {}}); return; }
                if (ControlCredentialVault::encode(verified.records) != std::optional<QByteArray>(expected)) {
                    self->finish({Error::ReadbackMismatch, {}, {}});
                    return;
                }
                self->finish(describe(verified.records));
            });
        });
    });
}

} // namespace AetherSDR::control
