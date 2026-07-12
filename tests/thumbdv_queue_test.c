/* ThumbDV queue purge regression test. */

#include "thumbDV.h"
#include "utils.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__linux__)
#include <errno.h>
#include <poll.h>
#include <pty.h>
#include <unistd.h>
#endif

#define QUEUE_STRESS_ITERATIONS 2000U

void sched_waveform_setHandle(FT_HANDLE * handle)
{
    (void)handle;
}

#if defined(__linux__)
typedef struct fake_dv3000 {
    int master_fd;
    volatile int stop;
    unsigned int responses;
} fake_dv3000;

static int read_exact(int fd, unsigned char * buffer, size_t length)
{
    size_t total = 0U;
    while (total < length) {
        const ssize_t bytes = read(fd, buffer + total, length - total);
        if (bytes < 0 && errno == EINTR) {
            continue;
        }
        if (bytes <= 0) {
            return -1;
        }
        total += (size_t)bytes;
    }
    return 0;
}

static void * fake_dv3000_thread(void * opaque)
{
    fake_dv3000 * fake = (fake_dv3000 *)opaque;
    while (!fake->stop) {
        struct pollfd pfd = { .fd = fake->master_fd, .events = POLLIN, .revents = 0 };
        const int ready = poll(&pfd, 1, 100);
        if (ready <= 0 || (pfd.revents & POLLIN) == 0) {
            continue;
        }

        unsigned char header[4];
        if (read_exact(fake->master_fd, header, sizeof(header)) != 0) {
            continue;
        }
        const size_t payload_length = (size_t)header[1] * 256U + header[2];
        if (header[0] != 0x61U || header[3] != 0x00U
            || payload_length == 0U || payload_length > 64U) {
            continue;
        }
        unsigned char payload[64];
        if (read_exact(fake->master_fd, payload, payload_length) != 0) {
            continue;
        }

        const unsigned char response[5] = {
            0x61U, 0x00U, 0x01U, 0x00U,
            payload[0] == 0x33U ? 0x39U : payload[0]
        };
        if (write(fake->master_fd, response, sizeof(response))
            == (ssize_t)sizeof(response)) {
            fake->responses++;
        }
    }
    return NULL;
}

static int test_serial_probe(void)
{
    int failed = 0;
    FT_HANDLE missing_handle = NULL;
    const FT_STATUS missing_status = FT_OpenEx(
        "/definitely/not/a/thumbdv", FT_OPEN_BY_SERIAL_NUMBER, &missing_handle);
    failed += missing_status != FT_DEVICE_NOT_FOUND;
    failed += strstr(AetherSerial_GetLastError(), "No such file") == NULL;

    int master_fd = -1;
    int slave_fd = -1;
    char slave_name[256] = {0};
    if (openpty(&master_fd, &slave_fd, slave_name, NULL, NULL) != 0) {
        return failed + 1;
    }

    fake_dv3000 fake = { .master_fd = master_fd, .stop = 0, .responses = 0U };
    pthread_t fake_thread;
    if (pthread_create(&fake_thread, NULL, fake_dv3000_thread, &fake) != 0) {
        close(master_fd);
        close(slave_fd);
        return failed + 1;
    }

    setenv("AETHER_DV_THUMBDV_SERIAL", slave_name, 1);
    failed += thumbDV_probeConfiguredSerial() != 0;
    failed += fake.responses < 2U;
    unsetenv("AETHER_DV_THUMBDV_SERIAL");

    fake.stop = 1;
    pthread_join(fake_thread, NULL);
    close(master_fd);
    close(slave_fd);
    return failed;
}
#endif

static void * queue_encoded_responses(void * unused)
{
    (void)unused;
    uint32 i;
    for (i = 0U; i < QUEUE_STRESS_ITERATIONS; i++) {
        thumbDV_testEnqueueEncodedRequest();
        thumbDV_testQueueEncodedResponse();
    }
    return NULL;
}

static void * flush_queues(void * unused)
{
    (void)unused;
    uint32 i;
    for (i = 0U; i < QUEUE_STRESS_ITERATIONS; i++) {
        thumbDV_flushLists();
    }
    return NULL;
}

