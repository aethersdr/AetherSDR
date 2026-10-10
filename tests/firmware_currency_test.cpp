// Covers the status-bar firmware verdict end to end, minus the network: how the
// published software page is read, which radios the verdict applies to, which
// releases count as behind, and what the operator is told and offered.
//
// The gate is a DECLARED capability, never a family name (docs/HERMES.md
// §"For coding agents"), so "which radios" here means "which backends declared
// that their firmware versions are published".

#include "core/FirmwareCurrency.h"
#include "TestSettingsProfile.h"

#include "core/AppSettings.h"
#include "core/FirmwareStager.h"
#include "core/backends/flex/FlexBackend.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QJsonObject>
#include <QMap>
#include <QStringList>

#include <cstdio>
#include <memory>

namespace AetherSDR {

// Drives a completed lookup without a network peer. The schema re-read inside
// applyPublishedReleases() is the defect this exists to pin, and it cannot be
// reached from outside any other way.
class FlexBackendTestAccess {
public:
    static void completeLookup(AetherSDR::FlexBackend& backend,
                               const QMap<int, QString>& releases)
    {
        backend.applyPublishedReleases(releases);
    }
};

}  // namespace AetherSDR

namespace {

using AetherSDR::FirmwareStager;

using AetherSDR::FirmwareCurrency::Status;
using AetherSDR::FirmwareCurrency::compareReleases;
using AetherSDR::FirmwareCurrency::evaluate;
using AetherSDR::FirmwareCurrency::kPublishedVersionMaxAgeSecs;
using AetherSDR::FirmwareCurrency::publishedVersionIsStale;
using AetherSDR::FirmwareCurrency::releaseNotesUrl;
using AetherSDR::FirmwareCurrency::tooltip;
using AetherSDR::FirmwareCurrency::upgradeTargetFor;

int g_failures = 0;

void check(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

// What flexradio.com/software offered when these tests were written: several
// lines side by side, with THREE v4 releases downloadable at once (4.1.5,
// 4.2.18, 4.2.20). Spelled out rather than fetched — the tests must not need a
// network, and must not silently re-baseline when FlexRadio ships.
const QMap<int, QString> kPublished = {
    {2, QStringLiteral("2.10.1")},
    {3, QStringLiteral("3.10.15")},
    {4, QStringLiteral("4.2.20")},
};

Status verdict(const QString& reported) { return evaluate(kPublished, reported); }

// One SmartSDR record as FlexRadio's REST index returns it.
QString record(const QString& title, const QString& slug)
{
    return QStringLiteral(R"({"title":{"rendered":"%1"},"slug":"%2"})").arg(title, slug);
}

// A record whose title is the release, spelled the way the index spells it.
QString release(const QString& version)
{
    QString slug = version;
    return record(QStringLiteral("SmartSDR v") + version,
                  QStringLiteral("smartsdr-v") + slug.replace(QLatin1Char('.'),
                                                              QLatin1Char('-')));
}

QByteArray index(const QStringList& records)
{
    return (QLatin1Char('[') + records.join(QLatin1Char(','))
            + QLatin1Char(']')).toUtf8();
}

QString newestInLine(const QByteArray& json, int major)
{
    return FirmwareStager::parsePublishedReleases(json).value(major);
}

// THE FINDING THIS FEATURE WAS REBUILT AROUND (PR #6177, Jeremy's B1).
// A radio is judged against the newest release of ITS OWN LINE. Measuring every
// radio against the newest release overall tells a v3 operator they are behind
// 4.2.20 — a line they are not on — and links release notes they cannot use.
void checkTheVerdictIsScopedToTheRadiosOwnLine()
{
    // The two radios this was confirmed on.
    check(verdict(QStringLiteral("4.2.20.41343")) == Status::Current,
          "a radio on the newest v4 release is current");
    check(verdict(QStringLiteral("3.9.18.36988")) == Status::Outdated,
          "a radio on 3.9.18 is behind its own line's 3.10.15");

    // ...and the case that was wrong before: the newest release of an OLDER
    // line is CURRENT, not permanently out of date against v4.
    check(verdict(QStringLiteral("3.10.15")) == Status::Current,
          "a radio on the newest v3 release is current, not behind v4");
    check(verdict(QStringLiteral("2.10.1")) == Status::Current,
          "a radio on the newest v2 release is current, not behind v4");

    // Within a line it still nudges: three v4 releases are downloadable at once.
    check(verdict(QStringLiteral("4.1.5")) == Status::Outdated,
          "an older v4 release is behind the newest v4");
    check(verdict(QStringLiteral("4.2.18.41174")) == Status::Outdated,
          "4.2.18 is behind 4.2.20 — same line");

    // A radio on a line nobody publishes gets no verdict, rather than being
    // measured against someone else's line.
    check(verdict(QStringLiteral("1.12.1")) == Status::Unknown,
          "a radio on an unpublished line yields no verdict");

    // The PROTOCOL version, not a firmware version. A connect-by-IP session
    // briefly puts the `V` line into the model before `software_ver` corrects
    // it, and "1.4.0.0" was confirmed on the wire. Line scoping means it falls
    // in an unpublished line and draws plain, instead of flashing orange with a
    // 404 link (PR #6177 review, aethersdr-agent B3).
    //
    // CONTINGENT: this holds because FlexRadio publishes no v1 release today.
    // If one ever appears, the protocol version becomes judgeable again and the
    // real fix — judging only after software_ver arrives — is needed.
    check(verdict(QStringLiteral("1.4.0.0")) == Status::Unknown,
          "the protocol version falls in an unpublished line and is not judged");
}

// The link must point at the upgrade actually being recommended.
void checkTheUpgradeTargetIsTheOwnLineRelease()
{
    check(upgradeTargetFor(kPublished, QStringLiteral("3.9.18.36988"))
              == QStringLiteral("3.10.15"),
          "a v3 radio is pointed at 3.10.15, not 4.2.20");
    check(upgradeTargetFor(kPublished, QStringLiteral("4.1.5"))
              == QStringLiteral("4.2.20"),
          "a v4 radio is pointed at 4.2.20");
    check(upgradeTargetFor(kPublished, QStringLiteral("1.12.1")).isEmpty(),
          "an unpublished line offers no upgrade target");
    check(upgradeTargetFor({}, QStringLiteral("4.1.5")).isEmpty(),
          "nothing published means no upgrade target");
}

void checkTheBuildNumberIsIgnored()
{
    // No published source carries a build number, so two radios on the same
    // release must reach the same verdict whatever their build.
    check(verdict(QStringLiteral("4.2.20.99999")) == Status::Current,
          "a high build of the published release is current");
    check(verdict(QStringLiteral("4.2.20.1")) == Status::Current,
          "a low build of the published release is equally current");
    check(verdict(QStringLiteral("4.2.20")) == Status::Current,
          "the release with no build number at all is current");
}

void checkComparisonIsNumericNotLexicographic()
{
    // As text, "4.2.9" sorts AFTER "4.2.20" and "4.2.100" before it.
    check(verdict(QStringLiteral("4.2.9.99999")) == Status::Outdated,
          "4.2.9 is behind 4.2.20 despite sorting after it as text");
    check(verdict(QStringLiteral("4.2.100.1")) == Status::Current,
          "4.2.100 is ahead of 4.2.20 despite sorting before it as text");
    check(compareReleases(QStringLiteral("3.10.15"), QStringLiteral("3.9.19")) > 0,
          "compareReleases() orders 3.10.15 above 3.9.19");
    check(compareReleases(QStringLiteral("4.2.20.41343"), QStringLiteral("4.2.20")) == 0,
          "a build number does not make a release newer than itself");
}

void checkNewerThanPublishedIsNotOutdated()
{
    // The published answer is a web page read at connect and can lag a release.
    check(verdict(QStringLiteral("4.2.21.0")) == Status::Current,
          "a release newer than the published one is not flagged out of date");
}

void checkNothingPublishedMeansNoVerdict()
{
    check(evaluate({}, QStringLiteral("3.9.18")) == Status::Unknown,
          "no published releases yields no verdict, however old the radio is");
    check(evaluate({{4, QStringLiteral("not a version")}},
                   QStringLiteral("4.1.5")) == Status::Unknown,
          "an unparseable published release yields no verdict");
    // Guards the guard: the same radio IS judged once its line is published.
    check(verdict(QStringLiteral("3.9.18")) == Status::Outdated,
          "the same radio IS judged once its line is published");
}

void checkUnparseableRadioVersionsAreUnknown()
{
    check(verdict(QString()) == Status::Unknown,
          "a disconnected radio's cleared label yields no verdict");
    check(verdict(QStringLiteral("Gateware 75")) == Status::Unknown,
          "a label word in front of the number is not silently parsed");
    check(verdict(QStringLiteral("unknown")) == Status::Unknown,
          "a non-numeric version yields no verdict");
}

void checkTooltipsMatchTheState()
{
    check(tooltip(Status::Current) == QStringLiteral("Firmware up-to-date"),
          "the current-firmware tooltip reads as specified");
    check(tooltip(Status::Outdated)
              == QStringLiteral("Firmware is out of date, click to see release notes."),
          "the out-of-date tooltip reads as specified, including the invitation to click");
    check(tooltip(Status::Unknown).isEmpty(),
          "an unjudged radio clears the tooltip rather than keeping a stale one");
}

void checkWhenTheCachedAnswerIsLookedUpAgain()
{
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const QString cached = QStringLiteral("cached");

    check(!publishedVersionIsStale(cached, now.addSecs(-60), now),
          "a minutes-old answer is reused");
    check(kPublishedVersionMaxAgeSecs == 24 * 60 * 60,
          "the lookup window is 24 hours");
    check(!publishedVersionIsStale(cached, now.addSecs(-23 * 60 * 60), now),
          "23 hours old is still reused");
    check(publishedVersionIsStale(cached, now.addSecs(-25 * 60 * 60), now),
          "25 hours old is looked up again");
    check(publishedVersionIsStale(QString(), now.addSecs(-60), now),
          "an empty cache means look it up, however recent the stamp");
    check(publishedVersionIsStale(cached, QDateTime(), now),
          "an unreadable stamp means look it up");
    // A stamp in the FUTURE would otherwise never expire.
    check(publishedVersionIsStale(cached, now.addSecs(60), now),
          "a stamp in the future is looked up again rather than trusted forever");
}

void checkTheBackendDeclaresTheRecordAndAsksNobodyAtConstruction()
{
    auto backend = std::make_unique<AetherSDR::FlexBackend>();
    const auto source = backend->capabilities().firmwareUpdateSource;
    check(source.has_value(), "FlexBackend declares where its firmware is published");
    if (!source.has_value())
        return;

    check(source->publishedReleases.isEmpty(),
          "constructing a backend performs no lookup and publishes no releases");
    // Direct evidence, not an absence that proves nothing: the lookup is driven
    // by the wire's `connected` signal, and no radio is connected here.
    check(backend->findChild<AetherSDR::FirmwareStager*>() == nullptr,
          "constructing a backend builds no FirmwareStager, so nothing has asked");

    check(evaluate(source->publishedReleases, QStringLiteral("3.9.18")) == Status::Unknown,
          "a backend that has not looked anything up judges nothing");

    // The link template, applied to the radio's OWN line.
    check(releaseNotesUrl(source->releaseNotesUrlTemplate,
                          upgradeTargetFor(kPublished, QStringLiteral("3.9.18")))
              == QStringLiteral("https://www.flexradio.com/documentation/"
                                "smartsdr-v3-10-15-release-notes/"),
          "a v3 radio's link opens 3.10.15's notes");
    check(releaseNotesUrl(source->releaseNotesUrlTemplate,
                          upgradeTargetFor(kPublished, QStringLiteral("4.1.5")))
              == QStringLiteral("https://www.flexradio.com/documentation/"
                                "smartsdr-v4-2-20-release-notes/"),
          "a v4 radio's link opens 4.2.20's notes");
    check(releaseNotesUrl(source->releaseNotesUrlTemplate, QString()).isEmpty(),
          "no upgrade target means no link");
}

// FlexRadio's REST software index is untrusted input (Principle VII) and the
// part most likely to change under us, so its parse is pinned without a network.
//
// The index replaced a scrape of flexradio.com/software/ (Pat's find: it is
// what SmartSDR and Maestro themselves read). Each release is its own record,
// so the old "biggest version named anywhere on the page" heuristic is gone --
// but the index carries changelogs, an API page and fonts alongside the
// releases, and those are what the parse must not mistake for one.
void checkTheSoftwareIndexIsReadIntoLines()
{
    // The real index's shape, including every non-release record it returns
    // for `search=SmartSDR` today.
    const QByteArray real = index({
        release(QStringLiteral("4.2.20")),
        record(QStringLiteral("SmartSDR v4.x API (FlexLib)"),
               QStringLiteral("smartsdr-v4-x-api-flexlib")),
        record(QStringLiteral("SmartSDR v4 Changelog"),
               QStringLiteral("smartsdr_v4_changelog")),
        release(QStringLiteral("4.2.18")),
        release(QStringLiteral("4.1.5")),
        release(QStringLiteral("3.10.15")),
        record(QStringLiteral("SmartSDR v2.5.1+ Changelog"),
               QStringLiteral("smartsdr_v2_changelog")),
        release(QStringLiteral("2.10.1")),
        record(QStringLiteral("Didact Gothic Font"),
               QStringLiteral("didact-gothic-font")),
    });
    const auto rel = FirmwareStager::parsePublishedReleases(real);
    check(rel.value(4) == QStringLiteral("4.2.20"), "v4's newest is 4.2.20");
    check(rel.value(3) == QStringLiteral("3.10.15"), "v3's newest is 3.10.15");
    check(rel.value(2) == QStringLiteral("2.10.1"), "v2's newest is 2.10.1");
    check(rel.size() == 3, "exactly the three published lines are reported");

    // THE TRAP THE ANCHOR EXISTS FOR. "SmartSDR v2.5.1+ Changelog" is a real
    // record; an unanchored match reads 2.5.1 out of it and publishes a
    // changelog as v2's newest release, beating the genuine 2.10.1.
    check(rel.value(2) != QStringLiteral("2.5.1"),
          "a changelog's name is not read as a release");
    check(FirmwareStager::parsePublishedReleases(
              index({record(QStringLiteral("SmartSDR v2.5.1+ Changelog"),
                            QStringLiteral("smartsdr_v2_changelog"))})).isEmpty(),
          "a changelog record alone yields no release");
    check(FirmwareStager::parsePublishedReleases(
              index({record(QStringLiteral("SmartSDR v4.x API (FlexLib)"),
                            QStringLiteral("smartsdr-v4-x-api-flexlib"))})).isEmpty(),
          "the API record is not a release");

    // The slug is read only when the title has drifted, so one edited title
    // does not cost us the release.
    check(newestInLine(index({record(QStringLiteral("SmartSDR v4.2.20 (Windows)"),
                                     QStringLiteral("smartsdr-v4-2-20"))}), 4)
              == QStringLiteral("4.2.20"),
          "an edited title falls back to the slug");

    // Lexicographic traps, per line.
    check(newestInLine(index({release(QStringLiteral("4.2.20")),
                              release(QStringLiteral("4.2.5"))}), 4)
              == QStringLiteral("4.2.20"),
          "4.2.20 beats 4.2.5 although it sorts lower as text");
    check(newestInLine(index({release(QStringLiteral("3.9.19")),
                              release(QStringLiteral("3.10.15"))}), 3)
              == QStringLiteral("3.10.15"),
          "3.10.15 beats 3.9.19 although it sorts lower as text");

    // INT OVERFLOW IN ANY COMPONENT (PR #6177 review, rfoust B1 / Jeremy B2).
    // fromString() does NOT return null for a later overflowing component: it
    // stops and returns the PREFIX, so "99.2147483648.0" becomes
    // QVersionNumber(99) and would out-rank every real release.
    check(newestInLine(index({release(QStringLiteral("99.2147483648.0")),
                              release(QStringLiteral("4.2.20"))}), 4)
              == QStringLiteral("4.2.20"),
          "a second-component overflow does not out-rank a real release");
    check(FirmwareStager::parsePublishedReleases(
              index({release(QStringLiteral("99.2147483648.0"))})).isEmpty(),
          "a second-component overflow yields no release at all");
    check(FirmwareStager::parsePublishedReleases(
              index({release(QStringLiteral("4.2.2147483648"))})).isEmpty(),
          "a third-component overflow yields no release at all");
    check(FirmwareStager::parsePublishedReleases(
              index({release(QStringLiteral("2147483648.0.0"))})).isEmpty(),
          "a first-component overflow yields no release at all");

    // An index that says nothing useful must produce nothing.
    check(FirmwareStager::parsePublishedReleases(index({})).isEmpty(),
          "an empty index names no release");
    check(FirmwareStager::parsePublishedReleases(
              index({record(QStringLiteral("SmartSDR v4.2"),
                            QStringLiteral("smartsdr-v4-2"))})).isEmpty(),
          "a two-component version is not accepted as a release");

    // MALFORMED ANSWERS. The endpoint is public and we do not control it, so
    // every one of these must come back empty rather than crash or coerce.
    check(FirmwareStager::parsePublishedReleases(QByteArray()).isEmpty(),
          "an empty body names no release");
    check(FirmwareStager::parsePublishedReleases("<html>we have moved</html>").isEmpty(),
          "HTML where JSON was expected names no release");
    check(FirmwareStager::parsePublishedReleases(R"([{"title":)").isEmpty(),
          "truncated JSON names no release");
    check(FirmwareStager::parsePublishedReleases(R"({"code":"rest_no_route"})").isEmpty(),
          "a REST error object is not an array, so it names no release");
    check(FirmwareStager::parsePublishedReleases(R"(["SmartSDR v4.2.20"])").isEmpty(),
          "a bare string where a record was expected names no release");
    check(FirmwareStager::parsePublishedReleases(
              R"([{"title":"SmartSDR v4.2.20","slug":"smartsdr-v4-2-20"}])")
              .value(4) == QStringLiteral("4.2.20"),
          "a title that is a string rather than an object falls back to the slug");
    check(FirmwareStager::parsePublishedReleases(
              R"([{"title":{"rendered":4},"slug":42}])").isEmpty(),
          "numbers where strings were expected name no release");
}

// RADIO SETUP'S LIST. The button shows every release FlexRadio publishes, not
// just the newest, so an operator can see which line they are on and what else
// exists. parsePublishedReleases() is DERIVED from this list, so the two can
// never disagree about what counts as a release.
void checkEveryPublishedReleaseIsListedNewestFirst()
{
    const QByteArray real = index({
        release(QStringLiteral("4.2.20")),
        record(QStringLiteral("SmartSDR v4 Changelog"),
               QStringLiteral("smartsdr_v4_changelog")),
        release(QStringLiteral("4.2.18")),
        release(QStringLiteral("4.1.5")),
        release(QStringLiteral("3.10.15")),
        release(QStringLiteral("2.10.1")),
    });

    const QStringList all = FirmwareStager::parseAllReleases(real);
    check(all == QStringList({QStringLiteral("4.2.20"), QStringLiteral("4.2.18"),
                              QStringLiteral("4.1.5"), QStringLiteral("3.10.15"),
                              QStringLiteral("2.10.1")}),
          "every release is listed, newest first, with non-releases dropped");

    // ORDER IS NUMERIC, not lexicographic. The dialog shows this list top-down
    // and takes the first entry per line as that line's newest, so a text sort
    // would both mis-order the rows and mis-mark the recommended release.
    check(FirmwareStager::parseAllReleases(
              index({release(QStringLiteral("4.2.5")), release(QStringLiteral("4.2.20"))}))
              == QStringList({QStringLiteral("4.2.20"), QStringLiteral("4.2.5")}),
          "4.2.20 sorts above 4.2.5 although it sorts lower as text");
    check(FirmwareStager::parseAllReleases(
              index({release(QStringLiteral("3.9.19")), release(QStringLiteral("3.10.15"))}))
              == QStringList({QStringLiteral("3.10.15"), QStringLiteral("3.9.19")}),
          "3.10.15 sorts above 3.9.19 although it sorts lower as text");

    // A release named twice (title AND a duplicate record) is one release.
    check(FirmwareStager::parseAllReleases(
              index({release(QStringLiteral("4.2.20")), release(QStringLiteral("4.2.20"))}))
              == QStringList({QStringLiteral("4.2.20")}),
          "a release named twice is listed once");

    // The derived map must agree with the list it came from.
    const auto map = FirmwareStager::parsePublishedReleases(real);
    check(map.value(4) == QStringLiteral("4.2.20")
              && map.value(3) == QStringLiteral("3.10.15")
              && map.value(2) == QStringLiteral("2.10.1"),
          "the per-major map is the newest entry of each line in the list");
}

// WHICH ROW THE LIST CALLS OUT. Pure, so the decision is pinned without
// constructing a dialog: a v3 radio must be pointed at the newest v3, never at
// the newest release overall.
void checkTheListMarksTheRadiosOwnLine()
{
    const QStringList all = {QStringLiteral("4.2.20"), QStringLiteral("4.2.18"),
                             QStringLiteral("4.1.5"), QStringLiteral("3.10.15"),
                             QStringLiteral("2.10.1")};
    using AetherSDR::FirmwareCurrency::newestInLine;

    check(newestInLine(all, QStringLiteral("3.9.18.36988")) == QStringLiteral("3.10.15"),
          "a v3 radio is pointed at the newest v3, not at 4.2.20");
    check(newestInLine(all, QStringLiteral("4.1.5.39794")) == QStringLiteral("4.2.20"),
          "a v4 radio is pointed at the newest v4 across its whole line");
    check(newestInLine(all, QStringLiteral("4.2.20.41343")) == QStringLiteral("4.2.20"),
          "a radio already on the newest release is pointed at it");
    check(newestInLine(all, QStringLiteral("1.12.1")).isEmpty(),
          "a line with no published release marks nothing");
    check(newestInLine(all, QString()).isEmpty(),
          "a radio that reports no version marks nothing");
    check(newestInLine({}, QStringLiteral("4.2.20.41343")).isEmpty(),
          "an empty list marks nothing");
}

// THE SMARTLINK FIX (PR #6177 review nit). The lookup used to hang off the Flex
// backend's own RadioConnection, which a SmartLink session never dials — so a
// WAN operator got no verdict at all. It now hangs off the seam's
// transport-neutral session verb.
//
// Driven through `onRadioSessionEstablished()` with a FRESH cache already in the
// store, so the verb's wiring is proved without any network: a stale cache would
// build a FirmwareStager and reach for flexradio.com, a fresh one must not.
void checkTheSessionVerbAdoptsTheCacheWithoutAsking()
{
    QJsonObject releases;
    releases.insert(QStringLiteral("3"), QStringLiteral("3.10.15"));
    releases.insert(QStringLiteral("4"), QStringLiteral("4.2.20"));
    QJsonObject doc;
    doc.insert(QLatin1String("releases"), releases);
    doc.insert(QLatin1String("checkedAt"),
               QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    AetherSDR::AppSettings::instance().setRadioFeature(
        QStringLiteral("flex"), QString(), QStringLiteral("publishedFirmware"), 2, doc);

    auto backend = std::make_unique<AetherSDR::FlexBackend>();
    backend->onRadioSessionEstablished();

    const auto source = backend->capabilities().firmwareUpdateSource;
    check(source.has_value(), "the record is still declared");
    if (!source.has_value())
        return;

    check(source->publishedReleases.value(3) == QStringLiteral("3.10.15")
              && source->publishedReleases.value(4) == QStringLiteral("4.2.20"),
          "a session adopts the cached releases, whatever transport carried it");
    check(backend->findChild<AetherSDR::FirmwareStager*>() == nullptr,
          "a fresh cache means the session asks flexradio.com nothing");
    // And the verdict that follows is the one the SmartLink radio needed.
    check(evaluate(source->publishedReleases, QStringLiteral("3.9.18.36988"))
              == Status::Outdated,
          "a WAN-connected v3 radio now gets its verdict from the cache");
}

// THE CACHE-SCHEMA BLOCKER (PR #6177, @ten9876's second review).
//
// The completion handler used to capture the schema read at the START of the
// lookup, and that connection is made once, so every later session judged its
// write against the FIRST session's answer. A newer document written in between
// — by another client, or while a request was in flight — was silently replaced.
//
// Driven by injecting a completed lookup, so no HTTP peer is needed.
void checkANewerCacheDocumentSurvivesARepeatLookup()
{
    const QString kFamily = QStringLiteral("flex");
    const QString kFeature = QStringLiteral("publishedFirmware");
    auto& settings = AetherSDR::AppSettings::instance();

    const QMap<int, QString> releases = {{4, QStringLiteral("4.2.20")}};
    auto backend = std::make_unique<AetherSDR::FlexBackend>();

    // First lookup, nothing cached: this build's schema is written.
    AetherSDR::FlexBackendTestAccess::completeLookup(*backend, releases);
    int schema = 0;
    settings.radioFeatureExact(kFamily, QString(), kFeature, &schema);
    check(schema == 2, "a first lookup writes this build's schema");

    // Now a NEWER client writes a document this build does not understand —
    // between one lookup and the next, which is the window the old capture
    // could not see.
    QJsonObject future;
    future.insert(QStringLiteral("futureField"), QStringLiteral("must survive"));
    check(settings.setRadioFeature(kFamily, QString(), kFeature, 3, future),
          "the newer-schema document was written for the attack");

    // Second lookup on the SAME backend — the one that used to clobber it.
    AetherSDR::FlexBackendTestAccess::completeLookup(*backend, releases);

    int after = 0;
    const QJsonObject doc = settings.radioFeatureExact(kFamily, QString(), kFeature, &after);
    check(after == 3,
          "a repeat lookup leaves the newer schema alone");
    check(doc.value(QStringLiteral("futureField")).toString()
              == QStringLiteral("must survive"),
          "the newer client's document survives a repeat lookup intact");
}

} // namespace

int main(int argc, char** argv)
{
    // An isolated, writable settings store: the cache test below writes a radio
    // feature document, and must not touch the operator's real settings.
    // Constructed before QCoreApplication, as the fixture requires.
    TestSettingsProfile profile(QStringLiteral("aether-firmware-currency"));
    if (!profile.isValid()) {
        std::fprintf(stderr, "FAIL: could not create an isolated settings profile\n");
        return 1;
    }
    QCoreApplication app(argc, argv);
    AetherSDR::AppSettings::instance().load();
    checkTheVerdictIsScopedToTheRadiosOwnLine();
    checkTheUpgradeTargetIsTheOwnLineRelease();
    checkTheBuildNumberIsIgnored();
    checkComparisonIsNumericNotLexicographic();
    checkNewerThanPublishedIsNotOutdated();
    checkNothingPublishedMeansNoVerdict();
    checkUnparseableRadioVersionsAreUnknown();
    checkTooltipsMatchTheState();
    checkWhenTheCachedAnswerIsLookedUpAgain();
    checkTheBackendDeclaresTheRecordAndAsksNobodyAtConstruction();
    checkTheSoftwareIndexIsReadIntoLines();
    checkEveryPublishedReleaseIsListedNewestFirst();
    checkTheListMarksTheRadiosOwnLine();
    checkTheSessionVerbAdoptsTheCacheWithoutAsking();
    checkANewerCacheDocumentSurvivesARepeatLookup();
    return g_failures == 0 ? 0 : 1;
}
