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
    const auto lifetime = std::make_shared<SetupStatusLifetime>();
    const auto decode = [&](const QString& object, const Fields& fields) {
        return decodeFlexSetupStatus(object, fields, lifetime);
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
    const auto feature = decode("license feature", {{"name", " Future_Feature "}, {"enabled", "1"}});
    check(feature && feature->license.name == "future_feature" && feature->license.reason == "unknown",
          "valid unknown license features remain diagnostic observations");
    check(!decode("license feature", {{"name", "x"}, {"enabled", "2"}}), "license grants require strict zero or one");
    check(!decode("license subscription", {{"name", "smartsdr+"}, {"expiration", "2026-11-01garbage"}}),
          "license dates do not accept trailing junk");
    lifetime->retire();
    check(!decode("xvtr 1", {{"name", "2m"}}), "retired source cannot decode into a new session");
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
    request(3);
    const RadioSetupCompletion retired = backend->replies.back();
    RadioSetupControlTestAccess::disconnected(radio);
    retired({RadioSetupReply::Kind::Accepted, 0, {}});
    check(radio.filterSharpnessVoice() == 2 && !radio.filterSharpnessVoiceAuto(),
          "retired session cannot restore acknowledged sharpness values");
}

void sourceLifecycle()
{
    FlexSetupStatusSource source;
    const auto lifetime = source.lifetime();
    check(!source.decode("xvtr 22", {{"in_use", "0"}}, lifetime)
        && !source.decode("usb_cable UNKNOWN removed", {}, lifetime),
        "unknown removals cannot acquire an unbound identity");
    const auto first = source.decode("usb_cable SN", {{"type", "cat"}, {"name", "base"}, {"enable", "1"}}, lifetime);
    const auto partial = source.decode("usb_cable SN", {{"name", "new"}}, lifetime);
    check(first && partial && first->entityLifetime == partial->entityLifetime,
          "ordinary partial updates preserve the cable incarnation");
    check(!source.decode("usb_cable UNKNOWN bit 0", {{"enable", "1"}}, lifetime),
          "unknown per-bit reports cannot create phantom cables");
    const auto retyped = source.decode("usb_cable SN", {{"type", "bit"}}, lifetime);
    check(retyped && !first->entityLifetime->current() && retyped->entityLifetime->current()
        && first->entityLifetime != retyped->entityLifetime,
        "retyping retires queued writes before owner-thread delivery");
    check(retyped && retyped->usbCable.name == QStringLiteral("new") && retyped->usbCable.enabled == true,
          "a queued partial retype preserves base fields from its retired predecessor");
    const auto bcd = source.decode("usb_cable BCD", {{"type", "bcd"}}, lifetime);
    const auto vbcd = source.decode("usb_cable BCD", {{"type", "vbcd"}}, lifetime);
    check(bcd && vbcd && bcd->entityLifetime == vbcd->entityLifetime, "BCD subtypes preserve incarnation");
    const auto removal = source.decode("usb_cable SN removed", {}, lifetime);
    const auto replacement = source.decode("usb_cable SN", {{"type", "bit"}}, lifetime);
    check(removal && replacement && !retyped->entityLifetime->current()
        && replacement->entityLifetime != retyped->entityLifetime,
        "same-serial remove/recreate invalidates the old editor and final write");
    source.begin();
    check(!lifetime->current() && !replacement->entityLifetime->current()
        && !source.decode("xvtr 1", {{"name", "stale"}}, lifetime), "source rotation retires every old entity");
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
    check(radio.licenseSubscription() == "SmartSDR+ Early Access", "subscription precedence is independent of arrival order");
    FlexSetupStatusTestPeer::refreshLicense(radio, QDateTime::fromString("2099-02-03T12:00:00Z", Qt::ISODate));
    check(radio.licenseSubscription() == "SmartSDR+" && radio.licenseExpirationDate() == "03/04/2099",
          "expiry at its exact instant falls back to the remaining active subscription");
    FlexSetupStatusTestPeer::refreshLicense(radio, QDateTime::fromString("2099-04-01T00:00:00Z", Qt::ISODate));
    check(radio.licenseSubscription() == "SmartSDR" && radio.licenseExpirationDate().isEmpty(),
          "expiry clears stale subscription text without inventing feature permissions");
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
    FlexSetupStatusSource source;
    const auto old = source.decode("xvtr 7", {{"name", "old"}, {"rf_freq", "144"}}, source.lifetime());
    FlexSetupStatusTestPeer::apply(radio, *old);
    const auto removed = source.decode("xvtr 7", {{"in_use", "0"}}, source.lifetime());
    const auto fresh = source.decode("xvtr 7", {{"name", "fresh"}}, source.lifetime());
    FlexSetupStatusTestPeer::apply(radio, *fresh);
    FlexSetupStatusTestPeer::apply(radio, *removed);
    check(radio.xvtrList().value(7).name == "fresh" && radio.xvtrList().value(7).rfFreq == 0
        && radio.xvtrList().value(7).lifetime == fresh->entityLifetime,
        "replacement resets old fields and identity even if its removal arrives late");
    const auto usbOld = source.decode("usb_cable REUSED", {{"type", "cat"}, {"name", "old cable"}, {"enable", "1"}}, source.lifetime());
    FlexSetupStatusTestPeer::apply(radio, *usbOld);
    const auto usbRemoved = source.decode("usb_cable REUSED removed", {}, source.lifetime());
    const auto usbNew = source.decode("usb_cable REUSED", {{"type", "cat"}}, source.lifetime());
    FlexSetupStatusTestPeer::apply(radio, *usbNew);
    FlexSetupStatusTestPeer::apply(radio, *usbRemoved);
    check(radio.usbCableModel().cables().value("REUSED").name.isEmpty()
        && !radio.usbCableModel().cables().value("REUSED").enabled
        && radio.usbCableModel().cables().value("REUSED").lifetime == usbNew->entityLifetime,
        "same-serial replacement cannot inherit removed-cable fields or a late removal");
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
    installRadioSetupBackend(radio);
    QThread worker;
    auto connection = std::make_unique<RadioConnection>();
    std::atomic_int writes{0};
    FlexSetupStatusTestPeer::writer(*connection, [&](quint32, const QString&) { ++writes; });
    connection->moveToThread(&worker);
    worker.start();
    FlexSetupStatusTestPeer::connection(radio, connection.get());
    const auto queueBehind = [&](const std::shared_ptr<SetupStatusLifetime>& identity, bool retire) {
        QSemaphore entered, release;
        QMetaObject::invokeMethod(connection.get(), [&] { entered.release(); release.tryAcquire(1, 5000); });
        check(entered.tryAcquire(1, 5000), "worker barrier entered");
        check(FlexSetupStatusTestPeer::send(radio, identity), "configuration command admitted to the worker queue");
        if (retire) { identity->retire(); }
        release.release();
        QMetaObject::invokeMethod(connection.get(), [] {}, Qt::BlockingQueuedConnection);
    };
    queueBehind(std::make_shared<SetupStatusLifetime>(), true);
    check(writes == 0, "final queued write refuses an entity retired after model admission");
    queueBehind(std::make_shared<SetupStatusLifetime>(), false);
    check(writes == 1, "current configuration intent reaches the injected writer exactly once");
    FlexSetupStatusTestPeer::connection(radio, static_cast<RadioConnection*>(nullptr));
    QMetaObject::invokeMethod(connection.get(), [&] { connection->moveToThread(QCoreApplication::instance()->thread()); },
                              Qt::BlockingQueuedConnection);
    worker.quit();
    worker.wait();
}

template<typename Transport> void transportOrigin(const char* label)
{
    Transport transport;
    int reports = 0;
    QObject::connect(&transport, &Transport::setupStatusObserved, &transport,
                     [&](const SetupStatusObservation&) { ++reports; });
    FlexSetupStatusTestPeer::feed(transport, "S1|radio filter_sharpness VOICE level=3");
    check(reports == 1, label);
    QObject::connect(&transport, &Transport::messageReceived, &transport,
                     [&](const ParsedMessage&) { FlexSetupStatusTestPeer::begin(transport); });
    FlexSetupStatusTestPeer::feed(transport, "S1|radio filter_sharpness VOICE level=1");
    check(reports == 1, "a callback that rotates the transport cannot restamp the old report");
}
}

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("flex-setup-observation"));
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();
    parserSemantics();
    acknowledgedSharpness();
    sourceLifecycle();
    modelSemantics();
    reentrancy();
    finalWriteFence();
    transportOrigin<RadioConnection>("LAN emits exactly one typed observation");
    transportOrigin<WanConnection>("WAN emits exactly one typed observation");
    return failures ? 1 : 0;
}
