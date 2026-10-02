#include "gui/SplitQsySettings.h"
#include "gui/SplitQsyObservationPolicy.h"
#include "models/SliceModel.h"

#include <QCoreApplication>
#include <QJsonDocument>

#include <cstdio>

namespace {

int g_failures = 0;

void check(bool condition, const char* message)
{
    if (condition) {
        return;
    }
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++g_failures;
}

QJsonObject parseObject(const char* json)
{
    return QJsonDocument::fromJson(QByteArray(json)).object();
}

AetherSDR::SliceDelta frequencyObservation(double frequencyMhz)
{
    AetherSDR::SliceDelta delta;
    delta.frequency = frequencyMhz;
    return delta;
}

void testDefaults()
{
    const AetherSDR::SplitQsySettings settings;
    check(!settings.closeSplitOnQsy, "QSY split closure defaults off");
    check(settings.thresholdHz == 2000, "threshold defaults to 2000 Hz");

    const auto empty = AetherSDR::SplitQsySettings::fromJson({});
    check(!empty.closeSplitOnQsy, "empty settings keep QSY split closure off");
    check(empty.thresholdHz == 2000, "empty settings keep the 2000 Hz threshold");
}

void testRoundTrip()
{
    AetherSDR::SplitQsySettings settings;
    settings.closeSplitOnQsy = false;
    settings.thresholdHz = 5000;
    const auto restored =
        AetherSDR::SplitQsySettings::fromJson(settings.toJson());
    check(!restored.closeSplitOnQsy, "disabled setting round-trips");
    check(restored.thresholdHz == 5000, "threshold round-trips");
}

void testClampAndMalformedValues()
{
    const auto low = AetherSDR::SplitQsySettings::fromJson(
        parseObject(R"({"v":1,"thresholdHz":0})"));
    check(low.thresholdHz == 1, "threshold clamps to the 1 Hz minimum");

    const auto high = AetherSDR::SplitQsySettings::fromJson(
        parseObject(R"({"v":1,"thresholdHz":1e100})"));
    check(high.thresholdHz == 200000,
          "large threshold clamps to the 200000 Hz maximum before narrowing");

    const auto malformed = AetherSDR::SplitQsySettings::fromJson(
        parseObject(R"({"v":1,"closeSplitOnQsy":"yes","thresholdHz":"20"})"));
    check(!malformed.closeSplitOnQsy,
          "wrong-typed enable setting falls back to disabled");
    check(malformed.thresholdHz == 2000,
          "wrong-typed threshold falls back to 2000 Hz");

    const auto future = AetherSDR::SplitQsySettings::fromJson(
        parseObject(R"({"v":2,"closeSplitOnQsy":false,"thresholdHz":100})"));
    check(!future.closeSplitOnQsy,
          "unknown version is not partially read for same-named fields");
    check(future.thresholdHz == 2000, "unknown version uses current defaults");
}

void testQsyClosePolicy()
{
    AetherSDR::SplitQsySettings settings;
    settings.closeSplitOnQsy = true;
    check(!AetherSDR::shouldCloseSplitOnQsy(
              settings, true, true, 0.002, 0.0),
          "frequency change at the threshold keeps split active");
    check(AetherSDR::shouldCloseSplitOnQsy(
              settings, true, true, 0.002001, 0.0),
          "RX QSY beyond the threshold closes split");
    check(!AetherSDR::shouldCloseSplitOnQsy(
              settings, true, false, 14.005000, 14.000000),
          "TX slice QSY does not close split");

    auto disabled = settings;
    disabled.closeSplitOnQsy = false;
    check(!AetherSDR::shouldCloseSplitOnQsy(
              disabled, true, true, 14.005000, 14.000000),
          "disabled option leaves split active on RX QSY");
}

void testLocalTuneEchoAndIncrementalExternalQsy()
{
    AetherSDR::SplitQsySettings settings;
    settings.closeSplitOnQsy = true;
    AetherSDR::PendingSliceFrequencyEchoes pendingTuneEchoes;
    AetherSDR::SliceModel rx(0);
    rx.applyChanges(frequencyObservation(14.000));

    bool splitActive = true;
    double referenceFrequencyMhz = rx.frequency();
    int statusReports = 0;
    QObject::connect(&rx, &AetherSDR::SliceModel::frequencyCommandIssued, &rx,
                     [&](double frequencyMhz) {
        pendingTuneEchoes.record(rx.sliceId(), frequencyMhz, 1000);
    });
    QObject::connect(&rx, &AetherSDR::SliceModel::frequencyStatusReported, &rx,
                     [&](double frequencyMhz) {
        ++statusReports;
        if (pendingTuneEchoes.consume(rx.sliceId(), frequencyMhz, 1001)) {
            referenceFrequencyMhz = frequencyMhz;
            return;
        }
        if (AetherSDR::shouldCloseSplitOnQsyObservation(
                settings, splitActive, true, frequencyMhz,
                referenceFrequencyMhz)) {
            splitActive = false;
        }
    });

    rx.setFrequency(14.005);
    check(splitActive, "optimistic local tuning does not close split");
    check(statusReports == 0,
          "optimistic local tuning is not emitted as a radio status report");
    rx.applyChanges(frequencyObservation(14.005));
    check(splitActive,
          "radio echo of a local tune beyond threshold does not close split");
    check(pendingTuneEchoes.empty(),
          "radio status consumes the matching local tune expectation");

    rx.applyChanges(frequencyObservation(14.006));
    check(splitActive,
          "unmatched radio QSY below threshold keeps split active");
    check(std::abs(referenceFrequencyMhz - 14.006) < 1e-9,
          "small external QSY advances the comparison reference");

    rx.applyChanges(frequencyObservation(14.0079));
    check(splitActive,
          "successive small radio QSYs are compared to the latest reference");
    rx.applyChanges(frequencyObservation(14.0101));
    check(!splitActive,
          "radio QSY beyond threshold from latest reference closes split");

    rx.applyChanges(frequencyObservation(14.0101));
    check(statusReports == 5,
          "status frequency signal includes changed and same-value reports");
}

void testPendingTuneEchoesAreSliceSpecificAndExpire()
{
    AetherSDR::PendingSliceFrequencyEchoes pendingTuneEchoes;
    pendingTuneEchoes.record(1, 14.005, 1000);
    check(!pendingTuneEchoes.consume(2, 14.005, 1001),
          "a tune expectation cannot suppress another slice's status");
    check(!pendingTuneEchoes.consume(1, 14.005, 3000),
          "expired tune expectations do not suppress later radio changes");
    check(pendingTuneEchoes.empty(),
          "expired tune expectations are discarded");
}

void testTwoOutstandingSwapEchoesDoNotCloseSplit()
{
    AetherSDR::SplitQsySettings settings;
    settings.closeSplitOnQsy = true;
    AetherSDR::PendingSliceFrequencyEchoes pendingTuneEchoes;
    AetherSDR::SliceModel rx(0);
    rx.applyChanges(frequencyObservation(14.000));

    bool splitActive = true;
    double referenceFrequencyMhz = rx.frequency();
    int commandsIssued = 0;
    QObject::connect(&rx, &AetherSDR::SliceModel::frequencyCommandIssued, &rx,
                     [&](double frequencyMhz) {
        ++commandsIssued;
        pendingTuneEchoes.record(rx.sliceId(), frequencyMhz, 1000);
    });
    QObject::connect(&rx, &AetherSDR::SliceModel::frequencyStatusReported, &rx,
                     [&](double frequencyMhz) {
        if (pendingTuneEchoes.consume(rx.sliceId(), frequencyMhz, 1001)) {
            referenceFrequencyMhz = frequencyMhz;
            return;
        }
        if (AetherSDR::shouldCloseSplitOnQsyObservation(
                settings, splitActive, true, frequencyMhz,
                referenceFrequencyMhz)) {
            splitActive = false;
        }
    });

    // Two rapid SWAPs issue both RX tunes before either radio status arrives.
    rx.setFrequency(14.005);
    rx.setFrequency(14.000);
    check(commandsIssued == 2 && !pendingTuneEchoes.empty(),
          "two outstanding SWAP echoes are tracked before status arrives");

    rx.applyChanges(frequencyObservation(14.005));
    check(splitActive, "first delayed SWAP echo does not close split");
    rx.applyChanges(frequencyObservation(14.000));
    check(splitActive, "second delayed SWAP echo does not close split");
    check(pendingTuneEchoes.empty(),
          "both outstanding SWAP echoes are consumed");
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    testDefaults();
    testRoundTrip();
    testClampAndMalformedValues();
    testQsyClosePolicy();
    testLocalTuneEchoAndIncrementalExternalQsy();
    testPendingTuneEchoesAreSliceSpecificAndExpire();
    testTwoOutstandingSwapEchoesDoNotCloseSplit();
    return g_failures == 0 ? 0 : 1;
}
