// aetherd 2.4 (#4092) — FlexBackend::decodeTunerStatus. Pins the SmartSDR TGXL
// "atu"/"amplifier" wire → typed TunerDelta translation that moved out of
// TunerModel::applyStatus: present-only, "1"→bool, toInt relays/antenna, verbatim
// text, and dropping the informational keys.

#include "core/backends/flex/FlexBackend.h"
#include "core/backends/TunerDelta.h"

#include <QCoreApplication>
#include <QSignalSpy>
#include <cstdio>

using namespace AetherSDR;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

static TunerDelta decode(FlexBackend& b, const QMap<QString, QString>& kvs)
{
    QSignalSpy spy(&b, &IRadioBackend::tunerChanged);
    b.decodeTunerStatus(kvs);
    if (spy.count() != 1) return {};
    return spy.takeFirst().at(0).value<TunerDelta>();
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    qRegisterMetaType<TunerDelta>();
    FlexBackend b;

    // ---- full field set: "1"→bool, toInt relays/antenna, verbatim text ----
    {
        const TunerDelta d = decode(b, {
            {"serial_num", "TG9"}, {"model", "TunerGeniusXL"},
            {"operate", "1"}, {"bypass", "0"}, {"tuning", "1"},
            {"relayC1", "20"}, {"relayC2", "5"}, {"relayL", "12"},
            {"antA", "2"}, {"one_by_three", "1"}, {"ip", "10.0.0.5"}});
        CHECK(d.serialNum.has_value() && *d.serialNum == "TG9");
        CHECK(d.model.has_value() && *d.model == "TunerGeniusXL");
        CHECK(d.operate.has_value() && *d.operate == true);
        CHECK(d.bypass.has_value() && *d.bypass == false);      // "0" → false
        CHECK(d.tuning.has_value() && *d.tuning == true);
        CHECK(*d.relayC1 == 20 && *d.relayC2 == 5 && *d.relayL == 12);
        CHECK(d.antennaA.has_value() && *d.antennaA == 2);
        CHECK(d.oneByThree.has_value() && *d.oneByThree == true);
        CHECK(d.ip.has_value() && *d.ip == "10.0.0.5");
    }

    // ---- present-only: absent keys stay disengaged ----
    {
        const TunerDelta d = decode(b, {{"operate", "1"}});
        CHECK(d.operate.has_value() && *d.operate == true);
        CHECK(!d.bypass.has_value() && !d.relayC1.has_value()
              && !d.model.has_value() && !d.antennaA.has_value());
    }

    // ---- informational keys (nickname/version/gateway/…) are dropped ----
    {
        const TunerDelta d = decode(b, {
            {"nickname", "shack"}, {"version", "1.2"}, {"gateway", "192.168.0.1"},
            {"bypass", "1"}});
        CHECK(d.bypass.has_value() && *d.bypass == true);
        // nothing carries the informational keys; only the recognized field decoded.
        CHECK(!d.operate.has_value() && !d.model.has_value() && !d.ip.has_value());
    }

    if (g_failures == 0) {
        std::printf("aetherd_tuner_decode_test: all checks passed\n");
        return 0;
    }
    std::printf("aetherd_tuner_decode_test: %d failure(s)\n", g_failures);
    return 1;
}
