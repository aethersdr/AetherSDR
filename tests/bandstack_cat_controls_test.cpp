// Production recall, CAT parser and Flex encoders with injected sinks only.
// No sockets, external peer, physical device or keying operation.
#include "TestSettingsProfile.h"
#include "RadioSetupControlFixture.h"
#include "core/BandStackSettings.h"
#include "core/RigctlProtocol.h"

#include <QCoreApplication>
#include <QEvent>
#include <QPointer>
#include <QSignalSpy>
#include <cstdio>
#include <functional>
#include <limits>
#include <memory>
#include <thread>

namespace AetherSDR {
class RadioModelSliceLifecycleTestAccess {
public:
    static void wire(RadioModel& radio, SliceModel* slice)
    { radio.wireSliceReceiveIntentsToBackend(slice); }
    static void foreign(RadioModel& radio, int id)
    { radio.m_foreignSliceOwners.insert(id, 123); }
};
}

using namespace AetherSDR;
namespace {
int failures = 0;
void check(bool ok, const char* message)
{
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", message);
    failures += !ok;
}

class Backend final : public FlexBackend {
public:
    bool live{true};
    bool automaticAgc{false};
    bool localStep{false};
    QStringList commands;
    std::function<void(const QString&)> onCommand;
    Backend()
    {
        setModelProvider([] { return QStringLiteral("FLEX-6700"); });
        setSliceCommandSink([this](const QString& command) { record(command); });
        setSetupCommandSink([this](const QString& command, const RadioSetupCompletion&) {
            record(command);
            return true;
        });
    }
    bool isConnected() const override { return live; }
    ReceiveDispatch requestSliceAgcThresholdMode(int, AgcThresholdMode) override
    { return ReceiveDispatch::Dispatched; }
    RadioCapabilities capabilities() const override
    {
        RadioCapabilities caps = FlexBackend::capabilities();
        if (automaticAgc) {
            caps.autoAgcThresholdControl = RadioCapabilities::AutoAgcThresholdControl{};
        }
        if (localStep) { caps.slicePresetControl.reset(); }
        return caps;
    }
    void record(const QString& command)
    {
        commands.append(command);
        const auto callback = onCommand;
        if (callback) { callback(command); }
    }
};

struct Fixture {
    RadioModel radio;
    Backend* backend;
    SliceModel* slice;
    PanadapterModel* pan;
    Fixture()
    {
        auto owned = std::make_unique<Backend>();
        backend = owned.get();
        radio.setBackendForTest(std::move(owned), QStringLiteral("flex"));
        pan = RadioSetupControlTestAccess::addPan(radio, QStringLiteral("0x40000000"));
        slice = addSlice(3);
    }
    SliceModel* addSlice(int id)
    {
        SliceModel* result = RadioSetupControlTestAccess::addSlice(radio, id);
        SliceDelta d;
        d.panId = pan->panId(); d.mode = QStringLiteral("USB"); d.frequency = 14.1;
        d.modeList = QStringList{"USB", "LSB", "DIGU", "DIGL"};
        d.rxAntenna = QStringLiteral("ANT1"); d.txAntenna = QStringLiteral("ANT1");
        d.rxAntennaList = QStringList{"ANT1", "ANT2", "ANT3", "RX_A"};
        d.txAntennaList = QStringList{"ANT1", "ANT2", "ANT3"};
        d.agcMode = QStringLiteral("slow"); d.agcThreshold = 65; d.agcOffLevel = 30;
        result->applyChanges(d);
        RadioModelSliceLifecycleTestAccess::wire(radio, result);
        return result;
    }
    static BandStackEntry entry()
    {
        BandStackEntry e;
        e.frequencyMhz = 14.2; e.mode = QStringLiteral("LSB");
        e.rxAntenna = QStringLiteral("ANT2"); e.txAntenna = QStringLiteral("ANT2");
        e.agcMode = QStringLiteral("fast"); e.agcThreshold = 40;
        return e;
    }
    void recall(const BandStackEntry& e)
    { radio.recallBandStack(slice, e, [](SliceModel*, double) {}); }
};

void recallEncoding()
{
    Fixture f;
    f.slice->setExternalReceiveAudioReplacementMute(true, false);
    const QString kiwiMode = f.slice->receiveAgcMode();
    const int kiwiThreshold = f.slice->receiveAgcThreshold();
    f.backend->commands.clear();
    QSignalSpy rx(f.slice, &SliceModel::rxAntennaChanged);
    QSignalSpy tx(f.slice, &SliceModel::txAntennaChanged);
    f.recall(Fixture::entry());
    check(f.backend->commands.mid(0, 5) == QStringList{
        "slice set 3 mode=LSB", "slice set 3 rxant=ANT2", "slice set 3 txant=ANT2",
        "slice set 3 agc_mode=fast", "slice set 3 agc_threshold=40"},
        "recall sends mode, antennas and independent radio AGC fields in order");
    check(f.slice->rxAntenna() == QLatin1String("ANT1") && f.slice->txAntenna() == QLatin1String("ANT1")
          && rx.isEmpty() && tx.isEmpty(), "antenna admission does not fabricate state or notify accessories");
    check(f.slice->receiveAgcMode() == kiwiMode && f.slice->receiveAgcThreshold() == kiwiThreshold
          && f.slice->agcMode() == QLatin1String("slow") && f.slice->agcThreshold() == 65,
          "Kiwi and underlying radio AGC observations are unchanged until their own readback");
    SliceDelta accepted; accepted.rxAntenna = QStringLiteral("ANT2"); accepted.txAntenna = QStringLiteral("ANT2");
    accepted.agcMode = QStringLiteral("fast"); accepted.agcThreshold = 40;
    const qsizetype count = f.backend->commands.size();
    f.slice->applyChanges(accepted);
    check(rx.size() == 1 && tx.size() == 1 && f.backend->commands.size() == count,
          "radio observations publish accepted ports without sending commands back");
}

void invalidAndAutomaticAgc()
{
    Fixture f;
    f.slice->setExternalReceiveAudioReplacementMute(true, false);
    BandStackEntry e = Fixture::entry();
    e.rxAntenna = QStringLiteral("Kiwi receiver"); e.txAntenna = QStringLiteral("RX_A");
    e.agcMode = QStringLiteral("fast\ninvalid"); e.agcThreshold = 101;
    f.backend->commands.clear();
    f.recall(e);
    check(f.backend->commands == QStringList{"slice set 3 mode=LSB"},
          "invalid optional ports and AGC fields are skipped while the valid mode still applies");
    f.backend->commands.clear();
    e.frequencyMhz = std::numeric_limits<double>::quiet_NaN();
    f.recall(e);
    check(f.backend->commands.isEmpty(), "non-finite frequency prevents recall before any mutation");

    e = Fixture::entry(); e.mode = f.slice->mode(); e.rxAntenna.clear(); e.txAntenna.clear(); e.agcMode.clear();
    f.backend->automaticAgc = true;
    f.recall(e);
    check(f.backend->commands.isEmpty(), "unknown AGC-T mode on capable firmware is not manual authority");
    SliceDelta d; d.agcThresholdMode = AgcThresholdMode::Automatic; f.slice->applyChanges(d);
    f.recall(e);
    check(f.backend->commands.isEmpty(), "automatic AGC-T is not overwritten");
    d.agcThresholdMode = AgcThresholdMode::Manual; f.slice->applyChanges(d);
    f.slice->setExternalReceiveAudioReplacementMute(false, false);
    f.slice->requestAgcThresholdMode(AgcThresholdMode::Automatic);
    check(f.slice->automaticAgcThresholdActiveOrPending(), "production Auto request suppresses recall before readback");
    f.backend->commands.clear();
    f.recall(e);
    check(f.backend->commands.isEmpty(), "pending automatic AGC-T is not overwritten by recall");
    f.slice->requestAgcThresholdMode(AgcThresholdMode::Manual);
    f.slice->setExternalReceiveAudioReplacementMute(true, false);
    f.backend->commands.clear();
    f.recall(e);
    check(f.backend->commands == QStringList{"slice set 3 agc_threshold=40"},
          "known manual AGC-T permits only the recalled threshold field");
    f.backend->commands.clear();
    f.backend->requestSliceAgc(3, {SliceAgcRequest::Field::Mode, QStringLiteral("fast\ninvalid"), 40, 30});
    f.backend->requestSliceAgc(3, {SliceAgcRequest::Field::Threshold, QStringLiteral("slow"), -1, 30});
    f.backend->requestSliceAgc(-1, {SliceAgcRequest::Field::Mode, QStringLiteral("fast"), 40, 30});
    check(f.backend->commands.isEmpty(), "the production Flex encoder independently rejects malformed AGC requests");
}

void recallModeVocabulary()
{
    Fixture f;
    BandStackEntry e = Fixture::entry();
    e.rxAntenna.clear(); e.txAntenna.clear();
    e.mode = QStringLiteral("FDVU");
    bool tuned = false;
    f.radio.recallBandStack(f.slice, e, [&](SliceModel*, double) { tuned = true; });
    check(tuned, "client-side digital modes remain recallable without a firmware mode-list entry");
    SliceDelta d; d.modeList = QStringList{"USB", "QPSK"}; f.slice->applyChanges(d);
    e.mode = QStringLiteral("QPSK"); f.backend->commands.clear();
    f.recall(e);
    check(f.backend->commands.contains("slice set 3 mode=QPSK"),
          "a safe radio-advertised waveform mode remains recallable");
    e.mode = QStringLiteral("USB\ninvalid");
    d.modeList = QStringList{e.mode}; f.slice->applyChanges(d);
    f.backend->commands.clear(); f.recall(e);
    check(f.backend->commands.isEmpty(), "even an advertised malformed mode cannot enter recall");
}

void recallLifetime()
{
    for (int scenario = 0; scenario < 7; ++scenario) {
        Fixture f;
        f.backend->onCommand = [&](const QString&) {
            f.backend->onCommand = {};
            switch (scenario) {
            case 0: f.backend->live = false; break;
            case 1: RadioSetupControlTestAccess::replaceConnectionGeneration(f.radio); break;
            case 2: f.addSlice(3); break;
            case 3: RadioSetupControlTestAccess::addPan(f.radio, f.pan->panId()); break;
            case 4: RadioModelSliceLifecycleTestAccess::foreign(f.radio, 3); break;
            case 5: f.pan->setClientHandle(QStringLiteral("0x123")); break;
            case 6: f.slice->invalidateFrequencyObservation(); break;
            }
        };
        f.recall(Fixture::entry());
        check(f.backend->commands == QStringList{"slice set 3 mode=LSB"},
              "session, slice, pan or ownership replacement stops the recall tail");
    }
    {
        Fixture f;
        BandStackEntry outer = Fixture::entry();
        BandStackEntry inner = outer; inner.mode = QStringLiteral("DIGU");
        inner.rxAntenna = QStringLiteral("ANT3"); inner.txAntenna.clear();
        f.backend->onCommand = [&](const QString&) {
            f.backend->onCommand = {};
            f.recall(inner);
        };
        f.recall(outer);
        check(f.backend->commands.contains("slice set 3 rxant=ANT3")
              && !f.backend->commands.contains("slice set 3 rxant=ANT2")
              && !f.backend->commands.contains("slice set 3 txant=ANT2"),
              "nested newer recall supersedes the entire older tail");
    }
    {
        Fixture f;
        BandStackEntry e = Fixture::entry();
        f.radio.recallBandStack(f.slice, e, [&](SliceModel*, double) {
            e.rxAntenna = QStringLiteral("ANT3");
        });
        check(f.backend->commands.contains("slice set 3 rxant=ANT2")
              && !f.backend->commands.contains("slice set 3 rxant=ANT3"),
              "recall holds a value copy across bookmark-list edits");
    }
    {
        auto f = std::make_unique<Fixture>();
        f->radio.recallBandStack(f->slice, Fixture::entry(), [&](SliceModel*, double) { f.reset(); });
        check(!f, "destroying the model inside the tuning callback safely ends recall");
    }
    {
        Fixture f;
        std::thread worker([&] { f.recall(Fixture::entry()); });
        worker.join();
        check(f.backend->commands.isEmpty(), "recall from a foreign thread is refused before model access");
    }
    {
        Fixture f;
        BandStackEntry e = Fixture::entry();
        e.wnbOn = true; e.wnbLevel = 42;
        f.radio.recallBandStack(f.slice, e, [&](SliceModel*, double) {
            RadioSetupControlTestAccess::addPan(f.radio, QStringLiteral("0x40000001"));
            f.addSlice(4)->setActive(true);
        });
        check(f.backend->commands.contains("display pan set 0x40000000 wnb=1")
              && f.backend->commands.contains("display pan set 0x40000000 wnb_level=42"),
              "focus changes cannot redirect WNB to another panadapter");
    }
}

void catSteps()
{
    Fixture f;
    RigctlProtocol cat(&f.radio);
    cat.setSliceIndex(3);
    check(cat.handleLine("\\set_ts ?").endsWith("500000 0\n"), "CAT keeps its any-positive-step capability marker");
    for (const QString& value : {QString(), QStringLiteral("0"), QStringLiteral("-1"),
                                QStringLiteral("2147483648"), QStringLiteral("1.5"), QStringLiteral("abc")}) {
        check(cat.handleLine(QStringLiteral("\\set_ts ") + value) == QLatin1String("RPRT -1\n"),
              "CAT rejects missing, non-positive, overflowing and malformed steps");
    }
    check(cat.handleLine("\\set_ts VFOMEM 100") == QLatin1String("RPRT -8\n")
          && cat.handleLine("\\set_ts VFOB 100") == QLatin1String("RPRT -8\n"),
          "unavailable VFOs do not silently target the bound slice");
    const int before = f.slice->stepHz();
    check(cat.handleLine("\\set_ts VFOA 100") == QLatin1String("RPRT 0\n")
          && cat.handleLine("\\set_ts MAIN 250") == QLatin1String("RPRT 0\n")
          && f.backend->commands.isEmpty(), "CAT acknowledges queue acceptance before dispatch");
    f.addSlice(4); cat.setSliceIndex(4);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    check(f.backend->commands == QStringList{"slice set 3 step=100", "slice set 3 step=250"}
          && f.slice->stepHz() == before, "queued steps preserve order and original target without invented radio readback");
    SliceDelta d; d.step = 250; f.slice->applyChanges(d);
    cat.setSliceIndex(3);
    check(cat.handleLine("\\get_ts") == QLatin1String("250\n"), "CAT get reports the observed step");
    f.backend->commands.clear();
    cat.handleLine("\\set_ts 500");
    RadioSetupControlTestAccess::replaceConnectionGeneration(f.radio);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    check(f.backend->commands.isEmpty(), "a queued CAT step is rejected after reconnect");
    cat.handleLine("\\set_ts 500");
    f.slice = f.addSlice(3);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    check(f.backend->commands.isEmpty(), "a queued CAT step is rejected after numeric ID reuse");
    cat.handleLine("\\set_ts 500");
    f.slice->invalidateFrequencyObservation();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    check(f.backend->commands.isEmpty(), "a retained slice object cannot replay a retired receive generation");
    cat.handleLine("\\set_ts 500");
    RadioModelSliceLifecycleTestAccess::foreign(f.radio, 3);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    check(f.backend->commands.isEmpty(), "a queued CAT step cannot modify a newly foreign slice");
    f.slice = f.addSlice(5); cat.setSliceIndex(5);
    f.backend->localStep = true;
    cat.handleLine("\\set_ts 125");
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    check(f.slice->stepHz() == 125 && f.backend->commands.isEmpty(), "client-owned steps retain their local consumer");
}
}

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("bandstack-cat-controls"));
    QCoreApplication app(argc, argv);
    check(profile.isValid(), "settings are isolated");
    recallEncoding();
    invalidAndAutomaticAgc();
    recallModeVocabulary();
    recallLifetime();
    catSteps();
    return failures ? 1 : 0;
}
