// #5499 item 2 — `get_level STRENGTH` over rigctl, and the scalar behind it.
//
// RigctlProtocol::cmdGetLevel computed STRENGTH from MeterModel::sLevel(), a
// member written in exactly one place in the whole tree: MeterModel::clear(),
// to -130.0f. Nothing on the meter-packet path ever touched it. #155 split the
// single m_sLevelIdx into m_sLevelIdxBySlice — correctly, so the S-meter would
// stop showing the newest slice's signal for every slice — and deleted the
// scalar's store in the same hunk, leaving the getter and its two callers
// pointing at a member with no writer. The result was that every hamlib client
// on every backend and every radio family — WSJT-X, N1MM, gpredict — read
// STRENGTH as exactly -57.0 dB forever, which is -130 dBm expressed relative to
// the S9 reference. It looks like a plausible weak signal, and it never moves.
//
// The same function's own comment three lines above the branch says the
// slice-specific levels "use targetSlice so a VFOB query reads the TX slice
// rather than silently falling back to VFOA (#5)". STRENGTH sat inside that
// block, with `slice` resolved and in scope, and read a radio-wide scalar.
//
// WHY TWO SLICES. A test that fed one S-level and asserted one number would
// pass just as happily against a constant that happened to be the right
// magnitude — which is the exact trap this issue is about. So the positive
// control is two slices carrying DIFFERENT levels at the same moment: a reader
// that answers the packet must answer them differently, and a reader that
// answers a constant cannot, whatever that constant is.
//
// Differences between readings pin tracking and slice selection independently
// of the S9 offset. A separate known-value check pins the protocol's HF S9
// reference (-73 dBm = 0 dB relative to S9), so omitting or shifting that offset
// cannot pass merely because it cancels in every difference.
//
// Socket-free: an injected stub backend, slice fixtures, and meter values fed
// straight into MeterModel. Nothing is opened and nothing is keyed.

#include "TestSettingsProfile.h"
#include "core/RigctlProtocol.h"
#include "core/backends/IRadioBackend.h"
#include "models/MeterModel.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QCoreApplication>
#include <QString>

#include <cmath>
#include <cstdio>
#include <memory>

using namespace AetherSDR;

namespace {

int g_failed = 0;

void check(const char* name, bool ok)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", name);
    if (!ok) {
        ++g_failed;
    }
}

// Supplies a connection state and nothing else. RigctlProtocol::currentSlice()
// refuses to resolve a slice on a disconnected model, so the stub has to be
// able to say yes — it opens no transport to do it.
class StubBackend final : public IRadioBackend {
public:
    bool connected{false};
    RadioCapabilities capabilities() const override { return {}; }
    bool isConnected() const override { return connected; }
    void connectRadio(const RadioConnectRequest&) override {}
    void disconnectRadio() override { connected = false; }
    void setSliceFrequency(int, double) override {}
    void setSliceMode(int, const QString&) override {}
    void setSliceFilter(int, int, int) override {}
    void setSliceAudioGain(int, int) override {}
    void setSliceAudioMute(int, bool) override {}
    void setSliceAgc(int, const QString&, int) override {}
    void setPanCenter(const QString&, double, PanCenterIntent) override {}
    void setPanBandwidth(const QString&, double) override {}
    void setKeying(bool, const TxCoordinator::Operation&,
                   const TxCoordinator::Completion&) override {}
    void invokeExtension(const QString&, const QString&, quint64, const QVariant&) override {}
};

// One SLC/LEVEL meter per slice, declared the way a backend's manifest declares
// them: the meter's sourceIndex IS the slice index, which is what populates
// MeterModel::m_sLevelIdxBySlice.
MeterDef sliceLevelMeter(int meterIndex, int sliceIndex)
{
    MeterDef def;
    def.index = meterIndex;
    def.source = QStringLiteral("SLC");
    def.sourceIndex = sliceIndex;
    def.name = QStringLiteral("LEVEL");
    def.unit = QStringLiteral("dBm");
    def.low = -150.0;
    def.high = 20.0;
    return def;
}

struct Fixture {
    RadioModel radio;
    StubBackend* backend{nullptr};

