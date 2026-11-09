#include "TestSettingsProfile.h"
#include "core/backends/kenwood/KenwoodBackend.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"
#include "models/PanadapterModel.h"

#include <QCoreApplication>
#include <QMetaType>
#include <QHostAddress>
#include <QVariant>
#include <QPair>
#include <QStringList>
#include <QtEndian>

#include <cstdlib>
#include <cmath>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
#include <limits>
#include <utility>
#include <optional>

namespace AetherSDR::kenwood {
// Inject state and wire messages directly. No socket or synthetic firmware peer
// is created; these tests prove client lifecycle and publication behavior only.
struct KenwoodBackendTestAccess {
    static QVariantMap audioChronologyStatus(const KenwoodBackend& b) { return b.networkAudioLevelStatus(); }
    static void audioChronologyPublish(KenwoodBackend& b) { b.publishNetworkAudioLevels(); }
    static QVariantMap vfoChronologyStatus(const KenwoodBackend& b) { return b.vfoStatus(); }
    static QVariantMap chronologyStatus(const KenwoodBackend& b) { return b.nativeFilterStatus(); }
    static void chronologyPublish(KenwoodBackend& b) { b.publishNativeFilter(); }
    static void chronologyFeed(KenwoodBackend& b, const QByteArray& bytes) { b.adopt(bytes); }
    static void authenticate(KenwoodBackend& backend, bool administrator = false)
    {
        backend.m_clock.start();
        backend.m_loginAdministrator = administrator;
        backend.m_protocol.start("user", "password", administrator, 0);
        backend.m_protocol.receive("##CN1;", 1);
        backend.m_protocol.receive("##ID1;", 2);
        backend.ready();
    }
    static void antennaIntent(KenwoodBackend& b, int kind) {
        if (kind == 0) { b.m_keyIntent = true; }
        if (kind == 1) { b.m_cwIntent = true; }
        if (kind == 2) { b.m_atuIntent = true; }
        if (kind == 3) { b.m_tune.emplace(); }
    }
    static void replaceTransport(KenwoodBackend& backend, KenwoodBackend::Transport transport)
    {
        backend.m_transport = std::move(transport);
    }
    static QString validate(const RadioConnectRequest& request) { return KenwoodBackend::validateConnectRequest(request); }
    static void message(KenwoodBackend& backend, const QByteArray& bytes) { backend.adopt(bytes); }
    static void fail(KenwoodBackend& backend) { backend.fail(QStringLiteral("Injected failure")); }
    static void pumpAudio(KenwoodBackend& backend) { backend.pumpTxAudio(); }
    static void audio(KenwoodBackend& backend, const QByteArray& packet, qint64 now) { backend.adoptAudio(packet, now); }
    static qint64 nextSidetonePoll(const KenwoodBackend& backend) { return backend.m_nextCwPreferencesPollMs; }
    static void pollSidetone(KenwoodBackend& backend, qint64 now) { backend.pollCwPreferences(now); }
    static qint64 nextVfoPoll(const KenwoodBackend& backend) { return backend.m_nextVfoPollMs; }
    static void pollVfos(KenwoodBackend& backend, qint64 now) { backend.pollVfos(now); }
    static qint64 nextLevelPoll(const KenwoodBackend& backend) { return backend.m_nextVoipPollMs; }
    static void pollLevels(KenwoodBackend& backend, qint64 now) { backend.pollNetworkAudioLevels(now); }
    static qint64 nextPowerPoll(const KenwoodBackend& backend) { return backend.m_nextPowerPollMs; }
    static void pollPower(KenwoodBackend& backend, qint64 now) { backend.pollPowerLimits(now); }
    static qint64 nextInputPoll(const KenwoodBackend& backend) { return backend.m_nextInputPollMs; }
    static void pollInputs(KenwoodBackend& backend, qint64 now) { backend.pollTransmitInputs(now); }
    static qsizetype queuedSamples(const KenwoodBackend& backend) { return backend.m_txQueuedSamples + backend.m_txRemainder.size(); }
    static qint64 tuneDeadline(const KenwoodBackend& backend) { return backend.m_tune ? backend.m_tune->deadlineMs : -1; }
    static void pollTune(KenwoodBackend& backend, qint64 now) { backend.pollTunePreflight(now); }
    static bool tuning(const KenwoodBackend& backend) { return backend.m_tune.has_value(); }
    static TxCoordinator::Operation tuneOperation(const KenwoodBackend& backend) { return backend.m_tune->operation; }
    static void batch(KenwoodBackend& backend, const QList<QByteArray>& messages)
    {
        const quint64 batch = ++backend.m_controlIngressSequence;
        for (const QByteArray& message : messages) { backend.adopt(message, batch); }
    }
    static bool dataLan(const KenwoodBackend& backend) { return backend.m_dataSourceKnown && backend.m_dataLan; }
    static void control(KenwoodBackend& backend, const QByteArray& bytes)
    { backend.consume(backend.m_protocol.receive(bytes, backend.m_clock.elapsed())); }
};
}

namespace {
void require(bool value, const char* message)
{
    if (!value) { std::cerr << message << '\n'; std::exit(1); }
}
void verifyReceiveIntentRouting()
{
    using namespace AetherSDR;
    using namespace AetherSDR::kenwood;
    QList<QByteArray> commands;
    RadioModel radio;
    auto owned = std::make_unique<KenwoodBackend>(KenwoodBackend::Transport{
        [&](const QByteArray& bytes) { commands.append(bytes); return true; }, {}});
    KenwoodBackend* backend = owned.get();
    radio.setBackendForTest(std::move(owned), QStringLiteral("kenwood"));
    // Test injection wires normalized receiver state; mirror the production
    // meter relay here to verify identifiers through the real MeterModel.
    QObject::connect(backend, &IRadioBackend::meterDefined, &radio,
        [&](const MeterDef& definition) { radio.meterModel().defineMeter(definition); });
    QObject::connect(backend, &IRadioBackend::meterUpdate, &radio,
        [&](const QString& id, double value) {
            QString source, name;
            int index = -1;
            if (MeterModel::splitMeterId(id, &source, &name, &index)) {
                radio.meterModel().updateValueByName(source, name, static_cast<float>(value), index);
            }
        });
    KenwoodBackendTestAccess::authenticate(*backend);
    KenwoodBackendTestAccess::message(*backend, "ID024");
    KenwoodBackendTestAccess::message(*backend, "XV0");
    KenwoodBackendTestAccess::message(*backend, "FR0");
    KenwoodBackendTestAccess::message(*backend, "FA00014200000");
    KenwoodBackendTestAccess::message(*backend, "BS30");
    KenwoodBackendTestAccess::message(*backend, "BSK4");
    KenwoodBackendTestAccess::message(*backend, "BS43");
    require(radio.panadapters().size() == 1, "normalized geometry creates the real model pan");
    PanadapterModel* pan = radio.panadapters().first();
    require(radio.slice(0) != nullptr, "normalized radio observations create the real receive slice");
    require(radio.slice(0)->panId() == pan->panId(),
        "normalized receive slice belongs to its observed model pan");
    const double beforeTune = radio.slice(0)->frequency();
    commands.clear();
    require(radio.requestConfirmedReceiveTune(0, 14.201, IRadioBackend::ReceiveTuneView::Reveal),
        "confirmed model tune admits the typed backend verb");
    require(commands == QList<QByteArray>{"FA00014201000;", "FA;"}, "typed tune writes selected VFO and requests readback");
    require(radio.slice(0)->frequency() == beforeTune, "tune admission cannot advance live frequency");
    KenwoodBackendTestAccess::message(*backend, "FA00014201000");
    require(std::abs(radio.slice(0)->frequency() - 14.201) < 1e-10, "frequency advances only on readback");
    KenwoodBackendTestAccess::message(*backend, "FB00014201000");
    require(radio.requestConfirmedReceiveTune(0, 14.202, IRadioBackend::ReceiveTuneView::Preserve),
        "first VFO has a pending tune intent");
    require(std::abs(radio.slice(0)->incrementalTuneTarget(0.001) - 14.203) < 1e-10,
        "wheel cursor uses pending first-VFO target");
    KenwoodBackendTestAccess::message(*backend, "FR1");
    require(std::abs(radio.slice(0)->incrementalTuneTarget(0.001) - 14.202) < 1e-10,
        "same-frequency receive VFO role change retires first-VFO pending target");
    require(radio.requestConfirmedReceiveTune(0, 14.202, IRadioBackend::ReceiveTuneView::Preserve),
        "second VFO can admit a new pending tune");
    KenwoodBackendTestAccess::message(*backend, "FR1");
    require(std::abs(radio.slice(0)->incrementalTuneTarget(0.001) - 14.203) < 1e-10,
        "repeated unchanged role status preserves current-VFO pending target");
    commands.clear();
    radio.requestPanBandwidth(pan->panId(), 0.027);
    require(commands == QList<QByteArray>{"BS42;", "BS4;"}, "small zoom progresses to adjacent narrower discrete span");
    require(std::abs(pan->bandwidthMhz() - 0.03) < 1e-10, "span remains confirmed until readback");
    KenwoodBackendTestAccess::message(*backend, "BS42");
    require(std::abs(pan->bandwidthMhz() - 0.02) < 1e-10, "span readback updates model");
    commands.clear();
    radio.requestPanCenter(pan->panId(), 14.2, std::nextafter(0.02, 1.0));
    require(commands == QList<QByteArray>{"BS42;", "BS4;"}, "same-span floating roundoff cannot zoom");
    commands.clear();
    radio.requestPanBandwidth(pan->panId(), 0.025);
    require(commands == QList<QByteArray>{"BS43;", "BS4;"}, "25 kHz tie zoom selects30 kHz from confirmed20 kHz");
    KenwoodBackendTestAccess::message(*backend, "BS41");
    commands.clear();
    radio.requestPanBandwidth(pan->panId(), 0.015);
    require(commands == QList<QByteArray>{"BS42;", "BS4;"}, "small wider zoom progresses beyond nearest10k");
    commands.clear();
    require(!backend->requestReceiveTune(1, 14200000, IRadioBackend::ReceiveTuneView::Preserve)
        && !backend->requestReceiveTune(0, std::numeric_limits<double>::quiet_NaN(), IRadioBackend::ReceiveTuneView::Center)
        && commands.isEmpty(), "invalid typed receive intents emit no command");
    KenwoodBackendTestAccess::message(*backend, "BS31");
    KenwoodBackendTestAccess::message(*backend, "BS51");
    KenwoodBackendTestAccess::message(*backend, "BSM01400000014500000");
    const double fixedCenter = pan->centerMhz();
    const double fixedWidth = pan->bandwidthMhz();
    for (const auto view : {IRadioBackend::ReceiveTuneView::Preserve,
                           IRadioBackend::ReceiveTuneView::Reveal,
                           IRadioBackend::ReceiveTuneView::Center}) {
        commands.clear();
        require(radio.requestConfirmedReceiveTune(0, 14.202, view), "Fixed scope admits VFO tune");
        require(commands == QList<QByteArray>{"FB00014202000;", "FB;"},
            "Fixed scope tuning only writes VFO and reads it back");
        KenwoodBackendTestAccess::message(*backend, "FB00014202000");
        require(pan->centerMhz() == fixedCenter && pan->bandwidthMhz() == fixedWidth,
            "Fixed waterfall bounds stay unchanged while VFO marker moves");
    }
    KenwoodBackendTestAccess::message(*backend, "SM0035");
    require(radio.meterModel().value(100) == 35 && radio.meterModel().valueAgeMs(100) >= 0,
        "raw meter source-name identifier reaches the real meter store");
    // PC Command Reference (2019-01-30), printed 62: SQ has one 000–255 parameter.
    KenwoodBackendTestAccess::message(*backend, "SQ000");
    SliceModel* slice = radio.slice(0);
    require(slice->squelchStateKnown() && !slice->squelchOn() && slice->squelchLevel() == 0,
        "native zero squelch readback reaches the confirmed slice");
    commands.clear();
    slice->setSquelch(true, 50);
    require(commands == QList<QByteArray>{"SQ128;", "SQ;"},
        "production slice squelch route uses one three-digit native level without a selector");
    require(!slice->squelchOn() && slice->squelchLevel() == 0,
        "squelch intent cannot replace confirmed state before native readback");
    KenwoodBackendTestAccess::message(*backend, "SQ128");
    require(slice->squelchOn() && slice->squelchLevel() == 50,
        "representative native squelch level converges through the production model route");
    commands.clear();
    slice->setSquelch(true, 100);
    require(commands == QList<QByteArray>{"SQ255;", "SQ;"} && slice->squelchLevel() == 50,
        "maximum squelch intent encodes native255 and waits for readback");
    KenwoodBackendTestAccess::message(*backend, "SQ255");
    require(slice->squelchOn() && slice->squelchLevel() == 100,
        "maximum native squelch readback maps to100percent");
    for (const QByteArray& malformed : {QByteArray("SQ256"), QByteArray("SQ-01"),
             QByteArray("SQ00"), QByteArray("SQ0000")}) {
        KenwoodBackendTestAccess::message(*backend, malformed);
        require(slice->squelchOn() && slice->squelchLevel() == 100,
            "malformed or out-of-range squelch observations preserve confirmed state");
    }
    commands.clear();
    slice->setSquelch(false, 100);
    require(commands == QList<QByteArray>{"SQ000;", "SQ;"} && slice->squelchOn(),
        "manual squelch disable sends zero and waits for confirmed observation");
    KenwoodBackendTestAccess::message(*backend, "SQ000");
    require(!slice->squelchOn() && slice->squelchLevel() == 0,
        "native disable readback converges without inventing a separate enable bit");

}

void verifyNativeGainMuteRoutes()
{
    using namespace AetherSDR;
    using namespace AetherSDR::kenwood;
    QList<QByteArray> commands;
    int audioGainPublications = 0;
    int rfGainPublications = 0;
    int mutePublications = 0;
    RadioModel radio;
    require(radio.rebuildBackendForTest(QStringLiteral("kenwood")),
        "production family factory constructs the native Kenwood backend");
    KenwoodBackend* backend = static_cast<KenwoodBackend*>(radio.backend());
    KenwoodBackendTestAccess::replaceTransport(*backend, KenwoodBackend::Transport{
        [&commands](const QByteArray& bytes) { commands.append(bytes); return true; }, {}});
    QObject::connect(backend, &IRadioBackend::sliceChanged, &radio,
        [&](int, const SliceDelta& delta) {
            if (delta.audioGain.has_value()) { ++audioGainPublications; }
            if (delta.audioMute.has_value()) { ++mutePublications; }
        });
    QObject::connect(backend, &IRadioBackend::panRfGainChanged, &radio,
        [&](const QString&, int) { ++rfGainPublications; });
    KenwoodBackendTestAccess::authenticate(*backend);
    for (const QByteArray& message : {QByteArray("ID024"), QByteArray("XV0"), QByteArray("RX"), QByteArray("FR0"),
             QByteArray("FA00014200000"), QByteArray("BS30"), QByteArray("BSK4"), QByteArray("BS43")}) {
        KenwoodBackendTestAccess::message(*backend, message);
    }
    require(radio.slice(0) && radio.panadapters().size() == 1,
        "production backend wiring creates the native receiver and pan");
    SliceModel* slice = radio.slice(0);
    PanadapterModel* pan = radio.panadapters().first();
    KenwoodBackendTestAccess::message(*backend, "AG000");
    KenwoodBackendTestAccess::message(*backend, "RG000");
    KenwoodBackendTestAccess::message(*backend, "MU0");
    for (const QPair<int, QByteArray>& level : {
             QPair<int, QByteArray>{0, "000"}, {50, "128"}, {100, "255"}}) {
        const float audioBefore = slice->audioGain();
        commands.clear();
        slice->setAudioGain(level.first);
        require(commands == QList<QByteArray>{"AG" + level.second + ';', "AG;"},
            "production slice AF gain uses the native single-parameter level and query");
        require(slice->audioGain() == audioBefore, "AF request waits for native readback");
        KenwoodBackendTestAccess::message(*backend, "AG" + level.second);
        require(slice->audioGain() == level.first, "native AF readback converges to percent model value");
        const int rfBefore = pan->rfGain();
        commands.clear();
        radio.setPanRfGainFor(pan->panId(), level.first);
        require(commands == QList<QByteArray>{"RG" + level.second + ';', "RG;"},
            "production pan RF gain uses the native single-parameter level and query");
        require(pan->rfGain() == rfBefore, "RF request waits for native readback");
        KenwoodBackendTestAccess::message(*backend, "RG" + level.second);
        require(pan->rfGain() == level.first, "native RF readback converges to percent model value");
    }
    const int gainPublicationsBeforeInvalid = audioGainPublications;
    const int rfPublicationsBeforeInvalid = rfGainPublications;
    commands.clear();
    for (const QByteArray& invalid : {QByteArray("256"), QByteArray("-01"),
             QByteArray("00"), QByteArray("0000")}) {
        KenwoodBackendTestAccess::message(*backend, "AG" + invalid);
        KenwoodBackendTestAccess::message(*backend, "RG" + invalid);
        require(slice->audioGain() == 100 && pan->rfGain() == 100,
            "malformed gain replies preserve confirmed AF and RF values");
        require(audioGainPublications == gainPublicationsBeforeInvalid
            && rfGainPublications == rfPublicationsBeforeInvalid && commands.isEmpty(),
            "invalid native gain replies produce neither normalized publications nor outgoing commands");
    }
    for (const bool muted : {true, false}) {
        const bool beforeMute = slice->audioMute();
        commands.clear();
        slice->setAudioMute(muted);
        require(commands == QList<QByteArray>{muted ? "MU1;" : "MU0;", "MU;"},
            "production slice mute uses the explicit native boolean and query");
        require(slice->audioMute() == beforeMute, "mute request waits for native readback");
        KenwoodBackendTestAccess::message(*backend, muted ? "MU1" : "MU0");
        require(slice->audioMute() == muted, "native mute readback converges to the model");
    }
    const int mutePublicationsBeforeInvalid = mutePublications;
    commands.clear();
    KenwoodBackendTestAccess::message(*backend, "MU2");
    KenwoodBackendTestAccess::message(*backend, "MU10");
    require(!slice->audioMute() && mutePublications == mutePublicationsBeforeInvalid && commands.isEmpty(),
        "malformed mute reports produce neither fabricated state nor normalized publications or commands");
}

// Instruction Manual EN-7-3 and the independent FV1.01 USB edge captures in
// docs/architecture/kenwood-ts890-rx-parity/hardware-span-2026-11-03.json.
// This fixture qualifies the measured voice bank, not other firmware/modes.
void verifyMeasuredScopeSpans()
{
    using namespace AetherSDR;
    using namespace AetherSDR::kenwood;
    QList<QByteArray> commands;
    KenwoodBackend backend(KenwoodBackend::Transport{
        [&](const QByteArray& bytes) { commands.append(bytes); return true; }, {}});
    double width = 0;
    int geometryUpdates = 0;
    QObject::connect(&backend, &IRadioBackend::panCenterBandwidthChanged, &backend,
        [&](const QString&, double, double bandwidth) { width = bandwidth; ++geometryUpdates; });
    KenwoodBackendTestAccess::authenticate(backend);
    for (const QByteArray& message : {QByteArray("ID024"), QByteArray("XV0"), QByteArray("FR0"), QByteArray("FT0"),
         QByteArray("TS0"), QByteArray("RX"), QByteArray("SF0000142699002"),
         QByteArray("BS30"), QByteArray("BSK4"), QByteArray("BSO0")}) {
        KenwoodBackendTestAccess::message(backend, message);
    }
    const QList<int> measuredHz{5000, 10000, 20000, 30000, 50000, 100000, 200000, 500000};
    for (int index = 0; index < measuredHz.size(); ++index) {
        const int previous = (index + 1) % measuredHz.size();
        KenwoodBackendTestAccess::message(backend, "BS4" + QByteArray::number(previous));
        const double before = width;
        const int beforeUpdates = geometryUpdates;
        commands.clear();
        backend.setPanBandwidth(QStringLiteral("0"), measuredHz[index]);
        require(commands == QList<QByteArray>{"BS4" + QByteArray::number(index) + ';', "BS4;"},
            "all eight measured spans encode the correct BS4 index and query");
        require(width == before && geometryUpdates == beforeUpdates,
            "native span dispatch cannot optimistically change published geometry");
        KenwoodBackendTestAccess::message(backend, "BS4" + QByteArray::number(index));
        require(std::abs(width * 1e6 - measuredHz[index]) < 0.001,
            "all eight BS4 readbacks publish the independently measured width");
    }
    const double maximum = width;
    const int updates = geometryUpdates;
    for (const QByteArray& invalid : {QByteArray("BS48"), QByteArray("BS49"), QByteArray("BS4/"),
         QByteArray("BS4A"), QByteArray("BS470"), QByteArray("BS4")}) {
        KenwoodBackendTestAccess::message(backend, invalid);
    }
    require(width == maximum && geometryUpdates == updates,
        "index8 and malformed span reports cannot replace accepted index7 geometry");
    commands.clear();
    for (double invalid : {4999.0, 500001.0, std::numeric_limits<double>::quiet_NaN(),
         std::numeric_limits<double>::infinity()}) {
        backend.setPanBandwidth(QStringLiteral("0"), invalid);
    }
    require(commands.isEmpty(), "out-of-range and nonfinite span requests never reach transport");
    KenwoodBackendTestAccess::message(backend, "BS42");
    commands.clear();
    backend.setPanBandwidth(QStringLiteral("0"), 25000);
    require(commands == QList<QByteArray>{"BS43;", "BS4;"} && std::abs(width - 0.02) < 1e-10,
        "25 kHz halfway request zooms20-to30 without an invented25 kHz span");
    KenwoodBackendTestAccess::message(backend, "BS43");
    commands.clear();
    backend.setPanBandwidth(QStringLiteral("0"), 25000);
    require(commands == QList<QByteArray>{"BS42;", "BS4;"} && std::abs(width - 0.03) < 1e-10,
        "25 kHz halfway request zooms30-to20 in the reverse direction");
    KenwoodBackendTestAccess::message(backend, "BS31");
    KenwoodBackendTestAccess::message(backend, "BS51");
    KenwoodBackendTestAccess::message(backend, "BSM01400000014500000");
    KenwoodBackendTestAccess::message(backend, "BS47");
    require(std::abs(width - 0.5) < 1e-10, "BS4 readback cannot replace authoritative Fixed bounds");
    commands.clear();
    backend.setPanBandwidth(QStringLiteral("0"), 25000);
    require(commands == QList<QByteArray>{"BSM11423750014262500;", "BSM0;"}
        && std::abs(width - 0.5) < 1e-10,
        "Fixed zoom keeps its independent25 kHz bounds request and waits for readback");
    KenwoodBackendTestAccess::message(backend, "BSM01423750014262500");
    require(std::abs(width - 0.025) < 1e-10, "Fixed bounds retain valid25 kHz geometry independently of BS4 steps");
}

void verifyTransmitInputs(QCoreApplication& application)
{
    using namespace AetherSDR;
    using namespace AetherSDR::kenwood;
    QList<QByteArray> commands;
    KenwoodBackend backend(KenwoodBackend::Transport{
        [&](const QByteArray& bytes) { commands.append(bytes); return true; }, [](const QByteArray&) { return true; }});
    QVariantMap state;
    int sourceWarnings = 0;
    QObject::connect(&backend, &IRadioBackend::configurationWarning, &application,
        [&](const QString& warning) { if (warning.startsWith("PC Audio requires")) { ++sourceWarnings; } });
    QObject::connect(&backend, &IRadioBackend::extensionStatus, &application,
        [&](const QString&, const QString& kind, const QVariantMap& status) { if (kind == "inputs") { state = status; } });
    QObject::connect(&backend, &IRadioBackend::extensionResult, &application,
        [&](quint64 id, const QVariant& result) { if (id == 810) { state = result.toMap(); } });
    KenwoodBackendTestAccess::authenticate(backend);
    backend.invokeExtension("kenwood", "inputs.get", 810);
    require(!state.value("editable").toBool() && !state.contains("send") && !state.contains("data"), "native input state starts unknown");
    QString context = state.value("context").toString();
    const auto edit = [&](const QString& path, const QVariant& selection, const QString& session) {
        backend.invokeExtension("kenwood", "inputs.set", 811,
            QVariantMap{{"path", path}, {"selection", selection}, {"context", session}});
    };
    commands.clear();
    edit("send", 0, context);
    require(commands.isEmpty(), "native input edit requires identity and known RX");
    KenwoodBackendTestAccess::message(backend, "ID024");
    AetherSDR::kenwood::KenwoodBackendTestAccess::message(backend, "XV0");
    require(commands.contains("MS0;") && commands.contains("MS1;"), "startup reads both independent input paths");
    for (const QByteArray& command : commands) {
        if (command.startsWith("MS")) { require(command.size() == 4, "startup never restores input selections"); }
    }
    commands.clear();
    const qint64 pollAt = KenwoodBackendTestAccess::nextInputPoll(backend);
    KenwoodBackendTestAccess::pollInputs(backend, pollAt - 1);
    require(commands.isEmpty(), "native inputs are not polled early");
    KenwoodBackendTestAccess::pollInputs(backend, pollAt);
    KenwoodBackendTestAccess::pollInputs(backend, pollAt);
    require(commands == QList<QByteArray>{"MS0;", "MS1;"}, "bounded input polling reads both paths once");
    commands.clear();
    KenwoodBackendTestAccess::message(backend, "MS010");
    KenwoodBackendTestAccess::message(backend, "MS103");
    require(state.value("send").toInt() == 0 && state.value("data").toInt() == 3, "front microphone and DATA LAN are separate readbacks");
    edit("send", 6, context);
    require(commands.isEmpty(), "readback alone cannot qualify unknown transmitter state");
    KenwoodBackendTestAccess::message(backend, "RX");
    require(state.value("editable").toBool(), "actual RX enables native input configuration");
    for (const QByteArray& invalid : {QByteArray("MS000"), QByteArray("MS199"), QByteArray("MS1130")}) { KenwoodBackendTestAccess::message(backend, invalid); }
    require(state.value("send").toInt() == 0 && state.value("data").toInt() == 3, "invalid input reports do not replace live state");
    for (const QVariant& value : {QVariant(-1), QVariant(7), QVariant(true), QVariant("3"), QVariant(3.5), QVariant(std::numeric_limits<double>::quiet_NaN())}) { edit("data", value, context); }
    edit("unknown", 0, context);
    edit("send", 0, context + "stale");
    require(commands.isEmpty(), "input boundary rejects invalid values, paths and stale sessions");
    KenwoodBackendTestAccess::message(backend, "##TI1");
    KenwoodBackendTestAccess::message(backend, "##UE1");
    TxCoordinator coordinator([](const TxCoordinator::Operation&, TxCoordinator::StopReason) {});
    const TxCoordinator::Actor actor = coordinator.registerActor({true, 0});
    const TxCoordinator::Operation operation = coordinator.acquire(actor, TxCoordinator::monotonicMs()).operation;
    edit("send", 6, context);
    require(commands == QList<QByteArray>{"MS013;", "MS0;"} && !state.contains("send") && state.value("data").toInt() == 3,
        "explicit complete SEND pair waits for readback without altering DATA");
    commands.clear();
    backend.setKeying(true, operation);
    edit("send", 0, context);
    require(commands.isEmpty(), "pending ordinary input blocks keying and duplicate edits");
    KenwoodBackendTestAccess::message(backend, "MS013");
    backend.setKeying(true, operation);
    require(commands == QList<QByteArray>{"TX0;"}, "ordinary keying resumes only after native input readback");
    commands.clear();
    edit("data", 0, context);
    require(commands.isEmpty() && !state.value("editable").toBool(), "local key intent locks input edits before TX status");
    backend.setKeying(false, operation);
    commands.clear();
    edit("data", 0, context);
    require(commands.isEmpty(), "stop handoff cannot qualify input editing as idle");
    KenwoodBackendTestAccess::message(backend, "RX");
    backend.observePcAudioRouting(true);
    backend.setKeying(true, operation);
    const TxCoordinator::Producer producer = coordinator.registerProducer();
    const TxCoordinator::Context media = coordinator.mediaContext(producer, operation);
    backend.submitTxAudio(QByteArray(480 * 4, '\0'), 24000, TxAudioSource::Microphone, media);
    require(KenwoodBackendTestAccess::queuedSamples(backend) > 0, "fixture queues qualified network PCM");
    backend.requestPcAudioRouting(true);
    require(KenwoodBackendTestAccess::queuedSamples(backend) == 0 && !KenwoodBackendTestAccess::dataLan(backend),
        "explicit PC input reassertion retires queued PCM and old LAN qualification immediately");
    KenwoodBackendTestAccess::message(backend, "MS103");
    backend.setKeying(false, operation);
    KenwoodBackendTestAccess::message(backend, "RX");
    commands.clear();
    edit("data", 0, context);
    require(commands == QList<QByteArray>{"MS110;", "MS1;"} && !state.contains("data")
        && state.value("send").toInt() == 6 && !KenwoodBackendTestAccess::dataLan(backend),
        "DATA edit retires PCM and eligibility before write, preserving SEND");
    commands.clear();
    backend.setKeying(true, operation);
    require(commands.isEmpty(), "pending DATA request cannot key from a previously cached LAN source");
    KenwoodBackendTestAccess::message(backend, "MS110");
    const int warningsAfterChange = sourceWarnings;
    KenwoodBackendTestAccess::message(backend, "MS110");
    require(warningsAfterChange > 0 && sourceWarnings == warningsAfterChange, "periodic unchanged input readback does not repeat a source warning");
    backend.setKeying(true, operation);
    require(commands.isEmpty(), "confirmed microphone DATA source does not qualify host PCM keying");
    backend.requestPcAudioRouting(true);
    commands.clear();
    backend.setKeying(true, operation);
    require(commands.isEmpty(), "explicit PC input reassertion also waits for readback");
    KenwoodBackendTestAccess::message(backend, "MS103");
    backend.setKeying(true, operation);
    require(commands == QList<QByteArray>{"TX1;"}, "PC Audio selects pure DATA LAN and keys only after confirmation");
    backend.setKeying(false, operation);
    KenwoodBackendTestAccess::message(backend, "RX");
    KenwoodBackendTestAccess::message(backend, "TX0");
    commands.clear();
    edit("send", 0, context);
    require(commands.isEmpty() && !state.value("editable").toBool(), "front panel TX locks native input writes");
    backend.disconnectRadio();
    KenwoodBackendTestAccess::authenticate(backend);
    KenwoodBackendTestAccess::message(backend, "ID024");
    AetherSDR::kenwood::KenwoodBackendTestAccess::message(backend, "XV0");
    KenwoodBackendTestAccess::message(backend, "RX");
    backend.invokeExtension("kenwood", "inputs.get", 810);
    require(!state.contains("send") && !state.contains("data") && state.value("context").toString() != context, "reconnect retires native selections and session context");
    commands.clear();
    KenwoodBackendTestAccess::message(backend, "MS010");
    edit("send", 6, context);
    require(commands.isEmpty(), "old input edit cannot cross reconnect");
    KenwoodBackendTestAccess::message(backend, "MS103");
    backend.invokeExtension("kenwood", "inputs.get", 810);
    context = state.value("context").toString();
    int retiredResults = 0;
    QObject::connect(&backend, &IRadioBackend::extensionResult, &application,
        [&](quint64 id, const QVariant&) { if (id == 811) { ++retiredResults; } });
    const QMetaObject::Connection retirement = QObject::connect(&backend, &IRadioBackend::extensionStatus, &application,
        [&](const QString&, const QString& kind, const QVariantMap& status) {
            if (kind == "inputs" && !status.contains("data")) { backend.disconnectRadio(); }
        });
    commands.clear();
    edit("data", 0, context);
    require(!backend.isConnected() && retiredResults == 0, "input publication reentrancy retires stale completion");
    QObject::disconnect(retirement);
    QList<QByteArray> retiredCommands;
    std::unique_ptr<KenwoodBackend> retiring;
    bool retireOnWrite = false;
    retiring = std::make_unique<KenwoodBackend>(KenwoodBackend::Transport{
        [&](const QByteArray& bytes) {
            retiredCommands.append(bytes);
            if (retireOnWrite && bytes == "MS013;") { retiring.reset(); }
            return true;
        }, {}});
    QString retiringContext;
    QObject::connect(retiring.get(), &IRadioBackend::extensionResult, &application,
        [&](quint64, const QVariant& result) { retiringContext = result.toMap().value("context").toString(); });
    KenwoodBackendTestAccess::authenticate(*retiring);
    KenwoodBackendTestAccess::message(*retiring, "ID024");
    KenwoodBackendTestAccess::message(*retiring, "XV0");
    KenwoodBackendTestAccess::message(*retiring, "RX");
    KenwoodBackendTestAccess::message(*retiring, "MS010");
    retiring->invokeExtension("kenwood", "inputs.get", 812);
    retiredCommands.clear();
    retireOnWrite = true;
    retiring->invokeExtension("kenwood", "inputs.set", 813,
        QVariantMap{{"path", "send"}, {"selection", 6}, {"context", retiringContext}});
    require(!retiring && retiredCommands == QList<QByteArray>{"MS013;"},
        "destruction during input write cannot send follow-up queries on a retired owner");
}
class TuneFixture final {
public:
    using Backend = AetherSDR::kenwood::KenwoodBackend;
    using Access = AetherSDR::kenwood::KenwoodBackendTestAccess;
    QList<QByteArray> commands;
    QList<bool> observedMox;
    int idleObservations = 0;
    int completed = 0;
    int retired = 0;
    std::optional<AetherSDR::TxCoordinator::LocalIntentEnd> retirementPhase;
    int audioPackets = 0;
    std::function<void(const QByteArray&)> onCommand;
    AetherSDR::TxCoordinator coordinator{[](const AetherSDR::TxCoordinator::Operation&, AetherSDR::TxCoordinator::StopReason) {}};
    AetherSDR::TxCoordinator::Actor actor;
    AetherSDR::TxCoordinator::Operation operation;
    AetherSDR::TxCoordinator::Intent intent;
    std::unique_ptr<Backend> backend;

