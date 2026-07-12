#include "DigitalVoiceWaveformProcess.h"

#include "LogManager.h"
#include "DigitalVoiceWaveformSettings.h"

#include <QCoreApplication>
#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QList>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QTimer>

#include <limits>

#if !defined(Q_OS_WIN)
#include <unistd.h>
#endif

namespace AetherSDR {

namespace {

QString helperExecutableName()
{
#if defined(Q_OS_WIN)
    return QStringLiteral("aether-dv-waveform.exe");
#else
    return QStringLiteral("aether-dv-waveform");
#endif
}

bool dstarVerboseRxIdleDiagnosticsEnabled()
{
    const QByteArray value = qgetenv("AETHER_DSTAR_VERBOSE_RX_IDLE_DIAG").trimmed().toLower();
    return !value.isEmpty()
        && value != "0"
        && value != "false"
        && value != "no"
        && value != "off";
}

quint64 saturatedAdd(quint64 current, quint64 incoming)
{
    return std::numeric_limits<quint64>::max() - current < incoming
        ? std::numeric_limits<quint64>::max()
        : current + incoming;
}

} // namespace

DigitalVoiceWaveformProcess& DigitalVoiceWaveformProcess::instance()
{
    static DigitalVoiceWaveformProcess process;
    return process;
}

DigitalVoiceWaveformProcess::DigitalVoiceWaveformProcess(QObject* parent)
    : QObject(parent)
{
    m_process.setProcessChannelMode(QProcess::SeparateChannels);

    connect(&m_process, &QProcess::started, this, [this] {
        setState(State::Starting, tr("Registering %1")
            .arg(activeModeName()));
    });
    connect(&m_process, &QProcess::readyReadStandardOutput, this, [this] {
        drainOutput(QProcess::StandardOutput);
    });
    connect(&m_process, &QProcess::readyReadStandardError, this, [this] {
        drainOutput(QProcess::StandardError);
    });
    connect(&m_process,
            qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this,
            [this](int exitCode, QProcess::ExitStatus status) {
        drainOutput(QProcess::StandardOutput, true);
        drainOutput(QProcess::StandardError, true);
        resetTelemetry(false);
        deactivateModeAndRequestRestore();
        m_registrationVerified = false;

        if (m_state == State::Failed) {
            return;
        }
        if (m_state == State::Stopping) {
            setState(State::Stopped, tr("Stopped"));
            return;
        }

        if (status == QProcess::CrashExit) {
            fail(tr("Digital voice service crashed"));
            return;
        }
        if (exitCode != 0) {
            fail(m_helperError.isEmpty()
                     ? tr("Digital voice service exited with code %1").arg(exitCode)
                     : m_helperError);
            return;
        }
        setState(State::Stopped, tr("Stopped"));
    });
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            fail(tr("Failed to start digital voice service: %1").arg(m_process.errorString()));
        }
    });
}

QString DigitalVoiceWaveformProcess::stateName(State state)
{
    switch (state) {
    case State::Stopped:  return QStringLiteral("Stopped");
    case State::Starting: return QStringLiteral("Starting");
    case State::Running:  return QStringLiteral("Running");
    case State::Stopping: return QStringLiteral("Stopping");
    case State::Failed:   return QStringLiteral("Failed");
    }
    return QStringLiteral("Unknown");
}

QString DigitalVoiceWaveformProcess::healthName(DigitalVoiceWaveformHealth health)
{
    switch (health) {
    case DigitalVoiceWaveformHealth::Inactive:        return QStringLiteral("Inactive");
    case DigitalVoiceWaveformHealth::Measuring:       return QStringLiteral("Measuring");
    case DigitalVoiceWaveformHealth::Healthy:         return QStringLiteral("Healthy");
    case DigitalVoiceWaveformHealth::CadenceDegraded: return QStringLiteral("Cadence degraded");
    case DigitalVoiceWaveformHealth::TransportLoss:   return QStringLiteral("Transport loss");
    case DigitalVoiceWaveformHealth::SourceDeficits:  return QStringLiteral("Source deficits");
    }
    return QStringLiteral("Unknown");
}

