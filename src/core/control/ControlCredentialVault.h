#pragma once

#include "ControlCredentials.h"

#include <functional>
#include <optional>

namespace AetherSDR::control {

// One trusted owner per authority namespace. Provisioning must serialize edits
// and hold the daemon endpoint reservation; this adapter is not a multi-writer
// database. It is never constructed from client-supplied namespace/record data.
class ControlCredentialVault final : public QObject {
public:
    enum class Error { None, Unavailable, NotFound, Storage, InvalidData, Busy, WrongThread };
    struct Result {
        Error error{Error::Storage};
        QList<ControlCredentials::Record> records;
    };
    using Callback = std::function<void(Result)>;
    explicit ControlCredentialVault(QString authorityId, QObject* parent = nullptr);
    // Async OS-vault jobs only. No QSettings, files, argv, logs or plaintext
    // fallback. Destruction drops callbacks; an already submitted native write
    // may still finish. A provisioner must wait for the result and read it back.
    void load(Callback completed);
    void store(const QList<ControlCredentials::Record>& records, Callback completed);

    // Canonical private storage format, not a client protocol or export format.
    // Fixed-width records avoid attacker-controlled lengths/nesting/duplicates.
    [[nodiscard]] static std::optional<QByteArray> encode(const QList<ControlCredentials::Record>& records);
    [[nodiscard]] static std::optional<QList<ControlCredentials::Record>> decode(const QByteArray& bytes);
    [[nodiscard]] static bool validAuthorityId(const QString& id);

private:
    void start(bool writing, const QByteArray& bytes, Callback completed);
    const QString m_authorityId;
    bool m_busy{false};
};

} // namespace AetherSDR::control
