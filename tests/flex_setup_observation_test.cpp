#include "FlexSetupStatusTestSupport.h"
#include "RadioSetupControlFixture.h"
#include "TestSettingsProfile.h"
#include "core/backends/flex/RadioConnection.h"
#include "core/backends/flex/WanConnection.h"
#include "core/AppSettings.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QSignalSpy>
#include <QSemaphore>
#include <QThread>
#include <atomic>
#include <cstdio>

namespace AetherSDR {
// A connected LAN session on the backend's own connection, with an injected
// writer: no socket is opened.
class SessionControlTestAccess {
public:
    static void begin(RadioModel& model, FlexBackend& backend)
    {
        model.m_connection = backend.connection();
        model.m_connection->m_state.store(ConnectionState::Connected);
        model.m_sessionCommands = SessionCommandGuard::create();
        model.m_sessionWire = model.m_connection;
        model.m_sessionWireGeneration = model.m_connection->commandSessionGeneration();
        model.m_connectedStatusEpoch = model.beginStatusSession();
        model.m_connection->setLineEpoch(model.m_statusEpoch);
    }
    static quint64 statusEpoch(const RadioModel& model) { return model.m_statusEpoch; }
    static void beginConnect(RadioModel& model) { model.beginStatusSession(); }
    static void connectedNow(RadioModel& model) { model.m_connectedStatusEpoch = model.m_statusEpoch; }
    static void installWriter(RadioConnection& connection, std::function<void(quint32, const QString&)> writer)
    { connection.m_commandSinkForTest = std::move(writer); }
};
} // namespace AetherSDR

