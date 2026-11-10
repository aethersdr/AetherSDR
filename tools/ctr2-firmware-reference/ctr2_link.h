/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Jeremy Fielder (KK7GWY)
 *
 * CTR2 USB link, wire format version 0 -- portable reference implementation
 * for the controller firmware. C99, no heap, depends only on <stdint.h>,
 * <stddef.h> and (for the optional spectrum helper) <math.h>. MIT-licensed
 * (unlike the rest of AetherSDR) so it can be copied into closed firmware. Specification: docs/ctr2-usb-relay-design.md.
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

/* AUDIO_SPECTRUM, host -> device: the audio spectrum as bars (receive audio,
 * or the transmit audio while transmitting). Payload:
 *   [bar count N, 1..64][low Hz hi][low Hz lo][span Hz hi][span Hz lo][N levels]
 * Each level is 0..255, linear in dB from CTR2_SPECTRUM_FLOOR_DB (0) to
 * 0 dBFS (255). With low > 0 the bars are log-spaced from low to span Hz
 * (ctr2_spectrum_band_edge); with low 0 they split 0..span Hz evenly. Sent
 * at most ~20 times a second. */
#define CTR2_EXT_AUDIO_SPECTRUM 0x40u
#define CTR2_SPECTRUM_MAX_BARS  64u
#define CTR2_SPECTRUM_FLOOR_DB  (-90)
#define CTR2_SPECTRUM_HEADER    5u

/* RELAY_UDP_PORT, host -> device, sent once right after the host's READY:
 * [port hi][port lo], the host's UDP endpoint for this link. On a LAN the
 * radio sends UDP to the TCP peer (the host) at the port named by
 * `client udpport <port>`, and ignores `client udp_register`, so a device
 * registers for UDP by sending that command with this port. */
#define CTR2_EXT_RELAY_UDP_PORT 0x41u

/* AUDIO_SPECTRUM_LINEAR, host -> device: the same payload as AUDIO_SPECTRUM,
 * but the bars always split low..span Hz evenly (ctr2_spectrum_linear_edge),
 * whatever low is. A device that negotiates it gets it in place of
 * AUDIO_SPECTRUM for windows that do not start at 0: AetherSDR sends it while
 * receiving CW, centered on the CW pitch, to show zero beat. */
#define CTR2_EXT_AUDIO_SPECTRUM_LINEAR 0x42u

/* CW_TEXT, host -> device: CW decoded by the host from the active slice, as
 * it arrives (about a character at a time; word gaps are spaces). Payload:
 *   [confidence 0..3][1..CTR2_CW_TEXT_MAX printable ASCII bytes]
 * Confidence 0 is the most certain, 3 the least. */
#define CTR2_EXT_CW_TEXT        0x43u
#define CTR2_CW_TEXT_MAX        32u

/* FIRMWARE, both directions: replaces the device's application over the
 * link. A session opens in place of the host's READY: after the device
 * answers HELLO, the host resets its counter and sends FIRMWARE BEGIN. No
 * radio connection opens; the device takes that message's counter as the
 * start of the sequence (ctr2_rx_start) and sends only FIRMWARE messages
 * until the session ends (CLOSED, HELLO, or its own restart). Payload
 * [op][...], integers big-endian:
 *   host BEGIN    [0x01][image size u32][SHA-256 of the image, 32 bytes]
 *   host DATA     [0x02][offset u32][1..CTR2_FW_CHUNK image bytes], in order
 *   host END      [0x03]: verify, select the new image, restart into it
 *   host ABORT    [0x04]
 *   host SWITCH   [0x05]: restart into the device's other firmware (the
 *                 maker's own, beside this one), keeping both; BAD_STATE if
 *                 there is none
 *   host BOOTLOADER [0x06]: restart into the chip's ROM download mode, for
 *                 an install there (a device without one answers BAD_MESSAGE)
 *   device STATUS [0x81][op answered][CTR2_FW_* status][value u32]
 * Every host message gets one STATUS; for DATA, value is the bytes stored so
 * far. After END answers OK the device leaves the bus and restarts on the
 * new image, which must complete a link (answer HELLO) before it keeps
 * itself: a device that resets before then returns to its previous image. */
#define CTR2_EXT_FIRMWARE       0x44u
#define CTR2_FW_CHUNK           512u
#define CTR2_FW_BEGIN           0x01u
#define CTR2_FW_DATA            0x02u
#define CTR2_FW_END             0x03u
#define CTR2_FW_ABORT           0x04u
#define CTR2_FW_SWITCH          0x05u
#define CTR2_FW_BOOTLOADER      0x06u   /* restart into the chip's ROM download mode */
#define CTR2_FW_STATUS          0x81u
#define CTR2_FW_OK              0x00u
#define CTR2_FW_BAD_STATE       0x01u   /* no session, or a second BEGIN */
#define CTR2_FW_BAD_OFFSET      0x02u   /* DATA not at the next byte */
#define CTR2_FW_TOO_LARGE       0x03u   /* image larger than the device's slot */
#define CTR2_FW_FLASH_ERROR     0x04u
#define CTR2_FW_REJECTED        0x05u   /* not a valid, correctly signed image */
#define CTR2_FW_HASH_MISMATCH   0x06u   /* stored bytes do not match BEGIN's hash */
#define CTR2_FW_BAD_MESSAGE     0x07u   /* unknown op or wrong length */
#define CTR2_FW_BEGIN_LEN       37u
#define CTR2_FW_DATA_HEADER     5u
#define CTR2_FW_STATUS_LEN      7u

