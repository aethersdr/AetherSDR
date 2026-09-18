// Automation-only RN2 probe contract. This deliberately calls the bridge's
// private line dispatcher through its existing test friend: the test proves
// both bare and JSON forwarding without a socket, RadioModel, or TX surface.
#include "core/AudioEngine.h"
#include "core/QsoRecorder.h"
#include "models/RadioModel.h"
#include "core/AutomationServer.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

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

QJsonObject json(AetherSDR::AutomationServer& server, const QJsonObject& request)
{
    return AetherSDR::AutomationServerTestAccess::handleLine(
        server, QJsonDocument(request).toJson(QJsonDocument::Compact));
}

bool hasLegacyStereoFields(const QJsonObject& result)
{
    return result.contains(QStringLiteral("frames")) &&
           result.contains(QStringLiteral("discardFrames")) &&
           result.contains(QStringLiteral("input")) && result.contains(QStringLiteral("output")) &&
           result.contains(QStringLiteral("ratioError")) &&
           result.contains(QStringLiteral("audible")) &&
           result.contains(QStringLiteral("preserved"));
}

void checkRn2Contract(const QJsonObject& result, const QString& rateDomain, int sampleRate,
                      const QString& outputMode, const char* description)
{
    check(result.value(QStringLiteral("ok")).toBool(),
          "RN2 response passes its complete proof contract");
    check(result.value(QStringLiteral("mode")).toString() == QLatin1String("RN2"), description);
    check(result.value(QStringLiteral("rateDomain")).toString() == rateDomain,
          "RN2 response has canonical rate domain");
    check(result.value(QStringLiteral("sampleRate")).toInt() == sampleRate,
          "RN2 response has selected sample rate");
    check(result.value(QStringLiteral("outputMode")).toString() == outputMode,
          "RN2 response has canonical output mode");
    check(result.value(QStringLiteral("probeDryMix")).toDouble() == 1.0,
          "RN2 probe reports its deterministic dry-mix isolation");
    check(result.value(QStringLiteral("inputFrames")).toInt() == sampleRate * 3,
          "RN2 input is the deterministic three-second run");
    check(result.value(QStringLiteral("outputFrames")).toInt() == sampleRate * 3,
          "RN2 output frame count matches input");
    check(result.value(QStringLiteral("inputCoverage"))
              .toObject()
              .value(QStringLiteral("complete"))
              .toBool(),
          "RN2 input coverage is complete");
    check(result.value(QStringLiteral("outputCoverage"))
              .toObject()
              .value(QStringLiteral("complete"))
              .toBool(),
          "RN2 output coverage is complete");
    check(result.value(QStringLiteral("outputSizeExact")).toBool(),
          "RN2 output size is exact for every partition");
    check(result.value(QStringLiteral("firstOutputSizeMismatchBlock")).toInt() == -1,
          "RN2 reports no per-block output-size mismatch");
    check(result.value(QStringLiteral("firstAudibleFrame")).toInt(-1) >= 0,
          "RN2 reports selected first audible frame");
    check(result.value(QStringLiteral("referenceFirstAudibleFrame")).toInt(-1) >= 0,
          "RN2 reports reference first audible frame");
    check(result.value(QStringLiteral("sequenceComparisonFrames")).toInt() >= sampleRate,
          "RN2 compares a substantial aligned sequence window");
    check(result.value(QStringLiteral("sequenceEquivalent")).toBool(),
          "RN2 selected partitions match aligned reference");
    check(result.value(QStringLiteral("sequenceFirstMismatchFrame")).toInt() == -1,
          "RN2 reports no FIFO sequence mismatch");
    check(result.value(QStringLiteral("startupLatencyMeasured")).toBool(),
          "RN2 measures selected and reference startup latency");
    check(result.value(QStringLiteral("fifoOrderPreserved")).toBool(),
          "RN2 preserves reference sample order after startup");
    check(result.value(QStringLiteral("fifoSequenceEquivalent")).toBool() ==
              (result.value(QStringLiteral("sequenceEquivalent")).toBool() &&
               result.value(QStringLiteral("startupLatencyEquivalent")).toBool()),
          "RN2 distinguishes FIFO timing equivalence from aligned sample order");
    check(result.value(QStringLiteral("sequenceOrder")).toString() ==
              QLatin1String("firstAudibleAligned"),
          "RN2 sequence order reports first-audible alignment");
    check(hasLegacyStereoFields(result), "RN2 retains legacy stereo metrics");
    check(result.value(QStringLiteral("audible")).toBool(), "RN2 output is audible after startup");
    if (outputMode == QLatin1String("PreserveRxStereo")) {
        check(result.value(QStringLiteral("ratioPreserved")).toBool(),
              "PreserveRxStereo retains the L/R level ratio");
    } else {
        check(result.value(QStringLiteral("duplicated")).toBool(),
              "ProcessedMono duplicates its result to L/R");
    }
}

} // namespace

