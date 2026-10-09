/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Jeremy Fielder (KK7GWY)
 *
 * CTR2 USB link, wire format version 0 -- portable reference implementation
 * for the controller firmware. C99, no heap, depends only on <stdint.h> and
 * <stddef.h>. MIT-licensed (unlike the rest of AetherSDR) so it can be copied
 * into closed firmware. Specification: docs/ctr2-usb-relay-design.md.
 *
 * A "report" is the 8 bytes after HID report ID 0x01.
 *
 *   Header: [0xFF][version 0x00][counter][type][packets hi][packets lo]
 *           [length hi][length lo]              packets includes the header
 *   Data:   [counter][7 payload bytes, zero-padded after the last byte]
 *
 * Message types: DATA 0x00 (radio TCP bytes), HELLO 0x01 (host: calling;
 * device: restart), READY 0x02 (device: answer; host: radio connected),
 * CLOSED 0x03 (either direction),
 * DATAGRAM 0x04 (one UDP datagram: [radio port hi][radio port lo][bytes]).
 * Counters are 0x00-0x7F and wrap 0x7F -> 0x00.
 *
 * Optional extensions (types 0x40-0x5F) are negotiated outside the link,
 * through HID Feature report 0x02, and are never sent to a device that did
 * not offer them. A device without that Feature report is plain version 0.
 */
#ifndef CTR2_LINK_H
#define CTR2_LINK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CTR2_REPORT_ID          0x01u
#define CTR2_REPORT_BYTES       8u
#define CTR2_DATA_PER_REPORT    7u
#define CTR2_MARKER             0xFFu
#define CTR2_VERSION            0x00u
#define CTR2_COUNTER_MASK       0x7Fu
#define CTR2_MAX_PAYLOAD        512u    /* DATA */
#define CTR2_MAX_DATAGRAM       1472u   /* DATAGRAM, excluding its 2-byte port */
#define CTR2_MAX_MESSAGE        (2u + CTR2_MAX_DATAGRAM)

#define CTR2_TYPE_DATA          0x00u
#define CTR2_TYPE_HELLO         0x01u
#define CTR2_TYPE_READY         0x02u
#define CTR2_TYPE_CLOSED        0x03u
#define CTR2_TYPE_DATAGRAM      0x04u

/* ---- Extensions ------------------------------------------------------- */

/* Extension message types: 0x40-0x5F, one capability bit each. They carry
 * 1..ctr2_ext_max_length(type) bytes and are sequenced like DATA. */
#define CTR2_EXT_FIRST          0x40u
#define CTR2_EXT_LAST           0x5Fu
#define CTR2_CAP(type)          (1ul << ((type) - CTR2_EXT_FIRST))

/* AUDIO_SPECTRUM, host -> device: what the operator is hearing, as bars.
 * Payload: [bar count N, 1..64][span Hz hi][span Hz lo][N levels], each
 * level 0..255 linear in dB from CTR2_SPECTRUM_FLOOR_DB (0) to 0 dBFS (255).
 * Bars split 0..span Hz evenly. Sent at most ~20 times a second. */
#define CTR2_EXT_AUDIO_SPECTRUM 0x40u
#define CTR2_SPECTRUM_MAX_BARS  64u
#define CTR2_SPECTRUM_FLOOR_DB  (-90)

/* Largest payload each extension type may carry; 0 for a type not defined
 * yet. A longer message is a framing error (CTR2_ERR_BAD_LENGTH). */
uint16_t ctr2_ext_max_length(uint8_t type);

/* Capability negotiation: HID Feature report CTR2_FEATURE_REPORT_ID,
 * CTR2_FEATURE_BYTES after the report ID:
 *   ['C']['X'][extension version 0x01][0x00][capabilities, 32 bits MSB first]
 * Exactly CTR2_FEATURE_BYTES (8) bytes follow the report ID; 9 in all.
 * Get: the device's offer. Set: the subset the host will use, which the
 * device may send and should expect; it lasts until the device leaves the
 * bus. A device answering Get with anything else, or not at all, offers
 * nothing. Extension messages share the version 0 counter sequence with
 * DATA and DATAGRAM: one counter, one state machine. */
#define CTR2_FEATURE_REPORT_ID  0x02u
#define CTR2_FEATURE_BYTES      8u
#define CTR2_EXT_VERSION        0x01u

void ctr2_caps_encode(uint32_t caps, uint8_t out[CTR2_FEATURE_BYTES]);
/* Returns 1 and the capabilities for a well-formed report, else 0. */
int ctr2_caps_decode(const uint8_t *in, size_t len, uint32_t *caps);

/* Called once per outgoing 8-byte report, in order. Prepend report ID 0x01
 * if your USB stack needs it in the buffer. */
