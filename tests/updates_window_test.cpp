#include "TestSettingsProfile.h"
#include "UpdateTestNetwork.h"
#include "core/AppSettings.h"
#include "core/UpdateChecker.h"
#include "core/UpdateController.h"
#include "core/UpdateSettings.h"
#include "gui/FirmwareUpdateCard.h"
#include "gui/UpdatesWindow.h"
#include "models/RadioModel.h"

#include <QApplication>
#include <QCheckBox>
#include <QDebug>
#include <QFile>
#include <QFrame>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QDir>
#include <QFileInfo>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include "monocypher-ed25519.h"

#include <filesystem>

// The Updates window over an in-memory transport (no sockets) and the real
// v26.10.3 release JSON: which view each answer produces, that release text
// is never shown as markup, Skip, and the mirrored toggle. Then Install
// update in place: an UpdateController over the tests/fixtures/update
// release (file:// downloads, a test signing key, a recording fake helper).

using namespace AetherSDR;

namespace {

QByteArray releaseJson(const QString& tagline)
{
    QFile f(QStringLiteral(UPDATE_RELEASE_FIXTURE));
    if (!f.open(QIODevice::ReadOnly)) {
        return {};
    }
    QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    o[QStringLiteral("name")] = QStringLiteral("AetherSDR v26.10.3 — ") + tagline;
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

QString text(const UpdatesWindow& w, const char* name)
{
    const QLabel* l = w.findChild<QLabel*>(QString::fromLatin1(name));
    return l ? l->text() : QStringLiteral("<missing %1>").arg(QString::fromLatin1(name));
}

const QPushButton* button(const UpdatesWindow& w, const QString& textStart)
{
    for (const QPushButton* b : w.findChildren<QPushButton*>()) {
        if (b->text().startsWith(textStart)) {
            return b;
        }
    }
    return nullptr;
}

// A scratch install and release site for an UpdateController, signed by a
// key made here, and a helper that only records what it was given.
struct InstallRig {
    QTemporaryDir tmp;
    uint8_t secret[64];
    uint8_t pub[32];
    int launches = 0;
    bool lastRestart = false;
    bool inPlace = true;
    QString rolledBack;

    explicit InstallRig(bool withBundle = true)
    {
        uint8_t seed[32];
        for (int i = 0; i < 32; ++i) {
            seed[i] = static_cast<uint8_t>(0x71 + i);
        }
        crypto_ed25519_key_pair(secret, pub, seed);
        UpdateStager::setTrustedKeysForTesting({QByteArray(reinterpret_cast<const char*>(pub), 32)});
        const QString fixture = QStringLiteral(UPDATE_FIXTURE_DIR);
        std::error_code ec;
        std::filesystem::copy((fixture + QStringLiteral("/install-v26.10.2")).toStdString(),
                              tmp.filePath(QStringLiteral("install")).toStdString(),
                              std::filesystem::copy_options::recursive, ec);
        QFile m(fixture + QStringLiteral("/release/manifest.json"));
        m.open(QIODevice::ReadOnly);
        const QByteArray manifest = m.readAll();
        uint8_t sig[64];
        crypto_ed25519_sign(sig, secret, reinterpret_cast<const uint8_t*>(manifest.constData()),
                            static_cast<size_t>(manifest.size()));
        put(QStringLiteral("site/latest/manifest.json"), manifest);
        put(QStringLiteral("site/latest/manifest.json.sig"), QByteArray(reinterpret_cast<const char*>(sig), 64));
        if (withBundle) {
            const QString bundle = QStringLiteral("AetherSDR-v26.10.3-Windows-x64-from-v26.10.2.update.tar");
            QFile b(fixture + QStringLiteral("/release/") + bundle);
            b.open(QIODevice::ReadOnly);
            put(QStringLiteral("dl/v26.10.3/") + bundle, b.readAll());
        }
        UpdateStager::setDownloadBaseForTesting(QUrl::fromLocalFile(tmp.filePath(QStringLiteral("dl"))));
        UpdateSettings::setAcceptedManifestUtc({});
    }
    void put(const QString& rel, const QByteArray& data)
    {
        const QString path = tmp.filePath(rel);
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile f(path);
        f.open(QIODevice::WriteOnly | QIODevice::Truncate);
        f.write(data);
    }
    UpdateController::Environment env()
    {
        UpdateController::Environment e;
        e.request.runningVersion = QStringLiteral("26.10.2");
        e.request.platform = QStringLiteral("windows");
        e.request.arch = QStringLiteral("x86_64");
        e.request.installRoot = tmp.filePath(QStringLiteral("install"));
        e.request.stagingRoot = tmp.filePath(QStringLiteral("update"));
        e.request.manifestUrl = QUrl::fromLocalFile(tmp.filePath(QStringLiteral("site/latest/manifest.json")));
        e.request.fullFallback = true;
        e.canInstall = [this](QString*) { return inPlace; };
        e.launchHelper = [this](const QString&, const QString&, bool restart, QString*) {
            ++launches;
            lastRestart = restart;
            return true;
        };
        e.rolledBackVersion = [this] { return rolledBack; };
        return e;
    }
};

} // namespace

int main(int argc, char** argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    TestSettingsProfile profile(QStringLiteral("updates-window"));
    if (!profile.isValid()) { return 1; }
    QApplication app(argc, argv);
    QCoreApplication::setApplicationVersion(QStringLiteral("26.10.2"));
    AppSettings::instance().load();
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        if (!ok) { qCritical() << "FAIL:" << label; ++failures; }
    };

