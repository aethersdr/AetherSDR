// #5998: the HL2's own PTT/key input (EP6 ptt_resp) is an observation. In the
// backend it moves the transmit state and mutes receive; it never keys, modulates
// or touches drive. RadioModel follows it with a host key through the gated
// producer path, so the front-panel PTT modulates like MOX, and releases only that.
// A PTT already held when the stream starts or resumes is shown and keys nothing:
// only a press after a release seen on the wire is a key request (#7067 review).
// So is only a level carried by a packet newer than every one already believed: a
// rewound or duplicate EP6 packet neither presses, releases nor arms (#7067 review).
// Socket-free: EP6 status frames go into the backend's own MetisClient on its I/O
// thread. The silence legs wait out MetisClient's real silence timeout, twice.

#include "TestSettingsProfile.h"
#include "TxTestAuthority.h"
#include "core/backends/anan/AnanBackend.h"
#include "core/backends/flex/FlexBackend.h"
#include "core/backends/hl2/Hl2Backend.h"
#include "core/backends/hl2/Hl2RxDsp.h"
#include "core/backends/hl2/Hl2TxDsp.h"
#include "core/backends/hl2/MetisClient.h"
#include "core/backends/hl2/MetisProtocol.h"
#include "core/backends/icom/IcomCivBackend.h"
#include "core/backends/sim/SimBackend.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"
#include "models/TransmitModel.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QList>
#include <QThread>

#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <set>
#include <vector>

