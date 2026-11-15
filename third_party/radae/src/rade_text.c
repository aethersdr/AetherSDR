//==========================================================================
// Name:            rade_text.c
// Purpose:         Handles reliable text (callsign) in RADE EOO frame.
//                  Encoding: 6-bit char set + CRC-8 + LDPC(112,56) + GP interleave.
// Authors:         Mooneer Salem
// Source:          FreeDV-GUI src/pipeline/rade_text.c (BSD-2-Clause)
//
// AetherSDR changes from upstream:
//   - #include "ofdm_internal.h" replaced with "mpdecode_core.h"
//   - #include "../util/logging/ulog.h" replaced with "ulog.h" (no-op stub)
//==========================================================================

#include "rade_text.h"

#include <assert.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gp_interleaver.h"
#include "ldpc_codes.h"
#include "mpdecode_core.h"
#include "ulog.h"

#define LDPC_TOTAL_SIZE_BITS (112)

#define RADE_TEXT_MAX_LENGTH     (8)
#define RADE_TEXT_CRC_LENGTH     (1)
#define RADE_TEXT_MAX_RAW_LENGTH (RADE_TEXT_MAX_LENGTH + RADE_TEXT_CRC_LENGTH)

#define RADE_TEXT_BYTES_PER_ENCODED_SEGMENT (8)

static float LastEncodedLDPC[LDPC_TOTAL_SIZE_BITS];
static char  LastLDPCAsBits[LDPC_TOTAL_SIZE_BITS];

typedef struct {
    on_text_rx_t text_rx_callback;
    void        *callback_state;

    char tx_text[LDPC_TOTAL_SIZE_BITS];
    int  tx_text_index;
    int  tx_text_length;

    COMP  inbound_pending_syms[LDPC_TOTAL_SIZE_BITS / 2];
    float inbound_pending_amps[LDPC_TOTAL_SIZE_BITS / 2];
    float incomingData[LDPC_TOTAL_SIZE_BITS];

    struct LDPC ldpc;
    int enableStats;

    int unusedEooBitCount;
    int unusedEooErrCount;
} rade_text_impl_t;

// 6-bit character set:
// 0: null  1-9: ASCII 38-47  10-19: '0'-'9'  20-46: 'A'-'Z'  47: space
static void convert_callsign_to_ota_string_(const char *input, char *output,
                                             int maxLength) {
    assert(input && output && maxLength >= 0);
    int outidx = 0;
    for (int index = 0; index < maxLength; index++) {
        if (input[index] == 0) break;
        if      (input[index] >= 38  && input[index] <= 47) output[outidx++] = input[index] - 37;
        else if (input[index] >= '0' && input[index] <= '9') output[outidx++] = input[index] - '0' + 10;
        else if (input[index] >= 'A' && input[index] <= 'Z') output[outidx++] = input[index] - 'A' + 20;
        else if (input[index] >= 'a' && input[index] <= 'z') output[outidx++] = toupper(input[index]) - 'A' + 20;
    }
    output[outidx] = 0;
}

static void convert_ota_string_to_callsign_(const char *input, char *output,
                                             int maxLength) {
    assert(input && output && maxLength >= 0);
    int outidx = 0;
    for (int index = 0; index < maxLength; index++) {
        if (input[index] == 0) break;
        if      (input[index] >= 1  && input[index] <= 9)  output[outidx++] = input[index] + 37;
        else if (input[index] >= 10 && input[index] <= 19) output[outidx++] = input[index] - 10 + '0';
        else if (input[index] >= 20 && input[index] <= 46) output[outidx++] = input[index] - 20 + 'A';
    }
    output[outidx] = 0;
}

static char calculateCRC8_(char *input, int length) {
    assert(input && length >= 0);
    unsigned char generator = 0x1D;
    unsigned char crc = 0x00;
    while (length > 0) {
        unsigned char ch = (unsigned char)*input++;
        length--;
        if (ch == 0) break;
        crc ^= ch;
        for (int i = 0; i < 8; i++) {
            if (crc & 0x80) crc = (unsigned char)((crc << 1) ^ generator);
            else            crc <<= 1;
        }
    }
    return (char)crc;
}