    {
        HeldNetwork network;
        UpdateChecker checker(nullptr, &network);
        auto* w = new UpdatesWindow(&checker, nullptr);
        w->show();
        app.processEvents();
        check(network.requests == 1, "opening with nothing known starts a check");
        check(text(*w, "updatesEyebrow").startsWith(QLatin1String("CHECKING")), "checking view");

        network.last->finishWithBody(releaseJson(QStringLiteral("<b>bold</b> & <a href=x>link</a>")));
        app.processEvents();
        check(text(*w, "updatesEyebrow") == QStringLiteral("NEW RELEASE · OCT 18, 2026"),
              "available view names the release date");
        check(text(*w, "updatesTagline") == QLatin1String("<b>bold</b> & <a href=x>link</a>"),
              "the tagline is shown as text, markup and all");
        bool allPlain = true;
        for (const QLabel* l : w->findChildren<QLabel*>()) {
            allPlain = allPlain && l->textFormat() == Qt::PlainText;
        }
        check(allPlain, "every label in the window is plain text");
        int chips = 0;
        for (const QFrame* f : w->findChildren<QFrame*>()) {
            chips += f->objectName() == QLatin1String("updatesChip");
        }
        check(chips == 10, "one chip per chapter");
        const QPushButton* get = button(*w, QStringLiteral("Get AetherSDR 26.10.3"));
        check(get && get->isVisible(), "the primary action names the release");
        check(!text(*w, "updatesFootnote").isEmpty(), "the footnote says what the download is");

        // Skip retires the badge and is not offered twice.
        QSignalSpy badge(w, &UpdatesWindow::badgeChanged);
        auto* skip = const_cast<QPushButton*>(button(*w, QStringLiteral("Skip this version")));
        check(skip != nullptr, "Skip is offered");
        if (skip) {
            skip->click();
        }
        app.processEvents();
        check(badge.size() == 1 && badge.first().first().toString().isEmpty(), "Skip clears the badge");
        check(UpdateChecker::isSkipped(QStringLiteral("26.10.3")), "Skip stores the release");
        check(skip && !skip->isEnabled() && skip->text() == QLatin1String("Skipped"), "Skip shows it is done");

        // The Help toggle and the window's box stay in step without echoing.
        auto* daily = w->findChild<QCheckBox*>(QStringLiteral("updatesCheckDaily"));
        QSignalSpy toggled(w, &UpdatesWindow::automaticCheckToggled);
        w->setAutomaticCheck(false);
        check(daily && !daily->isChecked() && toggled.isEmpty(), "mirroring the toggle does not echo it");
        if (daily) {
            daily->click();
        }
        check(toggled.size() == 1 && toggled.first().first().toBool(), "the box reports the operator's change");
        delete w;
    }

