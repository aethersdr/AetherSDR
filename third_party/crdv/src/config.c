/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "crdv.h"

#include <string.h>

static bool ascii_alnum(unsigned char value)
{
    return (value >= (unsigned char)'A' && value <= (unsigned char)'Z') ||
           (value >= (unsigned char)'0' && value <= (unsigned char)'9');
}

static unsigned char ascii_upper(unsigned char value)
{
    if (value >= (unsigned char)'a' && value <= (unsigned char)'z') {
        return (unsigned char)(value - (unsigned char)'a' + (unsigned char)'A');
    }
    return value;
}

static crdv_result normalize_field(const char *source, char *destination,
                                   size_t width, size_t minimum,
                                   bool leading_slash)
{
    size_t length;
    bool has_alnum = false;

    if (source == NULL) {
        return CRDV_E_ARGUMENT;
    }
    length = strlen(source);
    if (length < minimum || length > width) {
        return CRDV_E_RANGE;
    }
    for (size_t index = 0; index < length; ++index) {
        unsigned char value = ascii_upper((unsigned char)source[index]);
        bool slash = leading_slash && index == 0u && value == (unsigned char)'/';
        if (!ascii_alnum(value) && value != (unsigned char)' ' && !slash) {
            return CRDV_E_FORMAT;
        }
        has_alnum = has_alnum || ascii_alnum(value);
        destination[index] = (char)value;
    }
    if (!has_alnum) {
        return CRDV_E_FORMAT;
    }
    for (size_t index = length; index < width; ++index) {
        destination[index] = ' ';
    }
    destination[width] = '\0';
    return CRDV_OK;
}

crdv_result crdv_config_normalize(const char *mycall, const char *suffix,
                                  const char *urcall, const char *rpt1,
                                  const char *rpt2, const char *message,
                                  crdv_station_config *out)
{
    crdv_station_config next;
    crdv_result result;
    bool has_letter = false;
    bool has_digit = false;
    const char *actual_suffix = suffix == NULL ? "" : suffix;
    const char *actual_urcall = urcall == NULL ? "CQCQCQ" : urcall;
    const char *actual_rpt1 = rpt1 == NULL ? "DIRECT" : rpt1;
    const char *actual_rpt2 = rpt2 == NULL ? "DIRECT" : rpt2;
    const char *actual_message = message == NULL ? "" : message;
    size_t mycall_length;
    size_t suffix_length;
    size_t message_length;

    if (mycall == NULL || out == NULL) {
        return CRDV_E_ARGUMENT;
    }
    mycall_length = strlen(mycall);
    if (mycall_length < 3u || mycall_length > 8u) {
        return CRDV_E_RANGE;
    }
    memset(&next, 0, sizeof(next));
    for (size_t index = 0; index < mycall_length; ++index) {
        unsigned char value = ascii_upper((unsigned char)mycall[index]);
        if (!ascii_alnum(value)) {
            return CRDV_E_FORMAT;
        }
        has_letter = has_letter ||
                     (value >= (unsigned char)'A' && value <= (unsigned char)'Z');
        has_digit = has_digit ||
                    (value >= (unsigned char)'0' && value <= (unsigned char)'9');
        next.mycall[index] = (char)value;
    }
    if (!has_letter || !has_digit) {
        return CRDV_E_FORMAT;
    }
    for (size_t index = mycall_length; index < 8u; ++index) {
        next.mycall[index] = ' ';
    }

    suffix_length = strlen(actual_suffix);
    if (suffix_length > 4u) {
        return CRDV_E_RANGE;
    }
    for (size_t index = 0; index < suffix_length; ++index) {
        unsigned char value = ascii_upper((unsigned char)actual_suffix[index]);
        if (!ascii_alnum(value)) {
            return CRDV_E_FORMAT;
        }
        next.suffix[index] = (char)value;
    }
    for (size_t index = suffix_length; index < 4u; ++index) {
        next.suffix[index] = ' ';
    }

    result = normalize_field(actual_urcall, next.urcall, 8u, 1u, true);
    if (result != CRDV_OK) {
        return result;
    }
    result = normalize_field(actual_rpt1, next.rpt1, 8u, 1u, false);
    if (result != CRDV_OK) {
        return result;
    }
    result = normalize_field(actual_rpt2, next.rpt2, 8u, 1u, false);
    if (result != CRDV_OK) {
        return result;
    }

    message_length = strlen(actual_message);
    if (message_length > 20u) {
        return CRDV_E_RANGE;
    }
    for (size_t index = 0; index < message_length; ++index) {
        unsigned char value = (unsigned char)actual_message[index];
        if (value < 0x20u || value > 0x7eu || value == (unsigned char)'|') {
            return CRDV_E_FORMAT;
        }
        next.message[index] = (char)value;
    }
    for (size_t index = message_length; index < 20u; ++index) {
        next.message[index] = ' ';
    }

    *out = next;
    return CRDV_OK;
}
