#include "AetherGpsTools.h"

#include "WaveformEnvironmentDialog.h"

#include "core/AetherGpsPackage.h"
#include "core/WaveformInstaller.h"
#include "models/FlexWaveformModel.h"
#include "models/RadioModel.h"

#include <QStandardPaths>
#include <QTimer>

#include <memory>

namespace AetherSDR {

namespace {

bool listed(RadioModel* model, bool* container = nullptr)
{
    for (const FlexWaveformEntry& w : model->flexWaveformModel().waveforms()) {
        if (isAetherGpsWaveform(w.name)) {
            if (container) {
                *container = w.isContainer;
            }
            return true;
        }
    }
    return false;
}

}  // namespace

AetherGpsTools::AetherGpsTools(RadioModel* model, QObject* parent)
    : QObject(parent)
    , m_model(model)
{
    if (!model) {
        return;
    }
    FlexWaveformModel& waveforms = model->flexWaveformModel();
    auto refresh = [this] {
        if (m_removalPending && !installed()) {
            m_removalPending = false;
        }
        emit changed();
    };
    connect(&waveforms, &FlexWaveformModel::waveformsChanged, this, refresh);
    connect(&waveforms, &FlexWaveformModel::wfpStatusChanged, this, refresh);
    connect(model, &RadioModel::connectionStateChanged, this, [this, refresh](bool connected) {
        if (!connected) {
            m_removalPending = false;
        }
        refresh();
    });
    connect(model, &RadioModel::capabilitiesChanged, this, refresh);
}

bool AetherGpsTools::radioTakesWaveforms() const
{
    return m_model && m_model->isConnected() && m_model->backendCapabilities().hasWaveforms;
}

bool AetherGpsTools::installed() const
{
    return radioTakesWaveforms() && listed(m_model);
}

bool AetherGpsTools::busy() const
{
    return m_removing || m_removalPending || (m_installer && m_installer->isInstalling());
}

AetherGpsBlocker AetherGpsTools::blocker() const
{
    const bool connected = m_model && m_model->isConnected();
    if (!connected) {
        return aetherGpsBlocker(false, false, false, DockerWaveformInstallBlocker::NotConnected);
    }
    const RadioPlatform platform = m_model->capabilities().platform;
    const FlexWaveformModel& wf = m_model->flexWaveformModel();
    DockerWaveformInstallBlocker docker =
        dockerWaveformInstallBlocker(true, platform, wf.wfpStatusSeen(), wf.wfpPowered(), wf.wfpReady());
    // A deploy picks up staged environment variables: wait them out.
    if (docker == DockerWaveformInstallBlocker::None && !waveformEnvironmentInstallBlocker(m_model).isEmpty()) {
        docker = DockerWaveformInstallBlocker::WfpNotReady;
    }
    return aetherGpsBlocker(true, m_model->backendCapabilities().hasWaveforms,
                            remoteAccessUsesNativePackage(platform), docker);
}

QString AetherGpsTools::blockerReason() const
{
    switch (blocker()) {
    case AetherGpsBlocker::None:
        return {};
    case AetherGpsBlocker::NotConnected:
        return tr("Connect to a radio to install AetherGPS on it.");
    case AetherGpsBlocker::NoWaveforms:
        return tr("This radio doesn't take waveforms.");
    case AetherGpsBlocker::Container:
        break;
    }
    const QString staging = waveformEnvironmentInstallBlocker(m_model);
    if (!staging.isEmpty()) {
        return staging;
    }
    const FlexWaveformModel& wf = m_model->flexWaveformModel();
    return wf.wfpStatusSeen() ? tr("The radio's Waveform Processor isn't ready.")
                              : tr("The radio hasn't reported whether it can run containers yet.");
}

void AetherGpsTools::install()
{
    if (!m_model || blocker() != AetherGpsBlocker::None || busy() || installed()) {
        emit changed();
        return;
    }
    // The package ships inside AetherSDR; the installer wants a file.
    const bool docker = !remoteAccessUsesNativePackage(m_model->capabilities().platform);
    QString error;
    const QString path = aethergps::writeBundledPackage(
        docker ? AetherGpsPackage::kContainer : AetherGpsPackage::kLegacy,
        QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
            + QStringLiteral("/waveforms/aether-gps-%1").arg(QString::fromLatin1(AetherGpsPackage::kVersion)),
        &error);
    if (path.isEmpty()) {
        emit changed();
        emit failed(error);
        return;
    }
    if (!m_installer) {
        m_installer = new WaveformInstaller(m_model, this);
        connect(m_installer, &WaveformInstaller::progressChanged, this,
                [this](int percent, const QString&) { emit installProgress(percent); });
    }
    connect(m_installer, &WaveformInstaller::finished, this, [this](bool ok, const QString& message) {
        emit changed();
        if (!ok) {
            emit failed(tr("Couldn't install AetherGPS: %1").arg(message));
        }
    }, Qt::SingleShotConnection);
    if (docker) {
        m_installer->installDockerWaveform(path, m_model->controlSession());
    } else {
        m_installer->installLegacyWaveform(path, m_model->controlSession());
    }
    emit changed();
}

void AetherGpsTools::remove()
{
    bool container = false;
    if (!m_model || blocker() != AetherGpsBlocker::None || busy() || !radioTakesWaveforms()
        || !listed(m_model, &container)) {
        emit changed();
        return;
    }
    FlexWaveformModel& waveforms = m_model->flexWaveformModel();
    const QString name = QString::fromLatin1(kAetherGpsWaveformName);
    const QString action = container ? QStringLiteral("remove_container") : QStringLiteral("uninstall");
    m_removing = true;
    auto conn = std::make_shared<QMetaObject::Connection>();
    *conn = connect(&waveforms, &FlexWaveformModel::commandFinished, this,
                    [this, conn, name, action](const QString& done, const QString& which, bool ok,
                                               const QString& message) {
        if (done != action || which != name) {
            return;
        }
        disconnect(*conn);
        m_removing = false;
        if (ok) {
            removalAccepted();
        }
        emit changed();
        if (!ok) {
            emit failed(tr("The radio didn't remove AetherGPS: %1")
                            .arg(message.isEmpty() ? tr("it refused") : message));
        }
    });
    if (container) {
        waveforms.requestRemoveContainer(name);
    } else {
        waveforms.requestUninstall(name);
    }
    emit changed();
}

void AetherGpsTools::removalAccepted()
{
    if (!installed()) {
        return;   // already gone from the list
    }
    m_removalPending = true;
    QTimer::singleShot(30'000, this, [this] {
        if (m_removalPending) {
            m_removalPending = false;
            emit changed();
        }
    });
}

}  // namespace AetherSDR
