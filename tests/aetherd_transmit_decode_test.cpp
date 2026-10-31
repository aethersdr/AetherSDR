// aetherd RFC 2.3 — TransmitModel touchpoint: FlexBackend::decode*Status.
// Pins the Flex transmit-family wire → typed TransmitDelta translation (key
// renames, "1"→bool, ok-guarded + clamped numerics, compander/dexp aliasing,
// ATU raw token, APD bare-flag, sampler INTERNAL-prepend). The model apply side
// is covered by transmit_model_test / transmit_model_apd_test.

#include "core/backends/flex/FlexBackend.h"
#include "core/backends/TransmitDelta.h"

#include <QCoreApplication>
#include <QSignalSpy>
#include <QList>
#include <QPair>
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

    // ---- ptt_mic_override (v4.3 firmware, #6442) ----
    {
        const TransmitDelta d = decode(b, [](FlexBackend& fb){
            fb.decodeTransmitStatus({{QStringLiteral("ptt_mic_override"), QStringLiteral("1")}});
        });
        CHECK(d.pttMicOverride.has_value() && *d.pttMicOverride == true);
        const TransmitDelta absent = decode(b, [](FlexBackend& fb){
            fb.decodeTransmitStatus({{QStringLiteral("rfpower"), QStringLiteral("50")}});
        });
        CHECK(!absent.pttMicOverride.has_value());   // pre-4.3 firmware: never reported
        const TransmitDelta off = decode(b, [](FlexBackend& fb){
            fb.decodeTransmitStatus({{QStringLiteral("ptt_mic_override"), QStringLiteral("0")}});
        });
        CHECK(off.pttMicOverride.has_value() && *off.pttMicOverride == false);
        // FlexLib rejects anything but 0/1: malformed input is dropped, never "off".
        for (const char* bad : {"garbage", "-1", "2", ""}) {
            const TransmitDelta invalid = decode(b, [bad](FlexBackend& fb){
                fb.decodeTransmitStatus({{QStringLiteral("ptt_mic_override"), QLatin1String(bad)}});
            });
            CHECK(!invalid.pttMicOverride.has_value());
        }
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

    // ---- APD lifecycle + scope, as a FLEX-8600 on 4.3.16.42034 sends them ----
    {
        // apd ant=ANT1 freq=14.268000 rfpower=100 state=IDLE deployed=0
        const TransmitDelta d = decode(b, [](FlexBackend& fb){
            fb.decodeApdStatus({{QStringLiteral("ant"), QStringLiteral("ANT1")},
                                {QStringLiteral("freq"), QStringLiteral("14.268000")},
                                {QStringLiteral("rfpower"), QStringLiteral("100")},
                                {QStringLiteral("state"), QStringLiteral("IDLE")},
                                {QStringLiteral("deployed"), QStringLiteral("0")}});
        });
        CHECK(d.apdLifecycle == ApdLifecycleState::Idle);
        CHECK(d.apdEqAntenna == QStringLiteral("ANT1"));
        CHECK(d.apdEqFrequencyMhz && qFuzzyCompare(*d.apdEqFrequencyMhz, 14.268));
        CHECK(d.apdEqRfPower == 100);
        CHECK(!d.apdEqActive);
    }
    {
        // apd slice=0 mmx=0 client_handle=0x1D6D44F8 ant=ANT1 freq=14.268000
        //     rfpower=100 rx_error_mHz=2.105713 equalizer_active=0 configurable=1
        const TransmitDelta d = decode(b, [](FlexBackend& fb){
            fb.decodeApdStatus({{QStringLiteral("slice"), QStringLiteral("0")},
                                {QStringLiteral("mmx"), QStringLiteral("0")},
                                {QStringLiteral("client_handle"), QStringLiteral("0x1D6D44F8")},
                                {QStringLiteral("ant"), QStringLiteral("ANT1")},
                                {QStringLiteral("freq"), QStringLiteral("14.268000")},
                                {QStringLiteral("rfpower"), QStringLiteral("100")},
                                {QStringLiteral("rx_error_mHz"), QStringLiteral("2.105713")},
                                {QStringLiteral("equalizer_active"), QStringLiteral("0")},
                                {QStringLiteral("configurable"), QStringLiteral("1")}});
        });
        CHECK(d.apdEqActive == false);
        CHECK(d.apdConfigurable == true);
        CHECK(d.apdEqAntenna == QStringLiteral("ANT1"));
        CHECK(!d.apdLifecycle);
    }
    {
        // Every lifecycle token, case-insensitively; anything else stays unset.
        const QList<QPair<QString, ApdLifecycleState>> tokens{
            {QStringLiteral("off"), ApdLifecycleState::Off},
            {QStringLiteral("IDLE"), ApdLifecycleState::Idle},
            {QStringLiteral("TRAINING"), ApdLifecycleState::Training},
            {QStringLiteral("ACTIVE"), ApdLifecycleState::Active},
            {QStringLiteral("HOLD"), ApdLifecycleState::Hold},
            {QStringLiteral("FAULT"), ApdLifecycleState::Fault},
        };
        for (const auto& [token, state] : tokens) {
            const TransmitDelta d = decode(b, [&token](FlexBackend& fb){
                fb.decodeApdStatus({{QStringLiteral("state"), token}});
            });
            CHECK(d.apdLifecycle == state);
        }
        for (const QString& bad : {QStringLiteral("3"), QStringLiteral("IDLE,TRAINING"), QString()}) {
            const TransmitDelta d = decode(b, [&bad](FlexBackend& fb){
                fb.decodeApdStatus({{QStringLiteral("state"), bad}});
            });
            CHECK(!d.apdLifecycle);
        }
    }
    {
        // Pre-scope global form and a malformed frequency carry no scope.
        const TransmitDelta d = decode(b, [](FlexBackend& fb){
            fb.decodeApdStatus({{QStringLiteral("equalizer_active"), QStringLiteral("1")},
                                {QStringLiteral("freq"), QStringLiteral("abc")},
                                {QStringLiteral("ant"), QString()}});
        });
        CHECK(d.apdEqActive == true);
        CHECK(!d.apdEqAntenna && !d.apdEqFrequencyMhz && !d.apdEqRfPower);
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
