// microHAM TRIO session tests: the HTTP and WebSocket bytes, and the held
// session driven through a scripted transport and a hand-moved clock. No
// socket, no event loop, no peer process.
//
// Recorded from one controller: every state document (checked by SHA-256
// against the frames of a one-hour held session), the command texts, the
// 0x81 / 0x8a first bytes, `ping` / `pong`, the 302 answer, the ten bytes of
// the error page, and the response heads of the page and of the upgrade.
//
// Run:  ./build/microham_trio_session_test

#include "core/MicrohamTrioProtocol.h"
#include "core/MicrohamTrioSession.h"
#include "core/MicrohamTrioWebSocket.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <algorithm>
#include <cstdio>
#include <string>

using namespace AetherSDR;
using namespace AetherSDR::MicrohamTrio;
using AetherSDR::MicrohamTrio::Ws::DecodeError;
using AetherSDR::MicrohamTrio::Ws::FrameDecoder;
using AetherSDR::MicrohamTrio::Ws::Opcode;
using AetherSDR::MicrohamTrio::Ws::ServerFrame;
using AetherSDR::MicrohamTrio::Ws::UpgradeResult;

namespace {

int g_failed = 0;

void report(const char* name, bool ok, const std::string& detail = {})
{
    std::printf("%s %-70s %s\n", ok ? "[ OK ]" : "[FAIL]", name, detail.c_str());
    if (!ok) {
        ++g_failed;
    }
}

std::string num(qint64 value)
{
    return std::to_string(value);
}

const char* const kZeroId = "00000000-0000-0000-0000-000000000000";

// The four request ids of the recorded session, in the order they were sent.
const char* const kIdAnt1   = "2d00ea67-27bd-4e3f-8bc6-8408674d7a59";
const char* const kIdAnt2   = "ac4efe0e-eb98-41b1-83ca-69612b6bd06e";
const char* const kIdRadio1 = "5db71f5f-fed5-4eb9-8d83-46e9a4a32f06";
const char* const kIdRadio2 = "5422cb17-999b-4381-af7c-4e86d872ab9f";

// One state document as the controller sends it: 995 bytes, compact, in this
// key order. Only the two selections and the id vary between recorded frames.
QByteArray stateDoc(const char* radio, const char* antenna, const char* id)
{
    return QByteArray(R"({"s":{"radio":{"v":")") + radio
           + R"(","b":{"radio1":{"n":"RADIO1","a":"R1","l":"FT710","e":true},)"
             R"("radio2":{"n":"RADIO2","a":"R2","l":"HL2","e":true},)"
             R"("radio3":{"n":"RADIO3","a":"R3","l":"SWR","e":true}}},)"
             R"("split":{"v":false,"b":{"split":{"n":"SPLIT","a":"SPL","l":"Split","e":true}}},)"
             R"("pa":{"v":false,"b":{"pa":{"n":"PA","a":"PA","l":"PA","e":true}}},)"
             R"("antenna":{"v":")"
           + antenna
           + R"(","b":{"ant1":{"n":"ANTENNA1","a":"A1","l":"EFHW","e":true},)"
             R"("ant2":{"n":"ANTENNA2","a":"A2","l":"Fan Dipole","e":true},)"
             R"("ant3":{"n":"ANTENNA3","a":"A3","l":"Antenna3","e":true},)"
             R"("ant4":{"n":"ANTENNA4","a":"A4","l":"Antenna4","e":true},)"
             R"("ant5":{"n":"ANTENNA5","a":"A5","l":"Antenna5","e":true},)"
             R"("ant6":{"n":"ANTENNA6","a":"A6","l":"Dummy","e":true}}},)"
             R"("rxant":{"v":"rx1","b":{"rx1":{"n":"RX1","a":"RX1","l":"RX1","e":true},)"
             R"("rx2":{"n":"RX2","a":"RX2","l":"RX2","e":true},)"
             R"("rx3":{"n":"RX3","a":"RX3","l":"RX3","e":true}}},)"
             R"("rxext":{"v":false,"b":{"rxext":{"n":"RXEXT","a":"RxE","l":"RxExt","e":true}}}},)"
             R"("id":")"
           + id + R"("})";
}

// Split engaged, yet `v` where the halves belong: the parser refuses it.
QByteArray splitEngagedDoc()
{
    QByteArray doc = stateDoc("radio2", "ant2", kZeroId);
    doc.replace(R"("split":{"v":false)", R"("split":{"v":true)");
    return doc;
}

QByteArray sha256Hex(const QByteArray& bytes)
{
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
}

// Every state frame of the recorded session after the one sent on connect:
// time in ms since the session opened, the id it carried, the selections, and
// the SHA-256 of the recorded bytes.
struct RecordedState {
    qint64      atMs;
    const char* id;
    const char* radio;
    const char* antenna;
    const char* sha256;
};

const RecordedState kRecorded[] = {
    {5180, kIdAnt1, "radio2", "ant1",
     "b2e1adec402c3d86b5c4fcd26d15dc56ca274cf94595acf6e71fa8e379ef5188"},
    {11184, kIdAnt2, "radio2", "ant2",
     "5040507b55a16de1a348aaff4f4e8e7f205254c12a26b5e0cec08b94561420f8"},
    {17188, kIdRadio1, "radio1", "ant2",
     "4997116885b767e430d1919938e6c7a8787cbaa32be4035fca407b9278fc80bb"},
    {30228, kIdRadio2, "radio2", "ant1",
     "3ec86ada1019dd1bf8b7cc08b32aca4e2530292f640c79e604d4cf919d110c25"},
    {38253, kIdRadio2, "radio2", "ant2",
     "1fcda0add493e2c243b9e50ca58d3d7edf84892bbdcb848ee67edb79885c0ee2"},
    {429540, kIdRadio2, "radio2", "ant3",
     "fa8fd327d180171c3099c50d5b43cb042e7a61c1808baf4ce8f35dbfbdea6eea"},
    {429740, kIdRadio2, "radio2", "ant2",
     "1fcda0add493e2c243b9e50ca58d3d7edf84892bbdcb848ee67edb79885c0ee2"},
    {431030, kIdRadio2, "radio2", "ant3",
     "fa8fd327d180171c3099c50d5b43cb042e7a61c1808baf4ce8f35dbfbdea6eea"},
    {432310, kIdRadio2, "radio2", "ant6",
     "be85364d1a8fa7d9eaed9e11e798812aaa93de0355c04be1e49b917bc7dc1372"},
    {434620, kIdRadio2, "radio2", "ant1",
     "3ec86ada1019dd1bf8b7cc08b32aca4e2530292f640c79e604d4cf919d110c25"},
    {434820, kIdRadio2, "radio2", "ant1",
     "3ec86ada1019dd1bf8b7cc08b32aca4e2530292f640c79e604d4cf919d110c25"},
    {434892, kIdRadio2, "radio3", "ant2",
     "d5a50eb638470ef5fc5461ca51cda3ca99ab0221a85d36ee2687438bc3816b87"},
    {435420, kIdRadio2, "radio3", "ant1",
     "9c3ba9af002d8bce1658298557c470e61d6f5c7058dc2930da69208a01f3f8a2"},
    {435630, kIdRadio2, "radio3", "ant1",
     "9c3ba9af002d8bce1658298557c470e61d6f5c7058dc2930da69208a01f3f8a2"},
    {435772, kIdRadio2, "radio1", "ant1",
     "a03af912b6ce192402ac94e6476ddea42a9dde2a00fb4180c101c8a325c96616"},
    {437290, kIdRadio2, "radio1", "ant3",
     "12b825f7cbe0e3d3f301aeba15b7fdac373a6dc8a791b206af2e87dcfa74d030"},
    {437640, kIdRadio2, "radio1", "ant2",
     "142a9694b6ce2c5b4cd32f26ee7ea4dc9d397fe588f358081076bd5f03d68f2a"},
    {437820, kIdRadio2, "radio1", "ant2",
     "142a9694b6ce2c5b4cd32f26ee7ea4dc9d397fe588f358081076bd5f03d68f2a"},
    {443562, kIdRadio2, "radio2", "ant1",
     "3ec86ada1019dd1bf8b7cc08b32aca4e2530292f640c79e604d4cf919d110c25"},
    {446629, kIdRadio2, "radio2", "ant6",
     "be85364d1a8fa7d9eaed9e11e798812aaa93de0355c04be1e49b917bc7dc1372"},
};

