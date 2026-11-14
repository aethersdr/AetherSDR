#pragma once

// A scripted microHAM TRIO for tests of the model and the Peripherals page: a transport
// that connects nowhere, a hand-moved clock, and the device's side of one
// connect. No socket, no event loop. The state document and the response
// heads are the recorded ones; selections, id and four labels are parameters.

#include "core/MicrohamTrioProtocol.h"
#include "core/MicrohamTrioSession.h"
#include "core/MicrohamTrioWebSocket.h"
#include "models/MicrohamTrioModel.h"

#include <QByteArray>
#include <QString>
#include <QVector>

#include <algorithm>

namespace TrioTestPeer {

using namespace AetherSDR;

inline const char* const kZeroId = "00000000-0000-0000-0000-000000000000";

inline QByteArray stateDoc(const char* radio, const char* antenna, const QByteArray& id = kZeroId,
                           const char* radio2Label = "HL2", const char* ant6Label = "Dummy",
                           const char* radio3Label = "SWR", const char* ant5Label = "Antenna5")
{
    return QByteArray(R"({"s":{"radio":{"v":")") + radio
           + R"(","b":{"radio1":{"n":"RADIO1","a":"R1","l":"FT710","e":true},)"
             R"("radio2":{"n":"RADIO2","a":"R2","l":")"
           + radio2Label
           + R"(","e":true},)"
             R"("radio3":{"n":"RADIO3","a":"R3","l":")"
           + radio3Label
           + R"(","e":true}}},)"
             R"("split":{"v":false,"b":{"split":{"n":"SPLIT","a":"SPL","l":"Split","e":true}}},)"
             R"("pa":{"v":false,"b":{"pa":{"n":"PA","a":"PA","l":"PA","e":true}}},)"
             R"("antenna":{"v":")"
           + antenna
           + R"(","b":{"ant1":{"n":"ANTENNA1","a":"A1","l":"EFHW","e":true},)"
             R"("ant2":{"n":"ANTENNA2","a":"A2","l":"Fan Dipole","e":true},)"
             R"("ant3":{"n":"ANTENNA3","a":"A3","l":"Antenna3","e":true},)"
             R"("ant4":{"n":"ANTENNA4","a":"A4","l":"Antenna4","e":true},)"
             R"("ant5":{"n":"ANTENNA5","a":"A5","l":")"
           + ant5Label
           + R"(","e":true},)"
             R"("ant6":{"n":"ANTENNA6","a":"A6","l":")"
           + ant6Label
           + R"(","e":true}}},)"
             R"("rxant":{"v":"rx1","b":{"rx1":{"n":"RX1","a":"RX1","l":"RX1","e":true},)"
             R"("rx2":{"n":"RX2","a":"RX2","l":"RX2","e":true},)"
             R"("rx3":{"n":"RX3","a":"RX3","l":"RX3","e":true}}},)"
             R"("rxext":{"v":false,"b":{"rxext":{"n":"RXEXT","a":"RxE","l":"RxExt","e":true}}}},)"
             R"("id":")"
           + id + R"("})";
}

