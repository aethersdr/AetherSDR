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

size_t ctr2_tx_send(ctr2_tx *tx, uint8_t type, const uint8_t *payload, uint16_t len,
                    ctr2_report_sink sink, void *ctx)
{
    uint8_t report[CTR2_REPORT_BYTES];
    uint16_t packets;
    uint16_t sent = 0;
    uint16_t p;
    uint8_t i;

    if (type > CTR2_TYPE_CLOSED || len > CTR2_MAX_PAYLOAD) {
        return 0;
    }
    if ((type == CTR2_TYPE_DATA) != (len > 0)) {
        return 0;  /* DATA needs bytes; control messages carry none */
    }

    packets = packets_for(len);
    report[0] = CTR2_MARKER;
    report[1] = CTR2_VERSION;
    report[2] = tx->counter;
    report[3] = type;
    report[4] = (uint8_t)(packets >> 8);
    report[5] = (uint8_t)(packets & 0xFFu);
    report[6] = (uint8_t)(len >> 8);
    report[7] = (uint8_t)(len & 0xFFu);
    sink(ctx, report);

    for (p = 1; p < packets; ++p) {
        report[0] = tx->counter;
        for (i = 0; i < CTR2_DATA_PER_REPORT; ++i) {
            report[1 + i] = (sent < len) ? payload[sent++] : 0x00u;
        }
        sink(ctx, report);
    }

    tx->counter = (uint8_t)((tx->counter + 1u) & CTR2_COUNTER_MASK);
    return packets;
}

void ctr2_rx_reset(ctr2_rx *rx)
{
    rx->started = 0;
    rx->expected = 0;
    rx->msg_counter = 0;
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
        *type = CTR2_TYPE_DATA;
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
    if (report[3] > CTR2_TYPE_CLOSED) {
        return rx_fail(rx, CTR2_ERR_BAD_TYPE);
    }
    packets = (uint16_t)((report[4] << 8) | report[5]);
    length = (uint16_t)((report[6] << 8) | report[7]);
    if (length > CTR2_MAX_PAYLOAD || (report[3] == CTR2_TYPE_DATA) != (length > 0)) {
        return rx_fail(rx, CTR2_ERR_BAD_LENGTH);
    }
    if (packets != packets_for(length)) {
        return rx_fail(rx, CTR2_ERR_PACKET_COUNT);
    }

    if (report[3] != CTR2_TYPE_DATA) {
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
    rx->packets_left = (uint16_t)(packets - 1u);
    rx->length = length;
    rx->received = 0;
    return CTR2_RX_PENDING;
}
