#include "KiwiSdrCredentialStore.h"

#include <QObject>
#include <QHash>
#include <QPointer>
#include <QQueue>
#include <QSet>
#include <QTimer>

#ifdef HAVE_KEYCHAIN
#include <qt6keychain/keychain.h>
#endif

#include <utility>

namespace AetherSDR {
namespace {

#ifdef HAVE_KEYCHAIN
constexpr const char* kKeychainService = "AetherSDR";

KiwiSdrCredentialResult resultForJob(QKeychain::Job* job)
{
    if (job->error() == QKeychain::NoError) {
        return {};
    }
    if (job->error() == QKeychain::EntryNotFound) {
        return {KiwiSdrCredentialResultCode::NotFound, {}, {}};
    }
    return {KiwiSdrCredentialResultCode::Error, {}, job->errorString()};
}

class KeychainKiwiSdrCredentialStore final
    : public IKiwiSdrCredentialStore,
      public std::enable_shared_from_this<KeychainKiwiSdrCredentialStore> {
public:
    bool isPersistent() const override { return true; }

    void read(const QString& key, QObject* context,
              Callback callback) override
    {
        enqueue({Operation::Read, key, {}, context, std::move(callback)});
    }

    void write(const QString& key, const QString& value, QObject* context,
               Callback callback) override
    {
        enqueue({Operation::Write, key, value, context, std::move(callback)});
    }

    void remove(const QString& key, QObject* context,
                Callback callback) override
    {
        enqueue({Operation::Remove, key, {}, context, std::move(callback)});
    }

private:
    enum class Operation { Read, Write, Remove };
    struct Request {
        Operation operation;
        QString key;
        QString value;
        QPointer<QObject> context;
        Callback callback;
    };

    void enqueue(Request request)
    {
        const QString key = request.key;
        m_queues[key].enqueue(std::move(request));
        startNext(key);
    }

    void startNext(const QString& key)
    {
        if (m_activeKeys.contains(key) || m_queues.value(key).isEmpty()) {
            return;
        }

        Request request = m_queues[key].dequeue();
        if (m_queues[key].isEmpty()) {
            m_queues.remove(key);
        }
        m_activeKeys.insert(key);

        QKeychain::Job* job = nullptr;
        if (request.operation == Operation::Read) {
            auto* readJob = new QKeychain::ReadPasswordJob(
                QString::fromLatin1(kKeychainService));
            job = readJob;
        } else if (request.operation == Operation::Write) {
            auto* writeJob = new QKeychain::WritePasswordJob(
                QString::fromLatin1(kKeychainService));
            writeJob->setTextData(request.value);
            job = writeJob;
        } else {
            job = new QKeychain::DeletePasswordJob(
                QString::fromLatin1(kKeychainService));
        }
        job->setKey(key);
        // Auto-delete the job after it finishes, matching every other QKeychain
        // user in the tree (SmartLinkClient, CallsignLookupService,
        // AutomationBridgeSettings). Otherwise the job leaks — and because the
        // finished handler captures a shared_ptr to this store, a leaked job
        // would pin the whole credential store alive for the process lifetime.
        job->setAutoDelete(true);

        const std::shared_ptr<KeychainKiwiSdrCredentialStore> self =
            shared_from_this();
        QObject::connect(
            job, &QKeychain::Job::finished, job,
            [self, key, request = std::move(request)](
                QKeychain::Job* finished) mutable {
                KiwiSdrCredentialResult result = resultForJob(finished);
                if (request.operation == Operation::Read
                    && result.code == KiwiSdrCredentialResultCode::Success) {
                    result.value = static_cast<QKeychain::ReadPasswordJob*>(
                                       finished)->textData();
                }
                if (request.context) {
                    request.callback(result);
                }
                self->m_activeKeys.remove(key);
                self->startNext(key);
            });
        job->start();
    }

    QHash<QString, QQueue<Request>> m_queues;
    QSet<QString> m_activeKeys;
};

#else

class SessionKiwiSdrCredentialStore final
    : public IKiwiSdrCredentialStore {
public:
    bool isPersistent() const override { return false; }

    void read(const QString&, QObject* context, Callback callback) override
    {
        QTimer::singleShot(0, context,
                           [callback = std::move(callback)] {
            callback({KiwiSdrCredentialResultCode::NotFound, {}, {}});
        });
    }

    void write(const QString&, const QString&, QObject* context,
               Callback callback) override
    {
        complete(context, std::move(callback));
    }

    void remove(const QString&, QObject* context,
                Callback callback) override
    {
        complete(context, std::move(callback));
    }

private:
    static void complete(QObject* context, Callback callback)
    {
        QTimer::singleShot(0, context,
                           [callback = std::move(callback)] {
            callback({});
        });
    }
};

#endif

} // namespace

std::shared_ptr<IKiwiSdrCredentialStore>
createDefaultKiwiSdrCredentialStore()
{
#ifdef HAVE_KEYCHAIN
    return std::make_shared<KeychainKiwiSdrCredentialStore>();
#else
    return std::make_shared<SessionKiwiSdrCredentialStore>();
#endif
}

} // namespace AetherSDR