namespace AetherSDR::hl2 {

struct MetisClientTestAccess {
    // A session without a socket. The link is marked up so the first frame does
    // not also fire the backend's connect sequence.
    static void setStreaming(MetisClient& client)
    {
        client.m_running = true;
        client.m_linkUp = true;
    }
    static void feed(MetisClient& client, const std::vector<std::uint8_t>& bytes)
    {
        client.handleDatagram(bytes);
    }
    static bool radioPtt(const MetisClient& client) { return client.m_telemetry.ptt; }
    static std::size_t txQueued(const MetisClient& client) { return client.m_txIq.size(); }
    static Cc driveBank(const MetisClient& client) { return client.m_ccTxDrive; }
    static int silenceTimeoutMs() { return MetisClient::kSilenceTimeoutMs; }
    static void watchdogTick(MetisClient& client) { client.onWatchdogTick(); }
};

struct Hl2HardwarePttTestAccess {
    template <typename Fn>
    static void onIo(Hl2Backend& backend, Fn fn)
    {
        QMetaObject::invokeMethod(backend.m_metis, [metis = backend.m_metis, &fn] {
            fn(*metis);
        }, Qt::BlockingQueuedConnection);
    }
    static void drain(Hl2Backend& backend)
    {
        QCoreApplication::sendPostedEvents(&backend, QEvent::MetaCall);
    }
    static void prepare(Hl2Backend& backend)
    {
        backend.m_txAllowed = true;
        onIo(backend, [](MetisClient& metis) {
            metis.enableTransmit(true);
            MetisClientTestAccess::setStreaming(metis);
        });
        auto* dsp = new Hl2RxDsp();
        dsp->moveToThread(backend.m_ioThread);
        backend.m_rx[0].dsp = dsp;
        backend.publishIoDsps();
    }
    static bool configureTxDsp(Hl2Backend& backend)
    {
        bool configured = false;
        onIo(backend, [&backend, &configured](MetisClient&) {
            configured = backend.m_txDsp->configure(Hl2TxDsp::Config{});
        });
        return configured;
    }
    static void drainTxAudio(Hl2Backend& backend)
    {
        // The barrier follows submitTxAudio's queued call on the same I/O thread.
        onIo(backend, [](MetisClient&) {});
        drain(backend);
    }
    // The sequence number the next radioPtt() frame carries.
    static std::uint32_t& nextSeq()
    {
        static std::uint32_t seq = 0;
        return seq;
    }
    // One EP6 datagram whose two status frames carry ptt_resp in C0 bit 0.
    static void radioPtt(Hl2Backend& backend, bool ptt) { radioPttAt(backend, ptt, nextSeq()++); }
    // The same with an explicit sequence number: a replay, a rewind or a wrap.
    static void radioPttAt(Hl2Backend& backend, bool ptt, std::uint32_t seq)
    {
        std::vector<std::uint8_t> pkt(kUsbPacketSize, 0);
        pkt[0] = 0xEF; pkt[1] = 0xFE; pkt[2] = 0x01; pkt[3] = 0x06;
        pkt[4] = static_cast<std::uint8_t>((seq >> 24) & 0xFF);
        pkt[5] = static_cast<std::uint8_t>((seq >> 16) & 0xFF);
        pkt[6] = static_cast<std::uint8_t>((seq >> 8) & 0xFF);
        pkt[7] = static_cast<std::uint8_t>(seq & 0xFF);
        const std::size_t frames[2] = {8, 8 + kFrameSize};
        for (const std::size_t fs : frames) {
            pkt[fs] = pkt[fs + 1] = pkt[fs + 2] = 0x7F;
            pkt[fs + 3] = ptt ? 0x01 : 0x00;   // response address 0, firmware word 0
        }
        onIo(backend, [&pkt](MetisClient& metis) { MetisClientTestAccess::feed(metis, pkt); });
        drain(backend);
    }
    static bool hostMoxOnWire(Hl2Backend& backend)
    {
        bool keyed = false;
        onIo(backend, [&keyed](MetisClient& metis) { keyed = metis.isKeyed(); });
        return keyed;
    }
    static bool clientRadioPtt(Hl2Backend& backend)
    {
        bool ptt = false;
        onIo(backend, [&ptt](MetisClient& metis) { ptt = MetisClientTestAccess::radioPtt(metis); });
        return ptt;
    }
    // The drive bank MetisClient holds: level, PA enable and tune request.
    static Cc driveBank(Hl2Backend& backend)
    {
        Cc bank{};
        onIo(backend, [&bank](MetisClient& metis) { bank = MetisClientTestAccess::driveBank(metis); });
        return bank;
    }
    static std::size_t txQueued(Hl2Backend& backend)
    {
        std::size_t n = 0;
        onIo(backend, [&n](MetisClient& metis) { n = MetisClientTestAccess::txQueued(metis); });
        return n;
    }
    // What the next control packets would put on the wire: any MOX bit, any
    // non-zero transmit sample, and every drive bank seen.
    struct Wire {
        bool mox = false;
        bool txSamples = false;
        std::set<std::array<std::uint8_t, 4>> driveBanks;
    };
    static Wire wire(Hl2Backend& backend)
    {
        Wire w;
        onIo(backend, [&w](MetisClient& metis) {
            for (int i = 0; i < 64; ++i) {
                const auto pkt = metis.buildNextControlPacket();
                const std::size_t frames[2] = {8, 8 + kFrameSize};
                for (const std::size_t fs : frames) {
                    const std::uint8_t c0 = pkt[fs + 3];
                    w.mox = w.mox || (c0 & kC0MoxBit) != 0;
                    if ((c0 & ~kC0MoxBit) == kC0TxDrive) {
                        w.driveBanks.insert({pkt[fs + 4], pkt[fs + 5], pkt[fs + 6], pkt[fs + 7]});
                    }
                    // Each slot is speaker L/R then transmit I/Q, two bytes apiece.
                    for (std::size_t k = 0; k + kTxSampleBytes <= kFramePayload;
                         k += kTxSampleBytes) {
                        for (std::size_t b = kTxSampleBytes / 2; b < kTxSampleBytes; ++b) {
                            w.txSamples = w.txSamples || pkt[fs + 8 + k + b] != 0;
                        }
                    }
                }
            }
        });
        return w;
    }
    static bool dspMuted(Hl2Backend& backend)
    {
        bool muted = false;
        Hl2RxDsp* dsp = backend.m_rx[0].dsp;
        QMetaObject::invokeMethod(dsp, [&muted, dsp] { muted = dsp->isAudioMuted(); },
                                  Qt::BlockingQueuedConnection);
        return muted;
    }
    static bool mixerGateClosed(const Hl2Backend& backend) { return backend.m_rxAudioMuted; }
    static bool holdArmed(const Hl2Backend& backend)
    {
        return backend.m_unkeyUnmuteTimer && backend.m_unkeyUnmuteTimer->isActive();
    }
    static void setHoldMs(Hl2Backend& backend, int ms) { backend.m_unkeyUnmuteHoldMs = ms; }
    static int holdMs(const Hl2Backend& backend) { return backend.m_unkeyUnmuteHoldMs; }
    static void stopStream(Hl2Backend& backend)
    {
        onIo(backend, [](MetisClient& metis) { metis.stop(); });
        drain(backend);
    }
    static void silenceWatchdog(Hl2Backend& backend)
    {
        onIo(backend, [](MetisClient& metis) { MetisClientTestAccess::watchdogTick(metis); });
        drain(backend);
    }
    // The link-down signal alone, with no falling PTT edge ahead of it.
    static void linkDownOnly(Hl2Backend& backend)
    {
        QMetaObject::invokeMethod(backend.m_metis, "linkDown", Qt::BlockingQueuedConnection);
        drain(backend);
    }
    static void tearDown(Hl2Backend& backend) { backend.tearDownReceivers(); }
};

}  // namespace AetherSDR::hl2

