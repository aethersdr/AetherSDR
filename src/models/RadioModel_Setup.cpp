#include "RadioModel.h"
#include "core/AppSettings.h"

#include <QPointer>
#include <QTimer>
#include <QThread>
#include <memory>

namespace AetherSDR {

RadioControlSession RadioModel::controlSession() const
{
    return {m_backendReceiverGeneration, m_sessionGeneration};
}

bool RadioModel::controlSessionCurrent(const RadioControlSession& session) const
{
    return session == controlSession() && isConnected() && !m_txSessionClosing
        && !m_expiringPendingCallbacks;
}

RadioSetupCompletion RadioModel::fencedSetupCompletion(
    QObject* context, const RadioSetupCompletion& completion, bool requiresLiveSession)
{
    if (!completion) { return {}; }
    const QPointer<RadioModel> model(this);
    const QPointer<QObject> owner(context ? context : this);
    const RadioControlSession session = controlSession();
    const auto finished = std::make_shared<bool>(false);
    const auto connection = std::make_shared<QMetaObject::Connection>();
    const RadioSetupCompletion reply = [model, owner, session, finished, connection, completion, requiresLiveSession]
        (const RadioSetupReply& result) {
        if (*finished) { return; }
        *finished = true;
        QObject::disconnect(*connection);
        if (!model || !owner) { return; }
        if (session != model->controlSession()
            || (requiresLiveSession && !model->controlSessionCurrent(session))) {
            completion({RadioSetupReply::Kind::Cancelled, -1,
                        QStringLiteral("the original radio session ended")});
        } else {
            completion(result);
        }
    };
    *connection = connect(this, &RadioModel::connectionStateChanged, this,
        [model, session, reply, requiresLiveSession] {
            if (!model || session != model->controlSession()
                || (requiresLiveSession && !model->controlSessionCurrent(session))) {
                reply({RadioSetupReply::Kind::Cancelled, -1,
                       QStringLiteral("the original radio session ended")});
            }
        });
    QTimer::singleShot(30000, this, [reply] {
        reply({RadioSetupReply::Kind::Failed, -1, QStringLiteral("no radio response")});
    });
    return reply;
}

ReceiveDispatch RadioModel::requestRadioSetup(const RadioSetupRequest& request,
                                             QObject* replyContext,
                                             const RadioSetupCompletion& completion)
{
    using Field = RadioSetupRequest::Field;
    if (QThread::currentThread() != thread()) { return ReceiveDispatch::Unsupported; }
    const QPointer<RadioModel> alive(this);
    const QPointer<QObject> owner(replyContext ? replyContext : this);
    const RadioControlSession session = controlSession();
    const bool identity = request.field == Field::Callsign || request.field == Field::ClientStation;
    const bool localDisplay = request.field == Field::ShowTxInWaterfall
        && backendCapabilities().clientPersistsShowTxInWaterfall();
    const bool calibration = request.field == Field::StartFrequencyCalibration;
    const auto pending = std::make_shared<bool>(true);
    const RadioSetupCompletion completed = [pending, completion](const RadioSetupReply& result) {
        *pending = false;
        if (completion) { completion(result); }
    };
    const RadioSetupCompletion reply = fencedSetupCompletion(replyContext,
        calibration ? completed : completion, !identity && !localDisplay);
    const auto persistIdentity = [&] {
        if (request.field == Field::Callsign) {
            setStationCallsign(std::get<QString>(request.value));
        } else if (request.field == Field::ClientStation) {
            AppSettings::instance().setValue("StationName", std::get<QString>(request.value));
            AppSettings::instance().save();
        }
    };
    const auto refuse = [&](bool announce = true) {
        if (reply) { reply({RadioSetupReply::Kind::Refused, -1, QStringLiteral("configuration not dispatched")}); }
        if (alive && announce) { reportReceiveDispatch(ReceiveDispatch::Unsupported, QStringLiteral("radio setup")); }
        return ReceiveDispatch::Unsupported;
    };
    if (!request.valid()) { return refuse(false); }
    // Client-owned display preferences remain editable without a radio link.
    if (request.field == Field::ShowTxInWaterfall
        && requestLocalShowTxInWaterfall(std::get<bool>(request.value))) {
        if (reply) { reply({RadioSetupReply::Kind::Accepted, 0, {}}); }
        return ReceiveDispatch::LocalOnly;
    }
    if (!m_backend || !controlSessionCurrent(session)) {
        if (identity) {
            persistIdentity();
            if (reply) { reply({RadioSetupReply::Kind::Accepted, 0, {}}); }
            return ReceiveDispatch::LocalOnly;
        }
        return refuse();
    }
    const RadioCapabilities caps = backendCapabilities();
    if ((request.field != Field::Callsign && request.field != Field::ClientStation
         && (!caps.radioSetupControl || !caps.radioSetupControl->supports(request.field)))
        || (request.field == Field::PrivateIpPolicy && !caps.hasPrivateIpConnectionPolicy)
        || ((request.field == Field::Dhcp || request.field == Field::StaticNetwork) && !caps.hasClientNetworkConfig)
        || (request.field == Field::NetworkMtu && !caps.usesVita49Transport)
        || ((request.field == Field::SharpnessLevel || request.field == Field::SharpnessAuto
             || request.field == Field::LowLatencyDigital) && !caps.hasSharpFilters)
        || (request.field == Field::MaximumPowerPercent && !caps.txPowerBands.isEmpty())) {
        return refuse();
    }
    RadioSetupRequest effective = request;
    if (effective.field == Field::NetworkMtu) {
        effective.value = networkMtuParam(std::get<int>(effective.value));
    }
    const RadioControlFence current = [alive, owner, session, pending, calibration] {
        return alive && alive->controlSessionCurrent(session) && (!calibration || (owner && *pending));
    };
    const ReceiveDispatch result = m_backend->requestRadioSetup(effective, reply, current);
    if (!alive) { return result; }
    if (identity) {
        persistIdentity();
        if (!alive) { return result; }
        if (result == ReceiveDispatch::Unsupported) {
            if (reply) { reply({RadioSetupReply::Kind::Accepted, 0, QStringLiteral("Station identity saved locally; radio copy not dispatched.")}); }
            return ReceiveDispatch::LocalOnly;
        }
    }
    if (!controlSessionCurrent(session)) {
        if (reply) { reply({RadioSetupReply::Kind::Cancelled, -1, QStringLiteral("the original radio session ended")}); }
        return result;
    }
    if (result == ReceiveDispatch::Unsupported) { return refuse(); }
    if (result == ReceiveDispatch::LocalOnly && reply) {
        reply({RadioSetupReply::Kind::Accepted, 0, {}});
    }
    return result;
}

ReceiveDispatch RadioModel::requestTxBand(const TxBandRequest& request,
                                         std::optional<RadioControlSession> expectedSession)
{
    if (QThread::currentThread() != thread()) { return ReceiveDispatch::Unsupported; }
    const QPointer<RadioModel> alive(this);
    const RadioControlSession session = controlSession();
    if (expectedSession && *expectedSession != session) { return ReceiveDispatch::Unsupported; }
    const ReceiveDispatch result = m_backend && controlSessionCurrent(session)
        && request.valid() && m_txBandSettings.contains(request.bandId)
        ? m_backend->requestTxBand(request) : ReceiveDispatch::Unsupported;
    if (alive && session == controlSession()) {
        reportReceiveDispatch(result, QStringLiteral("TX band setting"));
    }
    return result;
}

ReceiveDispatch RadioModel::requestUsbCable(const UsbCableRequest& request)
{
    if (QThread::currentThread() != thread()) { return ReceiveDispatch::Unsupported; }
    const QPointer<RadioModel> alive(this);
    const RadioControlSession session = controlSession();
    const ReceiveDispatch result = m_backend && controlSessionCurrent(session)
        && request.valid() && m_usbCableModel.cables().contains(request.serial)
        && (request.bit < 0 || m_usbCableModel.cables().value(request.serial).type == QLatin1String("bit"))
        ? m_backend->requestUsbCable(request) : ReceiveDispatch::Unsupported;
    if (alive && session == controlSession()) {
        reportReceiveDispatch(result, QStringLiteral("USB cable setting"));
    }
    return result;
}

ReceiveDispatch RadioModel::requestTransverter(const TransverterRequest& request,
                                              QObject* replyContext,
                                              const RadioSetupCompletion& completion,
                                              std::optional<RadioControlSession> expectedSession)
{
    if (QThread::currentThread() != thread()) { return ReceiveDispatch::Unsupported; }
    const QPointer<RadioModel> alive(this);
    const RadioControlSession session = controlSession();
    const RadioSetupCompletion reply = fencedSetupCompletion(replyContext, completion);
    if (expectedSession && *expectedSession != session) {
        if (reply) { reply({RadioSetupReply::Kind::Cancelled, -1, QStringLiteral("Original transverter editor session ended.")}); }
        return ReceiveDispatch::Unsupported;
    }
    const ReceiveDispatch result = m_backend && controlSessionCurrent(session) && request.valid()
        && (request.field == TransverterRequest::Field::Create || m_xvtrList.contains(request.id))
        ? m_backend->requestTransverter(request, reply) : ReceiveDispatch::Unsupported;
    if (alive && session == controlSession()) {
        if (result == ReceiveDispatch::Unsupported && reply) {
            reply({RadioSetupReply::Kind::Refused, -1, QStringLiteral("transverter configuration not dispatched")});
        }
        if (alive) { reportReceiveDispatch(result, QStringLiteral("transverter setting")); }
    }
    return result;
}

} // namespace AetherSDR