    // Two slices, because one cannot tell a reading from a constant.
    Fixture()
    {
        auto owned = std::make_unique<StubBackend>();
        backend = owned.get();
        radio.setBackendForTest(std::move(owned), QStringLiteral("strength-test"));
        // Fixtures are refused while connected, so both slices are installed
        // first and the link is declared up afterwards — the same order
        // tx_operation_integration_test's fixture uses.
        if (!radio.automationApplySliceFixture(0, QStringLiteral("A"))
            || !radio.automationApplySliceFixture(1, QStringLiteral("B"))
            || !radio.slice(0) || !radio.slice(1)) {
            std::fprintf(stderr, "FATAL: could not install the two-slice fixture\n");
            std::exit(2);
        }
        backend->connected = true;
    }
};

// `\get_level STRENGTH` — the long form, backslash and all, as rigctld takes
// it — answers "<value>\n" in non-extended mode; an unavailable reading answers
// "RPRT <code>\n". Returns false for the latter.
bool strengthOf(RigctlProtocol& proto, double* out)
{
    const QString reply =
        proto.handleLine(QStringLiteral("\\get_level STRENGTH")).trimmed();
    if (reply.startsWith(QLatin1String("RPRT"))) {
        return false;
    }
    bool ok = false;
    const double value = reply.toDouble(&ok);
    if (ok && out) {
        *out = value;
    }
    return ok;
}

bool nearly(double a, double b)
{
    return std::fabs(a - b) < 0.01;
}

// The two S-levels. Deliberately far apart, and deliberately not round numbers
// that a constant might coincide with. -83.9 dBm is the median the bench
// measured on the SLC:LEVEL row while `get meters`.sLevel sat at -130.
constexpr float kSlice0Dbm = -83.9f;
constexpr float kSlice1Dbm = -51.4f;

void testTwoSlicesReportTheirOwnLevels()
{
    Fixture f;
    f.radio.meterModel().defineMeter(sliceLevelMeter(10, 0));
    f.radio.meterModel().defineMeter(sliceLevelMeter(11, 1));
    // updateValueByName rather than the raw int16 path: these are dBm and must
    // not round-trip through Flex's wire scale, which is what that overload
    // exists for.
    f.radio.meterModel().updateValueByName(QStringLiteral("SLC"), QStringLiteral("LEVEL"),
                                           kSlice0Dbm, 0);
    f.radio.meterModel().updateValueByName(QStringLiteral("SLC"), QStringLiteral("LEVEL"),
                                           kSlice1Dbm, 1);

    RigctlProtocol portA(&f.radio);
    portA.setSliceIndex(0);
    RigctlProtocol portB(&f.radio);
    portB.setSliceIndex(1);

    double a = 0.0;
    double b = 0.0;
    const bool gotA = strengthOf(portA, &a);
    const bool gotB = strengthOf(portB, &b);
    check("a fed S-meter answers STRENGTH at all", gotA && gotB);

    // THE CONTROL. Two live readings at one moment must not be the same number.
    // Against the old scalar both were -57.0 and this is the assertion that
    // could see it; every other assertion here could not.
    check("two slices carrying different levels do not report the same strength",
          gotA && gotB && !nearly(a, b));

    // …and the gap between them is the gap between the samples. STRENGTH is dB
    // relative to S9 and the samples are dBm: one additive offset apart, so the
    // difference is the offset-free way to assert the VALUE without naming the
    // reference. A reader that answered, say, the first slice for both ports
    // would fail the line above; one that answered a scaled or inverted value
    // fails this one.
    check("the strength gap equals the dBm gap, whatever the S9 reference is",
          gotA && gotB
              && nearly(b - a, static_cast<double>(kSlice1Dbm - kSlice0Dbm)));
}

void testStrengthTracksANewSample()
{
    Fixture f;
    f.radio.meterModel().defineMeter(sliceLevelMeter(10, 0));
    f.radio.meterModel().updateValueByName(QStringLiteral("SLC"), QStringLiteral("LEVEL"),
                                           kSlice0Dbm, 0);
    RigctlProtocol port(&f.radio);
    port.setSliceIndex(0);

    double before = 0.0;
    const bool gotBefore = strengthOf(port, &before);

    constexpr float kRisenDbm = -62.5f;
    f.radio.meterModel().updateValueByName(QStringLiteral("SLC"), QStringLiteral("LEVEL"),
                                           kRisenDbm, 0);
    double after = 0.0;
    const bool gotAfter = strengthOf(port, &after);

    // A signal rising by a known number of dB must move STRENGTH by that many
    // dB. The old scalar moved by zero.
    check("a rising signal moves STRENGTH by exactly the same number of dB",
          gotBefore && gotAfter
              && nearly(after - before, static_cast<double>(kRisenDbm - kSlice0Dbm)));
}