QString DigitalVoiceWaveformProcess::healthDetail() const
{
    switch (m_health) {
    case DigitalVoiceWaveformHealth::CadenceDegraded:
        return tr("D-STAR waveform delivery is degraded (%1 of 24.00 ksps). "
                  "Other active radio clients may be consuming radio resources.")
            .arg(m_metrics.rxSampleRateHz / 1000.0, 0, 'f', 2);
    case DigitalVoiceWaveformHealth::TransportLoss:
        if (m_metrics.vitaSequenceGaps > 0U) {
            return tr("D-STAR waveform transport lost %n VITA packet(s) in the latest interval.",
                      nullptr,
                      static_cast<int>(m_metrics.vitaSequenceGaps));
        }
        return tr("D-STAR waveform transport is recovering after detected VITA packet loss.");
    case DigitalVoiceWaveformHealth::SourceDeficits:
        if (m_metrics.sourceBlockDeficits > 0U) {
            return tr("D-STAR waveform delivery omitted %n inferred 128-sample source block(s) "
                      "in the latest interval.",
                      nullptr,
                      static_cast<int>(m_metrics.sourceBlockDeficits));
        }
        return tr("D-STAR waveform delivery is recovering after inferred source-block deficits.");
    case DigitalVoiceWaveformHealth::Inactive:
    case DigitalVoiceWaveformHealth::Measuring:
    case DigitalVoiceWaveformHealth::Healthy:
        return {};
    }
    return {};
}

QString DigitalVoiceWaveformProcess::activeModeName() const
{
    return DigitalVoiceModeRegistry::descriptor(m_mode).displayName;
}

QString DigitalVoiceWaveformProcess::registrationName() const
{
    return DigitalVoiceModeRegistry::descriptor(m_mode).waveformName;
}

QString DigitalVoiceWaveformProcess::defaultExecutablePath()
{
    const QString appDir = QCoreApplication::applicationDirPath();
    const QString bundled = QDir(appDir).filePath(helperExecutableName());
    if (QFileInfo(bundled).isExecutable()) {
        return bundled;
    }

    const QString fromPath = QStandardPaths::findExecutable(helperExecutableName());
    if (!fromPath.isEmpty()) {
        return fromPath;
    }

    return bundled;
}

QString DigitalVoiceWaveformProcess::resolveExecutablePath(const QString& configuredPath)
{
    const QString trimmed = configuredPath.trimmed();
    if (!trimmed.isEmpty()) {
        return trimmed;
    }
    return defaultExecutablePath();
}