    {
        // Running the newest release.
        QCoreApplication::setApplicationVersion(QStringLiteral("26.10.3"));
        HeldNetwork network;
        UpdateChecker checker(nullptr, &network);
        UpdatesWindow w(&checker, nullptr);
        w.show();
        network.last->finishWithBody(releaseJson(QStringLiteral("tagline")));
        app.processEvents();
        check(text(w, "updatesEyebrow").contains(QLatin1String("UP TO DATE")), "current view");
        check(!button(w, QStringLiteral("Get AetherSDR")) || !button(w, QStringLiteral("Get AetherSDR"))->isVisible(),
              "no download offered when current");
    }

    {
        // A failed check, with nothing saved from an earlier answer.
        AppSettings::instance().remove(QStringLiteral("Updates"));
        AppSettings::instance().save();
        HeldNetwork network;
        UpdateChecker checker(nullptr, &network);
        UpdatesWindow w(&checker, nullptr);
        w.show();
        network.last->finishWithError(QNetworkReply::HostNotFoundError);
        app.processEvents();
        check(text(w, "updatesEyebrow") == QLatin1String("COULDN'T REACH GITHUB"), "failed view");
        const QPushButton* again = button(w, QStringLiteral("Check Now"));
        check(again && again->isVisible() && again->isEnabled(), "Check Now offered after a failure");
    }

    {
        // A release saved by an earlier answer, GitHub unreachable now: the
        // badge restored at startup must stay actionable (Skip) without a
        // successful request (#5304 Phase 1 ruling).
        QCoreApplication::setApplicationVersion(QStringLiteral("26.10.2"));
        UpdateSettings::setSkippedVersion(QString());
        UpdateSettings::recordAnswer(QDateTime::currentDateTimeUtc(), QStringLiteral("26.10.3"));
        HeldNetwork network;
        UpdateChecker checker(nullptr, &network);
        check(checker.latestRelease().version == QLatin1String("26.10.3")
                  && checker.latestRelease().fromSettings,
              "the checker starts on the saved release, without details");
        UpdatesWindow w(&checker, nullptr);
        w.show();
        check(network.requests == 1, "opening fetches the release's details");
        network.last->finishWithError(QNetworkReply::HostNotFoundError);
        app.processEvents();
        check(text(w, "updatesEyebrow").startsWith(QLatin1String("NEW RELEASE")),
              "offline, the saved release is still shown as new");
        const QWidget* numerals = w.findChild<QWidget*>(QStringLiteral("updatesVersion"));
        check(numerals && numerals->accessibleName() == QLatin1String("Version 26.10.3"),
              "offline, the release is named");
        check(text(w, "updatesChaptersTitle").isEmpty(), "offline, no chapters are built from the saved version");
        check(text(w, "updatesNote").contains(QLatin1String("Couldn't reach GitHub")),
              "offline, a note says the details are missing");
        const QPushButton* retry = button(w, QStringLiteral("Check Now"));
        check(retry && retry->isVisible() && retry->isEnabled(), "offline, Check Now is offered to retry");
        QSignalSpy badge(&w, &UpdatesWindow::badgeChanged);
        auto* skip = const_cast<QPushButton*>(button(w, QStringLiteral("Skip this version")));
        check(skip && skip->isVisible() && skip->isEnabled(), "offline, Skip is offered");
        if (skip) {
            skip->click();
        }
        check(badge.size() == 1 && UpdateChecker::isSkipped(QStringLiteral("26.10.3")),
              "offline, Skip clears the badge and is stored");

        // A check started from outside the window (Help → Check for Updates)
        // shows as checking at once.
        w.checkNow();
        check(network.requests == 2, "checkNow asks GitHub again");
    }

