#include "core/backends/icom/IcomCredentials.h"

#include <qt6keychain/keychain.h>

#include <QCoreApplication>
#include <QSemaphore>
#include <QThread>

#include <atomic>
#include <cstdio>
#include <memory>

using namespace AetherSDR;

namespace {

int g_failures{0};

void check(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void drainCallbacks()
{
    QCoreApplication::processEvents();
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QObject firstContext;
    QObject secondContext;
    QObject thirdContext;

    QKeychain::TestControl::reset();
    IcomCredentials::clearSession();

    QStringList results;
    IcomCredentials::load(&firstContext, [&results](const QString& value) {
        results.append(value);
    });
    IcomCredentials::load(&secondContext, [&results](const QString& value) {
        results.append(value);
    });
    IcomCredentials::load(&thirdContext, [&results](const QString& value) {
        results.append(value);
    });

    check(QKeychain::TestControl::readStartCount == 1,
          "three concurrent callers started more than one keychain read");
    check(results.isEmpty(), "a callback ran before the keychain read completed");

    QKeychain::TestControl::completeRead(QStringLiteral("stored-password"));
    drainCallbacks();
    check(results.size() == 3, "not every waiting caller received the result");
    check(results == QStringList(3, QStringLiteral("stored-password")),
          "waiting callers did not receive the same credential");
    check(IcomCredentials::sessionPassword() == QStringLiteral("stored-password"),
          "the successful read did not prime the process session cache");

    // A completed batch must release the gate. In particular, an access error
    // cannot be cached as "no password" forever: a newly unlocked keychain must
    // be retryable without restarting AetherSDR.
    QString retryResult = QStringLiteral("not-called");
    IcomCredentials::load(&firstContext, [&retryResult](const QString& value) {
        retryResult = value;
    });
    check(QKeychain::TestControl::readStartCount == 2,
          "a later load did not start a new keychain read");
    QKeychain::TestControl::failRead(
        QKeychain::AccessDeniedByUser, QStringLiteral("test denial"));
    drainCallbacks();
    check(retryResult.isEmpty(), "a failed read did not report an empty result");

    IcomCredentials::load(&firstContext, [&retryResult](const QString& value) {
        retryResult = value;
    });
    check(QKeychain::TestControl::readStartCount == 3,
          "a keychain error permanently suppressed retry");
    QKeychain::TestControl::completeRead(QStringLiteral("after-retry"));
    drainCallbacks();
    check(retryResult == QStringLiteral("after-retry"),
          "the retry did not deliver its successful result");

    // The QObject context owns callback delivery. Closing the connection panel
    // while a macOS authorization prompt is open must not call dead UI state.
    bool destroyedContextCalled = false;
    auto shortLivedContext = std::make_unique<QObject>();
    IcomCredentials::load(shortLivedContext.get(),
                          [&destroyedContextCalled](const QString&) {
                              destroyedContextCalled = true;
                          });
    shortLivedContext.reset();
    QKeychain::TestControl::completeRead(QStringLiteral("unused"));
    drainCallbacks();
    check(!destroyedContextCalled,
          "a destroyed callback context still received the credential");

    // Each waiter must receive the result on its own QObject thread, even
    // when the underlying job completes on the main thread.
    QThread worker;
    worker.start();
    QObject workerContext;
    workerContext.moveToThread(&worker);
    QSemaphore delivered;
    std::atomic<bool> correctThread{false};
    const int readsBeforeWorker = QKeychain::TestControl::readStartCount;
    IcomCredentials::load(&firstContext, [](const QString&) { });
    IcomCredentials::load(&workerContext, [&](const QString& value) {
        correctThread = QThread::currentThread() == &worker
                     && value == QStringLiteral("worker-result");
        delivered.release();
    });
    check(QKeychain::TestControl::readStartCount == readsBeforeWorker + 1,
          "contexts on different threads did not share the read");
    QKeychain::TestControl::completeRead(QStringLiteral("worker-result"));
    check(delivered.tryAcquire(1, 3000), "worker callback did not arrive");
    check(correctThread.load(), "callback violated context thread affinity");
    QMetaObject::invokeMethod(&workerContext, [&] {
        workerContext.moveToThread(app.thread());
    }, Qt::BlockingQueuedConnection);
    worker.quit();
    worker.wait();

    // Destruction after completion must also cancel the queued delivery.
    bool queuedDestroyedCalled = false;
    auto queuedContext = std::make_unique<QObject>();
    IcomCredentials::load(queuedContext.get(), [&](const QString&) { queuedDestroyedCalled = true; });
    QKeychain::TestControl::completeRead(QStringLiteral("queued"));
    queuedContext.reset();
    drainCallbacks();
    check(!queuedDestroyedCalled, "destroyed receiver got an already queued result");

    // Delivery runs outside the mutex and after the in-flight gate releases.
    bool reentrantCalled = false;
    IcomCredentials::load(&firstContext, [&](const QString&) {
        IcomCredentials::load(&secondContext, [&](const QString& value) {
            reentrantCalled = value == QStringLiteral("reentrant");
        });
    });
    QKeychain::TestControl::completeRead(QStringLiteral("outer"));
    drainCallbacks();
    check(QKeychain::TestControl::pendingRead != nullptr, "reentrant load failed to start");
    if (QKeychain::TestControl::pendingRead != nullptr) {
        QKeychain::TestControl::completeRead(QStringLiteral("reentrant"));
    }
    drainCallbacks();
    check(reentrantCalled, "reentrant callback did not complete");

    IcomCredentials::load(&firstContext, [&](const QString& value) {
        check(value.isEmpty(), "missing entry did not return empty");
    });
    QKeychain::TestControl::failRead(QKeychain::EntryNotFound, {});
    drainCallbacks();
    bool missingRetried = false;
    IcomCredentials::load(&firstContext, [&](const QString&) { missingRetried = true; });
    QKeychain::TestControl::completeRead(QStringLiteral("exists-now"));
    drainCallbacks();
    check(missingRetried, "missing entry blocked later retry");

    if (g_failures == 0) {
        std::printf("icom_credentials_singleflight_test: all checks passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
