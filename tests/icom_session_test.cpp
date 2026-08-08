// IcomCIV Phases 0-3 end to end — IcomSession against a fake IC-705 on
// localhost.
//
// This is the test that proves the ORDER of the RS-BA1 handshake, which is the
// part of the protocol with no documentation at all. The fake radio below is
// deliberately STRICT: it answers only what a real IC-705 answers, in the order
// it answers it, and it refuses to grant streams unless the client did the two
// separate auths and echoed back the radio identity from the capabilities
// packet. A client that skips a step gets silence, which is exactly what the
// real radio does.
//
// Requires Qt Core + Network. No hardware.

#include "IcomFakeRadio.h"
#include "core/backends/icom/IcomScope.h"
#include "core/backends/icom/IcomSession.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QObject>
#include <QTimer>
#include <QUdpSocket>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <optional>
#include <vector>

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



int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    FakeIc705 radio;
    IcomSession session;

    QString connectedName;
    QString disconnectReason;
    std::vector<CivFrame> frames;
    std::vector<float> audio;
    QObject::connect(&session, &IcomSession::connected, &app,
                     [&](const QString& n) { connectedName = n; });
    QObject::connect(&session, &IcomSession::disconnected, &app,
                     [&](const QString& r) { disconnectReason = r; });
    QObject::connect(&session, &IcomSession::civFrameReady, &app,
                     [&](const CivFrame& f) { frames.push_back(f); });
    QObject::connect(&session, &IcomSession::audioReady, &app,
                     [&](const std::vector<float>& s) {
                         audio.insert(audio.end(), s.begin(), s.end());
                     });

    IcomSession::Params p;
    p.host = QHostAddress::LocalHost;
    p.controlPort = radio.controlPort();
    p.serialPort  = radio.serialPort();
    p.audioPort   = radio.audioPort();
    p.username = QStringLiteral("beer");
    p.password = QStringLiteral("beerbeer");
    p.civAddress = kIc705Addr;

    check(session.start(p), "session starts");

    // ---- Phase 0: the session comes up -------------------------------------
    check(waitFor([&] { return !connectedName.isEmpty(); }),
          "the session completes login, auth, capabilities, stream request and serial open");
    check(connectedName == QStringLiteral("IC-705"),
          "the device name comes from the radio, not from a literal");
    check(disconnectReason.isEmpty(), "and nothing failed on the way");
    check(session.isConnected(), "the session reports itself connected");

    check(radio.sawUsernameObfuscated(),
          "credentials go out through the substitution table, not in clear text");
    check(radio.authCount() >= 2,
          "BOTH auths are sent — the first establishes the session, the second requests "
          "the token that gates the media streams");
    // WAITED FOR, not asserted instantly. The session emits connected() from
    // onSerialReady immediately after writing the serial-open datagram, so the
    // fake radio has not necessarily read it yet when waitFor returns. Asserting
    // directly passed by luck of scheduling and is exactly the kind of check
    // that fails later on a loaded machine.
    check(waitFor([&] { return radio.serialOpened(); }),
          "the CI-V pipe is explicitly opened after its handshake");

    // The ports the client announced must be the ones it actually bound. This
    // is the reason binding is separable from handshaking.
    check(radio.announcedCivPort() != 0 && radio.announcedAudioPort() != 0,
          "the client announced real local ports");
    check(radio.announcedSampleRate() == 48000, "sample rate is announced big-endian");
    check(radio.announcedRxCodec() == 4, "codec 4 (LPCM 1ch 16-bit) is negotiated");

    // ---- Phase 1: CI-V control ---------------------------------------------
    session.sendCiv(cmdReadFrequency(kIc705Addr));
    check(waitFor([&] { return !frames.empty(); }), "the radio answers a CI-V command");
    check(radio.civCommandsSeen() >= 1, "and the radio actually received it");

    bool sawFrequency = false;
    for (const auto& f : frames) {
        if (f.cmd == cmd::kReadFreq && f.data.size() == kFreqBytes) {
            auto hz = decodeFreq(f.data);
            if (hz && *hz == kRadioFrequencyHz)
                sawFrequency = true;
        }
    }
    check(sawFrequency, "the frequency decodes to exactly what the radio reported");

    // Our own commands are echoed by the bus and must NOT surface as radio
    // state — otherwise every command looks confirmed the instant it is sent,
    // including the ones the radio goes on to reject.
    const std::size_t before = frames.size();
    radio.pushCiv({0xFE, 0xFE, kIc705Addr, kControllerAddress, cmd::kReadFreq, kCivEom});
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    check(frames.size() == before, "an echo of our own command is filtered out");

    // ---- Phase 2: a scope sweep --------------------------------------------
    {
        std::vector<std::uint8_t> body;
        body.push_back(0x00);
        body.push_back(encodeBcdByte(1));    // division 1
        body.push_back(encodeBcdByte(1));    // of 1 — the WLAN case
        body.push_back(0x00);                // centre mode
        const auto centre = encodeFreq(14'100'000);
        const auto span   = encodeFreq(100'000);
        body.insert(body.end(), centre.begin(), centre.end());
        body.insert(body.end(), span.begin(), span.end());
        body.push_back(0x00);                // in range
        // A FULL 475-point sweep, deliberately. A short synthetic one would fit
        // inside both an 80-byte frame cap and an 8-bit payload-length field,
        // and would therefore pass against code that cannot carry a real one.
        for (int i = 0; i < kScopePointsIc705; ++i)
            body.push_back(static_cast<std::uint8_t>(i % (kScopeMaxAmplitude + 1)));

        std::vector<std::uint8_t> civ{0xFE, 0xFE, kControllerAddress, kIc705Addr, cmd::kScope,
                                      scope::kWaveData};
        civ.insert(civ.end(), body.begin(), body.end());
        civ.push_back(kCivEom);
        radio.pushCiv(civ);
    }
    check(waitFor([&] {
              for (const auto& f : frames)
                  if (f.cmd == cmd::kScope && f.hasSub && f.sub == scope::kWaveData)
                      return true;
              return false;
          }),
          "a scope sweep arrives over the serial stream");

    {
        ScopeDecoder decoder;
        std::optional<ScopeFrame> sweep;
        for (const auto& f : frames)
            if (auto s = decoder.feed(f))
                sweep = s;
        check(sweep.has_value(), "and the decoder assembles it");
        if (sweep) {
            check(sweep->startHz == 14'000'000 && sweep->endHz == 14'200'000,
                  "centre mode edges are centre +/- span (span is a HALF-width)");
            check(sweep->raw.size() == static_cast<std::size_t>(kScopePointsIc705),
                  "and the sweep is padded to the radio's geometry");
        }
    }

    // ---- Phase 3: audio, both directions -----------------------------------
    {
        std::vector<float> tone(682);
        for (std::size_t i = 0; i < tone.size(); ++i)
            tone[i] = (i % 2) ? 0.5f : -0.5f;
        radio.pushAudio(encodeAudio(AudioCodec::Lpcm1ch16, tone));
    }
    check(waitFor([&] { return audio.size() >= 682; }), "receive audio decodes to samples");
    check(std::fabs(audio[0] + 0.5f) < 1e-3f || std::fabs(audio[0] - 0.5f) < 1e-3f,
          "and the samples are the ones the radio sent");

    // Transmit: one 20 ms frame becomes TWO packets on the wire.
    session.sendAudio(std::vector<float>(960, 0.25f));
    check(waitFor([&] { return radio.audioPacketsFromClient() >= 2; }),
          "a 20 ms transmit frame reaches the radio as the 1364 + 556 pair");

    // ---- Teardown ----------------------------------------------------------
    session.stop();
    check(!session.isConnected(), "stop() tears the session down");

    if (g_failures == 0)
        std::printf("icom_session_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
