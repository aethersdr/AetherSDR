// `slice dsp nb2` refuses on a radio that does not run a host-side blanker.
//
// The read counterpart already does. `get hostnb` refuses outright without
// RadioCapabilities::hasHostNoiseBlanker, arguing that an empty success is a
// test passing against a radio where the question is meaningless. The write
// path six thousand lines further down had no equivalent check, so a bridge
// caller could put a Flex or an Icom slice into NoiseBlankerKind::Advanced —
// a kind no backend on those families can map onto anything.
//
// Three things make that worse than a no-op, and each is a reason this test
// exists rather than a comment:
//
//   1. It STICKS. SliceModel::applyChanges is deliberately written so a radio's
//      bool `nb` echo cannot pull a host kind back down, so nothing the radio
//      says afterwards clears it. Only a full NB off does.
//   2. The button LIES. VfoWidget::refreshNbControls keys the label on the kind
//      alone — only the fill row is capability-gated — so the NB button reads
//      "NB2" on a radio with no NB2.
//   3. IcomCivBackend::setSliceNoiseBlanker folds anything non-Off to plain
//      "on", which is faithful to the wire and means nothing upstream ever
//      notices the disagreement.
//
// The guard has to be NARROW in both directions, which is what the two halves
// below pin: `nb` still works on the same radio that cannot have `nb2` (its own
// blanker is not this host's), and `nb2` still works where the capability is
// published. A guard that refused both, or that refused everywhere, would pass
// a test that only checked the refusal.
//
// Socket-free: a stub backend, an injected slice, and the bridge dispatcher
// called directly. No transport, no DSP, no GUI.
#include "TestSettingsProfile.h"
#include "core/AutomationServer.h"
#include "core/backends/IRadioBackend.h"
#include "core/backends/NoiseBlankerKind.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>

#include <cstdio>
#include <memory>
#include <utility>

namespace AetherSDR {
class AutomationServerTestAccess
{
public:
    // handleLine() is private; the header already befriends this name for the
    // sibling automation tests. The JSON shape is the one every bridge caller
    // reaches, not a shortcut into doSlice().
    static QJsonObject slice(AutomationServer& server, const QString& action,
                             const QString& arg = {})
    {
        const QJsonObject request{{QStringLiteral("cmd"), QStringLiteral("slice")},
                                  {QStringLiteral("action"), action},
                                  {QStringLiteral("value"), arg}};
        return server.handleLine(QJsonDocument(request).toJson(QJsonDocument::Compact),
                                 nullptr);
    }
};
} // namespace AetherSDR

using namespace AetherSDR;

namespace {

int failures = 0;

void check(bool condition, const char* description)
{
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", description);
    if (!condition) {
        ++failures;
    }
}

// The minimum IRadioBackend that can carry one capability and publish one
// slice. Every seam verb is inert: the subject is what the bridge refuses
// BEFORE the seam, so a backend that recorded calls would only be able to
// confirm the absence this test already reads off the model.
class StubBackend : public IRadioBackend
{
public:
    RadioCapabilities caps;

