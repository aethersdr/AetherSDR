/*
 * ThumbDV vocoder backend for AetherSDR's local D-STAR waveform helper.
 *
 * Copyright (C) 2026 AetherSDR contributors.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "aether_vocoder_backend.h"

#include <string>

extern "C" {
#include "datatypes.h"
#include "thumbDV.h"
}

namespace {

std::string normalizedName(const char* name)
{
    if (name == nullptr) {
        return {};
    }

    std::string normalized;
    for (const char* p = name; *p != '\0'; ++p) {
        const char ch = *p;
        if (ch == '-' || ch == '_' || ch == ' ' || ch == '/') {
            continue;
        }
        if (ch >= 'A' && ch <= 'Z') {
            normalized.push_back(static_cast<char>(ch - 'A' + 'a'));
        } else {
            normalized.push_back(ch);
        }
    }
    return normalized;
}

} // namespace

extern "C" int aether_vocoder_set_kind_from_name(const char* name)
{
    const std::string normalized = normalizedName(name);
    if (normalized.empty()
            || normalized == "thumbdv"
            || normalized == "dv3000"
            || normalized == "hardware") {
        return 0;
    }
    return -1;
}

extern "C" const char* aether_vocoder_name(void)
{
    return "thumbdv";
}

extern "C" int aether_vocoder_requires_serial(void)
{
    return 1;
}

extern "C" int aether_vocoder_probe_serial(void)
{
    return thumbDV_probeConfiguredSerial();
}

extern "C" const char* aether_vocoder_last_error(void)
{
    return thumbDV_lastError();
}

extern "C" void aether_vocoder_init(FT_HANDLE* serial_handle)
{
    thumbDV_init(serial_handle);
}

extern "C" void aether_vocoder_flush_lists(void)
{
    thumbDV_flushLists();
}

extern "C" unsigned int aether_vocoder_encode(FT_HANDLE handle,
                                              short* speech_in,
                                              unsigned char* packet_out,
                                              unsigned int num_samples)
{
    return static_cast<unsigned int>(
        thumbDV_encode(handle, speech_in, packet_out, static_cast<uint8>(num_samples)));
}

extern "C" int aether_vocoder_submit_encode(FT_HANDLE handle,
                                              const short* speech_in,
                                              unsigned int num_samples,
                                              uint64_t correlation_id)
{
    if (num_samples > 255U) {
        return 0;
    }
    return thumbDV_submitEncode(handle, speech_in,
                                static_cast<uint8>(num_samples),
                                static_cast<uint64>(correlation_id));
}

extern "C" unsigned int aether_vocoder_take_encoded(
    unsigned char* packet_out,
    unsigned int packet_capacity,
    uint64_t* correlation_id)
{
    if (packet_capacity > 255U) {
        return 0U;
    }
    uint64 sequence = 0U;
    const unsigned int bytes = static_cast<unsigned int>(
        thumbDV_takeEncoded(packet_out, static_cast<uint8>(packet_capacity),
                            correlation_id != nullptr ? &sequence : nullptr));
    if (bytes > 0U && correlation_id != nullptr) {
        *correlation_id = static_cast<uint64_t>(sequence);
    }
    return bytes;
}

extern "C" unsigned int aether_vocoder_pending_encode_requests(void)
{
    return thumbDV_pendingEncodeRequests();
}

extern "C" unsigned int aether_vocoder_available_encoded_responses(void)
{
    return thumbDV_availableEncodedResponses();
}

extern "C" unsigned int aether_vocoder_encode_outstanding(void)
{
    return thumbDV_encodeOutstanding();
}

extern "C" void aether_vocoder_decode(FT_HANDLE handle,
                                      unsigned char* packet_in,
                                      unsigned int bytes_in_packet)
{
    thumbDV_decode(handle, packet_in, static_cast<uint8>(bytes_in_packet));
}

extern "C" int aether_vocoder_get_decode_list_buffering(void)
{
    return thumbDV_getDecodeListBuffering();
}

extern "C" int aether_vocoder_unlink_audio(short* speech_out)
{
    return thumbDV_unlinkAudio(speech_out);
}
