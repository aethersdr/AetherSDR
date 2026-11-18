#include "AetherPDTestTransport.h"
#include "core/AppSettings.h"
#include "core/TailnetShimCredentialPersistence.h"
#include "core/TailnetShimTokenStore.h"
#include "gui/TailnetShimDialog.h"

#include <QApplication>
#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QRegularExpression>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <qt6keychain/keychain.h>

#include <cstdio>
#include <memory>

namespace AetherSDR {
class TailnetShimDialogTestAccess {
public:
    static void prepare(TailnetShimDialog& dialog, const QString& id, bool managed)
    {
        dialog.m_lanReachable = true;
        dialog.m_apiAddress = QHostAddress(QStringLiteral("192.0.2.1"));
        dialog.m_originalSessionCurrent = [] { return true; };
        dialog.m_radioSerial = id;
        dialog.m_adminToken = QStringLiteral("fixture-stale-window");
        dialog.m_tokenLoaded = true;
        dialog.m_autoSignInTried = true;
        TailnetShimStatus status;
        status.version = QStringLiteral("0.5.0");
        status.provisioned = true;
        status.state = QStringLiteral("needs_login");
        status.claimable = !managed;
        if (managed) {
            status.installationId = QString(64, QLatin1Char('a'));
            status.managedOwnerVersion = 1;
        }
        dialog.m_status = status;
    }
    static void begin(TailnetShimDialog& dialog) { dialog.signInWithQr(); }
    static bool refused(const TailnetShimDialog& dialog)
    {
        return dialog.m_messageLabel->text().contains(QStringLiteral("local lock is unavailable"));
    }
};
} // namespace AetherSDR

namespace {
void report(const char* message)
{
    std::puts(message);
    std::fflush(stdout);
}
} // namespace

int main(int argc, char** argv)
{
    if (argc != 6) { return 2; }
    QString mode = QString::fromLocal8Bit(argv[1]);
    const QString id = QString::fromLocal8Bit(argv[2]);
    const QString expected = QString::fromLocal8Bit(argv[3]);
    const QString issued = QString::fromLocal8Bit(argv[4]);
    const QString mailbox = QString::fromLocal8Bit(argv[5]);
    const QRegularExpression identifier(QStringLiteral("\\Aaetherpd-native-credential-[0-9a-f]{32}\\z"));
    if (!identifier.match(id).hasMatch()
        || (!expected.isEmpty() && !expected.startsWith(QStringLiteral("fixture-")))
        || !issued.startsWith(QStringLiteral("fixture-"))) { return 2; }
    const bool managed = mode.endsWith(QStringLiteral("-managed"));
    if (managed) { mode.chop(8); }
    // Isolate settings without changing the production OS-lock cache location.
    QTemporaryDir profile;
    if (!profile.isValid()) { return 2; }
    qputenv("AETHER_SETTINGS_DIR", profile.path().toUtf8());
    const QString legacy = QDir(profile.path()).filePath(QStringLiteral("legacy"));
    if (!QDir().mkpath(legacy)) { return 2; }
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, legacy);
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, legacy);
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    app.setApplicationName(QStringLiteral("AetherSDR-native-enrollment-probe"));
    QTimer::singleShot(15000, &app, [&app] { report("deadline"); app.exit(3); });
    using namespace AetherSDR;
    using Persistence = tailnetshim::CredentialPersistence;
    const QString key = QStringLiteral("TailnetShim/AdminToken/") + id;
    if (mode == u"write" || mode == u"cleanup") {
        QTimer::singleShot(0, &app, [&] {
            TailnetShimTokenStore::save(id, mode == u"cleanup" ? QString{} : expected, &app,
                [&app](bool persisted, const QString&) {
                    report(persisted ? "pass" : "fail");
                    app.exit(persisted ? 0 : 1);
                });
        });
        return app.exec();
    }
    if (mode == u"read" || mode == u"absent" || mode == u"absent-metadata") {
        auto* job = new QKeychain::ReadPasswordJob(QStringLiteral("AetherSDR"), &app);
        job->setKey(mode == u"absent-metadata" ? Persistence::versionKey(key) : key);
        job->setAutoDelete(true);
        job->setInsecureFallback(false);
        QObject::connect(job, &QKeychain::Job::finished, &app, [&](QKeychain::Job* result) {
            const bool ok = mode == u"read"
                ? result->error() == QKeychain::NoError
                    && static_cast<QKeychain::ReadPasswordJob*>(result)->textData() == expected
                : result->error() == QKeychain::EntryNotFound;
            report(ok ? "pass" : "fail");
            app.exit(ok ? 0 : 1);
        });
        job->start();
        return app.exec();
    }
    if (mode != u"actor" && mode != u"try" && mode != u"hold-storage") { return 2; }
    Persistence::Lease held;
    AetherPDTest::Network network;
    std::unique_ptr<TailnetShimDialog> dialog;
    if (mode == u"hold-storage") {
        held = Persistence::systemAuthority()(key);
        if (!held) { return 1; }
        report("storage_held");
    } else {
        AppSettings::instance().load();
        dialog = std::make_unique<TailnetShimDialog>(nullptr, false, nullptr, &network);
        TailnetShimDialogTestAccess::prepare(*dialog, id, managed);
        TailnetShimDialogTestAccess::begin(*dialog);
        if (TailnetShimDialogTestAccess::refused(*dialog)) {
            if (!network.sent.empty()) { return 1; }
            report("setup_refused");
            return 4;
        }
    }
    bool sent = false;
    bool delivered = false;
    const auto complete = [&](bool success) {
        if (!sent || delivered) { app.exit(1); return; }
        delivered = true;
        if (success) {
            QJsonObject status{{"version", "0.5.0"}, {"provisioned", true}, {"state", "needs_login"}, {"claimable", !managed}};
            if (managed) {
                status.insert(QStringLiteral("installation_id"), QString(64, QLatin1Char('a')));
                status.insert(QStringLiteral("managed_owner_version"), 1);
            }
            network.sent.front().reply->complete(200,
                QJsonDocument(QJsonObject{{"admin_token", issued}, {"status", status}}).toJson(QJsonDocument::Compact));
        } else {
            network.sent.front().reply->complete(401, R"({"error":"fixture rejection"})");
        }
        report("reply_delivered");
    };
    QTimer tick;
    tick.setInterval(5);
    QObject::connect(&tick, &QTimer::timeout, &app, [&] {
        if (!sent && !network.sent.empty()) {
            const QNetworkRequest& request = network.sent.front().request;
            if (request.url().path() != QStringLiteral("/v1/provision")
                || request.rawHeader("Authorization") != "Bearer " + expected.toUtf8()) { app.exit(1); return; }
            sent = true;
            report("request_sent_with_fresh_credential");
            if (mode == u"try") { complete(true); }
        }
        if (delivered && TailnetShimClient::acquireProvisioningLease(id)) {
            report("setup_released");
            app.exit(0);
            return;
        }
        QFile file(mailbox);
        if (!file.exists()) { return; }
        if (!file.open(QIODevice::ReadOnly) || file.size() > 16) { app.exit(1); return; }
        const QByteArray command = file.readAll();
        file.close();
        if (!file.remove()) { app.exit(1); return; }
        if (command == "close") { dialog.reset(); report("window_closed"); }
        else if (command == "complete") { complete(true); }
        else if (command == "reject") { complete(false); }
        else if (command == "release" && held) { held.reset(); report("storage_released"); app.exit(0); }
        else { app.exit(1); }
    });
    tick.start();
    return app.exec();
}
