#include "core/backends/icom/IcomProtocol.h"

#include <algorithm>
#include <cstring>

namespace AetherSDR::icom {
namespace {

// ---- little / big endian helpers -----------------------------------------
//
// Both appear in the SAME packet — see buildSerialData, where the header
// sequence at 0x06 is little-endian and the serial send-sequence at 0x13 is
// big-endian. Naming them explicitly at every call site is the only way that
// stays readable.

void putLe16(std::span<std::uint8_t> b, std::size_t at, std::uint16_t v)
{
    b[at]     = static_cast<std::uint8_t>(v & 0xff);
    b[at + 1] = static_cast<std::uint8_t>((v >> 8) & 0xff);
}

void putLe32(std::span<std::uint8_t> b, std::size_t at, std::uint32_t v)
{
    b[at]     = static_cast<std::uint8_t>(v & 0xff);
    b[at + 1] = static_cast<std::uint8_t>((v >> 8) & 0xff);
    b[at + 2] = static_cast<std::uint8_t>((v >> 16) & 0xff);
    b[at + 3] = static_cast<std::uint8_t>((v >> 24) & 0xff);
}

void putBe16(std::span<std::uint8_t> b, std::size_t at, std::uint16_t v)
{
    b[at]     = static_cast<std::uint8_t>((v >> 8) & 0xff);
    b[at + 1] = static_cast<std::uint8_t>(v & 0xff);
}

void putBe32(std::span<std::uint8_t> b, std::size_t at, std::uint32_t v)
{
    b[at]     = static_cast<std::uint8_t>((v >> 24) & 0xff);
    b[at + 1] = static_cast<std::uint8_t>((v >> 16) & 0xff);
    b[at + 2] = static_cast<std::uint8_t>((v >> 8) & 0xff);
    b[at + 3] = static_cast<std::uint8_t>(v & 0xff);
}

std::uint16_t getLe16(std::span<const std::uint8_t> b, std::size_t at)
{
    return static_cast<std::uint16_t>(b[at] | (b[at + 1] << 8));
}

std::uint32_t getLe32(std::span<const std::uint8_t> b, std::size_t at)
{
    return static_cast<std::uint32_t>(b[at]) | (static_cast<std::uint32_t>(b[at + 1]) << 8)
         | (static_cast<std::uint32_t>(b[at + 2]) << 16)
         | (static_cast<std::uint32_t>(b[at + 3]) << 24);
}

std::uint16_t getBe16(std::span<const std::uint8_t> b, std::size_t at)
{
    return static_cast<std::uint16_t>((b[at] << 8) | b[at + 1]);
}

std::uint32_t getBe32(std::span<const std::uint8_t> b, std::size_t at)
{
    return (static_cast<std::uint32_t>(b[at]) << 24)
         | (static_cast<std::uint32_t>(b[at + 1]) << 16)
         | (static_cast<std::uint32_t>(b[at + 2]) << 8)
         | static_cast<std::uint32_t>(b[at + 3]);
}

// A fixed-size packet, zero-filled, with the common header already written.
std::vector<std::uint8_t> framed(std::size_t len, PacketType type, std::uint16_t seq,
                                 std::uint32_t localSid, std::uint32_t remoteSid)
{
    std::vector<std::uint8_t> p(len, 0);
    putLe32(p, 0x00, static_cast<std::uint32_t>(len));
    putLe16(p, 0x04, static_cast<std::uint16_t>(type));
    putLe16(p, 0x06, seq);
    putBe32(p, 0x08, localSid);
    putBe32(p, 0x0c, remoteSid);
    return p;
}

// The "inner" header shared by every control-stream request larger than 16
// bytes (login, auth, stream request). Offsets are relative to the packet.
//
// INNER SEQUENCE: a big-endian u16 at 0x16, matching wfview's packet struct and
// a live IC-7300MK2. kappanhang writes a little-endian u16 at 0x17; both shapes
// are byte-identical below 256, which hid the disagreement until a long-lived
// session crossed the 8-bit boundary. At 256 the shifted kappanhang shape was
// rejected with response 0xffffffff, and serial + audio expired 90 s after the
// last valid renewal while the outer control stream remained alive.
void writeInner(std::span<std::uint8_t> p, std::size_t payloadSize,
                std::uint8_t requestReply, std::uint8_t requestType,
                std::uint16_t innerSeq)
{
    putBe32(p, 0x10, static_cast<std::uint32_t>(payloadSize));
    p[0x14] = requestReply;
    p[0x15] = requestType;
    putBe16(p, 0x16, innerSeq);
}

// The 95-entry substitution table Icom uses to obfuscate credentials, indexed
// from ASCII 32. See encodePasscode() for what this is and is not.
constexpr std::array<std::uint8_t, 95> kPasscodeTable{
    0x47, 0x5d, 0x4c, 0x42, 0x66, 0x20, 0x23, 0x46, 0x4e, 0x57,  //  32..41
    0x45, 0x3d, 0x67, 0x76, 0x60, 0x41, 0x62, 0x39, 0x59, 0x2d,  //  42..51
    0x68, 0x7e, 0x7c, 0x65, 0x7d, 0x49, 0x29, 0x72, 0x73, 0x78,  //  52..61
    0x21, 0x6e, 0x5a, 0x5e, 0x4a, 0x3e, 0x71, 0x2c, 0x2a, 0x54,  //  62..71
    0x3c, 0x3a, 0x63, 0x4f, 0x43, 0x75, 0x27, 0x79, 0x5b, 0x35,  //  72..81
    0x70, 0x48, 0x6b, 0x56, 0x6f, 0x34, 0x32, 0x6c, 0x30, 0x61,  //  82..91
    0x6d, 0x7b, 0x2f, 0x4b, 0x64, 0x38, 0x2b, 0x2e, 0x50, 0x40,  //  92..101
    0x3f, 0x55, 0x33, 0x37, 0x25, 0x77, 0x24, 0x26, 0x74, 0x6a,  // 102..111
    0x28, 0x53, 0x4d, 0x69, 0x22, 0x5c, 0x44, 0x31, 0x36, 0x58,  // 112..121
    0x3b, 0x7a, 0x51, 0x5f, 0x52,                                 // 122..126
};

// Read a fixed-width, NUL-padded ASCII field.
std::string fixedString(std::span<const std::uint8_t> b, std::size_t at, std::size_t max)
{
    if (at >= b.size())
        return {};
    const std::size_t n = std::min(max, b.size() - at);
    std::string s;
    for (std::size_t i = 0; i < n; ++i) {
        if (b[at + i] == 0)
            break;
        s.push_back(static_cast<char>(b[at + i]));
    }
    return s;
}

bool startsWith(std::span<const std::uint8_t> pkt, std::size_t len, std::uint8_t lenByte)
{
    return pkt.size() == len && pkt[0] == lenByte && pkt[1] == 0 && pkt[2] == 0 && pkt[3] == 0
        && pkt[4] == 0 && pkt[5] == 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// Header
// ---------------------------------------------------------------------------

Header parseHeader(std::span<const std::uint8_t> pkt)
{
    Header h;
    if (pkt.size() < kHeaderSize)
        return h;
    h.len    = getLe32(pkt, 0x00);
    h.type   = getLe16(pkt, 0x04);
    h.seq    = getLe16(pkt, 0x06);
    h.sentId = getBe32(pkt, 0x08);
    h.rcvdId = getBe32(pkt, 0x0c);
    return h;
}

void writeHeader(std::span<std::uint8_t> out, const Header& h)
{
    if (out.size() < kHeaderSize)
        return;
    putLe32(out, 0x00, h.len);
    putLe16(out, 0x04, h.type);
    putLe16(out, 0x06, h.seq);
    putBe32(out, 0x08, h.sentId);
    putBe32(out, 0x0c, h.rcvdId);
}

std::uint32_t deriveLocalSessionId(std::uint32_t randomSeed, std::uint16_t localPort) noexcept
{
    // High half random, low half the port. Keeping the port in there is not
    // required by the protocol, but it makes two streams on the same host
    // trivially distinct even if the caller passes the same seed twice — which
    // is exactly the mistake that would otherwise produce two sessions the
    // radio cannot tell apart.
    return (randomSeed << 16) | localPort;
}

// ---------------------------------------------------------------------------
// Handshake
// ---------------------------------------------------------------------------

std::vector<std::uint8_t> buildAreYouThere(std::uint32_t localSid)
{
    return framed(kLenControl, PacketType::AreYouThere, 0, localSid, 0);
}

std::vector<std::uint8_t> buildAreYouReady(std::uint32_t localSid, std::uint32_t remoteSid)
{
    // seq is 1 here, not 0 — the radio matches its I-Am-Ready against it.
    return framed(kLenControl, PacketType::AreYouReady, 1, localSid, remoteSid);
}

std::vector<std::uint8_t> buildDisconnect(std::uint32_t localSid, std::uint32_t remoteSid)
{
    return framed(kLenControl, PacketType::Disconnect, 0, localSid, remoteSid);
}

std::vector<std::uint8_t> buildIdle(std::uint32_t localSid, std::uint32_t remoteSid,
                                    std::uint16_t seq)
{
    return framed(kLenControl, PacketType::Idle, seq, localSid, remoteSid);
}

bool parseIAmHere(std::span<const std::uint8_t> pkt, std::uint32_t& remoteSid)
{
    if (pkt.size() != kLenControl)
        return false;
    const Header h = parseHeader(pkt);
    if (h.type != static_cast<std::uint16_t>(PacketType::IAmHere))
        return false;
    remoteSid = h.sentId;   // the radio's own ID rides in ITS sentId field
    return true;
}

bool isIAmReady(std::span<const std::uint8_t> pkt)
{
    if (pkt.size() != kLenControl)
        return false;
    return parseHeader(pkt).type == static_cast<std::uint16_t>(PacketType::AreYouReady);
}

// ---------------------------------------------------------------------------
// Ping
// ---------------------------------------------------------------------------

bool isPing(std::span<const std::uint8_t> pkt)
{
    // The first byte is 0x15 from us and 0x00 from the radio — the radio does
    // not fill in its own length field on pings. So the length byte cannot be
    // part of the test, which is why this checks bytes 1..5 and the size.
    return pkt.size() == kLenPing && pkt[1] == 0x00 && pkt[2] == 0x00 && pkt[3] == 0x00
        && pkt[4] == 0x07 && pkt[5] == 0x00;
}

std::optional<Ping> parsePing(std::span<const std::uint8_t> pkt)
{
    if (!isPing(pkt))
        return std::nullopt;
    Ping p;
    p.seq     = getLe16(pkt, 0x06);
    p.isReply = pkt[0x10] != 0x00;
    std::copy_n(pkt.begin() + 0x11, 4, p.stamp.begin());
    return p;
}

std::vector<std::uint8_t> buildPingRequest(std::uint32_t localSid, std::uint32_t remoteSid,
                                           std::uint16_t seq,
                                           std::array<std::uint8_t, 4> stamp)
{
    auto p = framed(kLenPing, PacketType::Ping, seq, localSid, remoteSid);
    p[0x10] = 0x00;
    std::copy(stamp.begin(), stamp.end(), p.begin() + 0x11);
    return p;
}

std::vector<std::uint8_t> buildPingReply(std::uint32_t localSid, std::uint32_t remoteSid,
                                         const Ping& request)
{
    // The stamp is echoed VERBATIM. The radio matches its outstanding pings by
    // that field rather than by sequence number, so a reply that regenerates it
    // is silently ignored and the link is declared stale 15 s later.
    auto p = framed(kLenPing, PacketType::Ping, request.seq, localSid, remoteSid);
    p[0x10] = 0x01;
    std::copy(request.stamp.begin(), request.stamp.end(), p.begin() + 0x11);
    return p;
}

// ---------------------------------------------------------------------------
// Retransmission
// ---------------------------------------------------------------------------

int SeqRange::count() const noexcept
{
    return static_cast<int>(static_cast<std::uint16_t>(last - first)) + 1;
}

int compareSeq(std::uint16_t a, std::uint16_t b) noexcept
{
    if (a == b)
        return 0;
    // Wrapping distance: a difference of more than half the space means the
    // counter wrapped between them, so the "larger" raw value is actually the
    // older one. Getting this wrong makes a stream stall for 32 k packets every
    // time the sequence rolls over 0xFFFF.
    const std::uint16_t forward = static_cast<std::uint16_t>(b - a);
    return forward < 0x8000 ? -1 : 1;
}

std::vector<std::uint8_t> buildRetransmitRequest(std::uint32_t localSid, std::uint32_t remoteSid,
                                                 std::uint16_t seq)
{
    // The wanted sequence rides in the HEADER's own seq field. That is unusual
    // and it is why this cannot be built with a generic "payload" helper.
    return framed(kLenRetransmitOne, PacketType::Retransmit, seq, localSid, remoteSid);
}

std::vector<std::uint8_t> buildRetransmitRanges(std::uint32_t localSid, std::uint32_t remoteSid,
                                                std::span<const SeqRange> ranges)
{
    // The 24-byte form holds FOUR u16s — that is TWO ranges, not four. Both
    // reference implementations are loose here (kappanhang sizes the payload
    // for N ranges but always declares 0x18), and neither publishes a capture
    // of a multi-range request, so this form is UNVERIFIED against hardware.
    //
    // IcomStream deliberately prefers the single-sequence form above, looping
    // for a run: more datagrams, but provably correct. This exists so the shape
    // is available and tested once someone can confirm it on a real radio.
    auto p = framed(kLenRetransmitSet, PacketType::Retransmit, 0, localSid, remoteSid);
    const std::size_t n = std::min<std::size_t>(ranges.size(), 2);
    for (std::size_t i = 0; i < n; ++i) {
        putLe16(p, 0x10 + i * 4, ranges[i].first);
        putLe16(p, 0x12 + i * 4, ranges[i].last);
    }
    return p;
}

std::vector<SeqRange> parseRetransmitRequest(std::span<const std::uint8_t> pkt)
{
    std::vector<SeqRange> out;
    if (pkt.size() < kLenControl)
        return out;
    const Header h = parseHeader(pkt);
    if (h.type != static_cast<std::uint16_t>(PacketType::Retransmit))
        return out;

    if (pkt.size() == kLenRetransmitOne) {
        out.push_back({h.seq, h.seq});
        return out;
    }
    // Anything longer carries explicit ranges; take as many complete 4-byte
    // pairs as the datagram actually contains rather than trusting its length
    // field, because the radio's own length field is not reliable here.
    for (std::size_t at = 0x10; at + 4 <= pkt.size(); at += 4) {
        const std::uint16_t first = getLe16(pkt, at);
        const std::uint16_t last  = getLe16(pkt, at + 2);
        if (first == 0 && last == 0)
            continue;   // zero padding, not a range
        out.push_back({first, last});
    }
    return out;
}

// ---------------------------------------------------------------------------
// Credentials
// ---------------------------------------------------------------------------

std::array<std::uint8_t, 16> encodePasscode(std::string_view s)
{
    std::array<std::uint8_t, 16> out{};
    const std::size_t n = std::min<std::size_t>(s.size(), out.size());
    for (std::size_t i = 0; i < n; ++i) {
        int p = static_cast<unsigned char>(s[i]) + static_cast<int>(i);
        if (p > 126)
            p = 32 + p % 127;
        if (p < 32)
            continue;   // control characters have no table entry; leave the zero
        out[i] = kPasscodeTable[static_cast<std::size_t>(p - 32)];
    }
    return out;
}

// ---------------------------------------------------------------------------
// Login / auth / capabilities / stream request
// ---------------------------------------------------------------------------

std::vector<std::uint8_t> buildLogin(std::uint32_t localSid, std::uint32_t remoteSid,
                                     std::uint16_t innerSeq, std::uint16_t tokenRequestId,
                                     std::string_view username, std::string_view password)
{
    auto p = framed(kLenLogin, PacketType::Idle, 0, localSid, remoteSid);
    writeInner(p, kLenLogin - kHeaderSize, 0x01, 0x00, innerSeq);

    // Two bytes of our choosing. The auth ID the radio hands back echoes them
    // in its first two bytes, which is how a reply is matched to a login when
    // several are in flight. This must be fresh per login: both wfview and
    // kappanhang randomize it, and a live IC-7300MK2 rejected an immediate
    // reconnect when AetherSDR reused 0x0000 from the previous session.
    putLe16(p, 0x1a, tokenRequestId);

    const auto user = encodePasscode(username);
    const auto pass = encodePasscode(password);
    std::copy(user.begin(), user.end(), p.begin() + 0x40);
    std::copy(pass.begin(), pass.end(), p.begin() + 0x50);

    // The client name, in PLAIN TEXT, unlike the credentials above it. Both
    // reference implementations send exactly "icom-pc"; it is not known whether
    // the radio validates it, so we do not get creative.
    static constexpr char kClientName[] = "icom-pc";
    std::memcpy(p.data() + 0x60, kClientName, sizeof(kClientName) - 1);
    return p;
}

LoginResult parseLoginReply(std::span<const std::uint8_t> pkt, AuthId& authId)
{
    if (!startsWith(pkt, kLenLoginReply, 0x60))
        return LoginResult::NotALoginReply;
    // 0xFFFFFFFE in the error word is specifically "bad username/password", as
    // opposed to the 0xFFFFFF... prefix the status packet uses for a general
    // auth failure. Distinguishing them is what lets the UI say which one.
    if (pkt[0x30] == 0xff && pkt[0x31] == 0xff && pkt[0x32] == 0xff && pkt[0x33] == 0xfe)
        return LoginResult::BadCredentials;
    std::copy_n(pkt.begin() + 0x1a, authId.size(), authId.begin());
    return LoginResult::Ok;
}

std::vector<std::uint8_t> buildAuth(std::uint32_t localSid, std::uint32_t remoteSid,
                                    std::uint16_t innerSeq, const AuthId& authId, AuthKind kind)
{
    auto p = framed(kLenToken, PacketType::Idle, 0, localSid, remoteSid);
    writeInner(p, kLenToken - kHeaderSize, 0x01, static_cast<std::uint8_t>(kind), innerSeq);
    std::copy(authId.begin(), authId.end(), p.begin() + 0x1a);
    return p;
}

AuthReply parseAuthReply(std::span<const std::uint8_t> pkt)
{
    AuthReply out;
    if (!startsWith(pkt, kLenToken, 0x40)) {
        return out;
    }
    // 0x14 == 0x02 marks a REPLY (we send 0x01); 0x15 == 0x05 marks it as the
    // answer to a Renew, which is the one that actually gates the streams.
    if (pkt[0x14] != 0x02
        || pkt[0x15] != static_cast<std::uint8_t>(AuthKind::Renew)) {
        return out;
    }
    out.innerSeq = getBe16(pkt, 0x16);
    out.response = getLe32(pkt, 0x30);
    std::copy_n(pkt.begin() + 0x1a, out.authId.size(), out.authId.begin());
    out.result = out.response == 0 ? AuthReplyResult::Accepted
                                   : AuthReplyResult::Nonzero;
    return out;
}

bool parseCapabilities(std::span<const std::uint8_t> pkt, RadioId& radioId)
{
    if (!startsWith(pkt, kLenCapabilities, 0xa8))
        return false;
    // 0x42 is where the fixed 0x42-byte capabilities header ends and the first
    // 0x66-byte radio-capability record begins (0x42 + 0x66 == 0xA8).
    std::copy_n(pkt.begin() + 0x42, radioId.size(), radioId.begin());
    return true;
}

std::string parseCapabilitiesName(std::span<const std::uint8_t> pkt)
{
    if (!startsWith(pkt, kLenCapabilities, 0xa8))
        return {};
    // name[32] sits at record offset 0x10, so absolute 0x42 + 0x10 == 0x52.
    //
    // Worth parsing rather than hardcoding "IC-705": the stream request has to
    // name the radio it wants, and a hardcoded name is exactly what stops the
    // same backend reaching an IC-9700 or an RS-BA1 server fronting an IC-7300.
    return fixedString(pkt, 0x52, 32);
}

StatusKind parseStatus(std::span<const std::uint8_t> pkt)
{
    if (!startsWith(pkt, kLenStatus, 0x50))
        return StatusKind::None;
    // ORDER MATTERS. The same packet shape carries both outcomes and they are
    // distinguished by a three-byte prefix; testing the disconnect condition
    // first would classify an auth failure as a disconnect, because the
    // disconnect flag is not cleared on failure.
    if (pkt[0x30] == 0xff && pkt[0x31] == 0xff && pkt[0x32] == 0xff)
        return StatusKind::AuthFailed;
    if (pkt[0x30] == 0x00 && pkt[0x31] == 0x00 && pkt[0x32] == 0x00 && pkt[0x40] == 0x01)
        return StatusKind::Disconnected;
    return StatusKind::Ok;
}

std::vector<std::uint8_t> buildStreamRequest(const StreamRequest& req)
{
    auto p = framed(kLenConnInfo, PacketType::Idle, 0, req.localSid, req.remoteSid);
    writeInner(p, kLenConnInfo - kHeaderSize, 0x01, 0x03, req.innerSeq);

    std::copy(req.authId.begin(), req.authId.end(), p.begin() + 0x1a);
    std::copy(req.radioId.begin(), req.radioId.end(), p.begin() + 0x20);

    const std::size_t nameLen = std::min<std::size_t>(req.radioName.size(), 32);
    std::memcpy(p.data() + 0x40, req.radioName.data(), nameLen);

    const auto user = encodePasscode(req.username);
    std::copy(user.begin(), user.end(), p.begin() + 0x60);

    p[0x70] = 0x01;                                          // rxenable
    p[0x71] = req.enableTx ? 0x01 : 0x00;                    // txenable
    p[0x72] = static_cast<std::uint8_t>(req.rxCodec);
    p[0x73] = req.enableTx ? static_cast<std::uint8_t>(req.txCodec) : 0x00;
    putBe32(p, 0x74, req.sampleRateHz);                      // rxsample
    putBe32(p, 0x78, req.sampleRateHz);                      // txsample
    putBe32(p, 0x7c, req.civLocalPort);                      // our local CI-V port
    putBe32(p, 0x80, req.audioLocalPort);                    // our local audio port
    putBe32(p, 0x84, req.txBufferMs);                        // TX jitter buffer
    p[0x88] = 0x01;                                          // "convert"
    return p;
}

StreamGrant parseStreamGrant(std::span<const std::uint8_t> pkt)
{
    StreamGrant g;
    if (!startsWith(pkt, kLenConnInfo, 0x90))
        return g;
    if (pkt[0x60] != 0x01)
        return g;   // not the success echo

    g.granted    = true;
    g.deviceName = fixedString(pkt, 0x40, 32);
    // The header IDs are correlation data, not replacements for the current
    // control stream's IDs. A delayed grant from a previous session can arrive
    // on the new socket, so IcomSession validates both before adopting authId.
    g.remoteSid = getBe32(pkt, 0x08);
    g.localSid  = getBe32(pkt, 0x0c);
    std::copy_n(pkt.begin() + 0x1a, g.authId.size(), g.authId.begin());
    return g;
}

// ---------------------------------------------------------------------------
// Serial stream
// ---------------------------------------------------------------------------

std::vector<std::uint8_t> buildSerialOpen(std::uint32_t localSid, std::uint32_t remoteSid,
                                          std::uint16_t sendSeq, bool open)
{
    auto p = framed(kLenOpenClose, PacketType::Idle, 0, localSid, remoteSid);
    p[0x10] = 0xc0;
    p[0x11] = 0x01;
    p[0x12] = 0x00;
    putBe16(p, 0x13, sendSeq);
    p[0x15] = open ? 0x05 : 0x00;
    return p;
}

std::vector<std::uint8_t> buildSerialData(std::uint32_t localSid, std::uint32_t remoteSid,
                                          std::uint16_t headerSeq, std::uint16_t sendSeq,
                                          std::span<const std::uint8_t> civFrame)
{
    const std::size_t len = kSerialPayloadOffset + civFrame.size();
    auto p = framed(len, PacketType::Idle, headerSeq, localSid, remoteSid);
    p[0x10] = 0xc1;
    // The payload length is a LITTLE-endian u16 at 0x11, not a single byte.
    //
    // kappanhang writes (len, 0x00) and tests `pkt[0] - 0x15 == pkt[17]`, which
    // is byte-identical for payloads under 256 and is all it ever needs — it
    // relays commands and never decodes scope data. A SCOPE SWEEP is ~496 bytes,
    // so an 8-bit length silently truncates or rejects every panadapter frame.
    // wfview's packet struct has it as a u16 and wfview draws spectrum; that is
    // the tiebreak.
    putLe16(p, 0x11, static_cast<std::uint16_t>(civFrame.size()));
    putBe16(p, 0x13, sendSeq);   // BIG-endian, unlike the header seq at 0x06
    std::copy(civFrame.begin(), civFrame.end(), p.begin() + kSerialPayloadOffset);
    return p;
}

bool isSerialData(std::span<const std::uint8_t> pkt)
{
    if (pkt.size() < kSerialPayloadOffset + 1)
        return false;
    if (pkt[0x10] != 0xc1)
        return false;
    // The declared payload length MUST agree with the datagram size. A
    // disagreement means truncation or a spoofed packet, and accepting it feeds
    // garbage into the CI-V reassembler — which then resyncs on the next FE FE
    // it happens to find, potentially mid-frame.
    //
    // Read as a 16-bit field: an 8-bit read caps CI-V payloads at 234 bytes and
    // so rejects every scope sweep. See buildSerialData.
    return static_cast<std::size_t>(getLe16(pkt, 0x11)) == pkt.size() - kSerialPayloadOffset;
}

std::span<const std::uint8_t> serialPayload(std::span<const std::uint8_t> pkt)
{
    if (!isSerialData(pkt))
        return {};
    return pkt.subspan(kSerialPayloadOffset);
}

// ---------------------------------------------------------------------------
// Audio stream
// ---------------------------------------------------------------------------

std::vector<std::uint8_t> buildAudio(std::uint32_t localSid, std::uint32_t remoteSid,
                                     std::uint16_t headerSeq, std::uint16_t audioSeq,
                                     std::span<const std::uint8_t> pcm)
{
    const std::size_t len = kAudioPayloadOffset + pcm.size();
    auto p = framed(len, PacketType::Idle, headerSeq, localSid, remoteSid);
    putBe16(p, 0x10, 0x8000);                                       // ident
    putBe16(p, 0x12, audioSeq);                                     // BIG-endian
    putBe16(p, 0x16, static_cast<std::uint16_t>(pcm.size()));       // datalen, BIG-endian
    std::copy(pcm.begin(), pcm.end(), p.begin() + kAudioPayloadOffset);
    return p;
}

bool isAudioData(std::span<const std::uint8_t> pkt)
{
    if (pkt.size() <= kAudioPayloadOffset)
        return false;
    // Structural rather than a size whitelist. Both references test for exactly
    // 1388 or 580 bytes, which is correct only at 48 kHz — at 8 or 16 kHz the
    // split lands elsewhere and a size test would silently discard every audio
    // packet while everything else kept working.
    if (pkt[0x10] == 0xc0 || pkt[0x10] == 0xc1)
        return false;   // serial open/close or serial data
    const std::uint16_t declared = getBe16(pkt, 0x16);
    return static_cast<std::size_t>(declared) == pkt.size() - kAudioPayloadOffset;
}

std::span<const std::uint8_t> audioPayload(std::span<const std::uint8_t> pkt)
{
    if (!isAudioData(pkt))
        return {};
    return pkt.subspan(kAudioPayloadOffset);
}

}  // namespace AetherSDR::icom