    explicit TuneFixture(QCoreApplication& application)
    {
        using namespace AetherSDR;
        backend = std::make_unique<Backend>(Backend::Transport{
            [this](const QByteArray& bytes) {
                commands.append(bytes);
                if (onCommand) { onCommand(bytes); }
                return true;
            }, [this](const QByteArray&) { ++audioPackets; return true; }});
        QObject::connect(backend.get(), &IRadioBackend::transmitChanged, &application,
            [this](const TransmitDelta& delta) { if (delta.mox) { observedMox.append(*delta.mox); } });
        QObject::connect(backend.get(), &IRadioBackend::keyingStateConfirmed, &application,
            [this](bool keyed) { if (!keyed) { ++idleObservations; } });
        Access::authenticate(*backend);
        for (const QByteArray& message : {QByteArray("ID024"), QByteArray("XV0"), QByteArray("##TI1"), QByteArray("##UE1"),
             QByteArray("FR0"), QByteArray("FT0"), QByteArray("SF0000140740002"), QByteArray("AN1000"),
             QByteArray("XV0"), QByteArray("XT0"), QByteArray("RF00000"), QByteArray("LP20"),
             QByteArray("LP1505010"), QByteArray("RX")}) { feed(message); }
        require(backend->capabilities().hasCwTune && backend->capabilities().hasRadioPttReadback
            && backend->capabilities().tuneCarrierControl->modes.contains("CW"), "qualified native carrier declares original-intent retirement and physical PTT readback");
        actor = coordinator.registerActor({true, 0});
        operation = coordinator.acquire(actor, TxCoordinator::monotonicMs()).operation;
        intent = coordinator.beginIntent(operation, {}, TxCoordinator::Activity::Tune);
        operation = coordinator.bindLocalIntentRetirement(operation, intent, [this](TxCoordinator::LocalIntentEnd phase) {
            ++retired;
            retirementPhase = phase;
            (void)coordinator.endIntent(intent);
        });
        commands.clear();
        observedMox.clear();
        idleObservations = 0;
    }
    void feed(const QByteArray& message) { Access::message(*backend, message); }
    void start(int watts = 10)
    {
        backend->setTune(true, watts, operation, AetherSDR::TxCoordinator::Completion([this] { ++completed; }));
    }
    void fresh()
    {
        for (const QByteArray& message : {QByteArray("FT0"), QByteArray("SF0000140740002"), QByteArray("AN1000"),
             QByteArray("XV0"), QByteArray("XT0"), QByteArray("RF00000"), QByteArray("LP20")}) { feed(message); }
    }
};

void verifyTunePreflight(QCoreApplication& application)
{
    using namespace AetherSDR;
    using Access = kenwood::KenwoodBackendTestAccess;
    {
        TuneFixture fixture(application);
        fixture.backend->setTune(true, 10, {});
        require(fixture.commands.isEmpty(), "native TUNE cannot preflight without original TX authority");
        fixture.start();
        require(fixture.completed == 0 && fixture.commands.contains("FT;") && fixture.commands.contains("RF;")
            && fixture.commands.contains("LP2;") && !fixture.commands.contains("TX2;"), "TUNE retains completion and refreshes target before any carrier");
        const qint64 deadline = Access::tuneDeadline(*fixture.backend);
        fixture.backend->setTune(true, 10, fixture.operation);
        require(Access::tuneDeadline(*fixture.backend) == deadline, "repeat TUNE input cannot renew pending authorization or deadline");
        fixture.feed("LP1505010");
        require(!fixture.commands.contains("TX2;"), "cached or premature power does not replace fresh target admission");
        Access::batch(*fixture.backend, {"FT0", "SF0000140740002", "AN1000", "XV0", "XT0", "RF00000", "LP20", "LP1505010"});
        require(!fixture.commands.contains("TX2;") && fixture.completed == 0, "coalesced old power cannot answer a query sent while draining the same batch");
        fixture.feed("LP1505010");
        require(fixture.commands.count("TX2;") == 1 && fixture.completed == 1 && fixture.observedMox.isEmpty()
            && fixture.idleObservations == 0, "fresh power dispatches one gated TX2 without synthesizing key or idle observation");
        fixture.backend->observePcAudioRouting(true);
        fixture.feed("MS103");
        const TxCoordinator::Producer producer = fixture.coordinator.registerProducer();
        fixture.backend->submitTxAudio(QByteArray(480 * 4, '\0'), 24000, TxAudioSource::Microphone,
            fixture.coordinator.mediaContext(producer, fixture.operation));
        Access::pumpAudio(*fixture.backend);
        require(fixture.audioPackets == 0, "native carrier never admits microphone PCM from the PTT latch");
        fixture.feed("TX2");
        require(fixture.observedMox == QList<bool>{true}, "only native TX2 readback establishes observed transmit");
        fixture.backend->setTune(false, 10, fixture.operation);
        require(fixture.commands.last() == "RX;" && fixture.observedMox == QList<bool>{true}
            && fixture.idleObservations == 0, "carrier stop handoff does not declare idle");
        fixture.feed("RX");
        require(fixture.observedMox == QList<bool>{true, false} && fixture.idleObservations == 1,
            "actual RX reconciles the stopped native carrier");
    }
    {
        TuneFixture fixture(application);
        fixture.start(15);
        fixture.fresh();
        fixture.feed("LP1505010");
        require(fixture.commands.contains("LP1505015;") && !fixture.commands.contains("TX2;") && fixture.completed == 0,
            "TUNE writes only its separate requested native tuning-power row before keying");
        for (const QByteArray& command : fixture.commands) {
            require(!command.startsWith("PC") && command != "LP20;" && command != "LP21;", "preflight cannot rewrite ordinary RF power or the global limiter");
        }
        fixture.feed("LP1505015");
        require(fixture.commands.count("TX2;") == 1 && fixture.completed == 1, "requested native power must answer before TX2");
    }
    {
        TuneFixture fixture(application);
        fixture.start(15);
        fixture.fresh();
        fixture.feed("LP1505010");
        fixture.feed("LP1505014");
        fixture.feed("LP1505015");
        require(!fixture.commands.contains("TX2;") && fixture.commands.count("LP1505015;") == 1
            && fixture.completed == 1 && !Access::tuning(*fixture.backend), "mismatched power refuses without retry; late confirmation cannot revive it");
    }
    for (const QByteArray& changed : {QByteArray("FT1"), QByteArray("FT3"), QByteArray("FA00014200000"),
         QByteArray("SF0000140740003"), QByteArray("OM03"), QByteArray("XV1"), QByteArray("XT1"),
         QByteArray("AN1010"), QByteArray("LP21"), QByteArray("TX0"), QByteArray("TX2"), QByteArray("?"), QByteArray("##TI0")}) {
        TuneFixture fixture(application);
        fixture.start();
        fixture.feed(changed);
        fixture.feed("LP1505010");
        require(!fixture.commands.contains("TX2;") && fixture.completed == 1 && !Access::tuning(*fixture.backend),
            "target, source, account, unsolicited TX or rejection change retires pending TUNE");
    }
    {
        TuneFixture fixture(application);
        fixture.start();
        fixture.fresh();
        const qint64 deadline = Access::tuneDeadline(*fixture.backend);
        Access::pollTune(*fixture.backend, deadline);
        fixture.feed("LP1505010");
        require(!fixture.commands.contains("TX2;") && fixture.completed == 1 && fixture.idleObservations == 0,
            "preflight deadline retires completion without a carrier or invented idle sample");
    }
    {
        TuneFixture fixture(application);
        fixture.start();
        fixture.fresh();
        fixture.backend->setTune(false, 10, fixture.operation);
        fixture.feed("LP1505010");
        require(!fixture.commands.contains("TX2;") && !fixture.commands.contains("RX;") && fixture.completed == 1,
            "release before carrier handoff cancels queued TUNE without a state-changing RX command");
    }
    {
        TuneFixture fixture(application);
        fixture.start();
        fixture.fresh();
        fixture.coordinator.reset();
        fixture.feed("LP1505010");
        require(!fixture.commands.contains("TX2;") && fixture.completed == 1, "old operation cannot key on a later power response");
    }
    {
        TuneFixture fixture(application);
        fixture.start();
        fixture.backend->setSliceFrequency(0, 14200000);
        fixture.feed("LP1505010");
        require(!Access::tuning(*fixture.backend) && fixture.commands.contains("FA00014200000;")
            && !fixture.commands.contains("TX2;") && fixture.completed == 1, "explicit TX VFO retuning retires preflight before sending the frequency write");
    }
    {
        TuneFixture fixture(application);
        fixture.start();
        fixture.fresh();
        fixture.feed("LP1505010");
        fixture.feed("TX2");
        fixture.feed("FA00014200000");
        require(fixture.commands.count("RX;") == 1 && !Access::tuning(*fixture.backend)
            && fixture.observedMox == QList<bool>{true}, "active target changes request cleanup without asserting idle");
    }
    {
        TuneFixture fixture(application);
        fixture.start();
        fixture.backend->disconnectRadio();
        require(fixture.completed == 1, "disconnect completes the retained local TUNE handoff");
        Access::authenticate(*fixture.backend);
        fixture.feed("ID024");
        fixture.feed("LP1505010");
        require(!fixture.commands.contains("TX2;"), "reconnect never replays the retired TUNE request");
    }
    {
        TuneFixture fixture(application);
        fixture.start(15);
        fixture.fresh();
        fixture.feed("LP1505010");
        bool released = false;
        QObject::connect(fixture.backend.get(), &IRadioBackend::transmitChanged, &application,
            [&](const TransmitDelta& delta) {
                if (delta.tunePower == 15 && !released) {
                    released = true;
                    fixture.backend->setTune(false, 15, fixture.operation);
                }
            });
        fixture.feed("LP1505015");
        require(!fixture.commands.contains("TX2;") && fixture.completed == 1,
            "release from native power publication wins before deferred TX2 admission");
    }
    {
        TuneFixture fixture(application);
        fixture.onCommand = [&](const QByteArray& command) {
            if (command == "TX2;") { fixture.backend->setTune(false, 10, fixture.operation); }
        };
        fixture.start();
        fixture.fresh();
        fixture.feed("LP1505010");
        require(fixture.commands.count("TX2;") == 1 && fixture.commands.last() == "RX;" && fixture.completed == 1
            && !Access::tuning(*fixture.backend), "reentrant stop during carrier handoff sends cleanup and cannot resurrect the latch");
    }
    for (const QByteArray& destroyAt : {QByteArray("LP1505015;"), QByteArray("TX2;")}) {
        TuneFixture fixture(application);
        fixture.onCommand = [&](const QByteArray& command) { if (command == destroyAt) { fixture.backend.reset(); } };
        fixture.start(destroyAt.startsWith("LP") ? 15 : 10);
        fixture.fresh();
        fixture.feed("LP1505010");
        require(!fixture.backend && fixture.completed == 1, "backend destruction during power/carrier write safely retires original TUNE completion");
    }
    {
        TuneFixture fixture(application);
        bool armed = false;
        int checks = 0;
        fixture.operation = fixture.operation.withKeyingPermit([&] {
            if (armed && ++checks == 2) { fixture.backend->setTune(false, 10, fixture.operation); }
            return true;
        });
        fixture.start();
        fixture.fresh();
        armed = true;
        fixture.feed("LP1505010");
        require(checks == 2 && fixture.completed == 1 && !fixture.commands.contains("TX2;"),
            "original keying predicate cancellation wins at the terminal dispatch boundary");
    }
    {
        TuneFixture fixture(application);
        bool armed = false;
        fixture.operation = fixture.operation.withKeyingPermit([&] {
            if (armed) { fixture.backend.reset(); }
            return true;
        });
        fixture.start();
        fixture.fresh();
        armed = true;
        fixture.feed("LP1505010");
        require(!fixture.backend && fixture.completed == 1 && !fixture.commands.contains("TX2;"),
            "destruction during original-operation validation cannot reach deferred TX2");
    }
    {
        TuneFixture fixture(application);
        fixture.start();
        const TxCoordinator::Operation old = fixture.operation;
        fixture.backend->setTune(false, 10, old);
        const TxCoordinator::Operation root = fixture.coordinator.acquire(fixture.actor, TxCoordinator::monotonicMs()).operation;
        const TxCoordinator::Intent freshIntent = fixture.coordinator.beginIntent(root, {}, TxCoordinator::Activity::Tune);
        const TxCoordinator::Operation fresh = fixture.coordinator.bindLocalIntentRetirement(root, freshIntent,
            [&](TxCoordinator::LocalIntentEnd) { (void)fixture.coordinator.endIntent(freshIntent); });
        require(old.sameOperation(fresh) && !old.sameLocalIntent(fresh), "native cleanup fixture shares one root with distinct contributions");
        fixture.backend->setTune(true, 10, fresh);
        fixture.backend->setTune(false, 10, old);
        require(Access::tuning(*fixture.backend) && Access::tuneOperation(*fixture.backend).sameLocalIntent(fresh),
            "retired producer cleanup cannot stop a new native TUNE sharing the same root operation");
        fixture.backend->setTune(false, 10, fixture.coordinator.cleanupFence());
        require(!Access::tuning(*fixture.backend) && !freshIntent.pending(), "master cleanup fence ends the intended native latch without keying");
    }
}

void verifyNativeTunePower(QCoreApplication& application)
{
    using namespace AetherSDR;
    using namespace AetherSDR::kenwood;
    using Field = TransmitControlRequest::Field;
    QList<QByteArray> commands;
    KenwoodBackend backend(KenwoodBackend::Transport{
        [&](const QByteArray& bytes) { commands.append(bytes); return true; }, {}});
    std::optional<int> tunePower;
    QObject::connect(&backend, &IRadioBackend::transmitChanged, &application,
        [&](const TransmitDelta& delta) { if (delta.tunePower) { tunePower = *delta.tunePower; } });
    const auto supported = [&] { return backend.capabilities().radioTransmitSettings->fields.contains(Field::TunePower); };
    const auto report = [&](const QByteArray& message) { KenwoodBackendTestAccess::message(backend, message); };
    KenwoodBackendTestAccess::authenticate(backend);
    report("ID024");
    require(commands.contains("XV;"), "startup queries native transverter state without changing it");
    commands.clear();
    report("FT0");
    report("SF0000140740002");
    report("LP1505020");
    report("AN1000");
    report("XT0");
    require(!supported() && !tunePower, "a cached row cannot qualify unknown transverter state");
    commands.clear();
    report("XV0");
    require(commands == QList<QByteArray>{"FA;", "FB;", "SF0;", "SF1;", "LP1505;"} && !supported() && tunePower == 20,
        "normal physical TX VFO binds native tuning power to the correct band column");
    report("LP1305030");
    report("LP1503010");
    require(tunePower == 20, "ordinary mode limits and inactive-band tuning limits do not replace current tuning power");
    commands.clear();
    require(backend.requestTransmitControl({Field::TunePower, 15}) == ReceiveDispatch::Unsupported && commands.isEmpty(),
        "native tune-power edit requires actual known RX");
    report("RX");
    require(supported(), "actual RX exposes native tune-power editing");
    require(backend.requestTransmitControl({Field::TunePower, 15}) == ReceiveDispatch::Dispatched
        && commands == QList<QByteArray>{"LP1505015;", "LP1505;"} && tunePower == 20 && !supported(),
        "native tune power edits only the tuning row and wait for readback");
    commands.clear();
    require(backend.requestTransmitControl({Field::TunePower, 10}) == ReceiveDispatch::Unsupported && commands.isEmpty(),
        "pending native power is not writable from an old cached value");
    report("LP1505015");
    require(tunePower == 15 && supported() && commands.isEmpty(), "native tune-power readback never echoes a second write");
    for (TunePowerContext context : {TunePowerContext::LiveLocalCarrier, TunePowerContext::UnqualifiedCarrier}) {
        require(backend.requestTransmitControl({Field::TunePower, 10}, context) == ReceiveDispatch::Unsupported,
            "receive-only configuration does not claim live carrier power support");
    }
    require(backend.requestTransmitControl({Field::TunePower, 4}) == ReceiveDispatch::Unsupported && commands.isEmpty(),
        "unsupported below-minimum native power never becomes a command");
    report("TX0");
    require(backend.requestTransmitControl({Field::TunePower, 10}) == ReceiveDispatch::Unsupported && commands.isEmpty(),
        "native tuning power cannot change during reported transmission");
    report("RX");
    report("XT1");
    require(!supported(), "enabled XIT with unknown offset cannot select a native power column");
    report("RF00010");
    require(supported() && tunePower == 15, "known same-band XIT retains the native power column");
    report("FA00014349999");
    require(!supported(), "XIT crossing outside the documented column retires qualification");
    report("FA00014074000");
    require(supported(), "fresh same-band target restores native power configuration");
    report("AN1010");
    require(!supported(), "drive output cannot use watt-based native tuning power");
    report("AN1000");
    report("FT1");
    require(!supported(), "changing physical TX VFO requires fresh physical mode readback");
    commands.clear();
    report("SF1000070740003");
    require(commands == QList<QByteArray>{"LP1503;"} && supported() && tunePower == 10,
        "split TX power follows physical VFO B rather than receive frequency or mode");
    commands.clear();
    KenwoodBackendTestAccess::pollPower(backend, KenwoodBackendTestAccess::nextPowerPoll(backend));
    require(commands == QList<QByteArray>{"LP1503;", "LP2;"}, "native tuning row refreshes without a power-setting write");
    report("FT3");
    require(!supported(), "memory TX retires physical tuning-power addressing");
    backend.disconnectRadio();
    KenwoodBackendTestAccess::authenticate(backend);
    report("ID024");
    require(!supported(), "reconnect retires native tuning power and address qualification");
}

void verifyNetworkAudioLevels(QCoreApplication& application)
{
    using namespace AetherSDR;
    using namespace AetherSDR::kenwood;
    for (bool administrator : {false, true}) {
        QList<QByteArray> commands;
        KenwoodBackend backend(KenwoodBackend::Transport{
            [&](const QByteArray& bytes) { commands.append(bytes); return true; }, {}});
        QVariantMap state;
        int errors = 0;
        QObject::connect(&backend, &IRadioBackend::extensionStatus, &application,
            [&](const QString&, const QString& kind, const QVariantMap& status) {
                if (kind == "audio.levels") { state = status; }
            });
        QObject::connect(&backend, &IRadioBackend::extensionResult, &application,
            [&](quint64 id, const QVariant& result) { if (id == 100) { state = result.toMap(); } });
        QObject::connect(&backend, &IRadioBackend::extensionError, &application,
            [&](quint64, const QString&) { ++errors; });
        KenwoodBackendTestAccess::authenticate(backend, administrator);
        backend.invokeExtension("kenwood", "audio.levels.get", 100);
        require(!state.value("writable").toBool() && !state.contains("inputLevel"),
            "audio levels are unknown and not writable before model identity");
        KenwoodBackendTestAccess::message(backend, "ID024");
    AetherSDR::kenwood::KenwoodBackendTestAccess::message(backend, "XV0");
        require(commands.contains("##KN30;") && commands.contains("##KN31;"), "startup queries both native VoIP levels");
        for (const QByteArray& command : commands) {
            if (command.startsWith("##KN3")) {
                require(command == "##KN30;" || command == "##KN31;", "startup cannot restore native VoIP gain");
            }
        }
        commands.clear();
        const qint64 pollAt = KenwoodBackendTestAccess::nextLevelPoll(backend);
        KenwoodBackendTestAccess::pollLevels(backend, pollAt - 1);
        require(commands.isEmpty(), "native level polling does not run before its deadline");
        KenwoodBackendTestAccess::pollLevels(backend, pollAt);
        KenwoodBackendTestAccess::pollLevels(backend, pollAt);
        require(commands == QList<QByteArray>{"##KN30;", "##KN31;"},
            "periodic level refresh queries both directions once without writing defaults");
        KenwoodBackendTestAccess::message(backend, "##KN30050");
        KenwoodBackendTestAccess::message(backend, "##KN31060");
        require(state.value("inputLevel").toInt() == 50 && state.value("outputLevel").toInt() == 60
            && state.value("writable").toBool() == administrator, "confirmed levels remain readable for ordinary accounts");
        KenwoodBackendTestAccess::message(backend, "##KN30101");
        KenwoodBackendTestAccess::message(backend, "##KN3001A");
        require(state.value("inputLevel").toInt() == 50, "invalid gain reports cannot replace live values");
        const QString context = state.value("context").toString();
        const auto edit = [&](const QString& direction, const QVariant& level, const QString& session) {
            backend.invokeExtension("kenwood", "audio.levels.set", 101,
                QVariantMap{{"direction", direction}, {"level", level}, {"context", session}, {"administrator", true}});
        };
        commands.clear();
        edit("input", 25, context);
        if (!administrator) {
            require(commands.isEmpty() && errors == 1, "a request field cannot elevate a regular account to administrator");
            continue;
        }
        require(commands == QList<QByteArray>{"##KN30025;", "##KN30;"}
            && state.value("inputLevel").toInt() == 50 && state.value("outputLevel").toInt() == 60,
            "input edit is isolated and waits for native readback");
        KenwoodBackendTestAccess::message(backend, "##KN30025");
        require(state.value("inputLevel").toInt() == 25, "native input readback adopts the requested gain");
        commands.clear();
        edit("output", 0, context);
        require(commands == QList<QByteArray>{"##KN31000;", "##KN31;"}, "zero output gain is a valid independent native setting");
        commands.clear();
        edit("input", -1, context);
        edit("input", 101, context);
        edit("input", 2.5, context);
        edit("input", true, context);
        edit("input", QStringLiteral("50"), context);
        edit("input", std::numeric_limits<double>::quiet_NaN(), context);
        edit("unknown", 50, context);
        edit("input", 50, QStringLiteral("retired"));
        require(commands.isEmpty() && errors == 8, "bounds, type, direction and session checks precede native gain writes");
        require(!backend.capabilities().canTransmit, "administrator gain configuration grants no TX authority");
        backend.disconnectRadio();
        KenwoodBackendTestAccess::authenticate(backend, true);
        KenwoodBackendTestAccess::message(backend, "ID024");
    AetherSDR::kenwood::KenwoodBackendTestAccess::message(backend, "XV0");
        KenwoodBackendTestAccess::message(backend, "##KN30070");
        commands.clear();
        edit("input", 50, context);
        require(commands.isEmpty() && errors == 9, "a previous connection's level request cannot edit the new radio session");
    }
}
}

