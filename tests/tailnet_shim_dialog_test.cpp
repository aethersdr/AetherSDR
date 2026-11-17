// Socket-free callback ordering through the production window and session store.
#include "TestSettingsProfile.h"
#include "AetherPDTestTransport.h"
#include "core/AppSettings.h"
#include "core/TailnetShimTokenStore.h"
#include "gui/TailnetShimDialog.h"

#include <QApplication>
#include <QCoreApplication>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <iostream>
#include <memory>
#include <utility>

namespace AetherSDR {
class TailnetShimDialogTestAccess {
public:
    static void prepare(TailnetShimDialog& dialog, const QString& token, bool managed)
    {
        // The null-model constructor never subscribes or reads credentials.
        dialog.m_lanReachable = true;
        dialog.m_apiAddress = QHostAddress(QStringLiteral("192.0.2.1"));
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
        dialog.m_credentialRevision = TailnetShimTokenStore::revision(dialog.m_radioSerial);
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
    static void beginStatus(TailnetShimDialog& dialog)
    {
        dialog.m_client.setRadioAddress(QHostAddress(QStringLiteral("192.0.2.1")));
        dialog.m_client.fetchStatus();
    }
    static void beginProvision(TailnetShimDialog& dialog)
    {
        dialog.m_autoSignInTried = true;
        dialog.signInWithQr();
    }
    static bool signOut(TailnetShimDialog& dialog)
    {
        bool confirmed = false;
        QObject callbackLifetime;
        QTimer::singleShot(0, &callbackLifetime, [&] {
            QPushButton* button = dialog.findChild<QPushButton*>(QStringLiteral("tsConfirmDisconnect"));
            if (button) {
                confirmed = true;
                button->click();
            } else if (QWidget* modal = QApplication::activeModalWidget()) {
                modal->close();
            }
        });
        dialog.signOut();
        return confirmed;
    }
    static QString state(const TailnetShimDialog& dialog)
    {
        return dialog.m_status ? dialog.m_status->state : QString();
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
        for (bool replaceToken : {false, true}) {
            AetherPDTest::Network network;
            TailnetShimDialog dialog(nullptr, false, nullptr, &network);
            Access::prepare(dialog, QStringLiteral("old-test-token"), managed);
            Access::beginLogin(dialog);
            const QPointer<AetherPDTest::Reply> old = network.last;
            const QString next = replaceToken ? QStringLiteral("new-test-token") : QStringLiteral("old-test-token");
            Access::provisioned(dialog, next);
            old->complete(200, R"({"state":"needs_login","auth_url":"https://example.invalid/obsolete-code"})");
            ok &= check(Access::token(dialog) == next && savedToken() == next
                            && Access::authUrl(dialog) == QStringLiteral("https://example.invalid/replacement-code"),
                        "successful old login cannot replace a new QR, including unchanged owner token");
        }
    }
    {
        AetherPDTest::Network network;
        TailnetShimDialog dialog(nullptr, false, nullptr, &network);
        Access::prepare(dialog, QStringLiteral("owner-test-token"), true);
        Access::beginStatus(dialog);
        const QPointer<AetherPDTest::Reply> old = network.last;
        Access::provisioned(dialog, QStringLiteral("owner-test-token"));
        old->complete(200, R"({"version":"0.5.0","provisioned":true,"state":"running"})");
        ok &= check(Access::state(dialog) == QStringLiteral("needs_login")
                        && Access::authUrl(dialog) == QStringLiteral("https://example.invalid/replacement-code"),
                    "old public status cannot overwrite the completed rejoin state");
    }
    const QByteArray local = QByteArray(R"({"version":"0.5.0","provisioned":true,"state":"local","managed_owner_version":1,"installation_id":")")
        + QByteArray(64, 'a') + R"("})";
    const QByteArray pending = QByteArray(R"({"version":"0.5.0","provisioned":true,"state":"needs_login","managed_owner_version":1,"installation_id":")")
        + QByteArray(64, 'a') + R"("})";
    {
        AetherPDTest::Network network;
        std::unique_ptr<TailnetShimDialog> dialog = std::make_unique<TailnetShimDialog>(nullptr, false, nullptr, &network);
        Access::prepare(*dialog, QStringLiteral("old-test-token"), false);
        Access::beginProvision(*dialog);
        const QPointer<AetherPDTest::Reply> reply = network.last;
        dialog.reset();
        reply->complete(200, R"({"admin_token":"new-test-token","status":{"version":"0.5.0","provisioned":true,"state":"running"}})");
        ok &= check(savedToken() == QStringLiteral("new-test-token"),
                    "closing the window still saves an issued legacy credential without touching its deleted UI");
    }
    for (bool success : {false, true}) {
        AetherPDTest::Network network;
        TailnetShimDialog dialog(nullptr, false, nullptr, &network);
        Access::prepare(dialog, QStringLiteral("owner-test-token"), true);
        Access::beginProvision(dialog);
        const QPointer<AetherPDTest::Reply> old = network.last;
        ok &= check(old && old->request().url().path() == QStringLiteral("/v1/provision"),
                    "application-owned provision uses injected transport");
        ok &= check(Access::signOut(dialog), "later operator confirms cancel");
        network.last->complete(200, local);
        const QString before = Access::message(dialog);
        old->complete(success ? 200 : 401, success
            ? QByteArray(R"({"admin_token":"owner-test-token","auth_url":"https://example.invalid/obsolete-code","status":)") + pending + '}'
            : QByteArray(R"({"error":"rejected"})"));
        ok &= check(Access::state(dialog) == QStringLiteral("local") && Access::authUrl(dialog).isEmpty()
                        && !Access::signingIn(dialog)
                        && Access::token(dialog) == QStringLiteral("owner-test-token")
                        && savedToken() == QStringLiteral("owner-test-token")
                        && Access::message(dialog) == before && !Access::busy(dialog),
                    "late application-owned provision reply cannot undo a newer same-owner cancel");
    }
    {
        AetherPDTest::Network network;
        TailnetShimDialog dialog(nullptr, false, nullptr, &network);
        Access::prepare(dialog, QStringLiteral("owner-test-token"), true);
        Access::beginLogin(dialog);
        const QPointer<AetherPDTest::Reply> old = network.last;
        ok &= check(Access::signOut(dialog), "operator confirms managed sign-out");
        network.last->complete(200, local);
        old->complete(200, R"({"state":"needs_login","auth_url":"https://example.invalid/obsolete-code"})");
        ok &= check(Access::state(dialog) == QStringLiteral("local") && Access::authUrl(dialog).isEmpty()
                        && Access::token(dialog) == QStringLiteral("owner-test-token")
                        && savedToken() == QStringLiteral("owner-test-token"),
                    "old same-token login cannot resurrect a QR after managed sign-out");
    }
    {
        AetherPDTest::Network network;
        TailnetShimDialog dialog(nullptr, false, nullptr, &network);
        Access::prepare(dialog, QStringLiteral("legacy-test-token"), false);
        Access::beginStatus(dialog);
        const QPointer<AetherPDTest::Reply> old = network.last;
        ok &= check(Access::signOut(dialog), "operator confirms legacy sign-out");
        const QPointer<AetherPDTest::Reply> signOut = network.last;
        old->complete(200, R"({"version":"0.5.0","provisioned":true,"state":"running"})");
        ok &= check(Access::busy(dialog), "old status cannot finish pending sign-out");
        signOut->complete(200, R"({"version":"0.5.0","provisioned":false,"state":"unprovisioned"})");
        ok &= check(Access::token(dialog).isEmpty() && savedToken().isEmpty() && !Access::busy(dialog),
                    "only confirmed sign-out response retires the legacy credential");
    }
    {
        AetherPDTest::Network network;
        TailnetShimDialog dialog(nullptr, false, nullptr, &network);
        Access::prepare(dialog, QStringLiteral("legacy-test-token"), false);
        ok &= check(Access::signOut(dialog), "operator confirms rejected legacy sign-out");
        network.last->complete(401, R"({"error":"rejected"})");
        Access::provisioned(dialog, QStringLiteral("new-test-token"));
        Access::beginStatus(dialog);
        network.last->complete(200, R"({"version":"0.5.0","provisioned":false,"state":"unprovisioned"})");
        ok &= check(Access::token(dialog) == QStringLiteral("new-test-token")
                        && savedToken() == QStringLiteral("new-test-token"),
                    "failed sign-out cannot leave a credential-deleting observer on later public status");
    }
    {
        AetherPDTest::Network network;
        TailnetShimDialog dialog(nullptr, false, nullptr, &network);
        Access::prepare(dialog, QStringLiteral("owner-test-token"), true);
        Access::beginStatus(dialog);
        Access::retire(dialog);
        network.last->complete(200, local);
        ok &= check(Access::busy(dialog) && Access::token(dialog) == QStringLiteral("owner-test-token"),
                    "retired original connection ignores successful status before changing busy state");
    }
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
        ok &= check(old && old->aborted && Access::token(dialog) == QStringLiteral("new-test-token")
                        && savedToken() == QStringLiteral("new-test-token")
                        && Access::authUrl(dialog) == QStringLiteral("https://example.invalid/replacement-code")
                        && Access::signingIn(dialog),
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
    for (const std::pair<bool, bool>& flags : {std::pair{false, false}, std::pair{false, true},
                                             std::pair{true, false}, std::pair{true, true}}) {
        const bool signOut = flags.first;
        const QString replacement = flags.second ? QStringLiteral("other-window-new-token")
                                                : QStringLiteral("old-window-token");
        AetherPDTest::Network oldNetwork;
        TailnetShimDialog oldWindow(nullptr, false, nullptr, &oldNetwork);
        Access::prepare(oldWindow, QStringLiteral("old-window-token"), false);
        if (signOut) {
            ok &= check(Access::signOut(oldWindow), "old window starts an explicitly confirmed sign-out");
        } else {
            Access::beginLogin(oldWindow);
        }
        const QPointer<AetherPDTest::Reply> oldReply = oldNetwork.last;
        AetherPDTest::Network newNetwork;
        TailnetShimDialog newWindow(nullptr, false, nullptr, &newNetwork);
        Access::prepare(newWindow, replacement, true);
        oldReply->complete(signOut ? 200 : 401, signOut
            ? QByteArray(R"({"version":"0.5.0","provisioned":false,"state":"unprovisioned"})")
            : QByteArray(R"({"error":"revoked"})"));
        ok &= check(Access::token(oldWindow).isEmpty()
                        && Access::token(newWindow) == replacement
                        && savedToken() == replacement
                        && newNetwork.sent.empty(),
                    signOut ? "old window sign-out cannot delete another window's replacement credential"
                            : "old window login401 cannot delete another window's replacement credential");
    }
    return ok ? 0 : 1;
}
