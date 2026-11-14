#pragma once

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QJsonValue>
#include <QString>

#include <atomic>
#include <memory>

namespace AetherSDR::control {

// Trusted startup/operator composition, never a wire credential-management API.
// Records must come from the OS vault. This verifier persists nothing and keeps
// only token digests; it cannot create an armed grant or enable daemon TX.
class ControlCredentials final : public QObject {
    Q_OBJECT
    struct State;
public:
    // Client and GrantAdmin are local-socket identities. The two remote scopes
    // are network clients (aetherd-remote-clients.md); on the network the
    // scope IS the grant: observe, or observe and control. Never transmit.
    enum class Role { Client, GrantAdmin, RemoteObserve, RemoteControl };
    [[nodiscard]] static bool isRemote(Role role) noexcept
    {
        return role == Role::RemoteObserve || role == Role::RemoteControl;
    }
    struct Record {
        QString id;
        Role role{Role::Client};
        // Local roles: exactly 32 CSPRNG bytes, OS-vault storage only. Remote
        // roles: the SHA-256 digest of the client's token instead. The daemon
        // never needs a remote secret back, so it never keeps one.
        QByteArray secret;
        // Remote roles only: the name the client gave at pairing, at most
        // kMaximumNameBytes of UTF-8 with no control characters. Empty for a
        // local role, which has no name.
        QString name;
    };
    class Principal {
    public:
        [[nodiscard]] bool valid() const;
        [[nodiscard]] QString id() const;
        [[nodiscard]] bool canAdministerGrants() const;
        // The credential's scope; meaningful only while valid().
        [[nodiscard]] Role role() const;
    private:
        friend class ControlCredentials;
        friend class ControlSession;
        std::shared_ptr<State> m_state;
        QPointer<ControlCredentials> m_source;
    };
    static constexpr qsizetype kMaximumRecords = 32;
    static constexpr qsizetype kSecretBytes = 32;
    static constexpr qsizetype kMaximumNameBytes = 32;
    [[nodiscard]] static bool validName(const QString& name);

    explicit ControlCredentials(QObject* parent = nullptr);
    ~ControlCredentials() override;
    [[nodiscard]] static Record generate(Role role);
    // A remote-scope record and the token its holder presents (32 CSPRNG
    // bytes, hex-encoded on the wire). The record keeps only the digest.
    [[nodiscard]] static Record generateRemote(Role role, QByteArray* token);
    [[nodiscard]] static bool validRecords(const QList<Record>& records);
    // A complete vault reload replaces the verification epoch, including for
    // unchanged records. Invalid input fails closed and retires old identities.
    // Provisioning and durable write success must precede this call.
    [[nodiscard]] bool replace(const QList<Record>& records);
    // Adds one record, already durably stored, without retiring any other
    // identity (a reload would end every live session). False when the set
    // with it would be invalid.
    [[nodiscard]] bool add(const Record& record);
    [[nodiscard]] bool revoke(const QString& id);
    void clear();
    [[nodiscard]] Principal verify(const QJsonValue& auth) const;
    [[nodiscard]] qsizetype size() const;

signals:
    // Every affected handle is already fenced before this synchronous signal.
    // A listener must inspect its own principal, not retire unrelated sessions.
    void changed();

private:
    [[nodiscard]] bool onThread() const;
    QList<std::shared_ptr<State>> m_records;
    bool m_changing{false};
    bool m_closing{false};
};

} // namespace AetherSDR::control
