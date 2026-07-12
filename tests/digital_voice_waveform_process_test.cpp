// Standalone test harness for DigitalVoiceWaveformProcess lifecycle helpers.
//
// Build: produced by CMake as `digital_voice_waveform_process_test`.
// Run:   ./build/digital_voice_waveform_process_test
// Exit:  0 = pass, 1 = fail.

#include "core/DigitalVoiceWaveformProcess.h"
#include "core/DigitalVoiceWaveformSettings.h"
#include "models/DigitalVoiceWaveformHistory.h"

#include <QCoreApplication>
#include <QHostAddress>

#include <cstdio>

using AetherSDR::DigitalVoiceWaveformProcess;
using AetherSDR::DigitalVoiceWaveformSettings;
using AetherSDR::DigitalVoiceWaveformHealth;
using AetherSDR::DigitalVoiceWaveformHealthTracker;
using AetherSDR::DigitalVoiceWaveformMetricDirection;
using AetherSDR::DigitalVoiceWaveformMetrics;
using AetherSDR::DigitalVoiceWaveformTelemetryParser;
using AetherSDR::DigitalVoiceWaveformHistoryTracker;
using AetherSDR::DigitalVoiceModeId;
using AetherSDR::DigitalVoiceModeRegistry;

namespace {

int g_failed = 0;

void report(const char* name, bool ok, const QString& detail = {})
{
    std::printf("%s %-60s %s\n", ok ? "[ OK ]" : "[FAIL]", name,
                detail.toUtf8().constData());
    if (!ok) {
        ++g_failed;
    }
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    report("stateName: stopped",
           DigitalVoiceWaveformProcess::stateName(DigitalVoiceWaveformProcess::State::Stopped)
               == QStringLiteral("Stopped"));
    report("stateName: failed",
           DigitalVoiceWaveformProcess::stateName(DigitalVoiceWaveformProcess::State::Failed)
               == QStringLiteral("Failed"));

    report("resolveExecutablePath: trims configured path",
           DigitalVoiceWaveformProcess::resolveExecutablePath(QStringLiteral("  /tmp/aether-dv-waveform  "))
               == QStringLiteral("/tmp/aether-dv-waveform"));
    report("resolveExecutablePath: empty falls back to default",
           !DigitalVoiceWaveformProcess::resolveExecutablePath(QString()).isEmpty());

    report("backendFromString: empty uses ThumbDV backend",
           DigitalVoiceWaveformSettings::backendFromString(QString())
               == DigitalVoiceWaveformSettings::Backend::ThumbDv);
    report("backendFromString: ThumbDV maps to serial backend",
           DigitalVoiceWaveformSettings::backendFromString(QStringLiteral("ThumbDV"))
               == DigitalVoiceWaveformSettings::Backend::ThumbDv);
    report("backendRequiresSerial: ThumbDV requires serial",
           DigitalVoiceWaveformSettings::backendRequiresSerial(
               DigitalVoiceWaveformSettings::Backend::ThumbDv));
    report("backendArgument: ThumbDV CLI argument",
           DigitalVoiceWaveformSettings::backendArgument(
               DigitalVoiceWaveformSettings::Backend::ThumbDv)
               == QStringLiteral("thumbdv"));

    QJsonObject legacySettings {
        {QStringLiteral("Backend"), QStringLiteral("ThumbDV")},
        {QStringLiteral("AutoStart"), true},
        {QStringLiteral("SerialPort"), QStringLiteral("test-device")},
        {QStringLiteral("ExecutablePath"), QStringLiteral("/tmp/aether-dstar-waveform")},
        {QStringLiteral("MyCall"), QStringLiteral("KK7GWY")},
        {QStringLiteral("UrCall"), QStringLiteral("CQCQCQ")}
    };
    const QJsonObject migratedSettings =
        DigitalVoiceWaveformSettings::migrateLegacyObject(legacySettings);
    report("settings migration: shared values remain at service scope",
           migratedSettings.value(QStringLiteral("SerialPort")).toString()
               == QStringLiteral("test-device"));
    report("settings migration: D-STAR routing moves under mode scope",
           migratedSettings.value(QStringLiteral("DStar")).toObject()
               .value(QStringLiteral("MyCall")).toString() == QStringLiteral("KK7GWY"));
    report("settings migration: obsolete bundled executable is discarded",
           !migratedSettings.contains(QStringLiteral("ExecutablePath")));

    DigitalVoiceModeRegistry& modeRegistry = DigitalVoiceModeRegistry::instance();
    report("mode registry: exposes only the complete D-STAR engine",
           DigitalVoiceModeRegistry::supportedModes().size() == 1
               && DigitalVoiceModeRegistry::supportedModes().first().radioMode
                   == QStringLiteral("DSTR"));
    report("mode registry: activates D-STAR ThumbDV ownership",
           modeRegistry.activateMode(DigitalVoiceModeId::DStar));
    report("mode registry: first slice obtains exclusive ownership",
           modeRegistry.claimSlice(DigitalVoiceModeId::DStar,
                                   1,
                                   QStringLiteral("USB")));
    const std::optional<AetherSDR::DigitalVoiceSliceClaim> firstClaim =
        modeRegistry.activeClaim();
    report("mode registry: claim retains previous slice mode",
           firstClaim.has_value()
               && firstClaim->previousMode == QStringLiteral("USB"));
    report("mode registry: second slice is rejected",
           !modeRegistry.claimSlice(DigitalVoiceModeId::DStar, 2));
    modeRegistry.releaseSlice(1);
    report("mode registry: released ownership can move to another slice",
           modeRegistry.claimSlice(DigitalVoiceModeId::DStar, 2));
    const std::optional<AetherSDR::DigitalVoiceSliceClaim> finalClaim =
        modeRegistry.deactivateMode(DigitalVoiceModeId::DStar);
    report("mode registry: deactivation returns the active claim",
           finalClaim.has_value()
               && finalClaim->sliceId == 2
               && finalClaim->previousMode == QStringLiteral("DFM"));

    report("MYCALL validation: accepts amateur callsign",
           DigitalVoiceWaveformSettings::isValidMyCall(QStringLiteral("KK7GWY")));
    report("MYCALL validation: rejects placeholder",
           !DigitalVoiceWaveformSettings::isValidMyCall(QStringLiteral("CALLSIGN")));
    report("MYCALL validation: rejects punctuation",
           !DigitalVoiceWaveformSettings::isValidMyCall(QStringLiteral("KK7/GWY")));
    report("suffix validation: accepts four alphanumerics",
           DigitalVoiceWaveformSettings::isValidSuffix(QStringLiteral("AB12")));
    report("suffix validation: rejects overlength value",
           !DigitalVoiceWaveformSettings::isValidSuffix(QStringLiteral("ABCDE")));
    report("routing validation: accepts repeater module",
           DigitalVoiceWaveformSettings::isValidRoutingField(QStringLiteral("W7ABC B")));
    report("routing validation: rejects punctuation",
           !DigitalVoiceWaveformSettings::isValidRoutingField(QStringLiteral("W7ABC/B")));
    report("routing validation: rejects overlength value",
           !DigitalVoiceWaveformSettings::isValidRoutingField(QStringLiteral("W7ABCDEF B")));
    report("URCALL validation: accepts leading-slash gateway route",
           DigitalVoiceWaveformSettings::isValidUrCall(QStringLiteral("/W7ABC")));
    report("URCALL validation: rejects embedded slash",
           !DigitalVoiceWaveformSettings::isValidUrCall(QStringLiteral("W7/ABC")));
    report("message validation: accepts printable ASCII",
           DigitalVoiceWaveformSettings::isValidMessage(QStringLiteral("AetherSDR D-STAR")));
    report("message validation: rejects overlength text",
           !DigitalVoiceWaveformSettings::isValidMessage(QString(21, QLatin1Char('A'))));
    report("message validation: rejects SmartSDR delimiter",
           !DigitalVoiceWaveformSettings::isValidMessage(QStringLiteral("HELLO|WORLD")));

    const QByteArray metricLine =
        "AETHER_DV_METRIC v=1 mode=DSTR rx_hz=23998.7 vita_gaps=2 "
        "source_blocks=3 turn_mean_us=14.2 turn_max_us=29 queue_max=1";
    DigitalVoiceWaveformMetrics parsedMetrics;
    report("telemetry parser: accepts complete versioned metric",
           DigitalVoiceWaveformTelemetryParser::parseMetricLine(metricLine, &parsedMetrics));
    report("telemetry parser: parses sample rate",
           qAbs(parsedMetrics.rxSampleRateHz - 23998.7) < 0.01);
    report("telemetry parser: keeps transport and source errors distinct",
           parsedMetrics.vitaSequenceGaps == 2U
               && parsedMetrics.sourceBlockDeficits == 3U);
    report("telemetry parser: accepts directional RX metric",
           DigitalVoiceWaveformTelemetryParser::parseMetricLine(
               "AETHER_DV_METRIC v=2 mode=DSTR dir=RX rate_hz=24000 "
               "vita_gaps=0 source_blocks=0 turn_mean_us=10 "
               "turn_max_us=20 queue_max=0",
               &parsedMetrics)
               && parsedMetrics.direction
                   == DigitalVoiceWaveformMetricDirection::Rx
               && parsedMetrics.rxSampleRateHz == 24000.0);
    const QByteArray txMetricLine =
        "AETHER_DV_METRIC v=2 mode=DSTR dir=TX rate_hz=24001.2 vita_gaps=1 "
        "null_frames=2 pcm_clips=3 pcm_invalid=4 send_failures=5 "
        "queue_max=700 tail_samples=480 tail_us=20000";
    report("telemetry parser: accepts directional TX metric",
           DigitalVoiceWaveformTelemetryParser::parseMetricLine(
               txMetricLine, &parsedMetrics));
    report("telemetry parser: keeps TX fault counters distinct",
           parsedMetrics.direction == DigitalVoiceWaveformMetricDirection::Tx
               && parsedMetrics.txValid
               && qAbs(parsedMetrics.txSampleRateHz - 24001.2) < 0.01
               && parsedMetrics.txVitaSequenceGaps == 1U
               && parsedMetrics.txNullFrames == 2U
               && parsedMetrics.txPcmClips == 3U
               && parsedMetrics.txPcmInvalid == 4U
               && parsedMetrics.txSendFailures == 5U
               && parsedMetrics.txQueueMax == 700U
               && parsedMetrics.txTailSamples == 480U
               && parsedMetrics.txTailUs == 20000U);
    const QByteArray txPipelineMetricLine =
        "AETHER_DV_METRIC v=3 mode=DSTR dir=TX rate_hz=24000 vita_gaps=0 "
        "null_frames=1 pcm_clips=0 pcm_invalid=0 send_failures=0 "
        "queue_max=640 tail_samples=512 tail_us=410000 "
        "preroll_frames=19 preroll_delay_ms=390 ambe_queue_max=20 "
        "ambe_underflows=1 ambe_overflows=2 ambe_sequence_errors=3 "
        "vocoder_submit_failures=4 vocoder_pending_max=5 drain_frames=19 "
        "drain_timeouts=1 drain_discarded_frames=6";
    report("telemetry parser: accepts TX pre-roll and drain metrics",
           DigitalVoiceWaveformTelemetryParser::parseMetricLine(
               txPipelineMetricLine, &parsedMetrics)
               && parsedMetrics.txPreRollFrames == 19U
               && parsedMetrics.txPreRollDelayMs == 390U
               && parsedMetrics.txAmbeQueueMax == 20U
               && parsedMetrics.txAmbeUnderflows == 1U
               && parsedMetrics.txAmbeOverflows == 2U
               && parsedMetrics.txAmbeSequenceErrors == 3U
               && parsedMetrics.txVocoderSubmitFailures == 4U
               && parsedMetrics.txVocoderPendingMax == 5U
               && parsedMetrics.txDrainFrames == 19U
               && parsedMetrics.txDrainTimeouts == 1U
               && parsedMetrics.txDrainDiscardedFrames == 6U);
    report("telemetry parser: rejects incomplete version 3 TX pipeline metrics",
           !DigitalVoiceWaveformTelemetryParser::parseMetricLine(
               txPipelineMetricLine.left(
                   txPipelineMetricLine.indexOf(" drain_discarded_frames=")),
               &parsedMetrics));
    report("telemetry parser: rejects incomplete TX metric",
           !DigitalVoiceWaveformTelemetryParser::parseMetricLine(
               "AETHER_DV_METRIC v=2 mode=DSTR dir=TX rate_hz=24000 "
               "vita_gaps=0 null_frames=0 pcm_clips=0 pcm_invalid=0 "
               "send_failures=0 queue_max=0 tail_samples=0",
               &parsedMetrics));
    report("telemetry parser: rejects missing required field",
           !DigitalVoiceWaveformTelemetryParser::parseMetricLine(
               "AETHER_DV_METRIC v=1 mode=DSTR rx_hz=24000 vita_gaps=0 "
               "source_blocks=0 turn_mean_us=10 turn_max_us=20",
               &parsedMetrics));
    report("telemetry parser: rejects non-finite values",
           !DigitalVoiceWaveformTelemetryParser::parseMetricLine(
               "AETHER_DV_METRIC v=1 mode=DSTR rx_hz=nan vita_gaps=0 "
               "source_blocks=0 turn_mean_us=10 turn_max_us=20 queue_max=0",
               &parsedMetrics));
    report("telemetry parser: rejects duplicate fields",
           !DigitalVoiceWaveformTelemetryParser::parseMetricLine(
               metricLine + " queue_max=2", &parsedMetrics));

    DigitalVoiceWaveformTelemetryParser lineParser;
    report("line buffering: incomplete process output is retained",
           lineParser.append(metricLine.left(42)).isEmpty());
    const QList<QByteArray> completedLines =
        lineParser.append(metricLine.mid(42) + "\r\nnext line\n");
    report("line buffering: split metric is reconstructed once",
           completedLines.size() == 2 && completedLines[0] == metricLine);
    report("line buffering: following line remains intact",
           completedLines.size() == 2 && completedLines[1] == "next line");

    DigitalVoiceWaveformHealthTracker healthTracker;
    DigitalVoiceWaveformMetrics healthMetrics;
    healthMetrics.valid = true;
    healthMetrics.rxSampleRateHz = 23000.0;
    for (int i = 0; i < DigitalVoiceWaveformHealthTracker::kLowCadenceWindows - 1; ++i) {
        healthTracker.observe(healthMetrics);
    }
    report("health: isolated low-rate windows do not warn",
           healthTracker.health() == DigitalVoiceWaveformHealth::Measuring);
    report("health: five consecutive low-rate windows warn",
           healthTracker.observe(healthMetrics)
               == DigitalVoiceWaveformHealth::CadenceDegraded);

    healthMetrics.rxSampleRateHz = 24000.0;
    for (int i = 0; i < DigitalVoiceWaveformHealthTracker::kRecoveryWindows - 1; ++i) {
        healthTracker.observe(healthMetrics);
    }
    report("health: degraded state is sticky during recovery",
           healthTracker.health() == DigitalVoiceWaveformHealth::CadenceDegraded);
    report("health: ten healthy windows clear degradation",
           healthTracker.observe(healthMetrics) == DigitalVoiceWaveformHealth::Healthy);

    healthTracker.reset();
    healthMetrics.sourceBlockDeficits = 1U;
    report("health: one inferred source deficit window is not enough",
           healthTracker.observe(healthMetrics) == DigitalVoiceWaveformHealth::Measuring);
    report("health: repeated inferred source deficits warn",
           healthTracker.observe(healthMetrics)
               == DigitalVoiceWaveformHealth::SourceDeficits);

    healthTracker.reset();
    healthMetrics.sourceBlockDeficits = 0U;
    healthMetrics.vitaSequenceGaps = 1U;
    report("health: true VITA loss warns immediately",
           healthTracker.observe(healthMetrics)
               == DigitalVoiceWaveformHealth::TransportLoss);

    DigitalVoiceWaveformHistoryTracker historyTracker;
    DigitalVoiceWaveformMetrics historyMetrics;
    historyMetrics.valid = true;
    historyMetrics.generation = 1U;
    historyMetrics.timestampMs = 1000;
    historyMetrics.rxSampleRateHz = 24000.0;
    historyMetrics.vitaSequenceGaps = 4U;
    historyMetrics.sourceBlockDeficits = 6U;
    historyMetrics.vitaSequenceGapsTotal = 4U;
    historyMetrics.sourceBlockDeficitsTotal = 6U;
    const auto baselineReading = historyTracker.sample(historyMetrics, 1000, 1.0);
    report("history: first report uses only its latest window",
           baselineReading.valid
               && baselineReading.vitaSequenceGapsPerSecond == 4.0
               && baselineReading.sourceBlockDeficitsPerSecond == 6.0);
    const auto duplicateReading = historyTracker.sample(historyMetrics, 2000, 1.0);
    report("history: repeated poll does not double-count a report",
           duplicateReading.valid
               && duplicateReading.vitaSequenceGapsPerSecond == 0.0
               && duplicateReading.sourceBlockDeficitsPerSecond == 0.0);
    historyMetrics.timestampMs = 2000;
    historyMetrics.vitaSequenceGaps = 2U;
    historyMetrics.sourceBlockDeficits = 3U;
    historyMetrics.vitaSequenceGapsTotal = 6U;
    historyMetrics.sourceBlockDeficitsTotal = 9U;
    const auto deltaReading = historyTracker.sample(historyMetrics, 2000, 1.0);
    report("history: cumulative totals become per-second deltas",
           deltaReading.vitaSequenceGapsPerSecond == 2.0
               && deltaReading.sourceBlockDeficitsPerSecond == 3.0);
    report("history: stale telemetry is no data",
           !historyTracker.sample(historyMetrics, 6000, 1.0).valid);
    historyMetrics.generation = 2U;
    historyMetrics.timestampMs = 7000;
    historyMetrics.vitaSequenceGaps = 1U;
    historyMetrics.sourceBlockDeficits = 1U;
    historyMetrics.vitaSequenceGapsTotal = 1U;
    historyMetrics.sourceBlockDeficitsTotal = 1U;
    const auto restartedReading = historyTracker.sample(historyMetrics, 7000, 1.0);
    report("history: helper restart uses only the new latest window",
           restartedReading.valid
               && restartedReading.vitaSequenceGapsPerSecond == 1.0
               && restartedReading.sourceBlockDeficitsPerSecond == 1.0);
    historyMetrics.txValid = true;
    historyMetrics.txTimestampMs = 7000;
    historyMetrics.txSampleRateHz = 24000.0;
    historyMetrics.txVitaSequenceGaps = 2U;
    historyMetrics.txVitaSequenceGapsTotal = 2U;
    historyMetrics.txNullFrames = 3U;
    historyMetrics.txPcmClips = 1U;
    historyMetrics.txPcmInvalid = 0U;
    historyMetrics.txSendFailures = 1U;
    historyMetrics.txQueueMax = 700U;
    historyMetrics.txTailSamples = 480U;
    historyMetrics.txTailUs = 20000U;
    const auto txReading = historyTracker.sample(historyMetrics, 7000, 1.0);
    report("history: TX telemetry is sampled independently from RX",
           txReading.valid && txReading.rxValid && txReading.txValid
               && txReading.txSampleRateHz == 24000.0
               && txReading.txVitaSequenceGapsPerSecond == 2.0
               && txReading.txNullFrames == 3U
               && txReading.txPcmClips == 1U
               && txReading.txSendFailures == 1U
               && txReading.txTailSamples == 480U
               && txReading.txTailUs == 20000U);
    const auto duplicateTxReading =
        historyTracker.sample(historyMetrics, 8000, 1.0);
    report("history: repeated TX poll does not double-count gaps",
           duplicateTxReading.txValid
               && duplicateTxReading.txVitaSequenceGapsPerSecond == 0.0);

    DigitalVoiceWaveformProcess& process = DigitalVoiceWaveformProcess::instance();
    process.stop();
    const bool started = process.startForRadio(QHostAddress());
    report("startForRadio: null radio fails", !started);
    report("startForRadio: null radio sets failed state",
           process.state() == DigitalVoiceWaveformProcess::State::Failed);
    report("startForRadio: null radio stores useful error",
           process.lastError().contains(QStringLiteral("No connected radio address")));
    process.stop();
    report("stop: failed/not-running resets state",
           process.state() == DigitalVoiceWaveformProcess::State::Stopped);

    if (g_failed == 0) {
        std::printf("\nAll DigitalVoiceWaveformProcess tests passed.\n");
    } else {
        std::printf("\n%d test(s) failed.\n", g_failed);
    }
    return g_failed == 0 ? 0 : 1;
}
