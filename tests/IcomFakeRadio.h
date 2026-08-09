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
#include <map>
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
            auto reply = s.frame(0x60, 0x00, m_seq++);
            reply[0x14] = 0x02;
            for (std::size_t i = 0; i < 6; ++i)
                reply[0x1a + i] = static_cast<std::uint8_t>(0xC0 + i);
            s.send(reply);
            return;
        }
        case 0x40: {   // auth
            const auto kind = static_cast<std::uint8_t>(b[0x15]);
            ++m_authCount;
            if (kind != 0x05)
                return;   // the FIRST auth gets no reply; only the token request does
            auto reply = s.frame(0x40, 0x00, m_seq++);
            reply[0x14] = 0x02;
            reply[0x15] = 0x05;
            s.send(reply);

            if (!m_sentCaps) {
                m_sentCaps = true;
                auto caps = s.frame(0xA8, 0x00, m_seq++);
                for (std::size_t i = 0; i < 16; ++i)
                    caps[0x42 + i] = static_cast<std::uint8_t>(0x40 + i);
                const char* name = "IC-705";
                std::copy(name, name + 6, caps.begin() + 0x52);
                s.send(caps);
            }
            return;
        }
        case 0x90: {   // stream request
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

            auto grant = s.frame(0x90, 0x00, m_seq++);
            const char* name = "IC-705";
            std::copy(name, name + 6, grant.begin() + 0x40);
            for (std::size_t i = 0; i < 6; ++i)
                grant[0x1a + i] = static_cast<std::uint8_t>(0xD0 + i);
            grant[0x60] = 0x01;
            s.send(grant);
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

        // Name itself. A real radio answers 0x19 0x00 with its own CI-V
        // address, and that is how a client learns WHICH Icom it is talking to
        // — several models speak this same transport, and the address is
        // user-changeable, so a client that assumes 0xA4 mis-decodes the rest.
        if (frame->cmd == cmd::kReadId) {
            pushCiv({0xFE, 0xFE, kControllerAddress, kIc705Addr, cmd::kReadId, 0x00,
                     kIc705Addr, kCivEom});
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
        if (frame->cmd == cmd::kControl && frame->hasSub
            && frame->sub == control::kTuner && frame->data.empty()) {
            pushCiv({0xFE, 0xFE, kControllerAddress, kIc705Addr, cmd::kControl,
                     control::kTuner, m_tunerState, kCivEom});
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
    int  m_ritHz = -1230;
    bool m_ritOn = true;
    bool m_xitOn = false;
    std::uint8_t m_tunerState = 0x01;   // matched

private:

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
    std::uint8_t  m_civSeq = 0;
    std::vector<CivFrame> m_civLog;
    bool m_sentCaps = false;
    bool m_serialOpened = false;
    bool m_usernameObfuscated = false;
    int m_authCount = 0;
    int m_civCommands = 0;
    bool m_civSilent = false;
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
