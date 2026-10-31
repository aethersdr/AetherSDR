#include "TestSettingsProfile.h"
#include "PanControlTestFixture.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QLocale>
#include <QTimer>
#include <algorithm>
#include <cstdio>
#include <limits>

using namespace AetherSDR;
namespace {
int failures = 0;
void check(bool condition, const char* message)
{
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
    failures += !condition;
}
void events(int milliseconds)
{
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
}

void encoding()
{
    RecordingPanBackend backend;
    using Field = PanDisplayField;
    struct Case { PanDisplayRequest request; const char* expected; };
    const Case cases[]{
        {{Field::Geometry, PanGeometryRequest{14.123456, 0.250001}}, "display pan set 0x40000001 center=14.123456 bandwidth=0.250001"},
        {{Field::Band, QStringLiteral("20m")}, "display pan set 0x40000001 band=20m"},
        {{Field::LevelRange, PanLevelRange{-130.25, -40.5}}, "display pan set 0x40000001 min_dbm=-130.25 max_dbm=-40.50"},
        {{Field::Pixels, PanPixelDimensions{1234, 567}}, "display pan set 0x40000001 xpixels=1234 ypixels=567"},
        {{Field::Average, 73}, "display pan set 0x40000001 average=73"},
        {{Field::FrameRate, 30}, "display pan set 0x40000001 fps=30"},
        {{Field::WeightedAverage, true}, "display pan set 0x40000001 weighted_average=1"},
        {{Field::RfGain, -8}, "display pan set 0x40000001 rfgain=-8"},
        {{Field::Wnb, true}, "display pan set 0x40000001 wnb=1"},
        {{Field::WnbLevel, 51}, "display pan set 0x40000001 wnb_level=51"},
        {{Field::LoopA, true}, "display pan set 0x40000001 loopa=1"},
        {{Field::LoopB, false}, "display pan set 0x40000001 loopb=0"},
        {{Field::NoiseFloorPosition, 75}, "display pan set 0x40000001 pan_position=75"},
        {{Field::NoiseFloorEnabled, true}, "display pan set 0x40000001 pan_position_enable=1"},
        {{Field::WaterfallRate, 80}, "display panafall set 0x42000001 line_duration=80"},
        {{Field::WaterfallColorGain, 50}, "display panafall set 0x42000001 color_gain=50"},
        {{Field::WaterfallBlackLevel, 15}, "display panafall set 0x42000001 black_level=15"},
        {{Field::WaterfallAutoBlack, false}, "display panafall set 0x42000001 auto_black=0"},
        {{Field::WaterfallDefaults, true}, "display panafall set 0x42000001 auto_black=0 black_level=15 color_gain=50"}
    };
    QLocale::setDefault(QLocale(QLocale::German));
    for (const Case& entry : cases) {
        backend.writes.clear();
        const ReceiveDispatch result = backend.requestPanDisplay(QStringLiteral("0x40000001"), QStringLiteral("0x42000001"), entry.request);
        check(result == ReceiveDispatch::Dispatched && backend.commands() == QStringList{QString::fromLatin1(entry.expected)}, entry.expected);
    }
    QLocale::setDefault(QLocale::c());
    const PanDisplayRequest invalid[]{
        {Field::Count, 1}, {static_cast<Field>(-1), true}, {Field::FrameRate, 0},
        {Field::Average, 101}, {Field::Wnb, 1}, {Field::WaterfallDefaults, false},
        {Field::Band, QStringLiteral("20m\nmox 1")},
        {Field::Pixels, PanPixelDimensions{0, 700}}, {Field::LevelRange, PanLevelRange{-40, -130}},
        {Field::Geometry, PanGeometryRequest{std::numeric_limits<double>::infinity(), {}}},
        {Field::Geometry, PanGeometryRequest{{}, std::numeric_limits<double>::quiet_NaN()}}
    };
    backend.writes.clear();
    for (const PanDisplayRequest& request : invalid) {
        check(backend.requestPanDisplay(QStringLiteral("0x40000001"), QStringLiteral("0x42000001"), request)
            == ReceiveDispatch::Unsupported, "invalid display payload refused");
    }
    check(backend.requestPanDisplay(QStringLiteral("0x40000001\nmox 1"), {}, {Field::FrameRate, 25}) == ReceiveDispatch::Unsupported,
          "malformed display ID refused");
    check(backend.writes.empty(), "invalid requests write nothing");

    bool current = true;
    backend.requestPanDisplay(QStringLiteral("0x40000001"), QStringLiteral("0x42000001"), {Field::WaterfallDefaults, true}, {}, [&] { return current; });
    backend.answer(0, {RadioControlReply::Kind::Failed, 0x50000061, {}});
    check(backend.writes.size() == 1, "local hold refusal never triggers legacy fallback");
    backend.writes.clear();
    backend.requestPanDisplay(QStringLiteral("0x40000001"), QStringLiteral("0x42000001"), {Field::WaterfallDefaults, true}, {}, [&] { return current; });
    current = false;
    backend.answer(0, {RadioControlReply::Kind::Failed, 1, {}});
    check(backend.writes.size() == 1, "retired request never triggers fallback");
    current = true;
    backend.writes.clear();
    backend.requestPanDisplay(QStringLiteral("0x40000001"), QStringLiteral("0x42000001"), {Field::WaterfallDefaults, true});
    backend.answer(0, {RadioControlReply::Kind::Failed, 1, {}});
    check(backend.commands().last() == QStringLiteral("display waterfall set 0x42000001 auto_black=0 black_level=15 color_gain=50"),
          "explicit firmware rejection permits bounded legacy waterfall fallback");
}

void creation()
{
    for (bool statusFirst : {false, true}) {
        PanControlFixture f;
        int calls = 0;
        QPointer<PanadapterModel> created;
        check(f.radio.requestPanCreation(&f.radio, [&](const RadioControlReply& result, PanadapterModel* pan) {
            ++calls; if (result.kind == RadioControlReply::Kind::Accepted) { created = pan; }
        }, false) == ReceiveDispatch::Dispatched, "create admitted");
        check(f.backend->commands().first() == QStringLiteral("display panafall create x=100 y=100"), "modern create encoding");
        if (statusFirst) { f.publish(QStringLiteral("0x40000001")); }
        f.backend->answer(0, {RadioControlReply::Kind::Accepted, 0, QStringLiteral("0x40000001,0x42000001")});
        if (!statusFirst) { check(calls == 0, "ACK alone is not a published pan"); f.publish(QStringLiteral("0x40000001")); }
        check(calls == 1 && created == f.radio.panadapter(QStringLiteral("0x40000001")), "both arrival orders correlate the actual created pan");
        check(created && created->waterfallId().isEmpty(), "optional waterfall does not block usable pan publication");
        f.backend->answer(0, {RadioControlReply::Kind::Accepted, 0, QStringLiteral("0x40000002")});
        check(calls == 1 && TxOperationIntegrationTestAccess::pending(f.radio) == 0, "duplicate reply completes at most once");
    }
    {
        PanControlFixture f;
        int calls = 0;
        f.radio.requestPanCreation(&f.radio, [&](const RadioControlReply&, PanadapterModel*) { ++calls; }, false);
        QTimer* deadline = f.radio.findChild<QTimer*>(QStringLiteral("panCreationDeadline"));
        check(deadline && QMetaObject::invokeMethod(deadline, "timeout", Qt::DirectConnection), "timeout driven deterministically");
        check(calls == 1 && TxOperationIntegrationTestAccess::pending(f.radio) == 1, "unknown timeout retains allocation reservation");
        f.backend->answer(0, {RadioControlReply::Kind::Accepted, 0, QStringLiteral("0x40000001")});
        f.publish(QStringLiteral("0x40000001"));
        check(calls == 1 && TxOperationIntegrationTestAccess::pending(f.radio) == 0, "late identity settles bookkeeping without a second callback");
        check(f.backend->commands().filter(QStringLiteral("create")).size() == 1, "timeout never retries creation");
    }
    {
        PanControlFixture f;
        f.radio.requestPanCreation(&f.radio, {}, false);
        f.backend->answer(0, {RadioControlReply::Kind::Accepted, 0, QStringLiteral("not an id")});
        check(TxOperationIntegrationTestAccess::pending(f.radio) == 1, "malformed successful reply cannot free an allocation reservation");
        TxOperationIntegrationTestAccess::newSession(f.radio);
        check(TxOperationIntegrationTestAccess::pending(f.radio) == 0, "new session retires unknown allocations");
    }
    {
        PanControlFixture f;
        auto owner = std::make_unique<QObject>();
        int calls = 0;
        f.radio.requestPanCreation(owner.get(), [&](const RadioControlReply&, PanadapterModel*) { ++calls; });
        owner.reset();
        f.backend->answer(0, {RadioControlReply::Kind::Accepted, 0, QStringLiteral("0x40000001")});
        f.publish(QStringLiteral("0x40000001"));
        events(230);
        check(calls == 0 && TxOperationIntegrationTestAccess::pending(f.radio) == 0, "dead requester gets no callback or initialization; receipt still settles");
        check(f.backend->commands().filter(QStringLiteral("min_dbm=")).isEmpty(), "dead requester cannot initialize a late pan");
    }
    {
        PanControlFixture f;
        PanadapterModel* old = f.publish(QStringLiteral("0x40000001"));
        f.clear();
        int accepted = 0;
        f.radio.requestPanCreation(&f.radio, [&](const RadioControlReply& result, PanadapterModel*) { accepted += result.kind == RadioControlReply::Kind::Accepted; });
        f.backend->answer(0, {RadioControlReply::Kind::Accepted, 0, QStringLiteral("0x40000001")});
        events(230);
        check(accepted == 0 && f.radio.panadapter(QStringLiteral("0x40000001")) == old
            && f.backend->commands().filter(QStringLiteral("min_dbm=")).isEmpty(), "preexisting/restored ID is never initialized as a new allocation");
    }
    {
        PanControlFixture f;
        int accepted = 0;
        f.radio.requestPanCreation(&f.radio, [&](const RadioControlReply& result, PanadapterModel*) { accepted += result.kind == RadioControlReply::Kind::Accepted; });
        f.status(QStringLiteral("display pan 0x40000001"), {{QStringLiteral("client_handle"), QStringLiteral("0x72")}});
        f.backend->answer(0, {RadioControlReply::Kind::Accepted, 0, QStringLiteral("0x40000001")});
        f.publish(QStringLiteral("0x40000001"));
        check(accepted == 0, "foreign prepublication invalidates the old allocation identity");
    }
    {
        PanControlFixture f;
        f.radio.requestPanCreation(&f.radio, {}, false);
        for (int i = 0; i < 300; ++i) { TxOperationIntegrationTestAccess::retire(f.radio, QString::number(i)); }
        f.backend->answer(0, {RadioControlReply::Kind::Accepted, 0, QStringLiteral("0x40000001")});
        f.publish(QStringLiteral("0x40000001"));
        check(TxOperationIntegrationTestAccess::history(f.radio) <= 256 && TxOperationIntegrationTestAccess::pending(f.radio) == 1,
              "bounded history compaction invalidates pending identity rather than fabricating proof");
    }
    {
        PanControlFixture f;
        int admitted = 0;
        for (int i = 0; i < 70; ++i) { admitted += f.radio.requestPanCreation(&f.radio, {}, false) == ReceiveDispatch::Dispatched; }
        check(admitted > 0 && admitted <= 64 && TxOperationIntegrationTestAccess::pending(f.radio) == admitted,
              "pending creates share finite backend capacity");
    }
}

void initializationAndReadback()
{
    PanControlFixture f;
    f.radio.requestPanCreation(&f.radio, [&](const RadioControlReply& reply, PanadapterModel* pan) {
        if (reply.kind != RadioControlReply::Kind::Accepted) { return; }
        f.radio.requestPanDisplay(pan, {PanDisplayField::LevelRange, PanLevelRange{-115, -35}});
        f.radio.requestPanDimensions(pan, 1400, 850, 1400);
    });
    f.backend->answer(0, {RadioControlReply::Kind::Accepted, 0, QStringLiteral("0x40000001")});
    PanadapterModel* pan = f.publish(QStringLiteral("0x40000001"), QStringLiteral("0x42000001"));
    events(230);
    check(f.backend->commands().contains(QStringLiteral("display pan set 0x40000001 min_dbm=-115.00 max_dbm=-35.00"))
        && !f.backend->commands().contains(QStringLiteral("display pan set 0x40000001 min_dbm=-130.00 max_dbm=-40.00")),
          "newer range intent outranks delayed defaults");
    check(!f.backend->commands().contains(QStringLiteral("display pan set 0x40000001 xpixels=1024 ypixels=700")),
          "actual dimensions outrank fallback dimensions");
    f.clear();
    f.backend->onWrite = [pan](const QString&) { pan->applyStateExtension({{QStringLiteral("fps"), 17}}); };
    f.radio.requestPanDisplayRates(pan->panId(), 30, 0);
    check(pan->fps() == 17, "synchronous newer FPS observation outranks request optimism");
    f.clear();
    f.radio.requestPanAverage(pan->panId(), 73);
    check(pan->average() == 73, "existing pending FFT request display is preserved");
    f.backend->onWrite = [&](const QString& command) {
        if (command.endsWith(QStringLiteral("average=80"))) { f.radio.requestPanAverage(pan->panId(), 73); }
    };
    f.radio.requestPanAverage(pan->panId(), 80);
    check(pan->average() == 73, "reentrant newer average intent outranks older request optimism");
    f.clear();
    f.backend->onWrite = [&](const QString& command) {
        if (command.endsWith(QStringLiteral("center=14.200000"))) { f.radio.requestPanCenter(pan->panId(), 14.1); }
    };
    f.radio.requestPanCenter(pan->panId(), 14.2);
    check(pan->centerMhz() == 14.1, "reentrant same-as-model geometry intent cannot be overwritten by older optimism");
    f.clear();
    check(f.radio.resyncDisplayInventory() && f.radio.panadapter(pan->panId()) == pan
        && f.backend->commands().last() == QStringLiteral("sub pan all"), "inventory refresh does not clear current topology");
}

void removal()
{
    RecordingPanBackend backend;
    int calls = 0;
    RadioControlReply::Kind kind = RadioControlReply::Kind::Cancelled;
    backend.requestPanLifecycle({PanLifecycleAction::Remove, QStringLiteral("0x40000001"), QStringLiteral("0x42000001")},
        [&](const PanLifecycleReply& reply) { ++calls; kind = reply.result.kind; });
    check(backend.commands() == QStringList{QStringLiteral("display pan remove 0x40000001"), QStringLiteral("display panafall remove 0x42000001")},
          "paired cleanup uses original pan and waterfall IDs");
    backend.answer(1);
    check(calls == 0, "reverse-order waterfall ACK cannot complete teardown alone");
    backend.answer(0, {RadioControlReply::Kind::Failed, 1, QStringLiteral("denied")});
    check(calls == 1 && kind == RadioControlReply::Kind::Failed, "partial teardown failure is not reported as complete");
    backend.answer(0);
    check(calls == 1, "teardown reply completed once");
    {
        PanControlFixture f;
        f.publish(QStringLiteral("0x40000000"));
        f.clear();
        check(!f.radio.removePanadapter(QStringLiteral("0x40000000")), "ordinary close preserves the last pan");
        check(f.radio.removePanadapter(QStringLiteral("0x40000000"), PanCloseIntent::ExplicitTeardown), "explicit teardown can request last-pan removal");
    }
    for (bool reassigned : {false, true}) {
        PanControlFixture f;
        f.publish(QStringLiteral("0x40000000"));
        PanadapterModel* paired = f.publish(QStringLiteral("0x40000001"), QStringLiteral("0x42000001"));
        check(paired && paired->waterfallId() == QStringLiteral("0x42000001"), "fixture observed original waterfall pairing");
        f.clear();
        f.backend->onWrite = [&](const QString& command) {
            if (command != QStringLiteral("display pan remove 0x40000001")) { return; }
            f.remove(QStringLiteral("0x40000001"));
            if (reassigned) { f.status(QStringLiteral("display waterfall 0x42000001"),
                {{QStringLiteral("client_handle"), QStringLiteral("0x72")}, {QStringLiteral("panadapter"), QStringLiteral("0x40000002")}}); }
        };
        f.radio.removePanadapter(QStringLiteral("0x40000001"));
        if (!reassigned && f.backend->commands().size() != 2) {
            std::printf("teardown writes: %s\n", qPrintable(f.backend->commands().join(QLatin1Char('|'))));
        }
        check(f.backend->commands().contains(QStringLiteral("display panafall remove 0x42000001")) != reassigned,
              reassigned ? "reassigned waterfall is never removed" : "pan publication can retire before original waterfall cleanup dispatch");
    }
}

void deferredGeometry()
{
    PanControlFixture f;
    PanadapterModel* pan = f.publish(QStringLiteral("0x40000000"));
    f.clear();
    TxOperationIntegrationTestAccess::hold(f.radio);
    f.radio.requestPanBand(pan->panId(), QStringLiteral("20m"));
    f.radio.requestPanCenter(pan->panId(), 14.2, 0.3);
    check(f.backend->writes.empty() && pan->centerMhz() == 14.1, "profile hold keeps geometry unapplied and unsent");
    f.backend->onWrite = [&](const QString& command) {
        if (command.endsWith(QStringLiteral("band=20m"))) { f.radio.requestPanCenter(pan->panId(), 14.3, 0.4); }
    };
    TxOperationIntegrationTestAccess::flush(f.radio);
    check(f.backend->commands().contains(QStringLiteral("display pan set 0x40000000 center=14.300000 bandwidth=0.400000"))
        && !f.backend->commands().contains(QStringLiteral("display pan set 0x40000000 center=14.200000 bandwidth=0.300000")),
          "reentrant newer geometry supersedes detached profile batch atomically");
    f.clear();
    TxOperationIntegrationTestAccess::hold(f.radio);
    f.radio.requestPanBand(pan->panId(), QStringLiteral("40m"));
    f.radio.requestPanCenter(pan->panId(), 7.2, 0.2);
    f.backend->onWrite = [&](const QString& command) {
        if (!command.endsWith(QStringLiteral("band=40m"))) { return; }
        f.remove(QStringLiteral("0x40000000"));
        f.publish(QStringLiteral("0x40000000"));
    };
    TxOperationIntegrationTestAccess::flush(f.radio);
    check(!f.backend->commands().contains(QStringLiteral("display pan set 0x40000000 center=7.200000 bandwidth=0.200000")),
          "replacement ID cannot receive old detached geometry");
}

void staleDisplayAndGainInfo()
{
    PanControlFixture f;
    PanadapterModel* pan = f.publish(QStringLiteral("0x40000000"), QStringLiteral("0x42000000"));
    const QString id = pan->panId();
    const auto gainQuery = std::find_if(f.backend->writes.cbegin(), f.backend->writes.cend(),
        [](const RecordingPanBackend::Write& write) { return write.command.startsWith(QStringLiteral("display pan rfgain_info")); });
    check(gainQuery != f.backend->writes.cend(), "RF gain range query uses lifecycle seam");
    const size_t query = static_cast<size_t>(gainQuery - f.backend->writes.cbegin());
    f.backend->answer(query, {RadioControlReply::Kind::Accepted, 0, QStringLiteral("-10,30,5,ignored")});
    check(pan->rfGainLow() == -10 && pan->rfGainHigh() == 30 && pan->rfGainStep() == 5,
          "optional waterfall association does not cancel RF gain range answer");
    f.clear();
    f.radio.requestPanDisplay(pan, {PanDisplayField::WaterfallDefaults, true});
    f.status(QStringLiteral("display waterfall 0x42000000"),
        {{QStringLiteral("client_handle"), QStringLiteral("0x72")}});
    f.status(QStringLiteral("display waterfall 0x42000000"),
        {{QStringLiteral("client_handle"), QStringLiteral("0x71")}});
    f.backend->answer(0, {RadioControlReply::Kind::Failed, 1, {}});
    check(f.backend->writes.size() == 1, "waterfall owner ABA cannot revive a retired fallback");
    f.clear();
    const PanDisplayRequest components[]{
        {PanDisplayField::WaterfallColorGain, 67},
        {PanDisplayField::WaterfallBlackLevel, 19},
        {PanDisplayField::WaterfallAutoBlack, true}};
    for (const PanDisplayRequest& component : components) {
        f.radio.requestPanDisplay(pan, {PanDisplayField::WaterfallDefaults, true});
        f.radio.requestPanDisplay(pan, component);
        f.backend->answer(0, {RadioControlReply::Kind::Failed, 1, {}});
        check(f.backend->commands().filter(QStringLiteral("display waterfall set")).isEmpty(),
              "aggregate defaults fallback cannot overwrite a newer component setting");
        f.clear();
    }
    f.status(QStringLiteral("display waterfall 0x42000000"),
        {{QStringLiteral("client_handle"), QStringLiteral("0x72")}});
    check(f.radio.requestPanDisplay(pan, {PanDisplayField::WaterfallBlackLevel, 20}) == ReceiveDispatch::Unsupported
        && f.backend->writes.empty(), "foreign waterfall refuses even when the parent pan is ours");
    f.clear();
    int callbacks = 0;
    RadioControlReply::Kind result = RadioControlReply::Kind::Accepted;
    f.radio.requestPanDisplay(pan, {PanDisplayField::LevelRange, PanLevelRange{-120, -30}},
        [&](const RadioControlReply& reply) { ++callbacks; result = reply.kind; });
    pan->setClientHandle(QStringLiteral("72"));
    pan->setClientHandle(QStringLiteral("71"));
    f.backend->answer(0);
    check(callbacks == 1 && result == RadioControlReply::Kind::Cancelled, "pan ownership ABA cancels old answer");
    f.clear();
    f.radio.requestPanDisplay(pan, {PanDisplayField::LevelRange, PanLevelRange{-125, -35}},
        [&](const RadioControlReply& reply) { ++callbacks; result = reply.kind; });
    TxOperationIntegrationTestAccess::newSession(f.radio);
    f.backend->answer(0);
    check(callbacks == 2 && result == RadioControlReply::Kind::Cancelled, "old session answer cannot apply to reconnect");
    f.clear();
    f.radio.requestPanCreation(&f.radio, {}, false);
    f.remove(QStringLiteral("0x40000001"));
    f.publish(QStringLiteral("0x40000001"));
    f.backend->answer(0, {RadioControlReply::Kind::Accepted, 0, QStringLiteral("0x40000001")});
    check(TxOperationIntegrationTestAccess::pending(f.radio) == 1, "prepublication removal cannot be correlated to a reused ID");
    check(f.radio.panadapter(id) == pan, "adversarial replies preserve unrelated pan identity");
}
} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("pan-control-routing"));
    if (!profile.isValid()) { return 1; }
    QCoreApplication app(argc, argv);
    encoding();
    creation();
    initializationAndReadback();
    removal();
    deferredGeometry();
    staleDisplayAndGainInfo();
    std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