bool DigitalVoiceWaveformProcess::startForRadio(const QHostAddress& radioAddress,
                                                const QString& radioCallsign,
                                                DigitalVoiceModeId mode)
{
    if (isActive()) {
        return m_mode == mode;
    }
    if (m_state == State::Stopping
            || m_process.state() != QProcess::NotRunning) {
        return false;
    }

    m_lastError.clear();
    m_helperError.clear();
    m_registrationVerified = false;
    resetTelemetry(true);

    if (radioAddress.isNull()) {
        fail(tr("No connected radio address is available"));
        return false;
    }

    const QString configurationError =
        DigitalVoiceWaveformSettings::validationError(radioCallsign);
    if (!configurationError.isEmpty()) {
        fail(configurationError);
        return false;
    }
    const QString effectiveMyCall =
        DigitalVoiceWaveformSettings::effectiveMyCall(radioCallsign);

    const QString executable =
        resolveExecutablePath(DigitalVoiceWaveformSettings::executablePath());
    const QFileInfo exeInfo(executable);
    if (!exeInfo.exists() || !exeInfo.isFile() || !exeInfo.isExecutable()) {
        fail(tr("Digital voice executable is not available: %1").arg(executable));
        return false;
    }

    const DigitalVoiceWaveformSettings::Backend backend = DigitalVoiceWaveformSettings::backend();
    const QString backendArg = DigitalVoiceWaveformSettings::backendArgument(backend);
    const QString serialPort = DigitalVoiceWaveformSettings::serialPort().trimmed();
    if (DigitalVoiceWaveformSettings::backendRequiresSerial(backend) && serialPort.isEmpty()) {
        fail(tr("ThumbDV serial port is not configured"));
        return false;
    }
#if !defined(Q_OS_WIN)
    if (DigitalVoiceWaveformSettings::backendRequiresSerial(backend)
            && !QFileInfo::exists(serialPort)) {
        fail(tr("ThumbDV device is not connected: %1").arg(serialPort));
        return false;
    }
    if (DigitalVoiceWaveformSettings::backendRequiresSerial(backend)
            && access(QFile::encodeName(serialPort).constData(), R_OK | W_OK) != 0) {
#if defined(Q_OS_LINUX)
        fail(tr("AetherSDR cannot access the ThumbDV device: %1. "
                "Grant your user serial-device access with the AetherSDR udev rule "
                "or your distribution's serial group, then reconnect the device.")
                 .arg(serialPort));
#else
        fail(tr("AetherSDR cannot read and write the ThumbDV device: %1")
                 .arg(serialPort));
#endif
        return false;
    }
#endif

    m_mode = mode;
    QString ownershipError;
    if (!DigitalVoiceModeRegistry::instance().activateMode(mode, &ownershipError)) {
        fail(ownershipError);
        return false;
    }

    const DigitalVoiceModeDescriptor& descriptor =
        DigitalVoiceModeRegistry::descriptor(mode);
    setState(State::Starting, tr("Starting"));
    m_process.setProgram(executable);
    m_process.setArguments(launchArguments(radioAddress, effectiveMyCall, descriptor));
    m_process.setWorkingDirectory(exeInfo.absolutePath());
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("SSDR_RADIO_ADDRESS"), radioAddress.toString());
    env.insert(QStringLiteral("AETHER_DV_VOCODER"), backendArg);
    if (!serialPort.isEmpty()) {
        env.insert(QStringLiteral("AETHER_DV_THUMBDV_SERIAL"), serialPort);
    }
    env.insert(QStringLiteral("AETHER_DV_FAIL_FAST"), QStringLiteral("1"));
    env.insert(QStringLiteral("AETHER_DV_MODE"), descriptor.radioMode);
    env.insert(QStringLiteral("AETHER_DV_UNDERLYING_MODE"), descriptor.underlyingMode);
    env.insert(QStringLiteral("AETHER_DV_WAVEFORM_NAME"), descriptor.waveformName);
    env.insert(QStringLiteral("AETHER_DSTAR_MYCALL"), effectiveMyCall);
    env.insert(QStringLiteral("AETHER_DSTAR_MYCALL_SUFFIX"),
               DigitalVoiceWaveformSettings::myCallSuffix());
    env.insert(QStringLiteral("AETHER_DSTAR_URCALL"), DigitalVoiceWaveformSettings::urCall());
    env.insert(QStringLiteral("AETHER_DSTAR_RPT1"), DigitalVoiceWaveformSettings::rpt1());
    env.insert(QStringLiteral("AETHER_DSTAR_RPT2"), DigitalVoiceWaveformSettings::rpt2());
    env.insert(QStringLiteral("AETHER_DSTAR_MESSAGE"), DigitalVoiceWaveformSettings::message());
    m_process.setProcessEnvironment(env);
    qCInfo(lcWaveform) << "DigitalVoiceWaveformProcess: starting" << executable
                       << "for" << radioAddress.toString()
                       << "mode" << descriptor.radioMode
                       << "vocoder" << backendArg;
    const quint64 startGeneration = ++m_startGeneration;
    m_process.start();
    QTimer::singleShot(10000, this, [this, startGeneration]() {
        if (startGeneration != m_startGeneration || m_state != State::Starting) {
            return;
        }
        if (m_process.state() != QProcess::NotRunning) {
            m_process.kill();
        }
        fail(tr("Timed out while registering %1 with the radio")
            .arg(activeModeName()));
    });
    return true;
}