typedef void (*ctr2_report_sink)(void *ctx, const uint8_t report[CTR2_REPORT_BYTES]);

/* ---- Sending ---------------------------------------------------------- */

typedef struct {
    uint8_t counter;
} ctr2_tx;

/* Call before sending HELLO or READY: each resets the counter to 0. */
void ctr2_tx_reset(ctr2_tx *tx);

/* Sends one message: the header report, then ceil(len / 7) data reports.
 * DATA needs 1..CTR2_MAX_PAYLOAD bytes; HELLO/READY/CLOSED take len 0.
 * Returns the number of reports sent, or 0 if the arguments are invalid.
 * Longer DATA must be split by the caller into several messages. */
size_t ctr2_tx_send(ctr2_tx *tx, uint8_t type, const uint8_t *payload, uint16_t len,
                    ctr2_report_sink sink, void *ctx);

/* Sends one UDP datagram (1..CTR2_MAX_DATAGRAM bytes) for the radio's UDP
 * port `port` (4992 for the radio's VITA-49 port). The host sends it to the
 * radio from a socket of its own, so the radio's replies come back as
 * DATAGRAM messages. Returns the number of reports sent, or 0 if invalid. */
size_t ctr2_tx_send_datagram(ctr2_tx *tx, uint16_t port, const uint8_t *data, uint16_t len,
                             ctr2_report_sink sink, void *ctx);

/* Sends one extension message (a defined type, payload
 * 1..ctr2_ext_max_length(type)). Only for types the peer enabled. Returns
 * the number of reports sent, or 0 if invalid. */
size_t ctr2_tx_send_extension(ctr2_tx *tx, uint8_t type, const uint8_t *payload, uint16_t len,
                              ctr2_report_sink sink, void *ctx);

/* ---- Receiving -------------------------------------------------------- */

typedef enum {
    CTR2_RX_PENDING = 0,  /* report consumed, message not complete yet */
    CTR2_RX_MESSAGE = 1,  /* *type, *payload, *len describe a complete message */
    CTR2_RX_ERROR   = -1  /* framing error: end the link (send HELLO to restart) */
} ctr2_rx_result;

typedef enum {
    CTR2_ERR_NONE = 0,
    CTR2_ERR_BAD_MARKER,      /* expected a header (0xFF) */
    CTR2_ERR_BAD_VERSION,
    CTR2_ERR_BAD_COUNTER,     /* header counter above 0x7F */
    CTR2_ERR_BAD_TYPE,        /* unknown type, or an extension not enabled */
    CTR2_ERR_BAD_LENGTH,      /* too long, or wrong for the message type */
    CTR2_ERR_PACKET_COUNT,    /* packets != 1 + ceil(len / 7) */
    CTR2_ERR_NOT_STARTED,     /* DATA/DATAGRAM before any HELLO/READY/CLOSED */
    CTR2_ERR_SEQUENCE,        /* DATA/DATAGRAM counter not previous + 1 */
    CTR2_ERR_DATA_COUNTER     /* data report counter != its header's */
} ctr2_rx_error;

typedef struct {
    uint8_t  started;
    uint8_t  expected;        /* counter the next DATA must carry */
    uint8_t  msg_counter;
    uint8_t  msg_type;
    uint16_t packets_left;
    uint16_t length;
    uint16_t received;
    ctr2_rx_error error;      /* sticky until ctr2_rx_reset() */
    uint32_t extensions;      /* accepted extension types; kept by ctr2_rx_reset() */
    uint8_t  buffer[CTR2_MAX_MESSAGE];
} ctr2_rx;

void ctr2_rx_reset(ctr2_rx *rx);

/* Extension types (CTR2_CAP bits) this receiver accepts; others stay
 * framing errors. Zero (the default) is plain version 0. Pass exactly the
 * negotiated set: what the device offered, intersected with what the host
 * accepted (a device stores the host's SET masked by its own offer). A bit
 * for a type with no defined length is ignored. */
void ctr2_rx_set_extensions(ctr2_rx *rx, uint32_t caps);

/* Feed every received 8-byte report, in order. On CTR2_RX_MESSAGE the
 * payload points into rx->buffer and stays valid until the next call.
 * DATA payload is a byte stream: radio output is split at arbitrary points,
 * so feed it to the same line parser the Wi-Fi TCP path uses. DATAGRAM
 * payload is one whole UDP datagram from the radio: payload[0..1] is the
 * radio's source UDP port (MSB first) and the datagram follows, so its
 * length is *len - 2. */
ctr2_rx_result ctr2_rx_feed(ctr2_rx *rx, const uint8_t report[CTR2_REPORT_BYTES],
                            uint8_t *type, const uint8_t **payload, uint16_t *len);

#ifdef __cplusplus
}
#endif

#endif /* CTR2_LINK_H */