static int rade_text_ldpc_decode(rade_text_impl_t *obj, char *dest,
                                  float meanAmplitude) {
    assert(obj && dest);
    float llr[LDPC_TOTAL_SIZE_BITS];
    unsigned char output[LDPC_TOTAL_SIZE_BITS];
    int parityCheckCount = 0;

    int Npayloadsymsperpacket = LDPC_TOTAL_SIZE_BITS / 2;
    float EsNo = 3.0f;
    log_info("mean amplitude: %f", meanAmplitude);

    symbols_to_llrs(llr, (COMP *)obj->inbound_pending_syms,
                    obj->inbound_pending_amps, EsNo, meanAmplitude,
                    Npayloadsymsperpacket);
    run_ldpc_decoder(&obj->ldpc, output, llr, &parityCheckCount);

    float ber_est = (float)(obj->ldpc.NumberParityBits - parityCheckCount) /
                    obj->ldpc.NumberParityBits;
    int result = (ber_est < 0.2f);

    log_info("Estimated BER: %f", ber_est);

    if (result) {
        memset(dest, 0, RADE_TEXT_BYTES_PER_ENCODED_SEGMENT);
        for (int bitIndex = 0; bitIndex < 8; bitIndex++) {
            if (output[bitIndex])
                dest[0] |= (char)(1 << bitIndex);
        }
        for (int bitIndex = 8; bitIndex < (LDPC_TOTAL_SIZE_BITS / 2); bitIndex++) {
            int bitsSinceCrc = bitIndex - 8;
            if (output[bitIndex])
                dest[1 + (bitsSinceCrc / 6)] |= (char)(1 << (bitsSinceCrc % 6));
        }
    }
    return result;
}

void rade_text_rx(rade_text_t ptr, float *syms, int symSize) {
    rade_text_impl_t *obj = (rade_text_impl_t *)ptr;
    assert(obj);

    gp_deinterleave_comp((COMP *)obj->inbound_pending_syms, (COMP *)syms,
                         LDPC_TOTAL_SIZE_BITS / 2);

    float rms = 0;
    obj->unusedEooBitCount = 0;
    obj->unusedEooErrCount = 0;
    for (int index = 0; index < symSize; index++) {
        if (index < (LDPC_TOTAL_SIZE_BITS / 2)) {
            COMP *sym = (COMP *)&obj->inbound_pending_syms[index];
            rms += sym->real * sym->real + sym->imag * sym->imag;
        }
    }
    rms = sqrtf(rms / symSize);

    for (int index = 0; index < LDPC_TOTAL_SIZE_BITS / 2; index++) {
        obj->inbound_pending_amps[index] = rms;
    }

    char decodedStr[RADE_TEXT_MAX_RAW_LENGTH + 1];
    char rawStr[RADE_TEXT_MAX_RAW_LENGTH + 1];
    memset(rawStr,     0, RADE_TEXT_MAX_RAW_LENGTH + 1);
    memset(decodedStr, 0, RADE_TEXT_MAX_RAW_LENGTH + 1);

    if (rade_text_ldpc_decode(obj, rawStr, rms) != 0) {
        convert_ota_string_to_callsign_(&rawStr[RADE_TEXT_CRC_LENGTH],
                                        &decodedStr[RADE_TEXT_CRC_LENGTH],
                                        RADE_TEXT_MAX_LENGTH);
        decodedStr[0] = rawStr[0];

        unsigned char receivedCRC = (unsigned char)decodedStr[0];
        unsigned char calcCRC     = (unsigned char)calculateCRC8_(
            &rawStr[RADE_TEXT_CRC_LENGTH], RADE_TEXT_MAX_LENGTH);

        log_info("rxCRC: %d, calcCRC: %d, decodedStr: %s",
                 receivedCRC, calcCRC, &decodedStr[RADE_TEXT_CRC_LENGTH]);

        if (receivedCRC == calcCRC && obj->text_rx_callback) {
            obj->text_rx_callback(obj, &decodedStr[RADE_TEXT_CRC_LENGTH],
                                  (int)strlen(&decodedStr[RADE_TEXT_CRC_LENGTH]),
                                  obj->callback_state);
        }
    }
}

rade_text_t rade_text_create(void) {
    rade_text_impl_t *ret = calloc(1, sizeof(rade_text_impl_t));
    assert(ret);
    int code_index = ldpc_codes_find("HRA_56_56");
    memcpy(&ret->ldpc, &ldpc_codes[code_index], sizeof(struct LDPC));
    return (rade_text_t)ret;
}

void rade_text_destroy(rade_text_t ptr) {
    assert(ptr);
    free(ptr);
}

