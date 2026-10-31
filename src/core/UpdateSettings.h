#pragma once

// In-app update preferences (RFC #5304). One nested JSON object under the
// single AppSettings key "Updates" (the AetherClockSettings pattern), never
// flat keys.

#include <QDateTime>
#include <QJsonObject>
#include <QString>

namespace AetherSDR {

class UpdateSettings {
public:
    // An automatic check runs at most once per this many hours, which keeps
    // launches well under the unauthenticated GitHub API's 60 requests/hour.
    static constexpr qint64 kAutomaticCheckIntervalHours = 24;

    // Check for a newer release in the background at startup. Default on.
    static bool checkOnStartup();
    static void setCheckOnStartup(bool enabled);

    // The release the operator chose to skip; empty when none. A skipped
    // release never lights the title-bar badge, but an explicit
    // Help → Check for Updates still reports it.
    static QString skippedVersion();
    static void setSkippedVersion(const QString& version);
    static bool isSkipped(const QString& version);

    // When the last check (either trigger) got an answer from the server, or
    // an automatic check failed (which still starts the daily wait). Invalid
    // when there has been neither.
    static QDateTime lastCheckUtc();
    static void setLastCheckUtc(const QDateTime& when);

    // The newest release that answer named, as parsed digits; empty before
    // the first answer. Kept so a restart inside the throttle still knows
    // an update exists without asking the server again.
    static QString latestVersion();

    // One write for a successful answer: its time and the release it named.
    static void recordAnswer(const QDateTime& when, const QString& latestVersion);

    // The generated_at of the newest signed update manifest accepted
    // (docs/architecture/update-manifest.md): an older one is a replay and
    // is refused. Compared only with itself, never with the clock. Invalid
    // before the first.
    static QDateTime acceptedManifestUtc();
    static void setAcceptedManifestUtc(const QDateTime& generatedAt);

    // A release the updater installed and then rolled back because it failed
    // its self-test (RFC #5304 Phase 3). Install update does not offer it
    // again, so a broken release is never re-staged in a loop. Empty: none.
    static QString rolledBackVersion();
    static void setRolledBackVersion(const QString& version);

    // How many quits in a row a staged Linux release failed its self-test
    // as it was switched in (RFC #5304 Phase 5): the third is final. Zero
    // for any other version.
    static int quitSwitchTries(const QString& version);
    static void setQuitSwitchTries(const QString& version, int tries);

    // Whether an automatic check may run at nowUtc: enabled, and the last
    // answer is missing, older than the interval, or in the future (the
    // clock moved backward).
    static bool automaticCheckDue(const QDateTime& nowUtc);

private:
    static QJsonObject readObj();
    static void write(const QJsonObject& o);
};

} // namespace AetherSDR
