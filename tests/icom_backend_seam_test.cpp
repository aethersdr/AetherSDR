// Socket-free IcomCivBackend seam policy. An unstarted IcomSession stands in
// for the transport: setters enqueue on the production CI-V scheduler, the
// dispatched frames are read back from the backend's own trace, and radio
// replies enter onCivFrame directly. No socket, peer, or event-loop wait.
#include "core/backends/icom/IcomCivBackend.h"
#include "core/backends/icom/IcomSession.h"
#include "TxTestAuthority.h"

#include <QCoreApplication>
#include <QLoggingCategory>
#include <QSignalSpy>
#include <QStringList>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

using namespace AetherSDR;
using namespace AetherSDR::icom;

namespace AetherSDR::icom {

struct IcomCivBackendTestAccess {
    static void prepare(IcomCivBackend& b, const char* modelName,
                        std::uint64_t generation = 1)
    {
        const IcomModel* model = modelForName(modelName);
        b.m_model = model ? model : modelForId(0xA4);
        b.m_session = std::make_unique<IcomSession>();
        b.m_session->setCivAddress(b.m_model->civAddress);
        b.m_civReported = b.m_model->civAddress;
        b.m_civModelId = b.m_model->civAddress;
        b.m_connected = true;
        b.m_sessionGeneration = generation;
        b.m_mode = CivMode::Usb;
        b.m_frequencyHz = 14'100'000;
    }
    static void deliver(IcomCivBackend& b, CivFrame frame, std::uint64_t generation = 1)
    {
        frame.to = kControllerAddress;
        frame.from = b.m_model->civAddress;
        b.onCivFrame(frame, generation);
    }
    static void expectPttConfirmation(IcomCivBackend& b, bool keyed)
    {
        b.m_keyed = !keyed;
        b.m_pendingPttIntent = keyed;
        b.m_pendingPttUntilMs = b.nowMs() + 1000;
    }
    // Every frame the backend dispatched (its trace) or still holds queued.
    static std::vector<CivFrame> issued(const IcomCivBackend& b)
    {
        std::vector<CivFrame> out;
        for (const IcomCivBackend::CivTraceEntry& entry : b.m_civTrace) {
            if (!entry.outbound) {
                continue;
            }
            std::vector<std::uint8_t> bytes;
            for (const QString& octet : entry.hex.split(QLatin1Char(' '))) {
                bytes.push_back(static_cast<std::uint8_t>(octet.toUInt(nullptr, 16)));
            }
            if (auto f = parseFrame(bytes)) {
                out.push_back(*f);
            }
        }
        for (const auto& queued : b.m_civScheduler.m_queue) {
            if (auto f = parseFrame(queued.request.frame)) {
                out.push_back(*f);
            }
        }
        return out;
    }
    static void forget(IcomCivBackend& b)
    {
        b.m_civTrace.clear();
        (void)b.m_civScheduler.reset();
    }
    // Drain by letting each in-flight transaction time out on a future clock.
    static void settle(IcomCivBackend& b)
    {
        qint64 t = std::max<qint64>(b.nowMs(), b.m_civScheduler.stats().lastDispatchMs);
        for (int i = 0; i < 128 && !b.m_civScheduler.idle(); ++i) {
            t += IcomCivScheduler::kReadTimeoutMs + 1;
            b.pumpCiv(t);
        }
    }
    // Operator picked SCROLL-F; the startup writes themselves are not under test.
    static void scrollF(IcomCivBackend& b)
    {
        b.m_scopeView = IcomSettings::ScopeView::ScrollF;
        b.m_scopeStarted = true;
    }
    static bool tuning(const IcomCivBackend& b) { return b.m_tuning; }
    static bool tuneTimerActive(const IcomCivBackend& b)
    {
        return b.m_tuneTimer && b.m_tuneTimer->isActive();
    }
    static int preTunePower(const IcomCivBackend& b) { return b.m_preTuneTxPowerPercent; }
    static const TxCoordinator::Operation& tuneOperation(const IcomCivBackend& b)
    {
        return b.m_tuneOperation;
    }
    static void setMode(IcomCivBackend& b, CivMode mode, bool data = false)
    {
        b.m_mode = mode;
        b.m_dataMode = data;
    }
    static int preampStep(const IcomCivBackend& b) { return b.m_preampStep; }
    static void reassertPreamp(IcomCivBackend& b) { b.reassertPanPreampWireStep(b.m_preampStep); }
    static void queuePendingRead(IcomCivBackend& b)
    {
        const std::vector<std::uint8_t> frame = cmdReadFrequency(0xA4);
        b.queueRead(frame, b.semanticKey(frame), IcomCivScheduler::Priority::Maintenance);
    }
    static void cancelScheduler(IcomCivBackend& b)
    {
        b.terminateScheduler(IcomCivScheduler::TerminalOutcome::Cancelled,
                             IcomCivBackend::SchedulerWaiterOutcome::Cancelled);
    }
    static void sessionConnected(IcomCivBackend& b) { b.onSessionConnected(QStringLiteral("IC-705")); }
    static void audio(IcomCivBackend& b, const std::vector<float>& mono) { b.onAudio(mono); }
    static void sendConnectReadBurst(IcomCivBackend& b) { b.sendConnectReadBurst(); }
    static void seedScrubValue(IcomCivBackend& b, const char* id, int rfGainPercent)
    {
        b.m_rfGainPercent = rfGainPercent;
        b.m_controlsValueKnown.insert(QString::fromLatin1(id));
    }
    static void markControlKnown(IcomCivBackend& b, const char* id)
    {
        b.m_controlsValueKnown.insert(QString::fromLatin1(id));
    }
    static void authenticateLease(IcomCivBackend& b)
    {
        b.m_session->m_authOk = true;
        b.m_session->m_lastRenewalResult = QStringLiteral("accepted");
    }
};

}  // namespace AetherSDR::icom

