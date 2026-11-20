/* Compile the pinned library itself; libusb is injected without USB hardware.
 * Cancellation is asynchronous: NOT_FOUND also means already cancelling.
 */
#include <libusb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(RTL_SIMULATE_WINDOWS) && !defined(_WIN32)
#include <unistd.h>
static void Sleep(unsigned int ms) { (void)ms; }
#define min(a,b) ((a)<(b)?(a):(b))
#define _WIN32 1
#endif
#define rtlsdr_STATIC
#include RTL_ASYNC_SOURCE

enum scenario { DELAYED, PROMPT, LATE_COMPLETED, EVENT_ERROR, DEVICE_LOST,
                SUBMIT_FAILURE, RESUBMIT_FAILURE, CALLBACK_STOP, ALLOC_FAILURE,
                PERSISTENT_STALL, SECOND_SESSION, LOST_AGAIN,
                SCENARIO_COUNT };
struct pending {
    struct libusb_transfer *transfer;
    int submitted, cancelling, attempts;
};
static struct pending slots[15];
static rtlsdr_dev_t device;
static enum scenario scenario;
static int allocated, polls, callbacks, freed_pending, freed, resubmitted_after_stop;
static int cancel_seen, injected;

static void fail(const char *message)
{
    fprintf(stderr, "FAIL scenario=%d: %s\n", scenario, message);
    exit(1);
}

/* The driver references this from a path the harness never takes (zero-copy
 * buffers). GNU ld keeps the reference, so give it a body that fails if it
 * is ever reached. */
#if defined(__linux__) && LIBUSB_API_VERSION >= 0x01000105
int LIBUSB_CALL libusb_dev_mem_free(libusb_device_handle *h, unsigned char *buffer,
                                    size_t length)
{
    (void)h; (void)buffer; (void)length;
    fail("zero-copy buffer freed");
    return 0;
}
#endif

struct libusb_transfer * LIBUSB_CALL libusb_alloc_transfer(int iso_packets)
{
    struct libusb_transfer *t;
    if (iso_packets || allocated >= 15) { fail("unexpected allocation"); }
    if (scenario == ALLOC_FAILURE && allocated == 5) { return NULL; }
    t = calloc(1, sizeof(*t));
    if (!t) { fail("out of memory"); }
    slots[allocated++].transfer = t;
    return t;
}

int LIBUSB_CALL libusb_submit_transfer(struct libusb_transfer *t)
{
    int i;
    for (i = 0; i < allocated; ++i) {
        if (slots[i].transfer != t) { continue; }
        ++slots[i].attempts;
        if (cancel_seen || device.async_status != RTLSDR_RUNNING) { ++resubmitted_after_stop; }
        if ((scenario == SUBMIT_FAILURE && i == 5)
            || (scenario == RESUBMIT_FAILURE && slots[i].attempts == 2)) {
            return LIBUSB_ERROR_IO;
        }
        slots[i].submitted = 1;
        slots[i].cancelling = 0;
        return 0;
    }
    fail("submitted unknown transfer");
    return -1;
}

int LIBUSB_CALL libusb_cancel_transfer(struct libusb_transfer *t)
{
    int i;
    for (i = 0; i < allocated; ++i) {
        if (slots[i].transfer != t) { continue; }
        if (!slots[i].submitted || slots[i].cancelling) {
            return LIBUSB_ERROR_NOT_FOUND;
        }
        slots[i].cancelling = 1;
        return 0;
    }
    fail("cancelled unknown transfer");
    return -1;
}

static void complete(int i, enum libusb_transfer_status status)
{
    if (!slots[i].submitted) { fail("callback without submission"); }
    slots[i].submitted = 0;
    slots[i].transfer->status = status;
    ++callbacks;
    slots[i].transfer->callback(slots[i].transfer);
}

static void stop_in_callback(unsigned char *buffer, uint32_t length, void *ctx)
{
    (void)buffer; (void)length; (void)ctx;
    cancel_seen = 1;
    rtlsdr_cancel_async(&device);
}

