#pragma once

#include <QObject>
#include <QHash>
#include <QQueue>
#include <QString>
#include <functional>
#include <memory>

namespace AetherSDR {

// Serialize operations per account. A forget queued after a save must finish
// after that save, even if an OS credential prompt completes much later.
class SmartLinkCredentials final : public QObject {
    Q_OBJECT
    friend struct SmartLinkCredentialsTestAccess;
public:
    enum class Kind { Read, Write, Remove };
    using Completion = std::function<void(bool, const QString&)>;
    using Execute = std::function<void(Kind, const QString&, const QString&, Completion)>;
    explicit SmartLinkCredentials(QObject* parent = nullptr);
    explicit SmartLinkCredentials(Execute execute, QObject* parent = nullptr);
    void read(const QString& account, Completion completion);
    void write(const QString& account, const QString& token);
    void remove(const QString& account);
    static bool available();
private:
    struct Operation { Kind kind; QString token; Completion completion; quint64 revision; };
    void enqueue(const QString& account, Operation operation);
    struct State {
        Execute execute;
        QHash<QString, QQueue<Operation>> queues;
        QHash<QString, quint64> revisions;
    };
    static void run(const std::shared_ptr<State>& state, const QString& account);
    std::shared_ptr<State> m_state;
};

} // namespace AetherSDR