using namespace AetherSDR;
using AetherSDR::hl2::Hl2Backend;
using Access = AetherSDR::hl2::Hl2HardwarePttTestAccess;

namespace {

int g_failures = 0;
void check(bool ok, const char* what)
{
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) {
        ++g_failures;
    }
}

void pumpUntilUnmuted(Hl2Backend& backend, int deadlineMs)
{
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < deadlineMs && Access::mixerGateClosed(backend)) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
}

// The radioPtt values transmitChanged carried, in order: what RadioModel is
// offered as a key request (true) or a release (false).
struct KeyRequests {
    QList<bool> seen;
    explicit KeyRequests(Hl2Backend& backend)
    {
        QObject::connect(&backend, &IRadioBackend::transmitChanged, &backend,
                         [this](const TransmitDelta& delta) {
            if (delta.radioPtt) {
                seen << *delta.radioPtt;
            }
        });
    }
};

// The mox values transmitChanged carried, in order.
struct MoxEdges {
    QList<bool> seen;
    explicit MoxEdges(Hl2Backend& backend)
    {
        QObject::connect(&backend, &IRadioBackend::transmitChanged, &backend,
                         [this](const TransmitDelta& delta) {
            if (delta.mox) {
                seen << *delta.mox;
            }
        });
    }
};

void testRadioPttWithHostIdle()
{
    std::printf("\n  radio PTT, host idle\n");
    TxTestAuthority tx;
    Hl2Backend backend;
    Access::prepare(backend);
    check(Access::configureTxDsp(backend), "the transmit DSP is configured");
    int processedAudioBlocks = 0;
    const QMetaObject::Connection audioProbe = QObject::connect(
        &backend, &IRadioBackend::meterUpdate, &backend,
        [&processedAudioBlocks](const QString& name, float) {
            if (name == QStringLiteral("TX:MICPEAK")) {
                ++processedAudioBlocks;
            }
        });
    const QByteArray microphoneAudio(512 * 4, '\x40');
    // An earlier host transmission, so the backend holds a live TX operation: a
    // radio edge that keyed with it would reach the wire.
    backend.setKeying(true, tx.operation, {});
    check(Access::hostMoxOnWire(backend), "control: a host key reaches MetisClient");
    backend.submitTxAudio(microphoneAudio, 24000, TxAudioSource::Microphone, tx.context);
    Access::drainTxAudio(backend);
    check(processedAudioBlocks == 1, "control: host keying sends microphone audio through the DSP");
    backend.setKeying(false, tx.operation, {});
    pumpUntilUnmuted(backend, 10 * Access::holdMs(backend));
    check(!Access::hostMoxOnWire(backend) && !Access::mixerGateClosed(backend),
          "control: the host is unkeyed and receiving again");
    MoxEdges edges(backend);
    const AetherSDR::hl2::Cc idleDrive = Access::driveBank(backend);
    backend.setTxPower(37);
    const AetherSDR::hl2::Cc drive = Access::driveBank(backend);
    check(drive != idleDrive, "control: a power change moves the drive bank");
    check(!Access::wire(backend).driveBanks.empty(),
          "control: a drive change is visible on the wire");
    check(!Access::mixerGateClosed(backend) && !Access::dspMuted(backend),
          "an idle receiver is not muted");

    for (int i = 0; i < 10; ++i) {
        Access::radioPtt(backend, true);
    }
    check(edges.seen == QList<bool>{true}, "ten PTT frames publish one transmit edge");
    check(Access::mixerGateClosed(backend), "the mixer gate closes on radio PTT");
    check(Access::dspMuted(backend), "the demodulator is muted on radio PTT");
    check(!Access::hostMoxOnWire(backend), "the host does not assert MOX");

    const int blocksBeforeRadioAudio = processedAudioBlocks;
    backend.submitTxAudio(microphoneAudio, 24000, TxAudioSource::Microphone, tx.context);
    Access::drainTxAudio(backend);
    check(processedAudioBlocks == blocksBeforeRadioAudio,
          "radio PTT admits no microphone audio to the configured DSP");
    check(Access::txQueued(backend) == 0, "radio PTT queues no transmit samples");
    const Access::Wire during = Access::wire(backend);
    check(!during.mox, "no control packet carries the MOX bit");
    check(!during.txSamples, "no control packet carries transmit samples");
    check(during.driveBanks.empty(), "no drive bank goes out on radio PTT");
    check(Access::driveBank(backend) == drive, "the drive bank is unchanged");

    check(Access::holdMs(backend) > 0, "the release uses the unkey hold");
    Access::radioPtt(backend, false);
    check(edges.seen == (QList<bool>{true, false}), "the falling edge publishes receive");
    check(Access::holdArmed(backend) && Access::mixerGateClosed(backend),
          "the falling edge arms the unkey hold");
    pumpUntilUnmuted(backend, 10 * Access::holdMs(backend));
    check(!Access::mixerGateClosed(backend) && !Access::dspMuted(backend),
          "receive is released when the hold expires");
    check(!Access::hostMoxOnWire(backend), "the host stays unkeyed after release");
    QObject::disconnect(audioProbe);
    Access::tearDown(backend);
}

