#include "AetherGpsTools.h"

#include "WaveformEnvironmentDialog.h"

#include "core/AetherGpsPackage.h"
#include "core/WaveformInstaller.h"
#include "models/FlexWaveformModel.h"
#include "models/RadioModel.h"

#include <QStandardPaths>

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

AetherGpsSettle* AetherGpsSettle::of(FlexWaveformModel& waveforms)
{
    if (auto* existing = waveforms.findChild<AetherGpsSettle*>(QString(), Qt::FindDirectChildrenOnly)) {
        return existing;
    }
    return new AetherGpsSettle(waveforms);
}

AetherGpsSettle::AetherGpsSettle(FlexWaveformModel& waveforms)
    : QObject(&waveforms)
    , m_waveforms(waveforms)
{
    m_timer.setSingleShot(true);
    m_timer.setInterval(30'000);
    connect(&m_timer, &QTimer::timeout, this, &AetherGpsSettle::cancel);
    connect(&waveforms, &FlexWaveformModel::waveformsChanged, this, [this] {
        if ((m_state == State::Install && listed()) || (m_state == State::Removal && !listed())) {
            cancel();
        }
    });
    // A reply that will never come (the session ended) must not leave the
    // install controls disabled.
    connect(&waveforms, &FlexWaveformModel::cleared, this, &AetherGpsSettle::cancel);
}

void AetherGpsSettle::start(bool installing)
{
    if (listed() == installing) {
        return;   // the list already agrees
    }
    m_state = installing ? State::Install : State::Removal;
    m_timer.start();   // restarts: an earlier change's timer can't end this one
    emit changed();
}

bool AetherGpsSettle::listed() const
{
    for (const FlexWaveformEntry& w : m_waveforms.waveforms()) {
        if (isAetherGpsWaveform(w.name)) {
            return true;
        }
    }
    return false;
}

void AetherGpsSettle::cancel()
{
    m_timer.stop();
    if (m_state == State::None) {
        return;
    }
    m_state = State::None;
    emit changed();
}

AetherGpsTools::AetherGpsTools(RadioModel* model, QObject* parent)
    : QObject(parent)
    , m_model(model)
{
    if (!model) {
        return;
    }
    FlexWaveformModel& waveforms = model->flexWaveformModel();
    m_settle = AetherGpsSettle::of(waveforms);
    auto refresh = [this] { emit changed(); };
    connect(m_settle, &AetherGpsSettle::changed, this, refresh);
    connect(&waveforms, &FlexWaveformModel::waveformsChanged, this, refresh);
    connect(&waveforms, &FlexWaveformModel::wfpStatusChanged, this, refresh);
    // The Waveforms window's uploads count toward busy() too.
    connect(WaveformInstallActivity::instance(), &WaveformInstallActivity::changed, this, refresh);
    connect(model, &RadioModel::connectionStateChanged, this, [this, refresh](bool connected) {
        if (!connected) {
            // A reply that will never come (the session ended) must not
            // leave the checkbox disabled.
            m_removing = false;
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
    // Any upload in this process counts: the Waveforms window's too.
    return m_removing || (m_settle && m_settle->active()) || WaveformInstaller::anyInstalling();
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
    // A deploy picks up staged environment variables: wait them out. The
    // reason comes from waveformEnvironmentInstallBlocker() itself.
    if (docker == DockerWaveformInstallBlocker::None && !waveformEnvironmentInstallBlocker(m_model).isEmpty()) {
        docker = DockerWaveformInstallBlocker::RuntimeStatusUnknown;
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
    // The Waveforms window's wording for the same state.
    const FlexWaveformModel& wf = m_model->flexWaveformModel();
    return dockerInstallBlockerMessage(
        dockerWaveformInstallBlocker(true, m_model->capabilities().platform, wf.wfpStatusSeen(),
                                     wf.wfpPowered(), wf.wfpReady()),
        wf.wfpPowered(), wf.wfpReady());
}

void AetherGpsTools::install()
{
    if (!m_model || blocker() != AetherGpsBlocker::None || busy() || installed()) {
        emit changed();
        return;
    }
    const bool docker = !remoteAccessUsesNativePackage(m_model->capabilities().platform);
    QString error;
    const QString path = writePackage(docker, &error);
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
        if (ok && m_settle) {
            m_settle->start(true);   // until the radio lists it
        }
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
        if (m_settle) {
            if (ok) {
                m_settle->start(false);   // until the radio drops it
            } else {
                m_settle->cancel();
            }
        }
        emit changed();
        if (!ok) {
            emit failed(tr("The radio didn't remove AetherGPS: %1")
                            .arg(message.isEmpty() ? tr("it refused") : message));
        }
    });
    // Held from the send, so the Waveforms window's row can't send another.
    if (m_settle) {
        m_settle->start(false);
    }
    if (container) {
        waveforms.requestRemoveContainer(name);
    } else {
        waveforms.requestUninstall(name);
    }
    emit changed();
}

void AetherGpsTools::awaitListing(bool installed)
{
    if (m_settle) {
        m_settle->start(installed);
    }
}

bool AetherGpsTools::awaitingRadio() const
{
    return m_settle && m_settle->active();
}

void AetherGpsTools::stopWaiting()
{
    if (m_settle) {
        m_settle->cancel();
    }
}

QString AetherGpsTools::writePackage(bool docker, QString* error)
{
    // The package ships inside AetherSDR; the installer wants a file.
    return aethergps::writeBundledPackage(
        docker ? AetherGpsPackage::kContainer : AetherGpsPackage::kLegacy,
        QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
            + QStringLiteral("/waveforms/aether-gps-%1").arg(QString::fromLatin1(AetherGpsPackage::kVersion)),
        error);
}

}  // namespace AetherSDR
