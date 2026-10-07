// What a Hermes-Lite 2 remembers across a restart beyond the flat operating
// state: TUNE power per band, a chosen auto-RF-gain law and floor, every
// receiver's own setpoints and the receiver set (#5777), and the manual
// notches. The radio stores none of these, so the client is its memory.
//
// Socket-free: the link edge is injected as in hl2_pan_create_async_test, so
// the replay runs through the production linkUp handler and the operator's add
// path. RadioStateMemory is driven against an isolated settings profile.

#include "core/AppSettings.h"
#include "core/RadioSettingsScope.h"
#include "core/RadioStateMemory.h"
#include "core/backends/NotchDelta.h"
#include "core/backends/SliceDelta.h"
#include "core/backends/TransmitDelta.h"
#include "core/backends/hl2/Hl2Backend.h"
#include "core/backends/hl2/Hl2Receivers.h"
#include "core/backends/hl2/Hl2RxDsp.h"
#include "core/backends/hl2/MetisClient.h"

#include "TestDspBuildWait.h"
#include "TestSettingsProfile.h"

#include <QCoreApplication>
#include <QEvent>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>

#include <cstdio>
#include <tuple>

namespace AetherSDR::hl2 {

struct Hl2SessionMemoryTestAccess {
    // One configured receiver and a link edge, without connectRadio().
    static bool bringUp(Hl2Backend& backend, bool replayPending, int count = 1)
    {
        backend.m_boardMaxRx = Hl2Backend::kAssumedBoardMaxRx;
        backend.buildReceivers(count);
        for (int ddc = 0; ddc < count; ++ddc) {
            Hl2Backend::Receiver* const receiver = backend.rx(ddc);
            if (!receiver || !receiver->dsp) {
                return false;
            }
            Hl2RxDsp::Config config;
            config.inputSampleRateHz = backend.m_rateLedger.committed();
            std::tie(config.filterLowHz, config.filterHighHz) = backend.dspFilterHz(*receiver);
            bool configured = false;
            int channelId = -1;
            QMetaObject::invokeMethod(receiver->dsp, [receiver, config, &configured, &channelId] {
                configured = receiver->dsp->configure(config);
                channelId = receiver->dsp->wdspChannelId();
            }, Qt::BlockingQueuedConnection);
            if (!configured) {
                return false;
            }
            receiver->configuredRateHz = config.inputSampleRateHz;
            Hl2ReceiverIds* const ids = backend.m_ids.mutableByDdc(ddc);
            ids->dspChannel = channelId;
            ids->analyzerId = ids->uiNumber;
        }
        backend.publishIoDsps();
        // What connectRadio() arms for every connect.
        backend.m_sessionReplayPending = replayPending;
        QMetaObject::invokeMethod(backend.m_metis, "linkUp", Qt::BlockingQueuedConnection);
        QCoreApplication::sendPostedEvents(&backend, QEvent::MetaCall);
        return backend.isConnected();
    }
    static void linkUpAgain(Hl2Backend& backend)
    {
        QMetaObject::invokeMethod(backend.m_metis, "linkUp", Qt::BlockingQueuedConnection);
        QCoreApplication::sendPostedEvents(&backend, QEvent::MetaCall);
    }
    static int receiverCount(const Hl2Backend& backend)
    {
        return static_cast<int>(backend.m_rx.size());
    }
    static int ceiling(const Hl2Backend& backend) { return backend.receiverCeiling(); }
    static bool buildsSettled(const Hl2Backend& backend)
    {
        for (const Hl2Backend::Receiver& r : backend.m_rx) {
            if (r.dspBuildInFlight) {
                return false;
            }
        }
        return true;
    }
    static int restoredReceivers(const Hl2Backend& backend)
    {
        return static_cast<int>(backend.m_restoredReceivers.size());
    }
    static void seedFirstReceiver(Hl2Backend& backend) { backend.seedFirstReceiverFromMemory(); }
    static int txDdc(const Hl2Backend& backend) { return backend.m_txDdc; }
    static void setTuning(Hl2Backend& backend, bool on) { backend.m_tuning = on; }
    static void armReplay(Hl2Backend& backend) { backend.m_sessionReplayPending = true; }
};

}   // namespace AetherSDR::hl2

using namespace AetherSDR;
using AetherSDR::hl2::Hl2Backend;
using Access = AetherSDR::hl2::Hl2SessionMemoryTestAccess;

