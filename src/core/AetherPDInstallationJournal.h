#pragma once

#include <QByteArray>

namespace AetherSDR {
class RadioSettingsScope;

// Public approved plans only. Credentials remain in the per-attempt keychain
// slot. Recovery must supply a publisher key from the authenticated catalogue.
class AetherPDInstallationJournal {
public:
    enum class State { Missing, Pending, CredentialSaved, Invalid, Unavailable };
    struct Read {
        State state{State::Unavailable};
        QByteArray plan;
    };
    static Read read(const RadioSettingsScope& scope, const QByteArray& publisherKey);
    static bool record(const RadioSettingsScope& scope, const QByteArray& plan, const QByteArray& publisherKey);
    static bool complete(const RadioSettingsScope& scope, const QByteArray& plan, const QByteArray& publisherKey);
};
} // namespace AetherSDR
