#pragma once

#include "core/control/ControlCredentialProvisioner.h"

#include <QCommandLineParser>

#include <optional>

namespace AetherSDR::aetherd {

struct CredentialOptions {
    QString authorityId;
    std::optional<control::ControlCredentialProvisioner::Operation> operation;
    QString removeId;
    QString error; // fixed diagnostics; never echoes argv or vault records
};

void addCredentialOptions(QCommandLineParser& parser);
// Pre-QCoreApplication dispatcher selection only, never credential validation.
// The normal command-line parser still validates every option before vault use.
[[nodiscard]] bool credentialDispatcherRequested(int argc, const char* const argv[]);
[[nodiscard]] CredentialOptions credentialOptions(const QCommandLineParser& parser);
// Offline, explicit operator action. Caller holds both endpoint and authority
// reservations. Waits for native storage and readback; never constructs a radio.
[[nodiscard]] int provisionCredentials(const CredentialOptions& options, control::ControlCredentialVault& vault);
// Serving only loads existing records. No create, write, grant or key-on here.
[[nodiscard]] bool loadCredentials(control::ControlCredentialVault& vault, control::ControlCredentials& credentials);

} // namespace AetherSDR::aetherd