namespace {
void verifyPowerLimits(QCoreApplication& application)
{
    using namespace AetherSDR;
    using namespace AetherSDR::kenwood;
    {
        QList<QByteArray> earlyCommands;
        QVariantMap earlyState;
        KenwoodBackend early(KenwoodBackend::Transport{
            [&](const QByteArray& bytes) { earlyCommands.append(bytes); return true; }, {}});
        QObject::connect(&early, &IRadioBackend::extensionResult, &application,
            [&](quint64 id, const QVariant& result) { if (id == 900) { earlyState = result.toMap(); } });
        KenwoodBackendTestAccess::authenticate(early);
        early.invokeExtension("kenwood", "power.limits.get", 900);
        earlyCommands.clear();
        early.invokeExtension("kenwood", "power.limits.query", 901,
            QVariantMap{{"type", 5}, {"band", 5}, {"context", earlyState.value("context")}});
        require(earlyCommands.isEmpty(), "view query waits for model identity without probing a different radio");
        KenwoodBackendTestAccess::message(early, "ID024");
    KenwoodBackendTestAccess::message(early, "XV0");
        require(earlyCommands.contains("LP1505;"), "confirmed identity resumes the current view's deferred read");
        early.disconnectRadio();
        KenwoodBackendTestAccess::authenticate(early);
        earlyCommands.clear();
        KenwoodBackendTestAccess::message(early, "ID024");
    KenwoodBackendTestAccess::message(early, "XV0");
        require(!earlyCommands.contains("LP1505;"), "reconnect retires the previous view's deferred selection");
    }
    QList<QByteArray> commands;
    KenwoodBackend backend(KenwoodBackend::Transport{
        [&](const QByteArray& bytes) { commands.append(bytes); return true; }, [](const QByteArray&) { return true; }});
    QVariantMap state;
    int errors = 0;
    QObject::connect(&backend, &IRadioBackend::extensionStatus, &application,
        [&](const QString&, const QString& kind, const QVariantMap& status) { if (kind == "power.limits") { state = status; } });
    QObject::connect(&backend, &IRadioBackend::extensionResult, &application,
        [&](quint64 id, const QVariant& result) { if (id == 700) { state = result.toMap(); } });
    QObject::connect(&backend, &IRadioBackend::extensionError, &application, [&](quint64, const QString&) { ++errors; });
    KenwoodBackendTestAccess::authenticate(backend);
    KenwoodBackendTestAccess::message(backend, "ID024");
    AetherSDR::kenwood::KenwoodBackendTestAccess::message(backend, "XV0");
    for (const QByteArray& command : commands) {
        if (command.startsWith("LP")) { require(command == "LP2;", "startup only reads limiter state"); }
    }
    backend.invokeExtension("kenwood", "power.limits.get", 700);
    require(!state.value("editable").toBool() && !state.contains("enabled") && state.value("limits").toList().isEmpty(),
        "unknown RX state and unknown limits are never invented");
    const QString context = state.value("context").toString();
    const auto call = [&](const QString& verb, QVariantMap fields) {
        fields.insert("context", context);
        backend.invokeExtension("kenwood", verb, 701, fields);
    };
    const auto watts = [&](int type, int band) {
        for (const QVariant& value : state.value("limits").toList()) {
            const QVariantMap limit = value.toMap();
            if (limit.value("type").toInt() == type && limit.value("band").toInt() == band) { return limit.value("watts").toInt(); }
        }
        return -1;
    };
    commands.clear();
    call("power.limits.query", {{"type", 5}, {"band", 5}});
    require(commands == QList<QByteArray>{"LP1505;", "LP2;"}, "explicit TX-tuning row query never changes band or RF power");
    commands.clear();
    KenwoodBackendTestAccess::pollPower(backend, KenwoodBackendTestAccess::nextPowerPoll(backend));
    require(commands == QList<QByteArray>{"LP2;"}, "unknown rows are not polled repeatedly before firmware confirms them");
    KenwoodBackendTestAccess::message(backend, "LP1505010");
    KenwoodBackendTestAccess::message(backend, "LP20");
    require(watts(5, 5) == 10 && state.contains("enabled") && !state.value("enabled").toBool(), "independent tuning limit and OFF state are authoritative");
    commands.clear();
    const qint64 pollAt = KenwoodBackendTestAccess::nextPowerPoll(backend);
    KenwoodBackendTestAccess::pollPower(backend, pollAt - 1);
    require(commands.isEmpty(), "power-limit polling respects its deadline");
    KenwoodBackendTestAccess::pollPower(backend, pollAt);
    KenwoodBackendTestAccess::pollPower(backend, pollAt);
    require(commands == QList<QByteArray>{"LP1505;", "LP2;"}, "confirmed selected row and limiter state refresh with bounded reads");
    commands.clear();
    call("power.limits.set", {{"type", 5}, {"band", 5}, {"watts", 20}});
    require(commands.isEmpty() && errors == 1, "an idle default cannot substitute for radio-reported RX");
    KenwoodBackendTestAccess::message(backend, "RX");
    require(state.value("editable").toBool() && !backend.capabilities().canTransmit, "RX permits non-keying configuration without granting TX authority");
    call("power.limits.set", {{"type", 5}, {"band", 5}, {"watts", 20}});
    require(commands == QList<QByteArray>{"LP1505020;", "LP1505;"} && watts(5, 5) == 10,
        "TUNE limit edit preserves ordinary RF power and waits for row readback");
    KenwoodBackendTestAccess::message(backend, "LP1505020");
    require(watts(5, 5) == 20, "power-limit readback confirms the selected row");
    commands.clear();
    call("power.limiter.set", {{"enabled", true}});
    require(commands == QList<QByteArray>{"LP21;", "LP2;"} && !state.value("enabled").toBool(),
        "global limiter changes only by explicit intent and waits for readback");
    KenwoodBackendTestAccess::message(backend, "LP21");
    require(state.value("enabled").toBool(), "limiter ON readback is independent of the tuning row");
    commands.clear();
    const int beforeInvalid = errors;
    call("power.limits.set", {{"type", 6}, {"band", 5}, {"watts", 20}});
    call("power.limits.set", {{"type", 5}, {"band", 12}, {"watts", 20}});
    call("power.limits.set", {{"type", 5}, {"band", 5}, {"watts", 999}});
    call("power.limits.set", {{"type", 5}, {"band", 5}, {"watts", 20.5}});
    call("power.limits.set", {{"type", 5}, {"band", 5}, {"watts", true}});
    call("power.limits.set", {{"type", 0}, {"band", 0}, {"watts", 20}});
    call("power.limiter.set", {{"enabled", 1}});
    require(commands.isEmpty() && errors == beforeInvalid + 7, "unassigned values, unknown rows and invalid types cannot change native limits");
    KenwoodBackendTestAccess::message(backend, "LP1505999");
    KenwoodBackendTestAccess::message(backend, "LP29");
    require(watts(5, 5) == 20 && state.value("enabled").toBool(), "malformed power reports cannot replace confirmed settings");
    KenwoodBackendTestAccess::message(backend, "TX0");
    call("power.limits.set", {{"type", 5}, {"band", 5}, {"watts", 10}});
    call("power.limiter.set", {{"enabled", false}});
    require(commands.isEmpty() && errors == beforeInvalid + 9, "radio-observed TX refuses both power-configuration writes");
    KenwoodBackendTestAccess::message(backend, "RX");
    KenwoodBackendTestAccess::message(backend, "##TI1");
    KenwoodBackendTestAccess::message(backend, "##UE1");
    TxCoordinator coordinator([](const TxCoordinator::Operation&, TxCoordinator::StopReason) {});
    const TxCoordinator::Actor actor = coordinator.registerActor({true, 0});
    const TxCoordinator::Operation operation = coordinator.acquire(actor, TxCoordinator::monotonicMs()).operation;
    backend.setKeying(true, operation);
    require(!state.value("editable").toBool(), "pending keying immediately withdraws power-page write availability");
    commands.clear();
    call("power.limiter.set", {{"enabled", false}});
    require(commands.isEmpty() && errors == beforeInvalid + 10, "pending keying cannot reuse an old RX observation for configuration");
    backend.setKeying(false, operation);
    require(!state.value("editable").toBool(), "stop handoff keeps power-page writes unavailable until radio readback");
    commands.clear();
    call("power.limiter.set", {{"enabled", false}});
    require(commands.isEmpty() && errors == beforeInvalid + 11, "stop handoff does not establish receive state for configuration");
    KenwoodBackendTestAccess::message(backend, "RX");
    call("power.limiter.set", {{"enabled", false}});
    require(commands == QList<QByteArray>{"LP20;", "LP2;"}, "fresh RX readback re-enables explicit configuration");
    backend.disconnectRadio();
    KenwoodBackendTestAccess::authenticate(backend);
    KenwoodBackendTestAccess::message(backend, "ID024");
    AetherSDR::kenwood::KenwoodBackendTestAccess::message(backend, "XV0");
    KenwoodBackendTestAccess::message(backend, "RX");
    KenwoodBackendTestAccess::message(backend, "LP1505020");
    commands.clear();
    call("power.limits.set", {{"type", 5}, {"band", 5}, {"watts", 10}});
    require(commands.isEmpty() && errors == beforeInvalid + 12, "a previous session's configuration request cannot edit reconnected state");
}
void verifyTransmitVfo(QCoreApplication& application)
{
    using namespace AetherSDR;
    using namespace AetherSDR::kenwood;
    QList<QByteArray> commands;
    KenwoodBackend backend(KenwoodBackend::Transport{
        [&](const QByteArray& bytes) { commands.append(bytes); return true; },
        [](const QByteArray&) { return true; }});
    QString txMode;
    double txFrequency = 0;
    int receiveChanges = 0;
    QVariantMap vfos;
    QObject::connect(&backend, &IRadioBackend::transmitChanged, &application,
        [&](const TransmitDelta& delta) {
            if (delta.txSliceMode) { txMode = *delta.txSliceMode; }
            if (delta.transmitFreq) { txFrequency = *delta.transmitFreq; }
        });
    QObject::connect(&backend, &IRadioBackend::sliceChanged, &application,
        [&](int id, const SliceDelta&) { require(id == 0, "SF cannot invent a second receiver"); ++receiveChanges; });
    QObject::connect(&backend, &IRadioBackend::extensionStatus, &application,
        [&](const QString&, const QString& kind, const QVariantMap& status) { if (kind == "vfo") { vfos = status; } });
    KenwoodBackendTestAccess::authenticate(backend);
    KenwoodBackendTestAccess::message(backend, "ID024");
    AetherSDR::kenwood::KenwoodBackendTestAccess::message(backend, "XV0");
    KenwoodBackendTestAccess::message(backend, "##TI1");
    KenwoodBackendTestAccess::message(backend, "##UE1");
    require(backend.capabilities().canTransmit, "VFO keyer fixture has native TX permission and transport");
    commands.clear();
    const qint64 pollAt = KenwoodBackendTestAccess::nextVfoPoll(backend);
    KenwoodBackendTestAccess::pollVfos(backend, pollAt - 1);
    require(commands.isEmpty(), "physical VFO polling respects its deadline");
    KenwoodBackendTestAccess::pollVfos(backend, pollAt);
    KenwoodBackendTestAccess::pollVfos(backend, pollAt);
    require(commands == QList<QByteArray>{"TS;", "SF0;", "SF1;"}, "physical VFO refresh reads both memories once without writes");
    KenwoodBackendTestAccess::message(backend, "FR0");
    KenwoodBackendTestAccess::message(backend, "FT1");
    KenwoodBackendTestAccess::message(backend, "OM02");
    TxCoordinator coordinator([](const TxCoordinator::Operation&, TxCoordinator::StopReason) {});
    const TxCoordinator::Actor actor = coordinator.registerActor({true, 0});
    const TxCoordinator::Operation operation = coordinator.acquire(actor, TxCoordinator::monotonicMs()).operation;
    commands.clear();
    require(!backend.sendCwText("CQ", operation).isEmpty() && commands.isEmpty(),
        "unknown physical TX mode cannot admit CW text");
    KenwoodBackendTestAccess::message(backend, "SF0000140740002");
    const int beforeInactive = receiveChanges;
    KenwoodBackendTestAccess::message(backend, "SF1000070740003");
    require(txMode == "CW" && txFrequency == 7.074 && receiveChanges == beforeInactive
        && vfos.value("modeA").toString() == "USB" && vfos.value("modeB").toString() == "CW",
        "physical TX VFO publishes its mode without changing the receive slice");
    commands.clear();
    require(backend.sendCwText("CQ", operation).isEmpty() && commands == QList<QByteArray>{"KY2CQ;"},
        "CW on transmit VFO B is admitted while receive VFO A is USB");
    backend.abortCwText(operation);
    KenwoodBackendTestAccess::message(backend, "OM03");
    require(txMode.isEmpty() && !vfos.contains("modeB"), "display mode change withdraws cached physical modes");
    KenwoodBackendTestAccess::message(backend, "SF0000140740003");
    KenwoodBackendTestAccess::message(backend, "SF1000070740002");
    commands.clear();
    require(!backend.sendCwText("CQ", operation).isEmpty() && commands.isEmpty() && txMode == "USB",
        "receive CW cannot admit a text keyer on a USB transmit VFO");
    KenwoodBackendTestAccess::message(backend, "SF1000070740007");
    commands.clear();
    require(backend.sendCwText("CQ", operation).isEmpty() && commands == QList<QByteArray>{"KY2CQ;"},
        "reverse CW is admitted on the actual transmit VFO");
    backend.abortCwText(operation);
    KenwoodBackendTestAccess::message(backend, "OM17");
    commands.clear();
    require(txMode.isEmpty() && !backend.sendCwText("CQ", operation).isEmpty() && commands.isEmpty(),
        "right-display mode changes also retire keyer mode qualification");
    KenwoodBackendTestAccess::message(backend, "SF1000070740003");
    KenwoodBackendTestAccess::message(backend, "FT0");
    commands.clear();
    require(txMode.isEmpty() && !backend.sendCwText("CQ", operation).isEmpty() && commands.isEmpty(),
        "TX VFO switch requires fresh physical mode readback");
    KenwoodBackendTestAccess::message(backend, "SF0000140740003");
    coordinator.reset();
    commands.clear();
    require(!backend.sendCwText("CQ", operation).isEmpty() && commands.isEmpty(),
        "confirmed CW mode cannot revive stale operation authority");
    backend.disconnectRadio();
    KenwoodBackendTestAccess::authenticate(backend);
    KenwoodBackendTestAccess::message(backend, "ID024");
    AetherSDR::kenwood::KenwoodBackendTestAccess::message(backend, "XV0");
    backend.invokeExtension("kenwood", "vfo.get", 500);
    KenwoodBackendTestAccess::message(backend, "##TI1");
    KenwoodBackendTestAccess::message(backend, "##UE1");
    KenwoodBackendTestAccess::message(backend, "FT0");
    const TxCoordinator::Actor freshActor = coordinator.registerActor({true, 0});
    const TxCoordinator::Operation freshOperation = coordinator.acquire(freshActor, TxCoordinator::monotonicMs()).operation;
    commands.clear();
    require(txMode.isEmpty() && !backend.sendCwText("CQ", freshOperation).isEmpty() && commands.isEmpty(),
        "reconnect does not preserve a known transmit mode even with fresh permission and operation");
}
void verifyTransmitFilter(QCoreApplication& application)
{
    using namespace AetherSDR;
    using namespace AetherSDR::kenwood;
    QList<QByteArray> commands;
    KenwoodBackend* currentBackend = nullptr;
    bool switchDuringWrite = false;
    KenwoodBackend backend(KenwoodBackend::Transport{
        [&](const QByteArray& bytes) {
            commands.append(bytes);
            if (switchDuringWrite && bytes == "EX00606 003;") {
                switchDuringWrite = false;
                KenwoodBackendTestAccess::message(*currentBackend, "SF1000070740003");
            }
            return true;
        }, {}});
    currentBackend = &backend;
    int low = -1;
    int high = -1;
    QObject::connect(&backend, &IRadioBackend::transmitChanged, &application,
        [&](const TransmitDelta& delta) {
            if (delta.txFilterLow) { low = *delta.txFilterLow; }
            if (delta.txFilterHigh) { high = *delta.txFilterHigh; }
        });
    const auto edit = [&](int lo, int hi) {
        return backend.requestTransmitControl({TransmitControlRequest::Field::Filter, TxPassband{lo, hi}});
    };
    KenwoodBackendTestAccess::authenticate(backend);
    KenwoodBackendTestAccess::message(backend, "ID024");
    AetherSDR::kenwood::KenwoodBackendTestAccess::message(backend, "XV0");
    require(commands.contains("EX00606;") && commands.contains("EX00607;")
        && commands.contains("EX00608;") && commands.contains("EX00609;"), "startup reads both native TX filter groups");
    for (const QByteArray& command : commands) {
        require(!command.startsWith("TF") && !(command.startsWith("EX0060") && command.contains(' ')),
            "startup never restores TX filter values or queries mode-dependent TF");
    }
    KenwoodBackendTestAccess::message(backend, "FR0");
    KenwoodBackendTestAccess::message(backend, "FT1");
    KenwoodBackendTestAccess::message(backend, "OM03");
    KenwoodBackendTestAccess::message(backend, "SF0000140740003");
    KenwoodBackendTestAccess::message(backend, "SF1000070740002");
    KenwoodBackendTestAccess::message(backend, "EX00606 001");
    commands.clear();
    require(!backend.capabilities().hasTxFilterControls && edit(200, 3000) == ReceiveDispatch::Unsupported
        && commands.isEmpty(), "both native TX filter edges must be confirmed before editing");
    KenwoodBackendTestAccess::message(backend, "EX00607 005");
    require(backend.capabilities().hasTxFilterControls && low == 100 && high == 3000
        && backend.capabilities().txFilterLowEdgesHz.first() == 10
        && backend.capabilities().txFilterHighEdgesHz.last() == 4000,
        "physical TX USB exposes native edges despite receive CW");
    commands.clear();
    require(edit(200, 3000) == ReceiveDispatch::Dispatched
        && commands == QList<QByteArray>{"EX00606 002;", "EX00606;"}
        && low == 100 && high == 3000, "one-edge edit preserves its companion and waits for radio readback");
    KenwoodBackendTestAccess::message(backend, "EX00606 002");
    require(low == 200, "TX filter adopts native readback");
    commands.clear();
    require(edit(250, 3000) == ReceiveDispatch::Unsupported && edit(200, 3400) == ReceiveDispatch::Unsupported
        && commands.isEmpty(), "TX filter rejects unreachable edge values instead of silently rounding");
    KenwoodBackendTestAccess::message(backend, "EX00606 006");
    KenwoodBackendTestAccess::message(backend, "EX00607 008");
    require(low == 200 && high == 3000, "invalid native TX replies cannot replace confirmed settings");
    KenwoodBackendTestAccess::message(backend, "EX00608 000");
    KenwoodBackendTestAccess::message(backend, "EX00609 007");
    require(low == 200 && high == 3000, "inactive data-group readbacks do not change the normal TX filter");
    KenwoodBackendTestAccess::message(backend, "SF100007074000D");
    require(low == 10 && high == 4000, "physical data TX mode selects the independently confirmed data filter");
    commands.clear();
    KenwoodBackendTestAccess::pollVfos(backend, KenwoodBackendTestAccess::nextVfoPoll(backend));
    require(commands == QList<QByteArray>{"TS;", "SF0;", "SF1;", "EX00608;", "EX00609;"},
        "periodic refresh queries only the active data TX filter group without writes");
    commands.clear();
    require(edit(100, 3500) == ReceiveDispatch::Dispatched
        && commands == QList<QByteArray>{"EX00608 001;", "EX00608;", "EX00609 006;", "EX00609;"}
        && low == 10 && high == 4000, "data filter edits use only their own menu group and remain radio-authoritative");
    KenwoodBackendTestAccess::message(backend, "OM12");
    commands.clear();
    require(!backend.capabilities().hasTxFilterControls && edit(200, 3000) == ReceiveDispatch::Unsupported
        && commands.isEmpty(), "unknown physical mode withdraws TX filter controls");
    for (char mode : QByteArray("34769ABE")) {
        KenwoodBackendTestAccess::message(backend, QByteArray("SF100007074000") + mode);
        commands.clear();
        require(!backend.capabilities().hasTxFilterControls && edit(100, 3000) == ReceiveDispatch::Unsupported
            && commands.isEmpty(), "non-SSB/AM physical TX modes cannot edit a fabricated filter");
    }
    KenwoodBackendTestAccess::message(backend, "SF1000070740005");
    require(backend.capabilities().hasTxFilterControls && low == 200 && high == 3000,
        "AM selects the normal SSB/AM menu group");
    switchDuringWrite = true;
    commands.clear();
    require(edit(300, 3500) == ReceiveDispatch::Unsupported
        && commands == QList<QByteArray>{"EX00606 003;", "EX00606;"}
        && !backend.capabilities().hasTxFilterControls,
        "mode change during first handoff prevents a second edge write under retired mode intent");
    backend.disconnectRadio();
    KenwoodBackendTestAccess::authenticate(backend);
    KenwoodBackendTestAccess::message(backend, "ID024");
    AetherSDR::kenwood::KenwoodBackendTestAccess::message(backend, "XV0");
    KenwoodBackendTestAccess::message(backend, "FT1");
    KenwoodBackendTestAccess::message(backend, "SF1000070740002");
    require(!backend.capabilities().hasTxFilterControls, "reconnect retires native filter readbacks");
}

void verifyReceiveVfo(QCoreApplication& application)
{
    using namespace AetherSDR;
    using namespace AetherSDR::kenwood;
    QList<QByteArray> commands;
    KenwoodBackend backend(KenwoodBackend::Transport{
        [&](const QByteArray& bytes) { commands.append(bytes); return true; }, {}});
    QStringList receiveModes;
    QVariantMap nativeFilter;
    QObject::connect(&backend, &IRadioBackend::sliceChanged, &application,
        [&](int id, const SliceDelta& delta) {
            require(id == 0, "physical receive VFO selection always retains slice zero");
            if (delta.mode) { receiveModes.append(*delta.mode); }
        });
    QObject::connect(&backend, &IRadioBackend::extensionStatus, &application,
        [&](const QString&, const QString& kind, const QVariantMap& status) {
            if (kind == "filter") { nativeFilter = status; }
        });
    KenwoodBackendTestAccess::authenticate(backend);
    KenwoodBackendTestAccess::message(backend, "ID024");
    AetherSDR::kenwood::KenwoodBackendTestAccess::message(backend, "XV0");
    KenwoodBackendTestAccess::message(backend, "SF0000140740002");
    KenwoodBackendTestAccess::message(backend, "SF1000070740003");
    require(receiveModes.isEmpty(), "physical memory modes cannot select an unknown receive VFO");
    commands.clear();
    KenwoodBackendTestAccess::message(backend, "FR1");
    require(commands == QList<QByteArray>{"SF1;"} && receiveModes.isEmpty(),
        "receive selection requests its physical memory rather than assuming a display position");
    KenwoodBackendTestAccess::message(backend, "SF1000070740003");
    require(receiveModes == QStringList{"CW"} && nativeFilter.value("mode").toString() == "CW",
        "receive VFO B supplies its own mode and native filter layout");
    KenwoodBackendTestAccess::message(backend, "SL006");
    KenwoodBackendTestAccess::message(backend, "SH0080");
    const QString oldContext = nativeFilter.value("context").toString();
    KenwoodBackendTestAccess::message(backend, "TX1");
    commands.clear();
    KenwoodBackendTestAccess::message(backend, "OM02");
    require(receiveModes == QStringList{"CW"} && !nativeFilter.value("editable").toBool()
        && nativeFilter.value("context").toString() != oldContext
        && commands == QList<QByteArray>{"TS;", "SF0;", "SF1;"},
        "left-display USB during TX cannot relabel receive CW or retain its filter edit context");
    KenwoodBackendTestAccess::message(backend, "SF0000140740002");
    require(receiveModes == QStringList{"CW"}, "inactive physical VFO cannot overwrite selected receive mode");
    KenwoodBackendTestAccess::message(backend, "SF1000070740003");
    require(receiveModes.last() == "CW", "fresh selected memory readback restores physical receive mode after display invalidation");
    KenwoodBackendTestAccess::message(backend, "RX");
    commands.clear();
    KenwoodBackendTestAccess::message(backend, "FR0");
    require(commands == QList<QByteArray>{"SF0;"} && !nativeFilter.value("editable").toBool(),
        "receive VFO switch retires previous filter qualification until its SF readback");
    KenwoodBackendTestAccess::message(backend, "SF000014074000D");
    require(receiveModes.last() == "DIGU" && nativeFilter.value("mode").toString() == "DIGU",
        "selected physical data mode reaches receive state without OM mapping");
    const qsizetype count = receiveModes.size();
    KenwoodBackendTestAccess::message(backend, "SF0000140740008");
    KenwoodBackendTestAccess::message(backend, "OM08");
    require(receiveModes.size() == count, "invalid physical and display mode codes cannot enter receive state");
}

void verifyPowerSessionRetirement(QCoreApplication& application)
{
    using namespace AetherSDR;
    using namespace AetherSDR::kenwood;
    using Access = KenwoodBackendTestAccess;
    for (const QByteArray& power : {QByteArray("PS0"), QByteArray("PS2"), QByteArray("PS3")}) {
        QList<QByteArray> commands;
        QList<PcmFrame> pcm;
        int disconnected = 0;
        int idle = 0;
        KenwoodBackend backend(KenwoodBackend::Transport{
            [&](const QByteArray& bytes) { commands.append(bytes); return true; }, [](const QByteArray&) { return true; }});
        QObject::connect(&backend, &IRadioBackend::disconnected, &application, [&] { ++disconnected; });
        QObject::connect(&backend, &IRadioBackend::keyingStateConfirmed, &application,
            [&](bool transmitting) { if (!transmitting) { ++idle; } });
        QObject::connect(&backend, &IRadioBackend::audioFrameReady, &application,
            [&](const PcmFrame& frame) { pcm.append(frame); });
        Access::authenticate(backend);
        require(commands.indexOf("PS;") >= 0 && commands.indexOf("PS;") < commands.indexOf("ID;"),
            "standby detection requests power before identity/bootstrap");
        Access::message(backend, "ID024");
    AetherSDR::kenwood::KenwoodBackendTestAccess::message(backend, "XV0");
        Access::message(backend, "##TI1");
        Access::message(backend, "##UE1");
        Access::message(backend, "MS103");
        QByteArray packet(652, '\0');
        packet[0] = char(0x80);
        packet[1] = 96;
        Access::audio(backend, packet, 0);
        require(!pcm.isEmpty() && pcm.last().current(), "power fixture has a live PCM lease");
        TxCoordinator coordinator([](const TxCoordinator::Operation&, TxCoordinator::StopReason) {});
        const TxCoordinator::Actor actor = coordinator.registerActor({true, 0});
        const TxCoordinator::Operation operation = coordinator.acquire(actor, TxCoordinator::monotonicMs()).operation;
        const TxCoordinator::Producer producer = coordinator.registerProducer();
        const TxCoordinator::Context media = coordinator.mediaContext(producer, operation);
        backend.observePcAudioRouting(true);
        backend.setKeying(true, operation);
        backend.submitTxAudio(QByteArray(480 * 4, '\0'), 24000, TxAudioSource::Microphone, media);
        require(Access::queuedSamples(backend) > 0, "power fixture queues qualified media");
        QObject::connect(&backend, &IRadioBackend::connectionError, &application, [&](const QString&) {
            backend.setKeying(true, operation);
            backend.submitTxAudio(QByteArray(480 * 4, '\0'), 24000, TxAudioSource::Microphone, media);
        });
        commands.clear();
        Access::control(backend, power + ";RX;ID024;IF00021195340      000000000030000000;");
        require(!backend.isConnected() && disconnected == 1 && idle == 0 && !pcm.last().current()
            && Access::queuedSamples(backend) == 0 && commands.isEmpty(),
            "power transition retires media before observers and drops the old batch without RF cleanup or false idle");
        Access::message(backend, power);
        require(disconnected == 1, "repeated retired power status cannot disconnect twice");
        Access::authenticate(backend);
        commands.clear();
        Access::control(backend, "PS1;ID024;");
        require(backend.isConnected() && commands.contains("IF;"), "powered reconnect requests a new initial status");
        const int writes = commands.size();
        Access::control(backend, "PS1;PS1;");
        require(commands.size() == writes && !commands.contains("PS1;"), "power keepalives do not replay bootstrap or power-on writes");
    }
    {
        auto backend = std::make_unique<KenwoodBackend>(KenwoodBackend::Transport{
            [](const QByteArray&) { return true; }, {}});
        Access::authenticate(*backend);
        QObject::connect(backend.get(), &IRadioBackend::connectionError, &application,
            [&](const QString&) { backend.reset(); });
        Access::control(*backend, "PS3;ID024;");
        require(!backend, "power-error observer may synchronously destroy its backend");
    }
}

void verifyTimerControlRetirement(QCoreApplication& application)
{
    using namespace AetherSDR;
    using namespace AetherSDR::kenwood;
    using Access = KenwoodBackendTestAccess;
    for (const QByteArray& state : {QByteArray("PS4"), QByteArray("PS5"), QByteArray("PS6")}) {
        QList<QByteArray> commands;
        int disconnected = 0;
        int errors = 0;
        KenwoodBackend backend(KenwoodBackend::Transport{
            [&](const QByteArray& bytes) { commands.append(bytes); return true; }, {}});
        Access::authenticate(backend);
        Access::control(backend, "ID024;XV0;FR0;FT0;OM02;SF0000142699002;TS0;RX;");
        require(backend.isConnected(), "timer fixture begins in a normal receiving session");
        QObject::connect(&backend, &IRadioBackend::disconnected, &application, [&] { ++disconnected; });
        QObject::connect(&backend, &IRadioBackend::connectionError, &application, [&](const QString& reason) {
            ++errors;
            require(reason.contains("timer"), "timer restriction explains why the session was retired");
            backend.setSliceFrequency(0, 14270000);
            backend.setPanBandwidth("0xe1000000", 50000);
        });
        commands.clear();
        Access::control(backend, state + ";ID024;RX;SF0000142699002;");
        require(!backend.isConnected() && disconnected == 1 && errors == 1 && commands.isEmpty(),
            "timer observations retire control before callbacks and reject the remainder of the old batch");
        Access::message(backend, state);
        require(disconnected == 1 && errors == 1, "a retired timer observation is not processed twice");
    }
}

void verifySharedNativeApf(QCoreApplication& application)
{
    using namespace AetherSDR;
    using namespace AetherSDR::kenwood;
    using Access = KenwoodBackendTestAccess;
    QList<QByteArray> commands;
    std::optional<bool> observed;
    KenwoodBackend backend(KenwoodBackend::Transport{
        [&](const QByteArray& bytes) { commands.append(bytes); return true; }, {}});
    QObject::connect(&backend, &IRadioBackend::sliceChanged, &application,
        [&](int id, const SliceDelta& delta) { if (id == 0 && delta.apf) { observed = *delta.apf; } });
    Access::authenticate(backend);
    Access::control(backend, "ID024;XV0;MO20;FR0;FT0;OM03;SF0000142699003;TS0;RX;");
    require(commands.contains("AP0;") && !commands.contains("AP1;") && !commands.contains("NBD;"),
        "shared APF discovers native enable without eagerly querying the extended DSP page");
    require(!backend.capabilities().hasAudioPeakingFilter && !observed,
        "shared APF remains unavailable until its native enable is observed");
    require(backend.capabilities().receiveDspControl->unavailableLevels.contains(ReceiveDspControl::Feature::Apf),
        "native APF never advertises a shared percent scale while readback is pending");
    Access::message(backend, "AP01");
    require(backend.capabilities().hasAudioPeakingFilter && observed == std::optional<bool>(true),
        "native APF readback reaches the shared slice and capability");
    SliceDspRequest request;
    request.feature = SliceDspRequest::Feature::Apf;
    request.field = SliceDspRequest::Field::Enabled;
    request.enabled = false;
    commands.clear();
    require(backend.requestSliceDsp(0, request) == ReceiveDispatch::Dispatched
        && commands == QList<QByteArray>{"AP00;", "AP0;"} && observed == std::optional<bool>(true),
        "shared APF enable uses native digits and keeps radio truth until readback");
    require(!backend.capabilities().hasAudioPeakingFilter
        && backend.requestSliceDsp(0, request) == ReceiveDispatch::Unsupported,
        "pending shared APF cannot dispatch a duplicate request");
    Access::message(backend, "AP00");
    require(backend.capabilities().hasAudioPeakingFilter && observed == std::optional<bool>(false),
        "shared APF adopts the confirmed off state");
    request.field = SliceDspRequest::Field::Level;
    request.level = 50;
    commands.clear();
    require(backend.requestSliceDsp(0, request) == ReceiveDispatch::Unsupported && commands.isEmpty(),
        "native APF dB gain is not represented as a shared percentage level");
    Access::control(backend, "OM02;SF0000142699002;");
    request.field = SliceDspRequest::Field::Enabled;
    request.enabled = true;
    commands.clear();
    require(!backend.capabilities().hasAudioPeakingFilter
        && backend.requestSliceDsp(0, request) == ReceiveDispatch::Unsupported && commands.isEmpty(),
        "voice mode cannot reuse the previous CW APF enable qualification");
}

void verifyFirmwareObservation(QCoreApplication& application)
{
    using namespace AetherSDR;
    using namespace AetherSDR::kenwood;
    using Access = KenwoodBackendTestAccess;
    QList<QByteArray> commands;
    RadioModel model;
    auto owned = std::make_unique<KenwoodBackend>(KenwoodBackend::Transport{
        [&](const QByteArray& bytes) { commands.append(bytes); return true; }, {}});
    KenwoodBackend* backend = owned.get();
    model.setBackendForTest(std::move(owned), QStringLiteral("kenwood"));
    int changes = 0;
    QObject::connect(&model, &RadioModel::infoChanged, &application, [&] { ++changes; });
    Access::authenticate(*backend);
    const QString initial = model.version();
    Access::message(*backend, "FV1.01");
    require(model.version() == initial, "unidentified KNS peers cannot publish a firmware identity");
    Access::message(*backend, "ID024");
    KenwoodBackendTestAccess::message(*backend, "XV0");
    require(commands.contains("FV;"), "identified sessions query the native firmware version");
    Access::message(*backend, "FV1.01");
    require(model.version() == QStringLiteral("1.01"),
        "firmware readback reaches the neutral model without losing its reported formatting");
    const int previousChanges = changes;
    for (const QByteArray& invalid : {QByteArray("FV"), QByteArray("FV1"), QByteArray("FV1..01"),
            QByteArray("FV1.01extra"), QByteArray("FV1.01."), QByteArray("FV9999999999999999999999999.01")}) {
        Access::message(*backend, invalid);
    }
    require(model.version() == QStringLiteral("1.01") && changes == previousChanges,
        "malformed firmware observations cannot replace confirmed identity or emit changes");
    Access::message(*backend, "FV2.03");
    require(model.version() == QStringLiteral("2.03") && changes == previousChanges + 1,
        "fresh firmware observations use the existing information notification");
    Access::control(*backend, "PS4;FV9.99;");
    require(!backend->isConnected() && model.version() == QStringLiteral("2.03"),
        "retired timer-session frames cannot replace firmware identity");
}

void verifyInitialStatusAndScope()
{
    using namespace AetherSDR;
    using namespace AetherSDR::kenwood;
    using Access = KenwoodBackendTestAccess;
    const QByteArray status = "IF00021195340      000000000030000000";
    for (bool keyBeforeSnapshot : {false, true}) {
        QList<QByteArray> commands;
        QList<bool> observations;
        KenwoodBackend backend(KenwoodBackend::Transport{
            [&](const QByteArray& bytes) { commands.append(bytes); return true; }, [](const QByteArray&) { return true; }});
        QObject::connect(&backend, &IRadioBackend::keyingStateConfirmed, &backend,
            [&](bool transmitting) { observations.append(transmitting); });
        Access::authenticate(backend);
        Access::message(backend, "ID024");
    AetherSDR::kenwood::KenwoodBackendTestAccess::message(backend, "XV0");
        require(commands.contains("IF;") && !commands.contains("RX;") && !commands.contains("TX;")
            && !commands.contains("BSM;") && observations.isEmpty(),
            "startup requests a read-only state snapshot and never forces receive or queries bare BSM");
        for (const QByteArray& message : {QByteArray("FR0"), QByteArray("FT0"), QByteArray("SF0000211953403"), QByteArray("TS0")}) {
            Access::message(backend, message);
        }
        require(backend.capabilities().receiveModeControl->modes.isEmpty(), "audio/VFO/TF-SET do not infer RX");
        QByteArray malformed = status;
        malformed[28] = '2';
        Access::message(backend, malformed);
        require(observations.isEmpty(), "malformed IF cannot publish an idle observation");
        if (keyBeforeSnapshot) {
            Access::message(backend, "##TI1");
            Access::message(backend, "##UE1");
            TxCoordinator coordinator([](const TxCoordinator::Operation&, TxCoordinator::StopReason) {});
            const TxCoordinator::Actor actor = coordinator.registerActor({true, 0});
            const TxCoordinator::Operation operation = coordinator.acquire(actor, TxCoordinator::monotonicMs()).operation;
            backend.setKeying(true, operation);
            backend.setKeying(false, operation);
            Access::message(backend, status);
            require(observations.isEmpty() && backend.capabilities().receiveModeControl->modes.isEmpty(),
                "snapshot requested before a local keying command cannot acknowledge its stop");
            Access::message(backend, "RX");
        } else {
            Access::message(backend, status);
        }
        require(observations == QList<bool>{false} && !backend.capabilities().receiveModeControl->modes.isEmpty(),
            "qualified initial IF or later RX enables receive controls without a transmit grant");
        Access::message(backend, "TX2");
        Access::message(backend, status);
        require(observations == QList<bool>{false, true} && backend.capabilities().receiveModeControl->modes.isEmpty(),
            "consumed initial snapshot cannot overwrite a newer native TX event");
        backend.disconnectRadio();
        Access::authenticate(backend);
        Access::message(backend, status);
        require(observations.size() == 2, "new transport cannot adopt an old snapshot before requesting its own");
    }
    {
        KenwoodBackend backend(KenwoodBackend::Transport{[](const QByteArray&) { return true; }, {}});
        QList<bool> observations;
        bool inventedTune = false;
        QObject::connect(&backend, &IRadioBackend::keyingStateConfirmed, &backend,
            [&](bool transmitting) { observations.append(transmitting); });
        QObject::connect(&backend, &IRadioBackend::transmitChanged, &backend,
            [&](const TransmitDelta& delta) { if (delta.tune) { inventedTune = true; } });
        Access::authenticate(backend);
        Access::message(backend, "ID024");
    AetherSDR::kenwood::KenwoodBackendTestAccess::message(backend, "XV0");
        QByteArray transmitting = status;
        transmitting[28] = '1';
        Access::message(backend, transmitting);
        require(observations == QList<bool>{true} && !inventedTune,
            "an initial TX snapshot reports busy without assigning PTT source or TUNE state");
    }
    {
        QList<QByteArray> commands;
        KenwoodBackend backend(KenwoodBackend::Transport{
            [&](const QByteArray& bytes) { commands.append(bytes); return true; }, {}});
        Access::authenticate(backend);
        Access::message(backend, "ID024");
    AetherSDR::kenwood::KenwoodBackendTestAccess::message(backend, "XV0");
        commands.clear();
        Access::message(backend, "BS30");
        require(commands.isEmpty(), "Center scope does not query Fixed scope limits");
        Access::message(backend, "BS31");
        require(commands == QList<QByteArray>{"BS5;"}, "Fixed scope first queries the selected stored range");
        commands.clear();
        Access::message(backend, "BS51");
        require(commands == QList<QByteArray>{"BSM0;"}, "range selection reads current displayed bounds with BSM0");
        Access::message(backend, "BSM02100000021450000");
        commands.clear();
        backend.setPanBandwidth(QStringLiteral("0"), 10000);
        require(commands == QList<QByteArray>{"BSM12122000021230000;", "BSM0;"},
            "current-limit reply cannot replace stored write-range identity with read-only range zero");
        commands.clear();
        Access::message(backend, "BS50");
        require(commands == QList<QByteArray>{"BSM0;"}, "temporary range refreshes current bounds");
        Access::message(backend, "BSM02100000021450000");
        commands.clear();
        backend.setPanBandwidth(QStringLiteral("0"), 10000);
        require(commands.isEmpty(), "temporary range zero is never used in a Fixed setting command");
    }
}

void verifyReceiveModeControl(QCoreApplication& application)
{
    using namespace AetherSDR;
    using namespace AetherSDR::kenwood;
    QList<QByteArray> commands;
    KenwoodBackend backend(KenwoodBackend::Transport{
        [&](const QByteArray& bytes) { commands.append(bytes); return true; },
        [](const QByteArray&) { return true; }});
    QString observedMode;
    QObject::connect(&backend, &IRadioBackend::sliceChanged, &application,
        [&](int, const SliceDelta& delta) { if (delta.mode) { observedMode = *delta.mode; } });
    KenwoodBackendTestAccess::authenticate(backend);
    KenwoodBackendTestAccess::message(backend, "ID024");
    AetherSDR::kenwood::KenwoodBackendTestAccess::message(backend, "XV0");
    require(commands.contains("TS;") && !commands.contains("TS0;"), "startup reads TF-SET without changing it");
    KenwoodBackendTestAccess::message(backend, "FR0");
    KenwoodBackendTestAccess::message(backend, "SF0000140740002");
    KenwoodBackendTestAccess::message(backend, "RX");
    commands.clear();
    backend.setSliceMode(0, "CWR");
    require(commands.isEmpty() && backend.capabilities().receiveModeControl->modes.isEmpty(),
        "unknown TF-SET cannot admit a receive-mode setting");
    KenwoodBackendTestAccess::message(backend, "TS1");
    commands.clear();
    backend.setSliceMode(0, "CWR");
    require(commands.isEmpty(), "TF-SET cannot redirect a receive-mode edit to the transmit VFO");
    KenwoodBackendTestAccess::message(backend, "TS0");
    require(backend.capabilities().receiveModeControl->modes.contains("CWR"),
        "confirmed RX with TF-SET off exposes native mode choices");
    commands.clear();
    backend.setSliceMode(0, "CWR");
    require(commands == QList<QByteArray>{"OM07;", "OM0;"} && observedMode == "USB",
        "admitted mode request uses native OM and waits for physical memory readback");
    KenwoodBackendTestAccess::message(backend, "OM07");
    require(backend.capabilities().receiveModeControl->modes.isEmpty() && observedMode == "USB",
        "display readback alone cannot confirm or re-admit physical receive mode");
    KenwoodBackendTestAccess::message(backend, "SF0000140740007");
    require(observedMode == "CWR" && !backend.capabilities().receiveModeControl->modes.isEmpty(),
        "physical mode confirmation updates live state and restores eligibility");
    KenwoodBackendTestAccess::message(backend, "TX1");
    commands.clear();
    backend.setSliceMode(0, "USB");
    require(commands.isEmpty() && backend.capabilities().receiveModeControl->modes.isEmpty(),
        "observed TX cannot retarget a receive-mode edit");
    KenwoodBackendTestAccess::message(backend, "RX");
    KenwoodBackendTestAccess::message(backend, "##TI1");
    KenwoodBackendTestAccess::message(backend, "##UE1");
    TxCoordinator coordinator([](const TxCoordinator::Operation&, TxCoordinator::StopReason) {});
    const TxCoordinator::Actor actor = coordinator.registerActor({true, 0});
    const TxCoordinator::Operation operation = coordinator.acquire(actor, TxCoordinator::monotonicMs()).operation;
    backend.setKeying(true, operation);
    backend.setKeying(false, operation);
    commands.clear();
    backend.setSliceMode(0, "USB");
    require(commands.isEmpty() && backend.capabilities().receiveModeControl->modes.isEmpty(),
        "stop-command handoff cannot reuse earlier RX mode-write eligibility");
    KenwoodBackendTestAccess::message(backend, "RX");
    require(!backend.capabilities().receiveModeControl->modes.isEmpty(), "fresh RX can restore non-keying mode eligibility");
    backend.disconnectRadio();
    KenwoodBackendTestAccess::authenticate(backend);
    KenwoodBackendTestAccess::message(backend, "ID024");
    AetherSDR::kenwood::KenwoodBackendTestAccess::message(backend, "XV0");
    KenwoodBackendTestAccess::message(backend, "FR0");
    KenwoodBackendTestAccess::message(backend, "SF0000140740002");
    KenwoodBackendTestAccess::message(backend, "RX");
    require(backend.capabilities().receiveModeControl->modes.isEmpty(), "reconnect cannot retain prior TF-SET qualification");
}

void verifyMemorySources(QCoreApplication& application)
{
    using namespace AetherSDR;
    using namespace AetherSDR::kenwood;
    QList<QByteArray> commands;
    KenwoodBackend backend(KenwoodBackend::Transport{
        [&](const QByteArray& bytes) { commands.append(bytes); return true; },
        [](const QByteArray&) { return true; }});
    QVariantMap vfos;
    QVariantMap filter;
    QString txMode;
    double txFrequency = -1;
    int receiveChanges = 0;
    QObject::connect(&backend, &IRadioBackend::extensionStatus, &application,
        [&](const QString&, const QString& kind, const QVariantMap& state) {
            if (kind == "vfo") { vfos = state; }
            if (kind == "filter") { filter = state; }
        });
    QObject::connect(&backend, &IRadioBackend::transmitChanged, &application,
        [&](const TransmitDelta& delta) {
            if (delta.txSliceMode) { txMode = *delta.txSliceMode; }
            if (delta.transmitFreq) { txFrequency = *delta.transmitFreq; }
        });
    QObject::connect(&backend, &IRadioBackend::sliceChanged, &application,
        [&](int, const SliceDelta&) { ++receiveChanges; });
    KenwoodBackendTestAccess::authenticate(backend);
    for (const QByteArray& message : {QByteArray("ID024"), QByteArray("XV0"), QByteArray("##TI1"), QByteArray("##UE1"),
            QByteArray("RX"), QByteArray("TS0"), QByteArray("FR0"), QByteArray("FT0"), QByteArray("SF0000140740003")}) {
        KenwoodBackendTestAccess::message(backend, message);
    }
    TxCoordinator coordinator([](const TxCoordinator::Operation&, TxCoordinator::StopReason) {});
    const TxCoordinator::Actor actor = coordinator.registerActor({true, 0});
    const TxCoordinator::Operation operation = coordinator.acquire(actor, TxCoordinator::monotonicMs()).operation;
    require(backend.sendCwText("CQ", operation).isEmpty(), "memory-source fixture begins with a qualified native CW VFO");
    backend.abortCwText(operation);
    KenwoodBackendTestAccess::message(backend, "RX");
    KenwoodBackendTestAccess::message(backend, "FR3");
    commands.clear();
    require(vfos.value("receiveSource") == "memory" && !vfos.contains("receive") && !vfos.contains("split")
        && !vfos.value("transmitSelectable").toBool() && txMode.isEmpty() && txFrequency == 0,
        "memory recall retires physical VFO selectors and cached transmit qualification");
    require(!backend.sendCwText("CQ", operation).isEmpty(), "FR3 alone cannot reuse an earlier CW VFO while FT readback is pending");
    backend.setSliceFrequency(0, 14100000);
    backend.setSliceMode(0, "USB");
    backend.setSliceFilter(0, -100, 100);
    backend.invokeExtension("kenwood", "vfo.transmit", 999, "B");
    require(commands.isEmpty() && !filter.value("editable").toBool()
        && backend.capabilities().receiveModeControl->modes.isEmpty(),
        "unsupported memory source cannot edit the old receive VFO or switch FT");
    const int beforeInactive = receiveChanges;
    KenwoodBackendTestAccess::message(backend, "FT3");
    KenwoodBackendTestAccess::message(backend, "SF0000140740003");
    KenwoodBackendTestAccess::message(backend, "FA00014200000");
    require(receiveChanges == beforeInactive && txMode.isEmpty() && txFrequency == 0
        && vfos.value("transmitSource") == "memory" && !vfos.contains("transmit"),
        "inactive VFO reports cannot masquerade as memory-channel live receive or transmit state");
    commands.clear();
    require(!backend.sendCwText("CQ", operation).isEmpty() && commands.isEmpty(), "FT3 cannot key using cached physical CW mode");
    backend.invokeExtension("kenwood", "vfo.receive", 1000, "A");
    require(commands == QList<QByteArray>{"FR0;", "FR;"}, "an explicit receive selector can leave memory-channel operation");
    commands.clear();
    KenwoodBackendTestAccess::message(backend, "FR0");
    require(commands == QList<QByteArray>{"FT;", "SF0;"}, "returning from memory refreshes actual TX selection and receive mode");
    KenwoodBackendTestAccess::message(backend, "FT0");
    KenwoodBackendTestAccess::message(backend, "SF0000140740003");
    require(txMode == "CW" && vfos.value("receive") == "A" && vfos.value("transmit") == "A"
        && vfos.value("transmitSelectable").toBool(), "fresh native VFO readbacks recover normal operation");
    commands.clear();
    require(backend.sendCwText("CQ", operation).isEmpty() && commands == QList<QByteArray>{"KY2CQ;"},
        "returning to confirmed physical CW restores admitted text keying");
    backend.abortCwText(operation);
    KenwoodBackendTestAccess::message(backend, "RX");
    KenwoodBackendTestAccess::message(backend, "SF0000140740002");
    KenwoodBackendTestAccess::message(backend, "EX00606 001");
    KenwoodBackendTestAccess::message(backend, "EX00607 005");
    require(backend.capabilities().hasTxFilterControls, "normal TX filter is qualified before memory-source transition");
    KenwoodBackendTestAccess::message(backend, "FT3");
    commands.clear();
    const ReceiveDispatch filterRequest = backend.requestTransmitControl(
        {TransmitControlRequest::Field::Filter, TxPassband{200, 3000}});
    require(!backend.capabilities().hasTxFilterControls && filterRequest == ReceiveDispatch::Unsupported
        && commands.isEmpty(), "memory transmitter withdraws a previously qualified native TX filter");
    bool recallDuringPublication = true;
    QObject::connect(&backend, &IRadioBackend::capabilitiesChanged, &application, [&] {
        if (recallDuringPublication) {
            recallDuringPublication = false;
            KenwoodBackendTestAccess::message(backend, "FR3");
        }
    });
    commands.clear();
    KenwoodBackendTestAccess::message(backend, "FR0");
    require(vfos.value("receiveSource") == "memory" && commands == QList<QByteArray>{"FT;"},
        "reentrant memory recall retires the old FR publication before array access or a stale SF query");
}

void verifyConnectionConfiguration()
{
    using namespace AetherSDR;
    using namespace AetherSDR::kenwood;
    RadioConnectRequest request;
    request.host = "127.0.0.1";
    request.params = {{"kenwood.username", "user"}, {"kenwood.password", "secret"}};
    require(KenwoodBackendTestAccess::validate(request).isEmpty(), "default native connection parameters validate without creating sockets");
    for (const QString& variant : {QString(), QString("k"), QString("e")}) {
        request.params["kenwood.variant"] = variant;
        require(KenwoodBackendTestAccess::validate(request).isEmpty(), "documented regional profiles validate before transport setup");
    }
    for (const QVariant& invalid : {QVariant("60002"), QVariant(true), QVariant(60002.5), QVariant(0),
             QVariant(65536), QVariant(std::numeric_limits<double>::quiet_NaN())}) {
        request.params["kenwood.txSourcePort"] = invalid;
        require(KenwoodBackendTestAccess::validate(request).contains("source port 60002"), "arbitrary or mistyped TX source port is rejected before sockets");
    }
    request.params["kenwood.txSourcePort"] = QVariant::fromValue<quint16>(60002);
    for (const QVariant& invalid : {QVariant("60001"), QVariant(true), QVariant(60001.5), QVariant(60002)}) {
        request.params["kenwood.audioPort"] = invalid;
        require(KenwoodBackendTestAccess::validate(request).contains("UDP audio port 60001"), "RX audio port has the same fixed numeric contract");
    }
    request.params["kenwood.audioPort"] = 60001;
    for (const QVariant& invalid : {QVariant(1), QVariant("true"), QVariant("false")}) {
        request.params["kenwood.administrator"] = invalid;
        require(KenwoodBackendTestAccess::validate(request).contains("boolean"), "account type cannot be coerced from a number or string");
    }
    request.params["kenwood.administrator"] = true;
    for (const QVariant& invalid : {QVariant("other"), QVariant(QVariantMap{}), QVariant(false)}) {
        request.params["kenwood.variant"] = invalid;
        require(KenwoodBackendTestAccess::validate(request).contains("regional variant"), "mistyped variant cannot silently choose a profile");
    }
    request.params["kenwood.variant"] = "k";
    for (const QString& host : {QString(), QString(" radio"), QString("radio name"), QString("radio\nname"),
             QString(254, QLatin1Char('a')), QString("::1"), QString("0.0.0.0"), QString("239.1.2.3"), QString("255.255.255.255")}) {
        request.host = host;
        require(!KenwoodBackendTestAccess::validate(request).isEmpty(), "invalid or unsupported destination is rejected without DNS or sockets");
    }
    request.host = "radio.example.invalid";
    for (const QHostAddress& source : {QHostAddress("::1"), QHostAddress("239.1.2.3"), QHostAddress(QHostAddress::Broadcast)}) {
        request.localBindAddress = source;
        require(KenwoodBackendTestAccess::validate(request).contains("source path"), "unsupported source paths are rejected before bind");
    }
    request.localBindAddress = QHostAddress::LocalHost;
    for (const QVariant& invalid : {QVariant(), QVariant(123), QVariant("bad;TX1"), QVariant(QString(33, QLatin1Char('x')))}) {
        request.params["kenwood.password"] = invalid;
        require(KenwoodBackendTestAccess::validate(request).contains("KNS credentials"), "missing, mistyped or invalid credentials fail before networking");
    }
    request.params["kenwood.password"] = "secret";
    request.port = 61000;
    require(KenwoodBackendTestAccess::validate(request).isEmpty(), "hostname, explicit IPv4 source and mapped TCP control port remain supported");
}

void verifyRadioSidetone(QCoreApplication& application)
{
    using namespace AetherSDR;
    using namespace AetherSDR::kenwood;
    using Field = TransmitControlRequest::Field;
    QList<QByteArray> commands;
    KenwoodBackend backend(KenwoodBackend::Transport{
        [&](const QByteArray& bytes) { commands.append(bytes); return true; }, {}});
    std::optional<bool> observed;
    int observations = 0;
    QObject::connect(&backend, &IRadioBackend::transmitChanged, &application,
        [&](const TransmitDelta& delta) { if (delta.cwSidetone) { observed = *delta.cwSidetone; ++observations; } });
    const auto edit = [&](bool enabled) { return backend.requestTransmitControl({Field::CwSidetone, enabled}); };
    KenwoodBackendTestAccess::authenticate(backend);
    KenwoodBackendTestAccess::message(backend, "ID024");
    AetherSDR::kenwood::KenwoodBackendTestAccess::message(backend, "XV0");
    require(commands.contains("EX00102;") && !commands.contains("EX00102 000;"), "startup reads native sidetone without enabling or restoring it");
    commands.clear();
    require(!backend.capabilities().radioTransmitSettings->fields.contains(Field::CwSidetone)
        && edit(true) == ReceiveDispatch::Unsupported && commands.isEmpty(), "unknown sidetone is not presented as an implemented writable setting");
    KenwoodBackendTestAccess::message(backend, "EX00102 000");
    require(observed == true && backend.capabilities().radioTransmitSettings->fields.contains(Field::CwSidetone)
        && !backend.capabilities().radioTransmitSettings->fields.contains(Field::CwMonitorGain),
        "monitor-linked native sidetone exposes its switch without inventing a percent gain mapping");
    for (int level = 2; level <= 21; ++level) {
        KenwoodBackendTestAccess::message(backend, "EX00102 " + QByteArray::number(level).rightJustified(3, '0'));
        commands.clear();
        require(observed == true && edit(true) == ReceiveDispatch::Dispatched && commands == QList<QByteArray>{"EX00102;"},
            "already enabled native fixed volume is preserved by an enable request");
    }
    commands.clear();
    require(edit(false) == ReceiveDispatch::Dispatched && commands == QList<QByteArray>{"EX00102 001;", "EX00102;"}
        && observed == true, "sidetone Off is explicit and waits for radio confirmation");
    KenwoodBackendTestAccess::message(backend, "EX00102 001");
    require(observed == false, "native Off readback updates the shared sidetone switch");
    commands.clear();
    require(edit(true) == ReceiveDispatch::Dispatched && commands == QList<QByteArray>{"EX00102 000;", "EX00102;"}
        && observed == false, "enabling from Off selects the documented monitor-linked native value without optimistic state");
    KenwoodBackendTestAccess::message(backend, "EX00102 000");
    const int beforeMalformed = observations;
    for (const QByteArray& message : {QByteArray("EX00102 022"), QByteArray("EX00102 999"),
            QByteArray("EX00102 00A"), QByteArray("EX00102 0010"), QByteArray("EX00102001")}) {
        KenwoodBackendTestAccess::message(backend, message);
    }
    require(observations == beforeMalformed && observed == true, "malformed native sidetone reports cannot change confirmed state");
    commands.clear();
    const qint64 pollAt = KenwoodBackendTestAccess::nextSidetonePoll(backend);
    KenwoodBackendTestAccess::pollSidetone(backend, pollAt - 1);
    require(commands.isEmpty(), "sidetone poll is bounded by its deadline");
    KenwoodBackendTestAccess::pollSidetone(backend, pollAt);
    KenwoodBackendTestAccess::pollSidetone(backend, pollAt);
    require(commands == QList<QByteArray>{"EX00102;", "EX00502;", "EX00503;", "EX00505;"}, "CW preference refresh is read-only and runs once per interval");
    backend.disconnectRadio();
    KenwoodBackendTestAccess::authenticate(backend);
    KenwoodBackendTestAccess::message(backend, "ID024");
    AetherSDR::kenwood::KenwoodBackendTestAccess::message(backend, "XV0");
    commands.clear();
    require(!backend.capabilities().radioTransmitSettings->fields.contains(Field::CwSidetone)
        && edit(true) == ReceiveDispatch::Unsupported && commands.isEmpty(), "reconnect cannot reuse old sidetone preferences or writability");
}

void verifyCwBfo(QCoreApplication& application)
{
    using namespace AetherSDR;
    using namespace AetherSDR::kenwood;
    using Field = TransmitControlRequest::Field;
    QList<QByteArray> commands;
    KenwoodBackend backend(KenwoodBackend::Transport{
        [&](const QByteArray& bytes) { commands.append(bytes); return true; }, {}});
    std::optional<bool> lower;
    int observations = 0;
    int receiveModes = 0;
    QObject::connect(&backend, &IRadioBackend::transmitChanged, &application,
        [&](const TransmitDelta& delta) { if (delta.cwlEnabled) { lower = *delta.cwlEnabled; ++observations; } });
    QObject::connect(&backend, &IRadioBackend::sliceChanged, &application,
        [&](int, const SliceDelta& delta) { if (delta.mode) { ++receiveModes; } });
    const auto edit = [&](bool enabled) { return backend.requestTransmitControl({Field::CwlEnabled, enabled}); };
    KenwoodBackendTestAccess::authenticate(backend);
    KenwoodBackendTestAccess::message(backend, "ID024");
    AetherSDR::kenwood::KenwoodBackendTestAccess::message(backend, "XV0");
    require(commands.contains("EX00505;") && !commands.contains("EX00505 000;") && !commands.contains("EX00506;"),
        "startup reads BFO sideband without restoring a default or touching automatic CW transmission");
    commands.clear();
    require(edit(true) == ReceiveDispatch::Unsupported && commands.isEmpty(), "unknown native BFO sideband is not writable");
    KenwoodBackendTestAccess::message(backend, "EX00505 000");
    require(lower == false && backend.capabilities().radioTransmitSettings->fields.contains(Field::CwlEnabled),
        "native USB BFO reports upper sideband and declares the existing setting field");
    commands.clear();
    require(edit(true) == ReceiveDispatch::Dispatched && commands == QList<QByteArray>{"EX00505 001;", "EX00505;"}
        && lower == false, "CWL intent changes only native BFO sideband and waits for readback");
    KenwoodBackendTestAccess::message(backend, "EX00505 001");
    require(lower == true && receiveModes == 0, "lower BFO readback does not masquerade as a physical VFO mode change");
    commands.clear();
    require(edit(true) == ReceiveDispatch::Dispatched && commands == QList<QByteArray>{"EX00505 001;", "EX00505;"},
        "explicit BFO selection reasserts intent even when a front-panel change could lag cached state");
    commands.clear();
    require(edit(false) == ReceiveDispatch::Dispatched && commands == QList<QByteArray>{"EX00505 000;", "EX00505;"}
        && lower == true, "CWU intent uses USB BFO and remains radio-authoritative");
    KenwoodBackendTestAccess::message(backend, "EX00505 000");
    const int beforeInvalid = observations;
    for (const QByteArray& message : {QByteArray("EX00505 002"), QByteArray("EX00505 999"), QByteArray("EX00505 00A"), QByteArray("EX00505 0000")}) {
        KenwoodBackendTestAccess::message(backend, message);
    }
    require(observations == beforeInvalid && lower == false, "invalid BFO reports cannot alter native sideband state");
    backend.disconnectRadio();
    KenwoodBackendTestAccess::authenticate(backend);
    KenwoodBackendTestAccess::message(backend, "ID024");
    AetherSDR::kenwood::KenwoodBackendTestAccess::message(backend, "XV0");
    commands.clear();
    require(!backend.capabilities().radioTransmitSettings->fields.contains(Field::CwlEnabled)
        && edit(true) == ReceiveDispatch::Unsupported && commands.isEmpty(), "reconnect cannot restore a cached CW BFO preference");
}

void verifyNormalPowerLimits()
{
    using namespace AetherSDR;
    using namespace AetherSDR::kenwood;
    QList<QByteArray> commands;
    KenwoodBackend backend(KenwoodBackend::Transport{
        [&](const QByteArray& bytes) { commands.append(bytes); return true; }, {}});
    KenwoodBackendTestAccess::authenticate(backend);
    KenwoodBackendTestAccess::message(backend, "ID024");
    AetherSDR::kenwood::KenwoodBackendTestAccess::message(backend, "XV0");
    KenwoodBackendTestAccess::message(backend, "AN1000");
    KenwoodBackendTestAccess::message(backend, "FT0");
    const auto edit = [&](int watts) {
        return backend.requestTransmitControl({TransmitControlRequest::Field::RfPower, watts});
    };
    commands.clear();
    require(edit(20) == ReceiveDispatch::Unsupported && commands.isEmpty(), "power edit requires physical TX mode/frequency");
    const QList<QPair<QByteArray, int>> cases{
        {"SF0000140740002", 100}, {"SF0000140740005", 25},
        {"SF000014074000F", 25}, {"SF0000701000002", 50},
        {"SF0000701000005", 13}, {"SF000070100000F", 13}};
    for (const auto& item : cases) {
        KenwoodBackendTestAccess::message(backend, item.first);
        commands.clear();
        require(edit(item.second) == ReceiveDispatch::Dispatched
            && commands == QList<QByteArray>{"PC" + QByteArray::number(item.second).rightJustified(3, '0') + ';', "PC;"},
            "physical TX mode and band select the documented PC ceiling");
        commands.clear();
        backend.setTxPower(item.second + 1);
        require(edit(item.second + 1) == ReceiveDispatch::Unsupported && commands.isEmpty(),
            "both direct and typed power edits refuse values above the native ceiling");
    }
    KenwoodBackendTestAccess::message(backend, "FR0");
    KenwoodBackendTestAccess::message(backend, "SF0000140740002");
    KenwoodBackendTestAccess::message(backend, "FT1");
    commands.clear();
    require(edit(10) == ReceiveDispatch::Unsupported && commands.isEmpty(),
        "TX VFO switch waits for fresh physical mode instead of using the receive VFO");
    KenwoodBackendTestAccess::message(backend, "SF1000701000005");
    commands.clear();
    require(edit(25) == ReceiveDispatch::Unsupported && commands.isEmpty(),
        "split TX on 70 MHz AM cannot inherit HF USB receive power");
    require(edit(13) == ReceiveDispatch::Dispatched && commands == QList<QByteArray>{"PC013;", "PC;"},
        "split power uses physical transmit mode and frequency together");
    KenwoodBackendTestAccess::message(backend, "SF0000701000005");
    KenwoodBackendTestAccess::message(backend, "SF1000140740002");
    commands.clear();
    require(edit(100) == ReceiveDispatch::Dispatched && commands == QList<QByteArray>{"PC100;", "PC;"},
        "receive-side AM ceiling cannot restrict HF USB transmission on the other VFO");
    KenwoodBackendTestAccess::message(backend, "AN1010");
    commands.clear();
    require(edit(10) == ReceiveDispatch::Unsupported && commands.isEmpty(),
        "drive-output selection withdraws watt edits even with known split TX state");
    KenwoodBackendTestAccess::message(backend, "AN1000");
    require(backend.capabilities().transmitDriveControl.has_value(), "normal output restores qualified watt editing");
    KenwoodBackendTestAccess::message(backend, "FT3");
    commands.clear();
    require(edit(10) == ReceiveDispatch::Unsupported && commands.isEmpty(), "memory TX source withdraws watt edits");
}

void verifyFiniteAudioCompletion()
{
    using namespace AetherSDR;
    using namespace AetherSDR::kenwood;
    for (int rate : {24000, 48000}) {
        for (bool cancel : {false, true}) {
            QList<QByteArray> packets;
            KenwoodBackend backend(KenwoodBackend::Transport{[](const QByteArray&) { return true; },
                [&](const QByteArray& packet) { packets.append(packet); return true; }});
            KenwoodBackendTestAccess::authenticate(backend);
            for (const QByteArray& status : {QByteArray("ID024"), QByteArray("XV0"), QByteArray("##TI1"), QByteArray("##UE1"), QByteArray("MS103")}) {
                KenwoodBackendTestAccess::message(backend, status);
            }
            backend.observePcAudioRouting(true);
            TxCoordinator coordinator([](const TxCoordinator::Operation&, TxCoordinator::StopReason) {});
            const TxCoordinator::Actor actor = coordinator.registerActor({true, 0});
            const TxCoordinator::Operation operation = coordinator.acquire(actor, TxCoordinator::monotonicMs()).operation;
            const TxCoordinator::Producer producer = coordinator.registerProducer();
            const TxCoordinator::Context media = coordinator.mediaContext(producer, operation);
            backend.setKeying(true, operation);
            const int frames = rate / 50;
            QByteArray pcm(frames * 4, '\0');
            for (int block = 0; block < 10; ++block) {
                if (block == 9) {
                    for (int frame = 0; frame < frames; ++frame) {
                        const qint16 sample = static_cast<qint16>(16000 * std::sin(2.0 * 3.141592653589793 * 1000 * frame / rate));
                        std::memcpy(pcm.data() + frame * 4, &sample, sizeof(sample));
                        std::memcpy(pcm.data() + frame * 4 + 2, &sample, sizeof(sample));
                    }
                }
                backend.submitTxAudio(pcm, rate, TxAudioSource::Microphone, media);
                if (block < 9) { KenwoodBackendTestAccess::pumpAudio(backend); }
            }
            const qsizetype before = packets.size();
            const int allowance = backend.finishTxAudio(media);
            require(allowance > 0 && allowance <= 100, "finite audio tail fits the existing bounded pacing window");
            require(backend.finishTxAudio(media) == allowance, "repeated EOF reports the same queue without appending another tail");
            if (cancel) { require(coordinator.cancel(actor, operation), "explicit operation cancellation"); }
            for (int packet = 0; packet < allowance / 20; ++packet) { KenwoodBackendTestAccess::pumpAudio(backend); }
            require(KenwoodBackendTestAccess::queuedSamples(backend) == 0, "finite output queue drains completely");
            if (cancel) {
                require(packets.size() == before, "cancelled finite media cannot emit its delayed tail");
                continue;
            }
            int lastSignal = -1;
            for (qsizetype packet = 0; packet < packets.size(); ++packet) {
                require(packets[packet].size() == 652, "finite output retains fixed KNS packet framing");
                for (int sample = 0; sample < 320; ++sample) {
                    const int value = qFromBigEndian<quint16>(packets[packet].constData() + 12 + sample * 2) - 32768;
                    if (std::abs(value) > 20) { lastSignal = static_cast<int>(packet * 320) + sample; }
                }
            }
            require(packets.size() > before && lastSignal > 3400,
                "captured TX packets preserve the terminal tone beyond the unflushed input duration");
        }
    }
}

void verifyDspModeQualification()
{
    using namespace AetherSDR;
    using namespace AetherSDR::kenwood;
    using Feature = ReceiveDspControl::Feature;
    QList<QByteArray> commands;
    KenwoodBackend backend(KenwoodBackend::Transport{
        [&](const QByteArray& bytes) { commands.append(bytes); return true; }, {}});
    KenwoodBackendTestAccess::authenticate(backend);
    KenwoodBackendTestAccess::message(backend, "ID024");
    AetherSDR::kenwood::KenwoodBackendTestAccess::message(backend, "XV0");
    require(!commands.contains("GC;"), "startup does not query AGC before a physical receive mode is known");
    KenwoodBackendTestAccess::message(backend, "FR0");
    const auto rejectedWhileUnknown = [&] {
        commands.clear();
        backend.setSliceAgc(0, "fast", 0);
        require(backend.requestSliceDsp(0, {SliceDspRequest::Feature::Nb, SliceDspRequest::Field::Enabled, true})
                == ReceiveDispatch::Unsupported
            && backend.requestSliceDsp(0, {SliceDspRequest::Feature::Nr, SliceDspRequest::Field::Enabled, true})
                == ReceiveDispatch::Unsupported
            && backend.requestSliceDsp(0, {SliceDspRequest::Feature::Anf, SliceDspRequest::Field::Enabled, true})
                == ReceiveDispatch::Unsupported && commands.isEmpty(),
            "unqualified physical receive mode cannot dispatch DSP settings");
        const std::optional<ReceiveDspControl> controls = backend.capabilities().receiveDspControl;
        require(controls && !controls->supports(Feature::Agc, "USB") && !controls->supports(Feature::Rit, "USB"),
            "unknown receive state withdraws DSP availability instead of retaining an old mode");
    };
    rejectedWhileUnknown();
    struct ModeCase { char code; QString name; bool nonFm; bool voice; };
    const QList<ModeCase> modes{{'1', "LSB", true, true}, {'2', "USB", true, true},
        {'3', "CW", true, false}, {'4', "FM", false, true}, {'5', "AM", true, true},
        {'6', "RTTY", true, false}, {'7', "CWR", true, false}, {'9', "RTTYR", true, false},
        {'A', "PSK", true, false}, {'B', "PSKR", true, false}, {'C', "DIGL", true, true},
        {'D', "DIGU", true, true}, {'E', "DFM", false, true}, {'F', "DAM", true, true}};
    for (const ModeCase& mode : modes) {
        commands.clear();
        KenwoodBackendTestAccess::message(backend, QByteArray("SF000014074000") + mode.code);
        require(commands.contains("GC;") == mode.nonFm,
            "AGC observation is queried only after a non-FM physical receive mode is confirmed");
        const std::optional<ReceiveDspControl> controls = backend.capabilities().receiveDspControl;
        require(controls && controls->supports(Feature::Agc, mode.name) == mode.nonFm
            && controls->supports(Feature::Nb, mode.name) == mode.nonFm
            && controls->supports(Feature::Anf, mode.name) == mode.voice
            && controls->supports(Feature::Nr, mode.name) && controls->supports(Feature::Rit, mode.name)
            && !controls->supports(Feature::ManualNotch, mode.name) && !controls->supports(Feature::Apf, mode.name),
            "DSP capabilities retain implemented controls and qualify documented mode restrictions including data variants");
        commands.clear();
        backend.setSliceAgc(0, "fast", 0);
        require(commands == (mode.nonFm ? QList<QByteArray>{"GC3;", "GC;"} : QList<QByteArray>{}),
            "FM and data-FM cannot write AGC while valid modes keep native AGC writes");
        commands.clear();
        const ReceiveDispatch blanker = backend.requestSliceDsp(0,
            {SliceDspRequest::Feature::Nb, SliceDspRequest::Field::Enabled, true});
        require(blanker == (mode.nonFm ? ReceiveDispatch::Dispatched : ReceiveDispatch::Unsupported)
            && commands == (mode.nonFm ? QList<QByteArray>{"NB11;", "NB1;"} : QList<QByteArray>{}),
            "noise blanker enable is unavailable in FM and data-FM at the dispatch boundary");
        commands.clear();
        const ReceiveDispatch blankerLevel = backend.requestSliceDsp(0,
            {SliceDspRequest::Feature::Nb, SliceDspRequest::Field::Level, true, 50});
        require(blankerLevel == (mode.nonFm ? ReceiveDispatch::Dispatched : ReceiveDispatch::Unsupported)
            && commands == (mode.nonFm ? QList<QByteArray>{"NL1011;", "NL1;"} : QList<QByteArray>{}),
            "noise blanker level obeys the same mode restrictions as enable");
        commands.clear();
        require(backend.requestSliceDsp(0, {SliceDspRequest::Feature::Nr, SliceDspRequest::Field::Enabled, true})
            == ReceiveDispatch::Dispatched && commands == QList<QByteArray>{"NR1;", "NR;"},
            "NR1 remains available including FM and data-FM");
        commands.clear();
        const ReceiveDispatch beat = backend.requestSliceDsp(0,
            {SliceDspRequest::Feature::Anf, SliceDspRequest::Field::Enabled, true});
        require(beat == (mode.voice ? ReceiveDispatch::Dispatched : ReceiveDispatch::Unsupported)
            && commands == (mode.voice ? QList<QByteArray>{"BC1;", "BC;"} : QList<QByteArray>{}),
            "beat cancellation is limited to SSB, AM and FM including data variants");
        commands.clear();
        backend.setSliceRitEnabled(0, true);
        require(commands == QList<QByteArray>{"RT1;", "RT;"}, "valid RIT control remains implemented across receive modes");
    }
    KenwoodBackendTestAccess::message(backend, "OM03");
    rejectedWhileUnknown();
    KenwoodBackendTestAccess::message(backend, "SF1000070740002");
    rejectedWhileUnknown();
    KenwoodBackendTestAccess::message(backend, "SF0000140740002");
    require(backend.capabilities().receiveDspControl->supports(Feature::Agc, "USB"),
        "only fresh selected physical receive readback restores DSP availability");
    int publications = 0;
    QObject::connect(&backend, &IRadioBackend::capabilitiesChanged, &backend,
        [&] { ++publications; });
    for (const QByteArray& nr : {QByteArray("NR1"), QByteArray("NR2"), QByteArray("NR1")}) {
        KenwoodBackendTestAccess::message(backend, nr);
        const ReceiveDspControl controls = *backend.capabilities().receiveDspControl;
        const bool level = nr != "NR2";
        require(controls.supports(Feature::Nr, "USB") && controls.supportsLevel(Feature::Nr, "USB") == level,
            "NR2 retains enable support while withdrawing only its unmapped level field");
        commands.clear();
        require(backend.requestSliceDsp(0, {SliceDspRequest::Feature::Nr, SliceDspRequest::Field::Level, true, 50})
            == (level ? ReceiveDispatch::Dispatched : ReceiveDispatch::Unsupported), "NR level capability matches dispatch");
        require(commands.isEmpty() != level, "only mapped NR1 level emits a command");
    }
    require(publications == 3, "algorithm readback refreshes field availability on existing widgets");
}

}

