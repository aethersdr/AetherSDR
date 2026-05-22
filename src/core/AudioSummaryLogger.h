#pragma once

#include <QAudioFormat>
#include <QJsonObject>
#include <QString>

namespace AetherSDR::AudioSummaryLogger {

struct RxSinkSummary {
    QString deviceDescription;
    int sampleRate{0};
    int channelCount{0};
    QAudioFormat::SampleFormat sampleFormat{QAudioFormat::Unknown};
    bool resamplingActive{false};
    bool fallbackOccurred{false};
    QString fallbackReason;
};

struct TxSourceSummary {
    QString deviceDescription;
    int sampleRate{0};
    int channelCount{0};
    QAudioFormat::SampleFormat sampleFormat{QAudioFormat::Unknown};
    bool resamplingTo24k{false};
    bool fallbackOccurred{false};
    QString fallbackReason;
};

struct CwSidetoneSummary {
    QString backend;
    QString deviceDescription;
    int sampleRate{0};
    bool fallbackOccurred{false};
    QString fallbackReason;
};

struct AuxiliarySinkSummary {
    QString sinkName;
    QString deviceDescription;
    int sampleRate{0};
    int channelCount{0};
    QAudioFormat::SampleFormat sampleFormat{QAudioFormat::Unknown};
    bool resamplingActive{false};
    bool fallbackOccurred{false};
    QString fallbackReason;
};

struct OpenFailureSummary {
    QString path;
    QString backend;
    QString deviceDescription;
    QString attemptedFormats;
    QString failureReason;
    QString fallbackReason;
};

QString sampleFormatName(QAudioFormat::SampleFormat format);
QString formatStartupEnvironment(const QJsonObject& audioDevices);
QString formatRxSink(const RxSinkSummary& summary);
QString formatTxSource(const TxSourceSummary& summary);
QString formatCwSidetone(const CwSidetoneSummary& summary);
QString formatAuxiliarySink(const AuxiliarySinkSummary& summary);
QString formatOpenFailure(const OpenFailureSummary& summary);

void logStartupEnvironment(const QJsonObject& audioDevices);
void logRxSink(const RxSinkSummary& summary);
void logTxSource(const TxSourceSummary& summary);
void logCwSidetone(const CwSidetoneSummary& summary);
void logAuxiliarySink(const AuxiliarySinkSummary& summary);
void logOpenFailure(const OpenFailureSummary& summary);

} // namespace AetherSDR::AudioSummaryLogger
