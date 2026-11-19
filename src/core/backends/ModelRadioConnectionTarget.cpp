#include "core/control/RadioConnectionTarget.h"

#include "core/RadioDiscovery.h"
#include "core/RtlSdrDiscovery.h"
#include "core/backends/sim/SimBackend.h"
#include "models/RadioModel.h"

#include <QHostAddress>
#include <QPointer>
#include <QThread>
#include <QTimer>
#include <QRegularExpression>

namespace AetherSDR::control {
namespace {

class ModelRadioConnectionTarget final : public RadioConnectionTarget {
public:
    explicit ModelRadioConnectionTarget(RadioModel* radio) : m_radio(radio)
    {
        m_timeout.setSingleShot(true);
        m_timeout.setInterval(30000);
        connect(&m_timeout, &QTimer::timeout, this, [this] {
            m_error = QStringLiteral("engine.timeout");
            disconnectRadio();
        });
        connect(&m_radio->smartLinkAccounts(), &SmartLinkAccountModel::radioListReceived,
                this, [this] { startRemoteIfDiscovered(); });
        connect(&m_radio->smartLinkAccounts(), &SmartLinkAccountModel::authFailed,
                this, [this] {
            if (m_remoteSerial.isEmpty() || m_state != State::Connecting) { return; }
            m_error = QStringLiteral("engine.authentication_failed");
            disconnectRadio();
        });
        connect(m_radio, &RadioModel::certFingerprintMismatch, this,
                [this](quint64 attempt, const QString&, const QString&, const QString&) {
            // No prompt or automatic pin replacement in a headless process.
            m_radio->rejectPresentedWanCert(attempt);
        });
        connect(m_radio, &RadioModel::connectionStateChanged, this, [this](bool connected) {
            if (connected && m_state == State::Connecting) {
                m_timeout.stop();
                setState(State::Connected);
            } else if (!connected && m_state == State::Connected) {
                // Cancel RadioModel's legacy reconnect after its signal stack
                // unwinds. A new headless connection requires a new intent.
                m_error = QStringLiteral("engine.failed");
                disconnectRadio();
            } else if (!connected && m_state == State::Disconnecting) {
                scheduleSettlement();
            } else if (connected && m_state == State::Disconnecting) {
                scheduleTeardown();
            }
            // Family replacement emits an old-backend disconnect during
            // connectToRadio(); it does not end the new Connecting attempt.
        });
        connect(m_radio, &RadioModel::connectionError, this, [this] {
            if (m_state == State::Connecting || m_state == State::Connected) {
                m_error = QStringLiteral("engine.failed");
                disconnectRadio();
            }
        });
    }

    ~ModelRadioConnectionTarget() override
    {
        m_timeout.stop();
        QObject::disconnect(&m_radio->smartLinkAccounts(), nullptr, this, nullptr);
        QObject::disconnect(m_radio, nullptr, this, nullptr);
        if (m_state != State::Idle) {
            m_radio->disconnectFromRadio();
        }
    }

    State state() const override { return m_state; }
    // Last lifecycle error. Deliberately survives settlement to Idle so a
    // polling client can still learn why the attempt ended; the next accepted
    // connectRadio() clears it (see docs/aetherd-local-connection-control.md).
    QString errorCode() const override { return m_error; }

    bool supports(const DiscoveredRadio& radio) const override
    {
        if (radio.family == SimBackend::familyName()) {
            return radio.transport == QStringLiteral("sim")
                && radio.serial == SimBackend::demoSerial();
        }
        if (radio.family == QStringLiteral("rtl")) {
            return radio.transport == QStringLiteral("usb") && RtlSdrDiscovery::isAvailable();
        }
        return radio.transport == QStringLiteral("lan")
            && (radio.family == QStringLiteral("flex") || radio.family == QStringLiteral("hl2")
                || radio.family == QStringLiteral("anan"));
    }

    void connectRadio(const DiscoveredRadio& radio) override
    {
        if (m_state != State::Idle || !supports(radio)) {
            return;
        }
        RadioInfo info;
        info.family = radio.family;
        info.serial = radio.serial;
        info.name = radio.name;
        info.model = radio.model;
        info.nickname = radio.nickname;
        info.version = radio.version;
        info.address = QHostAddress(radio.address);
        info.port = radio.port;
        m_error.clear();
        setState(State::Connecting);
        m_timeout.start(30000);
        m_radio->connectToRadio(info);
    }

