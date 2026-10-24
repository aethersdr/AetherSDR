#include "TestSettingsProfile.h"
#include "UpdateTestNetwork.h"
#include "core/AppSettings.h"
#include "core/UpdateChecker.h"
#include "core/UpdateSettings.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QNetworkRequest>

// UpdateChecker's request lifecycle over an in-memory transport (no sockets):
// which failures start the daily wait, the trigger a result reports, and that
// only parsed digits leave the checker. The test finishes each reply by hand,
// so an automatic check can still be in flight when a manual request arrives.

using namespace AetherSDR;

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("update-checker"));
    if (!profile.isValid()) { return 1; }
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationVersion(QStringLiteral("26.10.2"));
    AppSettings& settings = AppSettings::instance();
    settings.load();
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        if (!ok) { qCritical() << "FAIL:" << label; ++failures; }
    };

    // The checker does not adopt a supplied manager; the test owns it.
    HeldNetwork network;
    UpdateChecker checker(nullptr, &network);
    const QNetworkRequest::RedirectPolicy managerPolicy = network.redirectPolicy();

    QList<UpdateChecker::Trigger> failed;
    QStringList available;
    QObject::connect(&checker, &UpdateChecker::checkFailed, &app,
                     [&](UpdateChecker::Trigger t) { failed.append(t); });
    QObject::connect(&checker, &UpdateChecker::updateAvailable, &app,
                     [&](const QString& v, UpdateChecker::Trigger) { available.append(v); });

    // Automatic, upgraded to manual mid-flight, then failing: the operator
    // hears about it, and the attempt still starts the daily wait.
    check(checker.checkNow(UpdateChecker::Trigger::Automatic), "automatic check starts");
    check(!checker.checkNow(UpdateChecker::Trigger::Manual), "manual request joins the one in flight");
    check(network.requests == 1, "joining makes no second request");
    network.last->finishWithError(QNetworkReply::HostNotFoundError);
    app.processEvents();
    check(failed.size() == 1 && failed.last() == UpdateChecker::Trigger::Manual,
          "the upgraded check reports its failure as manual");
    const QDateTime recorded = UpdateSettings::lastCheckUtc();
    check(recorded.isValid(), "an automatic-origin failure starts the daily wait");

    // A manual-only failure leaves the wait alone, so the origin resets per request.
    // Whole seconds: the store keeps ISO 8601 to the second.
    const QDateTime earlier = QDateTime::fromString(QStringLiteral("2026-10-20T12:00:00Z"), Qt::ISODate);
    UpdateSettings::setLastCheckUtc(earlier);
    check(checker.checkNow(UpdateChecker::Trigger::Manual), "manual check starts");
    network.last->finishWithError(QNetworkReply::HostNotFoundError);
    app.processEvents();
    check(failed.size() == 2 && failed.last() == UpdateChecker::Trigger::Manual,
          "manual failure is reported");
    check(UpdateSettings::lastCheckUtc() == earlier,
          "a manual-only failure does not start the daily wait");

    // A plain automatic failure counts too.
    check(checker.checkNow(UpdateChecker::Trigger::Automatic), "second automatic check starts");
    network.last->finishWithError(QNetworkReply::HostNotFoundError);
    app.processEvents();
    check(failed.size() == 3 && failed.last() == UpdateChecker::Trigger::Automatic,
          "automatic failure is reported as automatic");
    check(UpdateSettings::lastCheckUtc() > earlier, "an automatic failure starts the daily wait");

    // A redirect is refused, even one whose body names a newer release, and
    // the supplied manager's own policy is left alone.
    check(checker.checkNow(UpdateChecker::Trigger::Manual), "check answered by a redirect starts");
    // ManualRedirectPolicy is 0, so the attribute must be present, not just 0.
    const QVariant policy = network.lastRequest.attribute(QNetworkRequest::RedirectPolicyAttribute);
    check(policy.isValid() && policy.toInt() == QNetworkRequest::ManualRedirectPolicy,
          "the request follows no redirects");
    check(network.redirectPolicy() == managerPolicy, "the supplied manager is not reconfigured");
    network.last->finishWithBody(QByteArrayLiteral(R"({"tag_name":"v26.10.4"})"), 301);
    app.processEvents();
    check(failed.size() == 4 && available.isEmpty(), "a 301 fails the check instead of announcing a release");

    // A newer release leaves the checker as parsed digits only.
    check(checker.checkNow(UpdateChecker::Trigger::Manual), "check for a newer release starts");
    network.last->finishWithBody(QByteArrayLiteral(R"({"tag_name":"v26.10.3<b>x"})"));
    app.processEvents();
    check(available == QStringList{QStringLiteral("26.10.3")},
          "updateAvailable carries parsed digits, never the raw tag");
    check(UpdateSettings::latestVersion() == QLatin1String("26.10.3"),
          "the known release is stored as parsed digits");

    return failures ? 1 : 0;
}