void DigitalVoiceWaveformProcess::stop()
{
    if (m_state == State::Stopped || m_state == State::Stopping) {
        return;
    }
    if (m_state == State::Failed && m_process.state() == QProcess::NotRunning) {
        setState(State::Stopped, tr("Stopped"));
        return;
    }

    resetTelemetry(false);
    ++m_startGeneration;
    setState(State::Stopping, tr("Stopping"));
    // Restore the operator's previous slice mode as soon as stop is requested.
    // Waiting for QProcess::finished() races the radio's automatic DSTR -> DFM
    // status, which can release the claim before we recover the saved mode.
    deactivateModeAndRequestRestore();
    if (m_process.state() == QProcess::NotRunning) {
        setState(State::Stopped, tr("Stopped"));
        return;
    }
    const quint64 stopGeneration = ++m_stopGeneration;
    m_process.terminate();
    // Do not block the GUI thread waiting for the helper to exit. Its shutdown
    // path can sleep for about a second before exiting, so escalate asynchronously.
    QTimer::singleShot(1500, this, [this, stopGeneration]() {
        if (stopGeneration != m_stopGeneration || m_state != State::Stopping
                || m_process.state() == QProcess::NotRunning) {
            return;
        }
        qCWarning(lcWaveform) << "DigitalVoiceWaveformProcess: terminate timed out; killing process";
        m_process.kill();
    });
}

void DigitalVoiceWaveformProcess::stopAndWait()
{
    ++m_stopGeneration;
    ++m_startGeneration;
    if (m_state == State::Stopped) {
        return;
    }
    if (m_state == State::Failed && m_process.state() == QProcess::NotRunning) {
        setState(State::Stopped, tr("Stopped"));
        return;
    }

    resetTelemetry(false);
    setState(State::Stopping, tr("Stopping"));
    deactivateModeAndRequestRestore();
    if (m_process.state() == QProcess::NotRunning) {
        setState(State::Stopped, tr("Stopped"));
        return;
    }

    m_process.terminate();
    if (!m_process.waitForFinished(1500)) {
        qCWarning(lcWaveform) << "DigitalVoiceWaveformProcess: terminate timed out; killing process";
        m_process.kill();
        if (!m_process.waitForFinished(1000)) {
            m_lastError = tr("D-STAR helper did not stop after being killed");
            qCWarning(lcWaveform) << "DigitalVoiceWaveformProcess:" << m_lastError;
            setState(State::Failed, m_lastError);
            return;
        }
    }
    setState(State::Stopped, tr("Stopped"));
}

void DigitalVoiceWaveformProcess::setState(State state, const QString& statusText)
{
    const bool didChangeState = m_state != state;
    m_state = state;

    const QString text = statusText.isEmpty() ? stateName(state) : statusText;
    if (m_statusText != text) {
        m_statusText = text;
        emit statusTextChanged(m_statusText);
    }
    if (didChangeState) {
        emit stateChanged(m_state);
    }
}

void DigitalVoiceWaveformProcess::fail(const QString& message)
{
    m_lastError = message;
    m_registrationVerified = false;
    deactivateModeAndRequestRestore();
    resetTelemetry(false);
    if (m_process.state() != QProcess::NotRunning) {
        m_process.kill();
    }
    qCWarning(lcWaveform) << "DigitalVoiceWaveformProcess:" << message;
    setState(State::Failed, message);
}

void DigitalVoiceWaveformProcess::deactivateModeAndRequestRestore()
{
    const std::optional<DigitalVoiceSliceClaim> claim =
        DigitalVoiceModeRegistry::instance().deactivateMode(m_mode);
    if (claim.has_value()) {
        emit sliceRestoreRequested(claim->sliceId, claim->previousMode);
    }
}

