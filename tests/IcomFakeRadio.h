#pragma once

// A fake IC-705 on localhost, shared by the IcomCIV integration tests.
//
// Deliberately STRICT: it answers only what a real IC-705 answers, in the order
// it answers it, and it refuses to grant streams unless the client did the two
// separate auths and echoed back the radio identity from the capabilities
// packet. A client that skips a step gets silence, which is exactly what the
// real radio does.
//
// Header-only so each test gets its own instance with no link-order surprises.

#include "core/backends/icom/CivCodec.h"
#include "core/backends/icom/IcomProtocol.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QObject>
#include <QUdpSocket>

#include <algorithm>
#include <functional>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace AetherSDR::icom::test {

using namespace AetherSDR::icom;


constexpr std::uint8_t kIc705Addr = 0xA4;
constexpr std::uint64_t kRadioFrequencyHz = 14'074'000;

void putLe32(std::vector<std::uint8_t>& p, std::size_t at, std::uint32_t v)
{
    p[at] = v & 0xff; p[at + 1] = (v >> 8) & 0xff;
    p[at + 2] = (v >> 16) & 0xff; p[at + 3] = (v >> 24) & 0xff;
}
void putLe16(std::vector<std::uint8_t>& p, std::size_t at, std::uint16_t v)
{
    p[at] = v & 0xff; p[at + 1] = (v >> 8) & 0xff;
}
void putBe32(std::vector<std::uint8_t>& p, std::size_t at, std::uint32_t v)
{
    p[at] = (v >> 24) & 0xff; p[at + 1] = (v >> 16) & 0xff;
    p[at + 2] = (v >> 8) & 0xff; p[at + 3] = v & 0xff;
}
std::uint32_t getBe32(const QByteArray& b, int at)
{
    return (static_cast<std::uint32_t>(static_cast<std::uint8_t>(b[at])) << 24)
         | (static_cast<std::uint32_t>(static_cast<std::uint8_t>(b[at + 1])) << 16)
         | (static_cast<std::uint32_t>(static_cast<std::uint8_t>(b[at + 2])) << 8)
         | static_cast<std::uint8_t>(b[at + 3]);
}
std::uint16_t getLe16(const QByteArray& b, int at)
{
    return static_cast<std::uint16_t>(static_cast<std::uint8_t>(b[at])
                                      | (static_cast<std::uint8_t>(b[at + 1]) << 8));
}
std::uint16_t getBe16(const QByteArray& b, int at)
{
    return static_cast<std::uint16_t>((static_cast<std::uint8_t>(b[at]) << 8)
                                      | static_cast<std::uint8_t>(b[at + 1]));
}

// One UDP endpoint of the fake radio. Handles the handshake and pings that are
// common to all three streams; the payload behaviour is a callback.
class FakeStream : public QObject {
public:
    using PayloadFn = std::function<void(FakeStream&, const QByteArray&)>;

    FakeStream(std::uint32_t sid, PayloadFn fn, QObject* parent = nullptr)
        : QObject(parent), m_sid(sid), m_fn(std::move(fn))
    {
        m_socket = new QUdpSocket(this);
        m_socket->bind(QHostAddress::LocalHost, 0);
        connect(m_socket, &QUdpSocket::readyRead, this, &FakeStream::onReadyRead);
    }

    [[nodiscard]] quint16 port() const { return m_socket->localPort(); }
    [[nodiscard]] bool sawHandshake() const { return m_ready; }
    [[nodiscard]] int payloadCount() const { return m_payloads; }

    void send(const std::vector<std::uint8_t>& p)
    {
        if (m_peerPort == 0)
            return;
        m_socket->writeDatagram(reinterpret_cast<const char*>(p.data()),
                                static_cast<qint64>(p.size()), m_peerAddr, m_peerPort);
    }

    // A 16-byte framed packet from the radio's point of view.
    std::vector<std::uint8_t> frame(std::size_t len, std::uint16_t type, std::uint16_t seq) const
    {
        std::vector<std::uint8_t> p(len, 0);
        putLe32(p, 0x00, static_cast<std::uint32_t>(len));
        putLe16(p, 0x04, type);
        putLe16(p, 0x06, seq);
        putBe32(p, 0x08, m_sid);        // the radio's own id
        putBe32(p, 0x0c, m_clientSid);  // echoed back
        return p;
    }

private:
    void onReadyRead()
    {
        while (m_socket->hasPendingDatagrams()) {
            QByteArray buf;
            buf.resize(static_cast<qsizetype>(m_socket->pendingDatagramSize()));
            m_socket->readDatagram(buf.data(), buf.size(), &m_peerAddr, &m_peerPort);
            handle(buf);
        }
    }