    bool connectConfiguredRemote(const QString& account, const QString& serial) override
    {
        static const QRegularExpression accountId(QStringLiteral("^(legacy|[a-f0-9]{64})$"));
        static const QRegularExpression radioId(QStringLiteral("^[A-Za-z0-9_.:-]{1,128}$"));
        SmartLinkAccountModel& accounts = m_radio->smartLinkAccounts();
        if (m_state != State::Idle || m_radio->isConnected() || m_radio->isConnectAttemptInFlight()
            || accounts.accountLocked() || !accountId.match(account).hasMatch()
            || !radioId.match(serial).hasMatch()) { return false; }
        m_error.clear();
        m_remoteSerial = serial;
        m_remoteAccount = account;
        const QPointer<ModelRadioConnectionTarget> alive(this);
        setState(State::Connecting);
        if (!alive || m_state != State::Connecting || m_remoteAccount != account
            || m_remoteSerial != serial) { return true; }
        m_timeout.start(45000);
        accounts.setAccountId(account);
        if (!alive || m_state != State::Connecting || m_remoteAccount != account
            || m_remoteSerial != serial) { return true; }
        accounts.tryAutoLogin();
        if (alive) { startRemoteIfDiscovered(); }
        return true;
    }

    void disconnectRadio() override
    {
        if (m_state == State::Idle || m_state == State::Disconnecting) {
            return;
        }
        m_timeout.stop();
        m_remoteSerial.clear();
        m_remoteAccount.clear();
        setState(State::Disconnecting);
        scheduleTeardown();
    }

private:
    void startRemoteIfDiscovered()
    {
        if (m_state != State::Connecting || m_remoteSerial.isEmpty()
            || m_radio->isConnected() || m_radio->isConnectAttemptInFlight()) { return; }
        SmartLinkAccountModel& accounts = m_radio->smartLinkAccounts();
        if (!accounts.isConnected() || !accounts.isAuthenticated() || accounts.accountId() != m_remoteAccount) { return; }
        for (const WanRadioInfo& radio : accounts.radios()) {
            if (radio.serial != m_remoteSerial) { continue; }
            if (!m_radio->connectViaSmartLink(m_remoteSerial)) {
                m_error = QStringLiteral("engine.failed");
                disconnectRadio();
            }
            return;
        }
    }

    void setState(State state)
    {
        m_state = state;
        emit stateChanged();
    }

    void scheduleTeardown()
    {
        if (m_teardownQueued) {
            return;
        }
        m_teardownQueued = true;
        QTimer::singleShot(0, this, [this] {
            // Hold the reservation through the model's queued/backend cleanup.
            // Signals emitted by disconnectFromRadio cannot queue recursion.
            m_radio->disconnectFromRadio();
            m_teardownQueued = false;
            scheduleSettlement();
        });
    }

    void scheduleSettlement()
    {
        if (m_settlementQueued) {
            return;
        }
        m_settlementQueued = true;
        QTimer::singleShot(0, this, [this] {
            m_settlementQueued = false;
            // Blocking backend teardown may have queued the model's final
            // disconnected notification. Drain that turn before allowing reuse.
            if (!m_teardownQueued && m_state == State::Disconnecting
                && !m_radio->isConnected() && !m_radio->isConnectAttemptInFlight()) {
                setState(State::Idle);
            }
        });
    }

    RadioModel* m_radio;
    State m_state{State::Idle};
    QString m_error;
    QString m_remoteSerial;
    QString m_remoteAccount;
    QTimer m_timeout;
    bool m_teardownQueued{false};
    bool m_settlementQueued{false};
};

} // namespace

std::unique_ptr<RadioConnectionTarget> makeModelRadioConnectionTarget(RadioModel* radio)
{
    if (!radio || radio->thread() != QThread::currentThread()
        || radio->isConnected() || radio->isConnectAttemptInFlight()) {
        return {};
    }
    return std::make_unique<ModelRadioConnectionTarget>(radio);
}

} // namespace AetherSDR::control