int main(int argc, char** argv)
{
    qputenv("AETHER_AUTOMATION", "1");
    QCoreApplication app(argc, argv);
    AetherSDR::AudioEngine engine;
    AetherSDR::AutomationServer server;
    server.setAudioEngine(&engine);

    const QJsonObject legacyDefault =
        bare(server, QStringLiteral("audioCapture probeDspStereo RN2"));
    checkRn2Contract(legacyDefault, QStringLiteral("Legacy24k"), 24000,
                     QStringLiteral("PreserveRxStereo"), "bare no-option RN2 probe succeeds");
    check(legacyDefault.value(QStringLiteral("blockPartitions")).toArray() == QJsonArray{960},
          "no-option RN2 probe keeps the legacy 960-frame partition");

    const QJsonObject legacyIrregular = bare(
        server, QStringLiteral("audioCapture probeDspStereo RN2,strict blocks=73,211,17,604,91"));
    checkRn2Contract(legacyIrregular, QStringLiteral("Legacy24k"), 24000,
                     QStringLiteral("PreserveRxStereo"), "legacy irregular RN2 probe succeeds");
    check(legacyIrregular.value(QStringLiteral("strict")).toBool(),
          "legacy RN2,strict syntax remains accepted");

    const QJsonObject nativePreserve = json(
        server,
        QJsonObject{
            {QStringLiteral("cmd"), QStringLiteral("audioCapture")},
            {QStringLiteral("action"), QStringLiteral("probeDspStereo")},
            {QStringLiteral("value"), QStringLiteral("RN2 rate=Native48k output=PreserveRxStereo "
                                                     "blocks=73,211,17,604,91")},
        });
    checkRn2Contract(nativePreserve, QStringLiteral("Native48k"), 48000,
                     QStringLiteral("PreserveRxStereo"),
                     "JSON Native48k preserve-stereo probe succeeds");
    check(nativePreserve.value(QStringLiteral("ratioPreserved")).toBool(),
          "Native48k preserve-stereo probe retains the L/R ratio");

    const QJsonObject nativeMono =
        bare(server, QStringLiteral("audioCapture probeDspStereo RN2 rate=Native48k "
                                    "output=ProcessedMono blocks=73,211,17,604,91"));
    checkRn2Contract(nativeMono, QStringLiteral("Native48k"), 48000,
                     QStringLiteral("ProcessedMono"), "Native48k processed-mono probe succeeds");
    check(nativeMono.value(QStringLiteral("duplicated")).toBool(),
          "ProcessedMono output is duplicated to L/R");

    const QJsonObject nr2Alias = bare(server, QStringLiteral("audioCapture probeNr2Stereo"));
    check(nr2Alias.value(QStringLiteral("probe")).toString() == QLatin1String("nr2StereoBalance"),
          "probeNr2Stereo remains the no-option SpectralNR alias");

    const QJsonObject foreignOption =
        bare(server, QStringLiteral("audioCapture probeDspStereo all rate=Native48k"));
    check(!foreignOption.value(QStringLiteral("ok")).toBool(),
          "RN2-only options are rejected for all");
    const QJsonObject duplicateOption = bare(
        server, QStringLiteral("audioCapture probeDspStereo RN2 rate=Legacy24k rate=Native48k"));
    check(!duplicateOption.value(QStringLiteral("ok")).toBool(),
          "duplicate RN2 options are rejected");
    const QJsonObject invalidBlocks =
        bare(server, QStringLiteral("audioCapture probeDspStereo RN2 blocks=0"));
    check(!invalidBlocks.value(QStringLiteral("ok")).toBool(),
          "non-positive RN2 partitions are rejected");

    return failures == 0 ? 0 : 1;
}