    void handle(const QByteArray& b)
    {
        if (b.size() < 16)
            return;
        const std::uint16_t type = getLe16(b, 0x04);

        // Pings, both directions.
        if (b.size() == 21 && type == 0x07) {
            if (static_cast<std::uint8_t>(b[0x10]) == 0x00) {
                auto reply = frame(21, 0x07, getLe16(b, 0x06));
                reply[0x10] = 0x01;
                for (int i = 0; i < 4; ++i)
                    reply[0x11 + i] = static_cast<std::uint8_t>(b[0x11 + i]);
                send(reply);
            }
            return;
        }

        if (type == 0x03) {   // are-you-there
            m_clientSid = getBe32(b, 0x08);
            send(frame(16, 0x04, 0));
            return;
        }
        if (type == 0x06) {   // are-you-ready
            m_ready = true;
            send(frame(16, 0x06, 1));
            return;
        }
        if (type == 0x05) {   // disconnect
            m_ready = false;
            return;
        }
        if (type != 0x00 || b.size() <= 16)
            return;

        ++m_payloads;
        if (m_fn)
            m_fn(*this, b);
    }

    QUdpSocket* m_socket = nullptr;
    QHostAddress m_peerAddr;
    quint16 m_peerPort = 0;
    std::uint32_t m_sid = 0;
    std::uint32_t m_clientSid = 0;
    bool m_ready = false;
    int m_payloads = 0;
    PayloadFn m_fn;
};

// The fake IC-705 itself.
class FakeIc705 : public QObject {
public:
    explicit FakeIc705(QObject* parent = nullptr) : QObject(parent)
    {
        m_control = new FakeStream(0xAAAA0001,
                                   [this](FakeStream& s, const QByteArray& b) { control(s, b); },
                                   this);
        m_serial = new FakeStream(0xAAAA0002,
                                  [this](FakeStream& s, const QByteArray& b) { serial(s, b); },
                                  this);
        m_audio = new FakeStream(0xAAAA0003,
                                 [this](FakeStream& s, const QByteArray& b) { audio(s, b); },
                                 this);
    }

    [[nodiscard]] quint16 controlPort() const { return m_control->port(); }
    [[nodiscard]] quint16 serialPort() const { return m_serial->port(); }
    [[nodiscard]] quint16 audioPort() const { return m_audio->port(); }

    // What the client told us it would use. The whole point of announcing them.
    [[nodiscard]] quint32 announcedCivPort() const { return m_announcedCiv; }
    [[nodiscard]] quint32 announcedAudioPort() const { return m_announcedAudio; }
    [[nodiscard]] quint32 announcedSampleRate() const { return m_announcedRate; }
    [[nodiscard]] std::uint8_t announcedRxCodec() const { return m_announcedRxCodec; }
    [[nodiscard]] int authCount() const { return m_authCount; }
    [[nodiscard]] bool serialOpened() const { return m_serialOpened; }
    [[nodiscard]] bool sawUsernameObfuscated() const { return m_usernameObfuscated; }
    [[nodiscard]] int civCommandsSeen() const { return m_civCommands; }
    // What the radio currently holds for a 1A 05 leaf — the persisted-state
    // question a client must not answer from its own memory.
    [[nodiscard]] int setting(int item) const
    {
        auto it = m_settings.find(item);
        return it == m_settings.end() ? -1 : static_cast<int>(it->second);
    }
    void setSetting(int item, std::uint8_t value) { m_settings[item] = value; }
    [[nodiscard]] const std::vector<std::uint16_t>& renewalSequences() const
    {
        return m_renewalSequences;
    }
    [[nodiscard]] const std::vector<std::uint16_t>& deauthOuterSequences() const
    {
        return m_deauthOuterSequences;
    }
    [[nodiscard]] const std::vector<std::uint16_t>& loginTokenRequestIds() const
    {
        return m_loginTokenRequestIds;
    }
    [[nodiscard]] const std::vector<AuthId>& renewalAuthIds() const
    {
        return m_renewalAuthIds;
    }
    [[nodiscard]] const std::vector<AuthId>& streamRequestAuthIds() const
    {
        return m_streamRequestAuthIds;
    }

    // Lease fault injection used by IcomSession integration tests.
    void setInjectStaleGrant(bool on) { m_injectStaleGrant = on; }
    void setRejectRenewalsAfter(int accepted) { m_acceptRenewals = accepted; }
    void setReissueInitialTokenOnNextLogin(bool on) { m_reissueNextInitialToken = on; }
    void setAuthFailureOnLogin(bool on) { m_authFailureOnLogin = on; }
    // The PREVIOUS session's teardown status, delivered on the new control
    // association before this session has a stream grant to protect it. At
    // that point the lifecycle exception does not apply yet, so the header
    // IDs are the only thing separating it from a real login failure.
    void setStaleAuthFailureBeforeLoginReply(bool on) { m_staleAuthFailureOnLogin = on; }
    // Answer the next renewal with an inner sequence the client never sent.
    // A client that acknowledges on packet shape alone accepts it anyway.
    void setCorruptNextRenewalSequence(bool on) { m_corruptNextRenewalSeq = on; }