    {
        // A failed window turns to "checking" as soon as a check starts.
        QCoreApplication::setApplicationVersion(QStringLiteral("26.10.2"));
        AppSettings::instance().remove(QStringLiteral("Updates"));
        AppSettings::instance().save();
        HeldNetwork network;
        UpdateChecker checker(nullptr, &network);
        UpdatesWindow w(&checker, nullptr);
        w.show();
        network.last->finishWithError(QNetworkReply::HostNotFoundError);
        app.processEvents();
        check(text(w, "updatesEyebrow") == QLatin1String("COULDN'T REACH GITHUB"), "failed before the retry");
        w.checkNow();
        check(text(w, "updatesEyebrow").startsWith(QLatin1String("CHECKING")),
              "an outside retry shows as checking at once");
    }

    {
        // A saved release that is NOT newer says nothing about GitHub now: it
        // must never turn a failed or pending check into "up to date".
        for (const char* saved : {"26.10.3", "26.10.2"}) {
            QCoreApplication::setApplicationVersion(QStringLiteral("26.10.3"));
            UpdateSettings::setSkippedVersion(QString());
            UpdateSettings::recordAnswer(QDateTime::currentDateTimeUtc().addDays(-9),
                                         QString::fromLatin1(saved));
            HeldNetwork network;
            UpdateChecker checker(nullptr, &network);
            UpdatesWindow w(&checker, nullptr);
            w.show();
            check(text(w, "updatesEyebrow").startsWith(QLatin1String("CHECKING")),
                  "a saved release that is not newer shows as checking, not up to date");
            network.last->finishWithError(QNetworkReply::HostNotFoundError);
            app.processEvents();
            check(text(w, "updatesEyebrow") == QLatin1String("COULDN'T REACH GITHUB"),
                  "offline, a saved release that is not newer reports the failure");
        }
    }

    {
        // Opened on a saved version, then GitHub answers for that same
        // version: the answer's chapters must be built.
        for (const char* running : {"26.10.3", "26.10.2"}) {
            QCoreApplication::setApplicationVersion(QString::fromLatin1(running));
            UpdateSettings::setSkippedVersion(QString());
            UpdateSettings::recordAnswer(QDateTime::currentDateTimeUtc(), QStringLiteral("26.10.3"));
            HeldNetwork network;
            UpdateChecker checker(nullptr, &network);
            UpdatesWindow w(&checker, nullptr);
            w.show();
            network.last->finishWithBody(releaseJson(QStringLiteral("tagline")));
            app.processEvents();
            check(text(w, "updatesChaptersTitle") == QStringLiteral("WHAT'S INSIDE · 10 CHAPTERS"),
                  "the answer's chapters replace the saved version's");
            const QWidget* chips = w.findChild<QWidget*>(QStringLiteral("updatesChapters"));
            check(chips && chips->findChildren<QFrame*>(QStringLiteral("updatesChip")).size() == 10,
                  "one chip per chapter after the answer");
        }
    }

    {
        // After a real answer: a check in flight says so, a failed re-check
        // says the window shows the last answer, the footer names when
        // GitHub answered, and edited release notes rebuild the chips.
        QCoreApplication::setApplicationVersion(QStringLiteral("26.10.3"));
        UpdateSettings::setSkippedVersion(QString());
        HeldNetwork network;
        UpdateChecker checker(nullptr, &network);
        UpdatesWindow w(&checker, nullptr);
        w.show();
        network.last->finishWithBody(releaseJson(QStringLiteral("tagline")));
        app.processEvents();
        check(text(w, "updatesFootnote").startsWith(QLatin1String("GitHub answered")),
              "the footer names when GitHub answered");
        check(text(w, "updatesNote").isEmpty(), "no note after a good answer");

        checker.checkNow(UpdateChecker::Trigger::Automatic);   // started outside the window
        const QPushButton* retry = button(w, QStringLiteral("Checking"));
        check(retry && retry->isVisible() && !retry->isEnabled(), "a check in flight shows Checking…");
        network.last->finishWithError(QNetworkReply::HostNotFoundError);
        app.processEvents();
        check(text(w, "updatesEyebrow").contains(QLatin1String("UP TO DATE")), "the last answer stays shown");
        check(text(w, "updatesNote").contains(QLatin1String("Couldn't reach GitHub just now")),
              "a failed re-check after an answer says so");
        check(button(w, QStringLiteral("Check Now")) && button(w, QStringLiteral("Check Now"))->isEnabled(),
              "Check Now returns when the check ends");

        QFile f(QStringLiteral(UPDATE_RELEASE_FIXTURE));
        QJsonObject o;
        if (f.open(QIODevice::ReadOnly)) {
            o = QJsonDocument::fromJson(f.readAll()).object();
        }
        o[QStringLiteral("body")] = QStringLiteral("### Only chapter\n- x");
        w.checkNow();
        network.last->finishWithBody(QJsonDocument(o).toJson(QJsonDocument::Compact));
        app.processEvents();
        check(text(w, "updatesChaptersTitle") == QStringLiteral("WHAT'S INSIDE · 1 CHAPTER"),
              "release notes edited under the same version rebuild the chips");
        check(text(w, "updatesNote").isEmpty(), "a good answer clears the failure note");
    }

