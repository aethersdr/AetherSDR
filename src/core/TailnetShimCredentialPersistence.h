#pragma once

#include <QByteArray>
#include <QString>
#include <functional>
#include <memory>

class QObject;

namespace AetherSDR::tailnetshim {

// Internal application-thread persistence seam. Backend completions run there.
// The primary key keeps its legacy token format;
// the sidecar identifies a committed save, including identical token bytes.
class CredentialPersistence {
public:
    enum class Error { None, Missing, Rejected, Unavailable };
    struct Value {
        Error error{Error::Unavailable};
        QString token;
        QByteArray version;
        QString message;
    };
    struct Raw {
        Error error{Error::Unavailable};
        QString text;
        QString message;
    };
    using RawDone = std::function<void(Raw)>;
    struct Backend {
        std::function<void(const QString&, QObject*, RawDone)> read;
        std::function<void(const QString&, const QString&, QObject*, RawDone)> write;
        std::function<void(const QString&, QObject*, RawDone)> remove;
    };
    using Lease = std::shared_ptr<void>;
    using Acquire = std::function<Lease(const QString&)>;
    using Done = std::function<void(Value)>;

    CredentialPersistence(Backend backend, Acquire acquire, int admissionTimeoutMs = 10000);
    ~CredentialPersistence();
    void read(const QString& key, Done done);
    void write(const QString& key, const QString& token, Done done);
    void retire(const QString& key, const QString& token, const QByteArray& version, Done done);
    static QString versionKey(const QString& key);
    static Acquire systemAuthority();
    static Backend keychainBackend();
    static CredentialPersistence& system();

private:
    class State;
    std::shared_ptr<State> m_state;
};

} // namespace AetherSDR::tailnetshim