void testReleaseIsImmediateWithoutHold()
{
    std::printf("\n  release with a zero hold\n");
    Hl2Backend backend;
    Access::prepare(backend);
    Access::setHoldMs(backend, 0);
    Access::radioPtt(backend, true);
    check(Access::mixerGateClosed(backend), "muted on radio PTT");
    Access::radioPtt(backend, false);
    check(!Access::mixerGateClosed(backend) && !Access::dspMuted(backend),
          "one falling frame releases receive, nothing else holds it");
    Access::tearDown(backend);
}

void testHostKeyOverlapsRadioPtt()
{
    std::printf("\n  host key and radio PTT overlap\n");
    {
        TxTestAuthority tx;
        Hl2Backend backend;
        Access::prepare(backend);
        Access::setHoldMs(backend, 0);
        MoxEdges edges(backend);
        backend.setKeying(true, tx.operation, {});
        check(Access::hostMoxOnWire(backend), "control: a host key does reach MetisClient");
        Access::radioPtt(backend, true);
        Access::radioPtt(backend, false);
        check(edges.seen == QList<bool>{true}, "host first: the radio's edges add none");
        check(Access::mixerGateClosed(backend), "host first: still muted while the host is keyed");
        backend.setKeying(false, tx.operation, {});
        check(edges.seen == (QList<bool>{true, false}), "host first: one transmission");
        check(!Access::hostMoxOnWire(backend), "host first: the unkey reaches MetisClient");
        check(!Access::mixerGateClosed(backend), "host first: receive released at the host unkey");
        Access::tearDown(backend);
    }
    {
        TxTestAuthority tx;
        Hl2Backend backend;
        Access::prepare(backend);
        Access::setHoldMs(backend, 0);
        MoxEdges edges(backend);
        Access::radioPtt(backend, true);
        backend.setKeying(true, tx.operation, {});
        backend.setKeying(false, tx.operation, {});
        check(!Access::hostMoxOnWire(backend), "radio first: the host unkey is not held");
        check(edges.seen == QList<bool>{true}, "radio first: a host unkey publishes no receive");
        check(Access::mixerGateClosed(backend) && Access::dspMuted(backend),
              "radio first: still muted after the host unkey");
        QCoreApplication::processEvents();
        check(Access::mixerGateClosed(backend), "radio first: no hold releases it either");
        Access::radioPtt(backend, false);
        check(edges.seen == (QList<bool>{true, false}), "radio first: one transmission");
        check(!Access::mixerGateClosed(backend), "radio first: released on the radio's edge");
        Access::tearDown(backend);
    }
}