    {
        // A new release from a real answer, then a failed re-check: still
        // actionable, and the note says the release is GitHub's last answer.
        QCoreApplication::setApplicationVersion(QStringLiteral("26.10.2"));
        UpdateSettings::setSkippedVersion(QString());
        HeldNetwork network;
        UpdateChecker checker(nullptr, &network);
        UpdatesWindow w(&checker, nullptr);
        w.show();
        network.last->finishWithBody(releaseJson(QStringLiteral("tagline")));
        app.processEvents();
        w.checkNow();
        network.last->finishWithError(QNetworkReply::HostNotFoundError);
        app.processEvents();
        check(text(w, "updatesEyebrow").startsWith(QLatin1String("NEW RELEASE")), "the new release stays shown");
        check(text(w, "updatesNote").contains(QLatin1String("Couldn't reach GitHub just now")),
              "a failed re-check over a new release says so");
    }

    {
        // The radio section with no radio connected: it says so, and its
        // install controls are dimmed with that reason for a screen reader.
        RadioModel model;   // cold; never connects
        HeldNetwork network;
        UpdateChecker checker(nullptr, &network);
        UpdatesWindow w(&checker, &model);
        w.show();
        app.processEvents();
        const QLabel* name = nullptr;
        for (const QLabel* l : w.findChildren<QLabel*>()) {
            if (l->property("role").toString() == QLatin1String("radioName")) {
                name = l;
            }
        }
        check(name && name->text() == QLatin1String("No radio connected"), "radio section names the absence");
        const QPushButton* install = button(w, QStringLiteral("Install"));
        const QPushButton* open = button(w, QStringLiteral("Open Installer"));
        check(install && !install->isEnabled() && !install->accessibleDescription().isEmpty(),
              "Install is dimmed with a stated reason");
        check(open && !open->isEnabled() && !open->accessibleDescription().isEmpty(),
              "Open Installer is dimmed with a stated reason");
    }

    {
        // Firmware lanes from a published list (newest first, as FlexRadio's
        // index gives it), and the release Install starts on.
        const QStringList published = {"4.3.16", "4.2.20", "4.2.18", "4.1.5", "3.10.15", "2.10.1"};
        const auto lanes = FirmwareUpdateCard::firmwareLanes(published);
        check(lanes.size() == 3 && lanes[0].first == 4 && lanes[1].first == 3 && lanes[2].first == 2,
              "one lane per major line, newest line first");
        check(lanes.value(0).second == QStringList({"4.1.5", "4.2.18", "4.2.20", "4.3.16"}),
              "a lane runs oldest to newest");
        check(FirmwareUpdateCard::preselectedRelease(published, QStringLiteral("4.2.20.41343"))
                  == QLatin1String("4.3.16"),
              "a radio behind its line starts on the newest in that line (build number ignored)");
        check(FirmwareUpdateCard::preselectedRelease(published, QStringLiteral("4.3.16.42034")).isEmpty(),
              "a current radio starts on nothing");
        check(FirmwareUpdateCard::preselectedRelease(published, QStringLiteral("3.9.0")) == QLatin1String("3.10.15"),
              "a v3 radio is offered v3, not v4");
        check(FirmwareUpdateCard::preselectedRelease(published, QString()).isEmpty(),
              "no installed version, no preselection");
    }

