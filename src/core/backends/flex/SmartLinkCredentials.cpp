#include "SmartLinkCredentials.h"
#include "core/SettingsCredentialPolicy.h"
#include "core/LogManager.h"

#include <QPointer>
#include <utility>
#ifdef HAVE_KEYCHAIN
#include <qt6keychain/keychain.h>
#endif

namespace AetherSDR {
namespace {
void executeCredential(SmartLinkCredentials::Kind kind, const QString& account,
                       const QString& token, SmartLinkCredentials::Completion complete)
{
#ifdef HAVE_KEYCHAIN
    using Kind = SmartLinkCredentials::Kind;
    QKeychain::Job* job = nullptr;
    if (kind == Kind::Read) {
        job = new QKeychain::ReadPasswordJob(QStringLiteral("AetherSDR"));
    } else if (kind == Kind::Write) {
        auto* write = new QKeychain::WritePasswordJob(QStringLiteral("AetherSDR"));
        write->setTextData(token);
        job = write;
    } else {
        job = new QKeychain::DeletePasswordJob(QStringLiteral("AetherSDR"));
    }
    job->setAutoDelete(true);
    job->setKey(SettingsCredentialPolicy::smartLinkCredentialKey(account));
    job->setInsecureFallback(false);
    QObject::connect(job, &QKeychain::Job::finished, job,
        [kind, complete = std::move(complete)](QKeychain::Job* result) {
            const bool ok = result->error() == QKeychain::NoError
                || (kind == Kind::Remove && result->error() == QKeychain::EntryNotFound);
            if (!ok && result->error() != QKeychain::EntryNotFound) {
                qCWarning(lcSmartLink) << "SmartLink credential operation failed:" << result->errorString();
            }
            const QString value = ok && kind == Kind::Read
                ? static_cast<QKeychain::ReadPasswordJob*>(result)->textData() : QString();
            complete(ok, value);
        });
    job->start();
#else
    Q_UNUSED(kind)
    Q_UNUSED(account)
    Q_UNUSED(token)
    complete(false, {});
#endif
}
}

SmartLinkCredentials::SmartLinkCredentials(QObject* parent)
    : SmartLinkCredentials(executeCredential, parent) {}
SmartLinkCredentials::SmartLinkCredentials(Execute execute, QObject* parent)
    : QObject(parent), m_state(std::make_shared<State>()) { m_state->execute = std::move(execute); }

bool SmartLinkCredentials::available()
{
#ifdef HAVE_KEYCHAIN
    return true;
#else
    return false;
#endif
}

void SmartLinkCredentials::read(const QString& account, Completion complete)
{
    const QPointer<SmartLinkCredentials> alive(this);
    enqueue(account, {Kind::Read, {}, [alive, complete = std::move(complete)](bool ok, const QString& token) {
        if (alive && complete) { complete(ok, token); }
    }, m_state->revisions.value(account)});
}
void SmartLinkCredentials::write(const QString& account, const QString& token)
{
    enqueue(account, {Kind::Write, token, {}, ++m_state->revisions[account]});
}
void SmartLinkCredentials::remove(const QString& account)
{
    enqueue(account, {Kind::Remove, {}, {}, ++m_state->revisions[account]});
}
void SmartLinkCredentials::enqueue(const QString& account, Operation operation)
{
    QQueue<Operation>& queue = m_state->queues[account];
    queue.enqueue(std::move(operation));
    if (queue.size() == 1) { run(m_state, account); }
}
void SmartLinkCredentials::run(const std::shared_ptr<State>& state, const QString& account)
{
    const Operation operation = state->queues.value(account).head();
    // Jobs keep the queue alive across account/model destruction. In particular,
    // a pending save cannot survive its already-requested forget operation.
    state->execute(operation.kind, account, operation.token,
        [state, account, operation](bool ok, const QString& token) {
            const bool current = operation.revision == state->revisions.value(account);
            if (operation.completion) { operation.completion(ok && current, current ? token : QString()); }
            QQueue<Operation>& queue = state->queues[account];
            queue.dequeue();
            if (queue.isEmpty()) { state->queues.remove(account); }
            else { run(state, account); }
        });
}

} // namespace AetherSDR
