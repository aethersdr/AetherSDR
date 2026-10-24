#pragma once

#include <QNetworkAccessManager>
#include <QObject>
#include <QString>

namespace AetherSDR {

// Checks the GitHub releases API for a newer AetherSDR version, and is the
// GUI's one entry point to the Updates settings (UpdateSettings stays inside
// the engine). kReleasesPageUrl is the browser-facing page; the API endpoint
// lives in the .cpp.
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

    // `network` lets a test supply an in-memory transport; null makes one.
    // A supplied manager is not adopted: the caller keeps it alive.
    explicit UpdateChecker(QObject* parent = nullptr, QNetworkAccessManager* network = nullptr);

    // Starts a check; false when one is already in flight. A manual request
    // made during an automatic check upgrades that check, so the operator
    // still gets an answer.
    bool checkNow(Trigger trigger = Trigger::Manual);

    // Starts an automatic check when UpdateSettings allow one now and this
    // install is not updated by a store. Returns whether a check started.
    bool checkAutomaticallyIfDue();

    // "the Microsoft Store" or "Flatpak" when that installs this copy's
    // updates, worded to follow "come from"; empty for an installer,
    // portable, DMG or AppImage build. Probed once per process.
    static QString storeName();

    // The Help menu's automatic-check toggle. Off also retires the badge.
    static bool checkOnStartup();
    static void setCheckOnStartup(bool enabled);

    // Skip This Version: stores the parsed release, never the raw text.
    static void skipVersion(const QString& version);

    // The release the title-bar badge should announce: `latestVersion` when
    // it is newer than `runningVersion`, the automatic check is on, it is not
    // skipped, and this copy is not updated by a store; otherwise empty. Live
    // results and the one restored from settings at startup both go through it.
    static QString badgeVersion(const QString& latestVersion, const QString& runningVersion);

    // badgeVersion() for the release the last successful check recorded.
    static QString knownBadgeVersion();

signals:
    void updateAvailable(const QString& latestVersion, AetherSDR::UpdateChecker::Trigger trigger);
    void upToDate(const QString& currentVersion, AetherSDR::UpdateChecker::Trigger trigger);
    void checkFailed(AetherSDR::UpdateChecker::Trigger trigger);

private:
    void fail(const QString& reason);

    QNetworkAccessManager* m_nam{nullptr};
    bool m_inFlight = false;
    bool m_tooLarge = false;
    // Whether the request in flight began as the automatic check. A manual
    // request can upgrade m_trigger mid-flight; the daily wait still counts it.
    bool m_startedAutomatically = false;
    Trigger m_trigger = Trigger::Manual;
};

} // namespace AetherSDR
