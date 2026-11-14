#pragma once

#include "ControlCredentialVault.h"

#include <QPointer>

#include <optional>

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
        QString name;   // remote roles only
    };
    struct Result {
        Error error{Error::Storage};
        ControlCredentialVault::Error storageError{ControlCredentialVault::Error::None};
        QList<Descriptor> records; // identities and roles only, never secrets
    };
    using Callback = std::function<void(Result)>;
    explicit ControlCredentialProvisioner(ControlCredentialVault& vault, QObject* parent = nullptr);
    void run(Operation operation, const QString& removeId, Callback completed);
    // Adds a record the caller generated (a paired remote client), with the
    // same read-modify-write-readback as run().
    void add(const ControlCredentials::Record& record, Callback completed);

private:
    void start(Operation operation, const QString& removeId,
               std::optional<ControlCredentials::Record> prepared, Callback completed);
    void finish(Result result);
    QPointer<ControlCredentialVault> m_vault;
    Callback m_completed;
    bool m_busy{false};
};

} // namespace AetherSDR::control