void testTxMonitorHearsRadioPtt()
{
    std::printf("\n  TX audio monitor\n");
    Hl2Backend backend;
    Access::prepare(backend);
    backend.setTxAudioMonitor(true);
    Access::radioPtt(backend, true);
    check(!Access::mixerGateClosed(backend), "the monitor keeps receive open on radio PTT");
    backend.setTxAudioMonitor(false);
    check(Access::mixerGateClosed(backend), "monitor off while the radio is keyed mutes");
    Access::tearDown(backend);
}

void testLinkLossFailsToReceive()
{
    std::printf("\n  link loss while radio PTT is asserted\n");
    {
        Hl2Backend backend;
        Access::prepare(backend);
        Access::setHoldMs(backend, 0);
        MoxEdges edges(backend);
        Access::radioPtt(backend, true);
        Access::stopStream(backend);
        check(!Access::clientRadioPtt(backend), "stop: the client forgets the PTT");
        check(edges.seen == (QList<bool>{true, false}), "stop: receive is published");
        check(!Access::mixerGateClosed(backend), "stop: receive is released");
        Access::tearDown(backend);
    }
    {
        Hl2Backend backend;
        Access::prepare(backend);
        Access::setHoldMs(backend, 0);
        MoxEdges edges(backend);
        Access::radioPtt(backend, true);
        Access::silenceWatchdog(backend);
        check(edges.seen == QList<bool>{true}, "silence: a fresh stream is left alone");
        QThread::msleep(static_cast<unsigned long>(
            AetherSDR::hl2::MetisClientTestAccess::silenceTimeoutMs() + 100));
        Access::silenceWatchdog(backend);
        check(!Access::clientRadioPtt(backend), "silence: the client forgets the PTT");
        check(edges.seen == (QList<bool>{true, false}), "silence: receive is published");
        check(!Access::mixerGateClosed(backend), "silence: receive is released");
        Access::tearDown(backend);
    }
    {
        Hl2Backend backend;
        Access::prepare(backend);
        Access::setHoldMs(backend, 0);
        MoxEdges edges(backend);
        Access::radioPtt(backend, true);
        Access::linkDownOnly(backend);
        check(edges.seen == (QList<bool>{true, false}),
              "link-down alone: the backend publishes receive");
        check(!Access::mixerGateClosed(backend), "link-down alone: receive is released");
        Access::tearDown(backend);
    }
}

void testHeldPttIsNotAKeyRequest()
{
    std::printf("\n  a PTT held when the stream starts or resumes\n");
    Hl2Backend backend;
    Access::prepare(backend);
    Access::setHoldMs(backend, 0);
    MoxEdges edges(backend);
    KeyRequests requests(backend);

    Access::radioPtt(backend, true);
    check(edges.seen == QList<bool>{true}, "first frame held: transmit is shown");
    check(Access::mixerGateClosed(backend), "first frame held: receive is muted");
    check(requests.seen.isEmpty(), "first frame held: no key request is offered");
    Access::radioPtt(backend, false);
    check(requests.seen == QList<bool>{false}, "the release is offered");
    Access::radioPtt(backend, true);
    check(requests.seen == (QList<bool>{false, true}),
          "a press after a seen release is a key request");
    Access::radioPtt(backend, false);
    requests.seen.clear();

    // Silence with the PTT held: the stream resumes with it still held.
    Access::radioPtt(backend, true);
    check(requests.seen == QList<bool>{true}, "control: a fresh press is a key request");
    QThread::msleep(static_cast<unsigned long>(
        AetherSDR::hl2::MetisClientTestAccess::silenceTimeoutMs() + 100));
    Access::silenceWatchdog(backend);
    check(requests.seen == (QList<bool>{true, false}), "silence: the release is offered");
    Access::onIo(backend, [](AetherSDR::hl2::MetisClient& metis) {
        AetherSDR::hl2::MetisClientTestAccess::setStreaming(metis);
    });
    Access::radioPtt(backend, true);
    check(Access::clientRadioPtt(backend) && edges.seen.last(),
          "after silence: the held PTT is shown again");
    check(requests.seen == (QList<bool>{true, false}),
          "after silence: the held PTT is no key request");
    Access::radioPtt(backend, false);
    Access::radioPtt(backend, true);
    check(requests.seen == (QList<bool>{true, false, false, true}),
          "after silence: release then press is a key request");
    requests.seen.clear();

    // A release seen before a stop does not arm the next stream either.
    Access::radioPtt(backend, false);
    Access::stopStream(backend);
    Access::onIo(backend, [](AetherSDR::hl2::MetisClient& metis) {
        AetherSDR::hl2::MetisClientTestAccess::setStreaming(metis);
    });
    Access::radioPtt(backend, true);
    check(requests.seen == QList<bool>{false}, "after a restart: the held PTT is no key request");
    check(!Access::hostMoxOnWire(backend), "the host never asserted MOX");
    Access::tearDown(backend);
}

