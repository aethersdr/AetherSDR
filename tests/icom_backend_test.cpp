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
#include <QSignalSpy>
#include <QTest>

#include <algorithm>
#include <cmath>
#include <span>
#include <cstdio>

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
    };
    QObject::connect(&backend, &IRadioBackend::sliceChanged, &app,
                     [&](int, const SliceDelta& d) { mergeSlice(d); });
    QObject::connect(&backend, &IRadioBackend::transmitChanged, &app,
                     [&](const TransmitDelta& d) {
        if (d.speechProcEnable) lastTransmitState.speechProcEnable = d.speechProcEnable;
        if (d.speechProcLevel) lastTransmitState.speechProcLevel = d.speechProcLevel;
        if (d.rfPower) lastTransmitState.rfPower = d.rfPower;
        if (d.micLevel) lastTransmitState.micLevel = d.micLevel;
        if (d.sbMonitor) lastTransmitState.sbMonitor = d.sbMonitor;
        if (d.voxEnable) lastTransmitState.voxEnable = d.voxEnable;
        if (d.voxLevel) lastTransmitState.voxLevel = d.voxLevel;
        if (d.atuEnabled) lastTransmitState.atuEnabled = d.atuEnabled;
        if (d.atuStatusRaw) lastTransmitState.atuStatusRaw = d.atuStatusRaw;
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
            backend.submitTxAudio(pcm, 24000);
        QTest::qWait(120);
        check(radio.audioPacketsFromClient() == before,
              "transmit audio is DROPPED while unkeyed");

        // KEYED: the same buffers must arrive.
        radio.clearCivLog();
        backend.setKeying(true);
        for (int i = 0; i < 20; ++i)
            backend.submitTxAudio(pcm, 24000);
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

        // ...and stops again on unkey, rather than draining a backlog into the
        // next transmission.
        QTest::qWait(120);
        const int afterUnkey = radio.audioPacketsFromClient();
        for (int i = 0; i < 20; ++i)
            backend.submitTxAudio(pcm, 24000);
        QTest::qWait(120);
        check(radio.audioPacketsFromClient() == afterUnkey,
              "and stops again on unkey");
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
        const SliceDelta sl = lastSliceState;
        const TransmitDelta tx = lastTransmitState;

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

        check(sl.ritOn.value_or(false), "RIT ON is adopted");
        check(!sl.xitOn.value_or(true), "XIT OFF is adopted");
        check(sl.ritFreq.value_or(0) == -1230,
              "and the RIT OFFSET comes back SIGNED — folding the sign byte into "
              "the magnitude tunes the wrong way");
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
        radio.clearCivLog();
        radio.setCivSilent(true);

        // A command the radio receives and does not answer.
        backend.setPanPreamp(QStringLiteral("0"), 1);
        QTest::qWait(150);
        check(std::any_of(radio.civCommands().begin(), radio.civCommands().end(),
                          [](const CivFrame& f) {
                              return f.cmd == cmd::kFunction && f.hasSub
                                  && f.sub == func::kPreamp;
                          }),
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
        QTest::qWait(200);
        const auto& sent = radio.civCommands();

        check(!res.contains(QStringLiteral("error")), "the scrub runs on a live session");

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
            return f.cmd == cmd::kAttenuator;
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
    }

    backend.disconnectRadio();
    check(!backend.isConnected(), "the backend disconnects cleanly");

    if (g_failures == 0)
        std::printf("icom_backend_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
