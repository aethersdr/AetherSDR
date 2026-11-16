/* Injected syscalls: this test opens no sockets and never reaches a radio. */
#define _GNU_SOURCE
#include <poll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

int fixture_clock_gettime(clockid_t id, struct timespec* now);
ssize_t fixture_send(int fd, const void* data, size_t size, int flags);
ssize_t fixture_recv(int fd, void* data, size_t capacity, int flags);
int fixture_poll(struct pollfd* fds, nfds_t count, int timeout);
ssize_t fixture_write(int fd, const void* bytes, size_t size);

#define clock_gettime fixture_clock_gettime
#define send fixture_send
#define recv fixture_recv
#define poll fixture_poll
#define write fixture_write
#define main unkey_guard_program_main
#include "unkey_guard.c"
#undef main
#undef write
#undef poll
#undef recv
#undef send
#undef clock_gettime
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>

static struct {
    int64_t now, cleanup_due, unkey_at;
    int startup_stall, cleanup_stall, refresh_delay, cleanup_event_stall, unkey_write_stall;
    int stage, stalled, unkeys, foreign, refreshes, delayed;
    char pending[1024];
} fixture;

static void queue(const char* text)
{
    assert(!fixture.pending[0]);
    assert(strlen(text) < sizeof(fixture.pending));
    strcpy(fixture.pending, text);
}

int fixture_clock_gettime(clockid_t id, struct timespec* now)
{
    assert(id == CLOCK_MONOTONIC);
    now->tv_sec = fixture.now / 1000;
    now->tv_nsec = fixture.now % 1000 * 1000000;
    return 0;
}

ssize_t fixture_send(int fd, const void* data, size_t size, int flags)
{
    assert(fd == 41 && flags == (MSG_NOSIGNAL | MSG_DONTWAIT));
    if (size == strlen("C910001|sub tx all\n") && !memcmp(data, "C910001|sub tx all\n", size)) {
        fixture.stage = 1;
        queue("S0|interlock tx_client_handle=0x00000000 state=READY reason= source= tx_allowed=1\n"
              "R910000|0|\nR910001|0|\n");
    } else if (size == strlen("C910002|sub tx all\n") && !memcmp(data, "C910002|sub tx all\n", size)) {
        ++fixture.refreshes;
        fixture.stage = 3;
        fixture.stalled = 0;
    } else {
        assert(size == strlen("C910003|xmit 0\n") && !memcmp(data, "C910003|xmit 0\n", size));
        if (fixture.unkey_write_stall) {
            fixture.stage = 5;
            errno = EAGAIN;
            return -1;
        }
        ++fixture.unkeys;
        fixture.unkey_at = fixture.now;
        fixture.stage = 4;
        queue("R910003|0|\n");
    }
    return (ssize_t)size;
}

ssize_t fixture_recv(int fd, void* data, size_t capacity, int flags)
{
    assert(fd == 41 && flags == MSG_DONTWAIT);
    if (!fixture.pending[0]) { errno = EAGAIN; return -1; }
    const size_t size = strlen(fixture.pending);
    assert(size <= capacity);
    memcpy(data, fixture.pending, size);
    fixture.pending[0] = 0;
    return (ssize_t)size;
}

int fixture_poll(struct pollfd* fds, nfds_t count, int timeout)
{
    assert(timeout >= 0);
    for (nfds_t i = 0; i < count; ++i) { fds[i].revents = 0; }
    if (fixture.stage == 5) {
        assert(count == 1 && fds[0].fd == 41 && fds[0].events == POLLOUT);
        fixture.now += fixture.unkey_write_stall;
        fixture.unkey_write_stall = 0;
        fds[0].revents = POLLOUT;
        return 1;
    }
    if (fixture.stage == 1 && !fixture.stalled && fixture.startup_stall) {
        fixture.now += fixture.startup_stall;
        fixture.stalled = 1;
    }
    if (fixture.stage == 3 && !fixture.stalled) {
        fixture.now += fixture.cleanup_stall;
        fixture.stalled = 1;
    }
    if (fixture.stage == 3 && !fixture.pending[0]) {
        if (fixture.refresh_delay > 0) {
            const int elapsed = timeout < fixture.refresh_delay ? timeout : fixture.refresh_delay;
            fixture.now += elapsed;
            fixture.refresh_delay -= elapsed;
            if (fixture.refresh_delay) { return 0; }
        }
        queue(fixture.foreign
            ? "S0|interlock tx_client_handle=0x00009999 state=TRANSMITTING reason= source=SW tx_allowed=1\nR910002|0|\n"
            : "R910002|0|\n");
    }
    if (fixture.pending[0]) {
        for (nfds_t i = 0; i < count; ++i) {
            if (fds[i].fd == 41 && (fds[i].events & POLLIN)) { fds[i].revents = POLLIN; return 1; }
        }
    }
    fixture.now += timeout;
    return 0;
}

