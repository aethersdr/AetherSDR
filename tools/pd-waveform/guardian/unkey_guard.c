#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/capability.h>
#include <poll.h>
#include <stdint.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

enum { kLineBytes = 4096, kBind = 910000, kSubscribe = 910001, kRefresh = 910002, kUnkey = 910003,
       kCleanupNominalMs = 3000 };
struct unkey_state {
    uint32_t client, owner, query;
    int acknowledged, rejected, binding_acknowledged, binding_rejected, valid, idle, software_owner, unkeying;
    int bounded, owned_seen, observation_ended;
};
struct unkey_input { char line[kLineBytes]; size_t used; int discard; };

static int handle(const char* text, uint32_t* value)
{
    if (strlen(text) != 10 || text[0] != '0' || text[1] != 'x') { return 0; }
    uint32_t result = 0;
    for (unsigned i = 2; i < 10; ++i) {
        unsigned c = (unsigned char)text[i];
        if (c >= '0' && c <= '9') { c -= '0'; }
        else if (c >= 'a' && c <= 'f') { c = c - 'a' + 10; }
        else if (c >= 'A' && c <= 'F') { c = c - 'A' + 10; }
        else { return 0; }
        result = (result << 4) | c;
    }
    *value = result;
    return 1;
}
static int field(const char* body, const char* name, char* value, size_t capacity)
{
    size_t key = strlen(name);
    int found = 0;
    while (*body) {
        while (*body == ' ') { ++body; }
        if (!*body) { break; }
        const char* end = strchr(body, ' ');
        if (!end) { end = body + strlen(body); }
        size_t count = (size_t)(end - body);
        if (count > key && !memcmp(body, name, key) && body[key] == '=') {
            size_t size = count - key - 1;
            if (found || size >= capacity) { return -1; }
            memcpy(value, body + key + 1, size); value[size] = 0; found = 1;
        }
        body = end;
    }
    return found;
}
static int bounded_duration(const char* text, int* duration)
{
    if (!text || !text[0] || text[0] == '0' || strlen(text) > 5) { return 0; }
    unsigned value = 0;
    for (const char* p = text; *p; ++p) {
        if (*p < '0' || *p > '9') { return 0; }
        value = value * 10 + (unsigned)(*p - '0');
    }
    if (!value || value > 15000) { return 0; }
    *duration = (int)value;
    return 1;
}
struct bounded_deadline { int64_t due, last; };
static int deadline_start(struct bounded_deadline* deadline, int duration, int64_t now)
{
    if (duration < 1 || duration > 15000 || now <= 0 || now > INT64_MAX - duration) { return 0; }
    deadline->due = now + duration;
    deadline->last = now;
    return 1;
}
static int deadline_wait(struct bounded_deadline* deadline, int64_t now)
{
    if (now < deadline->last || now >= deadline->due) { return 0; }
    deadline->last = now;
    int64_t left = deadline->due - now;
    return left < 500 ? (int)left : 500;
}
static void invalid(struct unkey_state* state)
{
    state->valid = state->idle = state->software_owner = state->unkeying = 0;
    if (state->bounded) { state->observation_ended = 1; }
}
static void parse(struct unkey_state* state, const char* line)
{
    if (line[0] == 'R') {
        const char* p = line + 1;
        if (*p < '0' || *p > '9') { return; }
        char* end; errno = 0;
        unsigned long sequence = strtoul(p, &end, 10);
        if (errno || end - p > 10 || *end != '|' || (sequence != state->query && sequence != kBind)) { return; }
        int binding = sequence == kBind;
        p = end + 1;
        if (*p < '0' || *p > '9') { state->rejected = 1; return; }
        errno = 0; unsigned long code = strtoul(p, &end, 16);
        if (errno || end - p > 8 || *end != '|' || code > UINT32_MAX) { state->rejected = 1; return; }
        for (const char* c = p; c < end; ++c) {
            if (!((*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'f') || (*c >= 'A' && *c <= 'F'))) { state->rejected = 1; return; }
        }
        if (binding) { state->binding_acknowledged = code == 0; state->binding_rejected = code != 0; }
        else { state->acknowledged = code == 0; state->rejected = code != 0; }
        return;
    }
    if (line[0] != 'S') { return; }
    const char* body = strchr(line, '|');
    if (!body || body - line < 2 || body - line > 9) { invalid(state); return; }
    char status_handle[11] = "0x00000000";
    size_t digits = (size_t)(body - line - 1);
    memcpy(status_handle + 10 - digits, line + 1, digits);
    uint32_t sender;
    if (!handle(status_handle, &sender)) { invalid(state); return; }
    if (sender != 0 && sender != state->client) { return; }
    ++body;
    if (strncmp(body, "interlock ", 10) || !strncmp(body + 10, "band ", 5)) { return; }
    if (strchr(body, 127)) { invalid(state); return; }
    char mode[32], source[32], owner[32], allowed[8], reason[64];
    int a = field(body + 10, "state", mode, sizeof(mode));
    int b = field(body + 10, "source", source, sizeof(source));
    int c = field(body + 10, "tx_client_handle", owner, sizeof(owner));
    int d = field(body + 10, "tx_allowed", allowed, sizeof(allowed));
    int e = field(body + 10, "reason", reason, sizeof(reason));
    if (!a && !b && !c && !d && !e) { return; }
    state->valid = state->idle = state->software_owner = state->unkeying = 0;
    uint32_t tx_owner;
    if (a != 1 || b != 1 || c != 1 || d != 1 || e != 1 || !handle(owner, &tx_owner)) { invalid(state); return; }
    if (strcmp(allowed, "0") && strcmp(allowed, "1")) { invalid(state); return; }
    state->valid = 1;
    state->idle = !strcmp(mode, "READY") && !source[0] && !reason[0] && !strcmp(allowed, "1") && tx_owner == 0;
    state->software_owner = tx_owner == state->owner && !strcmp(source, "SW") &&
        (!strcmp(mode, "TRANSMITTING") || !strcmp(mode, "PTT_REQUESTED"));
    state->unkeying = tx_owner == state->owner && !source[0] && !strcmp(mode, "UNKEY_REQUESTED");
    if (state->bounded) {
        if (state->unkeying || (state->owned_seen && !state->software_owner)) { state->observation_ended = 1; }
        if (state->software_owner) { state->owned_seen = 1; }
        if (!state->idle && !state->software_owner && !state->unkeying) { state->observation_ended = 1; }
    }
}
static void feed(struct unkey_state* state, struct unkey_input* input, const unsigned char* data, size_t size)
{
    for (size_t i = 0; i < size; ++i) {
        unsigned c = data[i];
        if (c == '\n') {
            if (input->discard) { invalid(state); }
            else {
                if (input->used && input->line[input->used - 1] == '\r') { --input->used; }
                input->line[input->used] = 0;
                if (memchr(input->line, '\r', input->used)) { invalid(state); }
                else { parse(state, input->line); }
            }
            input->used = 0; input->discard = 0;
        } else if ((c < 32 && c != '\r') || c > 127 || input->used >= sizeof(input->line) - 1) {
            input->discard = 1;
        } else if (!input->discard) { input->line[input->used++] = (char)c; }
    }
}
static int64_t milliseconds(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) { return -1; }
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}
static int before_deadline(int64_t start, int duration)
{
    int64_t now = milliseconds();
    return now >= start && now - start < duration;
}
static void event(const char* name)
{
    char line[160]; int size = snprintf(line, sizeof(line), "{\"event\":\"%s\",\"keys_transmitter\":false}\n", name);
    if (size > 0 && size < (int)sizeof(line)) { (void)write(1, line, (size_t)size); }
}
static void bounded_ready(int64_t due)
{
    char line[192];
    int size = snprintf(line, sizeof(line), "{\"event\":\"bounded_ready\",\"cleanup_due_ms\":\"%lld\",\"keys_transmitter\":false}\n", (long long)due);
    if (size > 0 && size < (int)sizeof(line)) { (void)write(1, line, (size_t)size); }
}
static int send_command(int fd, const char* text)
{
    size_t remaining = strlen(text);
    struct bounded_deadline deadline;
    if (!deadline_start(&deadline, 500, milliseconds())) { return 0; }
    int wait;
    while (remaining && (wait = deadline_wait(&deadline, milliseconds())) > 0) {
        ssize_t sent = send(fd, text, remaining, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (sent > 0) { text += sent; remaining -= (size_t)sent; }
        else if (sent < 0 && errno == EINTR) { continue; }
        else if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd output = {fd, POLLOUT, 0}; (void)poll(&output, 1, wait < 10 ? wait : 10);
        } else { return 0; }
    }
    return remaining == 0 && deadline_wait(&deadline, milliseconds()) > 0;
}
static int receive_status(int fd, struct unkey_state* state, struct unkey_input* input)
{
    unsigned char bytes[8192];
    for (unsigned batch = 0; batch < 32; ++batch) {
        ssize_t size = recv(fd, bytes, sizeof(bytes), MSG_DONTWAIT);
        if (size > 0) { feed(state, input, bytes, (size_t)size); }
        else if (size < 0 && errno == EINTR) { continue; }
        else if (size < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { return 1; }
        else { return 0; }
    }
    return 2;
}
static int query(int fd, struct unkey_state* state, struct unkey_input* input, uint32_t sequence)
{
    state->query = sequence; state->acknowledged = state->rejected = 0;
    if (sequence == kSubscribe) { state->valid = state->idle = state->software_owner = state->unkeying = 0; }
    const char* text = sequence == kSubscribe ? "C910001|sub tx all\n" : "C910002|sub tx all\n";
    if (!send_command(fd, text)) { return 0; }
    struct bounded_deadline deadline;
    if (!deadline_start(&deadline, 2000, milliseconds())) { return 0; }
    int wait;
    while ((wait = deadline_wait(&deadline, milliseconds())) > 0) {
        struct pollfd socket = {fd, POLLIN, 0}; int ready = poll(&socket, 1, wait < 50 ? wait : 50);
        if (ready < 0 && errno == EINTR) { continue; }
        if (ready < 0) { return 0; }
        int drained = ready ? receive_status(fd, state, input) : 1;
        if (!drained) { return 0; }
        const int timely = deadline_wait(&deadline, milliseconds()) > 0;
        // Startup readiness can permit positive keying. An overdue refresh
        // may still authorize best-effort unkey after the ownership barrier.
        if (!timely && sequence == kSubscribe) { return 0; }
        if (state->rejected || state->binding_rejected) { return 0; }
        if (state->acknowledged && state->binding_acknowledged && state->valid
            && drained == 1 && !input->used && !input->discard) { return 1; }
        if (!timely) { return 0; }
    }
    return 0;
}
static int run(int fd, int life, uint32_t client, uint32_t owner, int duration)
{
    struct unkey_state state = {.client = client, .owner = owner, .bounded = duration != 0};
    struct unkey_input input = {0};
    struct bounded_deadline deadline = {0};
    if (!query(fd, &state, &input, kSubscribe)) { event("initial_status_unknown"); return 5; }
    if (!state.idle || state.owned_seen || state.observation_ended) { event("initial_radio_not_idle"); return 5; }
    if (duration) {
        if (!deadline_start(&deadline, duration, milliseconds())) {
            event("deadline_invalid"); return 5;
        }
        bounded_ready(deadline.due);
    } else { event("ready"); }
    for (;;) {
        int wait_ms = duration ? deadline_wait(&deadline, milliseconds()) : 500;
        if (!wait_ms) { event("bounded_deadline_reached"); break; }
        struct pollfd fds[2] = {{life, 0, 0}, {fd, POLLIN, 0}};
        int ready = poll(fds, 2, wait_ms);
        if (ready < 0 && errno == EINTR) { continue; }
        if (ready < 0 || fds[0].revents & (POLLERR | POLLNVAL)) { event("lifetime_error"); return 5; }
        if (fds[1].revents && !receive_status(fd, &state, &input)) { event("api_lost"); return 7; }
        if (state.observation_ended) { event("bounded_observation_ended"); return 0; }
        if (fds[0].revents & POLLHUP) { break; }
    }
    // Cleanup remains best effort after scheduling delays. Expiry cannot
    // suppress an ownership-validated unkey or certify RF release.
    const int64_t cleanup_start = milliseconds();
    if (cleanup_start <= 0) { event("cleanup_clock_invalid"); return 5; }
    const int64_t cleanup_due = duration && deadline.due < cleanup_start ? deadline.due : cleanup_start;
    // The connection retains its initial snapshot and every subsequent update.
    // SmartSDR4.3.16 does not replay interlock state on repeated subscriptions.
    // Drain queued updates, then wait for the subscription acknowledgment barrier.
    if (receive_status(fd, &state, &input) != 1 || !query(fd, &state, &input, kRefresh)) { event("final_status_unknown"); return 5; }
    if (state.observation_ended) { event("bounded_observation_ended"); return 0; }
    if (state.idle || state.unkeying) { event("already_safe_or_unkeying"); return 0; }
    if (!state.software_owner) { event("unkey_ownership_mismatch"); return 4; }
    // FlexLib Radio.Mox encodes this command. No path encodes xmit1 or tune.
    if (!send_command(fd, "C910003|xmit 0\n")) { event("unkey_write_failed"); return 5; }
    const int64_t dispatched = milliseconds();
    if (dispatched >= cleanup_due && dispatched - cleanup_due >= kCleanupNominalMs) { event("cleanup_delayed"); }
    event("unkey_requested"); state.query = kUnkey; state.acknowledged = state.rejected = 0;
    int64_t start = milliseconds();
    if (start < 0) { return 5; }
    while (before_deadline(start, 2000)) {
        struct pollfd socket = {fd, POLLIN, 0}; (void)poll(&socket, 1, 50);
        if (!receive_status(fd, &state, &input) || state.rejected) { event("unkey_ack_failed"); return 5; }
        if (state.acknowledged) { event("unkey_acknowledged"); return 0; }
    }
    event("unkey_ack_timeout"); return 5;
}
int main(int argc, char** argv)
{
    // Go owns the diagnostics reader; losing it must not interrupt unkey.
    signal(SIGPIPE, SIG_IGN);
    if ((argc != 5 && argc != 6) || getuid() == 0 || getuid() != geteuid() || prctl(PR_SET_DUMPABLE, 0, 0, 0, 0)) { return 2; }
    int duration = 0;
    if (argc == 6 && !bounded_duration(argv[5], &duration)) { return 2; }
    // Linux2.6 lacks no_new_privs. This helper never execs or gains capabilities.
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) && errno != EINVAL) { return 2; }
    int values[2];
    for (unsigned i = 0; i < 2; ++i) {
        char* end; errno = 0; long value = strtol(argv[i + 1], &end, 10);
        if (errno || !argv[i + 1][0] || *end || value < 3 || value > 1023) { return 2; }
        for (const char* c = argv[i + 1]; *c; ++c) { if (*c < '0' || *c > '9') { return 2; } }
        values[i] = (int)value;
    }
    uint32_t client, owner;
    if (!handle(argv[3], &client) || !handle(argv[4], &owner) || !client || !owner || values[0] == values[1]) { return 2; }
    struct __user_cap_header_struct header = {_LINUX_CAPABILITY_VERSION_3, 0};
    struct __user_cap_data_struct caps[2] = {{0}};
    if (syscall(SYS_capget, &header, caps) || caps[0].effective || caps[0].permitted || caps[0].inheritable || caps[1].effective || caps[1].permitted || caps[1].inheritable) { event("capabilities_invalid"); return 3; }
    struct sockaddr_in peer; socklen_t size = sizeof(peer); int type = 0; socklen_t length = sizeof(type);
    int fd = values[0], life = values[1]; struct stat info;
    if (getpeername(fd, (struct sockaddr*)&peer, &size) || size != sizeof(peer) || peer.sin_family != AF_INET || peer.sin_addr.s_addr != htonl(INADDR_LOOPBACK) || peer.sin_port != htons(4992) || getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &length) || type != SOCK_STREAM) { event("api_descriptor_invalid"); return 3; }
    if (fstat(life, &info) || !S_ISFIFO(info.st_mode) || (fcntl(life, F_GETFL) & O_ACCMODE) != O_RDONLY || fstat(1, &info) || !S_ISFIFO(info.st_mode)) { event("lifetime_or_output_invalid"); return 3; }
    return run(fd, life, client, owner, duration);
}