namespace {

int g_failures = 0;
void check(bool ok, const char* what)
{
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

using Access = IcomCivBackendTestAccess;

CivFrame frame(std::uint8_t command, std::optional<std::uint8_t> sub,
               std::vector<std::uint8_t> data)
{
    CivFrame f;
    f.cmd = command;
    f.hasSub = sub.has_value();
    f.sub = sub.value_or(0);
    f.data = std::move(data);
    return f;
}

bool any(const std::vector<CivFrame>& frames, const std::function<bool(const CivFrame&)>& p)
{
    return std::any_of(frames.begin(), frames.end(), p);
}

std::vector<CivFrame> writes(const std::vector<CivFrame>& frames, std::uint8_t command,
                             std::optional<std::uint8_t> sub)
{
    std::vector<CivFrame> out;
    for (const CivFrame& f : frames) {
        if (f.cmd == command && (!sub || (f.hasSub && f.sub == *sub)) && !f.data.empty()) {
            out.push_back(f);
        }
    }
    return out;
}

bool keysTransmitter(const CivFrame& f)
{
    return f.cmd == cmd::kControl && f.hasSub
        && (f.sub == control::kPtt || f.sub == control::kTuner) && !f.data.empty()
        && f.data.front() != 0x00;
}

std::optional<CivFrame> scopeSweep(std::uint64_t centreHz, std::uint64_t spanHz)
{
    std::vector<std::uint8_t> body{0x00, encodeBcdByte(1), encodeBcdByte(1), 0x00};
    const auto centre = encodeFreq(centreHz);
    const auto span = encodeFreq(spanHz);
    body.insert(body.end(), centre.begin(), centre.end());
    body.insert(body.end(), span.begin(), span.end());
    body.push_back(0x00);
    body.insert(body.end(), static_cast<std::size_t>(kScopePointsIc705), 0x20);
    return parseFrame(buildFrameSub(0xA4, cmd::kScope, scope::kWaveData, body));
}

// A frame from a previous session generation publishes nothing.
void testStaleGenerationDropped()
{
    IcomCivBackend backend;
    Access::prepare(backend, "IC-705", 2);
    QSignalSpy transmit(&backend, &IRadioBackend::transmitChanged);
    QSignalSpy meter(&backend, &IRadioBackend::meterUpdate);
    QSignalSpy spectrum(&backend, &IRadioBackend::spectrumFrameReady);

    const CivFrame ptt = frame(cmd::kControl, control::kPtt, {0x01});
    const CivFrame sMeter = frame(cmd::kMeter, meter::kSMeter, {0x01, 0x20});
    const std::optional<CivFrame> sweep = scopeSweep(14'100'000, 100'000);
    check(sweep.has_value(), "stale-generation scope fixture parses");

    Access::expectPttConfirmation(backend, true);
    Access::deliver(backend, ptt, 1);
    Access::deliver(backend, sMeter, 1);
    if (sweep) {
        Access::deliver(backend, *sweep, 1);
    }
    check(transmit.isEmpty() && meter.isEmpty() && spectrum.isEmpty(),
          "a previous generation's PTT, meter and scope frames publish nothing");

    Access::deliver(backend, ptt, 2);
    Access::deliver(backend, sMeter, 2);
    if (sweep) {
        Access::deliver(backend, *sweep, 2);
    }
    check(transmit.count() >= 1 && meter.count() == 1 && spectrum.count() == 1,
          "the same frames on the current generation do publish");
}

// An isolated keyed SWR minimum is held at the backend seam.
void testSwrMinimumHold()
{
    IcomCivBackend backend;
    Access::prepare(backend, "IC-705");
    Access::expectPttConfirmation(backend, true);
    Access::deliver(backend, frame(cmd::kControl, control::kPtt, {0x01}));

    QSignalSpy meterSpy(&backend, &IRadioBackend::meterUpdate);
    CivFrame swr = frame(cmd::kMeter, meter::kSwr, {0x00, 0x80});
    Access::deliver(backend, swr);
    check(meterSpy.count() == 1, "a keyed IC-705 SWR reading publishes");
    swr.data = {0x00, 0x00};
    Access::deliver(backend, swr);
    check(meterSpy.count() == 1, "an isolated keyed IC-705 SWR minimum is held");
    swr.data = {0x00, 0x82};
    Access::deliver(backend, swr);
    check(meterSpy.count() == 2, "the next real SWR reading publishes");
}

// IC-9700 preamp wire state 02 is mirrored; operator intent stays bounded.
void testIc9700PreampWireState()
{
    IcomCivBackend backend;
    Access::prepare(backend, "IC-9700");
    Access::deliver(backend, frame(cmd::kFunction, func::kPreamp, {0x02}));
    check(Access::preampStep(backend) == 2, "IC-9700 preamp wire state 02 is mirrored");
    Access::forget(backend);
    Access::reassertPreamp(backend);
    const auto reasserted = writes(Access::issued(backend), cmd::kFunction, func::kPreamp);
    check(reasserted.size() == 1 && reasserted.front().data.front() == 0x02,
          "the diagnostic reassert writes the radio-adopted 02 unchanged");
    backend.setPanPreamp(QStringLiteral("0"), 2);
    check(Access::preampStep(backend) == 1,
          "operator intent is bounded to the IC-9700's OFF/P.AMP ladder");
}

// Scheduler waiters are cancelled with their owner and survive re-entry.
void testWaiterLifetime(QCoreApplication& app)
{
    {
        auto backend = std::make_unique<IcomCivBackend>();
        Access::prepare(*backend, "IC-705");
        Access::queuePendingRead(*backend);
        QVariantMap terminal;
        QObject::connect(backend.get(), &IRadioBackend::extensionResult, &app,
                         [&](quint64 id, const QVariant& result) {
                             if (id == 0xD357) {
                                 terminal = result.toMap();
                             }
                         });
        backend->invokeExtension(QStringLiteral("icom"), QStringLiteral("civ.scheduler.wait-idle"),
                                 0xD357, QVariantMap{{QStringLiteral("timeoutMs"), 10000}});
        check(terminal.isEmpty(), "a waiter on a busy scheduler stays pending");
        backend.reset();
        check(terminal.value(QStringLiteral("outcome")).toString() == QLatin1String("cancelled")
                  && terminal.value(QStringLiteral("cancelled")).toBool(),
              "backend destruction cancels a pending scheduler waiter");
    }
    {
        IcomCivBackend backend;
        Access::prepare(backend, "IC-705");
        Access::queuePendingRead(backend);
        QVariantMap first;
        QVariantMap reentrant;
        QObject::connect(&backend, &IRadioBackend::extensionResult, &app,
                         [&](quint64 id, const QVariant& result) {
                             if (id == 0x5119) {
                                 first = result.toMap();
                                 backend.invokeExtension(
                                     QStringLiteral("icom"),
                                     QStringLiteral("civ.scheduler.wait-idle"), 0x5120,
                                     QVariantMap{{QStringLiteral("timeoutMs"), 10000}});
                             } else if (id == 0x5120) {
                                 reentrant = result.toMap();
                             }
                         });
        backend.invokeExtension(QStringLiteral("icom"), QStringLiteral("civ.scheduler.wait-idle"),
                                0x5119, QVariantMap{{QStringLiteral("timeoutMs"), 10000}});
        Access::cancelScheduler(backend);
        check(first.value(QStringLiteral("outcome")).toString() == QLatin1String("cancelled"),
              "scheduler termination reports the original waiter as cancelled");
        check(reentrant.value(QStringLiteral("outcome")).toString() == QLatin1String("completed"),
              "a waiter registered re-entrantly after the reset is not erased");
    }
}

// Capability flags and the profile.show GPS keys follow the model.
void testCapabilityFlags()
{
    IcomCivBackend ic705;
    Access::prepare(ic705, "IC-705");
    const RadioCapabilities caps = ic705.capabilities();
    check(caps.hasGpsLocation && caps.hasGpsTimeConfiguration,
          "IC-705 advertises GPS position and clock configuration");
    check(!caps.hasGpsSatelliteTelemetry && !caps.hasGpsFrequencyReference,
          "IC-705 does not claim satellite telemetry or a GPS frequency reference");
    check(caps.hasRadioPttReadback,
          "Icom declares hasRadioPttReadback: setKeying() is intent, 1C 00 is state");

    QSignalSpy result(&ic705, &IRadioBackend::extensionResult);
    ic705.invokeExtension(QStringLiteral("icom"), QStringLiteral("profile.show"), 9001, {});
    check(result.count() == 1, "profile.show answers synchronously");
    if (result.count() == 1) {
        const QVariantMap profile = result.first().at(1).toMap();
        const QVariantMap gps = profile.value(QStringLiteral("gps")).toMap();
        check(profile.value(QStringLiteral("model")).toString() == QLatin1String("IC-705"),
              "profile.show names the active model");
        check(gps.value(QStringLiteral("ntpEnabledSetItem")).toInt() == 167
                  && gps.value(QStringLiteral("ntpServerSetItem")).toInt() == 168
                  && gps.value(QStringLiteral("timeCorrectSetItem")).toInt() == 169
                  && gps.value(QStringLiteral("ntpAccess")).toBool(),
              "profile.show carries the IC-705 GPS/NTP SET items and NTP access");
    }

    IcomCivBackend mk2;
    Access::prepare(mk2, "IC-7300MK2");
    const RadioCapabilities mk2Caps = mk2.capabilities();
    check(!mk2Caps.hasGpsLocation && !mk2Caps.hasGpsTimeConfiguration,
          "a model without GPS advertises neither GPS capability");
    check(mk2Caps.hasRadioPttReadback, "PTT readback is family-wide, not IC-705 only");
    QSignalSpy mk2Result(&mk2, &IRadioBackend::extensionResult);
    mk2.invokeExtension(QStringLiteral("icom"), QStringLiteral("profile.show"), 9002, {});
    check(mk2Result.count() == 1
              && !mk2Result.first().at(1).toMap().contains(QStringLiteral("gps")),
          "profile.show has no GPS block for a model without GPS");
}

// TUNE borrows the drive register and every unkey path puts it back.
void testTuneDriveRestore()
{
    const int tuneRaw = percentToLevelRaw(10);
    const auto powerWrites = [](const std::vector<CivFrame>& frames) {
        std::vector<int> out;
        for (const CivFrame& f : writes(frames, cmd::kLevel, level::kRfPower)) {
            if (const auto raw = decodeLevel(f.data)) {
                out.push_back(*raw);
            }
        }
        return out;
    };
    for (const int path : {0, 1, 2}) {
        TxTestAuthority authority;
        IcomCivBackend backend;
        backend.setTransmitContext(authority.context);
        Access::prepare(backend, "IC-705");
        backend.setTxPower(37);
        Access::settle(backend);
        Access::forget(backend);

        backend.setTune(true, 10, authority.operation);
        const auto keyed = Access::issued(backend);
        check(powerWrites(keyed) == std::vector<int>{tuneRaw},
              "TUNE applies exactly the requested 10% drive");
        check(any(keyed, keysTransmitter), "TUNE keys the transmitter");
        check(Access::tuning(backend) && Access::tuneTimerActive(backend),
              "TUNE owns its tone producer while keyed");
        Access::settle(backend);
        Access::forget(backend);

        if (path == 0) {
            backend.setTune(false, 10, authority.operation);
        } else if (path == 1) {
            backend.setKeying(false, authority.operation);
        } else {
            Access::expectPttConfirmation(backend, true);
            Access::deliver(backend, frame(cmd::kControl, control::kPtt, {0x01}));
            Access::deliver(backend, frame(cmd::kControl, control::kPtt, {0x00}));
        }
        const auto released = Access::issued(backend);
        check(powerWrites(released) == std::vector<int>{percentToLevelRaw(37)},
              path == 0 ? "TUNE release restores the operator's 37% drive"
              : path == 1 ? "a direct unkey during TUNE restores the 37% drive"
                          : "a radio-reported unkey during TUNE restores the 37% drive");
        check(!Access::tuning(backend) && !Access::tuneTimerActive(backend)
                  && Access::preTunePower(backend) == -1,
              "every unkey path ends TUNE ownership and clears the borrowed drive");
    }
}

QStringList g_txWarnings;
QtMessageHandler g_txPrevious = nullptr;
void captureTxWarning(QtMsgType type, const QMessageLogContext& ctx, const QString& msg)
{
    if (type == QtWarningMsg && ctx.category && std::strcmp(ctx.category, "aether.icom.tx") == 0) {
        g_txWarnings << msg;
        return;
    }
    if (g_txPrevious) {
        g_txPrevious(type, ctx, msg);
    }
}

// TUNE power applies live to the carrier, and RF power set during TUNE is what
// the unkey restores, not a write over the tune drive.
void testTunePowerAppliesLive()
{
    const auto powerWrites = [](const std::vector<CivFrame>& frames) {
        std::vector<int> out;
        for (const CivFrame& f : writes(frames, cmd::kLevel, level::kRfPower)) {
            if (const auto raw = decodeLevel(f.data)) {
                out.push_back(*raw);
            }
        }
        return out;
    };
    TxTestAuthority authority;
    IcomCivBackend backend;
    backend.setTransmitContext(authority.context);
    Access::prepare(backend, "IC-705");
    const RadioCapabilities caps = backend.capabilities();
    check(caps.transmitDriveControl && caps.transmitDriveControl->tunePowerAppliesLive,
          "Icom declares tune power live");
    backend.setTxPower(37);
    Access::settle(backend);
    Access::forget(backend);
    {
        QSignalSpy transmit(&backend, &IRadioBackend::transmitChanged);
        const auto raw = encodeLevel(percentToLevelRaw(37));
        Access::deliver(backend, frame(cmd::kLevel, level::kRfPower, {raw[0], raw[1]}));
        check(transmit.count() == 1 && transmit.first().at(0).value<TransmitDelta>().rfPower,
              "premise: an RF power readback outside TUNE is published");
    }

    backend.setTunePower(20);
    check(powerWrites(Access::issued(backend)).empty(),
          "tune power while not tuning writes nothing");

    backend.setTune(true, 10, authority.operation);
    Access::settle(backend);
    Access::forget(backend);
    // The radio reads the tune drive back as RF power while TUNE is up.
    const auto publishesRfPower = [&backend](int percent) {
        QSignalSpy transmit(&backend, &IRadioBackend::transmitChanged);
        const auto raw = encodeLevel(percentToLevelRaw(percent));
        Access::deliver(backend, frame(cmd::kLevel, level::kRfPower, {raw[0], raw[1]}));
        bool published = false;
        for (const QList<QVariant>& args : transmit) {
            published |= args.at(0).value<TransmitDelta>().rfPower.has_value();
        }
        return published;
    };
    check(!publishesRfPower(10),
          "the tune drive read back during TUNE is not published as the RF power");
    backend.setTunePower(25);
    const auto live = Access::issued(backend);
    check(powerWrites(live) == std::vector<int>{percentToLevelRaw(25)},
          "tune power while tuning writes the 25% tune drive");
    check(!any(live, keysTransmitter) && Access::tuning(backend),
          "a live tune power change neither re-keys nor ends TUNE");
    Access::settle(backend);
    Access::forget(backend);

    backend.setTxPower(50);
    check(powerWrites(Access::issued(backend)).empty() && Access::preTunePower(backend) == 50,
          "RF power during TUNE is held for the unkey, not written over the tune drive");
    backend.setTune(true, 15, authority.operation);
    check(powerWrites(Access::issued(backend)) == std::vector<int>{percentToLevelRaw(15)}
              && Access::preTunePower(backend) == 50,
          "TUNE re-requested while tuning moves the tune drive, not the RF power held");
    Access::settle(backend);
    Access::forget(backend);

    const ControlSpec* txPower = nullptr;
    for (const ControlSpec& spec : controlSpecs()) {
        if (spec.id == "tx.power") {
            txPower = &spec;
        }
    }
    check(txPower != nullptr, "the registry has a tx.power row");
    if (txPower) {
        Access::markControlKnown(backend, "tx.power");
        check(backend.scrubDrive(*txPower)
                  && powerWrites(Access::issued(backend)) == std::vector<int>{percentToLevelRaw(15)}
                  && Access::preTunePower(backend) == 50,
              "a tx.power scrub during TUNE re-asserts the live drive and keeps the RF power held");
        Access::settle(backend);
        Access::forget(backend);
    }

    check(Access::tuneOperation(backend).permitsCleanup(),
          "premise: the TUNE operation is held while tuning");
    backend.setTune(false, 15, authority.operation);
    check(powerWrites(Access::issued(backend)) == std::vector<int>{percentToLevelRaw(50)},
          "TUNE release restores the RF power set during TUNE (50%)");
    check(!Access::tuneOperation(backend).permitsCleanup(),
          "TUNE release drops the TUNE operation");
    Access::settle(backend);
    Access::forget(backend);
    backend.setTunePower(30);
    check(powerWrites(Access::issued(backend)).empty(),
          "tune power after TUNE is released writes nothing");

    {
        // A TUNE whose admission was revoked takes no drive change.
        TxTestAuthority revoked;
        IcomCivBackend stopped;
        stopped.setTransmitContext(revoked.context);
        Access::prepare(stopped, "IC-705");
        stopped.setTune(true, 10, revoked.operation);
        Access::settle(stopped);
        Access::forget(stopped);
        check(Access::tuning(stopped), "premise: TUNE keyed under the revocable operation");
        revoked.coordinator.emergencyStop();
        stopped.setTunePower(60);
        check(powerWrites(Access::issued(stopped)).empty(),
              "tune power after the TUNE operation was stopped writes nothing");
    }
    {
        // The carrier's operation still stands but this backend's transmit
        // context has lapsed: refused like setTune(true), and logged.
        TxTestAuthority keyedBy;
        TxTestAuthority lapsed;
        IcomCivBackend contextGone;
        contextGone.setTransmitContext(keyedBy.context);
        Access::prepare(contextGone, "IC-705");
        contextGone.setTune(true, 10, keyedBy.operation);
        Access::settle(contextGone);
        Access::forget(contextGone);
        check(Access::tuning(contextGone), "premise: TUNE keyed");
        lapsed.coordinator.emergencyStop();
        contextGone.setTransmitContext(lapsed.context);
        g_txWarnings.clear();
        g_txPrevious = qInstallMessageHandler(captureTxWarning);
        contextGone.setTunePower(60);
        qInstallMessageHandler(g_txPrevious);
        check(powerWrites(Access::issued(contextGone)).empty(),
              "tune power with the transmit context lapsed writes nothing");
        check(g_txWarnings.size() == 1 && g_txWarnings.first().contains(QLatin1String("not applied")),
              "a refused tune power is logged");
    }
}

// WFM receives only: no key and no TUNE, but an unkey still goes out.
void testWfmRefusesTransmit()
{
    TxTestAuthority authority;
    IcomCivBackend backend;
    backend.setTransmitContext(authority.context);
    Access::prepare(backend, "IC-705");
    check(backend.capabilities().receiveOnlyModes == QStringList{QStringLiteral("WFM")},
          "IC-705 declares WFM as its receive-only mode");
    Access::setMode(backend, CivMode::Wfm);
    Access::forget(backend);
    backend.setKeying(true, authority.operation);
    backend.setTune(true, 10, authority.operation);
    const auto refused = Access::issued(backend);
    check(!any(refused, keysTransmitter), "no PTT frame is sent in WFM");
    check(writes(refused, cmd::kLevel, level::kRfPower).empty() && !Access::tuning(backend),
          "TUNE in WFM borrows no drive and starts no tone");
    backend.setKeying(false, authority.operation);
    check(any(Access::issued(backend), [](const CivFrame& f) {
              return f.cmd == cmd::kControl && f.hasSub && f.sub == control::kPtt
                  && f.data == std::vector<std::uint8_t>{0x00};
          }),
          "the receive-only gate never swallows an unkey");
}

// PC Audio writes DATA OFF MOD only on a click, and OFF restores USB.
void testPcAudioRestoresUsb()
{
    const auto writesTo = [](const std::vector<CivFrame>& frames, int item) {
        std::vector<int> values;
        for (const CivFrame& f : frames) {
            if (f.cmd == cmd::kSetting && f.hasSub && f.sub == settingSub::kMenu
                && f.data.size() == 3
                && decodeBcdByte(f.data[0]) * 100 + decodeBcdByte(f.data[1]) == item) {
                values.push_back(f.data[2]);
            }
        }
        return values;
    };
    const auto dataOffReadback = [](std::uint8_t value) {
        return frame(cmd::kSetting, settingSub::kMenu,
                     {encodeBcdByte(1), encodeBcdByte(18), value});
    };
    IcomCivBackend backend;
    Access::prepare(backend, "IC-705");
    Access::deliver(backend, dataOffReadback(0x01));  // the operator's USB
    Access::sendConnectReadBurst(backend);
    backend.invokeExtension(QStringLiteral("icom"), QStringLiteral("audio.pc.state"), 0, true);
    check(writesTo(Access::issued(backend), 118).empty(),
          "connect and the PC Audio state publication write nothing to DATA OFF MOD");

    Access::forget(backend);
    backend.invokeExtension(QStringLiteral("icom"), QStringLiteral("audio.pc"), 0, true);
    check(writesTo(Access::issued(backend), 118) == std::vector<int>{0x03},
          "a PC Audio click selects WLAN (03)");
    Access::deliver(backend, dataOffReadback(0x03));

    Access::forget(backend);
    backend.invokeExtension(QStringLiteral("icom"), QStringLiteral("audio.pc"), 0, false);
    const auto off = Access::issued(backend);
    check(writesTo(off, 118) == std::vector<int>{0x01},
          "PC Audio OFF restores the operator's USB (01), not a hardcoded MIC");
    check(writesTo(off, 119).empty(), "PC Audio never writes DATA MOD");
}

// Mode, DATA and slot travel together in one 26 00 write.
void testCompoundModeWrite()
{
    IcomCivBackend backend;
    Access::prepare(backend, "IC-705");
    backend.setSliceMode(0, QStringLiteral("DIGU"));
    const auto digu = Access::issued(backend);
    const auto vfo = writes(digu, cmd::kVfoMode, vfoMode::kSelected);
    check(vfo.size() == 1 && vfo.front().data.size() == 3
              && vfo.front().data[0] == static_cast<std::uint8_t>(CivMode::Usb)
              && vfo.front().data[1] == 0x01 && vfo.front().data[2] >= 1
              && vfo.front().data[2] <= 3,
          "DIGU is one 26 00 write carrying USB, DATA ON and the slot");
    check(writes(digu, cmd::kSetMode, std::nullopt).empty(),
          "no bare 06 goes out beside it, which would clear DATA");
    check(any(digu, [](const CivFrame& f) {
              return f.cmd == cmd::kVfoMode && f.hasSub && f.sub == vfoMode::kSelected
                  && f.data.empty();
          }),
          "the compound write is confirmed by a 26 00 read");

    Access::forget(backend);
    backend.setSliceMode(0, QStringLiteral("USB"));
    const auto usb = writes(Access::issued(backend), cmd::kVfoMode, vfoMode::kSelected);
    check(usb.size() == 1 && usb.front().data.size() == 3 && usb.front().data[1] == 0x00,
          "plain USB clears DATA in the same compound write");
}

// A drag is one IF width plus a matched PBT pair, never a slot change.
void testPassbandDecomposition()
{
    IcomCivBackend backend;
    Access::prepare(backend, "IC-705");
    QSignalSpy slice(&backend, &IRadioBackend::sliceChanged);
    backend.setSliceFilter(0, 600, 2600);
    const auto sent = Access::issued(backend);
    const auto width = writes(sent, cmd::kSetting, settingSub::kFilterWidth);
    const auto inner = writes(sent, cmd::kLevel, level::kPbtInner);
    const auto outer = writes(sent, cmd::kLevel, level::kPbtOuter);
    check(width.size() == 1, "a drawn passband writes the IF width (1A 03) once");
    check(inner.size() == 1 && outer.size() == 1 && inner.front().data == outer.front().data,
          "and moves BOTH Twin PBTs to the same code, which slides without narrowing");
    const int expected = pbtCodeForShiftHz(1600 - passbandCentreHz("USB", 2000), 2000);
    check(!inner.empty() && decodeLevel(inner.front().data).value_or(-1) == expected
              && expected != kPbtCentreCode,
          "the PBT code is the shift from the mode's own filter centre");
    check(writes(sent, cmd::kVfoMode, std::nullopt).empty()
              && writes(sent, cmd::kSetMode, std::nullopt).empty(),
          "a drag never changes the filter slot or the mode");
    bool published = false;
    for (const QList<QVariant>& args : slice) {
        const SliceDelta d = qvariant_cast<SliceDelta>(args.at(1));
        if (d.filterLow && d.filterHigh) {
            // PBT steps quantise the shift; the width is exact.
            published = *d.filterHigh - *d.filterLow == 2000
                && std::abs(*d.filterLow - 600) < 50;
        }
    }
    check(published, "the published passband is the drawn 2.0 kHz, within one PBT step of 600..2600");
}

// Pan intents: zoom never retunes, a drag does, the dead zone does not,
// and zoom-out steps to the next span.
void testPanIntents()
{
    IcomCivBackend backend;
    Access::prepare(backend, "IC-705");
    const auto sweep = scopeSweep(14'100'000, 100'000);
    check(sweep.has_value(), "pan fixture sweep parses");
    if (!sweep) {
        return;
    }
    Access::deliver(backend, *sweep);
    const auto retunes = [](const std::vector<CivFrame>& frames) {
        return any(frames, [](const CivFrame& f) {
            return (f.cmd == cmd::kSetFreq || f.cmd == cmd::kSetFreqTrx) && !f.data.empty();
        });
    };

    Access::forget(backend);
    QSignalSpy pan(&backend, &IRadioBackend::panCenterBandwidthChanged);
    backend.setPanCenter(QStringLiteral("0"), 14'050'000.0, IRadioBackend::PanCenterIntent::Range);
    QCoreApplication::processEvents();
    check(!retunes(Access::issued(backend)), "a zoom's pan-centre sends no frequency command");
    check(pan.count() == 1 && std::fabs(pan.first().at(1).toDouble() - 14.1) < 1e-9,
          "and the radio's real centre is re-asserted");

    Access::forget(backend);
    backend.setPanCenter(QStringLiteral("0"), 14'100'200.0, IRadioBackend::PanCenterIntent::Drag);
    check(!retunes(Access::issued(backend)), "a drag inside the dead zone does not retune");

    Access::forget(backend);
    pan.clear();
    backend.setPanCenter(QStringLiteral("0"), 14'050'000.0, IRadioBackend::PanCenterIntent::Drag);
    QCoreApplication::processEvents();
    const auto dragged = writes(Access::issued(backend), cmd::kSetFreq, std::nullopt);
    check(dragged.size() == 1 && decodeFreq(dragged.front().data).value_or(0) == 14'050'000,
          "a drag outside the dead zone retunes to the dragged centre");
    check(pan.isEmpty(), "and does not snap the view back");

    Access::forget(backend);
    backend.setPanBandwidth(QStringLiteral("0"), 300'000.0);
    const auto span = writes(Access::issued(backend), cmd::kScope, scope::kSpan);
    check(span.size() == 1 && span.front().data.front() == 0x00
              && decodeFreq(std::span<const std::uint8_t>(span.front().data).subspan(1))
                     .value_or(0) == 250'000,
          "zoom-out from 100 kHz steps to the next span (250 kHz), not back");
}

std::optional<CivFrame> scopeEdgeSweep(std::uint64_t lowerHz, std::uint64_t upperHz,
                                       std::uint8_t civAddress = 0xB6)
{
    std::vector<std::uint8_t> body{0x00, encodeBcdByte(1), encodeBcdByte(1), 0x03};
    const auto lower = encodeFreq(lowerHz);
    const auto upper = encodeFreq(upperHz);
    body.insert(body.end(), lower.begin(), lower.end());
    body.insert(body.end(), upper.begin(), upper.end());
    body.push_back(0x00);
    body.insert(body.end(), static_cast<std::size_t>(kScopePointsIc705), 0x20);
    return parseFrame(buildFrameSub(civAddress, cmd::kScope, scope::kWaveData, body));
}

CivFrame frequencyEcho(std::uint64_t hz)
{
    const auto bytes = encodeFreq(hz);
    return frame(cmd::kReadFreq, std::nullopt, std::vector<std::uint8_t>(bytes.begin(), bytes.end()));
}

struct EdgeWrite {
    int range = 0;
    int slot = 0;
    std::uint64_t lowerHz = 0;
    std::uint64_t upperHz = 0;
};

std::vector<EdgeWrite> edgeWrites(const std::vector<CivFrame>& frames)
{
    std::vector<EdgeWrite> out;
    for (const CivFrame& f : writes(frames, cmd::kScope, scope::kFixedEdge)) {
        if (f.data.size() < 2 + 2 * kFreqBytes) {
            continue;
        }
        const std::span<const std::uint8_t> d(f.data);
        out.push_back({decodeBcdByte(d[0]), decodeBcdByte(d[1]),
                       decodeFreq(d.subspan(2, kFreqBytes)).value_or(0),
                       decodeFreq(d.subspan(2 + kFreqBytes, kFreqBytes)).value_or(0)});
    }
    return out;
}

// SCROLL-F (#6168): the window is free. A drag scrolls without retuning and
// stops short of losing the VFO; a reveal lands in the range of the tune it
// reveals, even when it arrives first; a zoom is one write.
void testScrollFPanIntents()
{
    IcomCivBackend backend;
    Access::prepare(backend, "IC-7300MK2");
    Access::scrollF(backend);
    const auto sweep = scopeEdgeSweep(14'000'000, 14'100'000);
    check(sweep.has_value(), "SCROLL-F fixture sweep parses");
    if (!sweep) {
        return;
    }
    Access::deliver(backend, frequencyEcho(14'050'000));
    Access::deliver(backend, *sweep);
    const auto retunes = [](const std::vector<CivFrame>& frames) {
        return any(frames, [](const CivFrame& f) {
            return (f.cmd == cmd::kSetFreq || f.cmd == cmd::kSetFreqTrx) && !f.data.empty();
        });
    };

    Access::forget(backend);
    backend.setPanCenter(QStringLiteral("0"), 14'030'000.0, IRadioBackend::PanCenterIntent::Drag);
    QCoreApplication::processEvents();
    auto edges = edgeWrites(Access::issued(backend));
    check(!retunes(Access::issued(backend)), "a SCROLL-F drag does not retune");
    check(edges.size() == 1 && edges.front().range == 6 && edges.front().slot == 4
              && edges.front().lowerHz == 13'980'000 && edges.front().upperHz == 14'080'000,
          "it writes the dragged window to slot 4 of range 06");

    Access::forget(backend);
    QSignalSpy pan(&backend, &IRadioBackend::panCenterBandwidthChanged);
    backend.setPanCenter(QStringLiteral("0"), 13'900'000.0, IRadioBackend::PanCenterIntent::Drag);
    QCoreApplication::processEvents();
    edges = edgeWrites(Access::issued(backend));
    check(edges.size() == 1 && edges.front().upperHz == 14'052'000,
          "a drag that would lose the VFO stops with it 2 kHz inside the edge");
    check(!pan.isEmpty(), "and the view is told where it stopped");

    // Memory recall order: the reveal arrives before the tune it reveals.
    Access::forget(backend);
    backend.setPanCenter(QStringLiteral("0"), 400'900.0, IRadioBackend::PanCenterIntent::Range);
    backend.setSliceFrequency(0, 400'900.0);
    QCoreApplication::processEvents();
    edges = edgeWrites(Access::issued(backend));
    check(edges.size() == 1 && edges.front().range == 1
              && edges.front().lowerHz == 351'000 && edges.front().upperHz == 451'000,
          "a reveal before its tune is centred in the tune's range, not the old band's");

    // A zoom's centre and width travel as one write.
    Access::deliver(backend, frequencyEcho(400'900));
    Access::deliver(backend, *scopeEdgeSweep(351'000, 451'000));
    Access::forget(backend);
    pan.clear();
    backend.setPanCenter(QStringLiteral("0"), 400'000.0, IRadioBackend::PanCenterIntent::Range);
    backend.setPanBandwidth(QStringLiteral("0"), 50'000.0);
    check(!pan.isEmpty() && std::fabs(pan.first().at(2).toDouble() - 0.05) < 1e-9,
          "a zoom announces its new width at once");
    QCoreApplication::processEvents();
    edges = edgeWrites(Access::issued(backend));
    check(edges.size() == 1 && edges.front().upperHz - edges.front().lowerHz == 50'000,
          "a zoom costs one edge write, at the new width");
}

// A VFO move AetherSDR did not command (the dial) is followed; our own is not.
void testScrollFFollowsTheDial()
{
    IcomCivBackend backend;
    Access::prepare(backend, "IC-7300MK2");
    Access::scrollF(backend);
    Access::deliver(backend, frequencyEcho(14'050'000));
    Access::deliver(backend, *scopeEdgeSweep(14'000'000, 14'100'000));

    Access::forget(backend);
    backend.setSliceFrequency(0, 14'095'000.0);
    Access::deliver(backend, frequencyEcho(14'095'000));
    QCoreApplication::processEvents();
    check(edgeWrites(Access::issued(backend)).empty(),
          "the echo of our own tune does not move the window (the GUI places it)");

    Access::forget(backend);
    Access::deliver(backend, frequencyEcho(14'097'000));
    QCoreApplication::processEvents();
    const auto edges = edgeWrites(Access::issued(backend));
    check(edges.size() == 1 && edges.front().upperHz == 14'122'000,
          "a dial move into the edge zone pans so the VFO sits 25 % inside");
}

// CW text keyer limits: rejected text emits nothing; 30 characters fit.
void testCwTextLimits()
{
    TxTestAuthority authority;
    IcomCivBackend backend;
    Access::prepare(backend, "IC-705");
    const RadioCapabilities caps = backend.capabilities();
    check(caps.cwTextMaxMessageChars == 30 && caps.cwTextMinWpm == 6 && caps.cwTextMaxWpm == 48,
          "CWK publishes its 30-character, 6..48 WPM limits");
    check(!backend.sendCwText(QStringLiteral("CQ ♥ TEST"), authority.operation).isEmpty(),
          "non-Latin-1 CW text is rejected, not rewritten");
    check(!backend.sendCwText(QStringLiteral("CQ*TEST"), authority.operation).isEmpty(),
          "a Latin-1 character outside the CWK set is rejected");
    check(!backend.sendCwText(QString(31, QLatin1Char('A')), authority.operation).isEmpty(),
          "a 31-character message is rejected");
    check(writes(Access::issued(backend), cmd::kCwMessage, std::nullopt).empty(),
          "a rejected message emits no CI-V frame");
    check(backend.sendCwText(QString(30, QLatin1Char('A')), authority.operation).isEmpty(),
          "a 30-character message is accepted");
    const auto sent = writes(Access::issued(backend), cmd::kCwMessage, std::nullopt);
    check(sent.size() == 1 && sent.front().data.size() == 30,
          "as exactly one bounded command 17 frame");
}

// 48 kHz mono in becomes ~24 kHz interleaved stereo out, on both feeds.
void testReceiveAudioRatio()
{
    IcomCivBackend backend;
    Access::prepare(backend, "IC-705");
    Access::sessionConnected(backend);
    qint64 sliceBytes = 0;
    int sliceBuffers = 0;
    int speakerBuffers = 0;
    QObject::connect(&backend, &IRadioBackend::sliceAudioFrameReady,
                     [&](int, const PcmFrame& pcm) {
                         ++sliceBuffers;
                         sliceBytes += pcm.legacyStereo24().size();
                     });
    QObject::connect(&backend, &IRadioBackend::audioFrameReady,
                     [&](const PcmFrame&) { ++speakerBuffers; });
    constexpr int kIn = 4800;
    for (int block = 0; block < 4; ++block) {
        std::vector<float> mono(kIn);
        for (int i = 0; i < kIn; ++i) {
            mono[static_cast<std::size_t>(i)] =
                0.25f * static_cast<float>(std::sin(2.0 * M_PI * 1000.0 * (block * kIn + i) / 48000.0));
        }
        Access::audio(backend, mono);
    }
    const qint64 frames = sliceBytes / static_cast<qint64>(2 * sizeof(float));
    check(sliceBytes % static_cast<qint64>(2 * sizeof(float)) == 0,
          "the per-slice feed is whole interleaved stereo frames");
    check(frames > 4 * 2400 - 600 && frames <= 4 * 2400,
          "4 x 4800 mono samples at 48 kHz become ~4 x 2400 stereo frames at 24 kHz");
    check(sliceBuffers > 0 && sliceBuffers == speakerBuffers,
          "every speaker buffer has its per-slice twin");
}

// The CI-V debug trace tags commands at the right index in each direction.
QStringList g_civLines;
QtMessageHandler g_previous = nullptr;
void captureCiv(QtMsgType type, const QMessageLogContext& ctx, const QString& msg)
{
    if (ctx.category && std::strcmp(ctx.category, "aether.icom.civ") == 0) {
        g_civLines << msg;
        return;
    }
    if (g_previous) {
        g_previous(type, ctx, msg);
    }
}

QString lastLine(const QString& prefix)
{
    for (auto it = g_civLines.crbegin(); it != g_civLines.crend(); ++it) {
        if (it->startsWith(prefix)) {
            return *it;
        }
    }
    return {};
}

void testTraceTags()
{
    IcomCivBackend backend;
    Access::prepare(backend, "IC-705");
    QLoggingCategory::setFilterRules(QStringLiteral("aether.icom.civ.debug=true"));
    g_previous = qInstallMessageHandler(captureCiv);

    Access::deliver(backend, frame(cmd::kSetting, 0x06, {0x01, 0x01}));
    const QString data = lastLine(QStringLiteral("RX <- 1a 06"));
    check(data.contains(QLatin1String("cmd=1a")) && data.contains(QLatin1String("sub=06")),
          "a four-byte RX reply is tagged with its command and subcommand");

    Access::deliver(backend, frame(cmd::kReadFreq, std::nullopt, {0x00, 0x40, 0x07, 0x14, 0x00}));
    const QString freq = lastLine(QStringLiteral("RX <- 03"));
    check(freq.contains(QLatin1String("cmd=03")) && !freq.contains(QLatin1String("sub=")),
          "a frequency reply is tagged 03 with no invented subcommand");

    backend.setSliceAudioGain(0, 42);
    const QString tx = lastLine(QStringLiteral("TX -> fe fe a4 e0 14 01"));
    check(tx.contains(QLatin1String("cmd=14")) && tx.contains(QLatin1String("sub=01")),
          "a TX frame decodes its tag past the envelope");

    qInstallMessageHandler(g_previous);
    QLoggingCategory::setFilterRules(QString());
}

// Health and civ.session separate the RS-BA1 lease from link liveness.
void testLeaseKeys()
{
    IcomCivBackend backend;
    Access::prepare(backend, "IC-705");
    Access::authenticateLease(backend);
    const auto health = backend.healthSnapshot();
    check(health.values.value(QStringLiteral("lease")).toString().startsWith(
              QLatin1String("authenticated")),
          "health reports the authenticated RS-BA1 lease");
    check(health.values.contains(QStringLiteral("leaseage"))
              && health.values.contains(QStringLiteral("leaseseq"))
              && health.values.contains(QStringLiteral("leasecounts")),
          "health exposes token age, sequence and reply counters");

    QSignalSpy result(&backend, &IRadioBackend::extensionResult);
    backend.invokeExtension(QStringLiteral("icom"), QStringLiteral("civ.session"), 7300, {});
    check(result.count() == 1, "civ.session answers synchronously");
    if (result.count() == 1) {
        const QVariantMap lease = result.first().at(1).toMap();
        check(lease.value(QStringLiteral("authenticated")).toBool()
                  && lease.value(QStringLiteral("tokenRequestId")).toString().startsWith(
                      QLatin1String("0x"))
                  && lease.contains(QStringLiteral("reissuedTokens"))
                  && lease.value(QStringLiteral("initialMaintenanceMs")).toInt() == 30000
                  && lease.contains(QStringLiteral("transport")),
              "civ.session returns the live lease and transport diagnostics");
    }
}

// controls.scrub re-asserts the last known value, never a default, and
// never drives PTT, the tuner or power.
void testScrubReasserts()
{
    IcomCivBackend backend;
    Access::prepare(backend, "IC-705");
    const ControlSpec* rfGain = nullptr;
    for (const ControlSpec& spec : controlSpecs()) {
        if (spec.id == "rf.gain") {
            rfGain = &spec;
        }
    }
    check(rfGain != nullptr, "the registry has an rf.gain row");
    if (!rfGain) {
        return;
    }
    Access::forget(backend);
    check(!backend.scrubDrive(*rfGain) && Access::issued(backend).empty(),
          "an unknown RF gain is not scrubbed with the construction default");
    Access::seedScrubValue(backend, "rf.gain", 37);
    check(backend.scrubDrive(*rfGain), "a known RF gain is re-asserted");
    const auto rf = writes(Access::issued(backend), cmd::kLevel, level::kRf);
    check(rf.size() == 1 && decodeLevel(rf.front().data).value_or(-1) == percentToLevelRaw(37),
          "at the last known 37%, not a default");

    Access::forget(backend);
    const QVariantMap scrub = backend.controlScrub(QString());
    bool sawExcluded = false;
    for (const QVariant& row : scrub.value(QStringLiteral("rows")).toList()) {
        const QString id = row.toMap().value(QStringLiteral("id")).toString();
        sawExcluded |= id == QLatin1String("ptt") || id == QLatin1String("tuner")
            || id == QLatin1String("power");
    }
    check(scrub.value(QStringLiteral("checked")).toInt() > 0 && !sawExcluded,
          "a full scrub has no PTT, tuner or power row");
    check(!any(Access::issued(backend), keysTransmitter)
              && writes(Access::issued(backend), cmd::kPower, std::nullopt).empty(),
          "and emits no keying, tuner or power-off frame");
}

}  // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    qRegisterMetaType<SliceDelta>("SliceDelta");
    qRegisterMetaType<MeterDef>("MeterDef");

    testStaleGenerationDropped();
    testSwrMinimumHold();
    testIc9700PreampWireState();
    testWaiterLifetime(app);
    testCapabilityFlags();
    testTuneDriveRestore();
    testTunePowerAppliesLive();
    testWfmRefusesTransmit();
    testPcAudioRestoresUsb();
    testCompoundModeWrite();
    testPassbandDecomposition();
    testPanIntents();
    testScrollFPanIntents();
    testScrollFFollowsTheDial();
    testCwTextLimits();
    testReceiveAudioRatio();
    testTraceTags();
    testLeaseKeys();
    testScrubReasserts();

    if (g_failures == 0) {
        std::printf("icom_backend_seam_test: all checks passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