void verifyReceiveAntennaAdmission()
{
    using namespace AetherSDR;
    using namespace AetherSDR::kenwood;
    const QStringList cases{"identity", "rx", "tf", "mode", "source", "tx", "tfset", "key", "cw", "atu", "tune", "disconnected"};
    for (const QString& denied : cases) {
        QList<QByteArray> commands;
        KenwoodBackend backend(KenwoodBackend::Transport{[&](const QByteArray& bytes) { commands.append(bytes); return true; }, {}});
        KenwoodBackendTestAccess::authenticate(backend);
        if (denied != "identity") { KenwoodBackendTestAccess::message(backend, "ID024");
    AetherSDR::kenwood::KenwoodBackendTestAccess::message(backend, "XV0"); }
        for (const QByteArray& truth : {QByteArray("FR0"), QByteArray("FT1"), QByteArray("FA00014074000"), QByteArray("OM03")}) {
            KenwoodBackendTestAccess::message(backend, truth);
        }
        if (denied != "mode") { KenwoodBackendTestAccess::message(backend, "SF0000140740003"); }
        if (denied != "tf") { KenwoodBackendTestAccess::message(backend, "TS0"); }
        if (denied != "rx") { KenwoodBackendTestAccess::message(backend, "RX"); }
        if (denied == "source") { KenwoodBackendTestAccess::message(backend, "FR3"); }
        if (denied == "tx") { KenwoodBackendTestAccess::message(backend, "TX0"); }
        if (denied == "tfset") { KenwoodBackendTestAccess::message(backend, "TS1"); }
        if (denied == "key") { KenwoodBackendTestAccess::antennaIntent(backend, 0); }
        if (denied == "cw") { KenwoodBackendTestAccess::antennaIntent(backend, 1); }
        if (denied == "atu") { KenwoodBackendTestAccess::antennaIntent(backend, 2); }
        if (denied == "tune") { KenwoodBackendTestAccess::antennaIntent(backend, 3); }
        if (denied == "disconnected") { backend.disconnectRadio(); }
        commands.clear();
        const ReceiveDispatch result = backend.requestSliceRxAntenna(0, QStringLiteral("RX-ANT"));
        if (result != ReceiveDispatch::Unsupported || !commands.isEmpty()) { std::cerr << "antenna admission case=" << denied.toStdString() << '\n'; }
        require(result == ReceiveDispatch::Unsupported && commands.isEmpty(), "antenna route requires complete confirmed RX and no local transmit intent");
    }
    QList<QByteArray> commands;
    KenwoodBackend backend(KenwoodBackend::Transport{[&](const QByteArray& bytes) { commands.append(bytes); return true; }, {}});
    KenwoodBackendTestAccess::authenticate(backend);
    for (const QByteArray& truth : {QByteArray("ID024"), QByteArray("XV0"), QByteArray("FR0"), QByteArray("FT1"), QByteArray("FA00014074000"),
            QByteArray("OM03"), QByteArray("SF0000140740003"), QByteArray("TS0"), QByteArray("RX")}) { KenwoodBackendTestAccess::message(backend, truth); }
    for (const auto& selection : {qMakePair(QStringLiteral("ANT1"), QByteArray("AN1099;")),
            qMakePair(QStringLiteral("ANT2"), QByteArray("AN2099;")), qMakePair(QStringLiteral("RX-ANT"), QByteArray("AN9199;"))}) {
        commands.clear();
        require(backend.requestSliceRxAntenna(0, selection.first) == ReceiveDispatch::Dispatched
            && commands == QList<QByteArray>{selection.second, "AN;"}, "qualified antenna route preserves existing mixed-main and isolated-RX wire semantics");
    }
    commands.clear();
    require(backend.requestSliceRxAntenna(1, "RX-ANT") == ReceiveDispatch::Unsupported
        && backend.requestSliceRxAntenna(0, "ANT3") == ReceiveDispatch::Unsupported && commands.isEmpty(), "invalid antenna or receiver cannot dispatch");
}


