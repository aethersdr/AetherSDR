#pragma once

#include <QList>
#include <QString>

#include <functional>
#include <optional>

namespace AetherSDR::control {

// The one open pairing (aetherd-remote-clients.md §3): a one-time code shown on
// the radio's own screen, exchanged once over TLS for a remote-control
// credential. Ten digits: six random, then four check digits derived from the
// six and the listener's certificate fingerprint, so the client can catch a
// man in the middle (another certificate) before it sends anything.
// Pure logic; the caller supplies the clock.
class ControlPairing final {
public:
    static constexpr qint64 kLifetimeMs = 5 * 60 * 1000;
    static constexpr int kMaxAttempts = 5;

    struct Opened {
        QString code;              // "123-456-7890"
        qint64 expiresInMs{0};
        QString certificateSha256; // lowercase hex, 64 characters
    };
    enum class Attempt { Accepted, Wrong, NotOpen, Expired };

    // Replaces any open pairing.
    [[nodiscard]] Opened begin(const QString& certificateSha256, qint64 nowMs);
    // Accepted closes the pairing; so do expiry and the last wrong attempt.
    [[nodiscard]] Attempt attempt(const QString& code, qint64 nowMs);
    void close() { m_open = false; }
    [[nodiscard]] bool isOpen(qint64 nowMs) const { return m_open && nowMs < m_expiresAtMs; }

    // The four check digits for six digits and a fingerprint: the first four
    // bytes of SHA-256(six ‖ fingerprint hex) as a big-endian integer, mod 10 000.
    [[nodiscard]] static QString checkDigits(const QString& six, const QString& certificateSha256);
    // "123-456-7890", "123 456 7890" and "1234567890" all read as ten digits;
    // nullopt for anything else.
    [[nodiscard]] static std::optional<QString> digitsOf(const QString& code);
    [[nodiscard]] static QString formatted(const QString& tenDigits);

    // Tests only: the six random digits come from here when set.
    std::function<QString()> randomSixForTest;

private:
    QString m_six;
    QString m_fingerprint;
    qint64 m_expiresAtMs{0};
    int m_attemptsLeft{0};
    bool m_open{false};
};

// Mints and durably stores a remote-control credential for a paired client
// (aetherd: the vault plus the live verifier). Completes asynchronously on the
// caller's thread; nullopt when the credential could not be stored.
class PairingIssuer {
public:
    struct Issued {
        QString credentialId;
        QString token;   // 64 lowercase hex; returned to the client once
    };
    using Done = std::function<void(std::optional<Issued>)>;
    virtual ~PairingIssuer() = default;
    virtual void issue(const QString& clientName, Done done) = 0;
};

// Paired clients at run time (aetherd-remote-clients.md §3): the radio's own
// screen lists them and removes one without stopping the engine. Only remote
// scopes are listed or removable here; local identities never are.
class RemoteClientAdmin {
public:
    struct Client {
        QString credentialId;
        QString name;
        QString scope;   // "remote-observe" or "remote-control"
    };
    enum class RevokeResult { Revoked, NotFound, Failed };
    using RevokeDone = std::function<void(RevokeResult)>;
    virtual ~RemoteClientAdmin() = default;
    [[nodiscard]] virtual QList<Client> clients() const = 0;
    // Removes it from durable storage first, then ends it live (closing its
    // sessions), so a failure never shows as gone a client a restart restores.
    // Completes on the caller's thread, possibly before revoke() returns.
    virtual void revoke(const QString& credentialId, RevokeDone done) = 0;
};

} // namespace AetherSDR::control