void installTxSlice(RadioModel& model)
{
    if (!model.automationApplySliceFixture(0, QStringLiteral("A")) || !model.slice(0)) {
        qFatal("Could not install the TX slice fixture");
    }
    SliceDelta delta;
    delta.txSlice = true;
    delta.mode = QStringLiteral("USB");
    model.slice(0)->applyChanges(delta);
}

void testModelKeysForRadioPtt()
{
    std::printf("\n  RadioModel on an HL2\n");
    RadioModel model;
    // The production family switch, so the seam is wired as it is for a radio.
    if (!model.rebuildBackendForTest(QStringLiteral("hl2"))) {
        qFatal("No HL2 backend in this build");
    }
    installTxSlice(model);
    auto* backend = qobject_cast<Hl2Backend*>(model.backend());
    if (!backend) {
        qFatal("The HL2 family did not build an Hl2Backend");
    }
    Access::prepare(*backend);
    Access::setHoldMs(*backend, 0);

    QList<bool> edges;
    QObject::connect(&model, &RadioModel::radioTransmittingChanged, &model,
                     [&edges](bool tx) { edges << tx; });

    // An earlier host transmission leaves the model and backend a live operation.
    model.transmitModel().setMox(true);
    model.transmitModel().setMox(false);
    check(edges == (QList<bool>{true, false}) && !Access::hostMoxOnWire(*backend),
          "control: a host MOX cycle is one transmission");
    edges.clear();

    // The stream's first frame already has the PTT down: shown, not keyed.
    Access::radioPtt(*backend, true);
    check(edges == QList<bool>{true}, "a PTT held at stream start is shown");
    check(!Access::hostMoxOnWire(*backend), "a PTT held at stream start keys no host");
    check(!model.transmitModel().isTransmitting(), "nor the model's intent");
    Access::radioPtt(*backend, false);
    check(edges == (QList<bool>{true, false}) && !model.isRadioTransmitting(),
          "its release ends the shown transmission");
    edges.clear();

    Access::radioPtt(*backend, true);
    check(edges == QList<bool>{true}, "radio PTT raises radioTransmittingChanged");
    check(model.isRadioTransmitting(), "isRadioTransmitting() follows the radio");
    check(Access::hostMoxOnWire(*backend), "radio PTT keys the host modulator");
    check(model.transmitModel().isTransmitting(), "through the model's own keying path");

    Access::radioPtt(*backend, false);
    check(!Access::hostMoxOnWire(*backend), "the radio's falling edge unkeys the host");
    check(!model.transmitModel().isTransmitting(), "and releases the model's intent");
    check(edges == (QList<bool>{true, false}), "the press is one transmission");
    check(!model.isRadioTransmitting(), "back to receive");
    edges.clear();

    // A host key held before the radio's PTT owns its own release.
    model.transmitModel().setMox(true);
    Access::radioPtt(*backend, true);
    Access::radioPtt(*backend, false);
    check(Access::hostMoxOnWire(*backend), "the radio's release leaves a host MOX keyed");
    check(model.isRadioTransmitting(), "and the transmission running");
    model.transmitModel().setMox(false);
    check(!Access::hostMoxOnWire(*backend), "host MOX-off unkeys the host");
    check(edges == (QList<bool>{true, false}), "the overlap is one transmission");
    edges.clear();

    // CW: the radio's own keyer raises the same PTT, and the CW path keys itself.
    SliceDelta cw;
    cw.mode = QStringLiteral("CW");
    model.slice(0)->applyChanges(cw);
    Access::radioPtt(*backend, true);
    check(!Access::hostMoxOnWire(*backend), "in CW the radio's PTT keys nothing");
    check(model.isRadioTransmitting(), "in CW the radio's PTT is still shown");
    Access::radioPtt(*backend, false);
    check(!model.isRadioTransmitting(), "in CW the falling edge ends it");
    Access::tearDown(*backend);
}