    {
        // Install update in place: Install → progress and Cancel → Restart
        // to update, against the real fixture release.
        UpdateSettings::setSkippedVersion(QString());
        InstallRig rig;
        UpdateController controller(rig.env());
        HeldNetwork network;
        UpdateChecker checker(nullptr, &network);
        auto* w = new UpdatesWindow(&checker, nullptr);
        w->setController(&controller);
        w->show();
        app.processEvents();
        network.last->finishWithBody(releaseJson(QStringLiteral("Tagline")));
        app.processEvents();
        auto* primary = w->findChild<QPushButton*>(QStringLiteral("updatesGet"));
        auto* progressRow = w->findChild<QWidget*>(QStringLiteral("updatesInstallProgress"));
        auto* progress = w->findChild<QProgressBar*>(QStringLiteral("updatesProgress"));
        check(primary && primary->isVisible() && primary->text() == QLatin1String("Install update"),
              "in place, the primary action is Install update");
        check(primary && primary->accessibleName() == QLatin1String("Install AetherSDR 26.10.3")
                  && !primary->accessibleDescription().isEmpty(),
              "Install update names the release for a screen reader");
        check(progressRow && !progressRow->isVisible(), "no progress before Install");
        const QPushButton* download = button(*w, QStringLiteral("Download release"));
        check(download && download->isVisible() && download->accessibleDescription().contains(QLatin1String("browser")),
              "Download release keeps the browser route beside Install update");
        check(progress && !progress->accessibleName().isEmpty(), "the progress bar has a name");

        // Cancel: back to Install update, nothing kept.
        if (primary) {
            primary->click();
        }
        app.processEvents();
        check(progressRow && progressRow->isVisible() && !primary->isVisible(), "staging shows progress, not the button");
        check(download && !download->isVisible(), "no Download release while staging");
        auto* cancel = const_cast<QPushButton*>(button(*w, QStringLiteral("Cancel")));
        check(cancel && !cancel->accessibleName().isEmpty(), "Cancel is offered, named");
        if (cancel) {
            cancel->click();
        }
        app.processEvents();
        check(controller.state() == UpdateController::State::Idle, "Cancel stops staging");
        check(primary && primary->isVisible() && primary->text() == QLatin1String("Install update"),
              "after Cancel, Install update again");

        // Install, staged: Restart to update, and the badge's state.
        if (primary) {
            primary->click();
        }
        QTest::qWaitFor([&] { return controller.state() == UpdateController::State::Staged; }, 20000);
        app.processEvents();
        check(controller.state() == UpdateController::State::Staged, "Install update stages the release");
        check(primary && primary->isVisible() && primary->text() == QLatin1String("Restart to update"),
              "staged, the primary action is Restart to update");
        check(text(*w, "updatesFootnote").contains(QLatin1String("installs when you quit")),
              "the footnote says it installs on quit");
        QSignalSpy restart(w, &UpdatesWindow::restartRequested);
        if (primary) {
            primary->click();
        }
        check(restart.size() == 1 && rig.launches == 0, "Restart to update asks MainWindow, which hands it over");

        // Skip a staged release: it is discarded.
        auto* skip = const_cast<QPushButton*>(button(*w, QStringLiteral("Skip this version")));
        if (skip) {
            skip->click();
        }
        app.processEvents();
        check(controller.state() == UpdateController::State::Idle
                  && !QFileInfo::exists(rig.tmp.filePath(QStringLiteral("update/26.10.3"))),
              "Skip discards the staged release");

        // Ozy311's review: Skip, then Install the skipped release anyway
        // (maintainer ruling: that clears the skip), then Skip again once it
        // is ready. The download is discarded and nothing installs on quit.
        UpdateSettings::setSkippedVersion(QStringLiteral("26.10.3"));
        w->checkNow();   // refresh as skipped
        network.last->finishWithBody(releaseJson(QStringLiteral("Tagline")));
        app.processEvents();
        QSignalSpy unskipBadge(w, &UpdatesWindow::badgeChanged);
        if (primary) {
            primary->click();
        }
        check(!UpdateChecker::isSkipped(QStringLiteral("26.10.3")) && unskipBadge.size() == 1
                  && unskipBadge.first().first().toString() == QLatin1String("26.10.3"),
              "installing a skipped release clears the skip and brings the badge back");
        const bool ready =
            QTest::qWaitFor([&] { return controller.state() == UpdateController::State::Staged; }, 20000);
        app.processEvents();
        check(ready, "the release installed after a skip is staged");
        skip = const_cast<QPushButton*>(button(*w, QStringLiteral("Skip this version")));
        check(skip && skip->isEnabled(), "a skipped release installed anyway can be skipped again");
        if (skip) {
            skip->click();
        }
        app.processEvents();
        check(controller.state() == UpdateController::State::Idle && !controller.installOnQuit()
                  && rig.launches == 0,
              "skipping it again discards the download; nothing installs on quit");
        UpdateSettings::setSkippedVersion(QString());

        // Closing the window while it stages cancels it.
        if (primary) {
            primary->click();
        }
        app.processEvents();
        check(controller.state() == UpdateController::State::Staging, "staging again");
        w->close();
        app.processEvents();
        check(controller.state() == UpdateController::State::Idle, "closing the window stops staging");
    }

