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
    enum class Role { Client, GrantAdmin };
    struct Record {
        QString id;
        Role role{Role::Client};
        QByteArray secret; // exactly 32 CSPRNG bytes; OS-vault storage only
    };
    class Principal {
    public:
        [[nodiscard]] bool valid() const;
        [[nodiscard]] QString id() const;
        [[nodiscard]] bool canAdministerGrants() const;
    private:
        friend class ControlCredentials;
        friend class ControlSession;
        std::shared_ptr<State> m_state;
        QPointer<ControlCredentials> m_source;
    };
    static constexpr qsizetype kMaximumRecords = 32;
    static constexpr qsizetype kSecretBytes = 32;

    explicit ControlCredentials(QObject* parent = nullptr);
    ~ControlCredentials() override;
    [[nodiscard]] static Record generate(Role role);
    [[nodiscard]] static bool validRecords(const QList<Record>& records);
    // A complete vault reload replaces the verification epoch, including for
    // unchanged records. Invalid input fails closed and retires old identities.
    // Provisioning and durable write success must precede this call.
    [[nodiscard]] bool replace(const QList<Record>& records);
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
