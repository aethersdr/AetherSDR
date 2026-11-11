#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/backends/kenwood/KenwoodBackend.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QList>
#include <QTimer>

#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>

namespace AetherSDR {
class TxOperationIntegrationTestAccess {
public:
    static unsigned activities(const RadioModel& model) { return model.activeTxActivities(); }
    static void capabilities(RadioModel& model) { model.publishCapabilities(model.isConnected()); }
    static void wireLifecycle(RadioModel& model, IRadioBackend& backend)
    {
        // Same slots used by the production seam for a connection-less backend.
        QObject::connect(&backend, &IRadioBackend::connected, &model, &RadioModel::onConnected);
        QObject::connect(&backend, &IRadioBackend::disconnected, &model, &RadioModel::onDisconnected);
    }
};
namespace kenwood {
struct KenwoodBackendTestAccess {
    static void authenticate(KenwoodBackend& backend)
    {
        backend.m_clock.start();
        backend.m_protocol.start("user", "password", false, 0);
        backend.m_protocol.receive("##CN1;", 1);
        backend.m_protocol.receive("##ID1;", 2);
        backend.ready();
    }
    static void message(KenwoodBackend& backend, const QByteArray& message, quint64 batch = 0)
    { backend.adopt(message, batch); }
    static void startTimer(KenwoodBackend& backend) { backend.m_timer.start(); }
    static quint64 reserveBatch(KenwoodBackend& backend) { return ++backend.m_controlIngressSequence; }
    static TxCoordinator::Operation cw(const KenwoodBackend& backend) { return backend.m_cwOperation; }
    static TxCoordinator::Operation atu(const KenwoodBackend& backend) { return backend.m_atuOperation; }
    static bool hasCw(const KenwoodBackend& backend) { return backend.m_cwIntent; }
    static bool hasAtu(const KenwoodBackend& backend) { return backend.m_atuIntent; }
};
}
}
using namespace AetherSDR;
namespace {
using Access = kenwood::KenwoodBackendTestAccess;
void require(bool condition, const char* message)
{
    if (!condition) { std::cerr << message << '\n'; std::exit(1); }
}
void tick()
{
    QEventLoop events;
    QTimer::singleShot(250, &events, &QEventLoop::quit);
    events.exec();
}
struct Fixture {
    QList<QByteArray> commands;
    std::function<void(const QByteArray&)> onCommand;
    RadioModel radio;
    kenwood::KenwoodBackend* backend;
    int receiveObservations = 0;
    Fixture()
    {
        auto owned = std::make_unique<kenwood::KenwoodBackend>(kenwood::KenwoodBackend::Transport{
            [this](const QByteArray& bytes) {
                commands.append(bytes);
                const std::function<void(const QByteArray&)> observer = onCommand;
                if (observer) { observer(bytes); }
                return true;
            }, [](const QByteArray&) { return true; }});
        backend = owned.get();
        radio.setBackendForTest(std::move(owned), QStringLiteral("kenwood"));
        require(radio.automationApplySliceFixture(0, QStringLiteral("A")), "isolated slice fixture");
        SliceDelta slice;
        slice.txSlice = true;
        slice.mode = QStringLiteral("CW");
        slice.panId = QStringLiteral("0");
        radio.slice(0)->applyChanges(slice);
        radio.transmitModel().setTxModeGetter([] { return QStringLiteral("CW"); });
        QObject::connect(backend, &IRadioBackend::capabilitiesChanged, &radio,
            [this] { TxOperationIntegrationTestAccess::capabilities(radio); });
        QObject::connect(backend, &IRadioBackend::transmitChanged, &radio,
            [this](const TransmitDelta& delta) { radio.applyBackendTransmitDeltaForTest(delta); });
        QObject::connect(backend, &IRadioBackend::keyingStateConfirmed, &radio, &RadioModel::radioTransmitConfirmed);
        QObject::connect(backend, &IRadioBackend::keyingStateConfirmed, &radio,
            [this](bool transmitting) { if (!transmitting) { ++receiveObservations; } });
        Access::authenticate(*backend);
        for (const QByteArray& message : {QByteArray("ID024"), QByteArray("##TI1"), QByteArray("##UE1"),
             QByteArray("FR0"), QByteArray("FT0"), QByteArray("SF0000140740003"), QByteArray("AN1000"),
             QByteArray("XV0"), QByteArray("XT0"), QByteArray("RF00000"), QByteArray("LP20"),
             QByteArray("LP1505010"), QByteArray("RX")}) { feed(message); }
        TxOperationIntegrationTestAccess::capabilities(radio);
        Access::startTimer(*backend);
        commands.clear();
        receiveObservations = 0;
    }
    void feed(const QByteArray& message, quint64 batch = 0) { Access::message(*backend, message, batch); }
    bool active(TxCoordinator::Activity activity) const
    { return (TxOperationIntegrationTestAccess::activities(radio) & static_cast<unsigned>(activity)) != 0; }
};
}
int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("kenwood-generator-intent"));
    require(profile.isValid(), "isolated settings directory");
    QCoreApplication application(argc, argv);
    AppSettings::instance().load();
    for (const QByteArray& power : {QByteArray("PS0"), QByteArray("PS2"), QByteArray("PS3")}) {
        Fixture f;
        TxOperationIntegrationTestAccess::wireLifecycle(f.radio, *f.backend);
        const TxCoordinator::Producer producer = f.radio.registerTxProducer();
        const TxCoordinator::Request request = producer.request();
        require(f.radio.requestProducerCwx(request, QStringLiteral("CQ")), "power fixture admits a production native CW request");
        const TxCoordinator::Operation operation = f.radio.transmitOperation();
        const TxCoordinator::Operation native = Access::cw(*f.backend);
        const TxCoordinator::Context media = f.radio.captureTxMedia(request);
        require(request.valid() && operation.permitsDispatch(TxCoordinator::monotonicMs())
            && native.permitsDispatch(TxCoordinator::monotonicMs()) && media.permitsDispatch(TxCoordinator::monotonicMs()),
            "power fixture captures original request, operation and media authority");
        f.commands.clear();
        f.feed(power);
        require(!request.valid() && !operation.permitsDispatch(TxCoordinator::monotonicMs())
            && !native.permitsDispatch(TxCoordinator::monotonicMs()) && !media.permitsDispatch(TxCoordinator::monotonicMs())
            && !f.active(TxCoordinator::Activity::Cwx) && f.receiveObservations == 0 && f.commands.isEmpty(),
            "real model disconnect route retires captured producer authority without RF cleanup or idle evidence");
        Access::authenticate(*f.backend);
        for (const QByteArray& status : {QByteArray("PS1"), QByteArray("ID024"), QByteArray("##TI1"), QByteArray("##UE1"),
             QByteArray("FR0"), QByteArray("FT0"), QByteArray("SF0000140740003"), QByteArray("RX")}) { f.feed(status); }
        f.commands.clear();
        require(!f.radio.requestProducerCwx(request, QStringLiteral("OLD")), "old producer request cannot bind to a powered replacement session");
        f.backend->sendCwText(QStringLiteral("OLD"), native);
        f.backend->submitTxAudio(QByteArray(480 * 4, '\0'), 24000, TxAudioSource::Microphone, media);
        Access::startTimer(*f.backend);
        tick();
        require(!request.valid() && !operation.permitsDispatch(TxCoordinator::monotonicMs())
            && !media.permitsDispatch(TxCoordinator::monotonicMs()) && !Access::hasCw(*f.backend)
            && !f.active(TxCoordinator::Activity::Cwx), "reconnect does not restore a pre-power producer or local native generator");
        for (const QByteArray& command : f.commands) {
            require(!command.startsWith("KY") && !command.startsWith("TX") && !command.startsWith("AC1")
                && command != "RX;" && command != "PS1;", "powered reconnect emits no old keying or automatic power command");
        }
    }
    {
        std::unique_ptr<Fixture> mortal = std::make_unique<Fixture>();
        mortal->radio.transmitModel().atuStart();
        QObject::connect(mortal->backend, &IRadioBackend::extensionStatus, &application,
            [&](const QString& ns, const QString& type, const QVariantMap&) {
                if (mortal && ns == QLatin1String("kenwood") && type == QLatin1String("power.limits")) {
                    mortal.reset();
                }
            });
        mortal->radio.transmitModel().atuBypass();
        require(!mortal, "synchronous ATU stop observer may destroy the model without post-callback access");
    }
    {
        Fixture f;
        require(f.backend->capabilities().hasCwTune, "idle fixture qualifies native TUNE before CW starts");
        f.radio.cwxModel().send(QStringLiteral("TEST TEST TEST TEST"));
        const TxCoordinator::Operation original = Access::cw(*f.backend);
        const CwxModel::TransmissionPermit originalQueue = f.radio.cwxModel().queuedTransmissionPermit();
        require(f.commands.count("KY2TEST TEST TEST TEST;") == 1 && f.active(TxCoordinator::Activity::Cwx)
            && original.hasLocalIntentRetirement(), "production CW handoff retains its exact backend-owned intent");
        tick();
        f.feed("RX");
        require(Access::hasCw(*f.backend) && f.active(TxCoordinator::Activity::Cwx) && original.permitsDispatch(TxCoordinator::monotonicMs())
            && !f.commands.contains("KY0;") && !f.backend->capabilities().hasCwTune,
            "ordinary handoff and QSK receive cannot drain native CW or expose competing TUNE");
        f.radio.cwxModel().send(QStringLiteral("CQ"));
        tick();
        require(f.commands.count("KY2CQ;") == 1 && !f.commands.contains("KY0;") && originalQueue()
            && original.sameLocalIntent(Access::cw(*f.backend)), "same-input append preserves queue cancellation identity and native ownership");
        f.feed("TX0");
        const int receives = f.receiveObservations;
        f.radio.cwxModel().clearBuffer();
        require(f.commands.count("KY0;") == 1 && !Access::hasCw(*f.backend) && !f.active(TxCoordinator::Activity::Cwx)
            && !originalQueue() && f.radio.isRadioTransmitting() && f.receiveObservations == receives,
            "explicit cancel retires exact CW ownership without inventing radio idle");
        f.radio.cwxModel().send(QStringLiteral("NEW"));
        const TxCoordinator::Operation replacement = Access::cw(*f.backend);
        require(replacement.hasLocalIntentRetirement() && !replacement.sameLocalIntent(original), "a new CW input has fresh retirement identity");
        const int stops = f.commands.count("KY0;");
        f.backend->abortCwText(original);
        require(f.commands.count("KY0;") == stops && replacement.sameLocalIntent(Access::cw(*f.backend))
            && f.active(TxCoordinator::Activity::Cwx), "old CW cleanup cannot stop its replacement input");
        f.radio.cwxModel().clearBuffer();
    }
    {
        Fixture f;
        const TxCoordinator::Producer producer = f.radio.registerTxProducer();
        const TxCoordinator::Request input = producer.request();
        require(f.radio.requestProducerCwx(input, QStringLiteral("CQ")), "producer CW request admitted");
        producer.discardInputs();
        tick();
        require(f.commands.count("KY0;") == 1 && !Access::hasCw(*f.backend) && !f.active(TxCoordinator::Activity::Cwx)
            && f.receiveObservations == 0, "producer cancellation stops native CW without a false RF-idle observation");
    }
    {
        Fixture f;
        const TxCoordinator::Producer producer = f.radio.registerTxProducer();
        const TxCoordinator::Request input = producer.request();
        require(f.radio.requestProducerAtu(input, true), "producer ATU request admitted");
        producer.discardInputs();
        tick();
        require(f.commands.count("AC110;") == 1 && !Access::hasAtu(*f.backend) && !f.active(TxCoordinator::Activity::Atu)
            && f.receiveObservations == 0, "producer cancellation preserves original ATU cleanup authority through watchdog retirement");
    }
    {
        Fixture f;
        f.radio.cwxModel().send(QStringLiteral("CQ"));
        f.feed("##TI0");
        require(f.commands.count("KY0;") == 1 && !Access::hasCw(*f.backend) && !f.active(TxCoordinator::Activity::Cwx),
            "account permission loss retires the original CW input");
    }
    {
        Fixture f;
        f.onCommand = [&](const QByteArray& command) {
            if (command.startsWith("KY2")) { f.radio.cwxModel().clearBuffer(); }
        };
        f.radio.cwxModel().send(QStringLiteral("CQ"));
        require(f.commands.count("KY2CQ;") == 1 && f.commands.count("KY0;") == 1
            && !Access::hasCw(*f.backend) && !f.active(TxCoordinator::Activity::Cwx),
            "reentrant cancellation during native CW handoff cannot resurrect its owner");
    }
    {
        Fixture f;
        f.radio.transmitModel().atuStart();
        const TxCoordinator::Operation original = Access::atu(*f.backend);
        require(f.active(TxCoordinator::Activity::Atu) && Access::hasAtu(*f.backend)
            && original.hasLocalIntentRetirement(), "ATU start retains its original backend-owned contribution");
        f.feed("AC110");
        require(f.active(TxCoordinator::Activity::Atu), "an idle AC reply alone does not establish a new cycle's completion");
        f.feed("AC111");
        f.radio.transmitModel().atuStart();
        require(f.commands.count("AC111;") == 1 && original.sameLocalIntent(Access::atu(*f.backend)),
            "repeated ATU on neither restarts the cycle nor replaces its owner");
        f.feed("TX0");
        f.feed("AC110");
        require(!Access::hasAtu(*f.backend) && !f.active(TxCoordinator::Activity::Atu)
            && f.radio.transmitModel().atuStatus() == ATUStatus::None && f.radio.isRadioTransmitting()
            && f.receiveObservations == 0, "native ATU stopped retires its local intent without claiming success or radio idle");
        require(!f.radio.transmitModel().supportsSetting(TransmitControlRequest::Field::TunePower),
            "ATU completion cannot bypass physical transmit readback when enabling power edits");
        f.feed("RX");
        require(f.radio.transmitModel().supportsSetting(TransmitControlRequest::Field::TunePower),
            "actual RX after ATU completion restores idle native power editing");
        f.radio.transmitModel().atuStart();
        const TxCoordinator::Operation replacement = Access::atu(*f.backend);
        const int stops = f.commands.count("AC110;");
        f.backend->setAtu(false, original);
        require(f.commands.count("AC110;") == stops && replacement.sameLocalIntent(Access::atu(*f.backend))
            && f.active(TxCoordinator::Activity::Atu), "old ATU cleanup cannot retire a replacement cycle");
        f.radio.transmitModel().atuBypass();
        require(!f.active(TxCoordinator::Activity::Atu) && !Access::hasAtu(*f.backend), "explicit ATU stop retires its matching cycle");
    }
    {
        Fixture f;
        const quint64 oldBatch = Access::reserveBatch(*f.backend);
        f.radio.transmitModel().atuStart();
        f.feed("AC111", oldBatch);
        f.feed("AC110", oldBatch);
        f.feed("AC110");
        require(f.active(TxCoordinator::Activity::Atu) && Access::hasAtu(*f.backend),
            "pre-request coalesced progress and stop cannot complete the current ATU input");
        f.feed("AC111");
        f.feed("AC110");
        require(!f.active(TxCoordinator::Activity::Atu) && !Access::hasAtu(*f.backend),
            "fresh native progress then stopped retires the matching cycle");
    }
    {
        Fixture f;
        f.onCommand = [&](const QByteArray& command) {
            if (command == "AC111;") { f.radio.transmitModel().atuBypass(); }
        };
        f.radio.transmitModel().atuStart();
        require(f.commands.count("AC111;") == 1 && f.commands.count("AC110;") == 1
            && !Access::hasAtu(*f.backend) && !f.active(TxCoordinator::Activity::Atu),
            "reentrant stop during ATU handoff cannot resurrect its original cycle");
    }
    for (const bool atu : {false, true}) {
        Fixture f;
        TxCoordinator source([](const TxCoordinator::Operation&, TxCoordinator::StopReason) {});
        const TxCoordinator::Actor actor = source.registerActor({true, 0});
        const TxCoordinator::Operation root = source.acquire(actor, TxCoordinator::monotonicMs()).operation;
        const TxCoordinator::Intent intent = source.beginIntent(root, {}, atu ? TxCoordinator::Activity::Atu : TxCoordinator::Activity::Cwx);
        int retired = 0;
        const TxCoordinator::Operation input = source.bindLocalIntentRetirement(root, intent,
            [&](TxCoordinator::LocalIntentEnd) { ++retired; });
        if (atu) { f.backend->setAtu(true, input); }
        else { require(f.backend->sendCwText(QStringLiteral("CQ"), input).isEmpty(), "direct admitted native CW input"); }
        source.reset();
        const int stops = f.commands.count(atu ? "AC110;" : "KY0;");
        tick();
        require(retired == 1 && !(atu ? Access::hasAtu(*f.backend) : Access::hasCw(*f.backend))
            && f.commands.count(atu ? "AC110;" : "KY0;") == stops && f.receiveObservations == 0,
            "expired original cleanup still retires local generator ownership without writing or claiming RF idle");
    }
    std::cout << "Kenwood native generator lifetime checks passed\n";
}