QStringList DigitalVoiceWaveformProcess::launchArguments(const QHostAddress& radioAddress,
                                                         const QString& effectiveMyCall,
                                                         const DigitalVoiceModeDescriptor& mode) const
{
    const DigitalVoiceWaveformSettings::Backend backend = DigitalVoiceWaveformSettings::backend();
    QStringList args {
        QStringLiteral("--host"), radioAddress.toString(),
        QStringLiteral("--vocoder"), DigitalVoiceWaveformSettings::backendArgument(backend),
        QStringLiteral("--mode"), mode.radioMode,
        QStringLiteral("--underlying-mode"), mode.underlyingMode,
        QStringLiteral("--waveform-name"), mode.waveformName,
        QStringLiteral("--mycall"), effectiveMyCall,
        QStringLiteral("--mycall-suffix"), DigitalVoiceWaveformSettings::myCallSuffix(),
        QStringLiteral("--urcall"), DigitalVoiceWaveformSettings::urCall(),
        QStringLiteral("--rpt1"), DigitalVoiceWaveformSettings::rpt1(),
        QStringLiteral("--rpt2"), DigitalVoiceWaveformSettings::rpt2(),
        QStringLiteral("--message"), DigitalVoiceWaveformSettings::message()
    };
    if (DigitalVoiceWaveformSettings::backendRequiresSerial(backend)) {
        args << QStringLiteral("--serial") << DigitalVoiceWaveformSettings::serialPort().trimmed();
    }
    return args;
}

void DigitalVoiceWaveformProcess::drainOutput(QProcess::ProcessChannel channel, bool flush)
{
    m_process.setReadChannel(channel);
    const QByteArray output = m_process.readAll();
    DigitalVoiceWaveformTelemetryParser& parser = channel == QProcess::StandardOutput
        ? m_stdoutParser
        : m_stderrParser;
    QList<QByteArray> lines = parser.append(output);
    if (flush) {
        lines.append(parser.finish());
    }
    processLines(lines);
}

void DigitalVoiceWaveformProcess::processLines(const QList<QByteArray>& lines)
{
    for (const QByteArray& rawLine : lines) {
        if (rawLine.isEmpty()) {
            continue;
        }
        if (DigitalVoiceWaveformTelemetryParser::isMetricLine(rawLine)) {
            DigitalVoiceWaveformMetrics parsed;
            if (DigitalVoiceWaveformTelemetryParser::parseMetricLine(rawLine, &parsed)) {
                updateMetrics(parsed);
            } else {
                qCWarning(lcWaveform) << "DigitalVoiceWaveformProcess: rejected malformed telemetry";
            }
            continue;
        }

        if (rawLine.startsWith("AETHER_DV_READY ")) {
            const QByteArray expectedMode =
                DigitalVoiceModeRegistry::descriptor(m_mode).radioMode.toLatin1();
            const QByteArray expectedWaveform = registrationName().toLatin1();
            const QList<QByteArray> fields = rawLine.split(' ');
            if (fields.count("mode=" + expectedMode) != 1
                    || fields.count("waveform=" + expectedWaveform) != 1) {
                fail(tr("Digital voice helper reported an unexpected registration"));
                continue;
            }
            ++m_startGeneration;
            m_helperError.clear();
            m_registrationVerified = true;
            setState(State::Running, tr("Running"));
            setHealth(DigitalVoiceWaveformHealth::Measuring);
            continue;
        }

        QString line = QString::fromLocal8Bit(rawLine).trimmed();
        if (line.isEmpty()) {
            continue;
        }
        if (line.size() > 1000) {
            line = line.left(1000);
        }
        if (line.startsWith(QStringLiteral("AETHER_DV_ERROR "))) {
            m_helperError = line.mid(QStringLiteral("AETHER_DV_ERROR ").size()).trimmed();
            qCWarning(lcWaveform).noquote()
                << "DigitalVoiceWaveformProcess:" << m_helperError;
            emit processOutput(line);
            continue;
        }
        if (line.startsWith(QStringLiteral("AETHER_DV_DEVICE state=disconnected"))) {
            setState(m_state, tr("ThumbDV disconnected; reconnecting"));
        } else if (line.startsWith(QStringLiteral("AETHER_DV_DEVICE state=connected"))
                   && m_state == State::Running) {
            m_helperError.clear();
            setState(State::Running, tr("Running"));
        }
        const bool isDiagnostic = line.startsWith(QStringLiteral("AETHER_DSTAR_DIAG "));
        const bool verboseRxIdle = dstarVerboseRxIdleDiagnosticsEnabled();
        if ((!verboseRxIdle && line.startsWith(QStringLiteral("AETHER_DSTAR_DIAG rx_input_buffer")))
                || (!verboseRxIdle && line.startsWith(QStringLiteral("AETHER_DSTAR_DIAG rx_zero_output")))
                || line.startsWith(QStringLiteral("AETHER_DSTAR_DIAG tx_input_buffer"))
                || line.startsWith(QStringLiteral("AETHER_DSTAR_DIAG tx_inhibited"))) {
            continue;
        }
        if (isDiagnostic) {
            qCWarning(lcWaveform).noquote() << "DigitalVoiceWaveformProcess:" << line;
        } else {
            qCInfo(lcWaveform).noquote() << "DigitalVoiceWaveformProcess:" << line;
        }
        emit processOutput(line);
    }
}