namespace {

int g_failures = 0;

void check(bool condition, const char* what)
{
    std::fprintf(stderr, "[%s] %s\n", condition ? " OK " : "FAIL", what);
    if (!condition) {
        ++g_failures;
    }
}

QJsonObject ext(const RestoredRadioState& state, const char* key)
{
    return state.extension.value(QLatin1String(key)).toObject();
}

QJsonObject receiverEntry(double freqHz, const QString& mode)
{
    return QJsonObject{{QStringLiteral("freqHz"), freqHz},
                       {QStringLiteral("ncoHz"), freqHz},
                       {QStringLiteral("mode"), mode}};
}

RestoredRadioState stateWith(const QJsonArray& receivers, const QJsonArray& notches = {})
{
    RestoredRadioState state;
    state.rfFrequencyHz = 7'100'000.0;
    state.mode = QStringLiteral("LSB");
    state.extensionSchemaVersion = 1;
    state.extension.insert(QStringLiteral("receivers"), receivers);
    state.extension.insert(QStringLiteral("notches"), notches);
    return state;
}

// The setters the operator reaches on receiver A land in the document, and a
// new backend reading it back gives receiver A the same values.
void receiverSetpointsRoundTrip()
{
    Hl2Backend backend;
    backend.setSliceNoiseBlanker(0, NoiseBlankerKind::Advanced, 70, NoiseBlankerFill::MeanHold);
    backend.setSliceApf(0, true, 35);
    backend.setSliceSquelch(0, true, 44);
    backend.setSliceRitEnabled(0, true);
    backend.setSliceRitOffset(0, 120);
    backend.setSliceXitEnabled(0, true);
    backend.setSliceXitOffset(0, -80);
    backend.setSliceAudioGain(0, 60);
    backend.setSliceAudioPan(0, 20);

    const RestoredRadioState captured = backend.currentOperatingState();
    const QJsonArray receivers = captured.extension.value(QStringLiteral("receivers")).toArray();
    check(receivers.size() == 1, "capture lists the one running receiver");
    const QJsonObject a = receivers.at(0).toObject();
    check(a.value(QStringLiteral("nbKind")).toInt() == static_cast<int>(NoiseBlankerKind::Advanced)
              && a.value(QStringLiteral("nbLevel")).toInt() == 70
              && a.value(QStringLiteral("nbFill")).toInt() == static_cast<int>(NoiseBlankerFill::MeanHold),
          "capture carries the blanker kind, level and fill");
    check(a.value(QStringLiteral("apfOn")).toBool() && a.value(QStringLiteral("apfLevel")).toInt() == 35,
          "capture carries APF");
    check(a.value(QStringLiteral("squelchOn")).toBool()
              && a.value(QStringLiteral("squelchLevel")).toInt() == 44,
          "capture carries squelch");
    check(a.value(QStringLiteral("ritOn")).toBool() && a.value(QStringLiteral("ritHz")).toInt() == 120
              && a.value(QStringLiteral("xitOn")).toBool()
              && a.value(QStringLiteral("xitHz")).toInt() == -80,
          "capture carries RIT and XIT");
    check(a.value(QStringLiteral("audioGain")).toInt() == 60
              && a.value(QStringLiteral("audioPan")).toInt() == 20,
          "capture carries audio gain and pan");

    Hl2Backend next;
    QHash<int, SliceDelta> seen;
    int nbEchoes = 0;
    SliceDelta nb;
    QObject::connect(&next, &IRadioBackend::sliceChanged, &next,
                     [&](int id, const SliceDelta& d) {
                         seen[id] = d;
                         if (id == 0 && d.nbKind) {
                             ++nbEchoes;
                             nb = d;
                         }
                     });
    next.applyRestoredState(captured);
    check(Access::restoredReceivers(next) == 1, "restore keeps the valid entry");
    Access::seedFirstReceiver(next);
    next.setSliceFrequency(0, next.currentOperatingState().rfFrequencyHz);   // a publish
    next.setSliceFrequency(0, next.currentOperatingState().rfFrequencyHz + 100.0);
    const SliceDelta d = seen.value(0);
    check(nb.nbKind && *nb.nbKind == NoiseBlankerKind::Advanced && nb.nbLevel && *nb.nbLevel == 70
              && nb.nbFill && *nb.nbFill == NoiseBlankerFill::MeanHold,
          "receiver A comes back with its blanker, and publishes it");
    check(nbEchoes == 1, "the restored blanker is published once, not on every slice echo");
    check(d.apf && *d.apf && d.apfLevel && *d.apfLevel == 35, "receiver A comes back with APF");
    check(d.squelchOn && *d.squelchOn && d.squelchLevel && *d.squelchLevel == 44,
          "receiver A comes back with squelch");
    check(d.ritOn && *d.ritOn && d.ritFreq && *d.ritFreq == 120 && d.xitOn && *d.xitOn
              && d.xitFreq && *d.xitFreq == -80,
          "receiver A comes back with RIT and XIT");
    check(d.audioGain && *d.audioGain == 60 && d.audioPan && *d.audioPan == 20,
          "receiver A comes back with audio gain and pan");
}

// Each field is judged on its own: a bad one falls to its default and the
// rest of the entry stands; an entry with no usable frequency ends the list.
void invalidEntriesAreDroppedNotClamped()
{
    QJsonObject a = receiverEntry(7'100'000.0, QStringLiteral("LSB"));
    a.insert(QStringLiteral("nbKind"), 7);
    a.insert(QStringLiteral("apfLevel"), 250);
    a.insert(QStringLiteral("ritOn"), true);
    a.insert(QStringLiteral("ritHz"), 20'000);
    a.insert(QStringLiteral("squelchLevel"), 30);
    QJsonArray receivers{a, receiverEntry(50.0, QStringLiteral("USB")),
                         receiverEntry(14'200'000.0, QStringLiteral("USB"))};
    Hl2Backend backend;
    backend.applyRestoredState(stateWith(receivers));
    check(Access::restoredReceivers(backend) == 1,
          "an entry with an impossible frequency ends the list, so C does not move into B");
    Access::seedFirstReceiver(backend);
    const QJsonObject back = backend.currentOperatingState()
        .extension.value(QStringLiteral("receivers")).toArray().at(0).toObject();
    check(back.value(QStringLiteral("nbKind")).toInt() == 0, "an unknown blanker kind reads as off");
    check(back.value(QStringLiteral("apfLevel")).toInt() == 50, "an out-of-range APF level reads as the default");
    check(back.value(QStringLiteral("ritHz")).toInt() == 0, "an offset past the RIT limit reads as zero");
    check(back.value(QStringLiteral("squelchLevel")).toInt() == 30, "the valid fields of the entry stand");
}

// Receiver A's entry wins over the flat fields, which follow the transmit
// receiver and so can describe receiver B.
void receiverAEntryOverridesTheFlatFields()
{
    QJsonObject a = receiverEntry(3'700'000.0, QStringLiteral("LSB"));
    Hl2Backend backend;
    RestoredRadioState state = stateWith(QJsonArray{a});
    state.rfFrequencyHz = 14'200'000.0;
    state.mode = QStringLiteral("USB");
    backend.applyRestoredState(state);
    check(backend.restoredStateForTest().rfFrequencyHz == 3'700'000.0
              && backend.restoredStateForTest().mode == QStringLiteral("LSB"),
          "receiver A starts on its own frequency and mode");
}

// After link-up the remembered receivers and notches come back through the
// add and create paths, once per connect, within the receiver ceiling.
void theSessionIsReplayedOnceAfterLinkUp()
{
    QJsonObject b = receiverEntry(14'074'000.0, QStringLiteral("DIGU"));
    b.insert(QStringLiteral("nbKind"), static_cast<int>(NoiseBlankerKind::Impulse));
    b.insert(QStringLiteral("audioGain"), 40);
    const QJsonObject c = receiverEntry(10'136'000.0, QStringLiteral("USB"));
    const QJsonArray receivers{receiverEntry(7'100'000.0, QStringLiteral("LSB")), b, c};
    const QJsonArray notches{
        QJsonObject{{QStringLiteral("centerHz"), 7'101'000.0}, {QStringLiteral("widthHz"), 100.0}},
        QJsonObject{{QStringLiteral("centerHz"), 7'102'000.0}, {QStringLiteral("widthHz"), 200.0},
                    {QStringLiteral("active"), false}}};

    Hl2Backend backend;
    backend.applyRestoredState(stateWith(receivers, notches));
    QHash<int, SliceDelta> slices;
    QHash<int, NotchDelta> notchesSeen;
    QObject::connect(&backend, &IRadioBackend::sliceChanged, &backend,
                     [&slices](int id, const SliceDelta& d) { slices[id] = d; });
    QObject::connect(&backend, &IRadioBackend::notchChanged, &backend,
                     [&notchesSeen](int id, const NotchDelta& d) { notchesSeen[id] = d; });
    check(Access::bringUp(backend, true), "the injected link edge reaches the backend");
    check(Access::receiverCount(backend) == 3, "receivers B and C are reopened");
    bool bFound = false;
    bool cFound = false;
    for (auto it = slices.constBegin(); it != slices.constEnd(); ++it) {
        if (it.key() == 0 || !it->frequency) {
            continue;
        }
        if (qAbs(*it->frequency - 14.074) < 1e-6) {
            bFound = it->mode && *it->mode == QStringLiteral("DIGU") && it->nbKind
                && *it->nbKind == NoiseBlankerKind::Impulse && it->audioGain && *it->audioGain == 40
                && it->agcMode && *it->agcMode == QStringLiteral("off");
        }
        if (qAbs(*it->frequency - 10.136) < 1e-6) {
            cFound = it->mode && *it->mode == QStringLiteral("USB");
        }
    }
    check(bFound, "B comes back with its frequency, mode, blanker and audio, AGC off in DIGU");
    check(cFound, "C comes back with its own frequency and mode");
    int active = 0;
    int inactive = 0;
    for (const NotchDelta& d : notchesSeen) {
        (d.active && *d.active ? active : inactive) += 1;
    }
    check(notchesSeen.size() == 2 && active == 1 && inactive == 1,
          "both notches come back with markers, the inactive one inactive");
    AetherSDR::test::spinUntil([&] { return Access::buildsSettled(backend); });

    Access::linkUpAgain(backend);
    check(Access::receiverCount(backend) == 3 && notchesSeen.size() == 2,
          "a link-up after EP6 silence replays nothing a second time");
    AetherSDR::test::spinUntil([&] { return Access::buildsSettled(backend); });
}

// Transmit comes back on the receiver that held it, and the capture says which.
void transmitReturnsToItsReceiver()
{
    QJsonObject b = receiverEntry(14'200'000.0, QStringLiteral("USB"));
    b.insert(QStringLiteral("transmit"), true);
    const QJsonArray receivers{receiverEntry(7'100'000.0, QStringLiteral("LSB")), b};
    Hl2Backend backend;
    backend.applyRestoredState(stateWith(receivers));
    check(Access::bringUp(backend, true), "the injected link edge reaches the backend");
    check(Access::txDdc(backend) == 1, "transmit is back on receiver B");
    const QJsonArray captured = backend.currentOperatingState()
        .extension.value(QStringLiteral("receivers")).toArray();
    check(captured.size() == 2 && !captured.at(0).toObject().contains(QStringLiteral("transmit"))
              && captured.at(1).toObject().value(QStringLiteral("transmit")).toBool(),
          "the capture marks the transmit receiver");
    AetherSDR::test::spinUntil([&] { return Access::buildsSettled(backend); });
}

// Receivers a connect already opened (the numRx param) take their slots in
// the list; the replay adds only the ones after them.
void theReplayCountsReceiversAlreadyRunning()
{
    const QJsonArray receivers{receiverEntry(7'100'000.0, QStringLiteral("LSB")),
                               receiverEntry(14'074'000.0, QStringLiteral("USB")),
                               receiverEntry(10'136'000.0, QStringLiteral("USB"))};
    Hl2Backend backend;
    backend.applyRestoredState(stateWith(receivers));
    check(Access::bringUp(backend, true, 2), "two receivers are up before the replay");
    check(Access::receiverCount(backend) == 3, "the replay adds only the third");
    AetherSDR::test::spinUntil([&] { return Access::buildsSettled(backend); });
}

// A capture taken before the replay has run keeps the remembered lists: the
// store rebuilds the document, so a list left out would be erased.
void aCaptureBeforeTheReplayKeepsTheLists()
{
    const QJsonArray receivers{receiverEntry(7'100'000.0, QStringLiteral("LSB")),
                               receiverEntry(14'074'000.0, QStringLiteral("USB"))};
    const QJsonArray notches{
        QJsonObject{{QStringLiteral("centerHz"), 7'101'000.0}, {QStringLiteral("widthHz"), 100.0}}};
    Hl2Backend backend;
    backend.applyRestoredState(stateWith(receivers, notches));
    Access::armReplay(backend);
    const RestoredRadioState captured = backend.currentOperatingState();
    check(captured.extension.value(QStringLiteral("receivers")).toArray().size() == 2,
          "both remembered receivers are kept");
    check(captured.extension.value(QStringLiteral("notches")).toArray().size() == 1,
          "the remembered notch is kept");
}

void theReplayStopsAtTheReceiverCeiling()
{
    QJsonArray receivers;
    for (int i = 0; i < 8; ++i) {
        receivers.append(receiverEntry(7'000'000.0 + 10'000.0 * i, QStringLiteral("LSB")));
    }
    Hl2Backend backend;
    backend.applyRestoredState(stateWith(receivers));
    check(Access::bringUp(backend, true), "the injected link edge reaches the backend");
    check(Access::receiverCount(backend) == Access::ceiling(backend),
          "no more receivers come back than this span allows");
    AetherSDR::test::spinUntil([&] { return Access::buildsSettled(backend); });
}

void withoutAConnectNothingIsReplayed()
{
    const QJsonArray receivers{receiverEntry(7'100'000.0, QStringLiteral("LSB")),
                               receiverEntry(14'074'000.0, QStringLiteral("USB"))};
    Hl2Backend backend;
    backend.applyRestoredState(stateWith(receivers));
    check(Access::bringUp(backend, false), "the injected link edge reaches the backend");
    check(Access::receiverCount(backend) == 1, "a link-up no connect armed replays nothing");
}

// TUNE power is recorded whether or not a carrier is up, under the band the
// transmit receiver is on, and a band change brings back the band's own value.
void tunePowerIsRememberedPerBand()
{
    Hl2Backend backend;
    QList<int> echoed;
    QObject::connect(&backend, &IRadioBackend::transmitChanged, &backend,
                     [&echoed](const TransmitDelta& d) {
                         if (d.tunePower) echoed << *d.tunePower;
                     });
    backend.setSliceFrequency(0, 7'100'000.0);
    backend.setTunePower(30);
    backend.setSliceFrequency(0, 14'200'000.0);
    check(echoed.isEmpty(), "a new band starts on the first-set baseline, which is the value already held");
    backend.setTunePower(55);
    backend.setSliceFrequency(0, 7'150'000.0);
    check(!echoed.isEmpty() && echoed.last() == 30, "back on 40 m, its own TUNE power is echoed");

    const QJsonObject tx = ext(backend.currentOperatingState(), "txSetpoints");
    const QJsonObject byBand = tx.value(QStringLiteral("tuneByBand")).toObject();
    check(byBand.value(QStringLiteral("40m")).toInt() == 30
              && byBand.value(QStringLiteral("20m")).toInt() == 55,
          "the capture holds TUNE power per band");
    check(tx.value(QStringLiteral("tuneDefaultPercent")).toInt() == 30,
          "the first value set is the baseline for bands never tuned");

    Hl2Backend next;
    QList<int> restored;
    QObject::connect(&next, &IRadioBackend::transmitChanged, &next,
                     [&restored](const TransmitDelta& d) {
                         if (d.tunePower) restored << *d.tunePower;
                     });
    RestoredRadioState state;
    state.extensionSchemaVersion = 1;
    state.extension.insert(QStringLiteral("txSetpoints"), tx);
    next.applyRestoredState(state);
    next.setSliceFrequency(0, 14'250'000.0);
    check(!restored.isEmpty() && restored.last() == 55,
          "a restored radio entering 20 m echoes 20 m's TUNE power");

    // Under a carrier the drive is the slider's value: no band memory moves it.
    restored.clear();
    Access::setTuning(next, true);
    next.setSliceFrequency(0, 7'100'000.0);
    check(restored.isEmpty(), "a band change under a TUNE carrier echoes no other TUNE power");
    Access::setTuning(next, false);
}

// Only an operator's choice of law or floor is kept; a law change takes the
// law's own floor with it.
void autoGainLawAndFloorAreKeptOnlyWhenChosen()
{
    Hl2Backend untouched;
    const QJsonObject plain = ext(untouched.currentOperatingState(), "rfGain");
    check(!plain.contains(QStringLiteral("autoLaw")) && !plain.contains(QStringLiteral("autoFloorDb")),
          "nothing is written for a law and floor nobody chose");

    Hl2Backend backend;
    check(backend.setLaw(QStringLiteral("ramp")), "the ramp law is accepted");
    backend.setFloorDb(10);
    const QJsonObject chosen = ext(backend.currentOperatingState(), "rfGain");
    check(chosen.value(QStringLiteral("autoLaw")).toString() == QStringLiteral("ramp")
              && chosen.value(QStringLiteral("autoFloorDb")).toInt() == 10,
          "a chosen law and floor are written");

    RestoredRadioState state;
    state.extensionSchemaVersion = 1;
    state.extension.insert(QStringLiteral("rfGain"), chosen);
    Hl2Backend next;
    next.applyRestoredState(state);
    check(next.law() == QStringLiteral("ramp") && next.floorDb() == 10,
          "a restore comes back on the chosen law and floor");

    next.setLaw(QStringLiteral("probe"));
    const QJsonObject relawed = ext(next.currentOperatingState(), "rfGain");
    check(relawed.value(QStringLiteral("autoLaw")).toString() == QStringLiteral("probe")
              && !relawed.contains(QStringLiteral("autoFloorDb")),
          "a new law drops the earlier floor choice");
    next.setLaw(QStringLiteral("default"));
    check(!ext(next.currentOperatingState(), "rfGain").contains(QStringLiteral("autoLaw")),
          "choosing the default clears the law choice");

    RestoredRadioState bad;
    bad.extensionSchemaVersion = 1;
    bad.extension.insert(QStringLiteral("rfGain"),
                         QJsonObject{{QStringLiteral("autoLaw"), QStringLiteral("bogus")},
                                     {QStringLiteral("autoFloorDb"), 999}});
    Hl2Backend fresh;
    Hl2Backend defaulted;
    defaulted.applyRestoredState(bad);
    check(defaulted.law() == fresh.law() && defaulted.floorDb() == fresh.floorDb(),
          "an unknown law and an out-of-range floor are dropped");
}

// The two new domains ride the document only where the backend declares them.
void theNewDomainsAreGatedByDeclaration()
{
    const RadioSettingsScope scope(QStringLiteral("hl2"), QStringLiteral("00:1C:C0:00:00:01"));
    Hl2Backend backend;
    RestoredRadioState state = backend.currentOperatingState();
    RadioCapabilities caps = backend.capabilities();
    check(caps.clientSettingsDomains.testFlag(RadioCapabilities::ClientSettingsDomain::Receivers)
              && caps.clientSettingsDomains.testFlag(RadioCapabilities::ClientSettingsDomain::Notches),
          "the HL2 declares the Receivers and Notches domains");
    check(caps.backendPanAveraging && caps.backendPanAveraging->clientPersistsAveraging,
          "the HL2 makes the client the owner of FFT AVG and Wt Avg");
    check(RadioStateMemory::store(scope, caps, state), "the document is stored");
    const RestoredRadioState loaded = RadioStateMemory::load(scope, caps);
    check(loaded.extension.contains(QStringLiteral("receivers"))
              && loaded.extension.contains(QStringLiteral("notches")),
          "declared, both lists round-trip through the store");

    caps.clientSettingsDomains &= ~RadioCapabilities::ClientSettingsDomains(
        RadioCapabilities::ClientSettingsDomain::Receivers);
    const RestoredRadioState narrowed = RadioStateMemory::load(scope, caps);
    check(!narrowed.extension.contains(QStringLiteral("receivers"))
              && narrowed.extension.contains(QStringLiteral("notches")),
          "an undeclared domain's list is withheld on load");
}

}   // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("hl2-session-memory"));
    if (!profile.isValid()) {
        std::fprintf(stderr, "Cannot isolate test settings\n");
        return 1;
    }
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();
    receiverSetpointsRoundTrip();
    invalidEntriesAreDroppedNotClamped();
    receiverAEntryOverridesTheFlatFields();
    theSessionIsReplayedOnceAfterLinkUp();
    transmitReturnsToItsReceiver();
    theReplayCountsReceiversAlreadyRunning();
    aCaptureBeforeTheReplayKeepsTheLists();
    theReplayStopsAtTheReceiverCeiling();
    withoutAConnectNothingIsReplayed();
    tunePowerIsRememberedPerBand();
    autoGainLawAndFloorAreKeptOnlyWhenChosen();
    theNewDomainsAreGatedByDeclaration();
    std::fprintf(stderr, "hl2_session_memory_test: %d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
