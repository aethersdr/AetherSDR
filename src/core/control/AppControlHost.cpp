#include "AppControlHost.h"

#include "LocalControlServer.h"
#include "RadioConnectionTarget.h"
#include "RadioResourceAdapter.h"
#include "ReceiveControlTarget.h"
#include "SliceFrequencyTarget.h"

#include "models/RadioModel.h"

#include <QPointer>

namespace AetherSDR::control {

namespace {

// A connection the app's own UI makes. Connected exactly while the model is,
// which is what ReceiveControlGuard asks of a target before any receive verb.
// It never connects, disconnects or cancels a reconnect, and it is never bound
// to the service, so radio.connect and radio.disconnect stay absent.
class ObservedRadioConnectionTarget final : public RadioConnectionTarget {
public:
    explicit ObservedRadioConnectionTarget(RadioModel* radio) : m_radio(radio)
    {
        connect(radio, &RadioModel::connectionStateChanged, this, [this] { emit stateChanged(); });
        connect(radio, &RadioModel::backendRebuilt, this, [this] { emit stateChanged(); });
    }

    State state() const override
    {
        return m_radio && m_radio->isConnected() ? State::Connected : State::Idle;
    }
    QString errorCode() const override { return {}; }
    bool supports(const DiscoveredRadio&) const override { return false; }
    void connectRadio(const DiscoveredRadio&) override {}
    void disconnectRadio() override {}

private:
    QPointer<RadioModel> m_radio;
};

} // namespace

AppControlHost::AppControlHost() = default;
AppControlHost::~AppControlHost() = default;

std::unique_ptr<AppControlHost> AppControlHost::start(
    RadioModel* radio, const QString& socketName, QString* error)
{
    const auto fail = [error](const QString& why) -> std::unique_ptr<AppControlHost> {
        if (error) { *error = why; }
        return nullptr;
    };
    if (!radio || socketName.isEmpty()) {
        return fail(QStringLiteral("no radio model or socket name"));
    }
    std::unique_ptr<AppControlHost> host(new AppControlHost);
    // No connection target for the service: allowLocalControl admits the
    // current user as observer and controller of the bound receive targets.
    host->m_server = std::make_unique<LocalControlServer>(
        nullptr, LocalControlServer::Limits{}, nullptr, true);
    // Reserve first: arrivals are closed until every target is bound.
    if (!host->m_server->listen(socketName, LocalControlServer::ListenMode::ReserveEndpoint)) {
        const QString why = host->m_server->listenError();
        return fail(why.isEmpty()
            ? QStringLiteral("the endpoint '%1' is in use or unavailable").arg(socketName)
            : why);
    }
    host->m_connection = std::make_unique<ObservedRadioConnectionTarget>(radio);
    host->m_frequency = makeModelSliceFrequencyTarget(radio, host->m_connection.get());
    if (!host->m_frequency || !host->m_server->bindFrequencyTarget(host->m_frequency.get())) {
        return fail(QStringLiteral("cannot bind frequency control"));
    }
    host->m_receive = makeModelReceiveControlTarget(radio, host->m_connection.get());
    if (!host->m_receive || !host->m_server->bindReceiveTarget(host->m_receive.get())) {
        return fail(QStringLiteral("cannot bind receive control"));
    }
    host->m_resources = std::make_unique<RadioResourceAdapter>(
        radio, &host->m_server->resourceStore(), QStringLiteral("radio-1"), nullptr,
        host->m_connection.get());
    if (!host->m_server->startServing()) {
        return fail(QStringLiteral("cannot start serving"));
    }
    return host;
}

QString AppControlHost::endpoint() const
{
    return m_server ? m_server->fullServerName() : QString();
}

} // namespace AetherSDR::control
