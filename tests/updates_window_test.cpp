#include "TestSettingsProfile.h"
#include "UpdateTestNetwork.h"
#include "core/AppSettings.h"
#include "core/UpdateChecker.h"
#include "gui/UpdateStage.h"
#include "gui/UpdatesWindow.h"

#include <QApplication>
#include <QCheckBox>
#include <QDebug>
#include <QFile>
#include <QFrame>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>

// The Updates window over an in-memory transport (no sockets) and the real
// v26.10.3 release JSON: which view each answer produces, that release text
// is never shown as markup, Skip, the mirrored toggle, and that the stage's
// motion stops for reduced motion.

using namespace AetherSDR;

namespace {

QByteArray releaseJson(const QString& tagline)
{
    QFile f(QStringLiteral(UPDATE_RELEASE_FIXTURE));
    f.open(QIODevice::ReadOnly);
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
        auto* w = new UpdatesWindow(&checker);
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
        int cards = 0;
        for (const QFrame* f : w->findChildren<QFrame*>()) {
            chips += f->objectName() == QLatin1String("updatesChip");
            cards += f->objectName() == QLatin1String("updatesCard");
        }
        check(chips == 10, "one chip per chapter");
        check(cards == 3, "three highlight cards");
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

        // Motion: the stage animates while shown and stops for reduced motion.
        auto* stage = w->findChild<UpdateStage*>(QStringLiteral("updatesStage"));
        check(stage && stage->isAnimating(), "the stage animates while visible");
        if (stage) {
            stage->setMotionPreference(Qt::MotionPreference::ReducedMotion);
        }
        check(stage && !stage->isAnimating(), "reduced motion holds the stage still");
        delete w;
    }

    {
        // Running the newest release.
        QCoreApplication::setApplicationVersion(QStringLiteral("26.10.3"));
        HeldNetwork network;
        UpdateChecker checker(nullptr, &network);
        UpdatesWindow w(&checker);
        w.show();
        network.last->finishWithBody(releaseJson(QStringLiteral("tagline")));
        app.processEvents();
        check(text(w, "updatesEyebrow").contains(QLatin1String("UP TO DATE")), "current view");
        check(!button(w, QStringLiteral("Get AetherSDR")) || !button(w, QStringLiteral("Get AetherSDR"))->isVisible(),
              "no download offered when current");
    }

    {
        // A failed check.
        HeldNetwork network;
        UpdateChecker checker(nullptr, &network);
        UpdatesWindow w(&checker);
        w.show();
        network.last->finishWithError(QNetworkReply::HostNotFoundError);
        app.processEvents();
        check(text(w, "updatesEyebrow") == QLatin1String("COULDN'T REACH GITHUB"), "failed view");
        const QPushButton* again = button(w, QStringLiteral("Check Now"));
        check(again && again->isVisible() && again->isEnabled(), "Check Now offered after a failure");
    }

    return failures ? 1 : 0;
}
