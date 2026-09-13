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

static TunerDelta decode(FlexBackend& b, const QMap<QString, QString>& kvs,
                         const QString& handle = QStringLiteral("0x2000"))
{
    QSignalSpy spy(&b, &IRadioBackend::tunerChanged);
    b.decodeTunerStatus(handle, kvs);
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
        CHECK(d.handle.has_value() && *d.handle == "0x2000");
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

    // ---- SmartSDR's placeholder never becomes neutral tuner identity ----
    {
        const TunerDelta d = decode(b, {{"model", "TunerGeniusXL"},
                                        {"operate", "1"}},
                                    QStringLiteral("0x00000000"));
        CHECK(!d.handle.has_value());
        CHECK(d.model.has_value() && *d.model == "TunerGeniusXL");
        CHECK(d.operate.has_value() && *d.operate == true);
    }

    // ---- per-port PTT reaches the delta; the rest stays dropped ----
    // ptta/pttb were dropped until TunerApplet grew the expanded front-panel
    // presentation, which shows a keying lamp per port. The informational and
    // routing keys around them are still dropped: nothing reads them.
    {
        const TunerDelta d = decode(b, {
            {"pttA", "1"}, {"pttB", "0"},
            {"nickname", "shack"}, {"version", "1.2.17"}, {"dhcp", "1"},
            {"gateway", "192.168.0.1"}, {"netmask", "255.255.255.0"},
            {"ant", "ANT1,ANT2"},
            {"bypass", "1"}});
        CHECK(d.pttA.has_value() && *d.pttA == true);
        CHECK(d.pttB.has_value() && *d.pttB == false);   // "0" → false, still present
        CHECK(d.bypass.has_value() && *d.bypass == true);
        // The dropped keys carry nothing; no recognized field was invented.
        CHECK(!d.operate.has_value() && !d.model.has_value() && !d.ip.has_value());
    }

    // ---- per-port antenna, split as FlexLib's ParseAntenna does ----
    // This is the only thing that identifies which port carries transmit: the
    // tuner's own direct status reports BOTH ports live when one radio is
    // cabled to both, so the answer has to come from matching the TX slice's
    // antenna against these.
    {
        const TunerDelta d = decode(b, {{"ant", "ANT1,ANT2"}});
        CHECK(d.portAAnt.has_value() && *d.portAAnt == "ANT1");
        CHECK(d.portBAnt.has_value() && *d.portBAnt == "ANT2");
    }
    // A single field leaves port B empty rather than unset — the tuner has
    // told us about B, and what it said is "nothing".
    {
        const TunerDelta d = decode(b, {{"ant", "ANT1"}});
        CHECK(d.portAAnt.has_value() && *d.portAAnt == "ANT1");
        CHECK(d.portBAnt.has_value() && d.portBAnt->isEmpty());
    }
    // Anything past the second is ignored, not treated as an error.
    {
        const TunerDelta d = decode(b, {{"ant", "ANT1,ANT2,XVTR"}});
        CHECK(d.portAAnt.has_value() && *d.portAAnt == "ANT1");
        CHECK(d.portBAnt.has_value() && *d.portBAnt == "ANT2");
    }

    // ---- PTT accepts either casing ----
    // FlexLib lower-cases every key before matching, so its "ptta"/"pttb"
    // cases do not pin the wire's spelling. The camel form is what the radio
    // is expected to send (it matches antA/relayC1); the lower form is
    // accepted so a firmware that disagrees still lights the lamp.
    {
        const TunerDelta d = decode(b, {{"ptta", "1"}, {"pttb", "1"}});
        CHECK(d.pttA.has_value() && *d.pttA == true);
        CHECK(d.pttB.has_value() && *d.pttB == true);
    }

    // ---- present-only still holds for the PTT keys ----
    {
        const TunerDelta d = decode(b, {{"operate", "1"}});
        CHECK(!d.pttA.has_value() && !d.pttB.has_value());
    }

    if (g_failures == 0) {
        std::printf("aetherd_tuner_decode_test: all checks passed\n");
        return 0;
    }
    std::printf("aetherd_tuner_decode_test: %d failure(s)\n", g_failures);
    return 1;
}
