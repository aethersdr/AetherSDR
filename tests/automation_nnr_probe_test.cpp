// #5687 follow-up: the automation DSP stereo probe never learned about NNR.
// `probeDspStereo NNR` fell through to the unknown-mode error, and the `all`
// sweep -- which exists so a coverage run exercises every client NR engine --
// silently skipped the seventh one, reporting fullCoverage while testing six.
//
// Drives the bridge's private line dispatcher through its existing test friend:
// no socket, RadioModel, or TX surface, matching automation_rn2_probe_test.
#include "core/AudioEngine.h"
#include "core/AutomationServer.h"

#include "TestSettingsProfile.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QString>

#include <cstdio>

namespace AetherSDR {

class AutomationServerTestAccess {
public:
    static QJsonObject handleLine(AutomationServer& server, const QByteArray& line)
    {
        return server.handleLine(line, nullptr);
    }
};

} // namespace AetherSDR

namespace {

int failures = 0;

void check(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

QJsonObject bare(AetherSDR::AutomationServer& server, const QString& line)
{
    return AetherSDR::AutomationServerTestAccess::handleLine(server, line.toUtf8());
}

} // namespace

int main(int argc, char** argv)
{
    // Isolate the settings store: the probe seeds NNR from NnrSettings, so an
    // ambient strength on a developer box would otherwise steer the result.
    TestSettingsProfile profile(QStringLiteral("automation-nnr-probe-test"));
    qputenv("AETHER_AUTOMATION", "1");
    QCoreApplication app(argc, argv);
    AetherSDR::AudioEngine engine;
    AetherSDR::AutomationServer server;
    server.setAudioEngine(&engine);

    // 1. NNR is a mode the probe accepts, not an unknown one.
    const QJsonObject nnr = bare(server, QStringLiteral("audioCapture probeDspStereo NNR"));
    const QString error = nnr.value(QStringLiteral("error")).toString();
    check(!error.contains(QStringLiteral("unknown DSP mode")),
          "probeDspStereo NNR is a recognized mode (it used to be rejected)");
    check(nnr.value(QStringLiteral("mode")).toString() == QLatin1String("NNR"),
          "the NNR probe reports its own mode back");
    // Deliberately not asserting `ok`: that folds in a stereo-ratio quality
    // verdict, and NNR is a speech model that attenuates the probe's steady
    // tones hard. What the enumeration defect was about is whether the mode
    // RUNS, which is `skipped`.
    check(!nnr.value(QStringLiteral("skipped")).toBool(),
          "the NNR probe runs rather than skipping: WDSP ships both models");
    check(nnr.value(QStringLiteral("coverageStatus")).toString()
              == QLatin1String("complete"),
          "and reports complete coverage for the mode it was asked for");
    check(nnr.contains(QStringLiteral("input")) && nnr.contains(QStringLiteral("output")),
          "the NNR probe returns the stereo-balance fields its siblings return");

    // 2. The `all` sweep covers it, which is what makes a coverage run honest.
    const QJsonObject all = bare(server, QStringLiteral("audioCapture probeDspStereo all"));
    QSet<QString> sweptModes;
    const QJsonArray modes = all.value(QStringLiteral("modes")).toArray();
    for (const QJsonValue& entry : modes) {
        sweptModes.insert(entry.toObject().value(QStringLiteral("mode")).toString());
    }
    check(sweptModes.contains(QStringLiteral("NNR")),
          "probeDspStereo all sweeps NNR (it swept six of the seven engines)");
    for (const QJsonValue& entry : modes) {
        const QJsonObject o = entry.toObject();
        if (o.value(QStringLiteral("mode")).toString() == QLatin1String("NNR")) {
            check(!o.value(QStringLiteral("skipped")).toBool(),
                  "and actually runs NNR in the sweep rather than skipping it");
        }
    }
    for (const char* expected : {"NR2", "RN2", "NR4", "MNR", "DFNR", "BNR", "NNR"}) {
        check(sweptModes.contains(QLatin1String(expected)),
              "probeDspStereo all still sweeps every client NR engine");
    }

    // 3. The error text is the discovery surface for the verb, so it must list it.
    const QJsonObject unknown =
        bare(server, QStringLiteral("audioCapture probeDspStereo NOPE"));
    check(unknown.value(QStringLiteral("error")).toString().contains(QStringLiteral("NNR")),
          "the unknown-mode error advertises NNR alongside the other engines");

    if (failures == 0) {
        std::printf("automation_nnr_probe_test: all checks passed\n");
    }
    return failures == 0 ? 0 : 1;
}