void DigitalVoiceWaveformProcess::updateMetrics(DigitalVoiceWaveformMetrics metrics)
{
    metrics.valid = true;
    metrics.generation = m_metricsGeneration;
    metrics.reportSequence = ++m_metricsSequence;
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();

    if (metrics.direction == DigitalVoiceWaveformMetricDirection::Tx) {
        m_txVitaSequenceGapsTotal = saturatedAdd(
            m_txVitaSequenceGapsTotal, metrics.txVitaSequenceGaps);
        m_txNullFramesTotal = saturatedAdd(
            m_txNullFramesTotal, metrics.txNullFrames);
        m_txPcmClipsTotal = saturatedAdd(
            m_txPcmClipsTotal, metrics.txPcmClips);
        m_txPcmInvalidTotal = saturatedAdd(
            m_txPcmInvalidTotal, metrics.txPcmInvalid);
        m_txSendFailuresTotal = saturatedAdd(
            m_txSendFailuresTotal, metrics.txSendFailures);
        m_txAmbeUnderflowsTotal = saturatedAdd(
            m_txAmbeUnderflowsTotal, metrics.txAmbeUnderflows);
        m_txAmbeOverflowsTotal = saturatedAdd(
            m_txAmbeOverflowsTotal, metrics.txAmbeOverflows);
        m_txAmbeSequenceErrorsTotal = saturatedAdd(
            m_txAmbeSequenceErrorsTotal, metrics.txAmbeSequenceErrors);
        m_txVocoderSubmitFailuresTotal = saturatedAdd(
            m_txVocoderSubmitFailuresTotal, metrics.txVocoderSubmitFailures);
        m_txDrainTimeoutsTotal = saturatedAdd(
            m_txDrainTimeoutsTotal, metrics.txDrainTimeouts);
        m_txDrainDiscardedFramesTotal = saturatedAdd(
            m_txDrainDiscardedFramesTotal, metrics.txDrainDiscardedFrames);

        DigitalVoiceWaveformMetrics merged = m_metrics;
        merged.direction = DigitalVoiceWaveformMetricDirection::Tx;
        merged.generation = m_metricsGeneration;
        merged.reportSequence = metrics.reportSequence;
        if (!metrics.mode.isEmpty()) {
            merged.mode = metrics.mode;
        }
        merged.txValid = true;
        merged.txTimestampMs = nowMs;
        merged.txSampleRateHz = metrics.txSampleRateHz;
        merged.txVitaSequenceGaps = metrics.txVitaSequenceGaps;
        merged.txNullFrames = metrics.txNullFrames;
        merged.txPcmClips = metrics.txPcmClips;
        merged.txPcmInvalid = metrics.txPcmInvalid;
        merged.txSendFailures = metrics.txSendFailures;
        merged.txQueueMax = metrics.txQueueMax;
        merged.txTailSamples = metrics.txTailSamples;
        merged.txTailUs = metrics.txTailUs;
        merged.txPreRollFrames = metrics.txPreRollFrames;
        merged.txPreRollDelayMs = metrics.txPreRollDelayMs;
        merged.txAmbeQueueMax = metrics.txAmbeQueueMax;
        merged.txAmbeUnderflows = metrics.txAmbeUnderflows;
        merged.txAmbeOverflows = metrics.txAmbeOverflows;
        merged.txAmbeSequenceErrors = metrics.txAmbeSequenceErrors;
        merged.txVocoderSubmitFailures = metrics.txVocoderSubmitFailures;
        merged.txVocoderPendingMax = metrics.txVocoderPendingMax;
        merged.txDrainFrames = metrics.txDrainFrames;
        merged.txDrainTimeouts = metrics.txDrainTimeouts;
        merged.txDrainDiscardedFrames = metrics.txDrainDiscardedFrames;
        merged.txVitaSequenceGapsTotal = m_txVitaSequenceGapsTotal;
        merged.txNullFramesTotal = m_txNullFramesTotal;
        merged.txPcmClipsTotal = m_txPcmClipsTotal;
        merged.txPcmInvalidTotal = m_txPcmInvalidTotal;
        merged.txSendFailuresTotal = m_txSendFailuresTotal;
        merged.txAmbeUnderflowsTotal = m_txAmbeUnderflowsTotal;
        merged.txAmbeOverflowsTotal = m_txAmbeOverflowsTotal;
        merged.txAmbeSequenceErrorsTotal = m_txAmbeSequenceErrorsTotal;
        merged.txVocoderSubmitFailuresTotal = m_txVocoderSubmitFailuresTotal;
        merged.txDrainTimeoutsTotal = m_txDrainTimeoutsTotal;
        merged.txDrainDiscardedFramesTotal = m_txDrainDiscardedFramesTotal;
        m_metrics = merged;
        emit metricsChanged();
        return;
    }

    metrics.timestampMs = nowMs;
    m_vitaSequenceGapsTotal = saturatedAdd(
        m_vitaSequenceGapsTotal, metrics.vitaSequenceGaps);
    m_sourceBlockDeficitsTotal = saturatedAdd(
        m_sourceBlockDeficitsTotal, metrics.sourceBlockDeficits);
    metrics.vitaSequenceGapsTotal = m_vitaSequenceGapsTotal;
    metrics.sourceBlockDeficitsTotal = m_sourceBlockDeficitsTotal;
    metrics.txValid = m_metrics.txValid;
    metrics.txTimestampMs = m_metrics.txTimestampMs;
    metrics.txSampleRateHz = m_metrics.txSampleRateHz;
    metrics.txVitaSequenceGaps = m_metrics.txVitaSequenceGaps;
    metrics.txNullFrames = m_metrics.txNullFrames;
    metrics.txPcmClips = m_metrics.txPcmClips;
    metrics.txPcmInvalid = m_metrics.txPcmInvalid;
    metrics.txSendFailures = m_metrics.txSendFailures;
    metrics.txQueueMax = m_metrics.txQueueMax;
    metrics.txTailSamples = m_metrics.txTailSamples;
    metrics.txTailUs = m_metrics.txTailUs;
    metrics.txPreRollFrames = m_metrics.txPreRollFrames;
    metrics.txPreRollDelayMs = m_metrics.txPreRollDelayMs;
    metrics.txAmbeQueueMax = m_metrics.txAmbeQueueMax;
    metrics.txAmbeUnderflows = m_metrics.txAmbeUnderflows;
    metrics.txAmbeOverflows = m_metrics.txAmbeOverflows;
    metrics.txAmbeSequenceErrors = m_metrics.txAmbeSequenceErrors;
    metrics.txVocoderSubmitFailures = m_metrics.txVocoderSubmitFailures;
    metrics.txVocoderPendingMax = m_metrics.txVocoderPendingMax;
    metrics.txDrainFrames = m_metrics.txDrainFrames;
    metrics.txDrainTimeouts = m_metrics.txDrainTimeouts;
    metrics.txDrainDiscardedFrames = m_metrics.txDrainDiscardedFrames;
    metrics.txVitaSequenceGapsTotal = m_txVitaSequenceGapsTotal;
    metrics.txNullFramesTotal = m_txNullFramesTotal;
    metrics.txPcmClipsTotal = m_txPcmClipsTotal;
    metrics.txPcmInvalidTotal = m_txPcmInvalidTotal;
    metrics.txSendFailuresTotal = m_txSendFailuresTotal;
    metrics.txAmbeUnderflowsTotal = m_txAmbeUnderflowsTotal;
    metrics.txAmbeOverflowsTotal = m_txAmbeOverflowsTotal;
    metrics.txAmbeSequenceErrorsTotal = m_txAmbeSequenceErrorsTotal;
    metrics.txVocoderSubmitFailuresTotal = m_txVocoderSubmitFailuresTotal;
    metrics.txDrainTimeoutsTotal = m_txDrainTimeoutsTotal;
    metrics.txDrainDiscardedFramesTotal = m_txDrainDiscardedFramesTotal;

    const bool wasDegraded = isDegradedDigitalVoiceWaveformHealth(m_health);
    m_metrics = metrics;
    const DigitalVoiceWaveformHealth newHealth = m_healthTracker.observe(m_metrics);
    setHealth(newHealth);
    emit metricsChanged();
    if (!wasDegraded && isDegradedDigitalVoiceWaveformHealth(newHealth)) {
        emit degradationStarted(healthDetail());
    }
}

