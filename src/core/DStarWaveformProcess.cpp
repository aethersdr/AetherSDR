#include "DStarWaveformProcess.h"

#include "DStarWaveformSettings.h"
#include "LogManager.h"

#include <QCoreApplication>
#include <QByteArray>
#include <QDir>
#include <QFileInfo>
#include <QList>
#include <QProcessEnvironment>
#include <QStandardPaths>

namespace AetherSDR {

DStarWaveformProcess& DStarWaveformProcess::instance()
{
    static DStarWaveformProcess process;
    return process;
}

DStarWaveformProcess::DStarWaveformProcess(QObject* parent)
    : QObject(parent)
{
    m_process.setProcessChannelMode(QProcess::SeparateChannels);

    connect(&m_process, &QProcess::started, this, [this] {
        const QString suffix = m_runningBackendLabel.isEmpty()
            ? QString{}
            : tr(" (%1)").arg(m_runningBackendLabel);
        setState(State::Running, tr("Running%1").arg(suffix));
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
        drainOutput(QProcess::StandardOutput);
        drainOutput(QProcess::StandardError);

        if (m_state == State::Stopping) {
            setState(State::Stopped, tr("Stopped"));
            return;
        }

        if (status == QProcess::CrashExit) {
            fail(tr("D-STAR waveform process crashed"));
            return;
        }
        if (exitCode != 0) {
            fail(tr("D-STAR waveform process exited with code %1").arg(exitCode));
            return;
        }
        setState(State::Stopped, tr("Stopped"));
    });
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            fail(tr("Failed to start D-STAR waveform process: %1").arg(m_process.errorString()));
        }
    });
}

QString DStarWaveformProcess::stateName(State state)
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

QString DStarWaveformProcess::defaultExecutablePath()
{
    const QString appDir = QCoreApplication::applicationDirPath();
#if defined(Q_OS_MAC)
    const QString bundled = QDir(appDir).filePath(QStringLiteral("aether-dstar-waveform"));
#else
    const QString bundled = QDir(appDir).filePath(QStringLiteral("aether-dstar-waveform"));
#endif
    if (QFileInfo(bundled).isExecutable()) {
        return bundled;
    }

    const QString fromPath = QStandardPaths::findExecutable(QStringLiteral("aether-dstar-waveform"));
    if (!fromPath.isEmpty()) {
        return fromPath;
    }

    return bundled;
}

QString DStarWaveformProcess::resolveExecutablePath(const QString& configuredPath)
{
    const QString trimmed = configuredPath.trimmed();
    if (!trimmed.isEmpty()) {
        return trimmed;
    }
    return defaultExecutablePath();
}

bool DStarWaveformProcess::startForRadio(const QHostAddress& radioAddress)
{
    if (isActive()) {
        return true;
    }

    m_lastError.clear();

    if (radioAddress.isNull()) {
        fail(tr("No connected radio address is available"));
        return false;
    }

    const QString executable =
        resolveExecutablePath(DStarWaveformSettings::executablePath());
    const QFileInfo exeInfo(executable);
    if (!exeInfo.exists() || !exeInfo.isFile() || !exeInfo.isExecutable()) {
        fail(tr("D-STAR waveform executable is not available: %1").arg(executable));
        return false;
    }

    const DStarWaveformSettings::Backend backend = DStarWaveformSettings::backend();
    const QString backendArg = DStarWaveformSettings::backendArgument(backend);
    const QString serialPort = DStarWaveformSettings::serialPort();
    if (DStarWaveformSettings::backendRequiresSerial(backend) && serialPort.isEmpty()) {
        fail(tr("ThumbDV serial port is not configured"));
        return false;
    }

    m_runningBackendLabel = DStarWaveformSettings::backendLabel(backend);
    setState(State::Starting, tr("Starting"));
    m_process.setProgram(executable);
    m_process.setArguments(launchArguments(radioAddress));
    m_process.setWorkingDirectory(exeInfo.absolutePath());
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("SSDR_RADIO_ADDRESS"), radioAddress.toString());
    env.insert(QStringLiteral("AETHER_DSTAR_VOCODER"), backendArg);
    if (!serialPort.isEmpty()) {
        env.insert(QStringLiteral("AETHER_DSTAR_THUMBDV_SERIAL"), serialPort);
    }
    env.insert(QStringLiteral("AETHER_DSTAR_FAIL_FAST"), QStringLiteral("1"));
    env.insert(QStringLiteral("AETHER_DSTAR_MODE"), QStringLiteral("DSTR"));
    env.insert(QStringLiteral("AETHER_DSTAR_UNDERLYING_MODE"), QStringLiteral("DFM"));
    m_process.setProcessEnvironment(env);
    qCInfo(lcWaveform) << "DStarWaveformProcess: starting" << executable
                       << m_process.arguments();
    m_process.start();
    return true;
}

void DStarWaveformProcess::stop()
{
    if (m_state == State::Stopped) {
        return;
    }
    if (m_state == State::Failed && m_process.state() == QProcess::NotRunning) {
        setState(State::Stopped, tr("Stopped"));
        return;
    }

    setState(State::Stopping, tr("Stopping"));
    if (m_process.state() == QProcess::NotRunning) {
        setState(State::Stopped, tr("Stopped"));
        return;
    }
    m_process.terminate();
    if (!m_process.waitForFinished(1500)) {
        qCWarning(lcWaveform) << "DStarWaveformProcess: terminate timed out; killing process";
        m_process.kill();
    }
}

void DStarWaveformProcess::setState(State state, const QString& statusText)
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

void DStarWaveformProcess::fail(const QString& message)
{
    m_lastError = message;
    m_runningBackendLabel.clear();
    qCWarning(lcWaveform) << "DStarWaveformProcess:" << message;
    setState(State::Failed, message);
}

QStringList DStarWaveformProcess::launchArguments(const QHostAddress& radioAddress) const
{
    const DStarWaveformSettings::Backend backend = DStarWaveformSettings::backend();
    QStringList args {
        QStringLiteral("--host"), radioAddress.toString(),
        QStringLiteral("--vocoder"), DStarWaveformSettings::backendArgument(backend),
        QStringLiteral("--mode"), QStringLiteral("DSTR"),
        QStringLiteral("--underlying-mode"), QStringLiteral("DFM")
    };
    if (DStarWaveformSettings::backendRequiresSerial(backend)) {
        args << QStringLiteral("--serial") << DStarWaveformSettings::serialPort();
    }
    return args;
}

void DStarWaveformProcess::drainOutput(QProcess::ProcessChannel channel)
{
    m_process.setReadChannel(channel);
    const QByteArray output = m_process.readAll();
    const QList<QByteArray> lines = output.split('\n');
    for (const QByteArray& rawLine : lines) {
        QString line = QString::fromLocal8Bit(rawLine).trimmed();
        if (line.isEmpty()) {
            continue;
        }
        if (line.size() > 1000) {
            line = line.left(1000);
        }
        qCInfo(lcWaveform).noquote() << "DStarWaveformProcess:" << line;
        emit processOutput(line);
    }
}

} // namespace AetherSDR