// The state sent on connect was recorded parsed, not raw; this is the SHA-256
// of its compact form (radio2, ant2, all-zero id).
const char* const kOnConnectSha256 =
    "3ff011ea6075a9d7293b836f09cec1b9f1dff6a8fdf530437ff1dff914aa7d86";

// The four commands as the recorded client sent them.
const QByteArray kSentAnt1(
    R"({"a": "set_value", "gid": "antenna", "v": "ant1", "id": "2d00ea67-27bd-4e3f-8bc6-8408674d7a59"})");
const QByteArray kSentAnt2(
    R"({"a": "set_value", "gid": "antenna", "v": "ant2", "id": "ac4efe0e-eb98-41b1-83ca-69612b6bd06e"})");
const QByteArray kSentRadio1(
    R"({"a": "set_value", "gid": "radio", "v": "radio1", "id": "5db71f5f-fed5-4eb9-8d83-46e9a4a32f06"})");
const QByteArray kSentRadio2(
    R"({"a": "set_value", "gid": "radio", "v": "radio2", "id": "5422cb17-999b-4381-af7c-4e86d872ab9f"})");

// The device's answer to a plain GET /, as recorded.
const QByteArray kRecordedRedirect("HTTP/1.1 302 Found\r\nLocation: ui.shtml\r\n\r\n");
// The first ten bytes of the error page the device sends in a frame after a
// raced upgrade; no more of it was recorded.
const QByteArray kErrorPage("<html><bod");

// The device's answer to GET /ui.shtml, as recorded: HTTP/1.0, no length, so
// the page ends when the device closes. The body is not on record; six bytes
// stand in for it.
const QByteArray kRecordedPageHead(
    "HTTP/1.0 200 OK\r\nServer: TRIO web server\r\nSet-Cookie: WID=970120870\r\n"
    "Content-type: text/html\r\nExpires: Fri, 10 Apr 2008 14:00:00 GMT\r\n"
    "Pragma: no-cache\r\n\r\n");
const QByteArray kPageResponse = kRecordedPageHead + "<html>";
// The device's answer to one recorded upgrade, and the key that upgrade sent.
const QByteArray kRecordedUpgradeKey("czulQ1bCfBT3n0ybg0ihSg==");
const QByteArray kRecordedUpgradeHead(
    "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
    "Sec-WebSocket-Accept: UimzJj3P4eYP9xtMC6D1sXTjiK0=\r\n\r\n");
// The first four bytes behind that head: a text frame of 995 bytes.
const QByteArray kRecordedFirstFrameHeader = QByteArray::fromHex("817e03e3");
const QByteArray kPageRequest(
    "GET /ui.shtml HTTP/1.1\r\nHost: 192.168.8.10\r\nAccept-Encoding: identity\r\n"
    "Connection: close\r\n\r\n");

