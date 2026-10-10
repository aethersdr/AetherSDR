#pragma once

#include <QDateTime>
#include <QMap>
#include <QStringList>
#include <QString>
#include <QVersionNumber>

// Is the radio's firmware behind the newest release its vendor publishes, and
// what do we tell the operator about it?
//
// NO FAMILY IS NAMED HERE. The published version arrives from whoever fetched
// it, and the release-notes link is built from a template the connected backend
// declares (RadioCapabilities::FirmwareUpdateSource) — see that record for why
// a consumer asks for it rather than testing a family name (docs/HERMES.md
// §"For coding agents"). A backend that declares nothing, and a check that has
// not answered, both yield Unknown, which draws exactly as the status bar
// always has.
namespace AetherSDR::FirmwareCurrency {

// A RELEASE is major.minor.patch, and that is the whole comparison.
//
// A radio reports four components ("4.2.20.41343"); the fourth is a build
// number. Nothing FlexRadio publishes carries one — not the software page, not
// the installer URL, not the MD5 file, not the release notes — it appears only
// inside a downloaded installer. So there is never a published build number to
// compare against, and keeping the radio's would make every radio look ahead of
// every published version. Truncating both sides is the only comparison that
// can actually be made.
inline QVersionNumber releaseOf(const QString& version)
{
    const QVersionNumber full = QVersionNumber::fromString(version);
    if (full.isNull())
        return {};

    const QList<int> segments = full.segments();
    return QVersionNumber(segments.mid(0, qMin<qsizetype>(3, segments.size())));
}

enum class Status {
    Unknown,   // nothing published yet, or a version neither side can parse
    Current,   // at or newer than the newest published release
    Outdated,  // behind the newest published release
};

// Judge a radio against the newest release OF ITS OWN LINE.
//
// `published` is newest-per-major (RadioCapabilities::FirmwareUpdateSource).
// The entry whose major matches the radio's is the only one it is measured
// against: a vendor can offer several lines at once, and telling an operator on
// v3 that they are behind a v4 release points them at a line they are not on
// and links notes they cannot use.
//
// A radio whose line is NOT published yields Unknown rather than a verdict —
// no entry means nobody has said what the newest release of that line is, and
// a radio must never be judged against a different line's.
//
// Within the line, a release NEWER than the published one is Current. The
// published answer can be stale — it is a web page read at connect — and an
// operator ahead of it must not be told to upgrade.
inline Status evaluate(const QMap<int, QString>& published, const QString& reported)
{
    const QVersionNumber theirs = releaseOf(reported);
    if (theirs.isNull())
        return Status::Unknown;

    const auto entry = published.constFind(theirs.majorVersion());
    if (entry == published.constEnd())
        return Status::Unknown;

    const QVersionNumber ours = releaseOf(*entry);
    if (ours.isNull())
        return Status::Unknown;

    return QVersionNumber::compare(theirs, ours) < 0 ? Status::Outdated
                                                     : Status::Current;
}

// The release a radio would be upgraded TO: the newest of its own line, or
// empty when that line is not published. This is what the release-notes link
// must be built from — never the newest release overall.
// The newest release in the SAME LINE as `reported`, taken from a list that is
// already newest-first (FirmwareStager::parseAllReleases()). Empty when the
// radio reports nothing parseable or the list names no release in its line.
//
// The list form of upgradeTargetFor(): Radio Setup holds every release rather
// than one per major, and still has to mark which single row is the one this
// radio should care about.
inline QString newestInLine(const QStringList& releasesNewestFirst,
                            const QString& reported)
{
    const QVersionNumber theirs = releaseOf(reported);
    if (theirs.isNull())
        return {};

    for (const QString& release : releasesNewestFirst) {
        const QVersionNumber candidate = QVersionNumber::fromString(release);
        // First match wins because the list is ordered, so this is the newest
        // release in the line and not merely one of them.
        if (!candidate.isNull() && candidate.majorVersion() == theirs.majorVersion())
            return release;
    }
    return {};
}

inline QString upgradeTargetFor(const QMap<int, QString>& published,
                                const QString& reported)
{
    const QVersionNumber theirs = releaseOf(reported);
    if (theirs.isNull())
        return {};

    const auto entry = published.constFind(theirs.majorVersion());
    return entry == published.constEnd() ? QString() : *entry;
}

// Which of two versions names the newer release: -1, 0 or 1, on the same
// major.minor.patch basis as evaluate(). Returns 0 when either is unparseable,
// which reads as "no reason to think one is newer".
inline int compareReleases(const QString& lhs, const QString& rhs)
{
    const QVersionNumber left = releaseOf(lhs);
    const QVersionNumber right = releaseOf(rhs);
    if (left.isNull() || right.isNull())
        return 0;

    const int cmp = QVersionNumber::compare(left, right);
    return cmp < 0 ? -1 : (cmp > 0 ? 1 : 0);
}

// The release-notes page for `version`, from a template whose "%1" is the
// release with its dots as dashes ("4.2.20" -> "4-2-20"). Empty when there is
// no template or nothing parseable to put in it, and the caller then offers the
// operator nowhere to click.
inline QString releaseNotesUrl(const QString& urlTemplate, const QString& version)
{
    const QVersionNumber release = releaseOf(version);
    if (urlTemplate.isEmpty() || release.isNull())
        return {};

    return urlTemplate.arg(release.toString().replace(QLatin1Char('.'),
                                                      QLatin1Char('-')));
}

// How long a published-release answer is trusted before it is looked up again.
//
// A day, because that is the longest a wrong answer can matter and the shortest
// that makes launches quiet. FlexRadio ships every few weeks, so a day-old
// answer is essentially never wrong; and an operator who connects daily then
// contacts flexradio.com once a day rather than once per launch.
inline constexpr qint64 kPublishedVersionMaxAgeSecs = 24 * 60 * 60;

// Should a cached answer be looked up again?
//
// Stale when there is nothing cached, when the stamp cannot be read, when it is
// older than the window — and ALSO when it is in the FUTURE. A clock that was
// wrong when the stamp was written, or a machine that has since been corrected
// backwards, would otherwise leave a cache that never expires. Re-asking once
// is cheap; never asking again is not.
inline bool publishedVersionIsStale(const QString& cachedVersion,
                                    const QDateTime& checkedAt,
                                    const QDateTime& now)
{
    if (cachedVersion.isEmpty() || !checkedAt.isValid() || !now.isValid())
        return true;

    const qint64 age = checkedAt.secsTo(now);
    return age < 0 || age >= kPublishedVersionMaxAgeSecs;
}

// Empty for Unknown, so the caller clears the tooltip rather than leaving a
// previous radio's verdict on the label.
inline QString tooltip(Status status)
{
    switch (status) {
    case Status::Current:
        return QStringLiteral("Firmware up-to-date");
    case Status::Outdated:
        return QStringLiteral("Firmware is out of date, click to see release notes.");
    case Status::Unknown:
        break;
    }
    return {};
}

} // namespace AetherSDR::FirmwareCurrency