// A delayed or duplicated EP6 packet is not a press. The loss accounting lets
// rewinds and duplicates through for their IQ; their ptt_resp must not move the
// PTT (#7067 review).
void testStalePacketsAreNoKeyIntent()
{
    std::printf("\n  rewound and duplicate EP6 packets\n");
    {
        // The review's replay, through the real model: release N, press N+1,
        // release N+2, then the same press N+1 again.
        RadioModel model;
        if (!model.rebuildBackendForTest(QStringLiteral("hl2"))) {
            qFatal("No HL2 backend in this build");
        }
        installTxSlice(model);
        auto* backend = qobject_cast<Hl2Backend*>(model.backend());
        if (!backend) {
            qFatal("The HL2 family did not build an Hl2Backend");
        }
        Access::prepare(*backend);
        Access::setHoldMs(*backend, 0);
        model.transmitModel().setMox(true);
        model.transmitModel().setMox(false);

        const std::uint32_t n = Access::nextSeq();
        Access::nextSeq() += 3;
        Access::radioPttAt(*backend, false, n);
        Access::radioPttAt(*backend, true, n + 1);
        check(Access::hostMoxOnWire(*backend) && model.transmitModel().isTransmitting(),
              "control: a fresh press keys the host");
        Access::radioPttAt(*backend, false, n + 2);
        check(!Access::hostMoxOnWire(*backend), "control: its release unkeys");
        Access::radioPttAt(*backend, true, n + 1);
        check(!Access::hostMoxOnWire(*backend), "a replayed press keys no host");
        check(!model.transmitModel().isTransmitting(), "nor the model's intent");
        check(!model.isRadioTransmitting() && !Access::clientRadioPtt(*backend),
              "nor shows transmit");
        Access::radioPttAt(*backend, true, n + 2);
        check(!Access::hostMoxOnWire(*backend), "a press on a duplicate number keys nothing");
        Access::radioPttAt(*backend, true, n + 3);
        check(Access::hostMoxOnWire(*backend), "the next fresh press keys again");
        Access::radioPttAt(*backend, false, n + 1);
        check(Access::hostMoxOnWire(*backend), "a stale release does not unkey it");
        Access::radioPttAt(*backend, false, n + 4);
        check(!Access::hostMoxOnWire(*backend), "a fresh release does");
        Access::nextSeq() = n + 5;
        Access::tearDown(*backend);
    }
    {
        // A PTT held from the stream's start, then a stale release from before it.
        // Believed, that release would arm the held input, and the next held
        // frame would become a key request with no release from the operator.
        Hl2Backend backend;
        Access::prepare(backend);
        Access::setHoldMs(backend, 0);
        KeyRequests requests(backend);
        const std::uint32_t m = 1000;
        Access::radioPttAt(backend, true, m);
        Access::radioPttAt(backend, false, m - 1);
        check(Access::clientRadioPtt(backend), "a stale release leaves the held PTT shown");
        check(requests.seen.isEmpty(), "and offers nothing");
        Access::radioPttAt(backend, true, m + 1);
        check(requests.seen.isEmpty(), "the held PTT is still no key request");
        Access::radioPttAt(backend, false, m + 2);
        Access::radioPttAt(backend, true, m + 3);
        check(requests.seen == (QList<bool>{false, true}),
              "control: a fresh release then press is a key request");
        Access::tearDown(backend);
    }
    {
        // The counter's wrap is a step forward; a replay across it is not.
        Hl2Backend backend;
        Access::prepare(backend);
        Access::setHoldMs(backend, 0);
        KeyRequests requests(backend);
        Access::radioPttAt(backend, false, 0xFFFFFFFEu);
        Access::radioPttAt(backend, true, 0xFFFFFFFFu);
        Access::radioPttAt(backend, false, 0u);
        Access::radioPttAt(backend, true, 1u);
        check(requests.seen == (QList<bool>{true, false, true}),
              "across the wrap: press, release, press");
        Access::radioPttAt(backend, false, 2u);
        Access::radioPttAt(backend, true, 0xFFFFFFFFu);
        check(requests.seen == (QList<bool>{true, false, true, false}),
              "across the wrap: a replayed press is nothing");
        Access::tearDown(backend);
    }
    {
        // A stream that rewinds and stays behind restarted without a reset: after
        // a long run of such packets the PTT is dropped and must be re-armed.
        Hl2Backend backend;
        Access::prepare(backend);
        Access::setHoldMs(backend, 0);
        KeyRequests requests(backend);
        Access::radioPttAt(backend, false, 50000);
        Access::radioPttAt(backend, true, 50001);
        check(requests.seen == QList<bool>{true}, "control: a fresh press");
        std::uint32_t seq = 0;
        for (int i = 0; i < 63; ++i) {
            Access::radioPttAt(backend, false, seq++);
        }
        check(requests.seen == QList<bool>{true} && Access::clientRadioPtt(backend),
              "a short rewound run is ignored");
        Access::radioPttAt(backend, true, seq++);
        check(requests.seen == (QList<bool>{true, false}),
              "a long one is a new stream: the PTT is released");
        Access::radioPttAt(backend, true, seq++);
        check(requests.seen == (QList<bool>{true, false}),
              "and a PTT held across it is no key request");
        Access::radioPttAt(backend, false, seq++);
        Access::radioPttAt(backend, true, seq++);
        check(requests.seen == (QList<bool>{true, false, false, true}),
              "release then press on the new stream keys");
        Access::tearDown(backend);
    }
}