using namespace AetherSDR;
namespace {
int failures = 0;
void check(bool result, const char* message)
{
    std::printf("[%s] %s\n", result ? "PASS" : "FAIL", message);
    failures += !result;
}
using Fields = QMap<QString, QString>;

void parserSemantics()
{
    const auto decode = [](const QString& object, const Fields& fields) {
        return decodeFlexSetupStatus(object, fields);
    };
    const auto sharp = decode("radio filter_sharpness cw", {{"level", "99"}, {"auto_level", "255"}});
    check(sharp && sharp->sharpness.level == 3 && sharp->sharpness.automatic == true,
          "sharpness uses case-insensitive mode, bounded level, and byte boolean");
    const auto partial = decode("radio filter_sharpness VOICE", {{"level", "bad"}, {"auto_level", "0"}});
    check(partial && !partial->sharpness.level && partial->sharpness.automatic == false,
          "bad or omitted sharpness fields stay absent");
    check(!decode("radio filter_sharpness CWjunk", {{"level", "1"}}), "mode tokens match exactly");
    const auto xvtr = decode("xvtr 9", {{"name", "\"2m\""}, {"rf_freq", "nan"}, {"if_freq", "28.125"},
        {"order", "17"}, {"rx_only", "2"}, {"is_valid", "0"}, {"lo_error", "-0.25"}});
    check(xvtr && xvtr->transverter.name == QStringLiteral("2m") && !xvtr->transverter.rfFreq
        && xvtr->transverter.ifFreq == 28.125 && !xvtr->transverter.order
        && !xvtr->transverter.rxOnly && xvtr->transverter.isValid == false
        && xvtr->transverter.loError == -0.25, "transverter fields validate independently without default-zero corruption");
    check(!decode("xvtr -1", {{"name", "bad"}}) && !decode("xvtr 99999999999999", {{"name", "bad"}}),
          "invalid object identities cannot alias transverter zero");
    const auto cable = decode("usb_cable SN", {{"type", "cat"}, {"name", QString("My") + QChar(0x7f) + "Cable"},
        {"enable", "255"}, {"speed", "broken"}, {"source_rx_ant", ""}, {"polarity", "unknown"}});
    check(cable && cable->usbCable.name == QStringLiteral("My Cable") && cable->usbCable.enabled == true
        && !cable->usbCable.speed && cable->usbCable.sourceRxAnt == QStringLiteral("")
        && !cable->usbCable.activeHigh, "USB decoded spaces, explicit empties, byte booleans, and invalid fields retain their meaning");
    for (const QString& token : {QStringLiteral("bad"), QStringLiteral("-1"), QStringLiteral("8"), QStringLiteral("99999999999")}) {
        check(!decode("usb_cable SN bit " + token, {{"enable", "1"}}), "malformed bit indexes cannot update bit zero");
    }
    check(!decode("usb_cable SN not_removed", {}), "removal is an exact object action");
    check(!decode("usb_cable " + QString(129, 's'), {{"type", "cat"}}), "USB identifiers are bounded");
    const auto reply = decode("usb_cable SN", {{"data:base64", "OzAwMDAuMDA="}});
    check(reply && reply->usbCable.passthroughData == QByteArray(";0000.00") && !reply->usbCable.type
        && !reply->usbCable.textData, "passthrough data decodes, padding kept, and sets no cable field");
    check(!decode("usb_cable SN", {{"data:base64", "not*base64"}})
        && !decode("usb_cable SN", {{"data:base64", QString(8193, 'A')}})
        && !decode("usb_cable SN bit 0", {{"data:base64", "UA=="}}),
          "malformed, oversized and bit-line passthrough data is rejected");
    const auto textReply = decode("usb_cable SN", {{"data:text", ";0000"}});
    check(textReply && textReply->usbCable.textData && !textReply->usbCable.passthroughData,
          "text-mode data is marked and carries no bytes");
    const auto feature = decode("license feature", {{"name", " Future_Feature "}, {"enabled", "1"}});
    check(feature && feature->license.name == "future_feature" && feature->license.reason == "unknown",
          "valid unknown license features remain diagnostic observations");
    check(!decode("license feature", {{"name", "x"}, {"enabled", "2"}}), "license grants require strict zero or one");
    check(!decode("license subscription", {{"name", "smartsdr+"}, {"expiration", "2026-11-01garbage"}}),
          "license dates do not accept trailing junk");
    const auto announced = decode("xvtr 3", {{"in_use", "1"}});
    check(announced && !announced->transverter.removed && !decode("xvtr 3", {{"in_use", "7"}}),
          "a bare in_use=1 announces a transverter; an invalid in_use alone is no report");
    const auto bounded = decode("usb_cable SN bit 0", {{"ptt_delay", "2147483647"}, {"tx_delay", "250"}});
    const auto rates = decode("usb_cable SN", {{"type", "cat"}, {"speed", "99999999"}});
    const auto rate = decode("usb_cable SN", {{"type", "cat"}, {"speed", "115200"}});
    check(bounded && !bounded->usbCable.pttDelayMs && bounded->usbCable.txDelayMs == 250
        && rates && !rates->usbCable.speed && rate && rate->usbCable.speed == 115200,
          "USB delays and serial rates are bounded like their siblings");
    check(isFlexSetupStatusObject("xvtr 1") && isFlexSetupStatusObject("usb_cable SN removed")
        && isFlexSetupStatusObject("license") && isFlexSetupStatusObject("license feature")
        && isFlexSetupStatusObject("radio filter_sharpness VOICE") && !isFlexSetupStatusObject("radio")
        && !isFlexSetupStatusObject("xvtrs") && !isFlexSetupStatusObject("radio filter_sharpnessX"),
          "the ingress owns exactly the four setup objects");
}

void acknowledgedSharpness()
{
    RadioModel radio;
    auto owned = std::make_unique<RadioSetupTestBackend>();
    RadioSetupTestBackend* backend = owned.get();
    radio.setBackendForTest(std::move(owned), QStringLiteral("flex"));
    using Field = RadioSetupRequest::Field;
    const auto request = [&](int value) { radio.requestRadioSetup({Field::SharpnessLevel, SharpnessLevel{SharpnessMode::Voice, value}}); };
    request(1);
    check(radio.filterSharpnessVoice() == 2, "sharpness intent waits for acceptance");
    const RadioSetupCompletion old = backend->replies.back();
    request(3);
    backend->replyLast({RadioSetupReply::Kind::Accepted, 0, {}});
    old({RadioSetupReply::Kind::Accepted, 0, {}});
    check(radio.filterSharpnessVoice() == 3, "late acknowledgement cannot replace a newer accepted intent");
    request(0);
    QSignalSpy refresh(&radio, &RadioModel::infoChanged);
    backend->replyLast({RadioSetupReply::Kind::Refused, 1, {}});
    check(radio.filterSharpnessVoice() == 3 && refresh.count() == 1,
          "refused sharpness write restores controls to the retained value");
    request(1);
    const RadioSetupCompletion beforeStatus = backend->replies.back();
    FlexSetupStatusTestPeer::deliver(radio, "radio filter_sharpness VOICE", {{"level", "0"}});
    beforeStatus({RadioSetupReply::Kind::Accepted, 0, {}});
    check(radio.filterSharpnessVoice() == 0, "authoritative status supersedes an outstanding acknowledgement");
    radio.requestRadioSetup({Field::SharpnessAuto, SharpnessAuto{SharpnessMode::Voice, true}});
    const RadioSetupCompletion automatic = backend->replies.back();
    request(2);
    backend->replyLast({RadioSetupReply::Kind::Accepted, 0, {}});
    automatic({RadioSetupReply::Kind::Accepted, 0, {}});
    check(radio.filterSharpnessVoice() == 2 && radio.filterSharpnessVoiceAuto(),
          "independent level and automatic fields can complete in either order");
    request(1);
    const RadioSetupCompletion accepted = backend->replies.back();
    request(3);
    const RadioSetupCompletion refused = backend->replies.back();
    accepted({RadioSetupReply::Kind::Accepted, 0, {}});
    refused({RadioSetupReply::Kind::Refused, 1, {}});
    check(radio.filterSharpnessVoice() == 1, "an older accepted write survives a newer refused one");
    request(3);
    const RadioSetupCompletion retired = backend->replies.back();
    RadioSetupControlTestAccess::disconnected(radio);
    retired({RadioSetupReply::Kind::Accepted, 0, {}});
    check(radio.filterSharpnessVoice() == 2 && !radio.filterSharpnessVoiceAuto(),
          "retired session cannot restore acknowledged sharpness values");

}

void acknowledgedRemoval()
{
    RadioModel radio;
    RadioSetupTestBackend* backend = installRadioSetupBackend(radio);
    RadioSetupControlTestAccess::transverter(radio, 6);
    std::optional<RadioSetupReply> removed;
    radio.requestTransverter({6, TransverterRequest::Field::Remove, std::monostate{}}, nullptr,
        [&](const RadioSetupReply& reply) { removed = reply; });
    const RadioSetupCompletion removal = backend->replies.back();
    FlexSetupStatusTestPeer::deliver(radio, "xvtr 6", {{"in_use", "0"}});
    removal({RadioSetupReply::Kind::Accepted, 0, {}});
    check(removed && removed->kind == RadioSetupReply::Kind::Accepted,
          "a removal the radio reports before its reply still completes as accepted");
}

void entityLifecycle()
{
    UsbCableModel cables;
    applyCableStatus(cables, "UNKNOWN", {});
    QMap<QString, QString> none;
    const auto removeUnknown = decodeFlexSetupStatus("usb_cable UNKNOWN removed", none);
    check(removeUnknown && (cables.applyObservation(removeUnknown->usbCable), cables.cables().isEmpty()),
          "unknown removals and untyped first reports create nothing");
    applyCableStatus(cables, "SN", {{"type", "cat"}, {"name", "base"}, {"enable", "1"}});
    const auto first = cables.cables().value("SN").lifetime;
    applyCableStatus(cables, "SN", {{"name", "new"}});
    check(first && cables.cables().value("SN").lifetime == first, "ordinary partial updates preserve the cable identity");
    const auto bit = decodeFlexSetupStatus("usb_cable UNKNOWN bit 0", {{"enable", "1"}});
    check(bit && (cables.applyObservation(bit->usbCable), !cables.cables().contains("UNKNOWN")),
          "unknown per-bit reports cannot create phantom cables");
    applyCableStatus(cables, "SN", {{"type", "bit"}});
    const auto retyped = cables.cables().value("SN").lifetime;
    check(retyped && !first->current() && retyped->current() && first != retyped,
          "retyping retires the old identity before any later write");
    check(cables.cables().value("SN").name == QStringLiteral("new") && cables.cables().value("SN").enabled,
          "a retype keeps the fields every family shares");
    applyCableStatus(cables, "BCD", {{"type", "bcd"}});
    const auto bcd = cables.cables().value("BCD").lifetime;
    applyCableStatus(cables, "BCD", {{"type", "vbcd"}});
    check(cables.cables().value("BCD").lifetime == bcd && bcd->current(), "BCD subtypes keep their identity");
    const auto removal = decodeFlexSetupStatus("usb_cable SN removed", none);
    cables.applyObservation(removal->usbCable);
    applyCableStatus(cables, "SN", {{"type", "bit"}});
    const auto replacement = cables.cables().value("SN").lifetime;
    check(!retyped->current() && replacement && replacement != retyped && cables.cables().value("SN").name.isEmpty(),
          "same-serial remove/recreate is a new cable: old identity retired, old fields gone");
}

void modelSemantics()
{
    RadioModel radio;
    installRadioSetupBackend(radio);
    const auto deliver = [&](const QString& object, const Fields& fields) {
        FlexSetupStatusTestPeer::deliver(radio, object, fields);
    };
    deliver("radio filter_sharpness VOICE", {{"level", "3"}, {"auto_level", "1"}});
    deliver("radio filter_sharpness VOICE", {{"level", "1"}});
    check(radio.filterSharpnessVoice() == 1 && radio.filterSharpnessVoiceAuto(),
          "partial sharpness level preserves automatic mode");
    deliver("radio filter_sharpness VOICE", {{"auto_level", "0"}});
    check(radio.filterSharpnessVoice() == 1 && !radio.filterSharpnessVoiceAuto(),
          "partial automatic mode preserves the level");
    deliver("license", {{"radio_id", "abc"}, {"highest_major_version", "4"}});
    deliver("license subscription", {{"name", "smartsdr+_early_access"}, {"expiration", "2099-02-03T12:00:00Z"}});
    deliver("license subscription", {{"name", "smartsdr+"}, {"expiration", "2099-03-04T12:00:00Z"}});
    check(radio.licenseSubscription() == "SmartSDR+" && radio.licenseExpirationDate() == "03/04/2099",
          "latest expiration selects the subscription independently of arrival order");
    deliver("license subscription", {{"name", "smartsdr+_early_access"}, {"expiration", "2000-01-01"}});
    check(radio.licenseSubscription() == "SmartSDR+" && radio.licenseExpirationDate() == "03/04/2099",
          "a lapsed subscription cannot replace the current subscription");
    RadioModel expired;
    installRadioSetupBackend(expired);
    FlexSetupStatusTestPeer::deliver(expired, "license subscription", {{"name", "smartsdr+"}, {"expiration", "2000-01-01"}});
    check(expired.licenseSubscription() == "SmartSDR+ (expired)" && expired.licenseExpirationDate() == "01/01/2000",
          "expired subscriptions retain their diagnostic name and date");
    FlexSetupStatusTestPeer::deliver(expired, "license feature", {{"name", "auto_tune"}, {"enabled", "1"}});
    check(expired.cwAutotuneLicensed(), "authoritative feature grants survive subscription expiration");
    deliver("license feature", {{"name", "digital_voice_keyer"}, {"enabled", "1"}});
    radio.dvkModel().noteRefusal(0x50004001u);
    deliver("license feature", {{"name", "digital_voice_keyer"}, {"enabled", "1"}});
    check(!radio.dvkLicenseRefused(), "repeated authoritative grant clears a later DVK refusal");
    deliver("xvtr 2", {{"name", "2m"}, {"rf_freq", "144"}, {"if_freq", "28"}});
    const auto identity = radio.xvtrList().value(2).lifetime;
    deliver("xvtr 2", {{"rf_freq", "bad"}, {"name", ""}});
    check(radio.xvtrList().value(2).rfFreq == 144 && radio.xvtrList().value(2).name.isEmpty()
        && radio.xvtrList().value(2).lifetime == identity, "transverter partial fields and explicit empty name preserve identity");
    TransverterRequest edit{2, TransverterRequest::Field::RxOnly, true, 0.0, identity};
    deliver("xvtr 2", {{"in_use", "0"}});
    deliver("xvtr 2", {{"name", "new"}});
    check(!identity->current() && radio.requestTransverter(edit) == ReceiveDispatch::Unsupported,
          "old transverter editor cannot change a same-ID replacement");
    applyCableStatus(radio.usbCableModel(), "SN", {{"type", "cat"}, {"name", "My cable"}, {"enable", "1"}});
    const auto cableIdentity = radio.usbCableModel().cables().value("SN").lifetime;
    applyCableStatus(radio.usbCableModel(), "SN", {{"type", "bit"}});
    check(radio.usbCableModel().cables().value("SN").name == "My cable"
        && radio.usbCableModel().cables().value("SN").enabled && !cableIdentity->current(),
          "retype keeps base fields while retiring the former identity");
    check(radio.requestUsbCable({"SN", -1, UsbCableRequest::Field::Enabled, false, cableIdentity}) == ReceiveDispatch::Unsupported,
          "stale USB intent cannot target a retyped cable");
    deliver("xvtr 7", {{"name", "old"}, {"rf_freq", "144"}});
    const auto oldXvtr = radio.xvtrList().value(7).lifetime;
    deliver("xvtr 7", {{"in_use", "0"}});
    deliver("xvtr 7", {{"name", "fresh"}});
    check(radio.xvtrList().value(7).name == "fresh" && radio.xvtrList().value(7).rfFreq == 0
        && oldXvtr && !oldXvtr->current() && radio.xvtrList().value(7).lifetime != oldXvtr,
        "a same-index replacement starts from fresh fields and a fresh identity");
    deliver("usb_cable REUSED", {{"type", "cat"}, {"name", "old cable"}, {"enable", "1"}});
    const auto oldCable = radio.usbCableModel().cables().value("REUSED").lifetime;
    deliver("usb_cable REUSED removed", {});
    deliver("usb_cable REUSED", {{"type", "cat"}});
    check(radio.usbCableModel().cables().value("REUSED").name.isEmpty()
        && !radio.usbCableModel().cables().value("REUSED").enabled
        && oldCable && !oldCable->current() && radio.usbCableModel().cables().value("REUSED").lifetime != oldCable,
        "a same-serial replacement cannot inherit the removed cable's fields or identity");
    FlexSetupStatusTestPeer::reset(radio);
    check(radio.xvtrList().isEmpty() && radio.usbCableModel().cables().isEmpty()
        && radio.licenseRadioId().isEmpty() && radio.licenseMaxVersion().isEmpty()
        && radio.licenseSubscription().isEmpty() && !radio.licenseFeatureSeen("digital_voice_keyer"),
          "reset clears all setup inventory and license presentation");
}

void reentrancy()
{
    UsbCableModel cables;
    applyCableStatus(cables, "SN", {{"type", "cat"}});
    QObject::connect(&cables, &UsbCableModel::cableRemoved, &cables, [&](const QString&) {
        applyCableStatus(cables, "SN", {{"type", "ldpa"}, {"name", "replacement"}});
    });
    applyCableStatus(cables, "SN", {{"type", "bit"}});
    check(cables.cables().value("SN").type == "ldpa" && cables.cables().value("SN").name == "replacement",
          "reentrant cable replacement survives the outer retype notification");
    RadioModel radio;
    QObject::connect(&radio, &RadioModel::licenseFeaturesChanged, &radio, [&] { FlexSetupStatusTestPeer::reset(radio); });
    FlexSetupStatusTestPeer::deliver(radio, "license feature", {{"name", "digital_voice_keyer"}, {"enabled", "1"}});
    check(!radio.licenseFeatureSeen("digital_voice_keyer") && radio.licenseSubscription().isEmpty(),
          "a reset during feature notification prevents the old report from continuing");
}

void finalWriteFence()
{
    RadioModel radio;
    RadioSetupTestBackend* backend = installRadioSetupBackend(radio);
    SessionControlTestAccess::begin(radio, *backend);
    RadioConnection* connection = backend->connection();
    std::atomic_int writes{0};
    SessionControlTestAccess::installWriter(*connection, [&](quint32, const QString&) { ++writes; });
    const auto queueBehind = [&](const std::shared_ptr<SetupStatusLifetime>& identity, bool retire) {
        QSemaphore entered, release;
        QMetaObject::invokeMethod(connection, [&] { entered.release(); release.tryAcquire(1, 5000); });
        check(entered.tryAcquire(1, 5000), "worker barrier entered");
        check(FlexSetupStatusTestPeer::send(radio, identity), "configuration command admitted to the worker queue");
        if (retire) { identity->retire(); }
        release.release();
        QMetaObject::invokeMethod(connection, [] {}, Qt::BlockingQueuedConnection);
    };
    queueBehind(std::make_shared<SetupStatusLifetime>(), true);
    check(writes == 0, "final queued write refuses an entity retired after model admission");
    queueBehind(std::make_shared<SetupStatusLifetime>(), false);
    check(writes == 1, "current configuration intent reaches the injected writer exactly once");
}

// The model applies a setup report only from the session being run, and
// none once that session has begun to end.
void sessionFence()
{
    RadioModel radio;
    RadioSetupTestBackend* backend = installRadioSetupBackend(radio);
    SessionControlTestAccess::begin(radio, *backend);
    const quint64 first = SessionControlTestAccess::statusEpoch(radio);
    const auto report = [](quint64 epoch, const char* name) {
        auto observation = decodeFlexSetupStatus("xvtr 1", {{"name", name}});
        observation->statusEpoch = epoch;
        return *observation;
    };
    emit backend->setupStatusObserved(report(first, "a"));
    check(radio.xvtrList().value(1).name == "a", "a report from the running session applies");
    SessionControlTestAccess::begin(radio, *backend);
    const quint64 second = SessionControlTestAccess::statusEpoch(radio);
    emit backend->setupStatusObserved(report(first, "stale"));
    check(second != first && radio.xvtrList().value(1).name == "a", "a report from an earlier session is refused");
    FlexSetupStatusTestPeer::deliver(radio, "license", {{"radio_id", "abc"}});
    SessionControlTestAccess::beginConnect(radio);   // a new connect, before the old disconnect lands
    FlexSetupStatusTestPeer::deliver(radio, "license", {{"radio_id", "new"}});
    FlexSetupStatusTestPeer::deliver(radio, "xvtr 8", {{"name", "new"}});
    RadioSetupControlTestAccess::disconnected(radio);
    check(radio.licenseRadioId() == "NEW" && radio.xvtrList().value(8).name == "new",
          "a late disconnect keeps the newer session's license and inventory");
    SessionControlTestAccess::connectedNow(radio);
    const quint64 third = SessionControlTestAccess::statusEpoch(radio);
    RadioSetupControlTestAccess::disconnected(radio);
    emit backend->setupStatusObserved(report(third, "late"));
    check(!radio.xvtrList().contains(1), "a session that has begun to end applies nothing more");
}

// A transport error that leaves the link up ends nothing: setup status
// keeps applying through the ingress, on LAN and on SmartLink.
void sessionSurvivesTransportError()
{
    const auto line = [](const char* text) { return CommandParser::parseLine(QString::fromLatin1(text)); };
    {
        RadioModel radio;
        RadioSetupTestBackend* backend = installRadioSetupBackend(radio);
        SessionControlTestAccess::begin(radio, *backend);
        RadioConnection* lan = backend->connection();
        emit lan->lineReceived(line("S1|xvtr 4 name=6m rf_freq=50 if_freq=28"), lan->lineEpoch());
        QCoreApplication::sendPostedEvents();
        check(radio.xvtrList().value(4).name == "6m", "LAN setup status reaches the model through the ingress");
        FlexSetupStatusTestPeer::connectionError(radio);
        emit lan->lineReceived(line("S1|radio filter_sharpness VOICE level=0"), lan->lineEpoch());
        emit lan->lineReceived(line("S1|xvtr 4 name=4m"), lan->lineEpoch());
        QCoreApplication::sendPostedEvents();
        check(radio.filterSharpnessVoice() == 0 && radio.xvtrList().value(4).name == "4m"
            && radio.xvtrList().value(4).lifetime->current(),
              "a LAN error that leaves the link up keeps setup status and entity authority");
    }
    {
        WanConnection wan;
        RadioModel radio;
        installRadioSetupBackend(radio);
        FlexSetupStatusTestPeer::connectWan(radio, &wan);
        emit wan.lineReceived(line("S1|usb_cable SN type=cat name=rig"), wan.lineEpoch());
        QCoreApplication::sendPostedEvents();
        check(radio.usbCableModel().cables().value("SN").name == "rig",
              "SmartLink setup status reaches the model through the ingress");
        QMetaObject::invokeMethod(&wan, "onSocketError", Qt::DirectConnection,
                                  Q_ARG(QAbstractSocket::SocketError, QAbstractSocket::SocketTimeoutError));
        FlexSetupStatusTestPeer::connectionError(radio);
        emit wan.lineReceived(line("S1|usb_cable SN name=rig2"), wan.lineEpoch());
        QCoreApplication::sendPostedEvents();
        check(radio.usbCableModel().cables().value("SN").name == "rig2"
            && radio.usbCableModel().cables().value("SN").lifetime->current(),
              "a SmartLink socket error keeps setup status and entity authority");
        FlexSetupStatusTestPeer::releaseWan(radio);
    }
}
}

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("flex-setup-observation"));
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();
    parserSemantics();
    acknowledgedSharpness();
    acknowledgedRemoval();
    entityLifecycle();
    modelSemantics();
    reentrancy();
    finalWriteFence();
    sessionFence();
    sessionSurvivesTransportError();
    return failures ? 1 : 0;
}