// The recorded head with the accept value of another key.
QByteArray upgradeResponse(const QByteArray& key)
{
    return QByteArray("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                      "Connection: Upgrade\r\nSec-WebSocket-Accept: ")
           + Ws::acceptFor(key) + "\r\n\r\n";
}

// A server frame: unmasked, with the length form RFC 6455 gives its size.
QByteArray serverFrame(quint8 firstByte, const QByteArray& payload)
{
    QByteArray out;
    out.append(static_cast<char>(firstByte));
    if (payload.size() < 126) {
        out.append(static_cast<char>(payload.size()));
    } else {
        out.append(static_cast<char>(126));
        out.append(static_cast<char>(payload.size() >> 8));
        out.append(static_cast<char>(payload.size() & 0xff));
    }
    return out + payload;
}

QByteArray textFrame(const QByteArray& payload)
{
    return serverFrame(kTextFrame, payload);
}

const QByteArray kIdleFrame("\x8a\x00", 2);

struct ClientFrame {
    bool       ok{false};   // FIN set, masked, whole
    quint8     opcode{0};
    QByteArray payload;
};

ClientFrame decodeClient(const QByteArray& bytes)
{
    ClientFrame out;
    if (bytes.size() < 6) {
        return out;
    }
    const quint8 first = static_cast<quint8>(bytes.at(0));
    const quint8 second = static_cast<quint8>(bytes.at(1));
    qsizetype length = second & 0x7f;
    qsizetype offset = 2;
    if (length == 126) {
        length = (static_cast<quint8>(bytes.at(2)) << 8) | static_cast<quint8>(bytes.at(3));
        offset = 4;
    }
    if ((first & 0x80) == 0 || (second & 0x80) == 0 || bytes.size() != offset + 4 + length) {
        return out;
    }
    out.opcode = first & 0x0f;
    for (qsizetype i = 0; i < length; ++i) {
        out.payload.append(static_cast<char>(bytes.at(offset + 4 + i)
                                             ^ bytes.at(offset + (i % 4))));
    }
    out.ok = true;
    return out;
}

bool isClientText(const QByteArray& bytes, const QByteArray& payload)
{
    const ClientFrame frame = decodeClient(bytes);
    return frame.ok && frame.opcode == 0x1 && frame.payload == payload;
}

class FakeClock final : public MicrohamTrioClock {
public:
    qint64 nowMs() const override { return now; }
    qint64 now{0};
};

// A transport that connects nowhere: it records what the session asks for and
// lets the test play the peer.
class FakeTransport final : public MicrohamTrioTransport {
public:
    void open(const QString& host, quint16 port) override
    {
        ++opens;
        openPending = true;
        connected = false;
        lastHost = host;
        lastPort = port;
    }
    void send(const QByteArray& bytes) override { sent.append(bytes); }
    void close() override
    {
        ++closes;
        openPending = false;
        connected = false;
    }

    void peerOpens()
    {
        openPending = false;
        connected = true;
        emit opened();
    }
    void peerSends(const QByteArray& bytes) { emit received(bytes); }
    void peerCloses()
    {
        connected = false;
        emit closedByPeer();
    }
    void breaks() { emit failed(QStringLiteral("scripted failure")); }

    int     opens{0};
    int     closes{0};
    bool    openPending{false};
    bool    connected{false};
    QString lastHost;
    quint16 lastPort{0};
    QVector<QByteArray> sent;
};

struct SeenSnapshot {
    MicrohamTrioSession::SnapshotSource source{MicrohamTrioSession::SnapshotSource::Push};
    QString radio;
    QString antenna;
    QString id;
};

using Status = MicrohamTrioSession::Status;
using Failure = MicrohamTrioSession::Failure;
using Source = MicrohamTrioSession::SnapshotSource;

struct Harness {
    FakeClock     clock;
    FakeTransport transport;
    quint32       randomWord{0x1f2e3d4c};
    int           randomCalls{0};
    MicrohamTrioSession session;

    QVector<SeenSnapshot> snapshots;
    QVector<Status>       statuses;

    bool   autoPong{true};
    int    pings{0};
    int    pongsGiven{0};
    qint64 lastPingAtMs{-1};
    qsizetype sentSeen{0};
    QByteArray lastKey;

    Harness()
        : session(&transport, &clock, entropy())
    {
        QObject::connect(&session, &MicrohamTrioSession::snapshotChanged,
                         [this](const Snapshot& snapshot, Source source) {
                             snapshots.append({source, snapshot.radio.selected,
                                               snapshot.antenna.selected, snapshot.id});
                         });
        QObject::connect(&session, &MicrohamTrioSession::sessionStateChanged,
                         [this](Status status) { statuses.append(status); });
    }

    MicrohamTrioSession::Entropy entropy()
    {
        MicrohamTrioSession::Entropy out;
        out.random32 = [this]() {
            ++randomCalls;
            randomWord = randomWord * 1664525u + 1013904223u;
            return randomWord;
        };
        return out;
    }

    static MicrohamTrioSession::Config config()
    {
        MicrohamTrioSession::Config out;
        out.host = QStringLiteral("192.168.8.10");
        out.port = 80;
        return out;
    }

    // The last of each, or an empty one: a failed check must not end the run.
    SeenSnapshot lastSnapshot() const { return snapshots.isEmpty() ? SeenSnapshot{} : snapshots.last(); }
    QByteArray lastSent() const
    {
        return transport.sent.isEmpty() ? QByteArray() : transport.sent.last();
    }

    // Count the keep-alives sent since the last look and answer them.
    void lookAtSent()
    {
        for (; sentSeen < transport.sent.size(); ++sentSeen) {
            if (isClientText(transport.sent.at(sentSeen), QByteArrayLiteral("ping"))) {
                ++pings;
                lastPingAtMs = clock.now;
                if (autoPong && transport.connected) {
                    ++pongsGiven;
                    transport.peerSends(textFrame(QByteArrayLiteral("pong")));
                }
            }
        }
    }

    // Move the clock to `targetMs`, running tick() exactly when the session's
    // own timer would.
    void advanceTo(qint64 targetMs)
    {
        while (session.isRunning() && session.nextTickAtMs() <= targetMs) {
            const qint64 due = session.nextTickAtMs();
            clock.now = std::max(clock.now, due);
            session.tick();
            lookAtSent();
            if (session.nextTickAtMs() <= due) {
                report("every tick schedules a later one", false);
                break;
            }
        }
        clock.now = std::max(clock.now, targetMs);
    }
    void advance(qint64 ms) { advanceTo(clock.now + ms); }

    // Advance with the device's idle frame once a second.
    void hold(qint64 ms)
    {
        const qint64 end = clock.now + ms;
        while (clock.now < end) {
            advanceTo(std::min(end, clock.now + 1000));
            if (transport.connected && session.status() == Status::Held) {
                transport.peerSends(kIdleFrame);
            }
        }
    }

    // Play the device through one connect: the page with its cookie, then the
    // upgrade, then `firstFrames` behind the 101.
    bool handshake(const QByteArray& firstFrames)
    {
        if (!transport.openPending) {
            return false;
        }
        transport.peerOpens();
        const bool pageAsked = lastSent() == kPageRequest;
        transport.peerSends(kPageResponse);
        transport.peerCloses();
        if (!pageAsked || !transport.openPending) {
            return false;
        }
        transport.peerOpens();
        const QByteArray request = lastSent();
        const QByteArray marker("Sec-WebSocket-Key: ");
        const qsizetype start = request.indexOf(marker);
        if (start < 0) {
            return false;
        }
        const qsizetype end = request.indexOf("\r\n", start);
        lastKey = request.mid(start + marker.size(), end - start - marker.size());
        sentSeen = transport.sent.size();
        transport.peerSends(upgradeResponse(lastKey) + firstFrames);
        return true;
    }

    bool handshakeWithState(const char* radio = "radio2", const char* antenna = "ant2")
    {
        return handshake(textFrame(stateDoc(radio, antenna, kZeroId)));
    }

    // Start and connect: held with a state at clock 1.
    bool connectHeld(const MicrohamTrioSession::Config& cfg = config())
    {
        session.start(cfg);
        advance(1);
        return handshakeWithState() && session.status() == Status::Held;
    }
};

// --- the pure HTTP and WebSocket code -------------------------------------

void testRequests()
{
    report("the page request is the recorded client's, byte for byte",
           Ws::buildCookieRequest(QStringLiteral("192.168.8.10"), 80) == kPageRequest);
    report("another port goes on the Host line",
           Ws::buildCookieRequest(QStringLiteral("trio.local"), 8080)
               == QByteArray("GET /ui.shtml HTTP/1.1\r\nHost: trio.local:8080\r\n"
                             "Accept-Encoding: identity\r\nConnection: close\r\n\r\n"));
    report("a host with a line break builds no page request",
           Ws::buildCookieRequest(QStringLiteral("192.168.8.10\r\nX: y"), 80).isEmpty());
    report("an empty host builds no page request",
           Ws::buildCookieRequest(QString(), 80).isEmpty());

    // RFC 6455 section 1.3: the worked example.
    report("the accept value of the RFC's sample key",
           Ws::acceptFor(QByteArrayLiteral("dGhlIHNhbXBsZSBub25jZQ=="))
               == QByteArrayLiteral("s3pPLMBiTxaQ9kYGzzhZRbK+xOo="));
    report("the key of the RFC's sample nonce",
           Ws::makeKey(QByteArrayLiteral("the sample nonce"))
               == QByteArrayLiteral("dGhlIHNhbXBsZSBub25jZQ=="));

    const QByteArray key = QByteArrayLiteral("dGhlIHNhbXBsZSBub25jZQ==");
    report("the upgrade request is the recorded client's, byte for byte",
           Ws::buildUpgradeRequest(QStringLiteral("192.168.8.10"), 80, key,
                                   QByteArrayLiteral("970120870"))
               == QByteArray("GET /ui.ws HTTP/1.1\r\nHost: 192.168.8.10:80\r\n"
                             "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                             "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                             "Sec-WebSocket-Version: 13\r\nCookie: WID=970120870\r\n\r\n"));
    report("a cookie with a line break builds no upgrade request",
           Ws::buildUpgradeRequest(QStringLiteral("192.168.8.10"), 80, key,
                                   QByteArrayLiteral("1\r\nX: y")).isEmpty());
    report("no cookie builds no upgrade request",
           Ws::buildUpgradeRequest(QStringLiteral("192.168.8.10"), 80, key, QByteArray()).isEmpty());
}

void testCookie()
{
    const Ws::HttpHead head = Ws::parseHead(kPageResponse);
    report("the page head parses", head.complete && head.valid && head.status == 200);
    report("the body starts after the blank line",
           kPageResponse.mid(head.bodyOffset) == QByteArrayLiteral("<html>"));
    const Ws::Cookie cookie = Ws::parseCookie(head);
    report("the WID value is read",
           cookie.error == Ws::CookieError::None && cookie.wid == QByteArrayLiteral("970120870"));

    const Ws::HttpHead attributes = Ws::parseHead(
        "HTTP/1.1 200 OK\r\nset-cookie: other=1\r\nSET-COOKIE: WID=42; path=/\r\n\r\n");
    report("attributes after the value are cut, header names in any case",
           Ws::parseCookie(attributes).wid == QByteArrayLiteral("42"));

    report("a page without the cookie",
           Ws::parseCookie(Ws::parseHead("HTTP/1.0 200 OK\r\nSet-Cookie: SID=7\r\n\r\n")).error
               == Ws::CookieError::NoCookie);
    report("a cookie value that needs quoting is refused",
           Ws::parseCookie(Ws::parseHead("HTTP/1.0 200 OK\r\nSet-Cookie: WID=a b\r\n\r\n")).error
               == Ws::CookieError::UnsafeValue);
    report("an empty cookie value is refused",
           Ws::parseCookie(Ws::parseHead("HTTP/1.0 200 OK\r\nSet-Cookie: WID=;\r\n\r\n")).error
               == Ws::CookieError::UnsafeValue);

    report("a head without its blank line is incomplete",
           !Ws::parseHead("HTTP/1.0 200 OK\r\nSet-Cookie: WID=1\r\n").complete);
    const Ws::HttpHead huge = Ws::parseHead(QByteArray(Ws::kMaxHeadBytes + 1, 'x'));
    report("an endless head is complete and invalid", huge.complete && !huge.valid);
    report("a page that is not HTTP is invalid",
           !Ws::parseHead("<html><body>\r\n\r\n").valid);

    report("a content length is read",
           Ws::contentLength(Ws::parseHead("HTTP/1.0 200 OK\r\nContent-Length: 6\r\n\r\n"))
               == std::optional<qint64>(6));
    report("the recorded page head carries no length", !Ws::contentLength(head).has_value());
}

void testUpgradeResponse()
{
    const QByteArray key = QByteArrayLiteral("dGhlIHNhbXBsZSBub25jZQ==");
    const QByteArray good = upgradeResponse(key);

    Ws::Upgrade upgrade = Ws::checkUpgradeResponse(good + kIdleFrame, key);
    report("a 101 with the right accept value is accepted",
           upgrade.result == UpgradeResult::Accepted && upgrade.status == 101);
    report("the bytes behind the head are handed on", upgrade.remainder == kIdleFrame);

    report("the recorded accept value is the one computed for the recorded key",
           Ws::acceptFor(kRecordedUpgradeKey) == QByteArrayLiteral("UimzJj3P4eYP9xtMC6D1sXTjiK0="));
    report("the helper's head for the recorded key is the recorded head, byte for byte",
           upgradeResponse(kRecordedUpgradeKey) == kRecordedUpgradeHead);
    upgrade = Ws::checkUpgradeResponse(kRecordedUpgradeHead + kRecordedFirstFrameHeader,
                                       kRecordedUpgradeKey);
    report("the recorded 101 is accepted for the recorded key",
           upgrade.result == UpgradeResult::Accepted && upgrade.status == 101
               && upgrade.remainder == kRecordedFirstFrameHeader);
    report("the recorded 101 is refused for another key",
           Ws::checkUpgradeResponse(kRecordedUpgradeHead, key).result
               == UpgradeResult::AcceptMismatch);
    report("the recorded frame header is the one written for a 995-byte state",
           textFrame(stateDoc("radio2", "ant2", kZeroId)).left(4) == kRecordedFirstFrameHeader);

    report("half a head is incomplete",
           Ws::checkUpgradeResponse(good.left(good.size() - 2), key).result
               == UpgradeResult::Incomplete);
    report("header names and the token match in any case",
           Ws::checkUpgradeResponse(
               QByteArray("HTTP/1.0 101 Switching Protocols\r\nupgrade: WebSocket\r\n"
                          "connection: keep-alive, upgrade\r\nsec-websocket-accept: ")
                   + Ws::acceptFor(key) + "\r\n\r\n", key).result
               == UpgradeResult::Accepted);

    report("the recorded 302 is a redirect",
           Ws::checkUpgradeResponse(kRecordedRedirect, key).result == UpgradeResult::Redirected);
    // The status line is the recorded one; nothing else of that answer was kept.
    report("400 is the cookie refused",
           Ws::checkUpgradeResponse("HTTP/1.0 400 Bad Request\r\n\r\n", key).result
               == UpgradeResult::CookieRefused);
    report("another status is a refusal",
           Ws::checkUpgradeResponse("HTTP/1.1 503 Busy\r\n\r\n", key).result
               == UpgradeResult::Refused);
    report("200 is a refusal",
           Ws::checkUpgradeResponse("HTTP/1.1 200 OK\r\n\r\n", key).result
               == UpgradeResult::Refused);
    report("a bare page is malformed",
           Ws::checkUpgradeResponse("<html><body>no</body></html>\r\n\r\n", key).result
               == UpgradeResult::Malformed);
    report("an endless head is malformed",
           Ws::checkUpgradeResponse(QByteArray(Ws::kMaxHeadBytes + 1, 'x'), key).result
               == UpgradeResult::Malformed);

    report("101 without the Upgrade header",
           Ws::checkUpgradeResponse(
               QByteArray("HTTP/1.1 101 OK\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ")
                   + Ws::acceptFor(key) + "\r\n\r\n", key).result
               == UpgradeResult::NotWebSocket);
    report("101 without the Connection token",
           Ws::checkUpgradeResponse(
               QByteArray("HTTP/1.1 101 OK\r\nUpgrade: websocket\r\nConnection: close\r\n"
                          "Sec-WebSocket-Accept: ")
                   + Ws::acceptFor(key) + "\r\n\r\n", key).result
               == UpgradeResult::NotWebSocket);
    report("101 without the accept value",
           Ws::checkUpgradeResponse(
               "HTTP/1.1 101 OK\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n\r\n", key).result
               == UpgradeResult::AcceptMissing);
    report("101 with the accept value of another key",
           Ws::checkUpgradeResponse(upgradeResponse(QByteArrayLiteral("AAAAAAAAAAAAAAAAAAAAAA==")),
                                    key).result
               == UpgradeResult::AcceptMismatch);
}

void testClientFrames()
{
    // 0x81: FIN and text. 0x84: masked, four bytes. Then the key, then
    // "ping" XOR the key.
    const QByteArray ping = Ws::encodeClientFrame(Opcode::Text, QByteArrayLiteral("ping"),
                                                  0x01020304u);
    report("a masked ping, byte for byte",
           ping == QByteArray("\x81\x84\x01\x02\x03\x04\x71\x6b\x6d\x63", 10));
    report("it unmasks to ping", isClientText(ping, QByteArrayLiteral("ping")));

    const QByteArray command = Ws::encodeClientFrame(Opcode::Text, kSentAnt1, 0xa1b2c3d4u);
    report("a 95-byte command uses the short length form",
           kSentAnt1.size() == 95 && static_cast<quint8>(command.at(1)) == (0x80 | 95)
               && isClientText(command, kSentAnt1));

    const QByteArray longPayload(300, 'x');
    const QByteArray longFrame = Ws::encodeClientFrame(Opcode::Text, longPayload, 0x0badf00du);
    report("300 bytes use the 16-bit length form",
           static_cast<quint8>(longFrame.at(1)) == (0x80 | 126)
               && static_cast<quint8>(longFrame.at(2)) == 0x01
               && static_cast<quint8>(longFrame.at(3)) == 0x2c
               && isClientText(longFrame, longPayload));

    const QByteArray big(70000, 'y');
    const QByteArray bigFrame = Ws::encodeClientFrame(Opcode::Binary, big, 0u);
    report("70000 bytes use the 64-bit length form",
           static_cast<quint8>(bigFrame.at(1)) == (0x80 | 127)
               && bigFrame.mid(2, 8) == QByteArray("\x00\x00\x00\x00\x00\x01\x11\x70", 8)
               && bigFrame.size() == 2 + 8 + 4 + 70000);

    const QByteArray pong = Ws::encodeClientFrame(Opcode::Pong, QByteArray(), 0x11223344u);
    report("an empty masked pong",
           pong == QByteArray("\x8a\x80\x11\x22\x33\x44", 6));
    report("a close frame carries its opcode",
           static_cast<quint8>(Ws::encodeClientFrame(Opcode::Close, QByteArray(), 1u).at(0))
               == 0x88);
}

void testDecoder()
{
    const QByteArray doc = stateDoc("radio2", "ant1", kIdAnt1);
    const QByteArray wire = textFrame(doc);
    report("a state frame is 995 bytes behind a four-byte header",
           doc.size() == 995 && wire.left(4) == QByteArray("\x81\x7e\x03\xe3", 4));

    FrameDecoder decoder;
    QVector<ServerFrame> frames = decoder.feed(wire);
    report("a whole text frame",
           frames.size() == 1 && frames.at(0).firstByte == kTextFrame
               && frames.at(0).payload == doc && decoder.error() == DecodeError::None);

    FrameDecoder slow;
    int count = 0;
    QByteArray payload;
    for (qsizetype i = 0; i < wire.size(); ++i) {
        const QVector<ServerFrame> part = slow.feed(wire.mid(i, 1));
        count += part.size();
        if (!part.isEmpty()) {
            payload = part.at(0).payload;
        }
        if (i + 1 < wire.size() && !part.isEmpty()) {
            count += 100;
        }
    }
    report("the same frame one byte at a time", count == 1 && payload == doc);

    FrameDecoder several;
    frames = several.feed(kIdleFrame + textFrame(QByteArrayLiteral("pong")) + kIdleFrame
                          + wire.left(10));
    report("three frames and the start of a fourth in one read",
           frames.size() == 3 && frames.at(0).firstByte == kPongFrame
               && frames.at(0).payload.isEmpty()
               && frames.at(1).payload == QByteArrayLiteral("pong")
               && frames.at(2).firstByte == kPongFrame);
    frames = several.feed(wire.mid(10));
    report("the fourth completes on the next read",
           frames.size() == 1 && frames.at(0).payload == doc);

    FrameDecoder control;
    frames = control.feed(serverFrame(0x89, QByteArrayLiteral("hi"))
                          + serverFrame(0x88, QByteArray("\x03\xe8", 2)));
    report("a ping and a close are frames",
           frames.size() == 2 && frames.at(0).opcode() == Opcode::Ping
               && frames.at(0).payload == QByteArrayLiteral("hi")
               && frames.at(1).opcode() == Opcode::Close);

    FrameDecoder atCap;
    frames = atCap.feed(serverFrame(kTextFrame, QByteArray(kMaxFrameBytes, 'a')));
    report("a frame exactly at the cap is taken",
           frames.size() == 1 && atCap.error() == DecodeError::None);

    const auto errorOf = [](const QByteArray& bytes) {
        FrameDecoder decoder;
        decoder.feed(bytes);
        return decoder.error();
    };
    report("one byte over the cap, 16-bit length",
           errorOf(QByteArray("\x81\x7e\x40\x01", 4)) == DecodeError::TooLarge);
    report("a 64-bit length over the cap",
           errorOf(QByteArray("\x81\x7f\x00\x00\x00\x01\x00\x00\x00\x00", 10))
               == DecodeError::TooLarge);
    report("a masked server frame",
           errorOf(QByteArray("\x81\x84\x01\x02\x03\x04\x71\x6b\x6d\x63", 10))
               == DecodeError::Masked);
    report("a frame without FIN",
           errorOf(QByteArray("\x01\x02hi", 4)) == DecodeError::Fragmented);
    report("a continuation frame",
           errorOf(QByteArray("\x80\x02hi", 4)) == DecodeError::Fragmented);
    report("reserved bits",
           errorOf(QByteArray("\xc1\x02hi", 4)) == DecodeError::ReservedBits);
    report("an unknown opcode",
           errorOf(QByteArray("\x83\x02hi", 4)) == DecodeError::UnknownOpcode);
    report("a control frame over 125 bytes",
           errorOf(QByteArray("\x89\x7e\x00\x7e", 4)) == DecodeError::ControlTooLong);

    FrameDecoder broken;
    frames = broken.feed(kIdleFrame + QByteArray("\x01\x02hi", 4) + kIdleFrame);
    report("frames before an error are still returned",
           frames.size() == 1 && broken.error() == DecodeError::Fragmented);
    report("after an error nothing is decoded", broken.feed(kIdleFrame).isEmpty());
    broken.reset();
    report("until reset",
           broken.feed(kIdleFrame).size() == 1 && broken.error() == DecodeError::None);
}

// --- the session -----------------------------------------------------------

void testFixtures()
{
    report("the on-connect document matches the recorded state",
           sha256Hex(stateDoc("radio2", "ant2", kZeroId)) == kOnConnectSha256);
    bool all = true;
    for (const RecordedState& row : kRecorded) {
        all = all && sha256Hex(stateDoc(row.radio, row.antenna, row.id)) == row.sha256;
    }
    report("all twenty recorded state frames are reproduced bit for bit", all);
    report("a split document without its halves is refused by the parser",
           parse(splitEngagedDoc()).error == ParseError::MissingKey);
}

void testNothingBeforeStart()
{
    Harness h;
    h.advance(60000);
    report("no connection before start()", h.transport.opens == 0 && h.transport.sent.isEmpty());
    report("idle before start()", h.session.status() == Status::Idle);
}

void testConnect()
{
    Harness h;
    h.session.start(Harness::config());
    report("start() alone opens nothing", h.transport.opens == 0);
    h.advance(1);
    report("the first tick connects",
           h.transport.opens == 1 && h.transport.openPending
               && h.transport.lastHost == QStringLiteral("192.168.8.10")
               && h.transport.lastPort == 80 && h.session.status() == Status::Connecting);
    report("nothing is sent before the connection is up", h.transport.sent.isEmpty());

    h.transport.peerOpens();
    report("the page is asked for first",
           h.transport.sent.size() == 1 && h.transport.sent.at(0) == kPageRequest);
    // The head arrives in two reads.
    h.transport.peerSends(kPageResponse.left(30));
    report("half a page head opens nothing", h.transport.opens == 1);
    h.transport.peerSends(kPageResponse.mid(30));
    report("without a length the page runs until the peer closes", h.transport.opens == 1);
    h.transport.peerCloses();
    report("then the second connection opens", h.transport.opens == 2 && h.transport.openPending);

    h.transport.peerOpens();
    QByteArray nonce;
    quint32 word = 0x1f2e3d4c;
    for (int i = 0; i < 4; ++i) {
        word = word * 1664525u + 1013904223u;
        nonce.append(static_cast<char>(word >> 24));
        nonce.append(static_cast<char>(word >> 16));
        nonce.append(static_cast<char>(word >> 8));
        nonce.append(static_cast<char>(word));
    }
    const QByteArray key = nonce.toBase64();
    report("the upgrade carries the cookie and a 16-byte key",
           h.transport.sent.size() == 2
               && h.transport.sent.at(1)
                      == QByteArray("GET /ui.ws HTTP/1.1\r\nHost: 192.168.8.10:80\r\n"
                                    "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                                    "Sec-WebSocket-Key: ")
                             + key
                             + "\r\nSec-WebSocket-Version: 13\r\nCookie: WID=970120870\r\n\r\n");

    const QByteArray response = upgradeResponse(key);
    h.transport.peerSends(response.left(20));
    report("half a 101 is not held yet", h.session.status() == Status::Connecting);
    const QByteArray frame = textFrame(stateDoc("radio2", "ant2", kZeroId));
    h.transport.peerSends(response.mid(20) + frame.left(500));
    report("the 101 makes the session held",
           h.session.status() == Status::Held && h.session.state().link == Link::Held);
    report("held without a state: no snapshot yet, not current",
           h.snapshots.isEmpty() && !h.session.snapshotCurrent());
    h.transport.peerSends(frame.mid(500));
    report("the state behind the 101 is the on-connect snapshot",
           h.snapshots.size() == 1 && h.snapshots.at(0).source == Source::OnConnect
               && h.snapshots.at(0).radio == QStringLiteral("radio2")
               && h.snapshots.at(0).antenna == QStringLiteral("ant2")
               && h.snapshots.at(0).id == QLatin1String(kZeroId));
    report("held with a state: the snapshot is current",
           h.session.snapshotCurrent() && h.session.snapshot().has_value());
    report("status went connecting, then held",
           h.statuses == QVector<Status>({Status::Connecting, Status::Held}));

    Harness withLength;
    withLength.session.start(Harness::config());
    withLength.advance(1);
    withLength.transport.peerOpens();
    withLength.transport.peerSends(
        "HTTP/1.1 200 OK\r\nContent-Length: 6\r\nSet-Cookie: WID=77\r\n\r\n<ht");
    report("a page with a length is read to that length", withLength.transport.opens == 1);
    withLength.transport.peerSends("ml>");
    report("and then the second connection opens without waiting for a close",
           withLength.transport.opens == 2 && withLength.transport.closes >= 1);
    withLength.transport.peerOpens();
    report("with that page's cookie",
           withLength.lastSent().contains("\r\nCookie: WID=77\r\n"));
}

// The recorded hour, read only: twenty state documents as one held session
// received them (four were replies to another client's commands, sixteen
// front-panel and band-data pushes), all taken as pushes here because this
// session sends no command. Idle frames: one a second from 0.497 s as
// recorded, with the two recorded gaps of 2.7 s.
void testRecordedHour()
{
    Harness h;
    report("connected", h.connectHeld());
    const qint64 t0 = h.clock.now;

    const qint64 endMs = 3525437;
    qint64 nextIdle = 497;
    int recorded = 0;
    int idleFrames = 0;
    bool heldThroughout = true;
    const int recordedCount = static_cast<int>(sizeof(kRecorded) / sizeof(kRecorded[0]));

    while (true) {
        qint64 next = nextIdle;
        if (recorded < recordedCount) {
            next = std::min(next, kRecorded[recorded].atMs);
        }
        if (next > endMs) {
            break;
        }
        h.advanceTo(t0 + next);
        heldThroughout = heldThroughout && h.session.status() == Status::Held;

        if (recorded < recordedCount && kRecorded[recorded].atMs == next) {
            const RecordedState& row = kRecorded[recorded++];
            h.transport.peerSends(textFrame(stateDoc(row.radio, row.antenna, row.id)));
            continue;
        }
        // The two recorded gaps: 21.496 to 24.223 s and 35.496 to 38.254 s.
        if (nextIdle == 21497) {
            nextIdle = 24223;
        } else if (nextIdle == 24223) {
            nextIdle = 24497;
        } else if (nextIdle == 35497) {
            nextIdle = 38254;
        } else if (nextIdle == 38254) {
            nextIdle = 38497;
        } else {
            nextIdle += 1000;
        }
        h.transport.peerSends(kIdleFrame);
        ++idleFrames;
    }
    h.advanceTo(t0 + endMs);

    report("the session is held for the whole recorded hour",
           heldThroughout && h.session.status() == Status::Held
               && h.statuses == QVector<Status>({Status::Connecting, Status::Held}),
           num(h.statuses.size()));
    report("one connect, never a second", h.transport.opens == 2);
    report("about one idle frame a second was fed", idleFrames > 3500, num(idleFrames));

    report("twenty-one snapshots", h.snapshots.size() == 21, num(h.snapshots.size()));
    bool sources = h.snapshots.size() == 21 && h.snapshots.at(0).source == Source::OnConnect;
    bool values = sources;
    for (int i = 0; sources && i < recordedCount; ++i) {
        const SeenSnapshot& seen = h.snapshots.at(i + 1);
        sources = sources && seen.source == Source::Push;
        values = values && seen.radio == QLatin1String(kRecorded[i].radio)
                 && seen.antenna == QLatin1String(kRecorded[i].antenna)
                 && seen.id == QLatin1String(kRecorded[i].id);
    }
    report("the on-connect state, then twenty pushes", sources);
    report("each snapshot shows the recorded selections", values);

    report("nothing but the handshake and keep-alives was sent", [&h]() {
        for (qsizetype i = 2; i < h.transport.sent.size(); ++i) {
            if (!isClientText(h.transport.sent.at(i), QByteArrayLiteral("ping"))) {
                return false;
            }
        }
        return true;
    }());
    // First keep-alive one tick after the state, then one every 5 s.
    const int expectedPings = 1 + static_cast<int>((endMs - 1) / 5000);
    report("a keep-alive every five seconds for the hour",
           h.pings == expectedPings, num(h.pings) + " of " + num(expectedPings));
    report("every keep-alive was answered", h.pongsGiven == h.pings);
    report("no connection was dropped", h.transport.closes == 2, num(h.transport.closes));
}

void testKeepAliveCadence()
{
    Harness h;
    h.connectHeld();
    report("no keep-alive before the first tick on the held session", h.pings == 0);
    h.advance(1);
    report("the first keep-alive goes out at once",
           h.pings == 1 && isClientText(h.lastSent(), QByteArrayLiteral("ping")));
    const qint64 first = h.lastPingAtMs;
    h.hold(4999);
    report("none in the next 4999 ms", h.pings == 1);
    h.hold(1);
    report("the second exactly 5000 ms after the first",
           h.pings == 2 && h.lastPingAtMs == first + 5000, num(h.lastPingAtMs - first));
    h.hold(10000);
    report("and so on", h.pings == 4 && h.lastPingAtMs == first + 15000);
    report("an answered keep-alive leaves nothing owing",
           !h.session.state().unansweredKeepAliveAgeMs.has_value());
    report("still held", h.session.status() == Status::Held && h.transport.opens == 2);
}

void testDeadSession()
{
    // Silence: no frame at all after the last idle frame.
    Harness silent;
    silent.connectHeld();
    silent.hold(20000);
    // Off the one-second grid the ticks have fallen into.
    silent.advance(300);
    silent.transport.peerSends(kIdleFrame);
    const qint64 lastFrame = silent.clock.now;
    silent.autoPong = false;
    const qint64 bound = Harness::config().policy.sessionDeadAfterMs;
    silent.advanceTo(lastFrame + bound);
    report("silent for exactly the bound: still held",
           silent.session.status() == Status::Held && silent.transport.opens == 2);
    silent.advance(1);
    report("one millisecond more: the session is dropped and reopened",
           silent.session.status() == Status::Connecting && silent.transport.opens == 3
               && silent.transport.openPending);
    report("a dead session's state is no longer current, and is named silent",
           !silent.session.snapshotCurrent()
               && silent.session.lastFailure() == Failure::SessionSilent);
    report("the reconnect holds again", silent.handshakeWithState()
               && silent.session.status() == Status::Held && silent.session.snapshotCurrent());

    // Idle frames keep coming, the keep-alive goes unanswered.
    Harness deaf;
    deaf.connectHeld();
    deaf.hold(3000);
    deaf.autoPong = false;
    const int answered = deaf.pings;
    for (int i = 0; i < 6000 && deaf.pings == answered; ++i) {
        deaf.hold(1);
    }
    report("the next keep-alive goes unanswered", deaf.pings == answered + 1);
    const qint64 unanswered = deaf.lastPingAtMs;
    while (deaf.clock.now < unanswered + bound) {
        deaf.hold(std::min<qint64>(500, unanswered + bound - deaf.clock.now));
    }
    report("a keep-alive unanswered for exactly the bound: still held",
           deaf.session.status() == Status::Held && deaf.transport.opens == 2);
    deaf.advance(1);
    report("one millisecond more: reopened, idle frames or not",
           deaf.session.status() == Status::Connecting && deaf.transport.opens == 3);
}

// The device serves one session. A close of the held one is another client
// taking it (most often the Trio's own web page): never taken back by itself.
void testEvictionIsNotRetaken()
{
    Harness h;
    h.connectHeld();
    h.hold(3000);
    h.transport.peerCloses();
    report("a close of the held session is an eviction",
           h.session.status() == Status::Evicted && h.session.state().link == Link::Evicted);
    report("the last state is kept, marked not current",
           h.session.snapshot().has_value() && !h.session.snapshotCurrent());
    const int opens = h.transport.opens;
    h.advance(600000);
    report("ten minutes later: no connect was made", h.transport.opens == opens,
           num(h.transport.opens - opens));
    report("and the status is still evicted", h.session.status() == Status::Evicted);

    Harness closeFrame;
    closeFrame.connectHeld();
    closeFrame.transport.peerSends(serverFrame(0x88, QByteArray("\x03\xe8", 2)));
    const int framed = closeFrame.transport.opens;
    closeFrame.advance(60000);
    report("a close frame is an eviction too, and not retaken",
           closeFrame.session.status() == Status::Evicted && closeFrame.transport.opens == framed);

    // start() again is the operator's Connect.
    h.session.start(Harness::config());
    h.advance(1);
    report("start() after an eviction connects again",
           h.transport.opens == opens + 1 && h.session.status() == Status::Connecting);
}

void testRelease()
{
    Harness h;
    h.connectHeld();
    h.hold(2000);
    const int closes = h.transport.closes;
    h.session.setReleasedForBrowser(true);
    h.advance(1);
    report("a release closes the held session",
           h.session.status() == Status::Released && h.transport.closes > closes
               && !h.transport.connected);
    const int opens = h.transport.opens;
    h.advance(600000);
    report("and nothing reconnects while released", h.transport.opens == opens);
    h.session.setReleasedForBrowser(false);
    h.advance(1);
    report("taking it back connects", h.transport.opens == opens + 1);
}

void testErrorPage()
{
    Harness h;
    h.session.start(Harness::config());
    h.advance(1);
    const qint64 firstAt = h.clock.now;
    report("the error page arrives in the first frame", h.handshake(textFrame(kErrorPage)));
    report("that is a transient: still connecting, not failed",
           h.session.status() == Status::Connecting
               && h.session.lastFailure() == Failure::ErrorPage
               && h.session.state().consecutiveFailures == 0 && !h.transport.openPending);
    h.advanceTo(firstAt + 1499);
    report("no retry before the pause is over", !h.transport.openPending);
    h.advance(1);
    report("the retry goes out after 1500 ms", h.transport.openPending);
    report("the second attempt reads the state",
           h.handshakeWithState() && h.session.status() == Status::Held
               && h.session.lastFailure() == Failure::None && h.snapshots.size() == 1);

    // A state in between starts the count again: after a dropped connection
    // and its backoff, two more are still transient.
    h.hold(3000);
    h.transport.breaks();
    h.advance(Harness::config().policy.failureBackoffMs);
    h.handshake(textFrame(kErrorPage));
    h.advance(1500);
    h.handshake(textFrame(kErrorPage));
    report("two error pages after a good session are transients again",
           h.session.status() == Status::Connecting
               && h.session.state().consecutiveFailures == 1);

    // Three in a row are no longer transient.
    Harness stuck;
    stuck.session.start(Harness::config());
    stuck.advance(1);
    stuck.handshake(textFrame(kErrorPage));
    stuck.advance(1500);
    stuck.handshake(textFrame(kErrorPage));
    report("the second error page is still a transient",
           stuck.session.status() == Status::Connecting);
    stuck.advance(1500);
    const qint64 thirdAt = stuck.clock.now;
    stuck.handshake(textFrame(kErrorPage));
    report("the third counts as a failure",
           stuck.session.status() == Status::Failed
               && stuck.session.lastFailure() == Failure::ErrorPage
               && stuck.session.state().consecutiveFailures == 1);
    stuck.advanceTo(thirdAt + 4999);
    report("a failure backs off: nothing for 4999 ms", !stuck.transport.openPending);
    stuck.advance(1);
    report("the next attempt at 5000 ms", stuck.transport.openPending);
    report("a state clears the failure count",
           stuck.handshakeWithState() && stuck.session.state().consecutiveFailures == 0);

    // The error page later, on a session that already had a state.
    Harness late;
    late.connectHeld();
    late.transport.peerSends(textFrame(kErrorPage));
    report("unreadable text after a state is a failure, not a transient",
           late.session.status() == Status::Failed
               && late.session.lastFailure() == Failure::UnreadableState);

    // A 101, idle frames, and never a state.
    Harness mute;
    mute.session.start(Harness::config());
    mute.advance(1);
    mute.handshake(QByteArray());
    const qint64 heldAt = mute.clock.now;
    while (mute.clock.now < heldAt + 5999) {
        mute.hold(std::min<qint64>(1000, heldAt + 5999 - mute.clock.now));
    }
    report("held without a state for 5999 ms: still waiting",
           mute.session.status() == Status::Held && mute.pings == 0);
    mute.advance(1);
    report("no state in 6000 ms is the same transient",
           mute.session.status() == Status::Connecting
               && mute.session.lastFailure() == Failure::NoStateDocument);
}

void testCloseAndPing()
{
    Harness h;
    h.connectHeld();
    h.advance(1);
    const qsizetype sent = h.transport.sent.size();
    h.transport.peerSends(serverFrame(0x89, QByteArrayLiteral("hi")));
    ClientFrame answer = decodeClient(h.lastSent());
    report("a server ping is answered with a masked pong carrying its payload",
           h.transport.sent.size() == sent + 1 && answer.ok && answer.opcode == 0xa
               && answer.payload == QByteArrayLiteral("hi"));
    report("and the session stays held", h.session.status() == Status::Held);

    // A ping counts as a sign of life. No keep-alive of ours falls in this
    // window, so only the device's frames hold the session.
    Harness quiet;
    MicrohamTrioSession::Config rare = Harness::config();
    rare.policy.keepAliveIntervalMs = 3600000;
    quiet.connectHeld(rare);
    quiet.advance(1);
    const qint64 lastPong = quiet.clock.now;
    quiet.advance(4000);
    quiet.transport.peerSends(serverFrame(0x89, QByteArray()));
    quiet.advance(4000);
    report("a ping resets the silence bound",
           quiet.pings == 1 && quiet.clock.now == lastPong + 8000
               && quiet.session.status() == Status::Held);
    quiet.advance(1001);
    report("and without another frame the bound runs out",
           quiet.session.status() == Status::Connecting);

    Harness closing;
    closing.connectHeld();
    const int closes = closing.transport.closes;
    closing.transport.peerSends(serverFrame(0x88, QByteArray("\x03\xe8", 2))
                                + textFrame(stateDoc("radio1", "ant1", kZeroId)));
    answer = decodeClient(closing.lastSent());
    report("a close frame is answered with a close frame",
           answer.ok && answer.opcode == 0x8 && answer.payload == QByteArray("\x03\xe8", 2));
    report("and the session is evicted, the connection dropped",
           closing.session.status() == Status::Evicted
               && closing.transport.closes == closes + 1);
    report("a frame behind the close is not read", closing.snapshots.size() == 1);
}

void testConnectFailures()
{
    {
        Harness h;
        h.session.start(Harness::config());
        h.advance(1);
        const qint64 at = h.clock.now;
        h.transport.breaks();
        report("a refused connection is a failure",
               h.session.status() == Status::Failed
                   && h.session.lastFailure() == Failure::ConnectFailed
                   && h.session.state().consecutiveFailures == 1 && h.transport.opens == 1);
        h.advanceTo(at + 4999);
        report("first failure: nothing for 4999 ms", !h.transport.openPending);
        h.advance(1);
        report("retry at 5000 ms", h.transport.openPending && h.transport.opens == 2);
        const qint64 second = h.clock.now;
        h.transport.breaks();
        h.advanceTo(second + 9999);
        report("second failure: the pause doubles", !h.transport.openPending
                   && h.session.state().consecutiveFailures == 2);
        h.advance(1);
        report("retry at 10000 ms", h.transport.openPending);
    }
    {
        Harness h;
        h.session.start(Harness::config());
        h.advance(1);
        const qint64 at = h.clock.now;
        h.transport.peerOpens();
        h.advanceTo(at + 5999);
        report("a connect that hangs: still trying at 5999 ms",
               h.session.status() == Status::Connecting);
        h.advance(1);
        report("timed out at 6000 ms",
               h.session.status() == Status::Failed
                   && h.session.lastFailure() == Failure::ConnectTimeout
                   && !h.transport.connected);
    }
    {
        Harness h;
        h.session.start(Harness::config());
        h.advance(1);
        h.transport.peerOpens();
        h.transport.peerCloses();
        report("the page connection closed before its head",
               h.session.lastFailure() == Failure::ClosedEarly
                   && h.session.status() == Status::Failed);
    }
    {
        Harness h;
        h.session.start(Harness::config());
        h.advance(1);
        h.transport.peerOpens();
        h.transport.peerSends("HTTP/1.0 200 OK\r\nContent-Type: text/html\r\n\r\n<html>");
        report("a page without the cookie",
               h.session.lastFailure() == Failure::CookieMissing && h.transport.opens == 1);
    }
    const auto upgradeAnswer = [](const QByteArray& answer) {
        Harness h;
        h.session.start(Harness::config());
        h.advance(1);
        h.transport.peerOpens();
        h.transport.peerSends(kPageResponse);
        h.transport.peerCloses();
        h.transport.peerOpens();
        if (answer.isEmpty()) {
            h.transport.peerCloses();
        } else {
            h.transport.peerSends(answer);
        }
        return h.session.status() == Status::Failed ? h.session.lastFailure() : Failure::None;
    };
    report("the upgrade answered with the recorded 302",
           upgradeAnswer(kRecordedRedirect) == Failure::UpgradeRedirected);
    report("the upgrade answered 400",
           upgradeAnswer("HTTP/1.0 400 Bad Request\r\n\r\n") == Failure::CookieRefused);
    report("the upgrade answered 503",
           upgradeAnswer("HTTP/1.1 503 Busy\r\n\r\n") == Failure::UpgradeRefused);
    report("a 101 for another key",
           upgradeAnswer(upgradeResponse(QByteArrayLiteral("AAAAAAAAAAAAAAAAAAAAAA==")))
               == Failure::HandshakeInvalid);
    report("a page instead of an answer",
           upgradeAnswer("<html><body></body></html>\r\n\r\n") == Failure::HandshakeInvalid);
    report("the upgrade connection closed early",
           upgradeAnswer(QByteArray()) == Failure::ClosedEarly);
    {
        Harness h;
        MicrohamTrioSession::Config cfg = Harness::config();
        cfg.host = QStringLiteral("bad host");
        h.session.start(cfg);
        h.advance(1);
        report("a host that cannot go into a header opens nothing",
               h.transport.opens == 0 && h.session.lastFailure() == Failure::HostUnusable);
    }
}

void testHeldFailures()
{
    Harness split;
    split.connectHeld();
    split.transport.peerSends(textFrame(splitEngagedDoc()));
    report("a state the parser refuses fails the session",
           split.session.status() == Status::Failed
               && split.session.lastFailure() == Failure::UnreadableState
               && !split.session.snapshotCurrent());

    Harness masked;
    masked.connectHeld();
    masked.transport.peerSends(kIdleFrame + QByteArray("\x81\x84\x01\x02\x03\x04\x71\x6b\x6d\x63", 10));
    report("a frame the decoder refuses fails the session",
           masked.session.status() == Status::Failed
               && masked.session.lastFailure() == Failure::ProtocolError
               && !masked.transport.connected);

    Harness broken;
    broken.connectHeld();
    broken.transport.breaks();
    report("a broken held connection is a failure, not an eviction",
           broken.session.status() == Status::Failed
               && broken.session.lastFailure() == Failure::ConnectionLost
               && broken.session.state().link == Link::Failed);

    Harness odd;
    odd.connectHeld();
    odd.transport.peerSends(serverFrame(0x82, QByteArrayLiteral("bin"))
                            + serverFrame(kTextFrame, QByteArray())
                            + serverFrame(kPongFrame, QByteArrayLiteral("x")));
    report("a binary frame, an empty text frame and a pong with a payload are ignored",
           odd.session.status() == Status::Held && odd.snapshots.size() == 1);
}

void testStop()
{
    Harness h;
    h.connectHeld();
    const int closes = h.transport.closes;
    const qsizetype sent = h.transport.sent.size();
    h.session.stop();
    report("stop() sends one WebSocket close, status 1000, masked",
           h.transport.sent.size() == sent + 1 && h.transport.sent.last().size() == 8
               && static_cast<quint8>(h.transport.sent.last().at(0)) == 0x88
               && static_cast<quint8>(h.transport.sent.last().at(1)) == 0x82);
    report("stop() closes the connection and forgets the state",
           h.transport.closes == closes + 1 && h.session.status() == Status::Idle
               && !h.session.snapshot().has_value()
               && h.session.state().link == Link::NeverConnected);
    const int opens = h.transport.opens;
    h.clock.now += 60000;
    h.session.tick();
    report("a stopped session does nothing", h.transport.opens == opens);
    h.transport.peerSends(textFrame(stateDoc("radio2", "ant1", kZeroId)));
    report("and reads nothing", h.snapshots.size() == 1);
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    // Line by line, so the checks before a crash are still in the log.
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    testRequests();
    testCookie();
    testUpgradeResponse();
    testClientFrames();
    testDecoder();
    testFixtures();
    testNothingBeforeStart();
    testConnect();
    testRecordedHour();
    testKeepAliveCadence();
    testDeadSession();
    testEvictionIsNotRetaken();
    testRelease();
    testErrorPage();
    testCloseAndPing();
    testConnectFailures();
    testHeldFailures();
    testStop();

    std::printf("\n%s (%d failed)\n", g_failed == 0 ? "PASS" : "FAIL", g_failed);
    return g_failed == 0 ? 0 : 1;
}