// A backend that publishes mox and does not declare radioPttObservation.
class PlainBackend final : public IRadioBackend {
public:
    RadioCapabilities capabilities() const override
    {
        RadioCapabilities c;
        c.family = QStringLiteral("plain");
        c.canTransmit = true;
        return c;
    }
    bool connected{false};
    bool isConnected() const override { return connected; }
    void connectRadio(const RadioConnectRequest&) override {}
    void disconnectRadio() override {}
    void setSliceFrequency(int, double) override {}
    void setSliceMode(int, const QString&) override {}
    void setSliceFilter(int, int, int) override {}
    void setSliceAgc(int, const QString&, int) override {}
    void setPanCenter(const QString&, double, PanCenterIntent) override {}
    void setKeying(bool, const TxCoordinator::Operation&,
                   const TxCoordinator::Completion&) override {}
    void invokeExtension(const QString&, const QString&, quint64, const QVariant&) override {}
};

void testUndeclaredBackendIsUnchanged()
{
    std::printf("\n  backends that do not declare the observation\n");
    check(Hl2Backend().capabilities().radioPttObservation.has_value(),
          "HL2 declares radioPttObservation");
    check(!Hl2Backend().capabilities().hasRadioPttReadback,
          "HL2 keeps hasRadioPttReadback false");
    check(!FlexBackend().capabilities().radioPttObservation.has_value(), "Flex declares none");
    check(!icom::IcomCivBackend().capabilities().radioPttObservation.has_value(),
          "Icom declares none");
    check(!SimBackend().capabilities().radioPttObservation.has_value(), "the simulator declares none");
    check(!anan::AnanBackend().capabilities().radioPttObservation.has_value(), "ANAN declares none");

    RadioModel model;
    auto owned = std::make_unique<PlainBackend>();
    PlainBackend* backend = owned.get();
    model.setBackendForTest(std::move(owned), QStringLiteral("plain"));
    installTxSlice(model);   // the slice fixture is refused on a connected backend
    backend->connected = true;
    TransmitDelta keyed;
    keyed.mox = true;
    model.applyBackendTransmitDeltaForTest(keyed);
    check(model.isRadioTransmitting(), "control: a published mox reaches the model");
    model.setTransmit(true, TransmitModel::PttSource::Mox);
    model.setTransmit(false, TransmitModel::PttSource::Mox);
    check(!model.isRadioTransmitting(),
          "without the record the command edge still clears the state");
    TransmitDelta ptt;
    ptt.radioPtt = true;
    model.applyBackendTransmitDeltaForTest(ptt);
    check(!model.transmitModel().isTransmitting(),
          "a radioPtt from an undeclared backend keys nothing");
}

}  // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("hl2-hardware-ptt-state-test"));
    QCoreApplication app(argc, argv);
    check(profile.isValid(), "settings are isolated");

    testRadioPttWithHostIdle();
    testReleaseIsImmediateWithoutHold();
    testHostKeyOverlapsRadioPtt();
    testTxMonitorHearsRadioPtt();
    testLinkLossFailsToReceive();
    testHeldPttIsNotAKeyRequest();
    testModelKeysForRadioPtt();
    testStalePacketsAreNoKeyIntent();
    testUndeclaredBackendIsUnchanged();

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAIL", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