void verifyFilterProducerChronology()
{
    using Backend = AetherSDR::kenwood::KenwoodBackend;
    using Probe = AetherSDR::kenwood::KenwoodBackendTestAccess;
    Backend backend(Backend::Transport{[](const QByteArray&) { return true; }, {}});
    const QVariant initial = Probe::chronologyStatus(backend).value("statusRevision");
    require(initial.typeId() == QMetaType::QString && initial.toString() == "0", "filter initial getter has canonical QString revision zero");
    Probe::authenticate(backend);
    for (const QByteArray& bytes : {QByteArray("ID024"), QByteArray("XV0"), QByteArray("FR0"), QByteArray("FA00014074000"), QByteArray("SF0000140740003"), QByteArray("TS0"), QByteArray("RX")}) { Probe::chronologyFeed(backend, bytes); }
    bool nested = false; QList<QVariantMap> delivered;
    QObject::connect(&backend, &AetherSDR::IRadioBackend::extensionStatus, &backend,
        [&](const QString&, const QString& statusKind, const QVariantMap&) {
            if (statusKind != "filter" || nested) { return; }
            nested = true; Probe::chronologyFeed(backend, "TS1");
        });
    QObject::connect(&backend, &AetherSDR::IRadioBackend::extensionStatus, &backend,
        [&](const QString&, const QString& statusKind, const QVariantMap& state) {
            if (statusKind == "filter") { delivered.append(state); }
        });
    Probe::chronologyPublish(backend);
    require(nested && delivered.size() >= 2, "filter real TF withdrawal publishes newer status during outer delivery");
    const quint64 newer = delivered.first().value("statusRevision").toString().toULongLong();
    const quint64 older = delivered.last().value("statusRevision").toString().toULongLong();
    require(newer > older, "filter producer stamps snapshot before nested newer delivery");
    for (const QVariantMap& state : delivered) {
        const QVariant revision = state.value("statusRevision");
        require(revision.typeId() == QMetaType::QString && revision.toString() == QString::number(revision.toString().toULongLong()), "filter every revision is canonical decimal QString");
    }
    QVariantMap queried;
    QObject::connect(&backend, &AetherSDR::IRadioBackend::extensionResult, &backend,
        [&](quint64 request, const QVariant& result) { if (request == 901) { queried = result.toMap(); } });
    backend.invokeExtension("kenwood", "filter.get", 901);
    require(queried.value("statusRevision") == Probe::chronologyStatus(backend).value("statusRevision")
        && queried.value("statusRevision").toString().toULongLong() >= newer, "filter GET carries latest producer revision after stale outer delivery");
    const quint64 beforeReset = queried.value("statusRevision").toString().toULongLong();
    backend.disconnectRadio(); Probe::chronologyPublish(backend);
    require(Probe::chronologyStatus(backend).value("statusRevision").toString().toULongLong() > beforeReset, "filter disconnect never resets lifetime publication chronology");
}


