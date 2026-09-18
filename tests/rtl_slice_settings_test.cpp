#include "TestSettingsProfile.h"
#include "core/RtlSliceSettings.h"
#include "core/BandStackSettings.h"
#include "core/RadioStateMemory.h"
#include "core/SettingsDatabase.h"
#include "core/SettingsPaths.h"
#include "models/RadioModel.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>

#include <iostream>
#include <limits>

using namespace AetherSDR;
namespace Policy = SharedCapturePolicy;
namespace AetherSDR {
// Existing model friendship. The injected backend owns no transport and no
// peer; retry invokes the production timer handler synchronously.
struct RadioModelWakeTestAccess {
    static void pending(RadioModel& model) { model.scheduleOperatingStateSave(); }
    static void retry(RadioModel& model)
    {
        QMetaObject::invokeMethod(&model.m_reconnectTimer, "timeout", Qt::DirectConnection);
    }
};
}
namespace {
int failures = 0;
void check(bool success, const char* message)
{
    std::cout << (success ? "[ OK ] " : "[FAIL] ") << message << '\n';
    failures += !success;
}

RtlSliceSettings::Slice slice(int id, double frequency = 100'000'000.0)
{
    RtlSliceSettings::Slice result;
    result.id = id;
    result.frequencyHz = frequency;
    result.mode = QStringLiteral("FM");
    result.filterLowHz = -8000;
    result.filterHighHz = 8000;
    return result;
}

QJsonObject legacy()
{
    return {{"rfFrequencyHz", 100'000'000.0}, {"mode", "FM"},
            {"filterLowHz", -8000}, {"filterHighHz", 8000}, {"sampleRateHz", 2'400'000},
            {"ext", QJsonObject{{"rfGain", QJsonObject{{"gainDb", 24}}}}},
            {"futureLegacy", "keep"}};
}

class SettingsBackend final : public IRadioBackend {
public:
    RadioConnectRequest request;
    RestoredRadioState restored;
    RestoredRadioState live;
    bool connected = false;
    int connections = 0;
    RadioCapabilities capabilities() const override
    {
        RadioCapabilities caps;
        caps.family = QStringLiteral("rtl");
        caps.clientSettingsDomains = RadioCapabilities::ClientSettingsDomain::Tuning
                                   | RadioCapabilities::ClientSettingsDomain::Passband
                                   | RadioCapabilities::ClientSettingsDomain::SpanRate
                                   | RadioCapabilities::ClientSettingsDomain::RfGain;
        return caps;
    }
    void applyRestoredState(const RestoredRadioState& state) override { restored = state; live = state; }
    RestoredRadioState currentOperatingState() const override { return live; }
    void connectRadio(const RadioConnectRequest& value) override
    {
        request = value;
        connected = true;
        ++connections;
    }
    void disconnectRadio() override { connected = false; emit disconnected(); }
    bool isConnected() const override { return connected; }
    void setSliceFrequency(int, double) override {}
    void setSliceMode(int, const QString&) override {}
    void setSliceFilter(int, int, int) override {}
    void setSliceAgc(int, const QString&, int) override {}
    void setPanCenter(const QString&, double, PanCenterIntent) override {}
    void setKeying(bool, const AetherSDR::TxCoordinator::Operation&, const AetherSDR::TxCoordinator::Completion&) override {}
    void invokeExtension(const QString&, const QString&, quint64, const QVariant&) override {}
};
} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("rtl-slice-settings"));
    if (!profile.isValid()) {
        return 1;
    }
    QCoreApplication app(argc, argv);
    AppSettings& settings = AppSettings::instance();
    settings.load();
    using Read = RtlSliceSettings::ReadStatus;
    using Migration = RtlSliceSettings::MigrationResult;
    const QString feature = RtlSliceSettings::featureName();
    const RadioSerialIdentity numeric{QStringLiteral("0"), false};
    const RadioSerialIdentity duplicate{QStringLiteral("0"), true};
    check(settingsRadioId("rtl", "0", numeric) == "0", "real numeric serial is identity");
    check(settingsRadioId("rtl", "rtl:7", duplicate) == "0", "duplicate reported serial shares real identity");
    check(settingsRadioId("rtl", "rtl:1", {}) == "", "index locator never becomes identity");
    check(settingsRadioId("rtl", "rtl:8", duplicate)
              == settingsRadioId("rtl", "0", numeric), "USB reorder preserves reported identity");
    check(settingsRadioId("flex", "flex-A", duplicate) == "flex-A", "Flex scope unchanged");
    check(settingsRadioId("hl2", "MAC-A", {}) == "MAC-A", "HL2 scope unchanged");

    const RadioSettingsScope family("rtl", "");
    const RadioSettingsScope a("rtl", "0");
    const RadioSettingsScope b("rtl", "B");
    RtlSliceSettings owner(a);
    check(owner.load().status == Read::Missing, "new scope has no document");
    check(RtlSliceSettings(family).patch(100'000'000, 2'400'000, {slice(3)}), "family default written");
    check(owner.load().document.slices.contains(3), "missing exact reads family default");
    check(owner.patch(100'000'000, 2'400'000, {slice(0), slice(7, 101'000'000)}), "sparse exact document written");
    check(!owner.load().document.slices.contains(3), "exact writer does not clone family default");
    check(a.setFeature(feature, 1, {}), "empty exact row planted");
    check(owner.load().status == Read::Ready && owner.load().document.slices.isEmpty(),
          "empty exact row suppresses family fallback");
    check(a.setFeature(RadioStateMemory::featureName(), 2, legacy()), "legacy source planted");
    check(owner.migrateLegacy(numeric) == Migration::Settled, "empty exact row blocks reimport");
    check(a.removeFeature(feature), "remove exact test document");
    check(owner.migrateLegacy(numeric) == Migration::Settled, "effective new-format family document blocks legacy import");
    check(family.setFeature(feature, 1, {}), "empty family document planted");
    check(owner.migrateLegacy(numeric) == Migration::Settled, "empty family document also blocks legacy import");
    check(family.setFeature(feature, 2, {}), "newer family document planted");
    check(owner.migrateLegacy(numeric) == Migration::Retry, "newer family document refuses migration");
    check(family.removeFeature(feature), "remove family fixture before exact legacy claim");
    check(owner.migrateLegacy(numeric) == Migration::Claimed, "exact trustworthy source claimed");
    check(owner.load().document.slices.value(0).frequencyHz == 100'000'000,
          "migration uses exact legacy Hz rather than family default");
    check(owner.migrateLegacy(numeric) == Migration::Settled, "migration happens once");
    check(a.featureExact(RadioStateMemory::featureName()) == legacy(), "migration preserves whole legacy snapshot");

    QJsonObject stored = a.featureExact(feature);
    stored.insert("unknownTop", QJsonObject{{"value", 1}});
    QJsonObject entries = stored.value("slices").toObject();
    QJsonObject entry = entries.value("0").toObject();
    entry.insert("unknownSlice", true);
    QJsonObject squelch = entry.value("squelch").toObject();
    squelch.insert("unknownSquelch", 9);
    entry.insert("squelch", squelch);
    entries.insert("0", entry);
    stored.insert("slices", entries);
    check(a.setFeature(feature, 1, stored), "unknown members planted");
    check(owner.patch(100'000'000, 2'400'000, {slice(0), slice(7, 101'000'000)}), "patch preserves extensible document");
    check(owner.patch(100'000'000, 2'400'000, {slice(0)}), "capacity-limited capture patches only accepted entries");
    stored = a.featureExact(feature);
    entries = stored.value("slices").toObject();
    entry = entries.value("0").toObject();
    check(entries.contains("7") && stored.contains("unknownTop") && entry.contains("unknownSlice")
              && entry.value("squelch").toObject().contains("unknownSquelch"),
          "omitted stable IDs and all unknown members survive");
    check(owner.patch(100'000'000, 2'400'000, {}, {7}), "explicit removal succeeds");
    check(!owner.load().document.slices.contains(7), "only explicitly removed ID disappears");
    check(!owner.patch(100'000'000, 2'400'000, {slice(0), slice(0)}), "duplicate patch rejected");
    check(!owner.patch(100'000'000, 2'400'000, {slice(0)}, {0}), "conflicting removal rejected");
    check(!owner.patch(100'000'000, 2'400'000, {slice(-1)}), "negative stable ID rejected");
    check(!owner.patch(100'000'000, 2'400'000, {slice(4096)}), "representation ceiling enforced");
    QVector<RtlSliceSettings::Slice> oversized(4097, slice(0));
    check(!owner.patch(100'000'000, 2'400'000, oversized), "oversized patch bounded before iteration");
    check(!owner.patch(std::numeric_limits<double>::infinity(), 2'400'000, {}), "nonfinite context rejected");
    RtlSliceSettings::Slice fractional = slice(0, 0.5);
    fractional.filterLowHz = -0.1;
    fractional.filterHighHz = 0.1;
    check(RtlSliceSettings(b).patch(0.5, 0.25, {fractional}), "positive fractional Hz remains representable");

    RtlSliceSettings::Document decoded;
    QString reason;
    const QJsonObject valid = a.featureExact(feature);
    for (const QString& key : {QStringLiteral("freq"), QStringLiteral("filterLow"),
                               QStringLiteral("audioGain"), QStringLiteral("audioMute")}) {
        QJsonObject bad = valid;
        QJsonObject badEntries = bad.value("slices").toObject();
        QJsonObject badEntry = badEntries.value("0").toObject();
        badEntry.insert(key, QStringLiteral("wrong type"));
        badEntries.insert("0", badEntry);
        bad.insert("slices", badEntries);
        decoded.captureCenterHz = 42;
        check(!RtlSliceSettings::decode(bad, decoded, reason) && decoded.captureCenterHz == 42,
              "malformed field rejects entire document without partial output");
    }
    QJsonObject bad = valid;
    entries = bad.value("slices").toObject();
    entries.insert("00", entries.take("0"));
    bad.insert("slices", entries);
    check(!RtlSliceSettings::decode(bad, decoded, reason), "noncanonical numeric ID rejected");

    QJsonObject overwrittenBad = valid;
    entries = overwrittenBad.value("slices").toObject();
    entry = entries.value("0").toObject();
    entry.insert("freq", QStringLiteral("wrong type"));
    entries.insert("0", entry);
    overwrittenBad.insert("slices", entries);
    check(a.setFeature(feature, 1, overwrittenBad), "malformed replacement-target field planted");
    check(!owner.patch(100'000'000, 2'400'000, {slice(0)})
              && a.featureExact(feature) == overwrittenBad,
          "patch validates old document even when replacement would repair its malformed field");

    check(a.setFeature(feature, 2, valid), "future schema planted");
    check(owner.load().status == Read::Refused && !owner.patch(100'000'000, 2'400'000, {slice(0)})
              && owner.migrateLegacy(numeric) == Migration::Retry, "future schema never overwritten or marked migrated");
    check(a.setFeature(feature, 1, bad), "malformed typed document planted");
    check(owner.migrateLegacy(numeric) == Migration::Retry, "malformed existing document is refused");
    {
        SettingsDatabase raw;
        check(raw.open(SettingsPaths::databasePath()), "raw database fixture opens");
        check(raw.upsertRadioFeature("rtl", "0", feature, 99, "{not json"), "corrupt future-schema row planted");
        raw.close();
    }
    int version = 0;
    AppSettings::FeatureReadStatus status;
    a.featureExact(feature, &version, &status);
    check(version == 99 && status == AppSettings::FeatureReadStatus::Corrupt,
          "raw schema and corrupt presence survive parse failure");
    check(owner.load().status == Read::Refused && owner.migrateLegacy(numeric) == Migration::Retry
              && !owner.patch(100'000'000, 2'400'000, {slice(0)}), "corrupt exact never falls back or reimports");
    check(RtlSliceSettings(family).patch(100'000'000, 2'400'000, {slice(3)}), "family fallback fixture restored");
    check(settings.radioFeature("rtl", "0", feature).contains("slices"),
          "legacy effective-reader corrupt fallback behavior unchanged");
    check(a.removeFeature(feature), "remove corrupt fixture");
    check(family.removeFeature(feature), "remove family fixture before retry migration");

    // An actual refused transaction must not destroy the source or latch completion.
    {
        SettingsDatabase locker;
        check(locker.open(SettingsPaths::databasePath()) && locker.begin(), "hold competing SQLite write transaction");
        check(owner.migrateLegacy(numeric) == Migration::Retry, "locked write refuses migration");
        check(a.featureExact(RadioStateMemory::featureName()) == legacy(), "refused migration retains source");
        locker.rollback();
        locker.close();
    }
    check(owner.migrateLegacy(numeric) == Migration::Claimed, "migration retries after write lock clears");
    RestoredRadioState gain;
    gain.extensionSchemaVersion = 1;
    gain.extension.insert("rfGain", QJsonObject{{"gainDb", 29}});
    check(RadioStateMemory::storeRtlRfGainPreservingLegacy(a, gain), "future cutover gain writer succeeds");
    QJsonObject afterGain = a.featureExact(RadioStateMemory::featureName());
    afterGain.insert("ext", legacy().value("ext"));
    afterGain.remove("extVersion");
    check(afterGain == legacy(), "later RF-gain write retains frozen tuning snapshot and unknown members");
    QJsonObject gainSnapshot = a.featureExact(RadioStateMemory::featureName());
    QJsonObject gainExtension = gainSnapshot.value("ext").toObject();
    QJsonObject gainFields = gainExtension.value("rfGain").toObject();
    gainFields.insert("futureGainField", QJsonObject{{"calibration", 17}});
    gainExtension.insert("rfGain", gainFields);
    gainExtension.insert("futureDomain", QJsonObject{{"value", true}});
    gainSnapshot.insert("ext", gainExtension);
    check(a.setFeature(RadioStateMemory::featureName(), 2, gainSnapshot), "unknown nested gain field fixture stored");
    gain.extension.insert("rfGain", QJsonObject{{"gainDb", 31}});
    check(RadioStateMemory::storeRtlRfGainPreservingLegacy(a, gain), "known gain updates inside extensible legacy document");
    gainFields.insert("gainDb", 31);
    gainExtension.insert("rfGain", gainFields);
    gainSnapshot.insert("ext", gainExtension);
    check(a.featureExact(RadioStateMemory::featureName()) == gainSnapshot,
          "unknown rfGain members and extension domains remain byte-equivalent JSON values");
    for (const QJsonValue& extensionVersion : {
             QJsonValue(2), QJsonValue("1"), QJsonValue(0), QJsonValue(-1), QJsonValue(1.5),
             QJsonValue(QJsonValue::Null)}) {
        QJsonObject invalidVersion = gainSnapshot;
        invalidVersion.insert("extVersion", extensionVersion);
        check(a.setFeature(RadioStateMemory::featureName(), 2, invalidVersion), "unsupported extension-version fixture stored");
        check(!RadioStateMemory::storeRtlRfGainPreservingLegacy(a, gain)
                  && a.featureExact(RadioStateMemory::featureName()) == invalidVersion,
              "future/malformed extension version refuses gain update without changing snapshot");
    }
    check(a.setFeature(RadioStateMemory::featureName(), 2, gainSnapshot), "restore supported extension fixture");
    gain.extensionSchemaVersion = 2;
    check(!RadioStateMemory::storeRtlRfGainPreservingLegacy(a, gain)
              && a.featureExact(RadioStateMemory::featureName()) == gainSnapshot,
          "unknown incoming extension version also refuses mutation");
    gain.extensionSchemaVersion = 1;
    check(!RadioStateMemory::storeRtlRfGainPreservingLegacy(RadioSettingsScope("flex", "A"), gain),
          "RTL-only cutover helper refuses other families");
    const RadioSettingsScope ordinary("hl2", "gain-policy");
    check(ordinary.setFeature(RadioStateMemory::featureName(), 2, legacy()), "ordinary domain-gating fixture stored");
    RadioCapabilities gainCaps;
    gainCaps.clientSettingsDomains = RadioCapabilities::ClientSettingsDomain::RfGain;
    check(RadioStateMemory::store(ordinary, gainCaps, gain)
              && !ordinary.featureExact(RadioStateMemory::featureName()).contains("rfFrequencyHz"),
          "ordinary non-RTL writer still drops undeclared domains rather than freezing them");
    const RadioSettingsScope index("rtl", "rtl:2");
    check(index.setFeature(RadioStateMemory::featureName(), 2, legacy()), "ambiguous legacy index row planted");
    check(RtlSliceSettings(index).migrateLegacy({"rtl:2", false}) == Migration::Retry,
          "new real serial matching old index spelling cannot claim ambiguous source");
    check(RtlSliceSettings(index).migrateLegacy({}) == Migration::Retry, "unproven index scope refused");
    const RadioSettingsScope missing("rtl", "missing");
    check(RtlSliceSettings(missing).migrateLegacy({"missing", false}) == Migration::NoSource,
          "migration never imports another scope through fallback");

    check(owner.patch(100'000'000, 2'400'000, {slice(0), slice(7, 101'000'000)}), "restore fixture written");
    const RtlSliceSettings::Document saved = owner.load().document;
    const Policy::CaptureDescriptor actual{1, 1, 100'000'000, 2'400'000, 1'100'000, 1'100'000};
    const std::vector<Policy::CenterDomain> domains{{1, 2'000'000'000, 0, 1}};
    std::vector<Policy::SliceDescriptor> prepared{{7, 101'000'000, -8000, 8000, 0, 100, 100},
                                                {0, 100'000'000, -8000, 8000, 0, 100, 100}};
    auto plan = RtlSliceSettings::planRestore(saved, actual, prepared, domains, {8, 1});
    check(plan.error == Policy::Error::None && plan.accepted.size() == 1
              && plan.accepted.front().stableId == 0 && plan.rejected.size() == 1,
          "production F2 planner selects ascending sparse ID capacity");
    prepared.front().guardHighHz = 200'000;
    plan = RtlSliceSettings::planRestore(saved, actual, prepared, domains, {8, 8});
    check(plan.accepted.size() == 1 && plan.rejected.front().reason == Policy::Error::OutsideCapture,
          "full guard interval rejects slice without recentering");
    prepared.front().carrierHz = 100'999'999;
    check(RtlSliceSettings::planRestore(saved, actual, prepared, domains, {8, 8}).accepted.empty(),
          "adapter cannot silently modify saved frequency");
    plan = RtlSliceSettings::planRestore(saved, actual, {}, domains, {8, 8});
    check(plan.error != Policy::Error::None && plan.accepted.empty(), "missing mode/guard descriptor fails closed");
    check(owner.load().document.slices.size() == 2, "planning never rewrites rejected entries");
    settings.reset();
    check(owner.migrateLegacy(numeric) == Migration::Retry, "closed store does not appear absent");
    settings.load();
    check(owner.load().document.slices.size() == 2, "document survives database reopen");

    // Actual production model connection, retry, pending-save and scope-swap
    // paths. The backend records calls; it never opens USB/network/audio I/O.
    {
        RadioModel model;
        auto backend = std::make_unique<SettingsBackend>();
        SettingsBackend* injected = backend.get();
        model.setBackendForTest(std::move(backend), QStringLiteral("rtl"));
        QObject::connect(injected, &IRadioBackend::disconnected, &model,
                         &RadioModel::flushPendingOperatingState);
        const RadioSettingsScope modelA("rtl", "123");
        const RadioSettingsScope modelB("rtl", "456");
        check(modelA.setFeature(RadioStateMemory::featureName(), 2,
                               QJsonObject{{"rfFrequencyHz", 98'000'000.0}, {"mode", "FM"}}),
              "model initial-restore fixture stored");
        RadioInfo info;
        info.family = "rtl";
        info.serial = "rtl:5";
        info.serialIdentity = {"123", true};
        info.address = QHostAddress(QStringLiteral("127.0.0.1")); // Injected request only.
        model.connectToRadio(info);
        check(model.settingsScope().radioId() == "123"
                  && injected->restored.rfFrequencyHz == 98'000'000.0
                  && injected->request.serial == "rtl:5"
                  && injected->request.serialIdentity.reportedSerial == "123",
              "production preconnect resolves reported scope and retains locator provenance");
        injected->live.rfFrequencyHz = 98'100'000.0;
        RadioModelWakeTestAccess::pending(model);
        RadioInfo next = info;
        next.serial = "rtl:1";
        next.serialIdentity = {"456", true};
        model.connectToRadio(next);
        check(modelA.featureExact(RadioStateMemory::featureName()).value("rfFrequencyHz").toDouble()
                  == 98'100'000.0 && injected->restored.rfFrequencyHz == 0,
              "same-family swap flushes old accepted snapshot before new empty restore");
        check(modelB.featureExact(RadioStateMemory::featureName()).isEmpty(),
              "old-session pending save never writes new scope");
        injected->live.rfFrequencyHz = 99'500'000.0;
        RadioModelWakeTestAccess::pending(model);
        injected->disconnectRadio();
        check(modelB.featureExact(RadioStateMemory::featureName()).value("rfFrequencyHz").toDouble()
                  == 99'500'000.0, "disconnect flush uses the same reported identity");
        RadioModelWakeTestAccess::retry(model);
        check(injected->connections == 3 && injected->restored.rfFrequencyHz == 99'500'000.0
                  && injected->request.serialIdentity.reportedSerial == "456",
              "production retry restores saved scope and forwards identity provenance");
        next.serial = "rtl:2";
        next.serialIdentity = {};
        model.connectToRadio(next);
        check(model.settingsScope().radioId().isEmpty(), "unidentified production session uses RTL family scope");
        BandStackSettings& bookmarks = BandStackSettings::instance();
        BandStackEntry manual;
        manual.frequencyMhz = 100.1;
        manual.mode = QStringLiteral("WFM");
        const RadioSettingsScope oldBookmarks("rtl", "rtl:2");
        bookmarks.addEntry(oldBookmarks, manual);
        bookmarks.addEntry(model.settingsScope(), manual);
        check(bookmarks.entries(model.settingsScope()).size() == 1,
              "connected anonymous RTL accepts a manual bookmark");
        BandStackEntry automatic = manual;
        automatic.autoSaved = true;
        automatic.createdAtMs = QDateTime::currentMSecsSinceEpoch() - 60'000;
        bookmarks.addEntry(model.settingsScope(), automatic);
        check(bookmarks.entries(model.settingsScope()).size() == 2,
              "connected anonymous RTL accepts an automatic bookmark");
        settings.reset();
        settings.load();
        check(bookmarks.entries(model.settingsScope()).size() == 2,
              "anonymous bookmarks survive database reopen");
        check(bookmarks.removeExpiredEntries(model.settingsScope(), 1000) == 1
                  && bookmarks.entries(model.settingsScope()).size() == 1,
              "anonymous automatic bookmark expires without removing manual entry");
        check(bookmarks.entries(oldBookmarks).size() == 1,
              "anonymous writes leave old locator bookmarks intact and unclaimed");
        injected->disconnectRadio();
        bookmarks.addEntry(model.settingsScope(), manual);
        bookmarks.clearAllEntries(model.settingsScope());
        check(!model.settingsScope().hasRadioIdentity()
                  && bookmarks.entries(model.settingsScope()).isEmpty()
                  && family.featureExact(BandStackSettings::featureName())
                         .value("entries").toArray().size() == 1,
              "disconnected anonymous model cannot read add or clear family bookmarks");
        model.connectToRadio(next);
        check(bookmarks.entries(model.settingsScope()).size() == 1,
              "anonymous reconnect recovers shared bookmarks");
        bookmarks.removeEntry(model.settingsScope(), 0);
        check(bookmarks.entries(model.settingsScope()).isEmpty(),
              "anonymous bookmark removal writes through");

        check(injected->restored.rfFrequencyHz == 0
                  && injected->restored.mode.isEmpty()
                  && injected->restored.filterLowHz == 0
                  && injected->restored.filterHighHz == 0
                  && injected->restored.sampleRateHz == 0
                  && injected->restored.extension.isEmpty(),
              "only old locator-keyed state yields empty restore when family row is absent");
        check(index.featureExact(RadioStateMemory::featureName()) == legacy(),
              "old locator-keyed state remains intact after anonymous connection");

        // RFC #5468 explicitly chooses a shared family row for absent identity.
        // Exercise its consequence through the current production owner, not
        // just settingsRadioId(): an unseen reported serial inherits that row.
        injected->live.rfFrequencyHz = 100'100'000.0;
        injected->live.mode = QStringLiteral("WFM");
        injected->live.filterLowHz = -100'000;
        injected->live.filterHighHz = 100'000;
        injected->live.sampleRateHz = 2'400'000;
        injected->live.extensionSchemaVersion = 1;
        injected->live.extension = QJsonObject{{"rfGain", QJsonObject{{"gainDb", 24}}}};
        RadioModelWakeTestAccess::pending(model);
        const RadioSettingsScope unseen("rtl", "789");
        check(unseen.featureExact(RadioStateMemory::featureName()).isEmpty(),
              "different reported serial has no exact OperatingState fixture");
        next.serial = "789";
        next.serialIdentity = {"789", false};
        model.connectToRadio(next);
        check(family.featureExact(RadioStateMemory::featureName()).value("rfFrequencyHz").toDouble()
                  == 100'100'000.0,
              "anonymous pending save writes the family row before identified connection");
        check(model.settingsScope().radioId() == "789"
                  && injected->restored.rfFrequencyHz == 100'100'000.0
                  && injected->restored.mode == "WFM"
                  && injected->restored.filterLowHz == -100'000
                  && injected->restored.filterHighHz == 100'000
                  && injected->restored.sampleRateHz == 2'400'000
                  && injected->restored.extension.value("rfGain").toObject().value("gainDb").toInt() == 24,
              "unseen identified dongle restores anonymous frequency mode passband rate and gain via family fallback");
        check(unseen.featureExact(RadioStateMemory::featureName()).isEmpty(),
              "effective restore alone does not create an exact identified row");

        injected->live.rfFrequencyHz = 102'300'000.0;
        RadioModelWakeTestAccess::pending(model);
        next.serial = "rtl:9";
        next.serialIdentity = {};
        model.connectToRadio(next);
        check(injected->restored.rfFrequencyHz == 100'100'000.0
                  && model.settingsScope().radioId().isEmpty(),
              "another anonymous locator restores the same shared family row");
        check(unseen.featureExact(RadioStateMemory::featureName()).value("rfFrequencyHz").toDouble()
                  == 102'300'000.0,
              "identified pending save creates its own exact row");
        next.serial = "789";
        next.serialIdentity = {"789", false};
        model.connectToRadio(next);
        check(injected->restored.rfFrequencyHz == 102'300'000.0,
              "exact identified OperatingState overrides the anonymous family row");

        injected->live.rfFrequencyHz = 103'400'000.0;
        RadioModelWakeTestAccess::pending(model);
        next.serial = "rtl:10";
        next.serialIdentity = {"789", true};
        model.connectToRadio(next);
        check(model.settingsScope().radioId() == "789"
                  && injected->request.serial == "rtl:10"
                  && injected->request.serialIdentity.reportedSerial == "789"
                  && injected->request.serialIdentity.indexLocator
                  && injected->restored.rfFrequencyHz == 103'400'000.0,
              "duplicate reported serial shares exact saved state while retaining index-locator provenance");

        // RtlSlices is not the active runtime owner in F3a. Test its approved
        // fallback and precedence separately, using the actual model scopes.
        check(RtlSliceSettings(family).patch(100'100'000, 2'400'000, {slice(0, 100'100'000)}),
              "anonymous RtlSlices family document written through owner");
        RtlSliceSettings modelOwner(model.settingsScope());
        check(modelOwner.load().document.slices.value(0).frequencyHz == 100'100'000,
              "reported serial with no exact RtlSlices inherits anonymous family document");
        check(modelOwner.patch(103'400'000, 2'400'000, {slice(0, 103'400'000)}),
              "reported-serial RtlSlices exact document written through model scope");
        check(modelOwner.load().document.slices.value(0).frequencyHz == 103'400'000
                  && RtlSliceSettings(family).load().document.slices.value(0).frequencyHz == 100'100'000,
              "exact RtlSlices overrides family without changing shared document");
        next.serial = "789";
        next.serialIdentity = {"789", false};
        model.connectToRadio(next);
        check(RtlSliceSettings(model.settingsScope()).load().document.slices.value(0).frequencyHz
                  == 103'400'000,
              "duplicate and original reported serial share the same exact RtlSlices document");
        check(index.featureExact(RadioStateMemory::featureName()) == legacy()
                  && index.featureExact(feature).isEmpty(),
              "old locator row stays preserved and unclaimed after all model saves and swaps");
    }
    {
        BandStackSettings& bookmarks = BandStackSettings::instance();
        BandStackEntry entry;
        entry.frequencyMhz = 101.5;
        const RadioSettingsScope identified("rtl", "bookmark-identified");
        bookmarks.addEntry(identified, entry);
        bookmarks.addEntry(RadioSettingsScope("rtl", ""), entry);
        bookmarks.addEntry(RadioSettingsScope("flex", ""), entry);
        check(bookmarks.entries(identified).size() == 1
                  && bookmarks.entries(RadioSettingsScope("rtl", "")).isEmpty()
                  && bookmarks.entries(RadioSettingsScope("flex", "")).isEmpty(),
              "identified bookmark scope works and ordinary empty scopes still refuse");
    }
    return failures == 0 ? 0 : 1;
}
