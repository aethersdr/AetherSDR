// IcomCIV Phase 0 — RS-BA1 transport wire test. Pins the packet shapes, the
// handshake, the ping stamp echo, the wrapping sequence comparison, the
// credential obfuscation table, and the serial/audio envelopes.
//
// Pure protocol: no sockets, no Qt, no hardware.

#include "core/backends/icom/IcomProtocol.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

using namespace AetherSDR::icom;

static int g_failures = 0;
static void check(bool cond, const char* what)
{
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

static std::uint16_t le16(const std::vector<std::uint8_t>& p, std::size_t at)
{
    return static_cast<std::uint16_t>(p[at] | (p[at + 1] << 8));
}
static std::uint32_t be32(const std::vector<std::uint8_t>& p, std::size_t at)
{
    return (static_cast<std::uint32_t>(p[at]) << 24) | (static_cast<std::uint32_t>(p[at + 1]) << 16)
         | (static_cast<std::uint32_t>(p[at + 2]) << 8) | p[at + 3];
}

static void testHeaderAndHandshake()
{
    const std::uint32_t kLocal = 0xDEADBEEF;
    const std::uint32_t kRemote = 0x12345678;

    auto ayt = buildAreYouThere(kLocal);
    check(ayt.size() == kLenControl, "are-you-there is 16 bytes");
    check(le16(ayt, 0x00) == kLenControl, "length field is little-endian");
    check(le16(ayt, 0x04) == static_cast<std::uint16_t>(PacketType::AreYouThere),
          "are-you-there type is 0x03");
    check(be32(ayt, 0x08) == kLocal, "our session id is in sentid");
    check(be32(ayt, 0x0c) == 0, "rcvdid is zero before we know the radio's id");

    // The radio's I-Am-Here carries ITS id in its OWN sentid field, which is
    // the one we then echo back as rcvdid.
    auto iah = buildAreYouThere(kRemote);
    iah[0x04] = static_cast<std::uint8_t>(PacketType::IAmHere);
    std::uint32_t got = 0;
    check(parseIAmHere(iah, got), "recognises i-am-here");
    check(got == kRemote, "extracts the radio's session id");

    auto ayr = buildAreYouReady(kLocal, kRemote);
    check(le16(ayr, 0x06) == 1, "are-you-ready carries seq 1");
    check(isIAmReady(ayr), "are-you-ready and i-am-ready share a type");

    auto hdr = parseHeader(ayr);
    check(hdr.sentId == kLocal && hdr.rcvdId == kRemote, "header round-trips both ids");
}

static void testPing()
{
    const std::array<std::uint8_t, 4> stamp{0x78, 0x40, 0xf6, 0x02};
    auto req = buildPingRequest(1, 2, 9, stamp);
    check(req.size() == kLenPing, "ping is 21 bytes");
    check(isPing(req), "recognises our own ping");
    check(req[0x10] == 0x00, "request marker is 0x00");

    // The radio does NOT fill in the length byte on its pings — byte 0 is 0x00
    // rather than 0x15 — so the recogniser must not depend on it.
    auto fromRadio = req;
    fromRadio[0] = 0x00;
    check(isPing(fromRadio), "recognises a radio ping with no length byte");

    auto parsed = parsePing(fromRadio);
    check(parsed.has_value(), "parses a ping");
    auto reply = buildPingReply(1, 2, *parsed);
    check(reply[0x10] == 0x01, "reply marker is 0x01");
    check(std::equal(stamp.begin(), stamp.end(), reply.begin() + 0x11),
          "the stamp is echoed VERBATIM — the radio matches on it, not on seq");
    check(le16(reply, 0x06) == 9, "reply carries the request's sequence");
}

static void testSeqWrap()
{
    check(compareSeq(5, 6) < 0, "5 precedes 6");
    check(compareSeq(6, 5) > 0, "6 follows 5");
    check(compareSeq(7, 7) == 0, "equal sequences compare equal");
    // The whole point: 0xFFFF precedes 0x0000. A naive integer compare says the
    // opposite and stalls the stream for 32k packets at every rollover.
    check(compareSeq(0xFFFF, 0x0000) < 0, "0xFFFF precedes 0x0000 across the wrap");
    check(compareSeq(0x0000, 0xFFFF) > 0, "0x0000 follows 0xFFFF across the wrap");
    check(compareSeq(0xFFFE, 0x0002) < 0, "a wrapping run stays ordered");

    SeqRange r{0xFFFE, 0x0001};
    check(r.count() == 4, "a wrapping range counts through the rollover");
}

static void testPasscode()
{
    // Values derived independently from the substitution table: the transform is
    // out[i] = table[(s[i] + i) - 32], with a wrap above 126.
    const auto beer = encodePasscode("beer");
    check(beer[0] == 0x2b, "passcode 'b' at index 0");
    check(beer[1] == 0x3f, "passcode 'e' at index 1");
    check(beer[2] == 0x55, "passcode 'e' at index 2 differs — it is POSITION dependent");
    check(beer[3] == 0x5c, "passcode 'r' at index 3");
    check(beer[4] == 0x00, "unused bytes are zero-padded");
    check(beer.size() == 16, "always exactly 16 bytes");

    // A 20-character password is truncated to the radio's own 16-byte limit
    // rather than overflowing.
    const auto longPw = encodePasscode("aaaaaaaaaaaaaaaaaaaa");
    check(longPw.size() == 16, "over-long credentials truncate, not overflow");
}

static void testLoginAndAuth()
{
    auto login = buildLogin(0x11111111, 0x22222222, 3, 0xBEEF, "beer", "beerbeer");
    check(login.size() == kLenLogin, "login is 128 bytes");
    check(login[0x14] == 0x01, "requestreply is 0x01");
    check(login[0x15] == 0x00, "login requesttype is 0x00");
    check(login[0x1a] == 0xef && login[0x1b] == 0xbe,
          "login carries the caller's little-endian token-request ID");
    check(std::memcmp(login.data() + 0x60, "icom-pc", 7) == 0,
          "the client name is PLAIN TEXT while the credentials are not");
    const auto user = encodePasscode("beer");
    check(std::equal(user.begin(), user.end(), login.begin() + 0x40), "username at 0x40");
    const auto pass = encodePasscode("beerbeer");
    check(std::equal(pass.begin(), pass.end(), login.begin() + 0x50), "password at 0x50");

    // A login reply carrying the bad-credentials sentinel.
    std::vector<std::uint8_t> reply(kLenLoginReply, 0);
    reply[0] = 0x60;
    reply[0x30] = 0xff; reply[0x31] = 0xff; reply[0x32] = 0xff; reply[0x33] = 0xfe;
    AuthId id{};
    check(parseLoginReply(reply, id) == LoginResult::BadCredentials,
          "0xFFFFFFFE is specifically bad username/password");

    // A successful one hands back the six auth bytes.
    std::vector<std::uint8_t> ok(kLenLoginReply, 0);
    ok[0] = 0x60;
    for (std::size_t i = 0; i < 6; ++i)
        ok[0x1a + i] = static_cast<std::uint8_t>(0xA0 + i);
    check(parseLoginReply(ok, id) == LoginResult::Ok, "a clean reply is accepted");
    check(id[0] == 0xA0 && id[5] == 0xA5, "auth id comes from 0x1a..0x1f");

    auto auth = buildAuth(1, 2, 4, id, AuthKind::Renew);
    check(auth.size() == kLenToken, "auth is 64 bytes");
    check(auth[0x15] == 0x05, "renew is requesttype 0x05");
    check(auth[0x16] == 0x00 && auth[0x17] == 0x04,
          "the inner sequence is a big-endian u16 at 0x16");
    check(std::equal(id.begin(), id.end(), auth.begin() + 0x1a), "auth id is echoed at 0x1a");

    const auto seq255 = buildAuth(1, 2, 0x00ff, id, AuthKind::Renew);
    const auto seq256 = buildAuth(1, 2, 0x0100, id, AuthKind::Renew);
    check(seq255[0x16] == 0x00 && seq255[0x17] == 0xff,
          "inner sequence 255 ends the one-byte-compatible range");
    check(seq256[0x16] == 0x01 && seq256[0x17] == 0x00 && seq256[0x18] == 0x00
              && std::equal(id.begin(), id.end(), seq256.begin() + 0x1a),
          "inner sequence 256 stays in its two-byte field and does not shift into auth id");

    auto accepted = auth;
    accepted[0x14] = 0x02;   // reply marker
    const AuthReply acceptedReply = parseAuthReply(accepted);
    check(acceptedReply.result == AuthReplyResult::Accepted,
          "a zero-response 0x02/0x05 reply gates the streams");
    check(acceptedReply.innerSeq == 4, "the accepted reply carries its correlation sequence");

    auto rejected = accepted;
    rejected[0x30] = rejected[0x31] = rejected[0x32] = rejected[0x33] = 0xff;
    const AuthReply rejectedReply = parseAuthReply(rejected);
    check(rejectedReply.result == AuthReplyResult::Nonzero,
          "0xffffffff is a nonzero reply whose meaning requires session context");
    check(rejectedReply.response == 0xffffffffU,
          "the rejected response word is preserved for diagnostics");

    auto diagnostic = accepted;
    diagnostic[0x30] = 0x78;
    diagnostic[0x31] = 0x56;
    diagnostic[0x32] = 0x34;
    diagnostic[0x33] = 0x12;
    check(parseAuthReply(diagnostic).response == 0x12345678U,
          "a non-palindromic response proves the diagnostic word is little-endian");

    auto firstAuthReply = accepted;
    firstAuthReply[0x15] = 0x02;
    check(parseAuthReply(firstAuthReply).result == AuthReplyResult::NotRenewal,
          "the FIRST auth's reply must not be mistaken for the token grant");
}

static void testStatusOrdering()
{
    // Both outcomes share a packet shape. Auth failure must win, because the
    // disconnect flag is not cleared when auth fails — testing disconnect first
    // misreports every bad password as "the radio disconnected".
    std::vector<std::uint8_t> failed(kLenStatus, 0);
    failed[0] = 0x50;
    failed[0x30] = 0xff; failed[0x31] = 0xff; failed[0x32] = 0xff;
    failed[0x40] = 0x01;   // disconnect flag ALSO set
    check(parseStatus(failed) == StatusKind::AuthFailed, "auth failure is tested first");

    std::vector<std::uint8_t> disc(kLenStatus, 0);
    disc[0] = 0x50;
    disc[0x40] = 0x01;
    check(parseStatus(disc) == StatusKind::Disconnected, "a clean disconnect is recognised");
}

static void testStreamRequestAndGrant()
{
    StreamRequest req;
    req.localSid = 0xAAAABBBB;
    req.remoteSid = 0xCCCCDDDD;
    req.innerSeq = 7;
    req.radioName = "IC-705";
    req.username = "beer";
    req.sampleRateHz = 48000;
    req.civLocalPort = 50002;
    req.audioLocalPort = 50003;
    req.txBufferMs = 200;

    auto p = buildStreamRequest(req);
    check(p.size() == kLenConnInfo, "stream request is 144 bytes");
    check(p[0x15] == 0x03, "stream request is requesttype 0x03");
    check(std::memcmp(p.data() + 0x40, "IC-705", 6) == 0, "radio name at 0x40");
    check(p[0x70] == 0x01 && p[0x71] == 0x01, "rx and tx enabled");
    check(p[0x72] == 4 && p[0x73] == 4, "codec 4 = LPCM 1ch 16-bit both ways");
    // Sample rate and ports are BIG-endian u32 in a header whose own fields are
    // little-endian. 48000 == 0x0000BB80.
    check(be32(p, 0x74) == 48000, "rx sample rate is big-endian");
    check(be32(p, 0x78) == 48000, "tx sample rate is big-endian");
    check(be32(p, 0x7c) == 50002, "our local CI-V port is announced");
    check(be32(p, 0x80) == 50003, "our local audio port is announced");
    check(be32(p, 0x84) == 200, "tx jitter buffer in ms");
    check(p[0x88] == 0x01, "convert flag");

    req.enableTx = false;
    auto rxOnly = buildStreamRequest(req);
    check(rxOnly[0x71] == 0x00 && rxOnly[0x73] == 0x00,
          "a receive-only request must not advertise a TX codec");

    // The grant, and the trap it carries.
    std::vector<std::uint8_t> grant(kLenConnInfo, 0);
    grant[0] = 0x90;
    // NEW session ids, different from the ones the login established.
    grant[0x08] = 0x99; grant[0x09] = 0x88; grant[0x0a] = 0x77; grant[0x0b] = 0x66;
    grant[0x0c] = 0x55; grant[0x0d] = 0x44; grant[0x0e] = 0x33; grant[0x0f] = 0x22;
    for (std::size_t i = 0; i < 6; ++i)
        grant[0x1a + i] = static_cast<std::uint8_t>(0xE0 + i);
    std::memcpy(grant.data() + 0x40, "IC-705", 6);
    grant[0x60] = 0x01;

    auto g = parseStreamGrant(grant);
    check(g.granted, "byte 0x60 == 1 means granted");
    check(g.deviceName == "IC-705", "device name is parsed, not assumed");
    check(g.remoteSid == 0x99887766, "the grant's session ids REPLACE the login's");
    check(g.localSid == 0x55443322, "both of them");
    check(g.authId[0] == 0xE0, "and so does the auth id");

    grant[0x60] = 0x00;
    check(!parseStreamGrant(grant).granted, "a non-success echo is not a grant");
}

static void testCapabilities()
{
    std::vector<std::uint8_t> caps(kLenCapabilities, 0);
    caps[0] = 0xa8;
    for (std::size_t i = 0; i < 16; ++i)
        caps[0x42 + i] = static_cast<std::uint8_t>(i + 1);
    std::memcpy(caps.data() + 0x52, "IC-705", 6);

    RadioId id{};
    check(parseCapabilities(caps, id), "recognises the capabilities packet");
    check(id[0] == 1 && id[15] == 16, "radio id is the 16 bytes at 0x42");
    check(parseCapabilitiesName(caps) == "IC-705",
          "the radio names itself — hardcoding IC-705 is what blocks other models");
}

static void testSerialEnvelope()
{
    const std::vector<std::uint8_t> civ{0xFE, 0xFE, 0xA4, 0xE0, 0x03, 0xFD};
    auto p = buildSerialData(0x11223344, 0x55667788, 0x1234, 0xABCD, civ);

    check(p.size() == kSerialPayloadOffset + civ.size(), "serial packet is 21 + payload");
    check(p[0] == static_cast<std::uint8_t>(0x15 + civ.size()), "length byte is 0x15 + payload");
    check(p[0x10] == 0xc1, "0xc1 marks serial data");
    check(p[0x11] == civ.size() && p[0x12] == 0x00,
          "payload length is a LITTLE-endian u16 at 0x11");
    // The two sequence numbers in ONE packet with DIFFERENT endianness. This is
    // the protocol, not a transcription error, and a test is the only place it
    // stays visible.
    check(le16(p, 0x06) == 0x1234, "header seq at 0x06 is LITTLE-endian");
    check(p[0x13] == 0xAB && p[0x14] == 0xCD, "serial send-seq at 0x13 is BIG-endian");

    check(isSerialData(p), "recognises its own serial packet");
    auto payload = serialPayload(p);
    check(payload.size() == civ.size() && std::equal(civ.begin(), civ.end(), payload.begin()),
          "payload round-trips");

    // A declared length that disagrees with the datagram must be REJECTED —
    // accepting it feeds garbage into the CI-V reassembler, which then resyncs
    // mid-frame on whatever FE FE it finds next.
    auto lying = p;
    lying[0x11] = 0x40;
    check(!isSerialData(lying), "a length field that disagrees with the datagram is rejected");

    // A SCOPE SWEEP is ~496 bytes and must survive the envelope. An 8-bit
    // length field caps CI-V payloads at 234 and silently rejects every
    // panadapter frame while commands keep working — a black waterfall on a
    // session that is demonstrably healthy.
    const std::vector<std::uint8_t> sweep(496, 0x33);
    auto big = buildSerialData(1, 2, 0, 0, sweep);
    check(big.size() == kSerialPayloadOffset + sweep.size(), "a 496-byte CI-V frame fits");
    check(big[0x11] == (496 & 0xff) && big[0x12] == ((496 >> 8) & 0xff),
          "and its length spans both bytes of the u16");
    check(isSerialData(big), "and it is recognised");
    check(serialPayload(big).size() == sweep.size(), "with the whole payload intact");

    auto open = buildSerialOpen(1, 2, 0, true);
    check(open.size() == kLenOpenClose && open[0x10] == 0xc0 && open[0x15] == 0x05,
          "serial open is 0xc0 with magic 0x05");
    auto close = buildSerialOpen(1, 2, 1, false);
    check(close[0x15] == 0x00, "serial close is magic 0x00");
}

static void testAudioEnvelope()
{
    std::vector<std::uint8_t> pcm(kAudioSplitLarge, 0x5A);
    auto p = buildAudio(1, 2, 0x0102, 0x0304, pcm);

    check(p.size() == kAudioPayloadOffset + kAudioSplitLarge, "large audio packet is 1388 bytes");
    check(p[0x10] == 0x80 && p[0x11] == 0x00, "ident is 0x8000");
    check(p[0x12] == 0x03 && p[0x13] == 0x04, "audio seq at 0x12 is BIG-endian");
    check(p[0x16] == ((kAudioSplitLarge >> 8) & 0xff) && p[0x17] == (kAudioSplitLarge & 0xff),
          "datalen at 0x16 is BIG-endian");

    check(isAudioData(p), "recognises audio");
    check(audioPayload(p).size() == kAudioSplitLarge, "payload starts at offset 24");

    std::vector<std::uint8_t> small(kAudioSplitSmall, 0x11);
    check(buildAudio(1, 2, 0, 1, small).size() == kAudioPayloadOffset + kAudioSplitSmall,
          "small audio packet is 580 bytes");

    // A serial packet must never be mistaken for audio — both ride type 0x00.
    const std::vector<std::uint8_t> civ{0xFE, 0xFE, 0xA4, 0xE0, 0x03, 0xFD};
    check(!isAudioData(buildSerialData(1, 2, 0, 0, civ)), "serial data is not audio");

    // The recogniser is structural, not a 1388/580 size whitelist: at 8 or
    // 16 kHz the split lands elsewhere and a whitelist would silently discard
    // every audio packet while the rest of the session kept working.
    std::vector<std::uint8_t> odd(999, 0x22);
    check(isAudioData(buildAudio(1, 2, 0, 1, odd)),
          "audio recognition is structural, not a size whitelist");
}

static void testRetransmit()
{
    auto one = buildRetransmitRequest(1, 2, 0x4242);
    check(one.size() == kLenRetransmitOne, "single retransmit request is 16 bytes");
    check(le16(one, 0x04) == static_cast<std::uint16_t>(PacketType::Retransmit), "type 0x01");
    check(le16(one, 0x06) == 0x4242,
          "the wanted sequence rides in the HEADER's own seq field");

    auto parsed = parseRetransmitRequest(one);
    check(parsed.size() == 1 && parsed[0].first == 0x4242 && parsed[0].last == 0x4242,
          "an inbound single request decodes to a one-packet range");

    const std::array<SeqRange, 2> ranges{SeqRange{10, 12}, SeqRange{20, 21}};
    auto many = buildRetransmitRanges(1, 2, ranges);
    check(many.size() == kLenRetransmitSet, "range form is 24 bytes — that is TWO ranges");
    auto back = parseRetransmitRequest(many);
    check(back.size() == 2 && back[0].first == 10 && back[0].last == 12 && back[1].first == 20,
          "range form round-trips");
}

int main()
{
    testHeaderAndHandshake();
    testPing();
    testSeqWrap();
    testPasscode();
    testLoginAndAuth();
    testStatusOrdering();
    testStreamRequestAndGrant();
    testCapabilities();
    testSerialEnvelope();
    testAudioEnvelope();
    testRetransmit();

    if (g_failures == 0)
        std::printf("icom_protocol_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
