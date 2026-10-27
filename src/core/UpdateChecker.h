#pragma once

#include <QDateTime>
#include <QList>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>

namespace AetherSDR {

// What the Updates window shows about one release, read from the GitHub
// release JSON. Every string is network text: plain text only, trimmed and
// capped by parseRelease(), never markup.
struct ReleaseAsset {
    QString name;
    QUrl    url;
    qint64  size = 0;
};

struct ReleaseInfo {
    QString version;                    // parsed digits, "26.10.3"; empty if unusable
    QString tagline;                    // the release name after "AetherSDR vX — "
    QDateTime published;                // UTC; invalid when absent
    QDateTime answered;                 // UTC, when this answer arrived; invalid from settings
    QStringList chapters;               // the body's ### headings, boilerplate dropped
    QList<ReleaseAsset> assets;         // only download URLs that pass isTrustedDownloadUrl()
    // Only the version an earlier answer saved, not an answer from this run:
    // no details, and no claim about what GitHub says now.
    bool fromSettings = false;
    bool isValid() const { return !version.isEmpty(); }
};

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
    static bool isSkipped(const QString& version);
    // Install update on a skipped release: the operator wants it after all.
    static void unskipVersion(const QString& version);

    // Whether parsed version `a` is newer than `b`; false if `a` is unusable.
    static bool isNewer(const QString& a, const QString& b);

    // The release the title-bar badge should announce: `latestVersion` when
    // it is newer than `runningVersion`, the automatic check is on, it is not
    // skipped, and this copy is not updated by a store; otherwise empty. Live
    // results and the one restored from settings at startup both go through it.
    static QString badgeVersion(const QString& latestVersion, const QString& runningVersion);

    // badgeVersion() for the release the last successful check recorded.
    static QString knownBadgeVersion();

    // The release the last successful check described. Until this run's
    // check lands it is the version a previous answer saved (no details), so
    // a badge restored at startup can be acted on without a request.
    // Held in memory only: settings keep just the parsed version.
    const ReleaseInfo& latestRelease() const { return m_latest; }
    bool isChecking() const { return m_inFlight; }

    // Reads the release JSON GitHub returns. Pure: no network, no settings.
    static ReleaseInfo parseRelease(const QByteArray& json);

    // Only https://github.com/aethersdr/AetherSDR/releases/download/... may
    // reach the browser from a release's asset list.
    static bool isTrustedDownloadUrl(const QUrl& url);

    // The installer for an OS ("windows", "macos", "linux"), a CPU
    // architecture as QSysInfo names it, and
    // whether a Windows copy is the portable zip. Index into `assets`, or -1.
    static int pickAsset(const QList<ReleaseAsset>& assets, const QString& os,
                         const QString& cpuArch, bool portable);

    // pickAsset() for this computer, or an invalid asset when none fits
    // (the window then offers the release page).
    static ReleaseAsset assetForThisComputer(const ReleaseInfo& release);

    // The release page for a parsed version, built from the digits alone.
    static QUrl releasePageUrl(const QString& version);

signals:
    // A request went out; isChecking() is now true.
    void checkStarted();
    // A check finished with a usable answer; latestRelease() now holds it.
    void releaseInfoChanged();
    void updateAvailable(const QString& latestVersion, AetherSDR::UpdateChecker::Trigger trigger);
    void upToDate(const QString& currentVersion, AetherSDR::UpdateChecker::Trigger trigger);
    void checkFailed(AetherSDR::UpdateChecker::Trigger trigger);

private:
    void fail(const QString& reason);

    QNetworkAccessManager* m_nam{nullptr};
    ReleaseInfo m_latest;
    bool m_inFlight = false;
    bool m_tooLarge = false;
    // Whether the request in flight began as the automatic check. A manual
    // request can upgrade m_trigger mid-flight; the daily wait still counts it.
    bool m_startedAutomatically = false;
    Trigger m_trigger = Trigger::Manual;
};

} // namespace AetherSDR
