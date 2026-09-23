// Socket-free regression requested in #5924's maintainer review. Capture only
// real FlexBackend seam dispatches; no radio, simulator peer or transport opens.
#include "TestSettingsProfile.h"
#include "core/backends/flex/FlexBackend.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QCoreApplication>
#include <cstdio>

using namespace AetherSDR;

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("flex-slice-mode-intent"));
    QCoreApplication app(argc, argv);
    int failures = 0;
    const auto check = [&](bool accepted, const char* message) {
        std::printf("%s: %s\n", accepted ? "PASS" : "FAIL", message);
        if (!accepted) { ++failures; }
    };
    if (!profile.isValid()) { return 1; }
    RadioModel model;
    check(model.rebuildBackendForTest(QStringLiteral("flex")), "production family switch builds Flex");
    auto* flex = dynamic_cast<FlexBackend*>(model.backend());
    check(flex != nullptr, "actual FlexBackend owns dispatch");
    if (!flex) { return 1; }
    QStringList commands;
    flex->setSliceCommandSink([&](const QString& command) { commands << command; });
    const QMap<QString, QString> status{{"in_use", "1"}, {"RF_frequency", "14.200000"},
        {"mode", "USB"}, {"active", "1"}};
    model.handleSliceStatusForTest(0, status, false);
    SliceModel* slice = model.slice(0);
    check(slice != nullptr, "slice zero materializes through real status handling");
    if (!slice) { return 1; }
    commands.clear();
    slice->setMode(QStringLiteral("LSB"));
    check(commands == QStringList{QStringLiteral("slice set 0 mode=LSB")},
          "one mode intent reaches the Flex seam exactly once");
    check(slice->mode() == QLatin1String("LSB"), "Flex mode retains optimistic publication");
    model.handleSliceStatusForTest(0, status, false);
    commands.clear();
    slice->setMode(QStringLiteral("CW"));
    check(commands == QStringList{QStringLiteral("slice set 0 mode=CW")},
          "repeated status does not add another mode binding");
    commands.clear();
    slice->setFrequency(14.210);
    slice->setFilterWidth(100, 2800);
    check(commands.isEmpty(), "Flex frequency and filter do not duplicate their existing command route");
    check(qFuzzyCompare(slice->frequency(), 14.210), "Flex frequency retains optimistic publication");
    return failures ? 1 : 0;
}
