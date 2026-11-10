#include "models/WsprSliceGuard.h"

#include "core/LogManager.h"
#include "core/WsprAgentClient.h"
#include "models/RadioModel.h"

#include <QDateTime>
#include <QTimer>

namespace AetherSDR {

WsprSliceGuard::WsprSliceGuard(RadioModel* radio, QObject* parent)
    : QObject(parent)
    , m_radio(radio)
    , m_client(new WsprAgentClient(this))
    , m_refresh(new QTimer(this))
{
    // Default source: the receiver's status, without spots.
    m_source = [this](const QHostAddress& address, std::function<void(std::optional<WsprStatus>)> done) {
        m_client->cancel();
        disconnect(m_client, nullptr, this, nullptr);
        auto finished = std::make_shared<bool>(false);
        connect(m_client, &WsprAgentClient::statusReceived, this,
                [done, finished](const WsprStatus& status) {
                    if (!std::exchange(*finished, true)) { done(status); }
                });
        connect(m_client, &WsprAgentClient::requestFailed, this,
                [done, finished](const QString& operation, const QString&, bool) {
                    if (operation == QLatin1String("status") && !std::exchange(*finished, true)) {
                        done(std::nullopt);
                    }
                });
        m_client->setRadioAddress(address);
        m_client->fetchStatus(QDateTime::currentSecsSinceEpoch());
    };
    m_refresh->setInterval(kRefreshMs);
    connect(m_refresh, &QTimer::timeout, this, &WsprSliceGuard::query);
    if (!m_radio) { return; }
    m_radio->setSliceAdoptionHoldOnConnect(kHoldMs);
    connect(m_radio, &RadioModel::sliceAdoptionHoldStarted, this, &WsprSliceGuard::query);
    connect(m_radio, &RadioModel::connectionStateChanged, this, [this](bool connected) {
        ++m_generation;
        if (connected) {
            m_refresh->start();
        } else {
            m_refresh->stop();
            m_client->cancel();
        }
    });
}

void WsprSliceGuard::setStatusSource(StatusSource source)
{
    m_source = std::move(source);
}

void WsprSliceGuard::query()
{
    if (!m_radio) { return; }
    const QHostAddress radio = m_radio->radioAddress();
    if (m_radio->isWan() || !wspr::reachableRadio(false, radio)) {
        m_radio->releaseSliceAdoption();
        return;
    }
    const quint64 generation = ++m_generation;
    const QPointer<WsprSliceGuard> alive(this);
    m_source(radio, [alive, generation](std::optional<WsprStatus> status) {
        if (!alive || generation != alive->m_generation) { return; }
        alive->apply(status);
    });
}

void WsprSliceGuard::apply(const std::optional<WsprStatus>& status)
{
    if (!m_radio) { return; }
    // An older receiver names neither; one that does not answer leaves what
    // is known as it is.
    if (status && status->clientHandle != 0) {
        if (status->sliceIndex != m_radio->externalSlice()) {
            qCInfo(lcProtocol) << "WsprSliceGuard: the WSPR receiver's slice is" << status->sliceIndex;
        }
        m_radio->setExternalSliceClient(status->clientHandle, status->sliceIndex, tr("WSPR receiver"));
    }
    m_radio->releaseSliceAdoption();
}

} // namespace AetherSDR
