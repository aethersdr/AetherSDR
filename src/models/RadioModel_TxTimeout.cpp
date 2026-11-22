// Host transmit time-out (RFC #7021): one timer for every keying path, armed
// on the radio's keyed edge. It only ever unkeys, through cancelLocalTransmit().
#include "RadioModel.h"
#include "TxController.h"
#include "core/AppSettings.h"
#include "core/LogManager.h"

#include <QScopeGuard>
#include <QTimer>

#include <algorithm>

namespace AetherSDR {
namespace {
const QString kTxTimeoutFeature = QStringLiteral("TxTimeout");
}

TxTimeoutSettings RadioModel::txTimeoutSettings() const
{
    return m_txTimeoutStored.value_or(
        TxTimeoutSettings::defaultsFor(m_transmitModel.interlockTimeout()));
}

bool RadioModel::setTxTimeoutSettings(const TxTimeoutSettings& requested)
{
    const TxTimeoutSettings settings = requested.clamped();
    const RadioSettingsScope scope = settingsScope();
    if (!scope.isValid() || !scope.hasRadioIdentity()) {
        qCWarning(lcProtocol) << "RadioModel: transmit time-out not stored: no radio identity yet";
        return false;
    }
    int version = 0;
    AppSettings::FeatureReadStatus state;
    QJsonObject doc = scope.featureExact(kTxTimeoutFeature, &version, &state);
    if (version > TxTimeoutSettings::kSchemaVersion
        || state == AppSettings::FeatureReadStatus::Corrupt
        || state == AppSettings::FeatureReadStatus::Unavailable) {
        qCWarning(lcProtocol) << "RadioModel: transmit time-out not stored: document unreadable"
                              << "or newer than schema" << TxTimeoutSettings::kSchemaVersion;
        return false;
    }
    const QJsonObject fields = settings.toJson();
    for (auto it = fields.begin(); it != fields.end(); ++it) {
        doc.insert(it.key(), it.value());
    }
    if (!scope.setFeature(kTxTimeoutFeature, TxTimeoutSettings::kSchemaVersion, doc)) {
        qCWarning(lcProtocol) << "RadioModel: transmit time-out settings write did not persist";
        return false;
    }
    m_txTimeoutStored = settings;
    // An over already running keeps the limit it armed with.
    if (!m_txTimeout.armed()) {
        m_txTimeout.setSettings(settings);
    }
    emit txTimeoutSettingsChanged();
    return true;
}

void RadioModel::loadTxTimeoutSettings()
{
    const std::optional<TxTimeoutSettings> before = m_txTimeoutStored;
    m_txTimeoutStored.reset();
    const RadioSettingsScope scope = settingsScope();
    if (scope.isValid() && scope.hasRadioIdentity()) {
        int version = 0;
        const QJsonObject doc = scope.featureExact(kTxTimeoutFeature, &version);
        if (version == TxTimeoutSettings::kSchemaVersion) {
            m_txTimeoutStored = TxTimeoutSettings::fromJson(
                doc, TxTimeoutSettings::defaultsFor(m_transmitModel.interlockTimeout()));
        }
    }
    if (m_txTimeoutStored != before) {
        emit txTimeoutSettingsChanged();
    }
}

QString RadioModel::txTimeoutKeyingSource() const
{
    if (m_transmitModel.isTuning()) {
        return QStringLiteral("TUNE");
    }
    if (m_transmitModel.atuStatus() == ATUStatus::InProgress) {
        return QStringLiteral("ATU");
    }
    if (m_radioPttKeyed) {
        return QStringLiteral("radio PTT input");
    }
    constexpr unsigned cw = static_cast<unsigned>(TxActivity::CwKey)
        | static_cast<unsigned>(TxActivity::CwPtt) | static_cast<unsigned>(TxActivity::Cwx);
    if (m_txOperationActivities & cw) {
        return QStringLiteral("CW");
    }
    if (m_txOperationActivities & static_cast<unsigned>(TxActivity::VoiceKeyer)) {
        return QStringLiteral("voice keyer");
    }
    if (m_txOperationActivities & static_cast<unsigned>(TxActivity::Navtex)) {
        return QStringLiteral("NAVTEX");
    }
    if (!m_txRequested) {
        return QStringLiteral("radio-reported (not keyed by this client)");
    }
    switch (m_transmitModel.activePttSource()) {
    case TransmitModel::PttSource::Mox: return QStringLiteral("MOX/PTT");
    case TransmitModel::PttSource::TciHardware: return QStringLiteral("TCI");
    case TransmitModel::PttSource::Footswitch: return QStringLiteral("footswitch");
    case TransmitModel::PttSource::Tune: return QStringLiteral("TUNE");
    case TransmitModel::PttSource::Dax: return QStringLiteral("DAX/CAT");
    case TransmitModel::PttSource::Atu: return QStringLiteral("ATU");
    case TransmitModel::PttSource::Wspr: return QStringLiteral("WSPR");
    }
    return QStringLiteral("unknown");
}

void RadioModel::publishTxTimeoutLatch(bool before)
{
    if (m_txTimeout.latched() != before) {
        if (!m_txTimeout.latched()) {
            m_txTimeoutRefusedRequests.clear();
        }
        emit txTimeoutLatchChanged(m_txTimeout.latched());
    }
    if (m_txTimeoutTimer && !m_txTimeout.armed() && !m_txTimeout.latched()) {
        m_txTimeoutTimer->stop();
    }
}

void RadioModel::onTxTimeoutTransmitting(bool transmitting)
{
    // multiFLEX: the host time-out covers only this client's own transmit
    // (ruling, ten9876); another station's over is not ours to time or stop.
    if (transmitting && m_flexBackend && !m_txOwnedByUs) {
        return;
    }
    const bool before = m_txTimeout.latched();
    if (transmitting) {
        if (!m_txTimeout.transmitting()) {
            m_txTimeout.setSettings(txTimeoutSettings());
        }
        m_txTimeout.setTransmitting(true, txTimeoutKeyingSource());
        // Flex re-sends state=TRANSMITTING within an over; a restart on each
        // would postpone the tick for as long as the status keeps coming.
        if (m_txTimeout.armed() && m_txTimeoutTimer && !m_txTimeoutTimer->isActive()) {
            m_txTimeoutTimer->start();
        }
    } else {
        m_txTimeout.setTransmitting(false, {});
        if (!m_txTimeout.latched() && !m_txCoordinator.hasIntents(m_txOperation)) {
            m_txTimeout.forgetHolders();
        }
    }
    publishTxTimeoutLatch(before);
}

void RadioModel::onTxTimeoutTick()
{
    const bool before = m_txTimeout.latched();
    switch (m_txTimeout.poll()) {
    case TxTimeoutGuard::Event::Warning: {
        const int secondsLeft = static_cast<int>((m_txTimeout.remainingMs() + 999) / 1000);
        emitInterlockNotification(tr("Transmit time-out in %1 s.").arg(secondsLeft),
                                  QStringLiteral("tx-timeout-warning"));
        emit txTimeoutWarning(secondsLeft);
        break;
    }
    case TxTimeoutGuard::Event::Expired:
        expireTransmitTimeout();
        return;
    case TxTimeoutGuard::Event::None:
        break;
    }
    publishTxTimeoutLatch(before);
}

void RadioModel::expireTransmitTimeout()
{
    const int afterS = m_txTimeout.lastExpiredAfterS();
    const QString source = m_txTimeout.source();
    qCWarning(lcProtocol).noquote() << "RadioModel: transmit time-out: stopped after" << afterS
                                    << "s; keying source:" << source;
    const QPointer<RadioModel> alive(this);
    {
        m_txTimeoutStopping = true;
        const auto stopping = qScopeGuard([alive] {
            if (alive) {
                alive->m_txTimeoutStopping = false;
            }
        });
        cancelLocalTransmit();
    }
    if (!alive) {
        return;
    }
    QString message = tr("Transmit time-out: transmission stopped after %1 s.").arg(afterS);
    message += QLatin1Char(' ') + (m_radioPttObservedDown
        ? tr("The radio's PTT input is still held: release it before keying again.")
        : tr("Release every PTT input before keying again."));
    emitInterlockNotification(message, QStringLiteral("tx-timeout-expired"));
    emit txTimeoutExpired(afterS, source);
    if (!alive) {
        return;
    }
    // poll() set the latch; the stop may already have cleared it.
    m_txTimeout.refreshLatch();
    emit txTimeoutLatchChanged(m_txTimeout.latched());
    publishTxTimeoutLatch(m_txTimeout.latched());
}

bool RadioModel::txTimeoutExemptHolder(const TxCoordinator::Request& request) const
{
    // The local controls' state is reset by the stop itself; their next
    // press is a new key-down, never a held one.
    return m_localTxController && m_localTxController->ownsRequest(request);
}

bool RadioModel::txTimeoutCanHold(const TxCoordinator::Request& request)
{
    return txTimeoutExemptHolder(request) || m_txTimeout.canHold(request.producerKey());
}

bool RadioModel::noteTxTimeoutHold(const TxCoordinator::Request& request)
{
    if (txTimeoutExemptHolder(request)) {
        return true;
    }
    return m_txTimeout.noteHold(request.producerKey(), request.producerAlive());
}

void RadioModel::noteTxTimeoutRelease(const TxCoordinator::Request& request)
{
    // A refused key-down's own teardown, on whichever release route its
    // producer uses, is not that input letting go (d274: TciServer's refusal
    // retires through requestProducerPttOff()).
    if (m_txKeyDownDepth > 0 || m_txTimeoutStopping || txTimeoutRefused(request)) {
        return;
    }
    const bool before = m_txTimeout.latched();
    m_txTimeout.noteRelease(request.producerKey());
    publishTxTimeoutLatch(before);
}

void RadioModel::noteTxInputReleased(const TxCoordinator::Request& anyRequestOfProducer)
{
    if (QThread::currentThread() != thread() || m_txTimeoutStopping) {
        return;
    }
    const bool before = m_txTimeout.latched();
    m_txTimeout.noteRelease(anyRequestOfProducer.producerKey());
    publishTxTimeoutLatch(before);
}

void RadioModel::noteTxKeyDownRefused(const TxCoordinator::Request& request)
{
    if (QThread::currentThread() != thread()) {
        return;
    }
    noteTxTimeoutRefused(request);
}

void RadioModel::noteTxTimeoutRefused(const TxCoordinator::Request& request)
{
    // Scoped to the refused input's own handling: the list is cleared when
    // control returns to the event loop, so the input's later off message
    // (CAT RX, rigctl T 0, a footswitch edge) is a release even when it
    // carries the same request. Bounded; the oldest entry goes first.
    if (m_txTimeoutRefusedRequests.size() >= 32) {
        m_txTimeoutRefusedRequests.erase(m_txTimeoutRefusedRequests.begin());
    }
    m_txTimeoutRefusedRequests.push_back(request.weak());
    if (!m_txTimeoutRefusedClearQueued) {
        m_txTimeoutRefusedClearQueued = true;
        QMetaObject::invokeMethod(this, [this] {
            m_txTimeoutRefusedClearQueued = false;
            m_txTimeoutRefusedRequests.clear();
        }, Qt::QueuedConnection);
    }
}

bool RadioModel::txTimeoutRefused(const TxCoordinator::Request& request) const
{
    return std::any_of(m_txTimeoutRefusedRequests.begin(), m_txTimeoutRefusedRequests.end(),
                       [&request](const auto& weak) { return weak.lock().sameRequest(request); });
}

} // namespace AetherSDR