void verifyVfoProducerChronology()
{
    using Backend = AetherSDR::kenwood::KenwoodBackend;
    using Probe = AetherSDR::kenwood::KenwoodBackendTestAccess;
    Backend backend(Backend::Transport{[](const QByteArray&) { return true; }, {}});
    const QVariant initial = Probe::vfoChronologyStatus(backend).value("statusRevision");
    require(initial.typeId() == QMetaType::QString && initial.toString() == "0", "VFO initial getter has canonical QString revision zero");
    Probe::authenticate(backend);
    for (const QByteArray& bytes : {QByteArray("ID024"), QByteArray("XV0"), QByteArray("FR0"), QByteArray("FT1"), QByteArray("FA00014074000"), QByteArray("FB00007074000"), QByteArray("SF0000140740003"), QByteArray("TS0"), QByteArray("RX")}) { Probe::message(backend, bytes); }
    bool nested = false; QList<QVariantMap> delivered;
    QObject::connect(&backend, &AetherSDR::IRadioBackend::extensionStatus, &backend,
        [&](const QString&, const QString& kind, const QVariantMap&) {
            if (kind != "vfo" || nested) { return; }
            nested = true; Probe::message(backend, "FA00014076000");
        });
    QObject::connect(&backend, &AetherSDR::IRadioBackend::extensionStatus, &backend,
        [&](const QString&, const QString& kind, const QVariantMap& state) { if (kind == "vfo") { delivered.append(state); } });
    Probe::message(backend, "FA00014075000");
    require(nested && delivered.size() == 2, "VFO actual frequency readback produces nested newer status during outer delivery");
    const quint64 newer = delivered.first().value("statusRevision").toString().toULongLong();
    const quint64 older = delivered.last().value("statusRevision").toString().toULongLong();
    require(newer > older && delivered.first().value("frequencyAHz").toLongLong() == 14076000
        && delivered.last().value("frequencyAHz").toLongLong() == 14075000, "VFO producer stamps snapshot before nested newer frequency delivery");
    for (const QVariantMap& state : delivered) {
        const QVariant revision = state.value("statusRevision");
        require(revision.typeId() == QMetaType::QString && revision.toString() == QString::number(revision.toString().toULongLong()), "VFO revisions are canonical decimal QString");
    }
    QVariantMap queried;
    QObject::connect(&backend, &AetherSDR::IRadioBackend::extensionResult, &backend,
        [&](quint64 request, const QVariant& result) { if (request == 902) { queried = result.toMap(); } });
    backend.invokeExtension("kenwood", "vfo.get", 902);
    require(queried.value("statusRevision").toString().toULongLong() == newer
        && queried.value("frequencyAHz").toLongLong() == 14076000, "VFO GET retains newest producer revision and observed frequency after stale outer delivery");
    backend.disconnectRadio();
    require(Probe::vfoChronologyStatus(backend).value("statusRevision").toString().toULongLong() >= newer, "VFO reset never resets lifetime chronology");
}


