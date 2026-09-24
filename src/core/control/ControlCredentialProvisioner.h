#pragma once

#include "ControlCredentialVault.h"

#include <QPointer>

namespace AetherSDR::control {

// Explicit, offline operator actions only; never called by startup restoration
// or a protocol request. One owner holds the authority namespace reservation
// through read-modify-write-readback. This object neither arms nor authenticates.
class ControlCredentialProvisioner final : public QObject {
public:
    enum class Operation { Initialize, AddClient, AddAdmin, Remove, List };
    enum class Error { None, Busy, InvalidInput, AlreadyExists, MissingCredential,
                       Capacity, Storage, ReadbackMismatch };
    struct Descriptor {
        QString id;
        ControlCredentials::Role role;
    };
    struct Result {
        Error error{Error::Storage};
        ControlCredentialVault::Error storageError{ControlCredentialVault::Error::None};
        QList<Descriptor> records; // identities and roles only, never secrets
    };
    using Callback = std::function<void(Result)>;
    explicit ControlCredentialProvisioner(ControlCredentialVault& vault, QObject* parent = nullptr);
    void run(Operation operation, const QString& removeId, Callback completed);

private:
    void finish(Result result);
    QPointer<ControlCredentialVault> m_vault;
    Callback m_completed;
    bool m_busy{false};
};

} // namespace AetherSDR::control