    // Every CI-V command the client sent, in order. Counting them says the link
    // is alive; keeping them is what lets a test assert that a particular
    // command was NOT sent — which is the only way to prove an intent stopped
    // retuning the radio.
    [[nodiscard]] const std::vector<CivFrame>& civCommands() const { return m_civLog; }
    void clearCivLog() { m_civLog.clear(); }

    // GO DEAF. The radio keeps its UDP streams up — the transport stays
    // perfectly healthy — and simply stops answering CI-V. That is the exact
    // shape of the stall this exists to reproduce: `isConnected()` stays true,
    // link statistics keep climbing, and the command plane is dead.
    void setCivSilent(bool silent) { m_civSilent = silent; }

    // ---- BE A DIFFERENT ICOM ------------------------------------------------
    //
    // The fake was hard-wired to the IC-705's 0xA4 and answered 0x19 0x00 no
    // matter who the frame was addressed to, which made it structurally unable
    // to reproduce the bug this whole feature exists for: on a real bus CI-V is
    // ADDRESSED, and an IC-9700 on 0xA2 ignores everything sent to 0xA4 in
    // total silence. A fake that answers anyway turns that silent dead session
    // into a passing test.
    //
    // Defaults are unchanged, so every test written before this one still faces
    // the same IC-705 it always did.
    void setCivAddress(std::uint8_t address) { m_addr = address; }
    // The tighter of the two windows the name is copied into: 0x40 in a
    // 0x90-byte connect grant, against 0x52 in a 0xA8-byte announce.
    static constexpr std::size_t kMaxNameBytes = 0x90 - 0x40;

    // TRUNCATED, because both carriers std::copy into a fixed offset in a
    // fixed-size frame with no length check of their own. Every name the suite
    // uses is a model designation of a handful of characters, so this has never
    // fired — but a fake radio that scribbles past the end of its own frame
    // fails as memory corruption somewhere else entirely, which is a debugging
    // trap rather than a test failure. Truncating is also what a fixed-width
    // device-name field on a real radio does.
    void setDeviceName(std::string name)
    {
        m_name = std::move(name);
        if (m_name.size() > kMaxNameBytes)
            m_name.resize(kMaxNameBytes);
    }

    // A SECOND DEVICE ON THE BUS. Icom's own RS-BA1 server can front a serial
    // CI-V bus carrying another radio, a rotator or an amplifier, and all of
    // them answer a broadcast. Set this and the broadcast draws two replies with
    // two different addresses — the case where adopting either would be a guess.
    void setSecondResponder(std::uint8_t address) { m_secondAddr = address; }

    // ECHO EVERY TRANSPORTED FRAME BACK, as a real CI-V bus does.
    //
    // Off by default: the suite predates it and none of its assertions expect
    // echoes. On, it is the only way to exercise the "skip our own echo" rule
    // against a frame that really was ours — and the echo of a broadcast is the
    // one that matters, because it arrives FIRST (measured on both lab radios,
    // 2026-08-14) and does not match the naive echo filter.
    void setCivEcho(bool echo) { m_civEcho = echo; }
    [[nodiscard]] int audioPacketsFromClient() const { return m_audioFromClient; }

    // Push an unsolicited CI-V frame (CI-V Transceive, or a scope sweep).
    void pushCiv(const std::vector<std::uint8_t>& civ)
    {
        auto p = m_serial->frame(21 + civ.size(), 0x00, m_serialSeq++);
        p[0x10] = 0xc1;
        // LITTLE-endian u16, not a byte — a scope sweep is ~496 bytes.
        p[0x11] = static_cast<std::uint8_t>(civ.size() & 0xff);
        p[0x12] = static_cast<std::uint8_t>((civ.size() >> 8) & 0xff);
        p[0x13] = 0x00;
        p[0x14] = static_cast<std::uint8_t>(m_civSeq++);
        std::copy(civ.begin(), civ.end(), p.begin() + 21);
        m_serial->send(p);
    }

    void pushAudio(const std::vector<std::uint8_t>& pcm)
    {
        auto p = m_audio->frame(24 + pcm.size(), 0x00, m_audioSeq++);
        p[0x10] = 0x80;
        p[0x12] = static_cast<std::uint8_t>((m_audioInner >> 8) & 0xff);
        p[0x13] = static_cast<std::uint8_t>(m_audioInner & 0xff);
        ++m_audioInner;
        p[0x16] = static_cast<std::uint8_t>((pcm.size() >> 8) & 0xff);
        p[0x17] = static_cast<std::uint8_t>(pcm.size() & 0xff);
        std::copy(pcm.begin(), pcm.end(), p.begin() + 24);
        m_audio->send(p);
    }