void DigitalVoiceWaveformProcess::resetTelemetry(bool advanceGeneration)
{
    const bool hadMetrics = m_metrics.valid || m_metrics.txValid;
    const bool hadHealth = m_health != DigitalVoiceWaveformHealth::Inactive;
    if (advanceGeneration) {
        ++m_metricsGeneration;
    }
    m_metricsSequence = 0;
    m_vitaSequenceGapsTotal = 0;
    m_sourceBlockDeficitsTotal = 0;
    m_txVitaSequenceGapsTotal = 0;
    m_txNullFramesTotal = 0;
    m_txPcmClipsTotal = 0;
    m_txPcmInvalidTotal = 0;
    m_txSendFailuresTotal = 0;
    m_txAmbeUnderflowsTotal = 0;
    m_txAmbeOverflowsTotal = 0;
    m_txAmbeSequenceErrorsTotal = 0;
    m_txVocoderSubmitFailuresTotal = 0;
    m_txDrainTimeoutsTotal = 0;
    m_txDrainDiscardedFramesTotal = 0;
    m_metrics = {};
    m_metrics.generation = m_metricsGeneration;
    m_healthTracker.reset();
    m_stdoutParser.reset();
    m_stderrParser.reset();
    m_health = DigitalVoiceWaveformHealth::Inactive;
    if (hadMetrics || advanceGeneration) {
        emit metricsChanged();
    }
    if (hadHealth) {
        emit healthChanged();
    }
}

void DigitalVoiceWaveformProcess::setHealth(DigitalVoiceWaveformHealth health)
{
    if (m_health == health) {
        return;
    }
    m_health = health;
    emit healthChanged();
}

} // namespace AetherSDR
