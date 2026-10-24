#pragma once

#include <QNetworkAccessManager>
#include <QObject>
#include <QString>

namespace AetherSDR {

// Checks the GitHub releases API for a newer AetherSDR version.
// kReleasesPageUrl is the browser-facing page; the API endpoint lives in the .cpp.
class UpdateChecker : public QObject {
    Q_OBJECT
public:
    static constexpr char kReleasesPageUrl[] =
        "https://github.com/aethersdr/AetherSDR/releases/latest";

    // Manual is Help → Check for Updates and reports every outcome to the
    // operator; Automatic is the background startup check, which only lights
    // the title-bar badge.
    enum class Trigger { Manual, Automatic };
    Q_ENUM(Trigger)

    explicit UpdateChecker(QObject* parent = nullptr);

    // A manual request made while an automatic check is in flight upgrades
    // that check, so the operator still gets an answer.
    void checkNow(Trigger trigger = Trigger::Manual);

    // Starts an automatic check when UpdateSettings allow one now and this
    // install is not updated by a store. Returns whether a check started.
    bool checkAutomaticallyIfDue();

    // "the Microsoft Store" or "Flathub" when that store installs this copy's
    // updates, worded to follow "come from"; empty for an installer,
    // portable, DMG or AppImage build.
    static QString storeName();

    // The release the title-bar badge should announce: `latestVersion` when
    // it is newer than `runningVersion`, not skipped, and this copy is not
    // updated by a store; otherwise empty. Live results and the one restored
    // from settings at startup both go through it.
    static QString badgeVersion(const QString& latestVersion, const QString& runningVersion);

    // badgeVersion() for the release the last successful check recorded.
    static QString knownBadgeVersion();

signals:
    void updateAvailable(const QString& latestVersion, AetherSDR::UpdateChecker::Trigger trigger);
    void upToDate(const QString& currentVersion, AetherSDR::UpdateChecker::Trigger trigger);
    void checkFailed(AetherSDR::UpdateChecker::Trigger trigger);

private:
    void fail(const QString& reason);

    QNetworkAccessManager m_nam;
    bool m_inFlight = false;
    Trigger m_trigger = Trigger::Manual;
};

} // namespace AetherSDR