    // Push the auth-failure status seen when an old session tears down during
    // a reconnect. With stale=false it belongs to the current session and must
    // be fatal; with stale=true it must not kill the new media lease.
    void pushAuthFailure(bool stale)
    {
        auto status = m_control->frame(kLenStatus, 0x00, m_seq++);
        status[0x30] = status[0x31] = status[0x32] = 0xff;
        status[0x40] = 0x01;
        if (stale) {
            putBe32(status, 0x08, 0xCCCC0001);
            putBe32(status, 0x0c, 0xCCCC0002);
        }
        m_control->send(status);
    }

private:
    void control(FakeStream& s, const QByteArray& b)
    {
        switch (b.size()) {
        case 0x80: {   // login
            // A real radio only accepts the OBFUSCATED credential. Checking it
            // here is what makes the passcode table load-bearing in this test
            // rather than incidental.
            const auto expect = encodePasscode("beer");
            m_usernameObfuscated = std::equal(expect.begin(), expect.end(),
                                              reinterpret_cast<const std::uint8_t*>(b.constData())
                                                  + 0x40);
            const std::uint16_t tokenRequestId = getLe16(b, 0x1a);
            m_loginTokenRequestIds.push_back(tokenRequestId);
            if (m_authFailureOnLogin) {
                auto status = s.frame(kLenStatus, 0x00, m_seq++);
                status[0x30] = status[0x31] = status[0x32] = status[0x33] = 0xff;
                s.send(status);
                return;
            }
            if (m_staleAuthFailureOnLogin) {
                m_staleAuthFailureOnLogin = false;
                pushAuthFailure(true);
            }
            m_sentCaps = false;
            m_reissueThisInitialToken = m_reissueNextInitialToken;
            m_reissueNextInitialToken = false;
            auto reply = s.frame(0x60, 0x00, m_seq++);
            reply[0x14] = 0x02;
            reply[0x1a] = static_cast<std::uint8_t>(tokenRequestId & 0xff);
            reply[0x1b] = static_cast<std::uint8_t>((tokenRequestId >> 8) & 0xff);
            for (std::size_t i = 2; i < 6; ++i) {
                reply[0x1a + i] = static_cast<std::uint8_t>(0xC0 + i);
            }
            s.send(reply);
            return;
        }
        case 0x40: {   // auth
            const auto kind = static_cast<std::uint8_t>(b[0x15]);
            ++m_authCount;
            if (kind == static_cast<std::uint8_t>(AuthKind::Deauth)) {
                m_deauthOuterSequences.push_back(getLe16(b, 0x06));
                return;
            }
            if (kind != 0x05) {
                return;   // the FIRST auth gets no reply; only the token request does
            }
            const std::uint16_t innerSeq = getBe16(b, 0x16);
            m_renewalSequences.push_back(innerSeq);
            AuthId requestAuthId{};
            for (std::size_t i = 0; i < requestAuthId.size(); ++i) {
                requestAuthId[i] = static_cast<std::uint8_t>(b[0x1a + i]);
            }
            m_renewalAuthIds.push_back(requestAuthId);
            auto reply = s.frame(0x40, 0x00, m_seq++);
            reply[0x14] = 0x02;
            reply[0x15] = 0x05;
            // A renewal reply correlates through both the 16-bit inner
            // sequence and the current auth ID. Echoing neither used to let a
            // client test pass while ignoring the fields the live radio uses.
            for (int i = 0x16; i < 0x20; ++i)
                reply[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(b[i]);
            if (m_corruptNextRenewalSeq) {
                m_corruptNextRenewalSeq = false;
                // Well-formed in every other respect — right shape, right auth
                // ID, zero response — so the outstanding-request set is the
                // only thing that can reject it.
                const auto bogus = static_cast<std::uint16_t>(innerSeq + 0x4000);
                reply[0x16] = static_cast<std::uint8_t>((bogus >> 8) & 0xff);
                reply[0x17] = static_cast<std::uint8_t>(bogus & 0xff);
            }
            const bool accepted = m_acceptedRenewals < m_acceptRenewals;
            if (m_reissueThisInitialToken) {
                m_reissueThisInitialToken = false;
                // Rotate the six-byte material so adopting the reissued token
                // is load-bearing in the session test rather than a no-op.
                for (std::size_t i = 0; i < requestAuthId.size(); ++i) {
                    reply[0x1a + i] = static_cast<std::uint8_t>(0xF0 + i);
                }
                reply[0x30] = reply[0x31] = reply[0x32] = reply[0x33] = 0xff;
            } else if (accepted) {
                ++m_acceptedRenewals;
            } else {
                reply[0x30] = reply[0x31] = reply[0x32] = reply[0x33] = 0xff;
            }

            auto sendCapabilities = [&] {
                if (m_sentCaps) {
                    return;
                }
                m_sentCaps = true;
                auto caps = s.frame(0xA8, 0x00, m_seq++);
                for (std::size_t i = 0; i < 16; ++i)
                    caps[0x42 + i] = static_cast<std::uint8_t>(0x40 + i);
                std::copy(m_name.begin(), m_name.end(), caps.begin() + 0x52);
                s.send(caps);
            };
            if (m_injectStaleGrant) {
                // Reproduce the live reconnect race: capabilities, then an old
                // grant, then this request's token reply. Normal fake-radio
                // behavior below keeps its historical token-before-caps order.
                sendCapabilities();
                if (!m_sentPrematureGrant) {
                    m_sentPrematureGrant = true;
                    sendGrant(s, true);
                }
                s.send(reply);
            } else {
                s.send(reply);
                sendCapabilities();
            }
            return;
        }
        case 0x90: {   // stream request
            AuthId requestAuthId{};
            for (std::size_t i = 0; i < requestAuthId.size(); ++i) {
                requestAuthId[i] = static_cast<std::uint8_t>(b[0x1a + i]);
            }
            m_streamRequestAuthIds.push_back(requestAuthId);

            // Refuse unless the client echoed back the identity we published in
            // the capabilities packet. A real radio has to know WHICH radio the
            // client wants when a server fronts several.
            bool identityEchoed = true;
            for (std::size_t i = 0; i < 16; ++i)
                if (static_cast<std::uint8_t>(b[0x20 + i]) != static_cast<std::uint8_t>(0x40 + i))
                    identityEchoed = false;
            if (!identityEchoed)
                return;

            m_announcedRxCodec = static_cast<std::uint8_t>(b[0x72]);
            m_announcedRate  = getBe32(b, 0x74);
            m_announcedCiv   = getBe32(b, 0x7c);
            m_announcedAudio = getBe32(b, 0x80);

            if (m_injectStaleGrant)
                sendGrant(s, true);
            sendGrant(s, false);
            return;
        }
        default:
            return;
        }
    }

    void serial(FakeStream& s, const QByteArray& b)
    {
        const auto marker = static_cast<std::uint8_t>(b[0x10]);
        if (b.size() == 0x16 && marker == 0xc0) {
            m_serialOpened = static_cast<std::uint8_t>(b[0x15]) == 0x05;
            return;
        }
        if (marker != 0xc1)
            return;

        const auto len = static_cast<std::size_t>(getLe16(b, 0x11));
        if (static_cast<std::size_t>(b.size()) != 21 + len)
            return;
        std::vector<std::uint8_t> civ(b.constData() + 21, b.constData() + 21 + len);
        auto frame = parseFrame(civ);
        if (!frame)
            return;
        ++m_civCommands;
        m_civLog.push_back(*frame);
        // Logged BEFORE the silence check: a deaf radio still receives, which
        // is what lets a test assert the command went out and got no answer.
        if (m_civSilent)
            return;

        // THE BUS ECHOES FIRST, whoever the frame was for. This is what makes a
        // missing echo mean "the transport is dead" rather than "the radio
        // declined to answer" — the diagnostic that separated a wedged radio
        // from a wrong address during the hardware measurements.
        if (m_civEcho)
            pushCiv(civ);

        // NOT FOR US. CI-V is addressed and a real radio simply ignores traffic
        // for somebody else — no error, no reply, nothing. That silence IS the
        // bug being fixed, so the fake has to reproduce it. 0x00 is broadcast
        // and every device on the bus answers it.
        if (frame->to != m_addr && frame->to != kBroadcastAddress)
            return;

        // Name itself. A real radio answers 0x19 0x00 with its own CI-V
        // address, and that is how a client learns WHICH Icom it is talking to
        // — several models speak this same transport, and the address is
        // user-changeable, so a client that assumes 0xA4 mis-decodes the rest.
        if (frame->cmd == cmd::kReadId) {
            pushCiv({0xFE, 0xFE, kControllerAddress, m_addr, cmd::kReadId, 0x00,
                     m_addr, kCivEom});
            // The other device on the bus answers the broadcast too. Only the
            // broadcast: a directed query reaches exactly one of them.
            if (m_secondAddr != 0 && frame->to == kBroadcastAddress)
                pushCiv({0xFE, 0xFE, kControllerAddress, m_secondAddr, cmd::kReadId,
                         0x00, m_secondAddr, kCivEom});
            return;
        }

        // Answer a read-frequency with the radio's frequency, addressed BACK to
        // the controller.
        if (frame->cmd == cmd::kReadFreq) {
            std::vector<std::uint8_t> reply{0xFE, 0xFE, kControllerAddress, kIc705Addr,
                                            cmd::kReadFreq};
            const auto bcd = encodeFreq(kRadioFrequencyHz);
            reply.insert(reply.end(), bcd.begin(), bcd.end());
            reply.push_back(kCivEom);
            pushCiv(reply);
            return;
        }
        // ---- READS ANSWERED FROM PERSISTED STATE ------------------------
        //
        // A real Icom remembers its own settings across power cycles and reports
        // them on request, and AetherSDR's whole contract with this family is
        // that it READS that state rather than pushing its own (Constitution
        // II/III). A fake radio that ACKs every read without answering it cannot
        // test that contract at all — the client would look correct while
        // adopting nothing.
        //
        // A read is the command with NO payload byte. The set form carries one,
        // and answering that with a value would be a radio talking back to its
        // own command.
        if (frame->cmd == cmd::kFunction && frame->hasSub && frame->data.empty()) {
            auto it = m_functions.find(frame->sub);
            if (it != m_functions.end()) {
                pushCiv({0xFE, 0xFE, kControllerAddress, kIc705Addr, cmd::kFunction,
                         frame->sub, it->second, kCivEom});
                return;
            }
        }
        if (frame->cmd == cmd::kLevel && frame->hasSub && frame->data.empty()) {
            auto it = m_levels.find(frame->sub);
            if (it != m_levels.end()) {
                // Two BCD bytes, 0000..0255 — the shape decodeLevel expects.
                const int v = it->second;
                pushCiv({0xFE, 0xFE, kControllerAddress, kIc705Addr, cmd::kLevel,
                         frame->sub,
                         static_cast<std::uint8_t>(((v / 1000) << 4) | ((v / 100) % 10)),
                         static_cast<std::uint8_t>((((v / 10) % 10) << 4) | (v % 10)),
                         kCivEom});
                return;
            }
        }
        if (frame->cmd == cmd::kLevel && frame->hasSub && !frame->data.empty()) {
            if (const std::optional<int> value = decodeLevel(frame->data)) {
                m_levels[frame->sub] = *value;
            }
            pushCiv({0xFE, 0xFE, kControllerAddress, kIc705Addr, kCivOk, kCivEom});
            return;
        }
        if (frame->cmd == cmd::kFunction && frame->hasSub && !frame->data.empty()) {
            m_functions[frame->sub] = frame->data.front();
            pushCiv({0xFE, 0xFE, kControllerAddress, kIc705Addr, kCivOk, kCivEom});
            return;
        }
        // ---- 1A 05 SET MENU ----------------------------------------------
        //
        // The item number is TWO BCD bytes, so 0118 arrives as 0x01 0x18; a
        // fake that read them as one byte would answer the wrong leaf. Read is
        // the two-byte form, write carries a third byte — the same read/write
        // split as every command pair above.
        //
        // Held as persisted state for the same reason the levels are: DATA OFF
        // MOD is a setting the RADIO remembers across power cycles, and a fake
        // that ACKs the read without answering it cannot tell a client which
        // adopts the radio's value from one that quietly keeps its own.
        if (frame->cmd == cmd::kSetting && frame->hasSub && frame->sub == 0x05
            && frame->data.size() >= 2) {
            const int item = decodeBcdByte(frame->data[0]) * 100
                + decodeBcdByte(frame->data[1]);
            if (frame->data.size() == 2) {
                auto it = m_settings.find(item);
                if (it == m_settings.end())
                    return;   // no such leaf on this model — silence, not an error
                pushCiv({0xFE, 0xFE, kControllerAddress, kIc705Addr, cmd::kSetting,
                         0x05, frame->data[0], frame->data[1], it->second, kCivEom});
                return;
            }
            m_settings[item] = frame->data[2];
            pushCiv({0xFE, 0xFE, kControllerAddress, kIc705Addr, kCivOk, kCivEom});
            return;
        }
        if (frame->cmd == cmd::kTuneOffset && frame->hasSub && frame->data.empty()) {
            if (frame->sub == tuneOffset::kFrequency) {
                // Two BCD bytes little-endian, then a sign byte.
                const int hz = m_ritHz < 0 ? -m_ritHz : m_ritHz;
                pushCiv({0xFE, 0xFE, kControllerAddress, kIc705Addr, cmd::kTuneOffset,
                         frame->sub,
                         static_cast<std::uint8_t>((((hz / 10) % 10) << 4) | (hz % 10)),
                         static_cast<std::uint8_t>((((hz / 1000) % 10) << 4) | ((hz / 100) % 10)),
                         static_cast<std::uint8_t>(m_ritHz < 0 ? 0x01 : 0x00),
                         kCivEom});
                return;
            }
            const std::uint8_t on = frame->sub == tuneOffset::kRitOnOff
                                        ? (m_ritOn ? 1 : 0) : (m_xitOn ? 1 : 0);
            pushCiv({0xFE, 0xFE, kControllerAddress, kIc705Addr, cmd::kTuneOffset,
                     frame->sub, on, kCivEom});
            return;
        }
        // MODE, DATA STATE AND FILTER SLOT — held as the three separate
        // registers a real radio holds, reachable through two different
        // commands that see different amounts of them.
        //
        // 04/06 CANNOT SEE DATA, DELIBERATELY. USB and USB-D are the same mode
        // byte 0x01; only 0x26 tells them apart. A fake that let 04 report DATA
        // would pass a client that never sends 26 — which is the entire bug
        // this radio exists to catch.
        if (frame->cmd == cmd::kReadMode) {
            pushCiv({0xFE, 0xFE, kControllerAddress, kIc705Addr, cmd::kReadMode,
                     m_mode, m_filter, kCivEom});
            return;
        }
        if (frame->cmd == cmd::kSetMode && frame->data.size() >= 2) {
            m_mode = frame->data[0];
            m_filter = frame->data[1];
            // WRITING THE ORDINARY MODE CLEARS DATA, which is what a real radio
            // does — and the reason a filter change must not go out as 06 while
            // the operator is in a data mode.
            m_dataOn = false;
            pushCiv({0xFE, 0xFE, kControllerAddress, kIc705Addr, kCivOk, kCivEom});
            return;
        }
        if (frame->cmd == cmd::kVfoMode && frame->hasSub
            && frame->sub == vfoMode::kSelected) {
            if (frame->data.empty()) {   // the read form
                pushCiv({0xFE, 0xFE, kControllerAddress, kIc705Addr, cmd::kVfoMode,
                         vfoMode::kSelected, m_mode,
                         static_cast<std::uint8_t>(m_dataOn ? 0x01 : 0x00),
                         m_filter, kCivEom});
                return;
            }
            if (frame->data.size() < 3)
                return;
            // A radio that will not enter DATA — a mode or band with no data
            // variant. It takes the mode and slot, ACKs, and stays DATA OFF,
            // which is the case an optimistic client cannot tell from success
            // without reading back.
            m_mode = frame->data[0];
            m_dataOn = m_refuseDataMode ? false : frame->data[1] != 0;
            if (frame->data[2] >= 1 && frame->data[2] <= 3)
                m_filter = frame->data[2];
            pushCiv({0xFE, 0xFE, kControllerAddress, kIc705Addr, kCivOk, kCivEom});
            return;
        }
        if (frame->cmd == cmd::kControl && frame->hasSub
            && frame->sub == control::kTuner && frame->data.empty()) {
            pushCiv({0xFE, 0xFE, kControllerAddress, kIc705Addr, cmd::kControl,
                     control::kTuner, m_tunerState, kCivEom});
            return;
        }
        if (frame->cmd == cmd::kControl && frame->hasSub
            && frame->sub == control::kTuner && !frame->data.empty()) {
            m_tunerState = frame->data.front() == 0x00 ? 0x00 : 0x01;
            pushCiv({0xFE, 0xFE, kControllerAddress, kIc705Addr, kCivOk, kCivEom});
            return;
        }
        if (frame->cmd == cmd::kControl && frame->hasSub
            && frame->sub == control::kPtt) {
            if (frame->data.empty()) {
                const bool report = m_pttOverride ? *m_pttOverride : m_ptt;
                pushCiv({0xFE, 0xFE, kControllerAddress, kIc705Addr, cmd::kControl,
                         control::kPtt, static_cast<std::uint8_t>(report ? 1 : 0), kCivEom});
                return;
            }
            // A radio that ACKs a PTT write and does not act on it is not
            // hypothetical: the write can be lost on the bus, refused (band
            // edge, SWR fold-back), or immediately overridden at the front
            // panel. `FB` means "understood", never "applied" — see
            // docs/radio-certification.md. Without a fake that can disagree
            // with the client's intent there is no way to test what the
            // backend does when radio truth contradicts a pending request.
            if (!m_pttOverride)
                m_ptt = frame->data.front() != 0;
            pushCiv({0xFE, 0xFE, kControllerAddress, kIc705Addr, kCivOk, kCivEom});
            return;
        }

        // Everything else is acknowledged.
        pushCiv({0xFE, 0xFE, kControllerAddress, kIc705Addr, kCivOk, kCivEom});
        (void)s;
    }

public:
    // The radio's PERSISTED state, as a test wants to arrange it. Defaults are
    // deliberately NOT the client's defaults — every one of these would read as
    // "working" if the client simply kept its own value, so a test that asserts
    // these exact numbers is asserting the pull actually happened.
    std::map<std::uint8_t, std::uint8_t> m_functions{
        {func::kNoiseReduce, 1},     // NR ON
        {func::kNoiseBlanker, 1},    // NB ON
        {func::kAutoNotch, 0},
        {func::kManualNotch, 1},     // manual notch ON
        {func::kCompressor, 1},      // PROC ON
        {func::kMonitorFn, 1},       // monitor ON
        {func::kVox, 1},             // VOX ON
        {func::kPreamp, 2},          // P.AMP2
        {func::kAgc, 3},             // SLOW
    };
    std::map<std::uint8_t, int> m_levels{
        {level::kAf, 128},        // ~50 %
        {level::kRf, 255},        // 100 %
        {level::kSquelch, 0},
        {level::kNrLevel, 77},    // ~30 %
        {level::kNbLevel, 179},   // ~70 %
        {level::kRfPower, 51},    // ~20 %
        {level::kMicGain, 153},   // ~60 %
        {level::kCompLevel, 102}, // ~40 %
        {level::kNotchPos, 128},  // ~50 %
        {level::kVoxGain, 204},   // ~80 %
    };
    // 1A 05 SET-menu leaves, by DECIMAL item number. The IC-705's DATA OFF MOD
    // starts at USB (0x01) rather than the WLAN (0x03) this client wants: an
    // operator with a rig interface on the USB port is the ordinary case, and
    // a fake that already sat on WLAN could not tell "PC Audio put it back"
    // from "PC Audio never touched it".
    std::map<int, std::uint8_t> m_settings{
        {118, 0x01},   // DATA OFF MOD = USB
        {119, 0x03},   // DATA MOD     = WLAN
        {116, 0x80},   // USB MOD level
        {117, 0x80},   // WLAN MOD level
    };
    // USB-D on FIL2 — NOT the client's construction default (USB, FIL1), so a
    // test asserting DIGU is asserting the client actually read 26 rather than
    // kept its own guess.
    std::uint8_t m_mode = static_cast<std::uint8_t>(CivMode::Usb);
    std::uint8_t m_filter = 2;
    bool m_dataOn = true;
    bool m_refuseDataMode = false;

    // Simulate the operator turning the MODE knob: the radio pushes an
    // unsolicited 01 (which cannot carry DATA) and nothing else. Following the
    // DATA half means the client has to ASK.
    void frontPanelMode(std::uint8_t mode, std::uint8_t filter, bool dataOn)
    {
        m_mode = mode;
        m_filter = filter;
        m_dataOn = dataOn;
        pushCiv({0xFE, 0xFE, kControllerAddress, kIc705Addr, cmd::kSetModeTrx,
                 m_mode, m_filter, kCivEom});
    }

    int  m_ritHz = -1230;
    bool m_ritOn = true;
    bool m_xitOn = false;
    std::uint8_t m_tunerState = 0x01;   // matched
    bool m_ptt = false;
    // Force what 1C 00 REPORTS, regardless of what it is told. While set, PTT
    // writes are still acknowledged but do not move m_ptt — the radio insists
    // on this state. Clear it (std::nullopt) to go back to an obedient radio.
    std::optional<bool> m_pttOverride;

private:

    void sendGrant(FakeStream& stream, bool stale)
    {
        auto grant = stream.frame(0x90, 0x00, m_seq++);
        std::copy(m_name.begin(), m_name.end(), grant.begin() + 0x40);
        for (std::size_t i = 0; i < 6; ++i)
            grant[0x1a + i] = static_cast<std::uint8_t>((stale ? 0xE0 : 0xD0) + i);
        grant[0x60] = 0x01;
        if (stale) {
            putBe32(grant, 0x08, 0xBBBB0001);
            putBe32(grant, 0x0c, 0xBBBB0002);
        }
        stream.send(grant);
    }

    void audio(FakeStream&, const QByteArray& b)
    {
        if (b.size() > 24 && static_cast<std::uint8_t>(b[0x10]) == 0x80)
            ++m_audioFromClient;
    }

    FakeStream* m_control = nullptr;
    FakeStream* m_serial = nullptr;
    FakeStream* m_audio = nullptr;
    std::uint16_t m_seq = 0;
    std::uint16_t m_serialSeq = 0;
    std::uint16_t m_audioSeq = 0;
    std::uint16_t m_audioInner = 1;
    // WHICH Icom this is. Defaults to the IC-705 the suite was written against.
    std::uint8_t  m_addr = kIc705Addr;
    std::uint8_t  m_secondAddr = 0;
    bool          m_civEcho = false;
    std::string   m_name = "IC-705";
    std::uint8_t  m_civSeq = 0;
    std::vector<CivFrame> m_civLog;
    bool m_sentCaps = false;
    bool m_serialOpened = false;
    bool m_usernameObfuscated = false;
    int m_authCount = 0;
    int m_civCommands = 0;
    bool m_civSilent = false;
    bool m_injectStaleGrant = false;
    bool m_sentPrematureGrant = false;
    bool m_reissueNextInitialToken = false;
    bool m_reissueThisInitialToken = false;
    bool m_authFailureOnLogin = false;
    bool m_staleAuthFailureOnLogin = false;
    bool m_corruptNextRenewalSeq = false;
    int m_acceptRenewals = std::numeric_limits<int>::max();
    int m_acceptedRenewals = 0;
    std::vector<std::uint16_t> m_renewalSequences;
    std::vector<AuthId> m_renewalAuthIds;
    std::vector<AuthId> m_streamRequestAuthIds;
    std::vector<std::uint16_t> m_deauthOuterSequences;
    std::vector<std::uint16_t> m_loginTokenRequestIds;
    int m_audioFromClient = 0;
    quint32 m_announcedCiv = 0;
    quint32 m_announcedAudio = 0;
    quint32 m_announcedRate = 0;
    std::uint8_t m_announcedRxCodec = 0;
};

// Spin the event loop until `pred` holds or the deadline passes.
template <typename Pred>
bool waitFor(Pred pred, int timeoutMs = 4000)
{
    QElapsedTimer clock;
    clock.start();
    while (!pred() && clock.elapsed() < timeoutMs)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return pred();
}


}  // namespace AetherSDR::icom::test
