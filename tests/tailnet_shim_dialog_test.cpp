// Socket-free callback ordering through the production window and session store.
#include "TestSettingsProfile.h"
#include "AetherPDTestTransport.h"
#include "core/AppSettings.h"
#include "core/TailnetShimTokenStore.h"
#include "gui/TailnetShimDialog.h"

#include <QApplication>
#include <QCoreApplication>
#include <QLabel>
#include <iostream>

namespace AetherSDR {
class TailnetShimDialogTestAccess {
public:
    static void prepare(TailnetShimDialog& dialog, const QString& token, bool managed)
    {
        // The null-model constructor never subscribes or reads credentials.
        dialog.m_lanReachable = true;
        dialog.m_originalSessionCurrent = [] { return true; };
        dialog.m_radioSerial = QStringLiteral("TAILNET-REPLY-TEST");
        dialog.m_adminToken = token;
        dialog.m_tokenLoaded = true;
        TailnetShimStatus status;
        status.version = QStringLiteral("0.5.0");
        status.provisioned = true;
        status.state = QStringLiteral("running");
        if (managed) {
            status.installationId = QString(64, QLatin1Char('a'));
            status.managedOwnerVersion = 1;
        }
        dialog.m_status = status;
        TailnetShimTokenStore::save(dialog.m_radioSerial, token);
        dialog.m_busy = true;
        dialog.m_signingIn = true;
        dialog.m_authUrl = QStringLiteral("https://example.invalid/current-code");
    }
    static void failed(TailnetShimDialog& dialog, const QString& token, bool unauthorized = true,
                       const QString& operation = QStringLiteral("login"))
    {
        emit dialog.m_client.requestFailed(operation, QStringLiteral("injected rejection"),
                                          unauthorized, tailnetshim::credentialFingerprint(token));
    }
    static QString token(const TailnetShimDialog& dialog) { return dialog.m_adminToken; }
    static bool busy(const TailnetShimDialog& dialog) { return dialog.m_busy; }
    static bool signingIn(const TailnetShimDialog& dialog) { return dialog.m_signingIn; }
    static QString authUrl(const TailnetShimDialog& dialog) { return dialog.m_authUrl; }
    static QString message(const TailnetShimDialog& dialog) { return dialog.m_messageLabel->text(); }
    static void beginLogin(TailnetShimDialog& dialog)
    {
        dialog.m_client.setRadioAddress(QHostAddress(QStringLiteral("192.0.2.1")));
        dialog.m_client.fetchLogin(dialog.m_adminToken);
    }
    static void provisioned(TailnetShimDialog& dialog, const QString& token)
    {
        TailnetShimProvisionReply reply;
        reply.adminToken = token;
        reply.status = *dialog.m_status;
        reply.status.state = QStringLiteral("needs_login");
        reply.status.claimable = reply.status.installationId.isEmpty();
        reply.authUrl = QStringLiteral("https://example.invalid/replacement-code");
        // The application-owned provisioning transaction saves before delivery.
        TailnetShimTokenStore::save(dialog.m_radioSerial, token);
        dialog.onProvisioned(reply);
    }
    static void retire(TailnetShimDialog& dialog)
    {
        dialog.m_originalSessionCurrent = [] { return false; };
    }
};
}