int main(void)
{
    lock_printf_init();
    lock_malloc_init();
    thumbDV_testInitializeQueues();

    int failed = 0;
    failed += thumbDV_testFailFastPolicy(TRUE, "1") != TRUE;
    failed += thumbDV_testFailFastPolicy(TRUE, "0") != FALSE;
    failed += thumbDV_testFailFastPolicy(FALSE, "1") != FALSE;
    failed += thumbDV_testReadSemaphoreReset() != TRUE;
    char serial_fault[128] = {0};
    thumbDV_testReportSerialFault("malformed DV3000 response");
    failed += thumbDV_testTakeSerialFault(
        serial_fault, sizeof(serial_fault)) != TRUE;
    failed += strcmp(serial_fault, "malformed DV3000 response") != 0;
    failed += thumbDV_testTakeSerialFault(
        serial_fault, sizeof(serial_fault)) != FALSE;
#if defined(__linux__)
    failed += test_serial_probe();
#endif

    thumbDV_testEnqueueEncodedRequestWithCorrelation(41U);
    failed += thumbDV_testPendingEncodedRequests() != 1U;
    failed += thumbDV_testEncodeOutstanding() != 1U;
    failed += thumbDV_testQueueEncodedResponse() != TRUE;
    failed += thumbDV_testPendingEncodedRequests() != 0U;
    failed += thumbDV_testEncodeOutstanding() != 1U;
    failed += thumbDV_testEncodedHeadCorrelation() != 41U;
    thumbDV_flushLists();
    failed += thumbDV_testEncodeOutstanding() != 0U;

    thumbDV_testQueueEncodedFrames(6U);
    thumbDV_testQueueDecodedFrames(2U);
    failed += thumbDV_testEncodedFrameCount() != 6U;
    failed += thumbDV_testDecodedFrameCount() != 2U;
    failed += thumbDV_testEncodedBuffering() != FALSE;
    failed += thumbDV_testDecodedBuffering() != FALSE;

    thumbDV_flushLists();
    failed += thumbDV_testEncodedFrameCount() != 0U;
    failed += thumbDV_testDecodedFrameCount() != 0U;
    failed += thumbDV_testEncodedBuffering() != TRUE;
    failed += thumbDV_testDecodedBuffering() != TRUE;

    thumbDV_testEnqueueEncodedRequest();
    thumbDV_testEnqueueDecodedRequest();
    thumbDV_flushLists();
    thumbDV_testEnqueueEncodedRequestWithCorrelation(84U);
    thumbDV_testEnqueueDecodedRequest();
    failed += thumbDV_testPendingEncodedRequests() != 1U;
    failed += thumbDV_testConsumeEncodedResponse() != FALSE;
    failed += thumbDV_testConsumeDecodedResponse() != FALSE;
    failed += thumbDV_testConsumeEncodedResponse() != TRUE;
    failed += thumbDV_testConsumeDecodedResponse() != TRUE;
    failed += thumbDV_testPendingEncodedRequests() != 0U;

    thumbDV_testEnqueueEncodedRequest();
    thumbDV_testEnqueueDecodedRequest();
    thumbDV_flushLists();
    failed += thumbDV_testQueueEncodedResponse() != FALSE;
    failed += thumbDV_testQueueDecodedResponse() != FALSE;
    failed += thumbDV_testEncodedFrameCount() != 0U;
    failed += thumbDV_testDecodedFrameCount() != 0U;

    thumbDV_testEnqueueEncodedRequestWithCorrelation(126U);
    thumbDV_testEnqueueDecodedRequest();
    failed += thumbDV_testQueueEncodedResponse() != TRUE;
    failed += thumbDV_testQueueDecodedResponse() != TRUE;
    failed += thumbDV_testEncodedFrameCount() != 1U;
    failed += thumbDV_testDecodedFrameCount() != 1U;
    failed += thumbDV_testEncodedHeadCorrelation() != 126U;

    thumbDV_flushLists();
    pthread_t response_thread;
    pthread_t flush_thread;
    const int response_thread_result =
        pthread_create(&response_thread, NULL, queue_encoded_responses, NULL);
    const int flush_thread_result =
        pthread_create(&flush_thread, NULL, flush_queues, NULL);
    failed += response_thread_result != 0;
    failed += flush_thread_result != 0;
    if (response_thread_result == 0) {
        pthread_join(response_thread, NULL);
    }
    if (flush_thread_result == 0) {
        pthread_join(flush_thread, NULL);
    }
    thumbDV_flushLists();
    failed += thumbDV_testEncodedFrameCount() != 0U;
    failed += thumbDV_testDecodedFrameCount() != 0U;

    thumbDV_testDestroyQueues();
    printf("%s ThumbDV queue purge\n", failed == 0 ? "[ OK ]" : "[FAIL]");
    return failed == 0 ? 0 : 1;
}
