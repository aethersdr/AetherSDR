/* SmartSDR framing and bounded TCP parser regression tests. */

#include "aether_smartsdr_command.h"
#include "aether_ipv4_source_filter.h"
#include "aether_tcp_frame_buffer.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>

static int failed = 0;

static void check(const char* name, int condition)
{
    printf("%s %s\n", condition ? "[ OK ]" : "[FAIL]", name);
    if (!condition) {
        failed++;
    }
}

static void test_command_codec(void)
{
    char frame[128];
    const int frame_length = aether_smartsdr_frame_command(
        frame, sizeof(frame), 42U, "waveform status slice=1 message=HELLO");
    check("command codec emits one complete API frame",
          frame_length > 0
              && strcmp(frame,
                        "C42|waveform status slice=1 message=HELLO\n") == 0
              && (size_t)frame_length == strlen(frame));
    check("command codec rejects LF injection",
          aether_smartsdr_frame_command(
              frame, sizeof(frame), 1U, "ping\nC2|xmit 1") < 0);
    check("command codec rejects CR injection",
          aether_smartsdr_frame_command(
              frame, sizeof(frame), 1U, "ping\rC2|xmit 1") < 0);
    check("command codec rejects truncation",
          aether_smartsdr_frame_command(
              frame, 8U, 999U, "waveform status") < 0);

    const unsigned char untrusted[] = {
        'A', ' ', '|', '\n', '=', '\r', 0x7fU, 0xffU
    };
    char encoded[sizeof(untrusted) + 1U];
    const size_t encoded_length = aether_smartsdr_encode_status_value(
        encoded, sizeof(encoded), untrusted, sizeof(untrusted));
    const unsigned char expected[] = {
        'A', 0x7fU, '?', '?', '?', '?', '?', '?', '\0'
    };
    check("RF-derived status values cannot carry protocol delimiters",
          encoded_length == sizeof(untrusted)
              && memcmp(encoded, expected, sizeof(expected)) == 0);

    uint32_t parsed = 0U;
    const char* end = NULL;
    check("uint32 parser accepts a delimited hexadecimal token",
          aether_smartsdr_parse_uint32(
              "0x89abcdef|body", 0, &parsed, &end) == 0
              && parsed == UINT32_C(0x89abcdef)
              && strcmp(end, "|body") == 0);
    check("uint32 parser rejects a negative token on every ABI",
          aether_smartsdr_parse_uint32("-1", 0, &parsed, &end) != 0);
    check("uint32 parser rejects an explicit positive sign",
          aether_smartsdr_parse_uint32("+1", 0, &parsed, &end) != 0);
    check("uint32 parser rejects leading whitespace",
          aether_smartsdr_parse_uint32(" 1", 0, &parsed, &end) != 0);
    check("uint32 parser rejects overflow",
          aether_smartsdr_parse_uint32(
              "4294967296", 10, &parsed, &end) != 0);
}

static void test_tcp_frames(void)
{
    unsigned char storage[32];
    aether_tcp_frame_buffer buffer;
    aether_tcp_frame_buffer_init(&buffer, storage, sizeof(storage));

    check("fragment append succeeds",
          aether_tcp_frame_buffer_append(
              &buffer, "R1|0|ok\r\nS2|", strlen("R1|0|ok\r\nS2|")) == 0);

    char frame[32];
    size_t length = 0U;
    check("first complete frame is extracted",
          aether_tcp_frame_buffer_next(
              &buffer, frame, sizeof(frame), &length) == AETHER_TCP_FRAME_READY
              && length == strlen("R1|0|ok")
              && strcmp(frame, "R1|0|ok") == 0);
    check("partial second frame waits for data",
          aether_tcp_frame_buffer_next(
              &buffer, frame, sizeof(frame), &length) == AETHER_TCP_FRAME_NEED_DATA);
    check("second fragment append succeeds",
          aether_tcp_frame_buffer_append(
              &buffer, "slice 0\n", strlen("slice 0\n")) == 0);
    check("fragmented frame is reconstructed",
          aether_tcp_frame_buffer_next(
              &buffer, frame, sizeof(frame), &length) == AETHER_TCP_FRAME_READY
              && strcmp(frame, "S2|slice 0") == 0);

    const unsigned char telnet_and_frame[] = {
        0xffU, 0xfbU, 0x01U, 'H', '0', '0', '0', '1', '\n'
    };
    check("Telnet prefix append succeeds",
          aether_tcp_frame_buffer_append(
              &buffer, telnet_and_frame, sizeof(telnet_and_frame)) == 0);
    check("complete Telnet negotiation is skipped",
          aether_tcp_frame_buffer_next(
              &buffer, frame, sizeof(frame), &length) == AETHER_TCP_FRAME_READY
              && strcmp(frame, "H0001") == 0);

    unsigned char small_storage[8];
    aether_tcp_frame_buffer_init(&buffer, small_storage, sizeof(small_storage));
    check("full unterminated input append succeeds once",
          aether_tcp_frame_buffer_append(
              &buffer, "12345678", sizeof(small_storage)) == 0);
    check("full unterminated input is rejected as overflow",
          aether_tcp_frame_buffer_next(
              &buffer, frame, sizeof(frame), &length) == AETHER_TCP_FRAME_OVERFLOW);

    const unsigned char terminate[] = { 3U };
    aether_tcp_frame_buffer_init(&buffer, storage, sizeof(storage));
    check("termination byte append succeeds",
          aether_tcp_frame_buffer_append(&buffer, terminate, sizeof(terminate)) == 0);
    check("termination byte is surfaced",
          aether_tcp_frame_buffer_next(
              &buffer, frame, sizeof(frame), &length) == AETHER_TCP_FRAME_TERMINATE);
}

static void test_ipv4_source_filter(void)
{
    aether_ipv4_source_filter filter;
    struct in_addr expected;
    struct in_addr foreign;
    inet_pton(AF_INET, "192.0.2.10", &expected);
    inet_pton(AF_INET, "192.0.2.11", &foreign);

    check("radio source filter accepts a valid IPv4 address",
          aether_ipv4_source_filter_init(&filter, "192.0.2.10") == 0);
    check("radio source filter accepts only the configured radio",
          aether_ipv4_source_filter_accepts(&filter, expected.s_addr)
              && !aether_ipv4_source_filter_accepts(&filter, foreign.s_addr));
    check("radio source filter fails closed on malformed input",
          aether_ipv4_source_filter_init(&filter, "not-an-address") != 0
              && !aether_ipv4_source_filter_accepts(&filter, expected.s_addr));
}

int main(void)
{
    test_command_codec();
    test_tcp_frames();
    test_ipv4_source_filter();
    return failed == 0 ? 0 : 1;
}
