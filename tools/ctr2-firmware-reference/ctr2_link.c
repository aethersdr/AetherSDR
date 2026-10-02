/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Jeremy Fielder (KK7GWY) and AetherSDR contributors
 *
 * CTR2 USB link, wire format version 0 -- reference implementation.
 * See ctr2_link.h.
 */
#include "ctr2_link.h"

static uint16_t packets_for(uint16_t len)
{
    return (uint16_t)(1u + (len + CTR2_DATA_PER_REPORT - 1u) / CTR2_DATA_PER_REPORT);
}

void ctr2_tx_reset(ctr2_tx *tx)
{
    tx->counter = 0;
}

/* Emits one message whose payload is prefix[0..prefix_len) followed by
 * data[0..len), without copying either. */
static size_t tx_emit(ctr2_tx *tx, uint8_t type, const uint8_t *prefix, uint16_t prefix_len,
                      const uint8_t *data, uint16_t len, ctr2_report_sink sink, void *ctx)
{
    uint8_t report[CTR2_REPORT_BYTES];
    const uint16_t total = (uint16_t)(prefix_len + len);
    const uint16_t packets = packets_for(total);
    uint16_t sent = 0;
    uint16_t p;
    uint8_t i;

    report[0] = CTR2_MARKER;
    report[1] = CTR2_VERSION;
    report[2] = tx->counter;
    report[3] = type;
    report[4] = (uint8_t)(packets >> 8);
    report[5] = (uint8_t)(packets & 0xFFu);
    report[6] = (uint8_t)(total >> 8);
    report[7] = (uint8_t)(total & 0xFFu);
    sink(ctx, report);

    for (p = 1; p < packets; ++p) {
        report[0] = tx->counter;
        for (i = 0; i < CTR2_DATA_PER_REPORT; ++i) {
            if (sent < prefix_len) {
                report[1 + i] = prefix[sent];
            } else if (sent < total) {
                report[1 + i] = data[sent - prefix_len];
            } else {
                report[1 + i] = 0x00u;
            }
            if (sent < total) {
                ++sent;
            }
        }
        sink(ctx, report);
    }

    tx->counter = (uint8_t)((tx->counter + 1u) & CTR2_COUNTER_MASK);
    return packets;
}

size_t ctr2_tx_send(ctr2_tx *tx, uint8_t type, const uint8_t *payload, uint16_t len,
                    ctr2_report_sink sink, void *ctx)
{
    if (type > CTR2_TYPE_CLOSED || len > CTR2_MAX_PAYLOAD) {
        return 0;  /* DATAGRAM goes through ctr2_tx_send_datagram() */
    }
    if ((type == CTR2_TYPE_DATA) != (len > 0)) {
        return 0;  /* DATA needs bytes; control messages carry none */
    }
    return tx_emit(tx, type, NULL, 0, payload, len, sink, ctx);
}

size_t ctr2_tx_send_datagram(ctr2_tx *tx, uint16_t port, const uint8_t *data, uint16_t len,
                             ctr2_report_sink sink, void *ctx)
{
    uint8_t prefix[2];
    if (len == 0 || len > CTR2_MAX_DATAGRAM) {
        return 0;
    }
    prefix[0] = (uint8_t)(port >> 8);
    prefix[1] = (uint8_t)(port & 0xFFu);
    return tx_emit(tx, CTR2_TYPE_DATAGRAM, prefix, 2, data, len, sink, ctx);
}

void ctr2_rx_reset(ctr2_rx *rx)
{
    rx->started = 0;
    rx->expected = 0;
    rx->msg_counter = 0;
    rx->msg_type = 0;
    rx->packets_left = 0;
    rx->length = 0;
    rx->received = 0;
    rx->error = CTR2_ERR_NONE;
}

static ctr2_rx_result rx_fail(ctr2_rx *rx, ctr2_rx_error error)
{
    rx->error = error;
    rx->packets_left = 0;
    return CTR2_RX_ERROR;
}

ctr2_rx_result ctr2_rx_feed(ctr2_rx *rx, const uint8_t report[CTR2_REPORT_BYTES],
                            uint8_t *type, const uint8_t **payload, uint16_t *len)
{
    uint16_t packets;
    uint16_t length;
    uint8_t i;

    if (rx->error != CTR2_ERR_NONE) {
        return CTR2_RX_ERROR;
    }

    if (rx->packets_left > 0) {
        /* Data report of the message in progress. */
        if (report[0] != rx->msg_counter) {
            return rx_fail(rx, CTR2_ERR_DATA_COUNTER);
        }
        for (i = 0; i < CTR2_DATA_PER_REPORT && rx->received < rx->length; ++i) {
            rx->buffer[rx->received++] = report[1 + i];
        }
        if (--rx->packets_left > 0) {
            return CTR2_RX_PENDING;
        }
        rx->expected = (uint8_t)((rx->msg_counter + 1u) & CTR2_COUNTER_MASK);
        *type = rx->msg_type;
        *payload = rx->buffer;
        *len = rx->length;
        return CTR2_RX_MESSAGE;
    }

    /* Header report. */
    if (report[0] != CTR2_MARKER) {
        return rx_fail(rx, CTR2_ERR_BAD_MARKER);
    }
    if (report[1] != CTR2_VERSION) {
        return rx_fail(rx, CTR2_ERR_BAD_VERSION);
    }
    if (report[2] > CTR2_COUNTER_MASK) {
        return rx_fail(rx, CTR2_ERR_BAD_COUNTER);
    }
    if (report[3] > CTR2_TYPE_DATAGRAM) {
        return rx_fail(rx, CTR2_ERR_BAD_TYPE);
    }
    packets = (uint16_t)((report[4] << 8) | report[5]);
    length = (uint16_t)((report[6] << 8) | report[7]);
    if (report[3] == CTR2_TYPE_DATA) {
        if (length == 0 || length > CTR2_MAX_PAYLOAD) {
            return rx_fail(rx, CTR2_ERR_BAD_LENGTH);
        }
    } else if (report[3] == CTR2_TYPE_DATAGRAM) {
        if (length < 3 || length > CTR2_MAX_MESSAGE) {
            return rx_fail(rx, CTR2_ERR_BAD_LENGTH);
        }
    } else if (length != 0) {
        return rx_fail(rx, CTR2_ERR_BAD_LENGTH);
    }
    if (packets != packets_for(length)) {
        return rx_fail(rx, CTR2_ERR_PACKET_COUNT);
    }

    if (report[3] != CTR2_TYPE_DATA && report[3] != CTR2_TYPE_DATAGRAM) {
        /* Control messages (re)synchronize the counter. */
        rx->started = 1;
        rx->expected = (uint8_t)((report[2] + 1u) & CTR2_COUNTER_MASK);
        *type = report[3];
        *payload = rx->buffer;
        *len = 0;
        return CTR2_RX_MESSAGE;
    }
    if (!rx->started) {
        return rx_fail(rx, CTR2_ERR_NOT_STARTED);
    }
    if (report[2] != rx->expected) {
        return rx_fail(rx, CTR2_ERR_SEQUENCE);
    }
    rx->msg_counter = report[2];
    rx->msg_type = report[3];
    rx->packets_left = (uint16_t)(packets - 1u);
    rx->length = length;
    rx->received = 0;
    return CTR2_RX_PENDING;
}
