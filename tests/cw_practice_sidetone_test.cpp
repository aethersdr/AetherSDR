// Socket-free: the production RadioModel CW key path with no transmit-capable
// backend (the Demo). A key the radio does not take still sounds the local sidetone when
// the operator is practising (CW mode, break-in off, sidetone on), and never
// keys anything.
#include "TestSettingsProfile.h"
#include "core/backends/TransmitDelta.h"
#include "core/backends/sim/SimBackend.h"
#include "models/RadioModel.h"
#include "models/TransmitModel.h"

#include <QCoreApplication>
#include <QSignalSpy>
#include <cstdio>
#include <memory>

using namespace AetherSDR;

namespace {
int failures = 0;
void check(bool passed, const char* claim)
{
    std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", claim);
    failures += !passed;
}

void setTransmit(RadioModel& model, const QString& mode, bool breakIn, bool sidetone)
{
    TransmitDelta delta;
    delta.txSliceMode = mode;
    delta.cwBreakIn = breakIn;
    delta.cwSidetone = sidetone;
    model.transmitModel().applyChanges(delta);
}

QList<bool> keyEdges(RadioModel& model)
{
    QSignalSpy sidetone(&model, &RadioModel::cwKeyDownChanged);
    model.sendCwKey(true);
    model.sendCwKey(false);
    QList<bool> edges;
    for (const QList<QVariant>& args : sidetone) { edges << args.first().toBool(); }
    return edges;
}
} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("cw-practice-sidetone"));
    if (!profile.isValid()) { return 1; }
    QCoreApplication app(argc, argv);
    // The Demo declares no transmit and no CW keying, so it refuses every edge.
    RadioModel model;
    model.setBackendForTest(std::make_unique<SimBackend>(), QStringLiteral("sim"));
    check(!model.backendCapabilities().canTransmit, "the fixture backend cannot key");

    setTransmit(model, QStringLiteral("CW"), false, true);
    check(keyEdges(model) == QList<bool>({true, false}),
          "CW with break-in off sounds the practice sidetone for each key edge");
    check(!model.isRadioTransmitting(), "practice keying never transmits");

    setTransmit(model, QStringLiteral("CW"), true, true);
    check(keyEdges(model).isEmpty(), "with break-in on, a key the radio refuses stays silent");

    setTransmit(model, QStringLiteral("CW"), false, false);
    check(keyEdges(model).isEmpty(), "with sidetone off there is no practice tone");

    setTransmit(model, QStringLiteral("USB"), false, true);
    check(keyEdges(model).isEmpty(), "outside CW mode there is no practice tone");

    // A key-up always ends a practice tone, even if the settings changed while
    // the key was down.
    setTransmit(model, QStringLiteral("CW"), false, true);
    QSignalSpy sidetone(&model, &RadioModel::cwKeyDownChanged);
    model.sendCwKey(true);
    setTransmit(model, QStringLiteral("CW"), true, true);
    model.sendCwKey(false);
    check(sidetone.size() == 2 && !sidetone.last().first().toBool(),
          "a key-up releases a practice tone started before break-in was enabled");

    std::printf("cw_practice_sidetone_test: %d failures\n", failures);
    return failures ? 1 : 0;
}