int LIBUSB_CALL libusb_handle_events_timeout_completed(libusb_context *ctx,
                                                      struct timeval *tv, int *completed)
{
    int i;
    (void)ctx; (void)tv;
    if (++polls > 200) { fail("did not drain/terminate"); }
    /* libusb must not dispatch events when the completed flag is already set. */
    if (completed && *completed) { return 0; }
    if (scenario == PERSISTENT_STALL) {
        /* Every running transfer keeps stalling and nothing cancels it from
         * outside. The driver must turn the repeating failure into device
         * loss; it must not resubmit forever. */
        for (i = 0; i < allocated; ++i) {
            if (slots[i].submitted && !slots[i].cancelling) {
                complete(i, LIBUSB_TRANSFER_STALL);
            }
        }
        for (i = 0; i < allocated; ++i) {
            if (slots[i].submitted && slots[i].cancelling) {
                complete(i, LIBUSB_TRANSFER_CANCELLED);
            }
        }
        return 0;
    }
    if (!injected) {
        injected = 1;
        if (scenario == EVENT_ERROR) { return LIBUSB_ERROR_IO; }
        if (scenario == DEVICE_LOST) {
            complete(0, LIBUSB_TRANSFER_NO_DEVICE);
            cancel_seen = 1;
            return 0;
        }
        if (scenario == SECOND_SESSION) {
            /* One stall, then a clean cancel, on a device whose previous
             * session ended in loss with 14 failures counted. */
            complete(0, LIBUSB_TRANSFER_STALL);
            cancel_seen = 1;
            rtlsdr_cancel_async(&device);
            return 0;
        }
        if (scenario == LOST_AGAIN) {
            /* A handle that lost the device before loses it again. */
            complete(0, LIBUSB_TRANSFER_NO_DEVICE);
            cancel_seen = 1;
            return 0;
        }
        if (scenario == RESUBMIT_FAILURE || scenario == CALLBACK_STOP) {
            complete(0, LIBUSB_TRANSFER_COMPLETED);
            cancel_seen = 1;
            return 0;
        }
        cancel_seen = 1;
        rtlsdr_cancel_async(&device);
        return 0;
    }
    /* Keep cancellations pending longer than the old loop's zero-time polls. */
    if (scenario != PROMPT && polls < 100) { return 0; }
    for (i = 0; i < allocated; ++i) {
        if (slots[i].submitted && slots[i].cancelling) {
            complete(i, scenario == LATE_COMPLETED
                     ? LIBUSB_TRANSFER_COMPLETED : LIBUSB_TRANSFER_CANCELLED);
        }
    }
    return 0;
}

void LIBUSB_CALL libusb_free_transfer(struct libusb_transfer *t)
{
    int i;
    if (!t) { return; }
    for (i = 0; i < allocated; ++i) {
        if (slots[i].transfer != t) { continue; }
        if (slots[i].submitted) { ++freed_pending; }
        ++freed;
        free(t);
        return;
    }
    fail("freed unknown transfer");
}

int main(void)
{
    int run, rc;
    for (run = 0; run < SCENARIO_COUNT; ++run) {
        scenario = (enum scenario)run;
        memset(&device, 0, sizeof(device));
        memset(slots, 0, sizeof(slots));
        allocated = polls = callbacks = freed_pending = freed = 0;
        resubmitted_after_stop = cancel_seen = injected = 0;
        if (scenario == SECOND_SESSION || scenario == LOST_AGAIN) {
            device.dev_lost = 1;
            device.xfer_errors = 14;
        }
        rc = rtlsdr_read_async(&device,
            scenario == CALLBACK_STOP ? stop_in_callback : NULL, NULL, 15, 16384);
        printf("scenario=%d rc=%d callbacks=%d freed_pending=%d freed=%d allocated=%d\n",
               scenario, rc, callbacks, freed_pending, freed, allocated);
        if (freed_pending) { fail("freed a submitted transfer before its terminal callback"); }
        if (freed != allocated) { fail("leaked allocated transfer"); }
        if (resubmitted_after_stop) { fail("resubmitted after cancellation began"); }
        if (device.async_status != RTLSDR_INACTIVE) { fail("did not become inactive"); }
        if ((scenario == EVENT_ERROR || scenario == SUBMIT_FAILURE
             || scenario == RESUBMIT_FAILURE) && rc != LIBUSB_ERROR_IO) {
            fail("lost transport error");
        }
        if (scenario == ALLOC_FAILURE && rc != LIBUSB_ERROR_NO_MEM) { fail("lost allocation error"); }
        if ((scenario == DEVICE_LOST || scenario == PERSISTENT_STALL
             || scenario == LOST_AGAIN)
            && rc != LIBUSB_ERROR_NO_DEVICE) {
            fail("device loss not reported");
        }
#ifdef _WIN32
        /* Any failure is device loss here, reported for this session, and
         * nothing is counted. */
        if (scenario == SECOND_SESSION
            && (rc != LIBUSB_ERROR_NO_DEVICE || device.xfer_errors != 0)) {
#else
        if (scenario == SECOND_SESSION && (rc != 0 || device.xfer_errors != 1)) {
#endif
            fail("previous session's loss or error count leaked into this one");
        }
    }
    puts("PASS: delayed/prompt cancellation, late completion, event/device/submit/resubmit errors, callback stop, partial allocation, persistent stall, second session, repeated loss");
    return 0;
}
