/* Per-stream VITA packet-count regression test. */

#include "vita_output.h"
#include "vita_packet_sequence.h"
#include "vita.h"

#include <arpa/inet.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static unsigned char captured_packet[2048];
static uint32 captured_packet_size;
static uint32 captured_ip_address;
static uint16 captured_udp_port;
static uint32 listener_send_count;

void output(const char * format, ...)
{
    (void)format;
}

void tc_abort(void)
{
}

BOOL hal_ListenerSend(const void* packet, uint32 num_bytes,
                      uint32 ip_address, uint16 udp_port)
{
    if (packet == NULL || num_bytes > sizeof(captured_packet)) {
        return FALSE;
    }

    memcpy(captured_packet, packet, num_bytes);
    captured_packet_size = num_bytes;
    captured_ip_address = ip_address;
    captured_udp_port = udp_port;
    listener_send_count++;
    return TRUE;
}

int main(void)
{
    int failed = 0;
    vita_output_resetPacketCounts();

    failed += vita_output_nextPacketCount(0x100U) != 0U;
    failed += vita_output_nextPacketCount(0x100U) != 1U;
    failed += vita_output_nextPacketCount(0x101U) != 0U;
    failed += vita_output_nextPacketCount(0x100U) != 2U;

    uint32 i;
    for (i = 3U; i < 16U; i++) {
        failed += vita_output_nextPacketCount(0x100U) != i;
    }
    failed += vita_output_nextPacketCount(0x100U) != 0U;

    vita_output_resetPacketCounts();
    failed += vita_output_nextPacketCount(0x100U) != 0U;

    Complex samples[128] = {{0.0f, 0.0f}};
    buffer_descriptor descriptor;
    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.buf_ptr = samples;
    descriptor.num_samples = 128U;
    descriptor.sample_size = sizeof(Complex);
    descriptor.stream_id = 0x81000000U;

    vita_output_Init("127.0.0.1");
    failed += emit_waveform_output(&descriptor) != TRUE;

    const VitaIFData packet = (const VitaIFData)captured_packet;
    failed += listener_send_count != 1U;
    failed += captured_packet_size != 28U + 128U * sizeof(Complex);
    failed += captured_ip_address != 0x7F000001U;
    failed += captured_udp_port != 4991U;
    failed += ntohl(packet->stream_id) != descriptor.stream_id;
    failed += ((ntohl(packet->header) & VITA_HEADER_PACKET_COUNT_MASK) >> 16U) != 0U;

    vita_packet_sequence_tracker tracker;
    vita_packet_sequence_reset(&tracker);

    vita_packet_sequence_result result =
        vita_packet_sequence_observe(&tracker, 0x81000000U, 14U);
    failed += result.status != VITA_PACKET_SEQUENCE_FIRST;
    result = vita_packet_sequence_observe(&tracker, 0x81000000U, 15U);
    failed += result.status != VITA_PACKET_SEQUENCE_IN_ORDER;
    result = vita_packet_sequence_observe(&tracker, 0x81000000U, 0U);
    failed += result.status != VITA_PACKET_SEQUENCE_IN_ORDER;
    result = vita_packet_sequence_observe(&tracker, 0x81000001U, 7U);
    failed += result.status != VITA_PACKET_SEQUENCE_FIRST;
    result = vita_packet_sequence_observe(&tracker, 0x81000000U, 2U);
    failed += result.status != VITA_PACKET_SEQUENCE_GAP;
    failed += result.expected != 1U;
    failed += result.received != 2U;
    failed += result.missing != 1U;
    result = vita_packet_sequence_observe(&tracker, 0x81000000U, 2U);
    failed += result.status != VITA_PACKET_SEQUENCE_DUPLICATE;
    result = vita_packet_sequence_observe(&tracker, 0x81000000U, 3U);
    failed += result.status != VITA_PACKET_SEQUENCE_IN_ORDER;

    printf("%s shared-socket VITA output, per-stream counters, and input continuity\n",
           failed == 0 ? "[ OK ]" : "[FAIL]");
    return failed == 0 ? 0 : 1;
}
