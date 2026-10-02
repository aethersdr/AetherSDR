// Socket-free RS-BA1 lease and echo policy for IcomSession. The session is
// never started: a test-built control IcomStream captures every outbound
// packet through its writer seam, and radio replies enter onControlPayload /
// onSerialPayload directly.
#include "core/backends/icom/IcomSession.h"
#include "core/backends/icom/IcomStream.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QSignalSpy>

#include <algorithm>
#include <cstdio>
#include <vector>

using namespace AetherSDR::icom;

namespace AetherSDR::icom {

struct IcomStreamTestAccess {
    static void attach(IcomStream& stream, std::vector<std::vector<std::uint8_t>>& sink)
    {
        stream.m_testWriter = [&sink](std::span<const std::uint8_t> packet) {
            sink.emplace_back(packet.begin(), packet.end());
        };
        stream.m_localSid = 0x11112222;
        stream.m_remoteSid = 0x33334444;
        stream.m_ready = true;
    }
};

// IcomSession already befriends this name; the definition is local to this test.
struct IcomCivBackendTestAccess {
    static void attachControl(IcomSession& session,
                              std::vector<std::vector<std::uint8_t>>& sink)
    {
        session.m_control = new IcomStream(&session);
        IcomStreamTestAccess::attach(*session.m_control, sink);
        session.m_params.tokenRenewalMs = 60000;
        session.m_params.tokenAckGraceMs = 3000;
        session.m_params.tokenDeadMs = 80000;
        session.m_params.initialMaintenanceMs = 30000;
    }
    static void control(IcomSession& s, const std::vector<std::uint8_t>& packet)
    {
        s.onControlPayload(QByteArray(reinterpret_cast<const char*>(packet.data()),
                                      static_cast<qsizetype>(packet.size())));
    }
    static void serial(IcomSession& s, const std::vector<std::uint8_t>& packet)
    {
        s.onSerialPayload(QByteArray(reinterpret_cast<const char*>(packet.data()),
                                     static_cast<qsizetype>(packet.size())));
    }
    static void controlReady(IcomSession& s) { s.onControlReady(); }
    static void tokenTick(IcomSession& s) { s.onTokenRenew(); }
    static void establish(IcomSession& s, const AuthId& id)
    {
        s.m_authId = id;
        s.m_authOk = true;
        s.m_streamsRequested = true;
        s.m_streamGranted = true;
        s.m_connected = true;
        s.m_lastAuthOkMs = QDateTime::currentMSecsSinceEpoch();
    }
    static void awaitGrant(IcomSession& s, const AuthId& id)
    {
        s.m_authId = id;
        s.m_authOk = true;
        s.m_streamsRequested = true;
        // Media streams exist but never bind; beginHandshake() is then a no-op.
        s.m_serial = new IcomStream(&s);
        s.m_audio = new IcomStream(&s);
    }
    static void pending(IcomSession& s, quint16 seq)
    {
        s.m_pendingRenewals.insert(seq);
        s.m_renewUnacked = true;
    }
    static void backdateAuth(IcomSession& s, qint64 ageMs, bool earlyPending)
    {
        s.m_lastAuthOkMs = QDateTime::currentMSecsSinceEpoch() - ageMs;
        s.m_initialMaintenancePending = earlyPending;
        s.m_renewUnacked = false;
    }
    static AuthId authId(const IcomSession& s) { return s.m_authId; }
    static int retries(const IcomSession& s) { return s.m_renewRetries; }
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

using Packets = std::vector<std::vector<std::uint8_t>>;
using Access = IcomCivBackendTestAccess;

constexpr std::uint32_t kLocal = 0x11112222;
constexpr std::uint32_t kRemote = 0x33334444;
const AuthId kLoginId{0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5};
const AuthId kRotatedId{0xF0, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5};

// A radio reply on the current association: header IDs mirrored.
std::vector<std::uint8_t> tokenReply(quint16 innerSeq, const AuthId& id,
                                     std::uint32_t response,
                                     std::uint32_t sent = kRemote,
                                     std::uint32_t rcvd = kLocal)
{
    auto p = buildAuth(sent, rcvd, innerSeq, id, AuthKind::Renew);
    p[0x14] = 0x02;
    p[0x30] = static_cast<std::uint8_t>(response & 0xff);
    p[0x31] = static_cast<std::uint8_t>((response >> 8) & 0xff);
    p[0x32] = static_cast<std::uint8_t>((response >> 16) & 0xff);
    p[0x33] = static_cast<std::uint8_t>((response >> 24) & 0xff);
    return p;
}

std::vector<std::uint8_t> authFailureStatus(std::uint32_t sent, std::uint32_t rcvd)
{
    std::vector<std::uint8_t> p(kLenStatus, 0);
    Header h;
    h.len = static_cast<std::uint32_t>(kLenStatus);
    h.sentId = sent;
    h.rcvdId = rcvd;
    writeHeader(p, h);
    p[0x30] = p[0x31] = p[0x32] = p[0x33] = 0xff;
    return p;
}

std::vector<std::uint8_t> streamGrant(std::uint32_t sent, std::uint32_t rcvd,
                                      const AuthId& id = {})
{
    std::vector<std::uint8_t> p(kLenConnInfo, 0);
    Header h;
    h.len = static_cast<std::uint32_t>(kLenConnInfo);
    h.sentId = sent;
    h.rcvdId = rcvd;
    writeHeader(p, h);
    p[0x60] = 0x01;
    std::copy(id.begin(), id.end(), p.begin() + 0x1a);
    return p;
}

std::vector<std::uint8_t> loginReply()
{
    std::vector<std::uint8_t> p(kLenLoginReply, 0);
    Header h;
    h.len = static_cast<std::uint32_t>(kLenLoginReply);
    h.sentId = kRemote;
    h.rcvdId = kLocal;
    writeHeader(p, h);
    std::copy(kLoginId.begin(), kLoginId.end(), p.begin() + 0x1a);
    return p;
}

bool isAuth(const std::vector<std::uint8_t>& p, AuthKind kind)
{
    return p.size() == kLenToken && p[0x14] == 0x01
        && p[0x15] == static_cast<std::uint8_t>(kind);
}

AuthId authIdOf(const std::vector<std::uint8_t>& p)
{
    AuthId id{};
    std::copy_n(p.begin() + 0x1a, id.size(), id.begin());
    return id;
}

quint16 innerSeqOf(const std::vector<std::uint8_t>& p)
{
    return static_cast<quint16>((p[0x16] << 8) | p[0x17]);
}

QVariant lease(const IcomSession& s, const char* key)
{
    return s.leaseDiagnostics().value(QString::fromLatin1(key));
}

void testOwnEchoIsDropped()
{
    IcomSession session;
    std::vector<CivFrame> frames;
    QObject::connect(&session, &IcomSession::civFrameReady,
                     [&](const CivFrame& f) { frames.push_back(f); });

    const std::vector<std::uint8_t> echo = cmdReadFrequency(0xA4);
    Access::serial(session, buildSerialData(kRemote, kLocal, 1, 1, echo));
    check(frames.empty(), "an echo of our own CI-V command is never radio state");

    const std::vector<std::uint8_t> reply{0xFE, 0xFE, kControllerAddress, 0xA4,
                                          cmd::kReadFreq, 0x00, 0x00, 0x10, 0x14, 0x00,
                                          kCivEom};
    Access::serial(session, buildSerialData(kRemote, kLocal, 2, 2, reply));
    check(frames.size() == 1 && frames.front().from == 0xA4,
          "the radio's own reply on the same pipe is delivered");

    const std::vector<std::uint8_t> broadcastEcho{0xFE, 0xFE, 0x00, kControllerAddress,
                                                  cmd::kReadId, 0x00, kCivEom};
    Access::serial(session, buildSerialData(kRemote, kLocal, 3, 3, broadcastEcho));
    check(frames.size() == 1, "a broadcast-addressed echo is still filtered by its source");
}

void testPartialLoginDeauths()
{
    Packets sent;  // outlives the session: its destructor still writes
    IcomSession session;
    Access::attachControl(session, sent);
    Access::controlReady(session);
    check(sent.size() == 1, "the ready control stream sends exactly the login");

    session.stop();
    std::vector<std::uint16_t> deauthSeqs;
    for (const auto& p : sent) {
        if (isAuth(p, AuthKind::Deauth)) {
            deauthSeqs.push_back(parseHeader(p).seq);
        }
    }
    check(deauthSeqs.size() == 2,
          "stopping a partial login sends exactly two token removals");
    check(deauthSeqs.size() == 2 && deauthSeqs[0] != 0 && deauthSeqs[1] != 0
              && deauthSeqs[0] != deauthSeqs[1],
          "each removal carries a distinct nonzero tracked outer sequence");
    check(!session.isConnected(), "the partial login is closed synchronously");
}

void testLoginThenReissuedToken()
{
    Packets sent;
    IcomSession session;
    Access::attachControl(session, sent);
    Access::control(session, loginReply());

    const auto first = std::find_if(sent.begin(), sent.end(),
                                    [](const auto& p) { return isAuth(p, AuthKind::First); });
    const auto renew = std::find_if(sent.begin(), sent.end(),
                                    [](const auto& p) { return isAuth(p, AuthKind::Renew); });
    check(first != sent.end() && renew != sent.end() && first < renew,
          "login acceptance sends the first auth, then the token request");
    if (renew == sent.end()) {
        return;
    }
    const quint16 seq = innerSeqOf(*renew);

    Access::control(session, tokenReply(seq, kRotatedId, 0xffffffffu));
    check(Access::authId(session) == kRotatedId,
          "a nonzero initial token reply rotates to the radio's reissued token");
    check(lease(session, "lastRenewalResult").toString() == QLatin1String("reissued")
              && lease(session, "reissuedTokens").toULongLong() == 1
              && lease(session, "rejectedRenewals").toULongLong() == 0,
          "the initial nonzero reply is a reissue, not a rejection");
    check(lease(session, "initialMaintenancePending").toBool(),
          "the initial token schedules the one-time early maintenance renewal");
}

void testStaleGrantAndAuthFailureRules()
{
    {
        Packets sent;
        IcomSession session;
        Access::attachControl(session, sent);
        QSignalSpy dropped(&session, &IcomSession::disconnected);
        Access::control(session, authFailureStatus(0x55556666, 0x77778888));
        check(dropped.isEmpty() && lease(session, "ignoredControlPackets").toULongLong() == 1,
              "a previous session's auth-failure status cannot abort this login");
        Access::control(session, authFailureStatus(kRemote, kLocal));
        check(dropped.count() == 1
                  && dropped.first().at(0).toString().contains(
                      QLatin1String("authentication failed")),
              "a current-session auth failure before the grant is fatal");
    }
    {
        Packets sent;
        IcomSession session;
        Access::attachControl(session, sent);
        Access::establish(session, kLoginId);
        QSignalSpy dropped(&session, &IcomSession::disconnected);
        Access::control(session, authFailureStatus(kRemote, kLocal));
        check(dropped.isEmpty() && lease(session, "ignoredControlPackets").toULongLong() == 1,
              "a post-grant 0x50 sentinel stamped with current IDs is ignored");
    }
}

void testGrantCorrelation()
{
    Packets sent;
    IcomSession session;
    Access::attachControl(session, sent);
    Access::control(session, streamGrant(kRemote, kLocal));
    check(!lease(session, "streamGranted").toBool()
              && lease(session, "ignoredControlPackets").toULongLong() == 1,
          "a grant before our stream request and authentication is ignored");

    Access::awaitGrant(session, kLoginId);
    Access::control(session, streamGrant(0x55556666, 0x77778888, kRotatedId));
    check(!lease(session, "streamGranted").toBool()
              && lease(session, "ignoredControlPackets").toULongLong() == 2
              && Access::authId(session) == kLoginId,
          "a previous session's grant opens no media and lends no token");
    Access::control(session, streamGrant(kRemote, kLocal, kRotatedId));
    check(lease(session, "streamGranted").toBool() && Access::authId(session) == kRotatedId,
          "only the correlated grant is adopted, with its token");
}

void testRejectedRenewalFailsFast()
{
    Packets sent;
    IcomSession session;
    Access::attachControl(session, sent);
    Access::establish(session, kLoginId);
    Access::pending(session, 7);
    QSignalSpy dropped(&session, &IcomSession::disconnected);
    Access::control(session, tokenReply(7, kLoginId, 0xffffffffu));
    check(dropped.count() == 1
              && dropped.first().at(0).toString().contains(
                  QLatin1String("rejected RS-BA1 session renewal")),
          "a rejected renewal on an established lease disconnects immediately");
    check(lease(session, "lastRenewalResult").toString() == QLatin1String("rejected")
              && lease(session, "rejectedRenewals").toULongLong() == 1,
          "lease diagnostics record exactly one rejection");
    check(Access::authId(session) == kLoginId,
          "a rejection never adopts the reply's token");
}

void testMiscorrelatedAckResends()
{
    Packets sent;
    IcomSession session;
    Access::attachControl(session, sent);
    Access::establish(session, kLoginId);
    Access::pending(session, 9);
    Access::control(session, tokenReply(10, kLoginId, 0));
    check(lease(session, "ignoredAuthReplies").toULongLong() == 1
              && lease(session, "acceptedRenewals").toULongLong() == 0,
          "a token reply for an unsent inner sequence is not an acknowledgement");

    sent.clear();
    Access::tokenTick(session);
    check(sent.size() == 1 && isAuth(sent.front(), AuthKind::Renew)
              && Access::retries(session) == 1,
          "the still-unacknowledged renewal is resent on the next watchdog tick");
    if (sent.size() == 1) {
        Access::control(session, tokenReply(innerSeqOf(sent.front()), kLoginId, 0));
        check(lease(session, "acceptedRenewals").toULongLong() == 1
                  && session.isConnected(),
              "the correlated retry is accepted and the lease survives");
    }
}

void testEarlyRenewalAndRandomTokenId()
{
    {
        Packets sent;
        IcomSession session;
        Access::attachControl(session, sent);
        Access::establish(session, kLoginId);
        Access::backdateAuth(session, 31000, false);
        Access::tokenTick(session);
        check(sent.empty(), "31 s after the last ack the steady 60 s cadence is quiet");
        Access::backdateAuth(session, 31000, true);
        Access::tokenTick(session);
        check(sent.size() == 1 && isAuth(sent.front(), AuthKind::Renew)
                  && authIdOf(sent.front()) == kLoginId,
              "the one-time early maintenance window renews at 30 s");
    }
    // start() draws the ID before anything binds; invalid lease timing
    // returns there, so no socket is opened.
    std::vector<QString> ids;
    for (int i = 0; i < 3; ++i) {
        IcomSession session;
        IcomSession::Params params;
        params.tokenRequestId = 0;
        params.tokenDeadMs = params.tokenRenewalMs;
        check(!session.start(params), "invalid lease timing refuses before binding");
        ids.push_back(lease(session, "tokenRequestId").toString());
    }
    check(std::none_of(ids.begin(), ids.end(),
                       [](const QString& id) { return id == QLatin1String("0x0000"); }),
          "an unconfigured session never uses token-request ID 0x0000");
    check(!std::all_of(ids.begin(), ids.end(),
                       [&](const QString& id) { return id == ids.front(); }),
          "consecutive unconfigured sessions draw fresh token-request IDs");
}

}  // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    testOwnEchoIsDropped();
    testPartialLoginDeauths();
    testLoginThenReissuedToken();
    testStaleGrantAndAuthFailureRules();
    testGrantCorrelation();
    testRejectedRenewalFailsFast();
    testMiscorrelatedAckResends();
    testEarlyRenewalAndRandomTokenId();
    QCoreApplication::processEvents();
    if (g_failures == 0) {
        std::printf("icom_session_lease_test: all checks passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