void rade_text_generate_tx_string(rade_text_t ptr, const char *str,
                                   int strlength, float *syms, int symSize) {
    rade_text_impl_t *impl = (rade_text_impl_t *)ptr;
    assert(impl);

    char tmp[RADE_TEXT_MAX_RAW_LENGTH + 1];
    memset(tmp, 0, RADE_TEXT_MAX_RAW_LENGTH + 1);

    int clamp = (strlength < RADE_TEXT_MAX_LENGTH) ? strlength : RADE_TEXT_MAX_LENGTH;
    convert_callsign_to_ota_string_(str, &tmp[RADE_TEXT_CRC_LENGTH], clamp);

    int txt_length = (int)strlen(&tmp[RADE_TEXT_CRC_LENGTH]);
    if (txt_length > RADE_TEXT_MAX_LENGTH) txt_length = RADE_TEXT_MAX_LENGTH;

    impl->tx_text_length = LDPC_TOTAL_SIZE_BITS;
    impl->tx_text_index  = 0;

    unsigned char crc = (unsigned char)calculateCRC8_(&tmp[RADE_TEXT_CRC_LENGTH], txt_length);
    tmp[0] = (char)crc;

    unsigned char ibits[LDPC_TOTAL_SIZE_BITS / 2];
    unsigned char pbits[LDPC_TOTAL_SIZE_BITS / 2];
    memset(ibits, 0, LDPC_TOTAL_SIZE_BITS / 2);
    memset(pbits, 0, LDPC_TOTAL_SIZE_BITS / 2);

    for (int index = 0; index < 8; index++) {
        if (tmp[0] & (1 << index))
            ibits[index] = 1;
    }
    for (int ibitsBitIndex = 8; ibitsBitIndex < (LDPC_TOTAL_SIZE_BITS / 2); ibitsBitIndex++) {
        int bitsFromCrc = ibitsBitIndex - 8;
        unsigned int byte     = (unsigned char)tmp[RADE_TEXT_CRC_LENGTH + bitsFromCrc / 6];
        unsigned int bitToCheck = bitsFromCrc % 6;
        if (byte & (1u << bitToCheck))
            ibits[ibitsBitIndex] = 1;
    }

    encode(&impl->ldpc, ibits, pbits);

    char tmpbits[LDPC_TOTAL_SIZE_BITS];
    memset(impl->tx_text, 0, LDPC_TOTAL_SIZE_BITS);
    memcpy(&tmpbits[0],                    &ibits[0], LDPC_TOTAL_SIZE_BITS / 2);
    memcpy(&tmpbits[LDPC_TOTAL_SIZE_BITS / 2], &pbits[0], LDPC_TOTAL_SIZE_BITS / 2);
    memcpy(LastLDPCAsBits,  tmpbits,         LDPC_TOTAL_SIZE_BITS);
    memcpy(impl->tx_text,   tmpbits,         LDPC_TOTAL_SIZE_BITS);

    gp_interleave_bits(&impl->tx_text[0], tmpbits, LDPC_TOTAL_SIZE_BITS / 2);

    for (int index = 0; index < LDPC_TOTAL_SIZE_BITS / 2; index++) {
        char *p = &impl->tx_text[2 * index];
        if      (*p == 0 && *(p+1) == 0) { syms[2*index] =  1; syms[2*index+1] =  0; }
        else if (*p == 0 && *(p+1) == 1) { syms[2*index] =  0; syms[2*index+1] =  1; }
        else if (*p == 1 && *(p+1) == 0) { syms[2*index] =  0; syms[2*index+1] = -1; }
        else                              { syms[2*index] = -1; syms[2*index+1] =  0; }
    }

    if (impl->enableStats)
        memcpy(LastEncodedLDPC, syms, LDPC_TOTAL_SIZE_BITS * sizeof(float));

    if (symSize > LDPC_TOTAL_SIZE_BITS) {
        for (int index = LDPC_TOTAL_SIZE_BITS; index < symSize; index++)
            syms[index] = (index % 2) ? 0.0f : 1.0f;
    }
}

void rade_text_set_rx_callback(rade_text_t ptr, on_text_rx_t text_rx_fn,
                                void *state) {
    rade_text_impl_t *impl = (rade_text_impl_t *)ptr;
    assert(impl);
    impl->text_rx_callback = text_rx_fn;
    impl->callback_state   = state;
}

void rade_text_enable_stats_output(rade_text_t ptr, int enable) {
    rade_text_impl_t *impl = (rade_text_impl_t *)ptr;
    assert(impl);
    impl->enableStats = enable;
}
