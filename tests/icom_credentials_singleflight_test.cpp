#include "core/backends/icom/IcomCredentials.h"

#include <qt6keychain/keychain.h>

#include <QCoreApplication>

#include <cstdio>

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
    auto* shortLivedContext = new QObject;
    IcomCredentials::load(shortLivedContext,
                          [&destroyedContextCalled](const QString&) {
                              destroyedContextCalled = true;
                          });
    delete shortLivedContext;
    QKeychain::TestControl::completeRead(QStringLiteral("unused"));
    drainCallbacks();
    check(!destroyedContextCalled,
          "a destroyed callback context still received the credential");

    if (g_failures == 0) {
        std::printf("icom_credentials_singleflight_test: all checks passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