// Split as d184 recorded it: no `v`, the halves "tx_<tx>" and "rx_<rx>".
inline QByteArray splitDoc(const char* radio, const char* tx, const char* rx,
                           const QByteArray& id = kZeroId)
{
    QByteArray doc = stateDoc(radio, tx, id);
    doc.replace(R"("split":{"v":false)", R"("split":{"v":true)");
    doc.replace(QByteArray(R"("antenna":{"v":")") + tx + '"',
                QByteArray(R"("antenna":{"txv":"tx_)") + tx + R"(","rxv":"rx_)" + rx + '"');
    return doc;
}

// The receive-only groups set: RX antenna `rxant`, RX EXT `rxext`.
inline QByteArray receiveDoc(const char* radio, const char* antenna, const char* rxant,
                             bool rxext, const QByteArray& id = kZeroId)
{
    QByteArray doc = stateDoc(radio, antenna, id);
    doc.replace(R"("rxant":{"v":"rx1")", QByteArray(R"("rxant":{"v":")") + rxant + '"');
    if (rxext) {
        doc.replace(R"("rxext":{"v":false)", R"("rxext":{"v":true)");
    }
    return doc;
}

// The recorded document with one edit, which must land exactly once.
inline QByteArray edited(QByteArray doc, const QByteArray& from, const QByteArray& to, bool* ok)
{
    if (doc.count(from) != 1) {
        *ok = false;
        return doc;
    }
    doc.replace(from, to);
    return doc;
}

inline QByteArray serverFrame(quint8 firstByte, const QByteArray& payload)
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

inline QByteArray textFrame(const QByteArray& payload)
{
    return serverFrame(MicrohamTrio::kTextFrame, payload);
}

inline QByteArray idleFrame()
{
    return QByteArray("\x8a\x00", 2);
}

// The text of a masked client text frame, or empty.
inline QByteArray clientText(const QByteArray& bytes)
{
    if (bytes.size() < 6) {
        return {};
    }
    const quint8 first = static_cast<quint8>(bytes.at(0));
    const quint8 second = static_cast<quint8>(bytes.at(1));
    qsizetype length = second & 0x7f;
    qsizetype offset = 2;
    if (length == 126) {
        length = (static_cast<quint8>(bytes.at(2)) << 8) | static_cast<quint8>(bytes.at(3));
        offset = 4;
    }
    if (first != 0x81 || (second & 0x80) == 0 || bytes.size() != offset + 4 + length) {
        return {};
    }
    QByteArray payload;
    for (qsizetype i = 0; i < length; ++i) {
        payload.append(static_cast<char>(bytes.at(offset + 4 + i) ^ bytes.at(offset + (i % 4))));
    }
    return payload;
}

class FakeClock final : public MicrohamTrioClock {
public:
    qint64 nowMs() const override { return now; }
    qint64 now{0};
};

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

struct Peer {
    FakeClock     clock;
    FakeTransport transport;
    quint32       randomWord{0x1f2e3d4c};
    bool          autoPong{true};
    bool          stuck{false};
    qsizetype     sentSeen{0};

    MicrohamTrioModel::Environment environment()
    {
        MicrohamTrioModel::Environment out;
        out.transport = &transport;
        out.clock = &clock;
        out.entropy.random32 = [this]() {
            randomWord = randomWord * 1664525u + 1013904223u;
            return randomWord;
        };
        return out;
    }

    void answerPings()
    {
        for (; sentSeen < transport.sent.size(); ++sentSeen) {
            if (autoPong && transport.connected
                && clientText(transport.sent.at(sentSeen)) == QByteArrayLiteral("ping")) {
                transport.peerSends(textFrame(QByteArrayLiteral("pong")));
            }
        }
    }

    // Move the clock, running the session's decision exactly when its own
    // timer would.
    void advanceTo(MicrohamTrioModel& model, qint64 targetMs)
    {
        for (;;) {
            MicrohamTrioSession* session = model.session();
            if (session == nullptr || !session->isRunning()
                || session->nextTickAtMs() > targetMs) {
                break;
            }
            const qint64 due = session->nextTickAtMs();
            clock.now = std::max(clock.now, due);
            session->tick();
            answerPings();
            session = model.session();
            if (session != nullptr && session->isRunning() && session->nextTickAtMs() <= due) {
                stuck = true;
                break;
            }
        }
        clock.now = std::max(clock.now, targetMs);
    }
    void advance(MicrohamTrioModel& model, qint64 ms) { advanceTo(model, clock.now + ms); }

    // Advance with the device's idle frame once a second.
    void hold(MicrohamTrioModel& model, qint64 ms)
    {
        const qint64 end = clock.now + ms;
        while (clock.now < end) {
            advanceTo(model, std::min(end, clock.now + 1000));
            const MicrohamTrioSession* session = model.session();
            if (transport.connected && session != nullptr
                && session->status() == MicrohamTrioSession::Status::Held) {
                transport.peerSends(idleFrame());
            }
        }
    }

    // The device's side of one connect: the page with its cookie, then the
    // 101, then `firstFrames`.
    bool handshake(const QByteArray& firstFrames)
    {
        if (!transport.openPending) {
            return false;
        }
        transport.peerOpens();
        transport.peerSends(
            "HTTP/1.0 200 OK\r\nServer: TRIO web server\r\nSet-Cookie: WID=970120870\r\n"
            "Content-type: text/html\r\nExpires: Fri, 10 Apr 2008 14:00:00 GMT\r\n"
            "Pragma: no-cache\r\n\r\n<html>");
        transport.peerCloses();
        if (!transport.openPending) {
            return false;
        }
        transport.peerOpens();
        const QByteArray request = transport.sent.isEmpty() ? QByteArray() : transport.sent.last();
        const QByteArray marker("Sec-WebSocket-Key: ");
        const qsizetype start = request.indexOf(marker);
        if (start < 0) {
            return false;
        }
        const qsizetype end = request.indexOf("\r\n", start);
        const QByteArray key = request.mid(start + marker.size(), end - start - marker.size());
        sentSeen = transport.sent.size();
        transport.peerSends(
            QByteArray("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                       "Connection: Upgrade\r\nSec-WebSocket-Accept: ")
            + MicrohamTrio::Ws::acceptFor(key) + "\r\n\r\n" + firstFrames);
        return true;
    }

    bool handshakeWithState(const char* radio = "radio2", const char* antenna = "ant2")
    {
        return handshake(textFrame(stateDoc(radio, antenna)));
    }

    // Run the pending connect of `model` to a held session with a state.
    bool connectHeld(MicrohamTrioModel& model, const char* radio = "radio2",
                     const char* antenna = "ant2")
    {
        advance(model, 1);
        return handshakeWithState(radio, antenna);
    }

    void push(const QByteArray& doc) { transport.peerSends(textFrame(doc)); }

    // The last text frame the model's session sent, unmasked.
    QByteArray lastSentText() const
    {
        return transport.sent.isEmpty() ? QByteArray() : clientText(transport.sent.last());
    }
};

} // namespace TrioTestPeer
