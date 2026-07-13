#include "core/DigitalVoiceModeRegistry.h"
#include "models/SliceModel.h"

#include <QCoreApplication>
#include <QSignalSpy>

#include <iostream>

namespace {

bool expect(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
    }
    return condition;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    bool ok = true;

    using namespace AetherSDR;
    DigitalVoiceModeRegistry& registry = DigitalVoiceModeRegistry::instance();
    registry.deactivateMode(DigitalVoiceModeId::DStar);
    ok &= expect(registry.activateMode(DigitalVoiceModeId::DStar),
                 "D-STAR service activates");
    QSignalSpy activeSliceSpy(&registry,
                              &DigitalVoiceModeRegistry::activeSliceChanged);

    SliceModel first(1);
    first.setMode(QStringLiteral("LSB"));
    first.setMode(QStringLiteral("DSTR"));
    const std::optional<DigitalVoiceSliceClaim> initialClaim = registry.activeClaim();
    ok &= expect(initialClaim.has_value()
                 && initialClaim->sliceId == 1
                 && initialClaim->previousMode == QStringLiteral("LSB"),
                 "slice claim retains the operator's previous mode");
    ok &= expect(activeSliceSpy.size() == 1
                 && activeSliceSpy.takeFirst().at(0).toInt() == 1,
                 "slice claim announces the controlled slice");

    SliceModel second(2);
    int correctionCount = 0;
    QString correctionMode;
    QObject::connect(&second, &SliceModel::modeChangeRequested,
                     [&correctionCount, &correctionMode](const QString& mode) {
        ++correctionCount;
        correctionMode = mode;
    });
    SliceDelta conflictingStatus;
    conflictingStatus.mode = QStringLiteral("DSTR");
    second.applyChanges(conflictingStatus);
    ok &= expect(second.mode() == QStringLiteral("USB"),
                 "rejected radio claim does not enter DSTR locally");
    ok &= expect(correctionCount == 1 && correctionMode == QStringLiteral("USB"),
                 "rejected radio claim requests a radio-side mode correction");

    QObject::connect(&second, &SliceModel::digitalVoiceSliceDisplaced,
                     [&first](int sliceId, const QString& previousMode) {
        if (sliceId == first.sliceId()) {
            first.setMode(previousMode);
        }
    });
    second.setMode(QStringLiteral("FM"));
    second.setMode(QStringLiteral("DSTR"));
    const std::optional<DigitalVoiceSliceClaim> transferredClaim =
        registry.activeClaim();
    ok &= expect(first.mode() == QStringLiteral("LSB"),
                 "operator transfer restores the previous D-STAR slice");
    ok &= expect(second.mode() == QStringLiteral("DSTR")
                 && transferredClaim.has_value()
                 && transferredClaim->sliceId == 2
                 && transferredClaim->previousMode == QStringLiteral("FM"),
                 "operator transfer moves the D-STAR claim to the selected slice");

    const std::optional<DigitalVoiceSliceClaim> stoppedClaim =
        registry.deactivateMode(DigitalVoiceModeId::DStar);
    ok &= expect(stoppedClaim.has_value()
                 && stoppedClaim->sliceId == 2
                 && stoppedClaim->previousMode == QStringLiteral("FM"),
                 "service stop returns the controlled slice and restore mode");
    second.setMode(stoppedClaim->previousMode);
    ok &= expect(second.mode() == QStringLiteral("FM"),
                 "controlled slice restores without being removed");
    ok &= expect(!activeSliceSpy.isEmpty()
                 && activeSliceSpy.constLast().at(0).toInt() == -1,
                 "service stop announces that no slice is controlled");

    ok &= expect(registry.activateMode(DigitalVoiceModeId::DStar),
                 "service reactivates");
    first.setMode(QStringLiteral("DSTR"));
    registry.deactivateMode(DigitalVoiceModeId::DStar);
    ok &= expect(first.mode() == QStringLiteral("DSTR"),
                 "fixture retains restored DSTR while registry is inactive");

    ok &= expect(registry.activateMode(DigitalVoiceModeId::DStar),
                 "service activates for restored slice reconciliation");
    first.setMode(QStringLiteral("DSTR"));
    ok &= expect(registry.activeSliceId() == 1,
                 "unchanged restored DSTR reclaims its slice");

    registry.deactivateMode(DigitalVoiceModeId::DStar);
    first.setMode(QStringLiteral("LSB"));
    return ok ? 0 : 1;
}