ssize_t fixture_write(int fd, const void* bytes, size_t size)
{
    assert(fd == 1);
    char text[256];
    assert(size < sizeof(text));
    memcpy(text, bytes, size); text[size] = 0;
    if (strstr(text, "cleanup_delayed")) { ++fixture.delayed; }
    if (strstr(text, "bounded_deadline_reached")) { fixture.now += fixture.cleanup_event_stall; }
    if (strstr(text, "bounded_ready")) {
        const char* value = strstr(text, "cleanup_due_ms\":\"");
        assert(value);
        fixture.cleanup_due = strtoll(value + strlen("cleanup_due_ms\":\""), NULL, 10);
        fixture.stage = 2;
        queue("S1234|interlock tx_client_handle=0x00005678 state=TRANSMITTING reason= source=SW tx_allowed=1\n");
    }
    return (ssize_t)size;
}

static void reset(void)
{
    memset(&fixture, 0, sizeof(fixture));
    fixture.now = 100000;
}

int main(void)
{
    reset();
    assert(run(41, 42, 0x1234, 0x5678, 9500) == 0);
    assert(fixture.unkeys == 1 && fixture.unkey_at == fixture.cleanup_due);
    reset();
    fixture.refresh_delay = 1900;
    assert(run(41, 42, 0x1234, 0x5678, 9500) == 0);
    assert(fixture.unkeys == 1 && fixture.unkey_at == fixture.cleanup_due + 1900);
    printf("cleanup initiation is not unkey: delayed ACK adds %lld ms\n",
           (long long)(fixture.unkey_at - fixture.cleanup_due));
    reset();
    fixture.refresh_delay = 1999;
    assert(run(41, 42, 0x1234, 0x5678, 9500) == 0 && fixture.unkeys == 1);
    reset();
    fixture.refresh_delay = 2000;
    assert(run(41, 42, 0x1234, 0x5678, 9500) == 0 && fixture.unkeys == 1);
    reset();
    fixture.foreign = 1;
    assert(run(41, 42, 0x1234, 0x5678, 9500) == 0 && !fixture.unkeys);
    reset();
    fixture.startup_stall = 2500;
    assert(run(41, 42, 0x1234, 0x5678, 9500) == 5 && !fixture.cleanup_due && !fixture.unkeys);
    reset();
    fixture.cleanup_stall = 3500;
    assert(run(41, 42, 0x1234, 0x5678, 9500) == 0 && fixture.unkeys == 1 && fixture.delayed == 1);
    assert(fixture.unkey_at == fixture.cleanup_due + 3500);
    reset();
    fixture.cleanup_event_stall = 3000;
    assert(run(41, 42, 0x1234, 0x5678, 9500) == 0 && fixture.refreshes == 1 && fixture.unkeys == 1 && fixture.delayed == 1);
    reset();
    fixture.unkey_write_stall = 500;
    assert(run(41, 42, 0x1234, 0x5678, 9500) == 5 && !fixture.unkeys);
    reset();
    fixture.unkey_write_stall = 499;
    assert(run(41, 42, 0x1234, 0x5678, 9500) == 0 && fixture.unkeys == 1);
    reset();
    fixture.cleanup_event_stall = 1800;
    fixture.refresh_delay = 1250;
    assert(run(41, 42, 0x1234, 0x5678, 9500) == 0 && fixture.unkeys == 1 && fixture.delayed == 1);
    puts("unkey guard injected-clock deadline tests passed");
    return 0;
}
