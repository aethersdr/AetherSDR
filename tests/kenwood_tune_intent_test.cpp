#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/backends/kenwood/KenwoodBackend.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QCoreApplication>
#include <QEvent>
#include <QList>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <thread>

namespace AetherSDR {
class TxOperationIntegrationTestAccess {
public:
    static unsigned activities(const RadioModel& model) { return model.activeTxActivities(); }
    static void capabilities(RadioModel& model) { model.publishCapabilities(model.isConnected()); }
    static void newSession(RadioModel& model) { ++model.m_sessionGeneration; }
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
    static void message(KenwoodBackend& backend, const QByteArray& message) { backend.adopt(message); }
    static void timeout(KenwoodBackend& backend) { backend.pollTunePreflight(backend.m_tune->deadlineMs); }
    static TxCoordinator::Operation operation(const KenwoodBackend& backend) { return backend.m_tune->operation; }
};
}
}
using namespace AetherSDR;
namespace {
void require(bool condition, const char* message)
{
    if (!condition) { std::cerr << message << '\n'; std::exit(1); }
}
struct Fixture {
    QList<QByteArray> commands;
    QList<TxCoordinator::Request> requests;
    QStringList notices;
    RadioModel radio;
    kenwood::KenwoodBackend* backend;
    int idleObservations = 0;
    Fixture()
    {
        QObject::connect(&radio, &RadioModel::interlockNotificationRequested, &radio,
            [this](const QString& text) { notices.append(text); });
        auto owned = std::make_unique<kenwood::KenwoodBackend>(kenwood::KenwoodBackend::Transport{
            [this](const QByteArray& bytes) { commands.append(bytes); return true; }, [](const QByteArray&) { return true; }});
        backend = owned.get();
        radio.setBackendForTest(std::move(owned), QStringLiteral("kenwood"));
        require(radio.automationApplySliceFixture(0, QStringLiteral("A")), "disconnected slice fixture");
        SliceDelta slice;
        slice.txSlice = true;
        slice.mode = QStringLiteral("USB");
        slice.panId = QStringLiteral("0");
        radio.slice(0)->applyChanges(slice);
        radio.transmitModel().setTxModeGetter([] { return QStringLiteral("USB"); });
        QObject::connect(backend, &IRadioBackend::capabilitiesChanged, &radio,
            [this] { TxOperationIntegrationTestAccess::capabilities(radio); });
        QObject::connect(backend, &IRadioBackend::transmitChanged, &radio,
            [this](const TransmitDelta& delta) { radio.applyBackendTransmitDeltaForTest(delta); });
        QObject::connect(backend, &IRadioBackend::keyingStateConfirmed, &radio, &RadioModel::radioTransmitConfirmed);
        QObject::connect(backend, &IRadioBackend::keyingStateConfirmed, &radio,
            [this](bool keyed) { if (!keyed) { ++idleObservations; } });
        kenwood::KenwoodBackendTestAccess::authenticate(*backend);
        for (const QByteArray& message : {QByteArray("ID024"), QByteArray("##TI1"), QByteArray("##UE1"),
             QByteArray("FR0"), QByteArray("FT0"), QByteArray("SF0000140740002"), QByteArray("AN1000"),
             QByteArray("XV0"), QByteArray("XT0"), QByteArray("RF00000"), QByteArray("LP20"),
             QByteArray("LP1505010"), QByteArray("RX")}) { feed(message); }
        TxOperationIntegrationTestAccess::capabilities(radio);
        require(radio.transmitModel().tuneAvailable(), "native source/RX/power enables model TUNE");
        commands.clear();
        idleObservations = 0;
    }
    void feed(const QByteArray& message) { kenwood::KenwoodBackendTestAccess::message(*backend, message); }
    unsigned activities() const { return TxOperationIntegrationTestAccess::activities(radio); }
    bool tuneIntent() const { return (activities() & static_cast<unsigned>(TxCoordinator::Activity::Tune)) != 0; }
    TxCoordinator::Request start()
    {
        const TxCoordinator::Producer producer = radio.registerTxProducer();
        const TxCoordinator::Request input = producer.request();
        requests.append(input);
        const bool accepted = radio.requestProducerTune(input, true);
        if (!accepted || !tuneIntent() || !radio.transmitModel().isTuning()) {
            std::cerr << "start=" << requests.size() << " accepted=" << accepted << " activities=" << activities()
                << " available=" << radio.transmitModel().tuneAvailable() << " tuning=" << radio.transmitModel().isTuning()
                << " notices=" << notices.join(" | ").toStdString() << '\n';
        }
        require(accepted && tuneIntent() && radio.transmitModel().isTuning(), "production model admits native TUNE intent");
        return input;
    }
    void active()
    {
        fresh();
        feed("LP1505010");
        require(commands.count("TX2;") == 1 && tuneIntent(), "native handoff preserves local intent until retirement");
        feed("TX2");
    }
    void fresh()
    {
        for (const QByteArray& message : {QByteArray("FT0"), QByteArray("SF0000140740002"), QByteArray("AN1000"),
             QByteArray("XV0"), QByteArray("XT0"), QByteArray("RF00000"), QByteArray("LP20")}) { feed(message); }
    }
};
}
int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("kenwood-tune-intent"));
    require(profile.isValid(), "isolated settings");
    QCoreApplication application(argc, argv);
    AppSettings::instance().load();
    {
        Fixture f;
        f.start();
        kenwood::KenwoodBackendTestAccess::timeout(*f.backend);
        require(!f.tuneIntent() && !f.radio.transmitModel().isTuning() && !f.commands.contains("TX2;") && f.idleObservations == 0,
            "native preflight timeout retires the original model contribution and pending latch");
        f.start();
        require(f.tuneIntent(), "a fresh producer can reengage after a failed preflight");
    }
    {
        Fixture f;
        f.start();
        f.active();
        require(f.radio.isRadioTransmitting(), "only native TX2 establishes observed transmit in model");
        f.feed("FA00014200000");
        require(!f.tuneIntent() && f.radio.isRadioTransmitting() && f.radio.transmitModel().isTuning()
            && f.commands.last() == "RX;" && f.idleObservations == 0, "active cancellation ends local intent while preserving unconfirmed physical transmit");
        f.feed("RX");
        require(!f.radio.isRadioTransmitting() && !f.radio.transmitModel().isTuning(), "native RX reconciles physical state after intent retirement");
    }
    {
        Fixture f;
        const TxCoordinator::Request input = f.start();
        const TxCoordinator::Operation first = kenwood::KenwoodBackendTestAccess::operation(*f.backend);
        require(f.radio.requestProducerTune(input, true), "repeated on keeps the original contribution");
        require(first.sameAuthority(kenwood::KenwoodBackendTestAccess::operation(*f.backend)),
            "repeated on cannot replace the captured transmit authority");
        kenwood::KenwoodBackendTestAccess::timeout(*f.backend);
        require(!f.tuneIntent() && !f.radio.transmitModel().isTuning(),
            "repeated on retires its latest pending UI epoch without renewing keying");
    }
    {
        Fixture f;
        const TxCoordinator::Request input = f.start();
        f.active();
        require(f.radio.requestProducerTune(input, true) && f.commands.count("TX2;") == 1
            && f.tuneIntent() && f.radio.transmitModel().isTuning(), "active repeated on neither rekeys nor unkeys its original carrier");
        f.feed("FA00014200000");
        require(!f.tuneIntent() && f.radio.isRadioTransmitting() && f.radio.transmitModel().isTuning(),
            "active repeated on still retires only local ownership until radio RX arrives");
    }
    {
        Fixture f;
        const TxCoordinator::Request input = f.start();
        const TxCoordinator::Operation original = kenwood::KenwoodBackendTestAccess::operation(*f.backend);
        f.fresh();
        f.feed("LP1505020");
        require(f.commands.contains("LP1505010;") && f.radio.requestProducerTune(input, true),
            "repeated on remains idempotent while the original power request awaits readback");
        require(original.sameAuthority(kenwood::KenwoodBackendTestAccess::operation(*f.backend)),
            "repeated on cannot adopt newly reported power as new keying authority");
        f.feed("LP1505010");
        require(f.commands.count("TX2;") == 1 && f.tuneIntent(), "original power request completes exactly one carrier handoff");
    }
    {
        Fixture f;
        f.start();
        const TxCoordinator::Operation old = kenwood::KenwoodBackendTestAccess::operation(*f.backend);
        std::thread retirement([old] { old.retireLocalIntent(TxCoordinator::LocalIntentEnd::BeforeKeying); });
        retirement.join();
        f.radio.transmitModel().stopTune();
        f.start();
        const TxCoordinator::Operation fresh = kenwood::KenwoodBackendTestAccess::operation(*f.backend);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        require(f.tuneIntent() && f.radio.transmitModel().isTuning() && fresh.permitsDispatch(TxCoordinator::monotonicMs())
            && !old.sameLocalIntent(fresh), "queued old receipt cannot clear a reengaged intent on the same operation");
        f.backend->setTune(false, 10, old);
        require(kenwood::KenwoodBackendTestAccess::operation(*f.backend).sameLocalIntent(fresh),
            "old producer cleanup cannot stop a new native contribution sharing root authority");
    }
    {
        Fixture f;
        f.start();
        const TxCoordinator::Operation old = kenwood::KenwoodBackendTestAccess::operation(*f.backend);
        std::thread retirement([old] { old.retireLocalIntent(TxCoordinator::LocalIntentEnd::BeforeKeying); });
        retirement.join();
        TxOperationIntegrationTestAccess::newSession(f.radio);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        require(f.tuneIntent() && f.radio.transmitModel().isTuning(), "queued receipt from an obsolete session is ignored");
    }
    {
        Fixture f;
        f.start();
        bool reengaged = false;
        QObject::connect(&f.radio.transmitModel(), &TransmitModel::stateChanged, &application, [&] {
            if (!reengaged && !f.radio.transmitModel().isTuning()) {
                reengaged = true;
                f.start();
            }
        });
        kenwood::KenwoodBackendTestAccess::timeout(*f.backend);
        require(reengaged && f.tuneIntent() && f.radio.transmitModel().isTuning(),
            "reengagement from pending-latch reconciliation survives the older receipt");
    }
    {
        auto f = std::make_unique<Fixture>();
        f->start();
        QObject::connect(&f->radio.transmitModel(), &TransmitModel::stateChanged, &application, [&] {
            if (f && !f->radio.transmitModel().isTuning()) { f.reset(); }
        });
        kenwood::KenwoodBackendTestAccess::timeout(*f->backend);
        require(!f, "model/backend destruction during pending-latch retirement stops further work");
    }
    return 0;
}