    RadioCapabilities capabilities() const override { return caps; }
    void connectRadio(const RadioConnectRequest&) override {}
    void disconnectRadio() override {}
    bool isConnected() const override { return true; }
    void setSliceFrequency(int, double) override {}
    void setSliceMode(int, const QString&) override {}
    void setSliceFilter(int, int, int) override {}
    void setSliceAgc(int, const QString&, int) override {}
    void setPanCenter(const QString&, double, PanCenterIntent) override {}
    void setKeying(bool, const AetherSDR::TxCoordinator::Operation&,
                   const AetherSDR::TxCoordinator::Completion&) override {}
    void invokeExtension(const QString&, const QString&, quint64,
                         const QVariant&) override {}
};

SliceDelta oneSlice(const QString& panId)
{
    SliceDelta delta;
    delta.panId = panId;
    delta.frequency = 14.074;
    delta.mode = QStringLiteral("USB");
    delta.filterLow = 150;
    delta.filterHigh = 2700;
    delta.inUse = true;
    return delta;
}

// A radio with one pan and one slice, built the way the production bindings
// build one: geometry first, then the slice delta.
StubBackend* radioWithOneSlice(RadioModel& radio, const QString& family,
                               bool hostNoiseBlanker)
{
    auto owned = std::make_unique<StubBackend>();
    StubBackend* backend = owned.get();
    backend->caps.hasHostNoiseBlanker = hostNoiseBlanker;
    radio.setBackendForTest(std::move(owned), family);
    const QString opaquePan = QStringLiteral("stub:pan/0");
    emit backend->panCenterBandwidthChanged(opaquePan, 14.1, 0.2);
    emit backend->sliceChanged(0, oneSlice(opaquePan));
    return backend;
}

bool refused(const QJsonObject& reply)
{
    return !reply.value(QStringLiteral("ok")).toBool();
}

bool saysWhy(const QJsonObject& reply)
{
    // The message matters as much as the refusal: `get hostnb` refuses with a
    // reason and points at the control that DOES exist, and a bridge caller
    // getting "ok: false" with no reason cannot tell a capability gap from a
    // typo.
    const QString error = reply.value(QStringLiteral("error")).toString();
    return error.contains(QLatin1String("host-side noise blanker"))
        && error.contains(QLatin1String("get hostnb"));
}

// ---------------------------------------------------------------------------
// A radio WITHOUT the capability: nb2 refused, nb untouched.
// ---------------------------------------------------------------------------
void testRefusedWithoutCapability()
{
    RadioModel radio;
    radioWithOneSlice(radio, QStringLiteral("icom"), false);
    AutomationServer server;
    server.setRadioModel(&radio);
    check(radio.slices().size() == 1, "fixture publishes exactly one slice");
    SliceModel* slice = radio.slice(0);
    check(slice != nullptr && slice->nbKind() == NoiseBlankerKind::Off,
          "precondition: the slice starts with no blanker");
    if (!slice) {
        return;
    }

    const QJsonObject reply = AutomationServerTestAccess::slice(
        server, QStringLiteral("dsp"), QStringLiteral("nb2 on"));
    check(refused(reply), "nb2 refuses on a radio with no host-side blanker");
    check(saysWhy(reply), "the refusal names the capability and the control that exists");
    check(slice->nbKind() == NoiseBlankerKind::Off,
          "the refused command leaves the kind Off rather than reaching Advanced");

    // The level and fill are parsed AFTER the guard, so a refused command must
    // not have applied them either — a guard placed one block too low would
    // pass the check above and still move the operator's fill.
    const int levelBefore = slice->nbLevel();
    const NoiseBlankerFill fillBefore = slice->nbFill();
    const QJsonObject withArgs = AutomationServerTestAccess::slice(
        server, QStringLiteral("dsp"), QStringLiteral("nb2 on 77 4"));
    check(refused(withArgs), "nb2 with a level and a fill refuses the same way");
    check(slice->nbLevel() == levelBefore && slice->nbFill() == fillBefore,
          "a refused nb2 applies neither the level nor the fill");

    // NARROW: the radio's own blanker is not this host's stage, and the guard
    // must not reach it. Without this row a guard on the whole nb branch, or on
    // every family, would look correct.
    const QJsonObject plain = AutomationServerTestAccess::slice(
        server, QStringLiteral("dsp"), QStringLiteral("nb on 40"));
    check(plain.value(QStringLiteral("ok")).toBool(),
          "plain nb still works on the same radio");
    check(slice->nbKind() == NoiseBlankerKind::Impulse && slice->nbLevel() == 40,
          "plain nb selects the first blanker and takes its level");
}

// ---------------------------------------------------------------------------
// A radio WITH the capability: nb2 still reaches Advanced.
// ---------------------------------------------------------------------------
void testAllowedWithCapability()
{
    RadioModel radio;
    radioWithOneSlice(radio, QStringLiteral("anan"), true);
    AutomationServer server;
    server.setRadioModel(&radio);
    SliceModel* slice = radio.slice(0);
    check(slice != nullptr, "fixture publishes a slice on the capable radio");
    if (!slice) {
        return;
    }

    const QJsonObject reply = AutomationServerTestAccess::slice(
        server, QStringLiteral("dsp"), QStringLiteral("nb2 on 70 4"));
    check(reply.value(QStringLiteral("ok")).toBool(),
          "nb2 is accepted where the capability is published");
    check(slice->nbKind() == NoiseBlankerKind::Advanced,
          "the accepted command reaches Advanced");
    check(slice->nbLevel() == 70 && slice->nbFill() == NoiseBlankerFill::Interpolate,
          "the accepted command carries the level and the fill");

    const QJsonObject off = AutomationServerTestAccess::slice(
        server, QStringLiteral("dsp"), QStringLiteral("nb2 off"));
    check(off.value(QStringLiteral("ok")).toBool()
              && slice->nbKind() == NoiseBlankerKind::Off,
          "nb2 off turns the blanker off rather than being refused");
}

// A radio with no slices at all still answers about the RADIO. The guard sits
// ahead of slice resolution on purpose, so the reason given is the capability
// and not "no slice available", which would send a bridge caller looking for a
// slice on a radio that can never have the control.
void testRefusalPrecedesSliceResolution()
{
    RadioModel radio;
    auto owned = std::make_unique<StubBackend>();
    owned->caps.hasHostNoiseBlanker = false;
    radio.setBackendForTest(std::move(owned), QStringLiteral("icom"));
    AutomationServer server;
    server.setRadioModel(&radio);
    check(radio.slices().isEmpty(), "precondition: no slice exists");
    const QJsonObject reply = AutomationServerTestAccess::slice(
        server, QStringLiteral("dsp"), QStringLiteral("nb2 on"));
    check(refused(reply) && saysWhy(reply),
          "with no slice, nb2 still refuses for the capability rather than the slice");
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("aether-automation-nb2-capability-test"));
    qputenv("AETHER_AUTOMATION", "1");
    QCoreApplication app(argc, argv);
    check(profile.isValid(), "isolated settings profile is available");
    testRefusedWithoutCapability();
    testAllowedWithCapability();
    testRefusalPrecedesSliceResolution();
    if (failures == 0) {
        std::printf("automation_nb2_capability_test: all checks passed\n");
    }
    return failures == 0 ? 0 : 1;
}