void verifyAudioOutputProducerChronology()
{
    using Backend = AetherSDR::kenwood::KenwoodBackend;
    using Probe = AetherSDR::kenwood::KenwoodBackendTestAccess;
    QList<QByteArray> commands;
    Backend backend(Backend::Transport{[&](const QByteArray& bytes) { commands.append(bytes); return true; }, {}});
    const QVariant initial = Probe::audioChronologyStatus(backend).value("statusRevision");
    require(initial.typeId() == QMetaType::QString && initial.toString() == "0", "audio status initial getter exposes canonical QString revision zero");
    Probe::authenticate(backend); Probe::message(backend, "ID024");
    AetherSDR::kenwood::KenwoodBackendTestAccess::message(backend, "XV0"); commands.clear();
    bool nested = false; QList<QVariantMap> delivered;
    QObject::connect(&backend, &AetherSDR::IRadioBackend::extensionStatus, &backend,
        [&](const QString&, const QString& kind, const QVariantMap& state) {
            if (kind != "audio.levels" || nested || state.value("outputLevel").toInt() != 60) { return; }
            nested = true; Probe::message(backend, "##KN31070");
        });
    QObject::connect(&backend, &AetherSDR::IRadioBackend::extensionStatus, &backend,
        [&](const QString&, const QString& kind, const QVariantMap& state) { if (kind == "audio.levels") { delivered.append(state); } });
    Probe::message(backend, "##KN31060");
    require(nested && delivered.size() == 2, "real output readback produces nested newer snapshot during outer delivery");
    const quint64 newer = delivered.first().value("statusRevision").toString().toULongLong();
    const quint64 older = delivered.last().value("statusRevision").toString().toULongLong();
    require(newer > older && delivered.first().value("outputLevel").toInt() == 70
        && delivered.last().value("outputLevel").toInt() == 60, "audio producer stamps snapshot before callbacks so newer output is distinguishable");
    for (const QVariantMap& state : delivered) {
        const QVariant revision = state.value("statusRevision");
        require(revision.typeId() == QMetaType::QString && revision.toString() == QString::number(revision.toString().toULongLong()), "audio revisions are canonical decimal QString");
    }
    QVariantMap queried;
    QObject::connect(&backend, &AetherSDR::IRadioBackend::extensionResult, &backend,
        [&](quint64 id, const QVariant& result) { if (id == 903) { queried = result.toMap(); } });
    backend.invokeExtension("kenwood", "audio.levels.get", 903);
    require(queried.value("outputLevel").toInt() == 70 && queried.value("statusRevision").toString().toULongLong() == newer,
        "audio GET returns latest observed output and publication revision without increment");
    const QVariantMap beforeMalformed = Probe::audioChronologyStatus(backend);
    for (const QByteArray& bytes : {QByteArray("##KN31101"), QByteArray("##KN31x70"), QByteArray("##KN3107"), QByteArray("##KN310700")}) { Probe::message(backend, bytes); }
    require(Probe::audioChronologyStatus(backend) == beforeMalformed, "malformed output reports neither change observation nor invent publication");
    require(commands.isEmpty() && !queried.contains("inputLevel"), "chronology/readback test issues no setters or input mutation");
    backend.disconnectRadio();
    require(Probe::audioChronologyStatus(backend).value("statusRevision").toString().toULongLong() >= newer,
        "audio session reset never resets lifetime chronology");
    Probe::audioChronologyPublish(backend);
    require(Probe::audioChronologyStatus(backend).value("statusRevision").toString().toULongLong() > newer,
        "post-reset audio publication advances chronology without asserting known levels");
}

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("kenwood-backend"));
    QCoreApplication application(argc, argv);
    verifyAudioOutputProducerChronology();
    verifyVfoProducerChronology();
    verifyFilterProducerChronology();
    verifyPowerSessionRetirement(application);
    verifyTimerControlRetirement(application);
    verifySharedNativeApf(application);
    verifyFirmwareObservation(application);
    verifyReceiveIntentRouting();
    verifyNativeGainMuteRoutes();
    verifyReceiveAntennaAdmission();
    verifyMeasuredScopeSpans();
    verifyInitialStatusAndScope();
    verifyFiniteAudioCompletion();
    verifyDspModeQualification();
    verifyNormalPowerLimits();
    verifyNetworkAudioLevels(application);
    verifyTransmitInputs(application);
    verifyNativeTunePower(application);
    verifyTunePreflight(application);
    verifyPowerLimits(application);
    verifyTransmitVfo(application);
    verifyTransmitFilter(application);
    verifyReceiveVfo(application);
    verifyReceiveModeControl(application);
    verifyMemorySources(application);
    verifyConnectionConfiguration();
    verifyRadioSidetone(application);
    verifyCwBfo(application);
    using namespace AetherSDR;
    using namespace AetherSDR::kenwood;
    KenwoodBackend backend;
    QStringList order;
    int slices = 0;
    int spectra = 0;
    QVariantMap vfoState;
    QObject::connect(&backend, &IRadioBackend::extensionStatus, &application,
        [&](const QString& ns, const QString& kind, const QVariantMap& state) {
            if (ns == "kenwood" && kind == "vfo") { vfoState = state; }
        });
    double frequency = 0;
    double bandwidth = 0;
    QList<PcmFrame> receivedAudio;
    QObject::connect(&backend, &IRadioBackend::audioFrameReady, &application,
        [&](const PcmFrame& frame) { receivedAudio.append(frame); });
    QObject::connect(&backend, &IRadioBackend::connected, &application, [&] { order.append("connected"); });
    QObject::connect(&backend, &IRadioBackend::connectionError, &application, [&] { order.append("error"); });
    QObject::connect(&backend, &IRadioBackend::disconnected, &application, [&] { order.append("disconnected"); });
    QObject::connect(&backend, &IRadioBackend::sliceChanged, &application, [&](int id, const SliceDelta& delta) {
        require(id == 0, "one physical receiver");
        ++slices;
        if (delta.frequency) { frequency = *delta.frequency; }
    });
    QObject::connect(&backend, &IRadioBackend::panCenterBandwidthChanged, &application,
        [&](const QString&, double, double width) { bandwidth = width; });
    QObject::connect(&backend, &IRadioBackend::spectrumFrameReady, &application,
        [&](int, const QByteArray& frame, const SpectrumCoverage&) {
            require(frame.size() == 640 * sizeof(float), "bounded float spectrum frame");
            ++spectra;
        });
    const RadioCapabilities caps = backend.capabilities();
    require(caps.family == "kenwood" && caps.maxSlices == 1 && !caps.canTransmit, "conservative initial capability");
    require(!caps.dbmAxisIsCalibrated() && !caps.panBinsAbsolute() && !caps.radioOwnsDbmScale,
        "relative bins cannot claim calibrated antenna dBm or automatic scale ownership");
    require(!caps.clientSettingsDomains, "radio owns saved operating state");
    KenwoodBackendTestAccess::authenticate(backend);
    require(backend.isConnected() && order == QStringList{"connected"} && slices == 1, "connect precedes slice publication");
    KenwoodBackendTestAccess::message(backend, "ID024");
    AetherSDR::kenwood::KenwoodBackendTestAccess::message(backend, "XV0");
    require(backend.capabilities().receiveFilterControl->modes.isEmpty(),
        "unknown DSP monitor cannot advertise effective filter control");
    KenwoodBackendTestAccess::message(backend, "MO20");
    KenwoodBackend filterCapabilities(KenwoodBackend::Transport{[](const QByteArray&) { return true; }, {}});
    KenwoodBackendTestAccess::authenticate(filterCapabilities);
    for (const QByteArray& frame : {QByteArray("ID024"), QByteArray("XV0"), QByteArray("FR0"),
            QByteArray("SF0000140740003"), QByteArray("TS0"), QByteArray("RX"), QByteArray("MO20"), QByteArray("SL006"), QByteArray("SH0080")}) {
        KenwoodBackendTestAccess::message(filterCapabilities, frame);
    }
    require(filterCapabilities.capabilities().receiveFilterControl
        && filterCapabilities.capabilities().receiveFilterControl->modePolicy == ReceiveFilterControl::ModePolicy::ListedOnly
        && filterCapabilities.capabilities().receiveFilterControl->modes.size() == 2,
        "unknown SSB filter style cannot advertise an editable SSB passband");
    KenwoodBackendTestAccess::message(filterCapabilities, "EX00611 000");
    require(filterCapabilities.capabilities().receiveFilterControl->modes.size() == 4,
        "confirmed SSB filter style advertises its two native sidebands");
    KenwoodBackendTestAccess::message(filterCapabilities, "EX00612 001");
    require(filterCapabilities.capabilities().receiveFilterControl->modes.size() == 6,
        "confirmed data filter style adds only mapped data sidebands");
    KenwoodBackendTestAccess::message(backend, "FA00014074000");
    require(frequency == 0, "frequency waits for confirmed receive VFO");
    require(!vfoState.contains("split"), "unknown VFO selection cannot invent simplex state");
    KenwoodBackendTestAccess::message(backend, "FR0");
    require(frequency == 14.074, "adopt confirmed VFO A");
    KenwoodBackendTestAccess::message(backend, "BS30");
    KenwoodBackendTestAccess::message(backend, "BSK4");
    KenwoodBackendTestAccess::message(backend, "BS42");
    require(bandwidth == 0.02, "hardware-qualified BS4 index 2 is 20 kHz");
    const QByteArray line = "##DD2" + QByteArray("46").repeated(640);
    KenwoodBackendTestAccess::message(backend, line);
    require(spectra == 0, "unknown EXPAND state cannot misplace spectrum");
    KenwoodBackendTestAccess::message(backend, "BSO0");
    KenwoodBackendTestAccess::message(backend, line);
    require(spectra == 1, "confirmed nonexpanded center scope publishes");
    KenwoodBackendTestAccess::message(backend, "BSO1");
    KenwoodBackendTestAccess::message(backend, line);
    require(spectra == 1, "expanded unknown geometry withheld");
    KenwoodBackendTestAccess::message(backend, "FB00007074000");
    KenwoodBackendTestAccess::message(backend, "FR1");
    require(frequency == 7.074 && slices > 1, "VFO B changes same receiver");
    KenwoodBackendTestAccess::message(backend, "FT0");
    require(vfoState.value("receive") == "B" && vfoState.value("transmit") == "A"
        && vfoState.value("split").toBool() && vfoState.value("frequencyBHz").toLongLong() == 7074000,
        "native VFO status describes split without creating another receiver");
    QByteArray incomingAudio(652, '\0');
    incomingAudio[0] = static_cast<char>(0x80);
    incomingAudio[1] = 96;
    KenwoodBackendTestAccess::audio(backend, incomingAudio, 0);
    require(receivedAudio.size() == 1 && receivedAudio.first().stream().format.sampleRateHz == 24000,
        "16 kHz network audio reaches the typed 24 kHz playback stream");
    KenwoodBackendTestAccess::audio(backend, incomingAudio, 20);
    require(receivedAudio.size() == 1, "duplicate audio is not published twice");
    qToBigEndian<quint16>(2, incomingAudio.data() + 2);
    KenwoodBackendTestAccess::audio(backend, incomingAudio, 40);
    require(receivedAudio.size() == 2 && receivedAudio.last().discontinuity(), "packet loss marks a PCM discontinuity");
    KenwoodBackendTestAccess::fail(backend);
    require(order == QStringList{"connected", "error", "disconnected"}, "error precedes terminal disconnect");
    require(!receivedAudio.first().current(), "queued PCM leases retire at disconnect");
    const int retiredSlices = slices;
    KenwoodBackendTestAccess::message(backend, "FA00021074000");
    require(slices == retiredSlices && !backend.isConnected(), "retired session cannot publish");
    backend.disconnectRadio();
    require(order.count("disconnected") == 1, "disconnect emitted once");

    KenwoodBackend reentrant;
    int unexpectedSlices = 0;
    QObject::connect(&reentrant, &IRadioBackend::connected, &application, [&] { reentrant.disconnectRadio(); });
    QObject::connect(&reentrant, &IRadioBackend::sliceChanged, &application, [&](int, const SliceDelta&) { ++unexpectedSlices; });
    KenwoodBackendTestAccess::authenticate(reentrant);
    require(!reentrant.isConnected() && unexpectedSlices == 0, "reentrant disconnect stops startup publication");
    std::unique_ptr<KenwoodBackend> mortal = std::make_unique<KenwoodBackend>();
    QObject::connect(mortal.get(), &IRadioBackend::connected, &application, [&] { mortal.reset(); });
    KenwoodBackendTestAccess::authenticate(*mortal);
    require(!mortal, "destruction during connected publication is bounded");

    bool destroyOnFilterQuery = false;
    std::unique_ptr<KenwoodBackend> queryTarget;
    queryTarget = std::make_unique<KenwoodBackend>(KenwoodBackend::Transport{
        [&](const QByteArray& bytes) {
            if (destroyOnFilterQuery && bytes == "SL0;") { queryTarget.reset(); }
            return true;
        }, {}});
    KenwoodBackendTestAccess::authenticate(*queryTarget);
    KenwoodBackendTestAccess::message(*queryTarget, "ID024");
    KenwoodBackendTestAccess::message(*queryTarget, "XV0");
    KenwoodBackendTestAccess::message(*queryTarget, "MO20");
    destroyOnFilterQuery = true;
    KenwoodBackendTestAccess::message(*queryTarget, "FR0");
    KenwoodBackendTestAccess::message(*queryTarget, "SF0000140740002");
    require(!queryTarget, "destruction during filter query cannot dispatch a companion on a dead backend");

    bool destroyOnProcessorQuery = false;
    std::unique_ptr<KenwoodBackend> processorTarget;
    processorTarget = std::make_unique<KenwoodBackend>(KenwoodBackend::Transport{
        [&](const QByteArray& bytes) {
            if (destroyOnProcessorQuery && bytes == "PL;") { processorTarget.reset(); }
            return true;
        }, {}});
    KenwoodBackendTestAccess::authenticate(*processorTarget);
    KenwoodBackendTestAccess::message(*processorTarget, "ID024");
    KenwoodBackendTestAccess::message(*processorTarget, "XV0");
    KenwoodBackendTestAccess::message(*processorTarget, "PL020075");
    destroyOnProcessorQuery = true;
    processorTarget->setSpeechProcessor(true, 40);
    require(!processorTarget, "destruction during companion readback retires the whole processor edit");

    QList<QByteArray> commands;
    QList<QByteArray> datagrams;
    KenwoodBackend transmitter(KenwoodBackend::Transport{
        [&](const QByteArray& bytes) { commands.append(bytes); return true; },
        [&](const QByteArray& bytes) { datagrams.append(bytes); return true; }});
    KenwoodBackendTestAccess::authenticate(transmitter);
    for (const QByteArray& command : commands) {
        require(!command.startsWith("TX") && !command.startsWith("MS103") && !command.startsWith("AC111"),
            "startup cannot key or overwrite modulation input");
    }
    require(!transmitter.capabilities().canTransmit, "unknown account permissions deny transmit");
    KenwoodBackendTestAccess::message(transmitter, "##TI1");
    KenwoodBackendTestAccess::message(transmitter, "##UE1");
    require(!transmitter.capabilities().canTransmit, "account grants cannot substitute for model identity");
    KenwoodBackendTestAccess::message(transmitter, "ID024");
    KenwoodBackendTestAccess::message(transmitter, "XV0");
    require(transmitter.capabilities().canTransmit, "explicit radio account grants expose gated TX");
    require(transmitter.capabilities().tuneCarrierControl
        && transmitter.capabilities().tuneCarrierControl->modes.isEmpty(), "unverified TUNE power contract is explicitly unavailable");
    commands.clear();
    transmitter.setKeying(true, {});
    transmitter.setTune(true, 10, {});
    transmitter.setAtu(true, {});
    require(!commands.contains("TX0;") && !commands.contains("TX1;")
        && !commands.contains("TX2;") && !commands.contains("AC111;"), "missing operation never keys");
    for (const QByteArray& truth : {QByteArray("FR0"), QByteArray("FT1"), QByteArray("FA00014074000"),
            QByteArray("OM03"), QByteArray("SF0000140740003"), QByteArray("TS0"), QByteArray("RX")}) {
        KenwoodBackendTestAccess::message(transmitter, truth);
    }
    commands.clear();
    transmitter.requestSliceRxAntenna(0, QStringLiteral("RX-ANT"));
    require(commands == QList<QByteArray>{"AN9199;", "AN;"},
        "RX antenna edit preserves unrelated antenna settings with documented sentinel");
    commands.clear();
    transmitter.setSliceRitOffset(0, -125);
    require(commands == QList<QByteArray>{"RD00125;", "RF;"},
        "RIT uses absolute signed offset, not incremental repeats");
    commands.clear();
    transmitter.setSliceRitOffset(0, -10000);
    require(commands.isEmpty(), "out-of-range offset is rejected before wire emission");
    transmitter.requestSliceFilter(0, {300, 2700, SliceFilterRequest::Origin::ModeNormalization});
    require(commands.isEmpty(), "mode normalization cannot overwrite radio filter memory");
    transmitter.setSpeechProcessor(true, 40);
    require(commands.isEmpty(), "unknown processor output cannot be overwritten by input adjustment");
    KenwoodBackendTestAccess::message(transmitter, "PL020075");
    transmitter.setSpeechProcessor(true, 40);
    require(commands == QList<QByteArray>{"PL040075;", "PL;", "PR01;", "PR0;"},
        "processor input edit preserves radio-authoritative output level");
    commands.clear();
    require(transmitter.requestTransmitControl({TransmitControlRequest::Field::ProcessorEnabled,
        ProcessorSetting{false, 90}}) == ReceiveDispatch::Dispatched,
        "processor enable switch has its own dispatch");
    require(commands == QList<QByteArray>{"PR00;", "PR0;"},
        "processor switch cannot overwrite input or output levels with stale companions");
    commands.clear();
    transmitter.requestTransmitControl({TransmitControlRequest::Field::ProcessorLevel, ProcessorSetting{true, 45}});
    require(commands == QList<QByteArray>{"PL045075;", "PL;"},
        "processor input level cannot change the radio's enable switch");
    commands.clear();
    transmitter.requestTransmitControl({TransmitControlRequest::Field::MonitorEnabled, MonitorSetting{true, 80}});
    require(commands == QList<QByteArray>{"MO01;", "MO0;"},
        "monitor switch preserves its independent level");
    commands.clear();
    transmitter.requestTransmitControl({TransmitControlRequest::Field::MonitorLevel, MonitorSetting{false, 43}});
    require(commands == QList<QByteArray>{"ML009;", "ML;"},
        "monitor level quantizes to the native 0..20 scale without changing enable");
    int monitorLevel = -1;
    std::optional<bool> monitorEnabled;
    QObject::connect(&transmitter, &IRadioBackend::transmitChanged, &application,
        [&](const TransmitDelta& delta) {
            if (delta.monGainSb) { monitorLevel = *delta.monGainSb; }
            if (delta.sbMonitor) { monitorEnabled = *delta.sbMonitor; }
        });
    KenwoodBackendTestAccess::message(transmitter, "ML009");
    KenwoodBackendTestAccess::message(transmitter, "MO01");
    require(monitorLevel == 45 && monitorEnabled == true, "monitor readback publishes normalized radio state");
    KenwoodBackendTestAccess::message(transmitter, "ML021");
    KenwoodBackendTestAccess::message(transmitter, "MO02");
    require(monitorLevel == 45 && monitorEnabled == true, "out-of-range monitor reports cannot alter live state");
    commands.clear();
    require(transmitter.capabilities().cwDelayControl
        && transmitter.capabilities().cwDelayControl->maximumMs == 1000
        && transmitter.capabilities().cwDelayControl->stepMs == 50,
        "native keyer delay capability carries its actual range and step");
    require(transmitter.requestTransmitControl({TransmitControlRequest::Field::CwDelay, 975})
        == ReceiveDispatch::Dispatched && commands == QList<QByteArray>{"SD1000;", "SD;"},
        "CW delay rounds to a documented native 50 ms step and requests readback");
    commands.clear();
    require(transmitter.requestTransmitControl({TransmitControlRequest::Field::CwDelay, 1001})
        == ReceiveDispatch::Unsupported && commands.isEmpty(), "unsupported keyer delay cannot reach the radio");
    int keyerDelay = -1;
    QObject::connect(&transmitter, &IRadioBackend::transmitChanged, &application,
        [&](const TransmitDelta& delta) { if (delta.cwDelay) { keyerDelay = *delta.cwDelay; } });
    KenwoodBackendTestAccess::message(transmitter, "SD0750");
    require(keyerDelay == 750, "CW delay adopts radio readback in milliseconds");
    KenwoodBackendTestAccess::message(transmitter, "SD0751");
    KenwoodBackendTestAccess::message(transmitter, "SD1001");
    require(keyerDelay == 750, "malformed or unsupported native keyer delays cannot replace valid state");
    commands.clear();
    transmitter.requestTransmitControl({TransmitControlRequest::Field::CwIambicMode, 1});
    require(commands == QList<QByteArray>{"EX00502 001;", "EX00502;"},
        "keyer squeeze mode changes only its documented menu item");
    commands.clear();
    transmitter.requestTransmitControl({TransmitControlRequest::Field::CwSwap, true});
    require(commands == QList<QByteArray>{"EX00503 001;", "EX00503;"},
        "paddle reversal preserves keyer port configuration and squeeze mode");
    std::optional<int> squeezeMode;
    std::optional<bool> paddleSwap;
    QObject::connect(&transmitter, &IRadioBackend::transmitChanged, &application,
        [&](const TransmitDelta& delta) {
            if (delta.cwIambicMode) { squeezeMode = *delta.cwIambicMode; }
            if (delta.cwSwapPaddles) { paddleSwap = *delta.cwSwapPaddles; }
        });
    KenwoodBackendTestAccess::message(transmitter, "EX00502 001");
    KenwoodBackendTestAccess::message(transmitter, "EX00503 001");
    require(squeezeMode == 1 && paddleSwap == true, "native keyer menu readback updates shared state");
    KenwoodBackendTestAccess::message(transmitter, "EX00502 002");
    require(squeezeMode == 1, "unassigned squeeze mode is rejected at the parser boundary");
    commands.clear();
    transmitter.setPanCenter(QStringLiteral("0"), 14.1, IRadioBackend::PanCenterIntent::Range);
    require(commands.isEmpty(), "scope range bookkeeping cannot retune the receiver");
    transmitter.invokeExtension("kenwood", "vfo.transmit", 7, "B");
    require(commands == QList<QByteArray>{"FT1;", "FT;"}, "native split selection requests TX VFO readback");
    commands.clear();
    transmitter.invokeExtension("kenwood", "vfo.frequency", 8,
        QVariantMap{{"vfo", "B"}, {"frequencyHz", 14200000}});
    require(commands == QList<QByteArray>{"FB00014200000;", "FB;"}, "inactive VFO can be tuned directly");
    TxCoordinator coordinator([](const TxCoordinator::Operation&, TxCoordinator::StopReason) {});
    const TxCoordinator::Actor actor = coordinator.registerActor({true, 0});
    const TxCoordinator::Operation operation = coordinator.acquire(actor, TxCoordinator::monotonicMs()).operation;
    const TxCoordinator::Producer producer = coordinator.registerProducer();
    const TxCoordinator::Context media = coordinator.mediaContext(producer, operation);
    commands.clear();
    transmitter.setTune(true, 10, operation);
    require(commands.isEmpty(), "TUNE cannot ignore its requested power or rewrite RF drive");
    transmitter.observePcAudioRouting(true);
    commands.clear();
    transmitter.setKeying(true, operation);
    require(commands.isEmpty(), "PC Audio cannot key before LAN input readback");
    transmitter.requestPcAudioRouting(true);
    require(commands.contains("MS103;"), "operator explicitly selects DATA SEND LAN");
    KenwoodBackendTestAccess::message(transmitter, "MS103");
    commands.clear();
    transmitter.setKeying(true, operation);
    require(commands == QList<QByteArray>{"TX1;"}, "network audio keys documented DATA SEND");
    QByteArray pcm(480 * 4, '\0');
    transmitter.submitTxAudio(pcm, 24000, TxAudioSource::Microphone, media);
    KenwoodBackendTestAccess::pumpAudio(transmitter);
    require(datagrams.size() == 1, "one timer tick sends one qualified audio packet");
    for (bool operatorEdit : {false, true}) {
        transmitter.submitTxAudio(pcm, 24000, TxAudioSource::Microphone, media);
        transmitter.submitTxAudio(pcm, 24000, TxAudioSource::Microphone, media);
        const qsizetype beforeRouting = datagrams.size();
        KenwoodBackendTestAccess::pumpAudio(transmitter);
        require(datagrams.size() == beforeRouting + 1, "routing fixture has an active producer with pending audio");
        commands.clear();
        if (operatorEdit) {
            require(transmitter.requestPcAudioRouting(false) == ReceiveDispatch::Dispatched,
                "operator can leave PC audio without rewriting ordinary SEND input");
            require(commands == QList<QByteArray>{"MS0;"}, "leaving PC audio only queries the preserved SEND/PTT path");
        } else {
            transmitter.observePcAudioRouting(false);
            require(commands.isEmpty(), "observed PC audio intent never writes a radio input setting");
        }
        transmitter.observePcAudioRouting(true);
        KenwoodBackendTestAccess::pumpAudio(transmitter);
        require(datagrams.size() == beforeRouting + 1, "off/on routing cannot revive queued audio from the old source intent");
        transmitter.submitTxAudio(pcm, 24000, TxAudioSource::Microphone, media);
        KenwoodBackendTestAccess::pumpAudio(transmitter);
        require(datagrams.size() == beforeRouting + 2, "new qualified PCM can flow after PC audio is selected again");
    }
    transmitter.submitTxAudio(pcm, 24000, TxAudioSource::Microphone, media);
    transmitter.submitTxAudio(pcm, 24000, TxAudioSource::Microphone, media);
    const qsizetype beforeRadioSource = datagrams.size();
    KenwoodBackendTestAccess::pumpAudio(transmitter);
    require(datagrams.size() == beforeRadioSource + 1, "radio-source fixture has current qualified audio");
    KenwoodBackendTestAccess::message(transmitter, "MS102");
    KenwoodBackendTestAccess::message(transmitter, "MS103");
    KenwoodBackendTestAccess::pumpAudio(transmitter);
    require(datagrams.size() == beforeRadioSource + 1, "front-panel source changes retire old PCM even when LAN is restored immediately");
    transmitter.submitTxAudio(pcm, 24000, TxAudioSource::Microphone, media);
    KenwoodBackendTestAccess::message(transmitter, "MS100");
    KenwoodBackendTestAccess::pumpAudio(transmitter);
    require(datagrams.size() == beforeRadioSource + 2, "impossible both-off MS reports cannot replace confirmed LAN source state");
    transmitter.setKeying(false, operation);
    require(commands.last() == "RX;", "qualified release sends RX");
    const qsizetype sentAudio = datagrams.size();
    transmitter.submitTxAudio(pcm, 24000, TxAudioSource::Microphone, media);
    KenwoodBackendTestAccess::pumpAudio(transmitter);
    require(datagrams.size() == sentAudio, "released PTT cannot leak audio");
    for (const QByteArray& fault : {QByteArray("E"), QByteArray("O")}) {
        QList<QByteArray> faultAudio;
        QList<QByteArray> faultCommands;
        KenwoodBackend faultBackend(KenwoodBackend::Transport{
            [&](const QByteArray& bytes) { faultCommands.append(bytes); return true; },
            [&](const QByteArray& bytes) { faultAudio.append(bytes); return true; }});
        KenwoodBackendTestAccess::authenticate(faultBackend);
        KenwoodBackendTestAccess::message(faultBackend, "ID024");
    KenwoodBackendTestAccess::message(faultBackend, "XV0");
        KenwoodBackendTestAccess::message(faultBackend, "##TI1");
        KenwoodBackendTestAccess::message(faultBackend, "##UE1");
        faultBackend.observePcAudioRouting(true);
        KenwoodBackendTestAccess::message(faultBackend, "MS103");
        faultBackend.setKeying(true, operation);
        std::optional<PcmFrame> faultRxFrame;
        QObject::connect(&faultBackend, &IRadioBackend::audioFrameReady, &application,
            [&](const PcmFrame& frame) { faultRxFrame = frame; });
        KenwoodBackendTestAccess::audio(faultBackend, incomingAudio, 0);
        require(faultRxFrame && faultRxFrame->current(), "fault fixture has a current receive PCM lease");
        int idleObservations = 0;
        QObject::connect(&faultBackend, &IRadioBackend::keyingStateConfirmed, &application,
            [&](bool keyed) { if (!keyed) { ++idleObservations; } });
        KenwoodBackendTestAccess::message(faultBackend, "TX1");
        faultBackend.submitTxAudio(pcm, 24000, TxAudioSource::Microphone, media);
        faultBackend.submitTxAudio(pcm, 24000, TxAudioSource::Microphone, media);
        KenwoodBackendTestAccess::pumpAudio(faultBackend);
        require(faultAudio.size() == 1, "fault fixture starts with a working qualified audio producer");
        QString faultReason;
        const qsizetype commandsBeforeFault = faultCommands.size();
        QObject::connect(&faultBackend, &IRadioBackend::connectionError, &application,
            [&](const QString& reason) {
                faultReason = reason;
                require(!faultBackend.isConnected() && !faultBackend.capabilities().canTransmit,
                    "error observers cannot see a still-authorized connection");
                require(!faultRxFrame->current(), "error observers cannot consume old receive PCM leases");
                faultBackend.setKeying(true, operation);
                KenwoodBackendTestAccess::pumpAudio(faultBackend);
            });
        KenwoodBackendTestAccess::message(faultBackend, fault);
        require(!faultBackend.isConnected() && !faultBackend.capabilities().canTransmit
            && faultReason.contains("transmitter state is unknown"),
            "communication faults retire the session and report uncertain transmitter state");
        KenwoodBackendTestAccess::pumpAudio(faultBackend);
        require(faultAudio.size() == 1 && idleObservations == 0
            && faultCommands.size() == commandsBeforeFault,
            "communication faults stop queued producers without fabricating radio idle");
        KenwoodBackendTestAccess::authenticate(faultBackend);
        require(!faultBackend.capabilities().canTransmit, "fault reconnect cannot restore old TX grants");
    }
    int refusals = 0;
    QObject::connect(&transmitter, &IRadioBackend::configurationWarning, &application,
        [&](const QString& warning) { if (warning.contains("rejected a command")) { ++refusals; } });
    KenwoodBackendTestAccess::message(transmitter, "?");
    KenwoodBackendTestAccess::message(transmitter, "?");
    require(refusals == 1 && transmitter.isConnected() && transmitter.capabilities().canTransmit,
        "uncorrelated refusals warn once per interval without inventing a session failure");
    coordinator.reset();
    commands.clear();
    transmitter.setKeying(true, operation);
    transmitter.setTune(true, 10, operation);
    transmitter.submitTxAudio(pcm, 24000, TxAudioSource::Microphone, media);
    KenwoodBackendTestAccess::pumpAudio(transmitter);
    require(commands.isEmpty() && datagrams.size() == sentAudio, "old session authority cannot key or stream");
    KenwoodBackendTestAccess::message(transmitter, "OM03");
    KenwoodBackendTestAccess::message(transmitter, "FT0");
    KenwoodBackendTestAccess::message(transmitter, "SF0000140740003");
    require(!transmitter.sendCwText("CQ", operation).isEmpty(), "stale operation refuses radio keyer");
    transmitter.disconnectRadio();
    KenwoodBackendTestAccess::authenticate(transmitter);
    require(!transmitter.capabilities().canTransmit, "reconnect retires account permissions");
    KenwoodBackendTestAccess::message(transmitter, "?");
    require(refusals == 2, "reconnect resets refusal warning suppression");
    QList<QByteArray> filterCommands;
    KenwoodBackend filterBackend(KenwoodBackend::Transport{
        [&](const QByteArray& bytes) { filterCommands.append(bytes); return true; }, {}});
    QVariantMap filterState;
    int filterErrors = 0;
    int projectedFilters = 0;
    QObject::connect(&filterBackend, &IRadioBackend::extensionStatus, &application,
        [&](const QString&, const QString& kind, const QVariantMap& state) {
            if (kind == "filter") { filterState = state; }
        });
    QObject::connect(&filterBackend, &IRadioBackend::extensionError, &application,
        [&](quint64, const QString&) { ++filterErrors; });
    QObject::connect(&filterBackend, &IRadioBackend::sliceChanged, &application,
        [&](int, const SliceDelta& delta) { if (delta.filterLow || delta.filterHigh) { ++projectedFilters; } });
    KenwoodBackendTestAccess::authenticate(filterBackend);
    KenwoodBackendTestAccess::message(filterBackend, "ID024");
    KenwoodBackendTestAccess::message(filterBackend, "XV0");
    require(!filterCommands.contains("SH0;") && !filterCommands.contains("SL0;"),
        "startup defers mode-specific filter queries until the receive mode is known");
    KenwoodBackendTestAccess::message(filterBackend, "FR0");
    KenwoodBackendTestAccess::message(filterBackend, "FT0");
    KenwoodBackendTestAccess::message(filterBackend, "TS0");
    KenwoodBackendTestAccess::message(filterBackend, "RX");
    KenwoodBackendTestAccess::message(filterBackend, "OM05");
    KenwoodBackendTestAccess::message(filterBackend, "SF0000140740005");
    KenwoodBackendTestAccess::message(filterBackend, "MO20");
    KenwoodBackendTestAccess::message(filterBackend, "SL001");
    KenwoodBackendTestAccess::message(filterBackend, "SH0011");
    const auto editFilter = [&](const QString& axis, const QVariant& index, const QString& context) {
        filterBackend.invokeExtension("kenwood", "filter.set", 500,
            QVariantMap{{"axis", axis}, {"index", index}, {"context", context}});
    };
    const QString amContext = filterState.value("context").toString();
    require(filterState.value("editable").toBool() && filterState.value("axes").toList().size() == 2,
        "native AM filter has two documented audio controls");
    filterCommands.clear();
    editFilter("low", 3, amContext);
    require(filterCommands == QList<QByteArray>{"SL003;", "SL0;"}
        && filterState.value("axes").toList()[0].toMap().value("index").toInt() == 1,
        "native low-cut edit preserves high cut and waits for readback");
    KenwoodBackendTestAccess::message(filterBackend, "SL003");
    require(filterState.value("axes").toList()[0].toMap().value("index").toInt() == 3,
        "native filter status adopts confirmed low cut");
    filterCommands.clear();
    editFilter("high", 14, amContext);
    editFilter("low", 1.5, amContext);
    editFilter("low", -1, amContext);
    require(filterCommands.isEmpty() && filterErrors == 3, "invalid native indices cannot enter the wire path");
    KenwoodBackendTestAccess::message(filterBackend, "OM04");
    KenwoodBackendTestAccess::message(filterBackend, "SF0000140740004");
    KenwoodBackendTestAccess::message(filterBackend, "SL002");
    KenwoodBackendTestAccess::message(filterBackend, "SH0013");
    filterCommands.clear();
    editFilter("high", 10, amContext);
    require(filterCommands.isEmpty() && filterErrors == 4, "old-mode native edits cannot reach a new filter layout");
    const QString fmContext = filterState.value("context").toString();
    editFilter("high", 21, fmContext);
    require(filterCommands == QList<QByteArray>{"SH0021;", "SH0;"}, "FM high-cut edit preserves low cut");
    KenwoodBackendTestAccess::message(filterBackend, "TX1");
    filterCommands.clear();
    editFilter("low", 0, fmContext);
    require(!filterState.value("editable").toBool() && filterCommands.isEmpty() && filterErrors == 5,
        "native filter edits are refused while transmitting");
    KenwoodBackendTestAccess::message(filterBackend, "RX");
    filterCommands.clear();
    KenwoodBackendTestAccess::message(filterBackend, "OM06");
    KenwoodBackendTestAccess::message(filterBackend, "SF0000140740006");
    require(filterCommands == QList<QByteArray>{"TS;", "SF0;", "SF1;", "SL0;", "GC;", "EX00206;"},
        "width-only FSK queries its APF prerequisite but does not query unsupported SH");
    KenwoodBackendTestAccess::message(filterBackend, "SL006");
    require(filterState.value("axes").toList().size() == 1, "FSK publishes only its native width axis");
    filterCommands.clear();
    editFilter("low", 7, filterState.value("context").toString());
    require(filterCommands == QList<QByteArray>{"SL007;", "SL0;"}, "FSK width uses its native table index");
    filterCommands.clear();
    editFilter("high", 0, filterState.value("context").toString());
    require(filterCommands.isEmpty() && filterErrors == 6, "nonexistent width-only companion cannot be written");
    KenwoodBackendTestAccess::message(filterBackend, "OM0A");
    KenwoodBackendTestAccess::message(filterBackend, "SF000014074000A");
    KenwoodBackendTestAccess::message(filterBackend, "SL026");
    filterCommands.clear();
    editFilter("low", 16, filterState.value("context").toString());
    require(filterCommands == QList<QByteArray>{"SL016;", "SL0;"}, "PSK width uses its distinct table");
    require(projectedFilters == 0, "native AM/FM/FSK/PSK controls do not invent shared RF passband geometry");
    const QString oldVfoContext = filterState.value("context").toString();
    KenwoodBackendTestAccess::message(filterBackend, "FR1");
    filterCommands.clear();
    editFilter("low", 16, oldVfoContext);
    require(filterCommands.isEmpty() && filterErrors == 7 && !filterState.value("editable").toBool(),
        "receive-VFO change retires old mode and native filter edits");
    std::cout << "Kenwood backend lifecycle checks passed\n";
}
