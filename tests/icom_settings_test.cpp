// IcomCIV — settings and credentials.
//
// The load-bearing assertion is the SECURITY one: the Icom network password
// must never reach the settings database. RFC #4603 proposal E makes QtKeychain
// the only persistent credential store, and AppSettings enforces it at the
// setValue() seam rather than by caller discipline.
//
// That matters more for this radio than for most. The Icom protocol does not
// encrypt the password — it passes it through a fixed, trivially reversible
// substitution table (icom-oracle §2.5) — so anyone with a LAN packet capture
// already has it. We cannot fix the radio; what we can avoid is writing the
// same secret into a settings file that gets attached to bug reports.
//
// Runs in its own process: AppSettings is a process-wide singleton.

#include "TestSettingsProfile.h"

#include "core/AppSettings.h"
#include "core/SettingsCredentialPolicy.h"
#include "core/backends/icom/IcomCredentials.h"
#include "core/backends/icom/IcomProtocol.h"
#include "core/backends/icom/IcomSettings.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>

#include <cstdio>

using namespace AetherSDR;

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

int main(int argc, char** argv)
{
    // BEFORE QCoreApplication and before the first AppSettings touch — the
    // profile redirects the settings home, and Qt caches those paths.
    TestSettingsProfile profile(QStringLiteral("icom-settings-test"));
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();

    // ---- the non-secret document round-trips ------------------------------
    IcomSettings::reset();
    check(IcomSettings::username().isEmpty(), "no username by default");
    check(IcomSettings::controlPort() == icom::kControlPort, "default control port 50001");
    check(IcomSettings::serialPort() == icom::kSerialPort, "default serial port 50002");
    check(IcomSettings::audioPort() == icom::kAudioPort, "default audio port 50003");
    check(IcomSettings::civAddress() == 0xA4, "default CI-V address is the IC-705's 0xA4");

    IcomSettings::setUsername(QStringLiteral("beer"));
    IcomSettings::setLastHost(QStringLiteral("ic-705.local"));
    IcomSettings::setPorts(51001, 51002, 51003);
    IcomSettings::setCivAddress(0xB6);
    check(IcomSettings::username() == QStringLiteral("beer"), "username round-trips");
    check(IcomSettings::lastHost() == QStringLiteral("ic-705.local"), "host round-trips");
    check(IcomSettings::serialPort() == 51002, "ports round-trip");
    check(IcomSettings::civAddress() == 0xB6, "the MK2's address round-trips");

    // A hand-edited or truncated file must not command a nonsense port — 0 in
    // particular would bind an ephemeral local port and then tell the radio to
    // answer on a different one (Principle VII).
    AppSettings::instance().setValue(
        QStringLiteral("Icom"),
        QStringLiteral(R"({"controlPort":0,"serialPort":99999,"civAddress":0})"));
    check(IcomSettings::controlPort() == icom::kControlPort, "port 0 falls back to the default");
    check(IcomSettings::serialPort() == icom::kSerialPort, "an out-of-range port falls back too");
    check(IcomSettings::civAddress() == 0xA4, "and so does a zero CI-V address");

    // ---- THE SECURITY PROPERTY --------------------------------------------
    //
    // ("Icom", "Password") is registered in SettingsCredentialPolicy, so a
    // document carrying one is stripped at the AppSettings seam. Nothing in
    // this backend writes it — the entry is defence in depth against a future
    // caller who does.
    check(SettingsCredentialPolicy::isCredentialDocKey(QStringLiteral("Icom")),
          "the Icom document is registered as credential-bearing");

    AppSettings::instance().setValue(
        QStringLiteral("Icom"),
        QStringLiteral(R"({"username":"beer","Password":"beerbeer"})"));

    const QString stored =
        AppSettings::instance().value(QStringLiteral("Icom"), QString{}).toString();
    check(!stored.contains(QStringLiteral("beerbeer")),
          "THE PASSWORD IS NOT IN THE SETTINGS STORE");
    const QJsonObject obj = QJsonDocument::fromJson(stored.toUtf8()).object();
    check(!obj.contains(QStringLiteral("Password")),
          "the Password field is stripped from the document entirely");
    check(obj.value(QStringLiteral("username")).toString() == QStringLiteral("beer"),
          "while the non-secret fields around it survive");

    // The stripped secret is diverted to the session vault so a legacy caller
    // keeps working for this process; its feature owns moving it to the
    // keychain. One-shot by design, hence take.
    check(AppSettings::instance().takeSessionCredential(QStringLiteral("icom_password"))
              == QStringLiteral("beerbeer"),
          "and is diverted to the session vault rather than dropped");

    // ---- the credential cache the connect path reads ----------------------
    IcomCredentials::clearSession();
    check(IcomCredentials::sessionPassword().isEmpty(), "the session cache starts empty");

    // setSessionPassword is the "typed but not saved" path — it must NOT reach
    // the keychain, and it must be readable synchronously, because the connect
    // path (including auto-reconnect) cannot block on a keyring.
    IcomCredentials::setSessionPassword(QStringLiteral("s3cret"));
    check(IcomCredentials::sessionPassword() == QStringLiteral("s3cret"),
          "a typed password is readable synchronously on the connect path");

    IcomCredentials::clearSession();
    check(IcomCredentials::sessionPassword().isEmpty(), "and clearing forgets it");

    // save() primes the cache BEFORE the async keychain job, so a connect
    // issued in the same breath as the save cannot authenticate with an empty
    // password. Asserted without waiting for the keychain precisely because the
    // point is that it does not have to.
    IcomCredentials::save(QStringLiteral("beerbeer"));
    check(IcomCredentials::sessionPassword() == QStringLiteral("beerbeer"),
          "save() primes the cache synchronously — the connect path cannot race the keyring");
    IcomCredentials::clearSession();

    if (g_failures == 0)
        std::printf("icom_settings_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
