#include "AudioSummaryLogger.h"
#include "LogManager.h"

#include <QHash>
#include <QJsonValue>
#include <QMutex>
#include <QMutexLocker>

namespace AetherSDR::AudioSummaryLogger {
namespace {

QMutex& summaryMutex()
{
    static QMutex mutex;
    return mutex;
}

QHash<QString, QString>& lastSummaries()
{
    static QHash<QString, QString> summaries;
    return summaries;
}

QString yesNo(bool value)
{
    return value ? QStringLiteral("yes") : QStringLiteral("no");
}

QString valueOrUnavailable(QString value)
{
    value = value.trimmed();
    return value.isEmpty() ? QStringLiteral("Unavailable") : value;
}

QString valueOrUnknown(QString value)
{
    value = value.trimmed();
    return value.isEmpty() ? QStringLiteral("Unknown") : value;
}

QString field(const QString& name, const QString& value)
{
    return QStringLiteral("%1=\"%2\"").arg(name, valueOrUnknown(value));
}

QString fallbackText(bool fallbackOccurred, const QString& reason)
{
    if (!fallbackOccurred) {
        return QStringLiteral("fallback=no");
    }
    const QString detail = reason.trimmed();
    if (detail.isEmpty()) {
        return QStringLiteral("fallback=yes");
    }
    return QStringLiteral("fallback=yes (%1)").arg(detail);
}

QString modeText(int channelCount)
{
    if (channelCount == 1) {
        return QStringLiteral("mono");
    }
    if (channelCount == 2) {
        return QStringLiteral("stereo");
    }
    return QStringLiteral("%1ch").arg(channelCount);
}

QString savedDeviceText(const QJsonObject& audioDevices, const QString& prefix)
{
    const bool configured = audioDevices.value(prefix + QStringLiteral("_configured")).toBool(false);
    if (!configured) {
        return QStringLiteral("not configured");
    }
    return audioDevices.value(prefix + QStringLiteral("_present")).toBool(false)
        ? QStringLiteral("present")
        : QStringLiteral("missing");
}

bool emitIfChanged(const QString& key, const QString& summary)
{
    {
        QMutexLocker locker(&summaryMutex());
        QHash<QString, QString>& summaries = lastSummaries();
        if (summaries.value(key) == summary) {
            return false;
        }
        summaries.insert(key, summary);
    }

    qCInfo(lcAudioSummary).noquote() << summary;
    return true;
}

} // namespace

QString sampleFormatName(QAudioFormat::SampleFormat format)
{
    switch (format) {
    case QAudioFormat::Unknown: return QStringLiteral("Unknown");
    case QAudioFormat::UInt8:   return QStringLiteral("UInt8");
    case QAudioFormat::Int16:   return QStringLiteral("Int16");
    case QAudioFormat::Int32:   return QStringLiteral("Int32");
    case QAudioFormat::Float:   return QStringLiteral("Float");
    default:                    return QStringLiteral("Unknown");
    }
}

QString formatStartupEnvironment(const QJsonObject& audioDevices)
{
    const QJsonObject selectedOutput = audioDevices.value("selected_output").toObject();
    const QJsonObject defaultOutput = audioDevices.value("default_output").toObject();
    const QJsonObject selectedInput = audioDevices.value("selected_input").toObject();
    const QJsonObject defaultInput = audioDevices.value("default_input").toObject();
    const QJsonObject volumes = audioDevices.value("volumes").toObject();
    const QJsonObject txRoute = audioDevices.value("tx_route").toObject();

    QStringList lines;
    lines << QStringLiteral("Audio startup summary:")
          << QStringLiteral("  output selected=\"%1\" default=\"%2\" saved=%3 source=\"%4\"")
                 .arg(valueOrUnavailable(selectedOutput.value("description").toString()),
                      valueOrUnavailable(defaultOutput.value("description").toString()),
                      savedDeviceText(audioDevices, QStringLiteral("saved_output")),
                      valueOrUnknown(audioDevices.value("selected_output_source").toString()))
          << QStringLiteral("  input selected=\"%1\" default=\"%2\" saved=%3 source=\"%4\"")
                 .arg(valueOrUnavailable(selectedInput.value("description").toString()),
                      valueOrUnavailable(defaultInput.value("description").toString()),
                      savedDeviceText(audioDevices, QStringLiteral("saved_input")),
                      valueOrUnknown(audioDevices.value("selected_input_source").toString()))
          << QStringLiteral("  pcAudio=%1 txMicRoute=\"%2\" micSelection=\"%3\" dax=%4")
                 .arg(yesNo(volumes.value("pc_audio_enabled").toBool(false)),
                      valueOrUnknown(txRoute.value("input").toString()),
                      valueOrUnknown(txRoute.value("mic_selection").toString()),
                      txRoute.contains("dax_on")
                          ? yesNo(txRoute.value("dax_on").toBool(false))
                          : QStringLiteral("unknown"));

    return lines.join(QLatin1Char('\n'));
}

QString formatRxSink(const RxSinkSummary& summary)
{
    QStringList lines;
    lines << QStringLiteral("Audio RX sink summary:")
          << QStringLiteral("  %1 rate=%2Hz channels=%3 format=%4")
                 .arg(field(QStringLiteral("device"), summary.deviceDescription))
                 .arg(summary.sampleRate)
                 .arg(summary.channelCount)
                 .arg(sampleFormatName(summary.sampleFormat))
          << QStringLiteral("  resampling=%1 %2")
                 .arg(yesNo(summary.resamplingActive),
                      fallbackText(summary.fallbackOccurred, summary.fallbackReason));
    return lines.join(QLatin1Char('\n'));
}

QString formatTxSource(const TxSourceSummary& summary)
{
    QStringList lines;
    lines << QStringLiteral("Audio TX source summary:")
          << QStringLiteral("  %1 rate=%2Hz channels=%3 mode=%4 format=%5")
                 .arg(field(QStringLiteral("device"), summary.deviceDescription))
                 .arg(summary.sampleRate)
                 .arg(summary.channelCount)
                 .arg(modeText(summary.channelCount),
                      sampleFormatName(summary.sampleFormat))
          << QStringLiteral("  resampleTo24k=%1 %2")
                 .arg(yesNo(summary.resamplingTo24k),
                      fallbackText(summary.fallbackOccurred, summary.fallbackReason));
    return lines.join(QLatin1Char('\n'));
}

QString formatCwSidetone(const CwSidetoneSummary& summary)
{
    QStringList lines;
    lines << QStringLiteral("Audio CW sidetone summary:")
          << QStringLiteral("  backend=\"%1\" %2 rate=%3Hz")
                 .arg(valueOrUnknown(summary.backend),
                      field(QStringLiteral("device"), summary.deviceDescription))
                 .arg(summary.sampleRate)
          << QStringLiteral("  %1").arg(fallbackText(summary.fallbackOccurred,
                                                     summary.fallbackReason));
    return lines.join(QLatin1Char('\n'));
}

QString formatAuxiliarySink(const AuxiliarySinkSummary& summary)
{
    QStringList lines;
    lines << QStringLiteral("Audio auxiliary sink summary:")
          << QStringLiteral("  sink=\"%1\" %2 rate=%3Hz channels=%4 format=%5")
                 .arg(valueOrUnknown(summary.sinkName),
                      field(QStringLiteral("device"), summary.deviceDescription))
                 .arg(summary.sampleRate)
                 .arg(summary.channelCount)
                 .arg(sampleFormatName(summary.sampleFormat))
          << QStringLiteral("  resampling=%1 %2")
                 .arg(yesNo(summary.resamplingActive),
                      fallbackText(summary.fallbackOccurred, summary.fallbackReason));
    return lines.join(QLatin1Char('\n'));
}

QString formatOpenFailure(const OpenFailureSummary& summary)
{
    QStringList lines;
    lines << QStringLiteral("Audio open failure summary:")
          << QStringLiteral("  path=\"%1\" backend=\"%2\" %3")
                 .arg(valueOrUnknown(summary.path),
                      valueOrUnknown(summary.backend),
                      field(QStringLiteral("device"), summary.deviceDescription))
          << QStringLiteral("  attempted=\"%1\"")
                 .arg(valueOrUnknown(summary.attemptedFormats))
          << QStringLiteral("  reason=\"%1\"")
                 .arg(valueOrUnknown(summary.failureReason));
    const QString fallback = summary.fallbackReason.trimmed();
    if (!fallback.isEmpty()) {
        lines << QStringLiteral("  fallbackHistory=\"%1\"").arg(fallback);
    }
    return lines.join(QLatin1Char('\n'));
}

void logStartupEnvironment(const QJsonObject& audioDevices)
{
    emitIfChanged(QStringLiteral("startup"), formatStartupEnvironment(audioDevices));
}

void logRxSink(const RxSinkSummary& summary)
{
    emitIfChanged(QStringLiteral("rx"), formatRxSink(summary));
}

void logTxSource(const TxSourceSummary& summary)
{
    emitIfChanged(QStringLiteral("tx"), formatTxSource(summary));
}

void logCwSidetone(const CwSidetoneSummary& summary)
{
    emitIfChanged(QStringLiteral("cw"), formatCwSidetone(summary));
}

void logAuxiliarySink(const AuxiliarySinkSummary& summary)
{
    emitIfChanged(QStringLiteral("aux:%1").arg(summary.sinkName),
                  formatAuxiliarySink(summary));
}

void logOpenFailure(const OpenFailureSummary& summary)
{
    emitIfChanged(QStringLiteral("failure:%1:%2")
                      .arg(summary.path, summary.backend),
                  formatOpenFailure(summary));
}

} // namespace AetherSDR::AudioSummaryLogger