namespace {
bool check(bool value, const char* message)
{
    if (!value) {
        std::cerr << "FAIL: " << message << '\n';
    }
    return value;
}
QString savedToken()
{
    QString token;
    bool loaded = false;
    AetherSDR::TailnetShimTokenStore::load(QStringLiteral("TAILNET-REPLY-TEST"),
        QCoreApplication::instance(), [&](const QString& value) { token = value; loaded = true; });
    QCoreApplication::processEvents();
    if (!loaded) {
        std::cerr << "FAIL: credential load did not complete\n";
    }
    return token;
}
}

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("tailnet_shim_dialog_test"));
    if (!profile.isValid()) { return 1; }
    QApplication app(argc, argv);
    using namespace AetherSDR;
    using Access = TailnetShimDialogTestAccess;
    AppSettings::instance().load();
    if (!check(!TailnetShimTokenStore::persistentStoreAvailable(),
               "test links the session-only credential store")) { return 1; }
    bool ok = true;
    for (bool managed : {false, true}) {
        for (bool unauthorized : {false, true}) {
            for (const QString& operation : {QStringLiteral("login"), QStringLiteral("provision"),
                                             QStringLiteral("sharing"), QStringLiteral("signout")}) {
                AetherPDTest::Network network;
                TailnetShimDialog dialog(nullptr, false, nullptr, &network);
                Access::prepare(dialog, QStringLiteral("new-test-token"), managed);
                Access::failed(dialog, QStringLiteral("old-test-token"), unauthorized, operation);
                ok &= check(Access::token(dialog) == QStringLiteral("new-test-token")
                            && savedToken() == QStringLiteral("new-test-token")
                            && Access::busy(dialog) && Access::signingIn(dialog)
                            && Access::authUrl(dialog) == QStringLiteral("https://example.invalid/current-code")
                            && network.sent.empty(),
                        "old authenticated failure cannot erase the current token, code or busy operation");
            }
        }
    }
    {
        AetherPDTest::Network network;
        TailnetShimDialog dialog(nullptr, false, nullptr, &network);
        Access::prepare(dialog, QStringLiteral("old-test-token"), false);
        Access::beginLogin(dialog);
        const QPointer<AetherPDTest::Reply> old = network.last;
        Access::provisioned(dialog, QStringLiteral("new-test-token"));
        old->complete(401, R"({"error":"revoked"})");
        ok &= check(Access::token(dialog) == QStringLiteral("new-test-token")
                        && savedToken() == QStringLiteral("new-test-token")
                        && Access::authUrl(dialog) == QStringLiteral("https://example.invalid/replacement-code")
                        && Access::signingIn(dialog) && network.sent.size() == 1,
                    "real delayed HTTP401 after provisioning preserves the replacement token and QR code");
    }
    {
        AetherPDTest::Network network;
        TailnetShimDialog dialog(nullptr, false, nullptr, &network);
        Access::prepare(dialog, QStringLiteral("current-test-token"), true);
        Access::retire(dialog);
        Access::failed(dialog, QStringLiteral("current-test-token"));
        ok &= check(Access::token(dialog) == QStringLiteral("current-test-token")
                        && savedToken() == QStringLiteral("current-test-token")
                        && Access::busy(dialog) && Access::signingIn(dialog)
                        && !Access::authUrl(dialog).isEmpty() && network.sent.empty(),
                    "retired original connection cannot mutate credential or window state");
    }
    {
        AetherPDTest::Network network;
        TailnetShimDialog dialog(nullptr, false, nullptr, &network);
        Access::prepare(dialog, QStringLiteral("current-test-token"), false);
        Access::failed(dialog, {}, false, QStringLiteral("status"));
        ok &= check(!Access::busy(dialog) && Access::message(dialog).contains(QStringLiteral("injected rejection"))
                        && Access::token(dialog) == QStringLiteral("current-test-token") && network.sent.empty(),
                    "unauthenticated public-status failure remains visible without deleting the token");
    }
    {
        AetherPDTest::Network network;
        TailnetShimDialog dialog(nullptr, false, nullptr, &network);
        Access::prepare(dialog, QStringLiteral("owner-test-token"), true);
        Access::failed(dialog, QStringLiteral("owner-test-token"));
        ok &= check(Access::token(dialog) == QStringLiteral("owner-test-token")
                        && savedToken() == QStringLiteral("owner-test-token")
                        && Access::message(dialog).contains(QStringLiteral("owner credential"))
                        && Access::authUrl(dialog).isEmpty() && !Access::signingIn(dialog)
                        && !Access::busy(dialog),
                    "managed login401 reports rejected ownership without deleting its only credential");
    }
    {
        AetherPDTest::Network network;
        TailnetShimDialog dialog(nullptr, false, nullptr, &network);
        Access::prepare(dialog, QStringLiteral("legacy-test-token"), false);
        Access::failed(dialog, QStringLiteral("legacy-test-token"));
        ok &= check(Access::token(dialog).isEmpty() && savedToken().isEmpty()
                        && !Access::signingIn(dialog) && !Access::busy(dialog)
                        && Access::authUrl(dialog).isEmpty(),
                    "matching legacy login401 still forgets the revoked credential and code");
    }
    return ok ? 0 : 1;
}
