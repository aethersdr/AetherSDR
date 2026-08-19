// IcomCIV — the IRadioBackend seam, against the fake IC-705.
//
// The load-bearing assertion here is the TCI/WSJT-X AUDIO CONTRACT. Everything
// downstream of this backend — the speaker, the TCI receiver channel, the
// decoders WSJT-X runs — consumes interleaved stereo float32 at 24 kHz. The
// radio delivers 48 kHz MONO. Neither half of that conversion is optional:
//
//   * skip the rate conversion and playback runs an octave low, so WSJT-X sees
//     every tone at twice its true frequency and decodes nothing;
//   * skip the channel duplication and TciServer, which divides the buffer by
//     2*sizeof(float), sees half the frames it thinks it has.
//
// Both failures are silent — audio flows, meters move, the session is healthy.
// So they get a test rather than a comment.

#include "IcomFakeRadio.h"

#include "core/backends/icom/IcomCivBackend.h"

#include <QCoreApplication>
#include <QLoggingCategory>
#include <QSignalSpy>
#include <QStringList>
#include <QTest>

#include <algorithm>
#include <array>
#include <cmath>
#include <span>
#include <cstdio>
#include <cstring>

using namespace AetherSDR;
using namespace AetherSDR::icom;
using namespace AetherSDR::icom::test;

