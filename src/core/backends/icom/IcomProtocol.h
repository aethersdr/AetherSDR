#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

// Icom RS-BA1 network transport wire primitives.
//
// An Icom networked radio is TWO protocols stacked, and keeping them apart is
// the single most important structural decision in this backend:
//
//   * CI-V is the COMMAND PLANE — a 1980s serial bus protocol, BCD-encoded,
//     documented by Icom in a per-model "CI-V Reference Guide". It lives in
//     CivCodec.h, not here.
//   * RS-BA1 is the TRANSPORT — three UDP streams, a session handshake, a
//     login, and a hand-rolled retransmission layer. Icom documents NOTHING
//     about it. That is what this file encodes.
//
// Over USB there is no transport at all: the CI-V bytes go straight down a
// serial port. So this file is the WiFi/Ethernet half and CivCodec.h is the
// half that is common to both.
//
// Grounded clean-room against Icom's own IC-705 CI-V Reference Guide (for the
// command plane) and, for this undocumented transport, against nonoo/kappanhang
// (MIT, portable — see THIRD_PARTY_LICENSES) with wfview (GPL-3) used ONLY as a
// read-only specification for field names and offsets, never as source.
// See ~/oracles/icom/icom-oracle.md §2 for the reasoning and the traps.
//
// Qt-free and socket-free on purpose, exactly like MetisProtocol.h: these are
// pure functions over byte buffers so icom_protocol_test can exercise every
// packet shape without linking Qt or opening a socket. IcomStream owns the
// UDP socket and calls into these.

