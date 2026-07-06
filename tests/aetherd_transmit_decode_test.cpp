// aetherd RFC 2.3 — TransmitModel touchpoint: FlexBackend::decode*Status.
// Pins the Flex transmit-family wire → typed TransmitDelta translation (key
// renames, "1"→bool, ok-guarded + clamped numerics, compander/dexp aliasing,
// ATU raw token, APD bare-flag, sampler INTERNAL-prepend). The model apply side
// is covered by transmit_model_test / transmit_model_apd_test.

#include "core/backends/flex/FlexBackend.h"
#include "core/backends/TransmitDelta.h"

#include <QCoreApplication>
#include <QSignalSpy>
#include <QString>
#include <cstdio>

using namespace AetherSDR;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

template <class Fn>
static TransmitDelta decode(FlexBackend& b, Fn call)
{
    QSignalSpy spy(&b, &IRadioBackend::transmitChanged);
    call(b);
    if (spy.count() != 1) return {};
    return spy.takeFirst().at(0).value<TransmitDelta>();
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    qRegisterMetaType<TransmitDelta>();
    FlexBackend b;

    // ---- core transmit: clamp, "1"→bool, ok-guard ----
    {
        const TransmitDelta d = decode(b, [](FlexBackend& fb){
            fb.decodeTransmitStatus({
                {QStringLiteral("rfpower"), QStringLiteral("150")},   // clamps to 100
                {QStringLiteral("tune"), QStringLiteral("1")},
                {QStringLiteral("freq"), QStringLiteral("14.2")},
                {QStringLiteral("mic_selection"), QStringLiteral("acc")},  // uppercased
                {QStringLiteral("max_power_level"), QStringLiteral("500")}, // unclamped
                {QStringLiteral("iambic_mode"), QStringLiteral("9")},  // clamps to 1
            });
        });
        CHECK(d.rfPower.has_value() && *d.rfPower == 100);
        CHECK(d.tune.has_value() && *d.tune == true);
        CHECK(d.transmitFreq.has_value() && qFuzzyCompare(*d.transmitFreq, 14.2));
        CHECK(d.micSelection.has_value() && *d.micSelection == QStringLiteral("ACC"));
        CHECK(d.maxPowerLevel.has_value() && *d.maxPowerLevel == 500);
        CHECK(d.cwIambicMode.has_value() && *d.cwIambicMode == 1);
    }

    // ---- ok-guard: malformed present numeric is dropped ----
    {
        const TransmitDelta d = decode(b, [](FlexBackend& fb){
            fb.decodeTransmitStatus({
                {QStringLiteral("rfpower"), QStringLiteral("junk")},
                {QStringLiteral("mox"), QStringLiteral("1")},
            });
        });
        CHECK(!d.rfPower.has_value());   // dropped, not clamped-0
        CHECK(d.mox.has_value() && *d.mox == true);
    }

    // ---- compander/dexp aliasing ----
    {
        // compander present → carried; dexp ignored.
        const TransmitDelta d = decode(b, [](FlexBackend& fb){
            fb.decodeTransmitStatus({
                {QStringLiteral("compander"), QStringLiteral("1")},
                {QStringLiteral("dexp"), QStringLiteral("0")},
                {QStringLiteral("compander_level"), QStringLiteral("33")},
                {QStringLiteral("noise_gate_level"), QStringLiteral("9")},
            });
        });
        CHECK(d.compander.has_value() && *d.compander == true);   // compander wins
        CHECK(d.companderLevel.has_value() && *d.companderLevel == 33);
    }
    {
        // dexp-only (no compander) → aliases into compander.
        const TransmitDelta d = decode(b, [](FlexBackend& fb){
            fb.decodeTransmitStatus({
                {QStringLiteral("dexp"), QStringLiteral("1")},
                {QStringLiteral("noise_gate_level"), QStringLiteral("8")},
            });
        });
        CHECK(d.compander.has_value() && *d.compander == true);
        CHECK(d.companderLevel.has_value() && *d.companderLevel == 8);
    }

    // ---- interlock ----
    {
        const TransmitDelta d = decode(b, [](FlexBackend& fb){
            fb.decodeInterlockStatus({{QStringLiteral("tx1_delay"), QStringLiteral("25")},
                                      {QStringLiteral("timeout"), QStringLiteral("120")}});
        });
        CHECK(d.tx1Delay.has_value() && *d.tx1Delay == 25);
        CHECK(d.interlockTimeout.has_value() && *d.interlockTimeout == 120);
        CHECK(!d.rfPower.has_value());   // core fields untouched
    }

    // ---- ATU: raw status token carried; model owns the enum parse ----
    {
        const TransmitDelta d = decode(b, [](FlexBackend& fb){
            fb.decodeAtuStatus({{QStringLiteral("status"), QStringLiteral("TUNE_SUCCESSFUL")},
                                {QStringLiteral("atu_enabled"), QStringLiteral("1")}});
        });
        CHECK(d.atuStatusRaw.has_value() && *d.atuStatusRaw == QStringLiteral("TUNE_SUCCESSFUL"));
        CHECK(d.atuEnabled.has_value() && *d.atuEnabled == true);
    }

    // ---- APD: bare equalizer_reset flag engages the optional ----
    {
        const TransmitDelta d = decode(b, [](FlexBackend& fb){
            fb.decodeApdStatus({{QStringLiteral("enable"), QStringLiteral("1")},
                                {QStringLiteral("equalizer_reset"), QString()}});
        });
        CHECK(d.apdEnabled.has_value() && *d.apdEnabled == true);
        CHECK(d.apdEqualizerReset.has_value());   // engaged by presence, value irrelevant
    }

    // ---- APD sampler: uppercase, INTERNAL-prepend, empty tx_ant → no emit ----
    {
        const TransmitDelta d = decode(b, [](FlexBackend& fb){
            fb.decodeApdSamplerStatus({{QStringLiteral("tx_ant"), QStringLiteral("ant1")},
                                       {QStringLiteral("selected_sampler"), QStringLiteral("rx_a")},
                                       {QStringLiteral("valid_samplers"), QStringLiteral("rx_a, xvta")}});
        });
        CHECK(d.apdSamplerTxAnt.has_value() && *d.apdSamplerTxAnt == QStringLiteral("ANT1"));
        CHECK(d.apdSamplerAvailable.has_value()
              && *d.apdSamplerAvailable == QStringList({QStringLiteral("INTERNAL"),
                                                        QStringLiteral("RX_A"),
                                                        QStringLiteral("XVTA")}));
        CHECK(d.apdSamplerSelected.has_value() && *d.apdSamplerSelected == QStringLiteral("RX_A"));
    }
    {
        // No tx_ant → no emit at all.
        QSignalSpy spy(&b, &IRadioBackend::transmitChanged);
        b.decodeApdSamplerStatus({{QStringLiteral("selected_sampler"), QStringLiteral("RX_A")}});
        CHECK(spy.count() == 0);
    }

    if (g_failures == 0) {
        std::printf("aetherd_transmit_decode_test: all checks passed\n");
        return 0;
    }
    std::printf("aetherd_transmit_decode_test: %d failure(s)\n", g_failures);
    return 1;
}