static int g_failures = 0;
static void check(bool cond, const char* what)
{
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

// A frame that can MOVE the transmitter or the antenna tuner, as opposed to one
// that merely asks about them.
//
// 1C 00 and 1C 01 each have two forms and only one of them does anything. The
// payload-bearing form is the command — cmdSetPtt writes one byte (00 receive,
// 01 transmit) and cmdSetTuner writes one byte (00 off, 01 on, 02 start a
// matching cycle). The empty form is a READ, which the register answers and
// never acts on. The backend polls exactly that read on a 250 ms cadence from
// onMeterTick, deliberately: m_keyed was set only by our own setKeying() and by
// an unsolicited status frame, so a radio keyed from its own front-panel PTT
// left every transmit meter suppressed and reading "never fed".
//
// Matching on cmd+sub alone cannot tell those two forms apart, and a check that
// cannot tell them apart is not a TX-safety check. It fires on the poll — which
// keys nothing — and it would go on firing at whoever silenced it, until it got
// silenced the easy way.
static bool movesPttOrTuner(const CivFrame& f)
{
    return f.cmd == cmd::kControl && f.hasSub
        && (f.sub == control::kPtt || f.sub == control::kTuner)
        && !f.data.empty();
}

// ---------------------------------------------------------------------------
// CI-V trace capture
//
// traceCiv writes the decoded `cmd=`/`sub=` tag ONLY to qCDebug — the in-memory
// ring stores raw hex. So the tag can only be asserted by capturing log output,
// which is why this needs a message handler rather than a getter.
// ---------------------------------------------------------------------------
static QStringList g_civLines;
static bool g_capturing = false;
static QtMessageHandler g_prevHandler = nullptr;

static void civCapture(QtMsgType type, const QMessageLogContext& ctx, const QString& msg)
{
    if (g_capturing && ctx.category && std::strcmp(ctx.category, "aether.icom.civ") == 0)
        g_civLines << msg;
    if (g_prevHandler)
        g_prevHandler(type, ctx, msg);
}

// The most recent captured line beginning with `prefix`, or a null string.
static QString lastLineStartingWith(const QString& prefix)
{
    for (auto it = g_civLines.crbegin(); it != g_civLines.crend(); ++it)
        if (it->startsWith(prefix))
            return *it;
    return {};
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    qRegisterMetaType<AetherSDR::SliceDelta>("SliceDelta");
    qRegisterMetaType<AetherSDR::MeterDef>("MeterDef");

    FakeIc705 radio;
    IcomCivBackend backend;

    int sliceAudioBuffers = 0;
    qint64 sliceAudioBytes = 0;
    int speakerBuffers = 0;
    QObject::connect(&backend, &IRadioBackend::sliceAudioFrameReady, &app,
                     [&](int, const QByteArray& pcm) {
                         ++sliceAudioBuffers;
                         sliceAudioBytes += pcm.size();
                     });
    QObject::connect(&backend, &IRadioBackend::audioFrameReady, &app,
                     [&](const QByteArray&) { ++speakerBuffers; });

    // ACCUMULATED, not last-wins. Each delta carries only the fields that
    // moved, so a plain "keep the newest" would show one setting and forget the
    // nine that arrived in their own frames a millisecond earlier.
    SliceDelta lastSliceState;
    TransmitDelta lastTransmitState;
    std::vector<QString> publishedModes;
    // Every mox edge in order. The PTT confirmation window is defined by which
    // transitions it lets through, so the SEQUENCE is the assertion, not the
    // final value — a suppressed-then-corrected state and a never-suppressed
    // one both end up in the same place.
    std::vector<bool> moxPublications;
    const auto mergeSlice = [&lastSliceState](const SliceDelta& d) {
        if (d.nr) lastSliceState.nr = d.nr;
        if (d.nb) lastSliceState.nb = d.nb;
        if (d.anf) lastSliceState.anf = d.anf;
        if (d.mn) lastSliceState.mn = d.mn;
        if (d.agcMode) lastSliceState.agcMode = d.agcMode;
        if (d.nrLevel) lastSliceState.nrLevel = d.nrLevel;
        if (d.nbLevel) lastSliceState.nbLevel = d.nbLevel;
        if (d.audioGain) lastSliceState.audioGain = d.audioGain;
        if (d.ritOn) lastSliceState.ritOn = d.ritOn;
        if (d.xitOn) lastSliceState.xitOn = d.xitOn;
        if (d.ritFreq) lastSliceState.ritFreq = d.ritFreq;
        if (d.mode) lastSliceState.mode = d.mode;
        if (d.filterLow) lastSliceState.filterLow = d.filterLow;
        if (d.filterHigh) lastSliceState.filterHigh = d.filterHigh;
    };
    QObject::connect(&backend, &IRadioBackend::sliceChanged, &app,
                     [&](int, const SliceDelta& d) {
                         mergeSlice(d);
                         if (d.mode)
                             publishedModes.push_back(*d.mode);
                     });
    QObject::connect(&backend, &IRadioBackend::transmitChanged, &app,
                     [&](const TransmitDelta& d) {
        if (d.speechProcEnable) lastTransmitState.speechProcEnable = d.speechProcEnable;
        if (d.speechProcLevel) lastTransmitState.speechProcLevel = d.speechProcLevel;
        if (d.rfPower) lastTransmitState.rfPower = d.rfPower;
        if (d.micLevel) lastTransmitState.micLevel = d.micLevel;
        if (d.sbMonitor) lastTransmitState.sbMonitor = d.sbMonitor;
        if (d.voxEnable) lastTransmitState.voxEnable = d.voxEnable;
        if (d.voxLevel) lastTransmitState.voxLevel = d.voxLevel;
        if (d.transmitFreq) lastTransmitState.transmitFreq = d.transmitFreq;
        if (d.atuEnabled) lastTransmitState.atuEnabled = d.atuEnabled;
        if (d.atuStatusRaw) lastTransmitState.atuStatusRaw = d.atuStatusRaw;
        if (d.mox) { lastTransmitState.mox = d.mox; moxPublications.push_back(*d.mox); }
    });

    QSignalSpy connectedSpy(&backend, &IRadioBackend::connected);
    QSignalSpy sliceSpy(&backend, &IRadioBackend::sliceChanged);
    QSignalSpy meterDefSpy(&backend, &IRadioBackend::meterDefined);
    QSignalSpy rfGainInfoSpy(&backend, &IRadioBackend::panRfGainInfoChanged);
    QSignalSpy spectrumSpy(&backend, &IRadioBackend::spectrumFrameReady);

    RadioConnectRequest req;
    req.host = QStringLiteral("127.0.0.1");
    req.port = radio.controlPort();
    req.params.insert(QStringLiteral("icom.serialPort"), radio.serialPort());
    req.params.insert(QStringLiteral("icom.audioPort"), radio.audioPort());
    req.params.insert(QStringLiteral("icom.username"), QStringLiteral("beer"));
    req.params.insert(QStringLiteral("icom.password"), QStringLiteral("beerbeer"));
    req.params.insert(QStringLiteral("icom.civAddress"), 0xA4);

    backend.connectRadio(req);
    check(waitFor([&] { return backend.isConnected(); }), "the backend connects");
    check(connectedSpy.count() == 1, "and emits connected() exactly once");

    quint64 schedulerRequestId = 9000;
    const auto waitSchedulerIdle = [&](int timeoutMs = 5000) {
        QSignalSpy replySpy(&backend, &IRadioBackend::extensionResult);
        const quint64 requestId = ++schedulerRequestId;
        QVariantMap arg;
        arg.insert(QStringLiteral("timeoutMs"), timeoutMs);
        backend.invokeExtension(QStringLiteral("icom"),
                                QStringLiteral("civ.scheduler.wait-idle"),
                                requestId, arg);
        QVariantMap result;
        const bool replied = waitFor([&] {
            for (const QList<QVariant>& reply : replySpy) {
                if (reply.at(0).toULongLong() == requestId) {
                    result = reply.at(1).toMap();
                    return true;
                }
            }
            return false;
        }, timeoutMs + 500);
        return replied && result.value(QStringLiteral("idle")).toBool()
            && !result.value(QStringLiteral("timedOut")).toBool();
    };

    // ---- capability -------------------------------------------------------
    const RadioCapabilities caps = backend.capabilities();
    check(caps.family == QStringLiteral("icom"), "family is icom");
    check(caps.maxSlices == 1, "one slice");
    // The three that are easy to get backwards, each with a real consequence.
    check(!caps.hostModulates,
          "the RADIO modulates — true here would open the host mic on a radio that never uses it");
    check(!caps.hasDaxStreams, "no IQ on any networked Icom — absent, not deferred");
    check(caps.clientSettingsDomains == RadioCapabilities::ClientSettingsDomains{},
          "the radio remembers its own state, so the client restores NOTHING");
    check(caps.hasRadioSideDsp, "NR/NB/notch run in the radio's firmware");
    check(!caps.canReboot, "power-off over WiFi is a one-way trip, so no reboot is offered");

    // ---- a slice exists, which TCI routing depends on ---------------------
    check(sliceSpy.count() >= 1, "a slice is published at connect");
    check(!meterDefSpy.isEmpty(), "meter definitions are published");
    check(rfGainInfoSpy.count() == 1, "the RF-gain range is advertised");
    if (rfGainInfoSpy.count() == 1) {
        const auto args = rfGainInfoSpy.first();
        // THE REAL RF-GAIN REGISTER (14 02), 0000..0255, published as a
        // percentage. This slider used to drive the three-position PREAMP
        // (16 02) and label its positions "0 dB / 1 dB / 2 dB" — the radio
        // calls them OFF, P.AMP1 and P.AMP2 and publishes no gain figures for
        // them, so none of those numbers was a decibel of anything.
        check(args.at(1).toInt() == 0 && args.at(2).toInt() == 100 && args.at(3).toInt() == 1,
              "as the real 0..100 continuous gain");
        // 14 02 has no published dB mapping, so a dB label would be the same
        // invention in a new place.
        check(args.at(4).toString() == QStringLiteral("%"),
              "in percent, because the register has no published dB scale");
    }

    // ---- model discovery (Phase 5) ----------------------------------------
    check(waitFor([&] { return backend.model().civAddress == 0xA4; }),
          "the backend ASKS the radio what it is (0x19 0x00) rather than assuming");
    check(backend.model().name == "IC-705", "and resolves the IC-705");
    check(backend.model().verified, "whose capability numbers are tier-1 verified");
    check(waitSchedulerIdle(),
          "connect-time radio-authoritative state converges through the scheduler");
    const SliceDelta connectedSliceState = lastSliceState;
    const TransmitDelta connectedTransmitState = lastTransmitState;

    // ---- THE AUDIO CONTRACT (TCI / WSJT-X) --------------------------------
    //
    // Feed exactly 4800 mono samples at 48 kHz — 100 ms. The contract says the
    // seam must see 100 ms of INTERLEAVED STEREO at 24 kHz, which is
    // 2400 frames x 2 channels x 4 bytes = 19200 bytes.
    constexpr int kMonoSamplesIn = 4800;
    constexpr qint64 kExpectedBytes = 2400 * 2 * static_cast<qint64>(sizeof(float));
    {
        std::vector<float> mono(kMonoSamplesIn);
        for (int i = 0; i < kMonoSamplesIn; ++i)
            mono[static_cast<std::size_t>(i)] =
                0.25f * std::sin(2.0 * M_PI * 1000.0 * i / 48000.0);
        // Push it the way the radio does: in the protocol's own unequal pair,
        // 682 + 278 samples per 20 ms frame, rather than as one big block.
        std::size_t at = 0;
        while (at < mono.size()) {
            const std::size_t n = std::min<std::size_t>(682, mono.size() - at);
            radio.pushAudio(encodeAudio(AudioCodec::Lpcm1ch16,
                                        std::span<const float>(mono.data() + at, n)));
            at += n;
        }
    }

    check(waitFor([&] { return sliceAudioBytes >= kExpectedBytes * 8 / 10; }),
          "per-slice audio reaches the seam — this IS the TCI receiver channel");

    // r8brain has a startup latency, so the exact byte count lags by a small
    // amount on the first block. What must hold is the RATIO: 4800 mono
    // samples in at 48 kHz must produce ~2400 stereo FRAMES out at 24 kHz.
    const qint64 framesOut = sliceAudioBytes / (2 * static_cast<qint64>(sizeof(float)));
    check(framesOut > 2000 && framesOut < 2600,
          "4800 mono samples at 48 kHz become ~2400 stereo frames at 24 kHz");
    check(framesOut < kMonoSamplesIn * 3 / 4,
          "NOT a passthrough — a backend that skipped the rate conversion would emit ~4800");

    // Stereo, not mono. TciServer divides by 2*sizeof(float); a mono buffer
    // makes it see half the frames it thinks it has.
    check(sliceAudioBytes % (2 * static_cast<qint64>(sizeof(float))) == 0,
          "the buffer is a whole number of INTERLEAVED STEREO frames");

    // Both feeds, and they are different consumers: the speaker gets the mix,
    // the per-slice feed gets one slice for the decoders.
    check(speakerBuffers > 0, "the speaker feed is emitted too");
    check(sliceAudioBuffers == speakerBuffers,
          "one per-slice buffer for every speaker buffer — neither path is starved");

    // ---- spectrum (Phase 2 through the seam) ------------------------------
    {
        std::vector<std::uint8_t> body;
        body.push_back(0x00);
        body.push_back(encodeBcdByte(1));
        body.push_back(encodeBcdByte(1));
        body.push_back(0x00);                       // centre mode
        const auto centre = encodeFreq(14'100'000);
        const auto span   = encodeFreq(100'000);
        body.insert(body.end(), centre.begin(), centre.end());
        body.insert(body.end(), span.begin(), span.end());
        body.push_back(0x00);
        for (int i = 0; i < kScopePointsIc705; ++i)
            body.push_back(static_cast<std::uint8_t>(i % (kScopeMaxAmplitude + 1)));
        std::vector<std::uint8_t> civ{0xFE, 0xFE, kControllerAddress, kIc705Addr,
                                      cmd::kScope, scope::kWaveData};
        civ.insert(civ.end(), body.begin(), body.end());
        civ.push_back(kCivEom);
        radio.pushCiv(civ);
    }
    check(waitFor([&] { return spectrumSpy.count() > 0; }),
          "a scope sweep reaches the seam as a spectrum frame");
    if (spectrumSpy.count() > 0) {
        const QByteArray frame = spectrumSpy.first().at(1).toByteArray();
        check(frame.size() == kScopePointsIc705 * static_cast<int>(sizeof(float)),
              "475 float32 bins");
    }

    // ---- metering (Phase 4) through the seam ------------------------------
    {
        QSignalSpy meterSpy(&backend, &IRadioBackend::meterUpdate);
        // The radio answers the S-meter poll the scheduler is already issuing.
        radio.pushCiv({0xFE, 0xFE, kControllerAddress, kIc705Addr, cmd::kMeter,
                       meter::kSMeter, 0x01, 0x20, kCivEom});   // BCD 0120 == S9
        check(waitFor([&] { return meterSpy.count() > 0; }), "a meter reading reaches the seam");
        if (meterSpy.count() > 0) {
            const auto args = meterSpy.first();
            // SOURCE:NAME, not a bare name. This assertion was left behind when
            // the meters were re-homed onto the convention MeterModel actually
            // looks up; a bare "LEVEL" is published where nothing reads it.
            check(args.at(0).toString() == QStringLiteral("SLC:LEVEL"),
                  "as the SLC:LEVEL meter");
            // Raw 120 is S9. On 20 m that is -73 dBm; the value must be
            // calibrated, not the raw byte.
            check(std::fabs(args.at(1).toDouble() - -73.0) < 1.0,
                  "calibrated to -73 dBm (S9 on HF), not passed through raw");
        }
    }

    // ---- transmit audio is gated on keying --------------------------------
    //
    // A SAFETY GATE, not an optimisation. AudioEngine does not PTT-gate the tap
    // that feeds submitTxAudio, on purpose and by documented contract, so if
    // the backend does not gate then nothing does and the operator's live
    // microphone streams to the radio's modulation input for the whole session
    // — which a radio with VOX enabled keys on (Principle VI).
    {
        const int before = radio.audioPacketsFromClient();
        QByteArray pcm(1024 * 2 * static_cast<int>(sizeof(qint16)), 0);
        auto* w = reinterpret_cast<qint16*>(pcm.data());
        for (int i = 0; i < 1024; ++i) {
            const auto v = static_cast<qint16>(8000 * std::sin(2.0 * M_PI * 1000.0 * i / 24000.0));
            w[i * 2] = v;
            w[i * 2 + 1] = v;
        }

        // UNKEYED: nothing may reach the radio.
        for (int i = 0; i < 20; ++i)
            backend.submitTxAudio(pcm, 24000, /*clientLeveled=*/false);
        QTest::qWait(120);
        check(radio.audioPacketsFromClient() == before,
              "transmit audio is DROPPED while unkeyed");

        // KEYED: the same buffers must arrive.
        radio.clearCivLog();
        backend.setKeying(true);
        check(waitFor([&] {
                  return std::any_of(radio.civCommands().begin(), radio.civCommands().end(),
                                     movesPttOrTuner);
              }, 1000),
              "the keyed intent reaches the radio through the scheduler");
        for (int i = 0; i < 20; ++i)
            backend.submitTxAudio(pcm, 24000, /*clientLeveled=*/false);
        QTest::qWait(200);
        const int keyed = radio.audioPacketsFromClient();
        check(keyed > before, "and flows once keyed");

        // THE TX-SAFETY PREDICATE HAS TEETH. The scrub check further down is a
        // negative — "no frame like this was sent" — and a negative passes for
        // free the moment it stops recognising the thing it forbids. Here a
        // transmitter really was keyed over the same wire and through the same
        // capture, so the predicate is proven to catch it before it is trusted
        // to say the scrub never did.
        check(std::any_of(radio.civCommands().begin(), radio.civCommands().end(),
                          movesPttOrTuner),
              "and a real key IS caught by the TX-safety predicate, so the "
              "scrub's 'never keys' check is not vacuous");

        backend.setKeying(false);
        check(waitSchedulerIdle(), "unkey and its radio confirmation complete");

        // ...and stops again on unkey, rather than draining a backlog into the
        // next transmission.
        QTest::qWait(120);
        const int afterUnkey = radio.audioPacketsFromClient();
        for (int i = 0; i < 20; ++i)
            backend.submitTxAudio(pcm, 24000, /*clientLeveled=*/false);
        QTest::qWait(120);
        check(radio.audioPacketsFromClient() == afterUnkey,
              "and stops again on unkey");
    }

    // ---- THE PTT CONFIRMATION WINDOW, IN BOTH DIRECTIONS ------------------
    //
    // RFC #4983's captured FT8 failure and its explicit counter-rule live here.
    // The window is what lets a newer key-on intent outlive an older poll's OFF
    // answer; it must NOT also let a client's unkey request outlive the radio
    // saying it is still transmitting.
    {
        // (a) THE CAPTURED FAILURE. Key on, then have the radio insist it is
        //     still RX — exactly the pre-key poll answer arriving late. The
        //     model must not follow it back to RX inside the window.
        backend.setKeying(false);
        check(waitSchedulerIdle(), "PTT fixture starts unkeyed");
        moxPublications.clear();

        radio.m_pttOverride = false;         // radio keeps answering "RX"
        backend.setKeying(true);
        QTest::qWait(600);                   // several 250 ms fallback polls
        check(std::find(moxPublications.begin(), moxPublications.end(), false)
                  == moxPublications.end(),
              "a contradictory PTT OFF is suppressed while a key-on intent is "
              "pending — the captured FT8 transmit-audio teardown");
        check(lastTransmitState.mox.value_or(false),
              "and the model stays keyed for the operator who asked to transmit");

        // The window is BOUNDED. Past it the radio wins again (Constitution II),
        // otherwise a client belief outlives the hardware indefinitely.
        QTest::qWait(700);
        check(!lastTransmitState.mox.value_or(true),
              "once the 1 s window expires the radio's own report wins again");

        // (b) THE DIRECTION THAT MUST NEVER BE SUPPRESSED. Ask to unkey while
        //     the radio insists it is transmitting — a lost or refused unkey.
        //     Swallowing this is the one failure that leaves an operator on the
        //     air with a UI that says otherwise (Constitution VI fails closed).
        radio.m_pttOverride = true;
        moxPublications.clear();
        backend.setKeying(false);
        check(waitFor([&] {
                  return std::find(moxPublications.begin(), moxPublications.end(),
                                   true) != moxPublications.end();
              }, 1000),
              "a radio reporting KEYED after an unkey request is published "
              "immediately, NOT suppressed by the confirmation window");
        check(lastTransmitState.mox.value_or(false),
              "and the model shows the transmitter that is actually on the air");

        radio.m_pttOverride.reset();
        backend.setKeying(false);
        check(waitFor([&] { return !lastTransmitState.mox.value_or(true); }, 2000),
              "an obedient radio then unkeys normally");
        check(waitSchedulerIdle(), "PTT fixture drains");
    }

    // ── PC AUDIO OWNS DATA OFF MOD ONLY WHEN THE OPERATOR CLICKS ─────────
    //
    // DATA OFF MOD (1A 05 item 0118 on this radio) is a SET-menu register the
    // RADIO persists. Constitution III therefore forbids the client replaying
    // its own remembered PC Audio flag onto it at connect — and the register is
    // four-valued while the button is two-valued, so "off" has to put back what
    // the operator had rather than assuming MIC. Both were live defects; both
    // are pinned here.
    //
    // The fake starts at USB (0x01), the ordinary setting for an operator with
    // a rig interface on the USB port. A fake already sitting on WLAN could not
    // tell "PC Audio put it back" from "PC Audio never touched it".
    {
        const auto writesTo118 = [](const std::vector<CivFrame>& log) {
            std::vector<int> values;
            for (const CivFrame& f : log) {
                // A WRITE is the three-byte form. The two-byte form is a read,
                // and counting those would make this assertion vacuous.
                if (f.cmd == cmd::kSetting && f.hasSub && f.sub == 0x05
                    && f.data.size() == 3
                    && decodeBcdByte(f.data[0]) * 100 + decodeBcdByte(f.data[1]) == 118) {
                    values.push_back(f.data[2]);
                }
            }
            return values;
        };

        check(waitSchedulerIdle(), "the connect burst drains before the check");
        check(writesTo118(radio.civCommands()).empty(),
              "CONNECT WRITES NOTHING to DATA OFF MOD — the client publishes its "
              "PC Audio state, it does not replay it onto radio-owned config "
              "(Constitution III)");
        check(radio.setting(118) == 0x01,
              "so the operator's own USB selection survives the connect");
        check(waitFor([&] {
                  return backend.healthSnapshot().values.contains(
                      QStringLiteral("dataoffmod"));
              }, 3000),
              "and the client ADOPTED it — Radio Health reports DATA OFF MOD");

        // The observation verb is not a back door to the write.
        radio.clearCivLog();
        backend.invokeExtension(QStringLiteral("icom"),
                                QStringLiteral("audio.pc.state"), 0, true);
        check(waitSchedulerIdle(), "the state publication settles");
        check(writesTo118(radio.civCommands()).empty(),
              "publishing PC Audio state writes nothing either");

        // An operator CLICK is a request, and Principle II allows exactly that.
        radio.clearCivLog();
        backend.invokeExtension(QStringLiteral("icom"),
                                QStringLiteral("audio.pc"), 0, true);
        check(waitSchedulerIdle(), "the PC Audio ON request converges");
        check(writesTo118(radio.civCommands()) == std::vector<int>{0x03},
              "a click selects WLAN (0x03) on an IC-705");
        check(radio.setting(118) == 0x03, "and the radio holds it");

        // ...and OFF restores what was captured, NOT a hardcoded MIC. This is
        // the assertion that fails if the restore is dropped: MIC is 0x00 and
        // the operator's USB is 0x01, so the two cannot be confused.
        radio.clearCivLog();
        backend.invokeExtension(QStringLiteral("icom"),
                                QStringLiteral("audio.pc"), 0, false);
        check(waitSchedulerIdle(), "the PC Audio OFF request converges");
        check(writesTo118(radio.civCommands()) == std::vector<int>{0x01},
              "turning PC Audio off puts back the operator's USB (0x01) — NOT a "
              "hardcoded MIC, which would destroy their rig-interface routing "
              "with no undo");
        check(radio.setting(118) == 0x01, "and the radio ends where it started");
        check(radio.setting(119) == 0x03,
              "DATA MOD is never written by PC Audio — digital routing stays "
              "radio-authoritative");
    }

    // TUNE temporarily borrows the RF-power register. Releasing it must put
    // the operator's ordinary drive back; otherwise a low-power tune silently
    // changes the next voice/data transmission (and the UI follows the poll).
    {
        backend.setTxPower(37);
        check(waitSchedulerIdle(), "ordinary RF power converges before TUNE");
        radio.clearCivLog();
        backend.setTune(true, 10);
        check(waitSchedulerIdle(), "temporary TUNE drive and tuner state converge");
        backend.setTune(false);
        check(waitSchedulerIdle(), "TUNE release and RF-power restore converge");

        std::vector<int> powerWrites;
        for (const CivFrame& f : radio.civCommands()) {
            if (f.cmd == cmd::kLevel && f.hasSub && f.sub == level::kRfPower) {
                if (const auto raw = decodeLevel(f.data))
                    powerWrites.push_back(*raw);
            }
        }
        check(powerWrites.size() >= 2, "TUNE writes a temporary drive and a restore");
        if (powerWrites.size() >= 2) {
            check(std::abs(powerWrites.front() - 25) <= 1,
                  "TUNE applies the requested 10% temporary drive");
            check(std::abs(powerWrites.back() - 94) <= 1,
                  "TUNE release restores the operator's prior 37% RF power");
        }
    }

    // ---- THE CONNECT-TIME STATE PULL --------------------------------------
    //
    // An Icom remembers its own settings across power cycles and reports them on
    // request, so AetherSDR's contract with this family is to READ that state at
    // connect and never push its own (Constitution II/III). Every value the fake
    // radio holds is deliberately NOT the client's default: a client that simply
    // kept its own numbers would look identical to one that read them, and only
    // asserting the radio's exact values can tell the two apart.
    //
    // This is the regression net for the whole class of bug the registry found —
    // a read that is issued and whose reply is dropped looks exactly like a read
    // that was never issued.
    {
        const SliceDelta sl = connectedSliceState;
        const TransmitDelta tx = connectedTransmitState;

        check(sl.nr.value_or(false), "NR ON is adopted from the radio");
        check(sl.nb.value_or(false), "NB ON is adopted");
        check(!sl.anf.value_or(true), "ANF OFF is adopted");
        check(sl.mn.value_or(false), "the manual notch's ON state is adopted");
        check(sl.agcMode.value_or(QString()) == QLatin1String("slow"),
              "AGC SLOW is adopted, not our own default");
        check(std::abs(sl.nrLevel.value_or(-1) - 30) <= 1,
              "the NR LEVEL comes back as ~30%, not the client's default");
        check(std::abs(sl.nbLevel.value_or(-1) - 70) <= 1, "and the NB level as ~70%");
        check(std::abs(sl.audioGain.value_or(-1) - 50) <= 1,
              "AF gain is read at connect — the control this branch made settable");

        check(tx.speechProcEnable.value_or(false), "the compressor's ON state is adopted");
        check(std::abs(tx.speechProcLevel.value_or(-1) - 40) <= 1,
              "and its level as ~40%");
        check(std::abs(tx.rfPower.value_or(-1) - 20) <= 1,
              "RF POWER comes from the radio — pushing our own would key at the "
              "wrong drive on the first transmission");
        check(std::abs(tx.micLevel.value_or(-1) - 60) <= 1, "mic gain as ~60%");
        check(tx.sbMonitor.value_or(false),
              "the TX monitor's ON state is adopted — its reply used to be dropped "
              "by the 0x16 switch's default");
        check(tx.voxEnable.value_or(false),
              "VOX ON is adopted — same dropped-reply bug");
        check(std::abs(tx.voxLevel.value_or(-1) - 80) <= 1, "and the VOX gain as ~80%");
        check(tx.atuEnabled.value_or(false) && tx.atuStatusRaw.has_value(),
              "the antenna tuner reports its own state, so the ATU button opens "
              "where the radio is");
        check(std::fabs(tx.transmitFreq.value_or(0.0) - 14.074) < 1e-6,
              "the Icom VFO also seeds TX frequency for frequency-aware ATU toggling");

        check(sl.ritOn.value_or(false), "RIT ON is adopted");
        check(!sl.xitOn.value_or(true), "XIT OFF is adopted");
        check(sl.ritFreq.value_or(0) == -1230,
              "and the RIT OFFSET comes back SIGNED — folding the sign byte into "
              "the magnitude tunes the wrong way");

        // DATA MODE, read from 26 and NOT inferrable from anything else.
        // The fake radio sits in USB-D; mode byte 0x01 is plain USB, so a client
        // that decodes only 04 reports USB on a radio the operator has already
        // put in a data mode. That is not cosmetic — it is the state that
        // decides whether the radio modulates from the network or the mic.
        check(sl.mode.value_or(QString()) == QLatin1String("DIGU"),
              "the radio's own DATA-ON state is adopted at connect, so USB-D "
              "reads as DIGU rather than as plain USB");
    }

    // ---- DATA MODE IS SENT, AND ADOPTED (#4984) ---------------------------
    //
    // DIGU and USB are the SAME mode byte on the wire. The only thing that
    // separates them is command 26, and until it was wired the backend inferred
    // DATA from the neutral name and told the radio nothing: selecting DIGU on
    // a radio in DATA OFF left it in plain USB, modulating from the MICROPHONE,
    // while AetherSDR's mode indicator, passband and capabilities all said
    // DIGU. Nothing errored — digital transmit looked wired and made no output.
    //
    // The three halves of that, each checked below: we SEND it, we ADOPT what
    // the radio reports, and the radio's report WINS over what we asked for.
    {
        // --- app-originated: one frame carries all three -------------------
        radio.clearCivLog();
        backend.setSliceMode(0, QStringLiteral("DIGU"));
        QTest::qWait(150);

        const auto& sent = radio.civCommands();
        auto write26 = std::find_if(sent.begin(), sent.end(), [](const CivFrame& f) {
            return f.cmd == cmd::kVfoMode && f.hasSub && f.sub == vfoMode::kSelected
                && f.data.size() >= 3;
        });
        check(write26 != sent.end(),
              "selecting DIGU sends 26 00 — the only command that can express "
              "the DATA flag at all");
        check(write26 != sent.end()
                  && write26->data[0] == static_cast<std::uint8_t>(CivMode::Usb)
                  && write26->data[1] == 0x01,
              "carrying mode USB *and* DATA ON — without the flag the radio "
              "stays in plain USB and transmits from the microphone");
        check(write26 != sent.end() && write26->data[2] >= 1 && write26->data[2] <= 3,
              "and the filter slot in the SAME frame, so mode, DATA and slot "
              "are applied or refused as one unit");
        // The negative half, and the one that matters: 06 is what CLEARS DATA
        // on the radio. A mode change that still sent it would undo the flag it
        // had just asked for, depending only on which frame the radio saw last.
        check(std::none_of(sent.begin(), sent.end(), [](const CivFrame& f) {
                  return f.cmd == cmd::kSetMode && !f.data.empty();
              }),
              "and NO bare 06 goes out alongside it — 06 clears DATA, so the two "
              "together would be a race with the radio's own side effects");
        check(std::any_of(sent.begin(), sent.end(), [](const CivFrame& f) {
                  return f.cmd == cmd::kVfoMode && f.hasSub
                      && f.sub == vfoMode::kSelected && f.data.empty();
              }),
              "followed by a CONFIRMATION read — the write is a request, only "
              "the radio's answer is state (Constitution II)");
        check(waitFor([&] { return radio.m_dataOn; }),
              "and the radio ends up actually in DATA mode");

        // --- ...and back off again -----------------------------------------
        radio.clearCivLog();
        backend.setSliceMode(0, QStringLiteral("USB"));
        check(waitFor([&] { return !radio.m_dataOn; }),
              "selecting plain USB clears DATA on the radio rather than leaving "
              "a data mode latched under a voice-mode indicator");

        // --- a filter change must not drop the radio out of DATA ------------
        //
        // The whole reason the three travel together. Sent as 06 this took an
        // operator running FT8 in USB-D back to plain USB — and their transmit
        // audio back to the microphone — from a button that says nothing about
        // the mode.
        backend.setSliceMode(0, QStringLiteral("DIGU"));
        check(waitFor([&] { return radio.m_dataOn; }), "back into DATA for the check");
        radio.clearCivLog();
        backend.setSliceFilter(0, -1800, 0);   // 1.8 kHz — the SSB ladder's FIL3
        check(waitFor([&] { return radio.m_filter == 3; }),
              "the filter request reaches the radio and selects FIL3");
        check(waitSchedulerIdle(), "the filter write's compound readback completes");
        check(radio.m_dataOn,
              "and leaves it IN DATA — a filter button must not silently take an "
              "FT8 operator back to microphone audio");
        check(std::none_of(radio.civCommands().begin(), radio.civCommands().end(),
                           [](const CivFrame& f) {
                               return f.cmd == cmd::kSetMode && !f.data.empty();
                           }),
              "because the slot went out as 26 restating DATA, not as a bare 06 "
              "— which is the frame that clears it");
        check(std::any_of(radio.civCommands().begin(), radio.civCommands().end(),
                          [](const CivFrame& f) {
                              return f.cmd == cmd::kVfoMode && f.hasSub
                                  && f.sub == vfoMode::kSelected && f.data.empty();
                          }),
              "and the filter write is confirmed by reading the compound state back");

        // --- front-panel adoption -------------------------------------------
        //
        // The radio pushes 01 <mode> <filter> when the operator turns the MODE
        // knob, and that frame CANNOT carry DATA. Following the DATA half means
        // asking, which is what the 26 read on the unsolicited form does.
        lastSliceState.mode.reset();
        publishedModes.clear();
        radio.frontPanelMode(static_cast<std::uint8_t>(CivMode::Lsb), 1, /*dataOn=*/true);
        check(waitFor([&] {
                  return lastSliceState.mode.value_or(QString()) == QLatin1String("DIGL");
              }),
              "a front-panel LSB-D reaches the model as DIGL — the 01 push says "
              "only LSB, so this proves the DATA half was asked for and adopted");
        check(std::all_of(publishedModes.begin(), publishedModes.end(), [](const QString& m) {
                  return m == QLatin1String("DIGL");
              }),
              "without first publishing a stale voice/DATA combination");

        lastSliceState.mode.reset();
        publishedModes.clear();
        radio.frontPanelMode(static_cast<std::uint8_t>(CivMode::Lsb), 1, /*dataOn=*/false);
        check(waitFor([&] {
                  return lastSliceState.mode.value_or(QString()) == QLatin1String("LSB");
              }),
              "and leaving DATA on the front panel comes back as plain LSB, so "
              "the flag clears as well as sets");
        check(std::all_of(publishedModes.begin(), publishedModes.end(), [](const QString& m) {
                  return m == QLatin1String("LSB");
              }),
              "without transiently republishing DIGL from the stale DATA flag");

        // --- the radio wins over our optimistic publish ----------------------
        //
        // Constitution II: setSliceMode publishes DIGU immediately because the
        // passband cannot come from anywhere else, but that is a guess. A radio
        // that refuses DATA must pull the indicator back rather than leaving it
        // claiming a mode the radio is not in.
        radio.m_refuseDataMode = true;
        lastSliceState.mode.reset();
        backend.setSliceMode(0, QStringLiteral("DIGU"));
        check(waitFor([&] {
                  return lastSliceState.mode.value_or(QString()) == QLatin1String("USB");
              }),
              "when the radio REFUSES DATA, its own report corrects the "
              "optimistic DIGU back to USB instead of the indicator lying");
        radio.m_refuseDataMode = false;
    }

    // ---- shared 0..255 percentage write buckets --------------------------
    // RF power, mic gain and monitor level were the three visible reports, but
    // they all use the same Icom register scale. Prove the real setter frames
    // select raw 26 for 10%; raw 25 is what the old floor encoder sent and the
    // radio's front panel correctly displays that as 9.
    {
        radio.clearCivLog();
        backend.setTxPower(10);
        backend.setMicGain(10);
        backend.setTxMonitor(true, 10);
        check(waitSchedulerIdle(), "percentage control writes and confirmations converge");

        const auto rawFor = [&](std::uint8_t sub) -> std::optional<int> {
            const auto& frames = radio.civCommands();
            auto it = std::find_if(frames.begin(), frames.end(), [=](const CivFrame& f) {
                return f.cmd == cmd::kLevel && f.hasSub && f.sub == sub
                    && !f.data.empty();
            });
            return it == frames.end() ? std::nullopt : decodeLevel(it->data);
        };
        check(rawFor(level::kRfPower).value_or(-1) == 26,
              "RF power 10 writes raw 26, which the radio displays as 10");
        check(rawFor(level::kMicGain).value_or(-1) == 26,
              "mic gain 10 uses the same radio-exact bucket");
        check(rawFor(level::kMonitor).value_or(-1) == 26,
              "monitor level 10 uses the same radio-exact bucket");
    }

    // ---- the CI-V stall detector ------------------------------------------
    //
    // The transport can be healthy while the COMMAND PLANE is dead: the control
    // stream keeps pinging, link statistics keep climbing, isConnected() stays
    // true, and the radio has answered no CI-V frame for a minute. That happened
    // on the bench and took real time to diagnose, because nothing anywhere said
    // so — every meter simply stopped at the same instant.
    //
    // What makes it triageable is naming the LAST COMMAND SENT. "The radio
    // stopped answering after 16 02 02" is a bug report; "the radio stopped
    // answering" is a guess.
    {
        const auto periodicallyRead = [&](std::uint8_t command, std::uint8_t sub) {
            return std::any_of(radio.civCommands().begin(), radio.civCommands().end(),
                               [=](const CivFrame& f) {
                return f.cmd == command && f.hasSub && f.sub == sub && f.data.empty();
            });
        };

        // Prove reconciliation while CI-V is healthy. Once replies stop, each
        // transaction is deliberately bounded by the timeout; a silence test
        // should not require the scheduler to spray every register into a
        // black hole merely to prove those reads exist.
        radio.clearCivLog();
        check(waitFor([&] {
                  return periodicallyRead(cmd::kFunction, func::kNoiseReduce)
                      && periodicallyRead(cmd::kFunction, func::kNoiseBlanker)
                      && periodicallyRead(cmd::kLevel, level::kNrLevel)
                      && periodicallyRead(cmd::kLevel, level::kNbLevel);
              }, 5000),
              "NR/NB state and levels are periodically reconciled while CI-V is live");

        radio.clearCivLog();
        radio.setCivSilent(true);

        // A command the radio receives and does not answer.
        backend.setPanPreamp(QStringLiteral("0"), 1);
        check(waitFor([&] {
                  return std::any_of(radio.civCommands().begin(), radio.civCommands().end(),
                                     [](const CivFrame& f) {
                                         return f.cmd == cmd::kFunction && f.hasSub
                                             && f.sub == func::kPreamp;
                                     });
              }, 1000),
              "a deaf radio still RECEIVES — the command reached it");

        // The detector needs its own threshold to elapse with no inbound frame.
        // Deliberately wall-clock rather than injectable: the thing being tested
        // is that a real session notices real silence.
        QSignalSpy healthSpy(&backend, &IRadioBackend::linkStatsUpdated);
        QTest::qWait(7000);

        check(healthSpy.count() > 0,
              "link statistics keep flowing while the command plane is dead — "
              "which is exactly why the transport cannot be trusted to report this");

        // The warning is emitted through the log category rather than a signal,
        // so what is asserted here is the STATE that produces it: the backend
        // still believes it is connected while nothing has come back.
        check(backend.isConnected(),
              "and isConnected() still reports true — the lie the detector exists "
              "to break");

        radio.setCivSilent(false);
        QTest::qWait(300);
    }

    // ---- the pan intents --------------------------------------------------
    //
    // The sweep pushed above put the radio at a 100 kHz span (200 kHz wide),
    // which is what both of these reason against.
    {
        // A ZOOM'S CENTRE MUST NOT RETUNE THE RADIO. Centre and bandwidth
        // travel together on a range change, so every zoom click carries a
        // centre; honouring it walked the VFO across the band one click at a
        // time. Refused, and re-asserted immediately so the view does not
        // drift for a frame before the next sweep contradicts it.
        radio.clearCivLog();
        QSignalSpy panSpy(&backend, &IRadioBackend::panCenterBandwidthChanged);
        backend.setPanCenter(QStringLiteral("0"), 14'050'000.0,
                             IRadioBackend::PanCenterIntent::Range);
        QTest::qWait(120);

        const auto& sent = radio.civCommands();
        const bool retuned = std::any_of(sent.begin(), sent.end(), [](const CivFrame& f) {
            return f.cmd == cmd::kSetFreq || f.cmd == cmd::kSetFreqTrx;
        });
        check(!retuned, "a zoom's pan-centre sends NO frequency command");
        check(panSpy.count() > 0,
              "and re-asserts the radio's real centre, so the view snaps back "
              "rather than drifting for a frame");
        if (panSpy.count() > 0) {
            check(std::fabs(panSpy.first().at(1).toDouble() - 14.1) < 1e-6,
                  "re-asserted at the radio's centre, not the requested one");
        }
    }

    {
        // A DRAG RETUNES, and does not snap back.
        //
        // In centre mode the scope window IS the operating frequency, so the
        // window cannot slide over stationary spectrum: refusing a drag left
        // the trace following the mouse for one frame and then jumping back,
        // which is the panadapter's most basic gesture reading as a bug. The
        // radio is at 14.1 MHz with a 200 kHz window; 14.05 is a quarter of the
        // span away, far outside the dead zone.
        radio.clearCivLog();
        QSignalSpy panSpy(&backend, &IRadioBackend::panCenterBandwidthChanged);
        backend.setPanCenter(QStringLiteral("0"), 14'050'000.0,
                             IRadioBackend::PanCenterIntent::Drag);
        QTest::qWait(120);

        const auto& sent = radio.civCommands();
        auto it = std::find_if(sent.begin(), sent.end(), [](const CivFrame& f) {
            return f.cmd == cmd::kSetFreq;
        });
        check(it != sent.end(), "a pan DRAG reaches the radio as a frequency command");
        check(panSpy.count() == 0,
              "and does NOT re-assert the old centre — no snap-back");
    }

    {
        // THE DEAD ZONE. A click with a pixel of hand movement arrives here as
        // a centre a few Hz off; one-to-one tuning would move the dial on every
        // stray click. 200 Hz against a 100 kHz half-span is well inside the
        // 1% dead zone.
        radio.clearCivLog();
        backend.setPanCenter(QStringLiteral("0"), 14'100'200.0,
                             IRadioBackend::PanCenterIntent::Drag);
        QTest::qWait(120);

        const auto& sent = radio.civCommands();
        const bool retuned = std::any_of(sent.begin(), sent.end(), [](const CivFrame& f) {
            return f.cmd == cmd::kSetFreq || f.cmd == cmd::kSetFreqTrx;
        });
        check(!retuned, "a drag inside the dead zone sends no frequency command");
    }

    {
        // ZOOM OUT MUST ACTUALLY WIDEN. The UI scales by 1.5 and the radio's
        // spans step by 2 and 2.5, so nearest-snapping a widen request returned
        // the span already in use — at every one of the eight spans. The
        // operator clicked zoom out and nothing happened, permanently, because
        // the view is re-seeded from the radio's own sweep 30 times a second.
        radio.clearCivLog();
        // 200 kHz * 1.5 — the exact request the zoom-out button produces here.
        backend.setPanBandwidth(QStringLiteral("0"), 300'000.0);
        QTest::qWait(120);

        const auto& sent = radio.civCommands();
        auto it = std::find_if(sent.begin(), sent.end(), [](const CivFrame& f) {
            return f.cmd == cmd::kScope && f.hasSub && f.sub == scope::kSpan;
        });
        check(it != sent.end(), "a zoom-out request reaches the radio as a span command");
        if (it != sent.end()) {
            // data[0] is the 0x27 family's leading fixed byte; the frequency
            // starts after it.
            check(!it->data.empty() && it->data.front() == 0x00,
                  "the span frame carries the leading fixed 0x00 the radio requires");
            const auto span = decodeFreq(
                std::span<const std::uint8_t>(it->data).subspan(1));
            check(span.has_value() && *span == 250'000,
                  "and steps to the NEXT span up (250 kHz), not back to 100 kHz");
        }
    }

    // ---- controls scrub: RE-ASSERT, never re-default -----------------------
    //
    // `controls scrub` is documented as leaving the radio untouched: it drives
    // every settable control AT ITS CURRENT VALUE and watches the wire, so the
    // question is "does this intent reach a register", not "does the radio
    // obey". That contract has exactly two ways to break, and both are silent —
    // the scrub reports the row LINKED either way, because the frame did reach
    // the wire. The radio is simply left somewhere else afterwards.
    {
        radio.clearCivLog();
        const QVariantMap res = backend.controlScrub(QString());
        check(waitSchedulerIdle(), "the full control scrub drains through the scheduler");
        const auto& sent = radio.civCommands();

        check(!res.contains(QStringLiteral("error")), "the scrub runs on a live session");
        check(res.value(QStringLiteral("broken")).toInt() == 0,
              "scheduler admission keeps the synchronous scrub from falsely "
              "reporting queued controls as broken");
        check(res.value(QStringLiteral("linked")).toInt() > 0,
              "the scrub reports controls admitted to the scheduler as linked");

        // 1. THE MIRROR MUST HOLD THE RADIO'S VALUE, NOT OUR DEFAULT.
        //
        // The scrub re-asserts from the backend's "last intent per control"
        // mirrors, and those were written only by the SETTERS — so on a session
        // where the operator had touched nothing, they still held their
        // construction defaults. The fake radio runs RF gain at 100 %; a scrub
        // reading a default mirror would have driven it to 0 and handed the
        // operator a deaf receiver, then called the row LINKED.
        auto rfGain = std::find_if(sent.begin(), sent.end(), [](const CivFrame& f) {
            return f.cmd == cmd::kLevel && f.hasSub && f.sub == level::kRf;
        });
        check(rfGain != sent.end(), "the scrub drives RF gain");
        if (rfGain != sent.end()) {
            const auto raw = decodeLevel(rfGain->data);
            check(raw.has_value() && *raw >= 250,
                  "and re-asserts the RADIO's 100%, not the mirror's construction "
                  "default of 0 — which would have deafened the receiver and "
                  "reported the row LINKED");
        }
        auto af = std::find_if(sent.begin(), sent.end(), [](const CivFrame& f) {
            return f.cmd == cmd::kLevel && f.hasSub && f.sub == level::kAf;
        });
        check(af != sent.end(), "the scrub drives AF gain");
        if (af != sent.end()) {
            const auto raw = decodeLevel(af->data);
            check(raw.has_value() && std::abs(*raw - 128) <= 2,
                  "at the radio's own ~50%, adopted through the ordinary decode "
                  "path rather than invented here");
        }

        // 2. A VALUE NOBODY ESTABLISHED IS NOT A CURRENT VALUE.
        //
        // The fake IC-705 deliberately does not answer the attenuator read —
        // one lost datagram on the lossy link this backend exists for looks
        // exactly the same. Re-asserting an unestablished mirror sends
        // "attenuator OFF" to an operator who may have it engaged. NOT-TESTED
        // is the honest third outcome, and the scrub already has that state;
        // this is the nr/nb/anf/notch sentinel rule, generalised.
        const bool touchedAtten = std::any_of(sent.begin(), sent.end(),
                                              [](const CivFrame& f) {
            return f.cmd == cmd::kAttenuator && !f.data.empty();
        });
        check(!touchedAtten,
              "a control the radio never reported is NOT driven — re-asserting an "
              "unestablished mirror would switch the operator's attenuator off");

        QString attenStatus;
        for (const QVariant& row : res.value(QStringLiteral("rows")).toList()) {
            const QVariantMap m = row.toMap();
            if (m.value(QStringLiteral("id")).toString() == QLatin1String("atten"))
                attenStatus = m.value(QStringLiteral("status")).toString();
        }
        check(attenStatus == QLatin1String("NOT-TESTED"),
              "and it is REPORTED as NOT-TESTED rather than LINKED, so the check "
              "does not claim coverage it does not have");

        // 3. PTT, the ATU and power-off are never scrubbed (Principle VI).
        //
        // Payload-bearing frames only — see movesPttOrTuner. The window this
        // reads spans the qWait above, so it also catches the backend's own
        // 250 ms PTT-state READ, which is not a scrub frame and keys nothing.
        // Matching that read made this assertion fail on roughly half of all
        // runs purely on timer phase, on a check whose whole job is to be
        // believed when it fires.
        const bool keyed = std::any_of(sent.begin(), sent.end(), movesPttOrTuner);
        check(!keyed, "and the scrub never touches PTT or the antenna tuner");
    }

    // ---- a refused mode correction must SURVIVE the caller ------------------
    //
    // SAM has no IC-705 equivalent, so the backend refuses it and re-asserts
    // what the radio is actually in — otherwise the mode indicator reads SAM
    // over an AM demodulator. The correction has to be QUEUED: SliceModel calls
    // us from modeChangeRequested and emits modeChanged(requestedMode) on the
    // line after that signal returns, so a direct emit here is applied and then
    // announced away, leaving the indicator lying exactly as it did before.
    {
        int corrections = 0;
        QString correctedMode;
        auto conn = QObject::connect(&backend, &IRadioBackend::sliceChanged, &app,
                                     [&](int, const SliceDelta& d) {
            if (d.mode) { ++corrections; correctedMode = *d.mode; }
        });
        backend.setSliceMode(0, QStringLiteral("SAM"));
        check(corrections == 0,
              "a refused mode emits NOTHING synchronously — a direct emit is "
              "overwritten by the caller's own modeChanged on the next line");
        QTest::qWait(50);
        check(corrections == 1 && !correctedMode.isEmpty(),
              "and the correction arrives on the next event-loop turn, naming the "
              "mode the radio is really in");
        QObject::disconnect(conn);
    }

    // ---- the CI-V trace tag decodes the RIGHT byte in BOTH directions -------
    //
    // The two call sites hand traceCiv DIFFERENT layouts, and the tag decoder
    // has to follow:
    //
    //   TX (sendUserCommand) — the raw wire frame from buildFrame:
    //       FE FE <to> <from> <cmd> [<sub>] <data…> FD    -> cmd at index 4
    //   RX (onCivFrame)      — re-serialised, envelope deliberately dropped:
    //       <cmd> [<sub>] <data…>                         -> cmd at index 0
    //
    // Reading index 4 for both is the defect this guards: on RX it printed a
    // PAYLOAD byte as the command, and printed nothing at all for frames
    // shorter than five bytes — which is most of them.
    //
    // These assertions are falsifiable by construction: restore the old
    // `frame[4]` / `size() >= 5` form and the RX cases below fail, because
    // 1A 06 01 01 is four bytes (no tag at all) and a frequency reply puts a
    // BCD digit pair where the command was assumed to be.
    {
        QLoggingCategory::setFilterRules(QStringLiteral("aether.icom.civ.debug=true"));
        g_prevHandler = qInstallMessageHandler(civCapture);
        g_capturing = true;

        // RX, SHORT FRAME — the 1A 06 DATA-flag reply. Four bytes once the
        // envelope is stripped, so the old `size() >= 5` guard skipped it
        // silently. This is the exact reply the category was added to make
        // visible (the radio never volunteers it), so "no tag" is the whole
        // failure rather than a cosmetic one.
        g_civLines.clear();
        radio.pushCiv({0xFE, 0xFE, kControllerAddress, kIc705Addr,
                       cmd::kSetting, 0x06, 0x01, 0x01, kCivEom});
        QTest::qWait(120);
        {
            const QString line = lastLineStartingWith(QStringLiteral("RX <- 1a 06"));
            check(!line.isNull(), "the 1A 06 DATA-flag reply reaches the CI-V trace");
            check(line.contains(QStringLiteral("cmd=1a")),
                  "and a four-byte RX frame is TAGGED — the old size()>=5 guard "
                  "dropped the tag on the very reply this category exists for");
            check(line.contains(QStringLiteral("sub=06")),
                  "with the subcommand read from index 1, not index 5");
        }

        // RX, LONG FRAME — a frequency report. Six bytes, so the old guard
        // PASSED and produced a confidently wrong label: for 14.074 MHz the
        // BCD payload puts 0x14 at index 4, which is cmd::kLevel, so a
        // frequency reply was printed as "cmd=14 sub=00" — a level command.
        // Plausible, wrong, and aimed at someone who switched this on because
        // they no longer trust their reading of the code.
        g_civLines.clear();
        radio.pushCiv({0xFE, 0xFE, kControllerAddress, kIc705Addr,
                       cmd::kReadFreq, 0x00, 0x40, 0x07, 0x14, 0x00, kCivEom});
        QTest::qWait(120);
        {
            const QString line = lastLineStartingWith(QStringLiteral("RX <- 03"));
            check(!line.isNull(), "the frequency report reaches the CI-V trace");
            check(line.contains(QStringLiteral("cmd=03")),
                  "a frequency reply is tagged as READ-FREQUENCY, not as the "
                  "level command its BCD payload happens to spell at index 4");
            check(!line.contains(QStringLiteral("sub=")),
                  "and read-frequency carries no subcommand, so none is invented");
        }

        // TX — unchanged behaviour, asserted so the RX fix cannot silently
        // break the direction that was already right. The raw wire frame keeps
        // its envelope, so the command really is at index 4 here. AF gain is
        // command 14 SUB 01, which is a subcommand-bearing frame — the case
        // where a wrong index would show.
        g_civLines.clear();
        backend.setSliceAudioGain(0, 42);
        QTest::qWait(120);
        {
            // The scheduler keeps draining unrelated commands during the wait,
            // so select the AF-gain frame rather than whichever TX happened last.
            const QString line =
                lastLineStartingWith(QStringLiteral("TX -> fe fe a4 e0 14 01"));
            check(!line.isNull(), "the outbound frame reaches the CI-V trace");
            check(line.contains(QStringLiteral("cmd=14")) && line.contains(QStringLiteral("sub=01")),
                  "a TX frame still decodes from index 4/5, past the envelope");
        }

        g_capturing = false;
        qInstallMessageHandler(g_prevHandler);
        QLoggingCategory::setFilterRules(QString());
    }

    // ---- health -----------------------------------------------------------
    {
        const auto h = backend.healthSnapshot();
        check(!h.isEmpty(), "the health snapshot is populated");
        // The IC-705 reports no PA temperature, and the key is OMITTED rather
        // than reported as zero — "not reported" and "0 degrees" are different
        // answers on a health readout.
        check(!h.values.contains(QStringLiteral("patemp")),
              "no PA temperature key, because the radio does not report one");
        check(h.values.contains(QStringLiteral("model")), "the resolved model is reported");
        check(h.values.contains(QStringLiteral("lease"))
                  && h.values.value(QStringLiteral("lease")).toString().contains(
                      QStringLiteral("authenticated")),
              "health separates the authenticated RS-BA1 lease from UDP link liveness");
        check(h.values.contains(QStringLiteral("leaseseq"))
                  && h.values.contains(QStringLiteral("leasecounts")),
              "health exposes renewal sequence and reply counters");

        QVariant leaseResult;
        bool leaseAnswered = false;
        auto leaseConn = QObject::connect(
            &backend, &IRadioBackend::extensionResult, &app,
            [&](quint64 id, const QVariant& result) {
                if (id == 7300) {
                    leaseAnswered = true;
                    leaseResult = result;
                }
            });
        backend.invokeExtension(QStringLiteral("icom"), QStringLiteral("civ.session"),
                                7300, {});
        QObject::disconnect(leaseConn);
        const QVariantMap lease = leaseResult.toMap();
        check(leaseAnswered && lease.value(QStringLiteral("authenticated")).toBool(),
              "civ.session synchronously returns the live authenticated lease");
        check(lease.value(QStringLiteral("lastRenewalResponse")).toString()
                  == QStringLiteral("0x00000000"),
              "civ.session preserves the protocol response word for troubleshooting");
        check(lease.value(QStringLiteral("tokenRequestId")).toString().startsWith(
                  QStringLiteral("0x")),
              "civ.session exposes the per-login token-request correlation ID");
        check(lease.contains(QStringLiteral("reissuedTokens")),
              "civ.session distinguishes reconnect token reissue from renewal rejection");
        check(lease.value(QStringLiteral("initialMaintenanceMs")).toInt() == 30000,
              "civ.session exposes the one-time early maintenance window");
    }

    // An operator disconnect while TUNE is active must unkey and restore the
    // borrowed RF-power register before the serial command path disappears.
    backend.setTxPower(37);
    check(waitSchedulerIdle(), "disconnect fixture's ordinary power converges");
    backend.setTune(true, 10);
    check(waitSchedulerIdle(), "disconnect fixture reaches active TUNE");
    radio.clearCivLog();
    backend.disconnectRadio();
    QTest::qWait(100);
    const auto restoreOnDisconnect = std::find_if(
        radio.civCommands().begin(), radio.civCommands().end(),
        [](const CivFrame& f) {
            return f.cmd == cmd::kLevel && f.hasSub
                && f.sub == level::kRfPower
                && decodeLevel(f.data).value_or(-1) >= 93;
        });
    check(restoreOnDisconnect != radio.civCommands().end(),
          "disconnect during TUNE restores ordinary RF power before teardown");
    check(!backend.isConnected(), "the backend disconnects cleanly");


    // =======================================================================
    // CI-V ADDRESS RESOLUTION
    // =======================================================================
    //
    // CI-V is ADDRESSED, and a wrong address fails in complete silence: the
    // radio ignores the frame, there is no error, and the session comes up
    // looking healthy with no frequency, no scope and no transmit. Every check
    // below is written around that silence — which is why they assert on the
    // `to` byte of what actually went out and on state that only arrives if
    // something answered, rather than on "it connected".
    //
    // Shared rig: a fake standing in for whichever Icom the case needs.
    struct CivCase {
        FakeIc705 radio;
        IcomCivBackend backend;
        double frequencyMHz = 0.0;
        QStringList warnings;

        CivCase(std::uint8_t addr, const char* name, bool echo = true)
        {
            radio.setCivAddress(addr);
            radio.setDeviceName(name);
            radio.setCivEcho(echo);
            QObject::connect(&backend, &IRadioBackend::sliceChanged, &backend,
                             [this](int, const SliceDelta& d) {
                                 if (d.frequency) frequencyMHz = *d.frequency;
                             });
            QObject::connect(&backend, &IRadioBackend::configurationWarning, &backend,
                             [this](const QString& m) { warnings << m; });
        }

        RadioConnectRequest request()
        {
            RadioConnectRequest r;
            r.host = QStringLiteral("127.0.0.1");
            r.port = radio.controlPort();
            r.params.insert(QStringLiteral("icom.serialPort"), radio.serialPort());
            r.params.insert(QStringLiteral("icom.audioPort"), radio.audioPort());
            r.params.insert(QStringLiteral("icom.username"), QStringLiteral("beer"));
            r.params.insert(QStringLiteral("icom.password"), QStringLiteral("beerbeer"));
            return r;
        }

        // How many frames the client addressed to `to`. Counted from the
        // radio's own log, which records everything the bus carried including
        // the frames it then ignored — so this reports where we SENT, not where
        // we were answered, and those are the two different things this whole
        // change is about.
        [[nodiscard]] int sentTo(std::uint8_t to) const
        {
            return static_cast<int>(std::count_if(
                radio.civCommands().begin(), radio.civCommands().end(),
                [to](const CivFrame& f) { return f.to == to; }));
        }
        // ...AND WHICH COMMAND went there. sentTo() alone cannot tell "the
        // reads recovered" from "the scope switches recovered", and after a
        // retarget those are two different questions with two different
        // answers - which is exactly how a black panadapter hid behind a
        // passing test.
        [[nodiscard]] int sentCmdTo(std::uint8_t to, std::uint8_t command) const
        {
            return static_cast<int>(std::count_if(
                radio.civCommands().begin(), radio.civCommands().end(),
                [to, command](const CivFrame& f) {
                    return f.to == to && f.cmd == command;
                }));
        }
        [[nodiscard]] bool warnedAbout(const char* fragment) const
        {
            return std::any_of(warnings.begin(), warnings.end(),
                               [fragment](const QString& w) {
                                   return w.contains(QLatin1String(fragment));
                               });
        }
    };

    // ---- AUTO: the bug, and the fix ---------------------------------------
    //
    // An IC-9700 lives on 0xA2. Before this change, an operator who left the
    // CI-V field blank got 0xA4 — the IC-705's address — hardcoded at the
    // connect path, so every read went somewhere nobody was listening. The
    // model still resolved by name, capabilities still published, and the
    // result read as "this backend has no panadapter yet".
    {
        CivCase c(0xA2, "IC-9700");
        c.backend.connectRadio(c.request());
        check(waitFor([&] { return c.backend.isConnected(); }),
              "auto: the session comes up on an IC-9700");

        // THE DISCRIMINATING ASSERTION. Not "it connected" and not "the model
        // resolved" — both of those were already true on the broken path.
        check(waitFor([&] { return c.sentTo(0xA2) > 1; }),
              "auto: the reads are addressed to 0xA2, the address this radio "
              "actually answers on");
        check(waitFor([&] { return c.frequencyMHz > 14.0 && c.frequencyMHz < 14.1; }),
              "auto: and the radio ANSWERS - a frequency arrives, where the "
              "0xA4-hardcoded path produced a permanently blank session");
        check(c.backend.model().civAddress == 0xA2, "auto: resolved as the IC-9700");

        // ONE broadcast per connect. Never polled, never retried on a timer:
        // RFC #4983 attributes an unrecoverable CI-V stall to request volume,
        // so the cost of this feature has to stay at exactly one frame.
        const int broadcasts = c.sentTo(kBroadcastAddress);
        check(broadcasts == 1,
              "auto: exactly one broadcast 0x19 0x00 is sent for the whole connect");
    }

    // ---- A DELIBERATELY WRONG TYPED ADDRESS MUST STILL FAIL ----------------
    //
    // The other half of the A/B, and the half that makes the first half mean
    // anything. If auto-detect quietly rescued a wrong entry too, the test
    // above would pass on a backend that simply ignored the operator — and an
    // operator who types an address on a shared bus is SELECTING A DEVICE.
    {
        CivCase c(0xA2, "IC-9700");
        RadioConnectRequest r = c.request();
        r.params.insert(QStringLiteral("icom.civAddress"), 0xA4);
        r.params.insert(QStringLiteral("icom.civAddressPinned"), true);
        c.backend.connectRadio(r);
        check(waitFor([&] { return c.backend.isConnected(); }),
              "wrong pin: the session still comes up - the RS-BA1 transport is fine");
        check(waitFor([&] { return c.sentTo(0xA4) > 1; }, 6000),
              "wrong pin: the reads go where the operator said, not where the "
              "radio is");
        check(c.frequencyMHz == 0.0,
              "wrong pin: and nothing answers - the typed address is NOT "
              "silently overridden");
    }

    // ---- A MODEL PICK IS A SHORTCUT, AND THE WIRE MAY CORRECT IT -----------
    //
    // Same wrong address, expressed the other way: picked from the list rather
    // than typed. The operator picked an IC-705 and is in front of an IC-9700,
    // or picked correctly and later changed the address on the radio. Either
    // way the radio is authoritative about itself, and a pick that is merely a
    // stale shortcut must not cost them the session.
    {
        CivCase c(0xA2, "IC-9700");
        RadioConnectRequest r = c.request();
        r.params.insert(QStringLiteral("icom.civAddress"), 0xA4);   // NOT pinned
        c.backend.connectRadio(r);
        check(waitFor([&] { return c.backend.isConnected(); }),
              "model pick: the session comes up");
        check(waitFor([&] { return c.frequencyMHz > 14.0 && c.frequencyMHz < 14.1; }),
              "model pick: the broadcast reply retargets the session and the "
              "radio answers");
        check(c.sentTo(0xA2) > 1, "model pick: the burst is re-issued at 0xA2");
    }

    // ---- A NAMED MODEL WHOSE ADDRESS WAS CHANGED ON THE RADIO --------------
    //
    // The case auto-detect exists for, and the one every case above misses: the
    // handshake name RESOLVES - so the session is seeded to 0xA2 and the connect
    // edge addresses everything there - but the radio actually answers on 0x50,
    // because someone changed it on the front panel. The cases above either
    // resolve by name to the right address (no retarget) or resolve by neither
    // (no model), so nothing yet exercises a live model on a moved address.
    //
    // The reads recovering is NOT enough to call this fixed. A retarget moves
    // the destination, and everything already sent went to a device that is not
    // there; asserting on frequency alone passes while the panadapter stays
    // black for the rest of the session.
    {
        CivCase c(0x50, "IC-9700");
        c.backend.connectRadio(c.request());
        check(waitFor([&] { return c.backend.isConnected(); }),
              "retarget: the session comes up");
        check(waitFor([&] { return c.frequencyMHz > 14.0 && c.frequencyMHz < 14.1; }),
              "retarget: the broadcast reply moves the reads to 0x50 and the "
              "radio answers");
        check(c.sentTo(0xA2) == 0,
              "retarget: the broadcast identity probe leads the scheduler, so "
              "the queued snapshot for the stale name-derived address is "
              "discarded before it reaches the wire");

        // THE DISCRIMINATING ASSERTION. 0x27 is the scope pair and an IC-9700
        // has a scope, so if "started" is latched per SESSION those two frames
        // exist only at 0xA2: the radio is never told to send sweeps, while
        // capabilities() goes on advertising a panadapter that cannot fill.
        // Frequency and mode all arrive, so the session reads as healthy.
        check(waitFor([&] { return c.sentCmdTo(0x50, cmd::kScope) > 0; }),
              "retarget: the scope switches are re-sent at 0x50 - 'started' is "
              "per DESTINATION, and a retarget is a new destination");
    }

    // ---- AN UNKNOWN NAME IN FRONT OF A MODEL THE TABLE DOES KNOW -----------
    //
    // The RS-BA1 shape: the handshake names the SERVER rather than the radio, so
    // no seed is possible and the burst waits on the broadcast. But the address
    // that comes back is in kModels - 0xB6 is the IC-7300MK2 - so the model IS
    // resolvable, and the burst has to be built against THAT model rather than
    // the conservative fallback the unresolved name left in place.
    //
    // 0x26 is the read #4984 added so a radio left in USB-D is not adopted as
    // plain USB and pushed the rest of the way out of DATA by our first write.
    // It is gated on hasVfoModeCommand, which only the resolved model sets - so
    // a burst re-issued before resolution silently drops it, on the one path
    // where the radio most needs it.
    {
        CivCase c(0xB6, "IC-7760");
        c.backend.connectRadio(c.request());
        check(waitFor([&] { return c.backend.isConnected(); }),
              "late model: the session comes up");
        check(waitFor([&] { return c.frequencyMHz > 14.0 && c.frequencyMHz < 14.1; }, 6000),
              "late model: broadcast alone finds 0xB6 and the reads land");
        check(waitFor([&] { return c.sentCmdTo(0xB6, cmd::kVfoMode) > 0; }, 6000),
              "late model: the DATA-mode read goes out too - the burst is built "
              "against the model the ADDRESS resolved, not the fallback the "
              "unrecognised name left behind");
    }

    // ---- A MODEL NOT IN THE TABLE ------------------------------------------
    //
    // No name seed is possible, so the broadcast is the ONLY thing that can
    // resolve this - which is exactly the case a model chooser cannot cover and
    // auto-detect can. The IC-7760 (0xB2) is a real example: it is missing from
    // kModels today.
    {
        CivCase c(0xB2, "IC-7760");
        c.backend.connectRadio(c.request());
        check(waitFor([&] { return c.backend.isConnected(); }),
              "unknown model: the session comes up");
        check(waitFor([&] { return c.frequencyMHz > 14.0 && c.frequencyMHz < 14.1; }, 6000),
              "unknown model: broadcast alone finds 0xB2 and the reads land");
        check(!c.backend.model().isKnown(),
              "unknown model: capabilities stay on the conservative fallback - "
              "the ADDRESS was learned, the model was not");
        check(c.warnedAbout("not a model AetherSDR has data for"),
              "unknown model: and the reduced radio is EXPLAINED rather than "
              "left looking like a half-finished backend");
    }

    // ---- A SILENT RADIO STILL CONNECTS -------------------------------------
    //
    // The bounded wait must not become a way to hang the connect. Unknown name
    // AND no CI-V answers at all: the worst case, and it has to degrade to
    // exactly today's behaviour rather than to a session that never proceeds.
    {
        CivCase c(0xA2, "IC-7760");
        c.radio.setCivSilent(true);
        c.backend.connectRadio(c.request());
        check(waitFor([&] { return c.backend.isConnected(); }),
              "silent radio: the session still comes up");
        check(waitFor([&] { return c.sentTo(0xA4) > 1; }, 6000),
              "silent radio: the wait times out and the burst goes out at the "
              "fallback address rather than never going out at all");
    }

    // ---- TWO RESPONDERS: ADOPT NEITHER -------------------------------------
    //
    // Icom's own RS-BA1 server can front a serial CI-V bus carrying a second
    // radio, a rotator or an amplifier, and every one of them answers a
    // broadcast. Taking whichever replied first would decode the rest of the
    // session against a device the operator never chose - silently, and with a
    // plausible-looking result.
    //
    // UNMEASURED on hardware: both lab radios are single direct-LAN devices.
    // The behaviour is reasoned, not reproduced, and the test says so.
    {
        CivCase c(0xA2, "IC-7760");
        c.radio.setSecondResponder(0x98);
        c.backend.connectRadio(c.request());
        check(waitFor([&] { return c.backend.isConnected(); }),
              "two responders: the session comes up");
        check(waitFor([&] { return c.warnedAbout("More than one device"); }, 6000),
              "two responders: the ambiguity is REPORTED, not resolved by guessing");
        check(!c.backend.model().isKnown(),
              "two responders: and neither identity is adopted");

        // THE DESTINATION HAS TO REVERT AND STAY REVERTED, which the two
        // assertions above cannot see — both survive the session quietly
        // drifting back onto one of the responders.
        //
        // It really does drift without a guard: the burst issued at the adopted
        // address is already in flight and carries its own directed 0x19 0x00,
        // and that reply arrives AFTER the revert. It matches the address we
        // recorded, so it is not a third responder — and would otherwise fall
        // straight through to the retarget branch and undo the revert.
        QTest::qWait(400);
        c.radio.clearCivLog();
        QTest::qWait(600);
        check(c.sentTo(0xA2) == 0,
              "two responders: and NOTHING further is addressed to the responder "
              "that answered first - the fallback holds");
    }

    // ---- THE REPLY THAT DISAGREES WITH ITSELF --------------------------------
    //
    // The address arrives twice in one frame — the `from` byte and the payload —
    // and they agreed on every measured run on both lab radios. They are still
    // read separately, because a disagreement means something is rewriting
    // frames between the radio and us, and that is worth knowing before it gets
    // diagnosed as a wrong address. The payload is preferred: it is what the
    // command is defined to answer.
    {
        // DEAF, so the crafted frame below is the FIRST id reply of the session.
        // A radio that answered the broadcast normally first would make the
        // disagreeing frame a SECOND, different address — which is the
        // two-responder case above, not this one.
        CivCase c(0xA2, "IC-7760");
        c.radio.setCivSilent(true);
        c.backend.connectRadio(c.request());
        check(waitFor([&] { return c.backend.isConnected(); }),
              "disagreeing reply: the session comes up");
        // Hand-built so the two fields differ, which no real radio produced.
        c.radio.pushCiv({0xFE, 0xFE, kControllerAddress, 0xA2, cmd::kReadId, 0x00,
                         0x98, kCivEom});
        check(waitFor([&] { return c.sentTo(0x98) > 0; }, 6000),
              "disagreeing reply: the PAYLOAD wins over the frame's from byte");
        check(c.sentTo(0xA2) == 0,
              "disagreeing reply: and the from byte is NOT what gets addressed");
    }

    if (g_failures == 0)
        std::printf("icom_backend_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