namespace AetherSDR::icom {

// ---------------------------------------------------------------------------
// Ports and cadences
// ---------------------------------------------------------------------------

// Defaults. All three are user-changeable on the radio, so these seed the
// settings rather than being hardcoded at the call site.
inline constexpr std::uint16_t kControlPort = 50001;
inline constexpr std::uint16_t kSerialPort  = 50002;
inline constexpr std::uint16_t kAudioPort   = 50003;

// The token must be renewed or the radio stops the streams WITHOUT sending a
// disconnect. There is no error packet and no log line on the radio — audio
// simply stops. wfview renews at 60 s; kappanhang re-auths on a 1-minute
// ticker. 60 s is the observed contract, so renew comfortably inside it.
inline constexpr int kTokenRenewalMs   = 60'000;
// RENEW THREE TIMES INSIDE THE CONTRACT, not once.
//
// 45 s gave exactly one attempt before the 60 s expiry, and a renewal is a
// single UDP datagram. On a clean wired link that is fine. On the link this was
// found on — an IC-705 on 2.4 GHz WiFi 4 at -65 dBm with 14.7% TX retries and
// 802.11 power-save latency spiking to 300 ms — one shot is not enough, and
// losing it is silent: the radio's token expires, it stops honouring the
// session, and the transport keeps running so nothing above notices.
//
// 20 s gives three independent chances, and the ack tracking in IcomSession
// turns a lost one into an immediate resend rather than a dead session.
inline constexpr int kTokenRenewEarlyMs = 20'000;
// How long a renewal may go unacknowledged before we resend it. Sized off the
// observed worst-case round trip on a power-saving link, with margin.
inline constexpr int kTokenAckGraceMs   = 2'500;
// No auth acknowledgement for this long means the token is gone or about to be.
// Below the 60 s contract so the failure is reported while it is still true.
inline constexpr int kTokenDeadMs       = 50'000;

// Idle keepalive cadence. The radio drops a stream that goes quiet. kappanhang
// sends every 100 ms and relaxes to 1 s once nothing has been transmitted for
// a second; that relaxation is worth keeping — it is the difference between
// 10 and 1 wakeups a second on an idle link.
inline constexpr int kIdleIntervalMs      = 100;
inline constexpr int kIdleRelaxedAfterMs  = 1000;
inline constexpr int kIdleRelaxedIntervalMs = 1000;

// Ping cadence and staleness. The radio pings us roughly every 100 ms; we ping
// on our own clock because the ping round trip is the ONLY RTT measurement the
// protocol offers.
inline constexpr int kPingIntervalMs = 500;
inline constexpr int kStaleAfterMs   = 15'000;

// How long a received packet waits in the reorder buffer before it is delivered
// to the layer above. This is the retransmission budget: a gap noticed inside
// this window can still be repaired.
inline constexpr int kReorderHoldMs = 100;

// A single retransmit request covers at most this many packets. Past it the
// link is not having a glitch, it is broken, and asking for a hundred packets
// makes it worse.
inline constexpr int kMaxRetransmitRun = 10;

// ---------------------------------------------------------------------------
// Packet types
// ---------------------------------------------------------------------------

enum class PacketType : std::uint16_t {
    Idle        = 0x0000,   // keepalive; ALSO the carrier for serial + audio
    Retransmit  = 0x0001,
    AreYouThere = 0x0003,
    IAmHere     = 0x0004,
    Disconnect  = 0x0005,
    AreYouReady = 0x0006,   // and I-Am-Ready; same type both ways
    Ping        = 0x0007,
};

// Fixed packet lengths. The protocol dispatches larger payloads on LENGTH, not
// on type — every one of these rides type Idle (0x0000) — which is why these
// constants are load-bearing rather than documentation.
inline constexpr std::size_t kLenControl       = 0x10;   // 16
inline constexpr std::size_t kLenRetransmitOne = 0x10;   // 16
inline constexpr std::size_t kLenPing          = 0x15;   // 21
inline constexpr std::size_t kLenOpenClose     = 0x16;   // 22
inline constexpr std::size_t kLenRetransmitSet = 0x18;   // 24
inline constexpr std::size_t kLenAudioHeader   = 0x18;   // 24
inline constexpr std::size_t kLenToken         = 0x40;   // 64
inline constexpr std::size_t kLenStatus        = 0x50;   // 80
inline constexpr std::size_t kLenLoginReply    = 0x60;   // 96
inline constexpr std::size_t kLenLogin         = 0x80;   // 128
inline constexpr std::size_t kLenConnInfo      = 0x90;   // 144
inline constexpr std::size_t kLenCapabilities  = 0xA8;   // 168

// The common 16-byte header every packet on every stream starts with.
inline constexpr std::size_t kHeaderSize = 16;

// Serial-stream payloads start here (16-byte header + 5-byte serial subheader).
inline constexpr std::size_t kSerialPayloadOffset = 21;
// Audio payloads start after the 24-byte audio header.
inline constexpr std::size_t kAudioPayloadOffset = 24;

// ---------------------------------------------------------------------------
// Header
// ---------------------------------------------------------------------------

struct Header {
    std::uint32_t len    = 0;
    std::uint16_t type   = 0;
    std::uint16_t seq    = 0;
    std::uint32_t sentId = 0;
    std::uint32_t rcvdId = 0;
};

// ENDIANNESS, stated once so nobody has to re-derive it:
//
//   len, type, seq        LITTLE-endian
//   sentId, rcvdId        we write BIG-endian
//
// The session IDs are OPAQUE TOKENS. The radio echoes back whatever we sent and
// never interprets them, so the choice does not affect interoperability —
// kappanhang treats them big-endian and wfview reads them native. What DOES
// matter is that we are self-consistent, because we compare the rcvdId the
// radio echoed against the sentId we generated. Hence one pair of accessors
// used everywhere rather than ad-hoc shifts at each call site.
[[nodiscard]] Header parseHeader(std::span<const std::uint8_t> pkt);
void writeHeader(std::span<std::uint8_t> out, const Header& h);

// ---------------------------------------------------------------------------
// Session identity
// ---------------------------------------------------------------------------

// Our end of the session. kappanhang derives this from the socket
// (local IPv4 << 16 | local port, after truncation: the low two octets of the
// address in the high half and the port in the low half). We do NOT copy that.
//
// It is a convenience there and a small information leak here — it puts the
// host's LAN address into a payload that also carries an obfuscated password.
// Any value unique per stream works, so callers pass a random one.
[[nodiscard]] std::uint32_t deriveLocalSessionId(std::uint32_t randomSeed,
                                                 std::uint16_t localPort) noexcept;

// ---------------------------------------------------------------------------
// Handshake
// ---------------------------------------------------------------------------
//
// Per stream, in order:
//     -> AreYouThere      (TWICE)
//     <- IAmHere          carries the radio's session ID in bytes 8..11
//     -> AreYouReady      (TWICE)
//     <- IAmReady
//
// EVERY handshake packet is sent TWICE. This is not paranoia — the IC-705's
// WiFi stack drops the first packet of a burst often enough that both reference
// implementations do it unconditionally. sendTwice() in IcomStream is where
// that happens; these builders return one copy.

[[nodiscard]] std::vector<std::uint8_t> buildAreYouThere(std::uint32_t localSid);
[[nodiscard]] std::vector<std::uint8_t> buildAreYouReady(std::uint32_t localSid,
                                                         std::uint32_t remoteSid);
[[nodiscard]] std::vector<std::uint8_t> buildDisconnect(std::uint32_t localSid,
                                                        std::uint32_t remoteSid);
[[nodiscard]] std::vector<std::uint8_t> buildIdle(std::uint32_t localSid,
                                                  std::uint32_t remoteSid,
                                                  std::uint16_t seq);

// True when this datagram is the IAmHere reply; on success `remoteSid` receives
// the radio's session ID.
[[nodiscard]] bool parseIAmHere(std::span<const std::uint8_t> pkt,
                                std::uint32_t& remoteSid);
[[nodiscard]] bool isIAmReady(std::span<const std::uint8_t> pkt);

// ---------------------------------------------------------------------------
// Ping (type 0x07, 21 bytes)
// ---------------------------------------------------------------------------
//
// Byte 0x10 is 0x00 for a request and 0x01 for a reply. Bytes 0x11..0x14 carry
// the sender's uptime and MUST be echoed verbatim when replying — the radio
// matches its own pings by that field, not by sequence number.

struct Ping {
    std::uint16_t seq = 0;
    bool isReply = false;
    std::array<std::uint8_t, 4> stamp{};
};

[[nodiscard]] bool isPing(std::span<const std::uint8_t> pkt);
[[nodiscard]] std::optional<Ping> parsePing(std::span<const std::uint8_t> pkt);
[[nodiscard]] std::vector<std::uint8_t> buildPingRequest(std::uint32_t localSid,
                                                         std::uint32_t remoteSid,
                                                         std::uint16_t seq,
                                                         std::array<std::uint8_t, 4> stamp);
[[nodiscard]] std::vector<std::uint8_t> buildPingReply(std::uint32_t localSid,
                                                        std::uint32_t remoteSid,
                                                        const Ping& request);

// ---------------------------------------------------------------------------
// Retransmission
// ---------------------------------------------------------------------------

// A closed sequence range, inclusive, in a space that WRAPS at 0xFFFF.
struct SeqRange {
    std::uint16_t first = 0;
    std::uint16_t last  = 0;
    [[nodiscard]] int count() const noexcept;
};

[[nodiscard]] std::vector<std::uint8_t> buildRetransmitRequest(std::uint32_t localSid,
                                                                std::uint32_t remoteSid,
                                                                std::uint16_t seq);
// Up to four ranges fit the 24-byte form; callers must not exceed that.
[[nodiscard]] std::vector<std::uint8_t> buildRetransmitRanges(std::uint32_t localSid,
                                                               std::uint32_t remoteSid,
                                                               std::span<const SeqRange> ranges);
// Decode an INBOUND retransmit request — the radio asking US to replay.
// Returns the ranges it wants; empty when this is not a retransmit request.
[[nodiscard]] std::vector<SeqRange> parseRetransmitRequest(std::span<const std::uint8_t> pkt);

// Wrapping-aware comparison. Returns <0, 0, >0 for a before/equal/after b,
// treating a difference of more than half the space as a wrap.
[[nodiscard]] int compareSeq(std::uint16_t a, std::uint16_t b) noexcept;

// ---------------------------------------------------------------------------
// Credentials — obfuscation, NOT encryption
// ---------------------------------------------------------------------------

// Icom passes usernames and passwords through a fixed 95-entry substitution
// table. It is a position-dependent monoalphabetic substitution and it is
// TRIVIALLY REVERSIBLE: anyone on the LAN with a packet capture has the
// operator's radio password.
//
// This is stated here, in the code, because it is a fact the UI has to be
// honest about rather than an implementation detail. There is no alternative —
// the radio's firmware accepts only this scheme — so the mitigation is entirely
// about where WE store the credential (keychain, never the settings XML) and
// about telling the operator.
//
// Always returns exactly 16 bytes, zero-padded. Input beyond 16 characters is
// truncated, which is the radio's own limit.
[[nodiscard]] std::array<std::uint8_t, 16> encodePasscode(std::string_view s);

// ---------------------------------------------------------------------------
// Login / auth / capabilities / stream request  (control stream only)
// ---------------------------------------------------------------------------

// Inner request types carried at byte 0x15 of the 0x40/0x50/0x90 packets.
enum class AuthKind : std::uint8_t {
    Deauth   = 0x01,
    First    = 0x02,   // first auth, immediately after login
    Renew    = 0x05,   // token request and 60 s renewal
};

// The six bytes at 0x1a..0x1f. kappanhang calls this the "auth ID" and treats
// it as one opaque blob; wfview splits it into tokrequest(u16) + token(u32)
// because it also GENERATES a token request. They are the same six bytes.
//
// Opaque is the right model for a client: we copy it out of the login reply and
// echo it into every later packet, and we never need to interpret either half.
using AuthId = std::array<std::uint8_t, 6>;

// The 16 bytes at offset 0x42 of the capabilities packet — where the fixed
// 0x42-byte capabilities header ends and the first 0x66-byte radio-capability
// record begins (0x42 + 0x66 = 0xA8). kappanhang calls it a8replyID; wfview
// calls it radio_cap_packet.guid. It identifies WHICH radio we want when a
// server fronts several, and it is echoed back at offset 0x20 of the stream
// request.
using RadioId = std::array<std::uint8_t, 16>;

[[nodiscard]] std::vector<std::uint8_t> buildLogin(std::uint32_t localSid,
                                                    std::uint32_t remoteSid,
                                                    std::uint16_t innerSeq,
                                                    std::uint16_t tokenRequestId,
                                                    std::string_view username,
                                                    std::string_view password);

// Outcome of the 0x60 login reply.
enum class LoginResult { Ok, BadCredentials, NotALoginReply };
[[nodiscard]] LoginResult parseLoginReply(std::span<const std::uint8_t> pkt, AuthId& authId);

[[nodiscard]] std::vector<std::uint8_t> buildAuth(std::uint32_t localSid,
                                                   std::uint32_t remoteSid,
                                                   std::uint16_t innerSeq,
                                                   const AuthId& authId,
                                                   AuthKind kind);

// A 0x40 requesttype=0x05 packet is only an ordinary successful renewal when
// its response word is zero. The radio also sends the same reply shape with
// 0xffffffff: during initial reconnect authentication that supplies the token
// wfview continues with, while during an established lease it is a rejection.
// The protocol parser preserves the distinction and IcomSession applies the
// required session context.
enum class AuthReplyResult { NotRenewal, Accepted, Nonzero };
struct AuthReply {
    AuthReplyResult result = AuthReplyResult::NotRenewal;
    std::uint16_t innerSeq = 0;
    std::uint32_t response = 0;
    AuthId authId{};
};
[[nodiscard]] AuthReply parseAuthReply(std::span<const std::uint8_t> pkt);

// Extract the radio identity from the 0xA8 capabilities packet.
[[nodiscard]] bool parseCapabilities(std::span<const std::uint8_t> pkt, RadioId& radioId);

// The radio's own name ("IC-705") from the same packet. Parsed rather than
// hardcoded: the stream request has to name the radio it wants, and a literal
// there is exactly what stops this backend reaching an IC-9700 or an RS-BA1
// server fronting an IC-7300.
[[nodiscard]] std::string parseCapabilitiesName(std::span<const std::uint8_t> pkt);

// What the 0x50 status packet is telling us. The radio uses one packet shape
// for "your auth failed" and "you have been disconnected", distinguished by
// bytes we have to test in the right order.
enum class StatusKind { None, AuthFailed, Disconnected, Ok };
[[nodiscard]] StatusKind parseStatus(std::span<const std::uint8_t> pkt);

// Audio codec identifiers, as negotiated in the stream request. Values are a
// bit-per-format enumeration from wfview's published list; only the ones we
// actually implement are named here rather than transcribing the whole table.
enum class AudioCodec : std::uint8_t {
    ULaw1ch8   = 1,
    Lpcm1ch8   = 2,
    Lpcm1ch16  = 4,     // what we use — no decoder needed
    Lpcm2ch16  = 16,
    Opus1ch    = 64,
    Adpcm1ch   = 128,
};

struct StreamRequest {
    std::uint32_t localSid  = 0;
    std::uint32_t remoteSid = 0;
    std::uint16_t innerSeq  = 0;
    AuthId  authId{};
    RadioId radioId{};
    std::string radioName;          // e.g. "IC-705", from the capabilities packet
    std::string username;
    AudioCodec rxCodec = AudioCodec::Lpcm1ch16;
    AudioCodec txCodec = AudioCodec::Lpcm1ch16;
    std::uint32_t sampleRateHz = 48000;
    std::uint16_t civLocalPort   = kSerialPort;
    std::uint16_t audioLocalPort = kAudioPort;
    // 300 ms, which is kappanhang's txSeqBufLength — the value a byte-exact
    // IC-705 client negotiates. Ours was 200 ms for no recorded reason. This is
    // the radio's OWN jitter buffer for the audio we send it, so on a link that
    // delivers in bursts (WiFi power-save can stall 300 ms at a time) an
    // undersized one drops audio that arrived only slightly late.
    std::uint16_t txBufferMs = 300;
    bool enableTx = true;
};

[[nodiscard]] std::vector<std::uint8_t> buildStreamRequest(const StreamRequest& req);

// The 0x90 reply. Its header IDs identify the control session the grant belongs
// to; callers must reject a delayed grant from a previous session. The auth ID
// in a valid current-session grant replaces the login value for renewals.
struct StreamGrant {
    bool granted = false;
    std::string deviceName;
    std::uint32_t remoteSid = 0;
    std::uint32_t localSid  = 0;
    AuthId authId{};
};
[[nodiscard]] StreamGrant parseStreamGrant(std::span<const std::uint8_t> pkt);

// ---------------------------------------------------------------------------
// Serial stream (CI-V transport)
// ---------------------------------------------------------------------------

// Open/close the CI-V pipe. magic 0x05 = open, 0x00 = close.
[[nodiscard]] std::vector<std::uint8_t> buildSerialOpen(std::uint32_t localSid,
                                                         std::uint32_t remoteSid,
                                                         std::uint16_t sendSeq,
                                                         bool open);

// Wrap one raw CI-V frame for the serial stream.
//
// NOTE the two sequence numbers with DIFFERENT endianness in the same packet:
// the header seq at 0x06 is little-endian and the serial send-seq at 0x13 is
// big-endian. That is the protocol, not a transcription error.
[[nodiscard]] std::vector<std::uint8_t> buildSerialData(std::uint32_t localSid,
                                                         std::uint32_t remoteSid,
                                                         std::uint16_t headerSeq,
                                                         std::uint16_t sendSeq,
                                                         std::span<const std::uint8_t> civFrame);

// True when this datagram carries serial payload. The test is structural: byte
// 0x10 must be 0xc1 AND the declared payload length at 0x11 must agree with the
// packet length. A length field that disagrees means a truncated or spoofed
// datagram, and accepting it feeds garbage into the CI-V reassembler.
[[nodiscard]] bool isSerialData(std::span<const std::uint8_t> pkt);
// The CI-V bytes inside a serial datagram. Empty if this is not one.
[[nodiscard]] std::span<const std::uint8_t> serialPayload(std::span<const std::uint8_t> pkt);

// ---------------------------------------------------------------------------
// Audio stream
// ---------------------------------------------------------------------------

// A 20 ms frame at 48 kHz mono s16 is 1920 bytes and does not fit one
// comfortable datagram alongside the header, so it is SPLIT across two packets
// of unequal size. Both directions use this pair.
//
// Do NOT derive per-packet timing from packet size: these represent 14.2 ms and
// 5.8 ms, and kappanhang's "10 ms per packet" is an average across the pair.
// Reconstruct by concatenating payloads in sequence order and let the audio
// device clock it.
// THE FRAME IS A DURATION, and its byte count is derived from it.
//
// It used to be the constant 1920 with the split pair defining it, which is
// only 20 ms at 48 kHz mono s16 — the rate and the sample width were baked into
// a number that looked like a protocol constant. Changing the rate to 16 kHz
// while leaving 1920 in place produced 60 ms frames; the radio's jitter buffer
// read them as discontinuities and discarded every one, giving a keyed
// transmitter with zero forward power and nothing on the air.
//
// kappanhang derives it the same way:
//   audioFrameSize = (audioSampleRate * audioSampleBytes * audioFrameLength) / 1s
inline constexpr int         kAudioFrameMs      = 20;
inline constexpr int         kAudioSampleBytes  = 2;    // s16, mono
inline constexpr std::uint32_t kAudioRateHz     = 48000;
inline constexpr std::size_t kAudioFrameBytes =
    static_cast<std::size_t>(kAudioRateHz) * kAudioSampleBytes * kAudioFrameMs / 1000;  // 1920

// The 1364/556 pair is MTU fragmentation of that frame, not a protocol rule:
// 1920 bytes does not fit one comfortable datagram alongside the 24-byte
// header. kappanhang splits at exactly these offsets and so do we. The
// static_assert is the guard the old constants could not provide — if the frame
// duration or the rate ever moves, this stops the build instead of silently
// shipping frames the radio will drop.
inline constexpr std::size_t kAudioSplitLarge = 1364;
inline constexpr std::size_t kAudioSplitSmall = kAudioFrameBytes - kAudioSplitLarge;  // 556
static_assert(kAudioFrameBytes == 1920,
              "The 1364/556 split is sized for a 20 ms frame at 48 kHz mono s16. "
              "Changing the rate or the frame duration requires re-deriving the "
              "packet split — see IcomAudio's TxPacketizer.");

[[nodiscard]] std::vector<std::uint8_t> buildAudio(std::uint32_t localSid,
                                                    std::uint32_t remoteSid,
                                                    std::uint16_t headerSeq,
                                                    std::uint16_t audioSeq,
                                                    std::span<const std::uint8_t> pcm);

[[nodiscard]] bool isAudioData(std::span<const std::uint8_t> pkt);
[[nodiscard]] std::span<const std::uint8_t> audioPayload(std::span<const std::uint8_t> pkt);

}  // namespace AetherSDR::icom
