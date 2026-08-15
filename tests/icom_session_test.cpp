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
    p.tokenRequestId = 0x1234;

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
    check(radio.loginTokenRequestIds().size() == 1
              && radio.loginTokenRequestIds().front() == 0x1234,
          "the login carries this session's unique token-request ID");
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
    check(waitFor([&] { return radio.deauthOuterSequences().size() >= 2; }),
          "teardown sends the RS-BA1 deauthentication twice");
    const std::vector<std::uint16_t>& deauthSeqs = radio.deauthOuterSequences();
    check(deauthSeqs.size() >= 2 && deauthSeqs[0] != 0 && deauthSeqs[1] != 0
              && deauthSeqs[0] != deauthSeqs[1],
          "each deauthentication has a distinct nonzero tracked outer sequence");

    // A reconnect is a new login identity, not a replay of token request zero.
    // The live IC-7300MK2 accepts the login itself but rejects the first token
    // with 0xffffffff when that request ID is reused.
    {
        const std::size_t authRequestsBeforeReconnect = radio.renewalAuthIds().size();
        IcomSession reconnectSession;
        QString reconnectName;
        QObject::connect(&reconnectSession, &IcomSession::connected, &app,
                         [&](const QString& name) { reconnectName = name; });
        IcomSession::Params reconnectParams = p;
        reconnectParams.tokenRequestId = 0x1235;
        reconnectParams.tokenRenewalMs = 200;
        reconnectParams.tokenAckGraceMs = 10;
        reconnectParams.tokenDeadMs = 1000;
        radio.setReissueInitialTokenOnNextLogin(true);
        check(reconnectSession.start(reconnectParams), "reconnect session starts");
        check(waitFor([&] { return !reconnectName.isEmpty(); }),
              "a reissued reconnect token continues through the full media handshake");
        check(radio.loginTokenRequestIds().size() >= 2
                  && radio.loginTokenRequestIds()[0] != radio.loginTokenRequestIds()[1],
              "consecutive sessions do not reuse their token-request identity");
        const QVariantMap reconnectDiagnostics = reconnectSession.leaseDiagnostics();
        check(reconnectDiagnostics.value(QStringLiteral("lastRenewalResult")).toString()
                  == QStringLiteral("reissued")
                  && reconnectDiagnostics.value(QStringLiteral("reissuedTokens")).toULongLong()
                      == 1,
              "the nonzero initial reply is identified as a reissued token, not a renewal");
        check(reconnectDiagnostics.value(
                  QStringLiteral("initialMaintenancePending")).toBool()
                  && reconnectDiagnostics.value(QStringLiteral("initialMaintenanceMs")).toInt()
                      == 30000,
              "a reconnect schedules one early maintenance renewal before steady cadence");
        const AuthId reissuedAuthId{0xF0, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5};
        check(!radio.streamRequestAuthIds().empty()
                  && radio.streamRequestAuthIds().back() == reissuedAuthId,
              "the stream request carries the radio's rotated reissue token");
        check(waitFor([&] {
                  return radio.renewalAuthIds().size() >= authRequestsBeforeReconnect + 2;
              }, 1000),
              "the reissued session reaches its first maintenance renewal");
        const AuthId currentGrantAuthId{0xD0, 0xD1, 0xD2, 0xD3, 0xD4, 0xD5};
        check(!radio.renewalAuthIds().empty()
                  && radio.renewalAuthIds().back() == currentGrantAuthId,
              "the next renewal carries the later correlated grant token");
        reconnectSession.stop();
    }

    // ---- Lease correlation: stale grants never open media -----------------
    {
        FakeIc705 staleRadio;
        staleRadio.setInjectStaleGrant(true);
        IcomSession staleSession;
        QString staleConnected;
        QObject::connect(&staleSession, &IcomSession::connected, &app,
                         [&](const QString& name) { staleConnected = name; });

        IcomSession::Params staleParams = p;
        staleParams.controlPort = staleRadio.controlPort();
        staleParams.serialPort = staleRadio.serialPort();
        staleParams.audioPort = staleRadio.audioPort();
        staleParams.tokenRenewalMs = 200;
        staleParams.tokenAckGraceMs = 10;
        staleParams.tokenDeadMs = 1000;
        check(staleSession.start(staleParams), "stale-grant session starts");
        check(waitFor([&] { return !staleConnected.isEmpty(); }),
              "a stale grant is ignored and the current-session grant still connects");
        const QVariantMap diagnostics = staleSession.leaseDiagnostics();
        check(diagnostics.value(QStringLiteral("streamGranted")).toBool(),
              "only a correlated grant marks the stream granted");
        check(diagnostics.value(QStringLiteral("ignoredControlPackets")).toULongLong() >= 2,
              "premature and wrong-session grants are counted for diagnostics");
        const AuthId currentGrantAuthId{0xD0, 0xD1, 0xD2, 0xD3, 0xD4, 0xD5};
        check(waitFor([&] { return staleRadio.renewalAuthIds().size() >= 2; }, 1000),
              "the granted session reaches its first maintenance renewal");
        check(!staleRadio.renewalAuthIds().empty()
                  && staleRadio.renewalAuthIds().back() == currentGrantAuthId,
              "the renewal adopts the correlated grant, never the stale grant's token");
        staleRadio.pushAuthFailure(true);
        check(waitFor([&] {
                  return staleSession.leaseDiagnostics()
                             .value(QStringLiteral("ignoredControlPackets")).toULongLong() >= 3;
              }, 100),
              "the stale teardown is visible in diagnostics");
        check(staleSession.isConnected(),
              "a previous session's auth-failure status cannot kill the current lease");

        const qulonglong ignoredBeforeLateCurrent = staleSession.leaseDiagnostics()
                                                         .value(QStringLiteral(
                                                             "ignoredControlPackets"))
                                                         .toULongLong();
        staleRadio.pushAuthFailure(false);
        check(waitFor([&] {
                  return staleSession.leaseDiagnostics()
                             .value(QStringLiteral("ignoredControlPackets"))
                             .toULongLong()
                      > ignoredBeforeLateCurrent;
              }, 100),
              "the late teardown is visible even when stamped with current session IDs");
        check(staleSession.isConnected(),
              "a post-grant auth-failure sentinel cannot kill a working reconnect");
        staleSession.stop();
    }

    // The lifecycle exception above is not a blanket suppression: a current
    // 0x50 failure before a correlated stream grant is a real login failure.
    {
        FakeIc705 failedLoginRadio;
        failedLoginRadio.setAuthFailureOnLogin(true);
        IcomSession failedLoginSession;
        QString failedLoginReason;
        QObject::connect(&failedLoginSession, &IcomSession::disconnected, &app,
                         [&](const QString& reason) { failedLoginReason = reason; });

        IcomSession::Params failedLoginParams = p;
        failedLoginParams.controlPort = failedLoginRadio.controlPort();
        failedLoginParams.serialPort = failedLoginRadio.serialPort();
        failedLoginParams.audioPort = failedLoginRadio.audioPort();
        check(failedLoginSession.start(failedLoginParams),
              "pre-grant auth-failure session starts");
        check(waitFor([&] { return !failedLoginReason.isEmpty(); }),
              "a current-session auth failure before the stream grant is fatal");
        check(failedLoginReason.contains(QStringLiteral("authentication failed")),
              "the pre-grant failure retains its actionable reason");
        failedLoginSession.stop();
    }

    // ---- Lease rejection: fail fast instead of staying falsely green ------
    {
        FakeIc705 rejectingRadio;
        rejectingRadio.setRejectRenewalsAfter(1);  // initial token only
        IcomSession rejectingSession;
        QString rejectedReason;
        QVariantMap rejectedDiagnostics;
        QObject::connect(&rejectingSession, &IcomSession::disconnected, &app,
                         [&](const QString& reason) {
                             rejectedReason = reason;
                             // disconnected() precedes deferred teardown, so
                             // the failure evidence is still available here.
                             rejectedDiagnostics = rejectingSession.leaseDiagnostics();
                         });

        IcomSession::Params rejectingParams = p;
        rejectingParams.controlPort = rejectingRadio.controlPort();
        rejectingParams.serialPort = rejectingRadio.serialPort();
        rejectingParams.audioPort = rejectingRadio.audioPort();
        rejectingParams.tokenRenewalMs = 20;
        rejectingParams.tokenAckGraceMs = 5;
        rejectingParams.tokenDeadMs = 100;
        check(rejectingSession.start(rejectingParams), "renewal-rejection session starts");
        check(waitFor([&] { return rejectingSession.isConnected(); }),
              "initial accepted token opens the session");
        check(waitFor([&] { return !rejectedReason.isEmpty(); }, 1000),
              "a rejected scheduled renewal disconnects before lease expiry");
        check(rejectedReason.contains(QStringLiteral("rejected RS-BA1 session renewal")),
              "disconnect reports the rejected renewal rather than a generic stream stall");
        check(rejectedDiagnostics.value(QStringLiteral("lastRenewalResult")).toString()
                  == QStringLiteral("rejected"),
              "lease diagnostics preserve the protocol-level rejection");
        check(rejectedDiagnostics.value(QStringLiteral("rejectedRenewals")).toULongLong() == 1,
              "the rejected renewal is counted exactly once");
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
        check(rejectingSession.leaseDiagnostics()
                      .value(QStringLiteral("lastRenewalResult")).toString()
                  == QStringLiteral("rejected"),
              "deferred teardown preserves the rejection for civ.session diagnostics");
        check(rejectingRadio.renewalSequences().size() >= 2
                  && rejectingRadio.renewalSequences()[0]
                      != rejectingRadio.renewalSequences()[1],
              "initial and scheduled renewals carry distinct correlation sequences");
        rejectingSession.stop();
    }

    // ---- The previous session's teardown arrives BEFORE this one's grant ---
    //
    // The post-grant lifecycle exception cannot help here: at login time there
    // is no grant and no accepted token, so a 0x50 sentinel is indistinguishable
    // from a real credential failure by lifecycle alone. Only the header IDs
    // separate them, which is the reconnect race this PR exists for.
    {
        FakeIc705 lateTeardownRadio;
        lateTeardownRadio.setStaleAuthFailureBeforeLoginReply(true);
        IcomSession lateTeardownSession;
        QString lateTeardownName;
        QString lateTeardownReason;
        QObject::connect(&lateTeardownSession, &IcomSession::connected, &app,
                         [&](const QString& name) { lateTeardownName = name; });
        QObject::connect(&lateTeardownSession, &IcomSession::disconnected, &app,
                         [&](const QString& reason) { lateTeardownReason = reason; });

        IcomSession::Params lateTeardownParams = p;
        lateTeardownParams.controlPort = lateTeardownRadio.controlPort();
        lateTeardownParams.serialPort = lateTeardownRadio.serialPort();
        lateTeardownParams.audioPort = lateTeardownRadio.audioPort();
        check(lateTeardownSession.start(lateTeardownParams),
              "pre-grant stale-teardown session starts");
        check(waitFor([&] { return !lateTeardownName.isEmpty(); }),
              "a previous session's auth-failure status arriving before this session's "
              "grant does not abort the login");
        check(lateTeardownReason.isEmpty(),
              "the stale pre-grant sentinel raises no disconnect reason");
        check(lateTeardownSession.leaseDiagnostics()
                      .value(QStringLiteral("ignoredControlPackets")).toULongLong() >= 1,
              "the stale pre-grant sentinel is counted rather than silently dropped");
        lateTeardownSession.stop();
    }

    // ---- A reply for a sequence we never sent is not an acknowledgement -----
    {
        FakeIc705 miscorrelatedRadio;
        IcomSession miscorrelatedSession;
        QString miscorrelatedName;
        QObject::connect(&miscorrelatedSession, &IcomSession::connected, &app,
                         [&](const QString& name) { miscorrelatedName = name; });

        IcomSession::Params miscorrelatedParams = p;
        miscorrelatedParams.controlPort = miscorrelatedRadio.controlPort();
        miscorrelatedParams.serialPort = miscorrelatedRadio.serialPort();
        miscorrelatedParams.audioPort = miscorrelatedRadio.audioPort();
        miscorrelatedParams.tokenRenewalMs = 200;
        miscorrelatedParams.tokenAckGraceMs = 10;
        miscorrelatedParams.tokenDeadMs = 5000;
        check(miscorrelatedSession.start(miscorrelatedParams),
              "renewal-correlation session starts");
        check(waitFor([&] { return !miscorrelatedName.isEmpty(); }),
              "the correlation session connects normally");

        const qulonglong ignoredBefore = miscorrelatedSession.leaseDiagnostics()
                                             .value(QStringLiteral("ignoredAuthReplies"))
                                             .toULongLong();
        const qulonglong acceptedBefore = miscorrelatedSession.leaseDiagnostics()
                                              .value(QStringLiteral("acceptedRenewals"))
                                              .toULongLong();
        miscorrelatedRadio.setCorruptNextRenewalSequence(true);
        check(waitFor([&] {
                  return miscorrelatedSession.leaseDiagnostics()
                             .value(QStringLiteral("ignoredAuthReplies")).toULongLong()
                      > ignoredBefore;
              }, 2000),
              "a token reply carrying an unsent inner sequence is rejected, not "
              "acknowledged on packet shape alone");
        check(miscorrelatedSession.leaseDiagnostics()
                      .value(QStringLiteral("acceptedRenewals")).toULongLong()
                  == acceptedBefore,
              "the uncorrelated reply does not advance the accepted-renewal count");
        // The renewal it failed to acknowledge is still outstanding, so the
        // watchdog must resend rather than coast to the dead-session deadline.
        check(waitFor([&] {
                  return miscorrelatedSession.leaseDiagnostics()
                             .value(QStringLiteral("acceptedRenewals")).toULongLong()
                      > acceptedBefore;
              }, 2000),
              "the unacknowledged renewal is resent and the session recovers");
        check(miscorrelatedSession.isConnected(),
              "an uncorrelated reply costs a retry, not the session");
        miscorrelatedSession.stop();
    }

    // ---- The one-time early maintenance renewal actually fires early -------
    //
    // Asserting initialMaintenancePending only proves the flag was set. This
    // drives the timing: the first maintenance renewal must land well inside
    // the steady cadence, and the session must then fall back to that cadence.
    {
        FakeIc705 earlyRadio;
        IcomSession earlySession;
        QString earlyName;
        QObject::connect(&earlySession, &IcomSession::connected, &app,
                         [&](const QString& name) { earlyName = name; });

        IcomSession::Params earlyParams = p;
        earlyParams.controlPort = earlyRadio.controlPort();
        earlyParams.serialPort = earlyRadio.serialPort();
        earlyParams.audioPort = earlyRadio.audioPort();
        earlyParams.tokenRenewalMs = 2000;      // the steady cadence
        earlyParams.initialMaintenanceMs = 60;  // the one-time early window
        earlyParams.tokenAckGraceMs = 10;
        earlyParams.tokenDeadMs = 10000;
        check(earlySession.start(earlyParams), "early-maintenance session starts");
        check(waitFor([&] { return !earlyName.isEmpty(); }),
              "the early-maintenance session connects");
        const std::size_t renewalsAtConnect = earlyRadio.renewalSequences().size();
        check(earlySession.leaseDiagnostics()
                      .value(QStringLiteral("initialMaintenancePending")).toBool(),
              "an initial login schedules the one-time early maintenance renewal");
        // 500 ms is a quarter of the steady cadence: only the early window can
        // produce a renewal inside it.
        check(waitFor([&] {
                  return earlyRadio.renewalSequences().size() > renewalsAtConnect;
              }, 500),
              "the early maintenance renewal is sent well inside the steady cadence");
        check(!earlySession.leaseDiagnostics()
                       .value(QStringLiteral("initialMaintenancePending")).toBool(),
              "the early window is one-time — the next renewal uses the steady cadence");
        check(earlySession.leaseDiagnostics()
                      .value(QStringLiteral("initialMaintenanceMs")).toInt() == 60,
              "civ.session reports the early window actually in force");
        earlySession.stop();
    }

    // ---- An unconfigured session randomizes its own token-request ID -------
    //
    // The checks above all pass the ID explicitly, so they would still pass if
    // production reverted to the fixed 0x0000 that the live IC-7300MK2 rejected
    // on an immediate reconnect. This drives the default path instead.
    {
        FakeIc705 randomRadio;
        IcomSession::Params randomParams = p;
        randomParams.controlPort = randomRadio.controlPort();
        randomParams.serialPort = randomRadio.serialPort();
        randomParams.audioPort = randomRadio.audioPort();
        randomParams.tokenRequestId = 0;  // the production default

        for (int i = 0; i < 3; ++i) {
            IcomSession randomSession;
            QString randomName;
            QObject::connect(&randomSession, &IcomSession::connected, &app,
                             [&](const QString& name) { randomName = name; });
            check(randomSession.start(randomParams), "default-identity session starts");
            check(waitFor([&] { return !randomName.isEmpty(); }),
                  "a session with no configured token-request ID still connects");
            randomSession.stop();
        }

        const std::vector<std::uint16_t>& ids = randomRadio.loginTokenRequestIds();
        check(ids.size() == 3, "three default-identity logins were observed");
        check(std::none_of(ids.begin(), ids.end(),
                           [](std::uint16_t id) { return id == 0; }),
              "an unconfigured login never reuses the rejected 0x0000 request ID");
        // Three independent 16-bit draws collide entirely with probability
        // ~2e-10, so this is a randomness check rather than a flake.
        check(!std::all_of(ids.begin(), ids.end(),
                           [&](std::uint16_t id) { return id == ids.front(); }),
              "consecutive unconfigured logins draw fresh token-request identities");
    }

    if (g_failures == 0)
        std::printf("icom_session_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