    {
        // No patch and no full release can be had: the browser, with why.
        UpdateSettings::setSkippedVersion(QString());
        InstallRig rig(false);
        UpdateController controller(rig.env());
        HeldNetwork network;
        UpdateChecker checker(nullptr, &network);
        UpdatesWindow w(&checker, nullptr);
        w.setController(&controller);
        w.show();
        app.processEvents();
        network.last->finishWithBody(releaseJson(QStringLiteral("Tagline")));
        app.processEvents();
        auto* primary = w.findChild<QPushButton*>(QStringLiteral("updatesGet"));
        if (primary) {
            primary->click();
        }
        QTest::qWaitFor([&] { return primary && primary->text().startsWith(QLatin1String("Get AetherSDR")); },
                        20000);
        check(primary && primary->text().startsWith(QLatin1String("Get AetherSDR 26.10.3")),
              "a release that can't be prepared here falls back to the download");
        check(text(w, "updatesNote").contains(QLatin1String("Download it instead")), "the note says why");
    }

    {
        // Review of #6777: a release rolled back here is not offered in
        // place again; the window says why and offers the download.
        UpdateSettings::setSkippedVersion(QString());
        InstallRig rig;
        rig.rolledBack = QStringLiteral("26.10.3");
        UpdateController controller(rig.env());
        HeldNetwork network;
        UpdateChecker checker(nullptr, &network);
        UpdatesWindow w(&checker, nullptr);
        w.setController(&controller);
        w.show();
        app.processEvents();
        network.last->finishWithBody(releaseJson(QStringLiteral("Tagline")));
        app.processEvents();
        check(button(w, QStringLiteral("Install update")) == nullptr
                  && button(w, QStringLiteral("Get AetherSDR 26.10.3")) != nullptr,
              "a rolled-back release offers the download, not Install update");
        check(text(w, "updatesNote").contains(QLatin1String("rolled back")), "the note says it was rolled back");
    }

    {
        // Where in place isn't possible, nothing changes: the download.
        UpdateSettings::setSkippedVersion(QString());
        InstallRig rig;
        rig.inPlace = false;
        UpdateController controller(rig.env());
        HeldNetwork network;
        UpdateChecker checker(nullptr, &network);
        UpdatesWindow w(&checker, nullptr);
        w.setController(&controller);
        w.show();
        app.processEvents();
        network.last->finishWithBody(releaseJson(QStringLiteral("Tagline")));
        app.processEvents();
        check(button(w, QStringLiteral("Get AetherSDR 26.10.3")) != nullptr,
              "without in-place support, Get AetherSDR as before");
        const QPushButton* link = button(w, QStringLiteral("Download release"));
        check(!link || !link->isVisible(), "no Download release when the primary action is the download");
    }

    return failures ? 1 : 0;
}
