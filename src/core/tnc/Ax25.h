#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>
#include <QtGlobal>

#include <optional>

// Low-level AX.25 v2.0 frame primitives: callsign/SSID addresses plus parse and
// build of a raw frame (address fields through the info field, WITHOUT the HDLC
// flags or the trailing 2-byte FCS — the same "no FCS" convention used by the
// KISS path and AetherAx25LibmodemShim::buildTransmitAudioFromFrame()).
//
// These are deliberately self-contained (no Qt::Network, no DSP) so they can be
// unit-tested standalone and reused by the PMS connected-mode data link, the
// beacon, and the future APRS/AX.25 digipeater. Reference: AX.25 v2.2 spec.

namespace AetherSDR::ax25 {

// A single AX.25 address: a 1-6 character callsign plus a 0-15 SSID.
struct Address {
    QString call;
    int ssid{0};
    // Address-field flag bits as they sit in the SSID octet. For the digipeater
    // path we must preserve the "has-been-repeated" (H) bit on via callsigns and
    // the command/response (C) bits on the dest/src addresses.
    bool hasBeenRepeated{false}; // H bit (only meaningful on via addresses)
    bool commandResponse{false}; // C bit (dest = command, src = response sense)

    bool isValid() const { return !call.isEmpty() && ssid >= 0 && ssid <= 15; }

    // "N0CALL" or "N0CALL-7"; SSID 0 is omitted.
    QString toString() const;
    bool operator==(const Address& other) const
    {
        return call.compare(other.call, Qt::CaseInsensitive) == 0 && ssid == other.ssid;
    }
    bool operator!=(const Address& other) const { return !(*this == other); }

    // Parse "N0CALL" / "N0CALL-7" (case-insensitive). Returns nullopt if the
    // base callsign is empty/too long or the SSID is out of range.
    static std::optional<Address> parse(const QString& text);
};

// The AX.25 frame category, derived from the control octet.
enum class FrameType {
    I,    // Information (connected-mode data)
    RR,   // Receive Ready (supervisory)
    RNR,  // Receive Not Ready (supervisory)
    REJ,  // Reject (supervisory)
    SABM, // Set Async Balanced Mode (connect request, mod-8)
    DISC, // Disconnect
    DM,   // Disconnected Mode (response: "I won't connect")
    UA,   // Unnumbered Acknowledge
    FRMR, // Frame Reject
    UI,   // Unnumbered Information (connectionless, e.g. APRS / beacons)
    Unknown,
};

// A decoded / to-be-encoded AX.25 frame (mod-8 sequence space).
struct Frame {
    Address dest;
    Address src;
    QVector<Address> via; // digipeater path (0-8 addresses)
    FrameType type{FrameType::Unknown};

    // Sequence numbers (valid for I and S frames). 0-7.
    int ns{0}; // send sequence  N(S)  (I frames only)
    int nr{0}; // receive sequence N(R) (I and S frames)

    bool pollFinal{false}; // P/F bit
    // True when this frame is a *command* (vs response). For mod-8 AX.25 v2.x the
    // command/response sense lives in the two address C bits; we surface it here.
    bool command{true};

    quint8 pid{0xF0}; // protocol id (I and UI frames). 0xF0 = no layer 3.
    QByteArray info;  // payload (I and UI frames)

    // Build the raw on-air frame (address..info, no FCS).
    QByteArray encode() const;

    // Parse a raw frame (address..info, no FCS). Returns nullopt if the address
    // fields or control octet are malformed.
    static std::optional<Frame> decode(const QByteArray& rawNoFcs);

    // Convenience constructors for the data-link state machine.
    static Frame makeU(const Address& dest, const Address& src, FrameType u,
                       bool pollFinal, bool command);
    static Frame makeS(const Address& dest, const Address& src, FrameType s,
                       int nr, bool pollFinal, bool command);
    static Frame makeI(const Address& dest, const Address& src, int ns, int nr,
                       bool pollFinal, const QByteArray& info, quint8 pid = 0xF0);
    static Frame makeUI(const Address& dest, const Address& src,
                        const QVector<Address>& via, const QByteArray& info,
                        quint8 pid = 0xF0);
};

QString frameTypeName(FrameType type);

} // namespace AetherSDR::ax25