/* Device information: HID Feature report CTR2_INFO_REPORT_ID, Get only,
 * CTR2_INFO_BYTES after the report ID, readable without a link:
 *   ['F']['W'][layout 0x01][major][minor][patch][flags][0x00]
 *   [build ID, 16 bytes of printable ASCII, NUL-padded]
 * Flags: CTR2_INFO_PENDING (this image has not completed a link yet and is
 * dropped on the next reset), CTR2_INFO_SIGNED (FIRMWARE accepts only signed
 * images). A device without the report answers nothing. */
#define CTR2_INFO_REPORT_ID     0x03u
#define CTR2_INFO_BYTES         24u
#define CTR2_INFO_LAYOUT        0x01u
#define CTR2_INFO_BUILD_LEN     16u
#define CTR2_INFO_PENDING       0x01u
#define CTR2_INFO_SIGNED        0x02u

typedef struct {
    uint8_t major, minor, patch, flags;
    char build[CTR2_INFO_BUILD_LEN + 1];   /* NUL-terminated */
} ctr2_info;

void ctr2_info_encode(const ctr2_info *info, uint8_t out[CTR2_INFO_BYTES]);
/* Returns 1 and the fields for a well-formed report, else 0. */
int ctr2_info_decode(const uint8_t *in, size_t len, ctr2_info *info);

/* Lower edge in Hz of bar i of n (i == n gives span_hz, the top edge). */
float ctr2_spectrum_band_edge(uint16_t low_hz, uint16_t span_hz, uint8_t n, uint8_t i);
/* Lower edge in Hz of bar i of n for AUDIO_SPECTRUM_LINEAR. */
float ctr2_spectrum_linear_edge(uint16_t low_hz, uint16_t span_hz, uint8_t n, uint8_t i);

/* Largest payload each extension type may carry; 0 for a type not defined
 * yet. A longer message is a framing error (CTR2_ERR_BAD_LENGTH). */
uint16_t ctr2_ext_max_length(uint8_t type);

/* Wide reports: capability bit 5, which names no message (0x45 is never
 * sent). Once the host accepts it, both directions carry the 8-byte reports
 * bundled in HID report CTR2_WIDE_REPORT_ID instead of report 0x01, up to
 * CTR2_WIDE_REPORTS at a time:
 *   [count 1..7][count reports, back to back][zero padding]
 * in CTR2_WIDE_REPORT_BYTES after the report ID. The messages do not change.
 * A device offering it declares that report (Input and Output, 63 bytes)
 * beside report 0x01; Windows then pads every report it reads from the
 * device to 64 bytes. The device drops back to report 0x01 whenever the
 * host reads Feature report 0x02 (a new host session), until a Set accepts
 * the bit again. */
#define CTR2_CAP_WIDE_REPORTS   (1ul << 5)
#define CTR2_WIDE_REPORT_ID     0x04u
#define CTR2_WIDE_REPORT_BYTES  63u
#define CTR2_WIDE_REPORTS       7u

/* Reports in a wide report (`len` bytes after its report ID), or 0 when it
 * is malformed. They start at in + 1. */
size_t ctr2_wide_count(const uint8_t *in, size_t len);

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

/* First use: clears everything, including the accepted extensions, so the
 * receiver starts on plain version 0 whatever the storage held. */
void ctr2_rx_init(ctr2_rx *rx);

/* Link restart (HELLO, READY, error): clears the framing state but keeps the
 * accepted extensions, which belong to the USB session, not the link. Call
 * ctr2_rx_init() first; reset alone never clears the extension mask. */
void ctr2_rx_reset(ctr2_rx *rx);

/* Extension types (CTR2_CAP bits) this receiver accepts; others stay
 * framing errors. Zero (after ctr2_rx_init) is plain version 0. Pass exactly the
 * negotiated set: what the device offered, intersected with what the host
 * accepted (a device stores the host's SET masked by its own offer). A bit
 * for a type with no defined length is ignored. */
void ctr2_rx_set_extensions(ctr2_rx *rx, uint32_t caps);

/* After ctr2_rx_reset: starts the sequence so the next message must carry
 * `counter`, as a control message would. For a session opened by an
 * extension message instead of READY (FIRMWARE): call it with that
 * header's counter, then feed the header. */
void ctr2_rx_start(ctr2_rx *rx, uint8_t counter);

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