void testStrengthUsesTheHfS9Reference()
{
    Fixture f;
    f.radio.meterModel().defineMeter(sliceLevelMeter(10, 0));
    RigctlProtocol port(&f.radio);
    port.setSliceIndex(0);
    f.radio.meterModel().updateValueByName(QStringLiteral("SLC"), QStringLiteral("LEVEL"),
                                         -73.0f, 0);
    double strength = 0.0;
    check("HF S9 (-73 dBm) reports zero dB relative to S9",
          strengthOf(port, &strength) && nearly(strength, 0.0));
    f.radio.meterModel().updateValueByName(QStringLiteral("SLC"), QStringLiteral("LEVEL"),
                                         -67.0f, 0);
    check("six dB above HF S9 reports positive six dB",
          strengthOf(port, &strength) && nearly(strength, 6.0));
}

void testUndeclaredAndUnfedMetersAreNotReadings()
{
    Fixture f;
    RigctlProtocol port(&f.radio);
    port.setSliceIndex(0);

    double value = 0.0;
    check("a radio that declares no S-meter does not answer with a number",
          !strengthOf(port, &value));

    f.radio.meterModel().defineMeter(sliceLevelMeter(10, 0));
    check("a declared but never-fed S-meter does not answer with a number either",
          !strengthOf(port, &value));

    // CONTROL for both lines above: the refusal must be about the missing
    // sample and not about this harness being unable to reach the branch at
    // all. One sample, and the same port answers.
    f.radio.meterModel().updateValueByName(QStringLiteral("SLC"), QStringLiteral("LEVEL"),
                                           kSlice0Dbm, 0);
    check("and one sample is enough to make the same port answer",
          strengthOf(port, &value));

    // The meter definitions go with the session — MeterModel::clear() runs from
    // RadioModel::onDisconnected and drops m_sLevelIdxBySlice — so a reading
    // cannot outlive the radio that produced it.
    f.radio.meterModel().removeMeter(10);
    check("removing the meter takes the reading with it",
          !strengthOf(port, &value));
}

// The second surface for the same quantity. MeterModel::sLevel() was the
// getter; sLevelForSlice() is what replaces it, and the two consumers must not
// be able to disagree — that is the #4533 rule this issue quotes.
void testTheAccessorAgreesWithTheArray()
{
    Fixture f;
    f.radio.meterModel().defineMeter(sliceLevelMeter(10, 0));
    f.radio.meterModel().defineMeter(sliceLevelMeter(11, 1));
    f.radio.meterModel().updateValueByName(QStringLiteral("SLC"), QStringLiteral("LEVEL"),
                                           kSlice0Dbm, 0);
    f.radio.meterModel().updateValueByName(QStringLiteral("SLC"), QStringLiteral("LEVEL"),
                                           kSlice1Dbm, 1);

    const auto s0 = f.radio.meterModel().sLevelForSlice(0);
    const auto s1 = f.radio.meterModel().sLevelForSlice(1);
    check("sLevelForSlice returns each slice's own sample",
          s0 && s1 && nearly(*s0, kSlice0Dbm) && nearly(*s1, kSlice1Dbm));
    check("sLevelForSlice has nothing to say about a slice with no meter",
          !f.radio.meterModel().sLevelForSlice(2).has_value());

    // With two receivers declaring a LEVEL meter there is no single "the"
    // S-level, so the radio-wide accessor declines rather than picking one —
    // picking the last one to update is the bug #155 fixed.
    check("the radio-wide accessor declines when two slices could answer",
          !f.radio.meterModel().sLevelIfLive().has_value());

    f.radio.meterModel().removeMeter(11);
    const auto single = f.radio.meterModel().sLevelIfLive();
    check("with one receiver it answers, and answers that receiver's sample",
          single && nearly(*single, kSlice0Dbm));
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("rigctl-strength-slevel"));
    QCoreApplication app(argc, argv);
    if (!profile.isValid()) {
        return 1;
    }
    testTwoSlicesReportTheirOwnLevels();
    testStrengthTracksANewSample();
    testStrengthUsesTheHfS9Reference();
    testUndeclaredAndUnfedMetersAreNotReadings();
    testTheAccessorAgreesWithTheArray();
    std::printf("%s\n", g_failed == 0 ? "ALL PASS" : "FAILURES");
    return g_failed == 0 ? 0 : 1;
}
