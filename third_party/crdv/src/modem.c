/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "crdv.h"

#include <math.h>

size_t crdv_gaussian_tap_count(unsigned samples_per_bit, unsigned symbol_span)
{
    if (samples_per_bit == 0u || symbol_span == 0u ||
        (size_t)samples_per_bit > (SIZE_MAX - 1u) / (size_t)symbol_span) {
        return 0u;
    }
    return (size_t)samples_per_bit * (size_t)symbol_span + 1u;
}

crdv_result crdv_gaussian_taps(double bt, unsigned samples_per_bit,
                               unsigned symbol_span, float *out, size_t capacity)
{
    const double pi = 3.14159265358979323846264338327950288;
    const double log_two = 0.69314718055994530941723212145817657;
    size_t count = crdv_gaussian_tap_count(samples_per_bit, symbol_span);
    double total = 0.0;
    double center;

    if (out == NULL) {
        return CRDV_E_ARGUMENT;
    }
    if (!isfinite(bt) || bt <= 0.0 || count == 0u) {
        return CRDV_E_RANGE;
    }
    if (capacity < count) {
        return CRDV_E_CAPACITY;
    }
    center = (double)(count - 1u) / 2.0;
    for (size_t index = 0; index < count; ++index) {
        double time_in_bits = ((double)index - center) / (double)samples_per_bit;
        double argument = pi * bt * time_in_bits;
        double value = exp((-2.0 * argument * argument) / log_two);
        out[index] = (float)value;
        total += value;
    }
    if (!isfinite(total) || total <= 0.0) {
        return CRDV_E_RANGE;
    }
    for (size_t index = 0; index < count; ++index) {
        out[index] = (float)((double)out[index] / total);
    }
    return CRDV_OK;
}

crdv_result crdv_modulate_discriminator(const uint8_t *bits, size_t bit_count,
                                        const crdv_modulator_config *config,
                                        float *samples, size_t capacity,
                                        size_t *written)
{
    size_t needed;
    size_t center;
    if ((bits == NULL && bit_count != 0u) || config == NULL || samples == NULL ||
        written == NULL || config->frequency_taps == NULL) {
        return CRDV_E_ARGUMENT;
    }
    *written = 0u;
    if (config->samples_per_bit == 0u || config->tap_count == 0u ||
        !isfinite(config->gain) || config->gain <= 0.0f ||
        bit_count > SIZE_MAX / (size_t)config->samples_per_bit) {
        return CRDV_E_RANGE;
    }
    needed = bit_count * (size_t)config->samples_per_bit;
    if (capacity < needed) {
        return CRDV_E_CAPACITY;
    }
    for (size_t tap = 0; tap < config->tap_count; ++tap) {
        if (!isfinite(config->frequency_taps[tap])) {
            return CRDV_E_FORMAT;
        }
    }
    center = config->tap_count / 2u;
    for (size_t sample_index = 0; sample_index < needed; ++sample_index) {
        double shaped = 0.0;
        for (size_t tap = 0; tap < config->tap_count; ++tap) {
            ptrdiff_t source_sample = (ptrdiff_t)sample_index + (ptrdiff_t)tap -
                                      (ptrdiff_t)center;
            size_t source_bit;
            double symbol;
            if (source_sample < 0) {
                source_bit = 0u;
            } else {
                source_bit = (size_t)source_sample /
                             (size_t)config->samples_per_bit;
                if (source_bit >= bit_count) {
                    source_bit = bit_count - 1u;
                }
            }
            symbol = (bits[source_bit] & 1u) != 0u ? 1.0 : -1.0;
            shaped += symbol * (double)config->frequency_taps[tap];
        }
        if (config->invert) {
            shaped = -shaped;
        }
        shaped *= (double)config->gain;
        if (shaped > 0.98) {
            shaped = 0.98;
        } else if (shaped < -0.98) {
            shaped = -0.98;
        }
        samples[sample_index] = (float)shaped;
    }
    *written = needed;
    return CRDV_OK;
}

crdv_result crdv_demodulate_discriminator(const float *samples,
                                          size_t sample_count,
                                          unsigned samples_per_bit,
                                          bool invert, uint8_t *bits,
                                          size_t capacity, size_t *written)
{
    size_t count;
    if ((samples == NULL && sample_count != 0u) || bits == NULL ||
        written == NULL || samples_per_bit == 0u) {
        return CRDV_E_ARGUMENT;
    }
    *written = 0u;
    if (sample_count % (size_t)samples_per_bit != 0u) {
        return CRDV_E_FORMAT;
    }
    count = sample_count / (size_t)samples_per_bit;
    if (capacity < count) {
        return CRDV_E_CAPACITY;
    }
    for (size_t bit = 0; bit < count; ++bit) {
        double total = 0.0;
        for (size_t offset = 0; offset < (size_t)samples_per_bit; ++offset) {
            float value = samples[bit * (size_t)samples_per_bit + offset];
            if (!isfinite(value)) {
                return CRDV_E_FORMAT;
            }
            total += (double)value;
        }
        if (invert) {
            total = -total;
        }
        bits[bit] = (uint8_t)(total >= 0.0 ? 1u : 0u);
    }
    *written = count;
    return CRDV_OK;
}
