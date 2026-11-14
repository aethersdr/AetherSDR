// AetherGPS: shares a FlexRadio's GPS on the local network. Part of
// AetherSDR, GPL-3.0.
//
// The radio runs gpsd, but it listens on 127.0.0.1:2947 only. This relay runs
// on the radio as a legacy waveform (FLEX-6000 ARMv7, FLEX-8000/Aurora arm64,
// built statically) and offers the radio's GPS two standard ways:
//
//  1. The gpsd protocol on TCP 2947 at the radio's LAN address, so any gpsd
//     client (cgps, xgps, OpenCPN, chrony, AetherSDR) can connect. Each client
//     gets its own gpsd session. gpsd's replies pass through unchanged, but
//     only read commands reach gpsd: ?WATCH, ?POLL, ?VERSION and ?DEVICES.
//     Anything else, ?DEVICE above all (it reconfigures the receiver), is
//     answered with an ERROR and never sent.
//  2. NMEA 0183 sentences broadcast on UDP 10110 (IANA "nmea-0183") on the
//     LAN subnet, for listeners that take NMEA from the network.
//
// Only callers on a private, link-local or loopback address are served. The
// LAN address is the one on the default route's interface, in the gateway's
// subnet; the radio's other interfaces (internal links) are never used.
//
//   aether-gps [-a ADDR] [-b BCAST] [-l PORT] [-g GPSD_PORT] [-u UDP_PORT] [-s 0|1] [-i IDLE_MS] [-q QUIET_MS]
//
// -a/-b pin the listen and broadcast addresses (tests); -u 0 turns the NMEA
// broadcast off; -s 0 skips publishing the port through the tailnet shim;
// -i and -q shorten the 30 s idle and 60 s quiet limits (tests).
//
// 3. When the tailnet shim runs on the radio, the gpsd port is published on
//    the tailnet through it (POST /v1/local/expose on 48992), like AetherWSPR.

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define AGENT_VERSION "0.1.1"
#define MAX_CLIENTS 8
#define OUT_CAP 65536      // gpsd -> client backlog; reading pauses when full
#define CMD_CAP 1024       // one client command
#define NMEA_CAP 4096
#define RESCAN_MS 30000    // re-read the LAN address (DHCP may move it)
#define RETRY_MS 5000      // reconnect the NMEA feed

struct client {
    int down;              // the LAN client
    int up;                // its own gpsd session
    char out[OUT_CAP];
    size_t outlen;
    char cmd[CMD_CAP];
    size_t cmdlen;
    long long since;       // accepted at
    long long full_since;  // backlog full since, 0 when it isn't
    long long active;      // when bytes last crossed the session, either way
    int commanded;         // has sent an allowed command
};

static struct client g_clients[MAX_CLIENTS];
static int g_listen = -1, g_udp = -1, g_nmea = -1;
static long long g_idle_ms = 30000;           // first-command and full-backlog limit (-i)
static long long g_quiet_ms = 60000;          // nothing relayed either way for this long (-q)
static int g_shim = 1;                        // -s 0: no tailnet publishing (tests)
static int g_listen_port = 2947, g_gpsd_port = 2947, g_udp_port = 10110;
static struct in_addr g_addr, g_bcast;       // current LAN address and broadcast
static int g_pin_addr, g_pin_bcast;           // set by -a / -b
static struct in_addr g_pinned;               // the -a address
static long long g_publish_due;               // next tailnet shim registration
static char g_nmea_buf[NMEA_CAP];
static size_t g_nmea_len;

static long long now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void set_nonblock(int fd)
{
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
}

static int lan_caller(const struct sockaddr_in *a)
{
    unsigned long ip = ntohl(a->sin_addr.s_addr);
    return (ip >> 24) == 10 || (ip >> 24) == 127
        || (ip >> 20) == (172UL << 4 | 1)          // 172.16.0.0/12
        || (ip >> 16) == (192UL << 8 | 168)        // 192.168.0.0/16
        || (ip >> 16) == (169UL << 8 | 254);       // 169.254.0.0/16
}

// The default route's interface and gateway, from /proc/net/route. 0 if none.
static int default_route(char *iface, size_t cap, struct in_addr *gw)
{
    FILE *f = fopen("/proc/net/route", "r");
    if (!f) {
        return 0;
    }
    char line[256], name[IFNAMSIZ + 1];
    unsigned int dest, gate, flags;
    int found = 0;
    while (!found && fgets(line, sizeof line, f)) {
        if (sscanf(line, "%16s %x %x %x", name, &dest, &gate, &flags) == 4
            && dest == 0 && (flags & 0x2)) {   // RTF_GATEWAY
            snprintf(iface, cap, "%s", name);
            gw->s_addr = gate;                 // already network byte order
            found = 1;
        }
    }
    fclose(f);
    return found;
}

// The LAN address: on the default route's interface, in the gateway's subnet
// (an interface can carry an internal address too). 0 if there is none.
static int find_lan(struct in_addr *addr, struct in_addr *bcast)
{
    char iface[IFNAMSIZ + 1];
    struct in_addr gw;
    if (!default_route(iface, sizeof iface, &gw)) {
        return 0;
    }
    struct ifaddrs *list, *i;
    if (getifaddrs(&list) != 0) {
        return 0;
    }
    int found = 0;
    for (i = list; i && !found; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !i->ifa_netmask) {
            continue;
        }
        size_t n = strcspn(i->ifa_name, ":");   // "eth0:1" labels an alias of eth0
        if (n != strlen(iface) || strncmp(i->ifa_name, iface, n) != 0) {
            continue;
        }
        struct in_addr a = ((struct sockaddr_in *)i->ifa_addr)->sin_addr;
        struct in_addr m = ((struct sockaddr_in *)i->ifa_netmask)->sin_addr;
        if ((a.s_addr & m.s_addr) == (gw.s_addr & m.s_addr)) {
            *addr = a;
            bcast->s_addr = a.s_addr | ~m.s_addr;
            found = 1;
        }
    }
    freeifaddrs(list);
    return found;
}

static int connect_gpsd(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)g_gpsd_port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    // Loopback: refused or accepted at once, so a blocking connect is fine.
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        close(fd);
        return -1;
    }
    set_nonblock(fd);
    return fd;
}

static void rebind_listener(void)
{
    struct in_addr addr = g_pinned, bcast = g_bcast;
    if (!g_pin_addr && !find_lan(&addr, &bcast)) {
        addr.s_addr = 0;
    }
    if (!g_pin_bcast) {
        g_bcast = bcast;
    }
    if (g_listen >= 0 && addr.s_addr == g_addr.s_addr) {
        return;
    }
    if (g_listen >= 0) {
        close(g_listen);
        g_listen = -1;
    }
    g_addr = addr;
    if (addr.s_addr == 0) {
        return;
    }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)g_listen_port);
    sa.sin_addr = addr;
    if (fd < 0 || bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0 || listen(fd, 8) != 0) {
        fprintf(stderr, "aether-gps: cannot listen on %s:%d: %s\n", inet_ntoa(addr),
                g_listen_port, strerror(errno));
        if (fd >= 0) {
            close(fd);
        }
        g_addr.s_addr = 0;   // retried at the next rescan
        return;
    }
    set_nonblock(fd);
    g_listen = fd;
    g_publish_due = 0;   // a new socket: tell the shim now
    fprintf(stderr, "aether-gps: serving gpsd on %s:%d\n", inet_ntoa(addr), g_listen_port);
}

static void drop_client(struct client *c)
{
    if (c->down >= 0) {
        close(c->down);
    }
    if (c->up >= 0) {
        close(c->up);
    }
    c->down = c->up = -1;
    c->outlen = c->cmdlen = 0;
}

static void queue(struct client *c, const char *s, size_t n)
{
    if (n <= OUT_CAP - c->outlen) {
        memcpy(c->out + c->outlen, s, n);
        c->outlen += n;
    }
}

static const char *const ALLOWED[] = {"?WATCH", "?POLL", "?VERSION", "?DEVICES"};

// True for the read-only commands. The name must match exactly: "?DEVICES"
// is allowed, "?DEVICE" (which writes to the receiver) is not. Only ?WATCH
// takes an argument, "={...}". gpsd runs whatever follows a WATCH's JSON
// object on the same line (gpsd 3.22 answers `?WATCH={...}?VERSION;` with
// both), so a second '?' anywhere is refused: every gpsd command starts with
// one, and no WATCH value needs it. This holds without parsing JSON the way
// gpsd does.
static int allowed(const char *cmd, size_t n)
{
    if (n == 0 || cmd[0] != '?' || memchr(cmd + 1, '?', n - 1)) {
        return 0;
    }
    for (size_t i = 0; i < sizeof ALLOWED / sizeof *ALLOWED; ++i) {
        size_t k = strlen(ALLOWED[i]);
        if (n < k || memcmp(cmd, ALLOWED[i], k) != 0) {
            continue;
        }
        if (n == k) {
            return 1;
        }
        return i == 0 && n >= k + 3 && cmd[k] == '=' && cmd[k + 1] == '{' && cmd[n - 1] == '}';
    }
    return 0;
}

// Splits the client's bytes into commands (ended by ';' or a newline) and
// passes on the allowed ones. 0 when the client must be dropped.
static int client_input(struct client *c)
{
    char buf[512];
    ssize_t r = read(c->down, buf, sizeof buf);
    if (r <= 0) {
        return r < 0 && (errno == EAGAIN || errno == EINTR);
    }
    c->active = now_ms();
    for (ssize_t i = 0; i < r; ++i) {
        char ch = buf[i];
        if (ch != ';' && ch != '\n') {
            if (c->cmdlen == CMD_CAP) {
                return 0;   // no gpsd command is this long
            }
            c->cmd[c->cmdlen++] = ch;
            continue;
        }
        size_t s = 0, n = c->cmdlen;
        while (s < n && (c->cmd[s] == ' ' || c->cmd[s] == '\t' || c->cmd[s] == '\r')) {
            ++s;
        }
        while (n > s && (c->cmd[n - 1] == ' ' || c->cmd[n - 1] == '\r')) {
            --n;
        }
        c->cmdlen = 0;
        if (n == s) {
            continue;
        }
        if (allowed(c->cmd + s, n - s)) {
            c->commanded = 1;
            char line[CMD_CAP + 2];
            memcpy(line, c->cmd + s, n - s);
            line[n - s] = ';';
            line[n - s + 1] = '\n';
            if (send(c->up, line, n - s + 2, MSG_NOSIGNAL) != (ssize_t)(n - s + 2)) {
                return 0;
            }
        } else {
            static const char refused[] =
                "{\"class\":\"ERROR\",\"message\":\"AetherGPS is read-only: only ?WATCH, "
                "?POLL, ?VERSION and ?DEVICES are passed to gpsd\"}\r\n";
            queue(c, refused, sizeof refused - 1);
        }
    }
    return 1;
}

static void accept_client(void)
{
    struct sockaddr_in peer;
    socklen_t len = sizeof peer;
    int fd = accept(g_listen, (struct sockaddr *)&peer, &len);
    if (fd < 0) {
        return;
    }
    if (peer.sin_family != AF_INET || !lan_caller(&peer)) {
        close(fd);
        return;
    }
    struct client *c = NULL;
    for (int i = 0; i < MAX_CLIENTS && !c; ++i) {
        if (g_clients[i].down < 0) {
            c = &g_clients[i];
        }
    }
    if (!c) {
        close(fd);
        return;
    }
    set_nonblock(fd);
    c->down = fd;
    c->since = now_ms();
    c->active = c->since;
    c->full_since = 0;
    c->commanded = 0;
    c->up = connect_gpsd();
    if (c->up < 0) {
        static const char down[] =
            "{\"class\":\"ERROR\",\"message\":\"AetherGPS: gpsd is not running on the radio\"}\r\n";
        ssize_t w = send(fd, down, sizeof down - 1, MSG_NOSIGNAL);
        (void)w;
        drop_client(c);
    }
}

static void nmea_input(void)
{
    ssize_t r = read(g_nmea, g_nmea_buf + g_nmea_len, NMEA_CAP - g_nmea_len);
    if (r <= 0) {
        if (r == 0 || (errno != EAGAIN && errno != EINTR)) {
            close(g_nmea);
            g_nmea = -1;
            g_nmea_len = 0;
        }
        return;
    }
    g_nmea_len += (size_t)r;
    size_t start = 0;
    for (size_t i = 0; i < g_nmea_len; ++i) {
        if (g_nmea_buf[i] != '\n') {
            continue;
        }
        const char *line = g_nmea_buf + start;
        size_t n = i + 1 - start;
        start = i + 1;
        // Sentences only: the JSON lines gpsd sends at the start of a watch
        // are its own protocol, not NMEA.
        if ((line[0] == '$' || line[0] == '!') && n <= 256 && g_bcast.s_addr) {
            struct sockaddr_in to;
            memset(&to, 0, sizeof to);
            to.sin_family = AF_INET;
            to.sin_port = htons((unsigned short)g_udp_port);
            to.sin_addr = g_bcast;
            ssize_t w = sendto(g_udp, line, n, 0, (struct sockaddr *)&to, sizeof to);
            (void)w;
        }
    }
    memmove(g_nmea_buf, g_nmea_buf + start, g_nmea_len - start);
    g_nmea_len -= start;
    if (g_nmea_len == NMEA_CAP) {
        g_nmea_len = 0;   // a "line" this long is not NMEA
    }
}

static void open_nmea(void)
{
    g_nmea = connect_gpsd();
    if (g_nmea < 0) {
        return;
    }
    static const char watch[] = "?WATCH={\"enable\":true,\"nmea\":true};\n";
    if (send(g_nmea, watch, sizeof watch - 1, MSG_NOSIGNAL) != (ssize_t)(sizeof watch - 1)) {
        close(g_nmea);
        g_nmea = -1;
    }
}

// When the tailnet shim runs on this radio, publish the gpsd port on the
// tailnet so a remote operator's gpsd clients reach it too. The lease is 10
// minutes, renewed every 5; without a shim the request fails and is retried
// every minute. The shim carries TCP only, so the NMEA broadcast stays on
// the LAN. Loopback answers at once; the 500 ms timeouts bound how long a
// wedged shim can hold up the loop (and every relayed session) to about 1 s.
static void publish(long long now)
{
    if (g_listen < 0 || now < g_publish_due) {
        return;
    }
    int ok = 0;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct timeval tv = {0, 500000};
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons(48992);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (fd >= 0) {
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    }
    if (fd >= 0 && connect(fd, (struct sockaddr *)&sa, sizeof sa) == 0) {
        char body[64], req[256], resp[256];
        int bl = snprintf(body, sizeof body, "{\"port\":%d,\"name\":\"AetherGPS\"}",
                          g_listen_port);
        int l = snprintf(req, sizeof req,
                         "POST /v1/local/expose HTTP/1.0\r\nHost: 127.0.0.1\r\n"
                         "Content-Type: application/json\r\nContent-Length: %d\r\n\r\n%s",
                         bl, body);
        if (send(fd, req, (size_t)l, MSG_NOSIGNAL) == l) {
            ssize_t k = read(fd, resp, sizeof resp - 1);
            if (k > 0) {
                resp[k] = '\0';
                ok = strstr(resp, " 200 ") != NULL;
            }
        }
    }
    if (fd >= 0) {
        close(fd);
    }
    static int logged = -1;
    if (ok != logged) {
        fprintf(stderr, ok ? "aether-gps: published on the tailnet by the shim\n"
                           : "aether-gps: no tailnet shim to publish to\n");
        logged = ok;
    }
    g_publish_due = now + (ok ? 300000 : 60000);
}

int main(int argc, char **argv)
{
    for (int a = 1; a + 1 < argc; a += 2) {
        const char *v = argv[a + 1];
        if (strcmp(argv[a], "-a") == 0 && inet_aton(v, &g_pinned)) {
            g_pin_addr = 1;
        } else if (strcmp(argv[a], "-b") == 0 && inet_aton(v, &g_bcast)) {
            g_pin_bcast = 1;
        } else if (strcmp(argv[a], "-l") == 0) {
            g_listen_port = atoi(v);
        } else if (strcmp(argv[a], "-g") == 0) {
            g_gpsd_port = atoi(v);
        } else if (strcmp(argv[a], "-i") == 0) {
            g_idle_ms = atoll(v) > 0 ? atoll(v) : 30000;
        } else if (strcmp(argv[a], "-q") == 0) {
            g_quiet_ms = atoll(v) > 0 ? atoll(v) : 60000;
        } else if (strcmp(argv[a], "-s") == 0) {
            g_shim = atoi(v) != 0;
        } else if (strcmp(argv[a], "-u") == 0) {
            g_udp_port = atoi(v);
        } else {
            fprintf(stderr, "aether-gps: unknown option %s\n", argv[a]);
            return 2;
        }
    }
    signal(SIGPIPE, SIG_IGN);
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        g_clients[i].down = g_clients[i].up = -1;
    }
    if (g_udp_port > 0) {
        g_udp = socket(AF_INET, SOCK_DGRAM, 0);
        int one = 1;
        if (g_udp >= 0) {
            setsockopt(g_udp, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
            set_nonblock(g_udp);
        }
    }

    rebind_listener();
    long long last_scan = now_ms(), last_retry = 0;

    for (;;) {
        struct pollfd p[2 + 2 * MAX_CLIENTS];
        struct client *owner[2 + 2 * MAX_CLIENTS];
        int np = 0;
        if (g_listen >= 0) {
            p[np] = (struct pollfd){g_listen, POLLIN, 0};
            owner[np++] = NULL;
        }
        int nmea_slot = -1;
        if (g_nmea >= 0) {
            nmea_slot = np;
            p[np] = (struct pollfd){g_nmea, POLLIN, 0};
            owner[np++] = NULL;
        }
        for (int i = 0; i < MAX_CLIENTS; ++i) {
            struct client *c = &g_clients[i];
            if (c->down < 0) {
                continue;
            }
            p[np] = (struct pollfd){c->down, (short)(POLLIN | (c->outlen ? POLLOUT : 0)), 0};
            owner[np++] = c;
            // gpsd's output pauses while this client's backlog is full.
            p[np] = (struct pollfd){c->up, (short)(c->outlen < OUT_CAP ? POLLIN : 0), 0};
            owner[np++] = c;
        }
        int r = poll(p, (nfds_t)np, 1000);
        long long now = now_ms();
        if (now - last_scan >= RESCAN_MS) {
            rebind_listener();
            last_scan = now;
        }
        if (g_shim) {
            publish(now);
        }
        if (g_udp >= 0 && g_nmea < 0 && now - last_retry >= RETRY_MS) {
            open_nmea();
            last_retry = now;
        }
        // Eight slots are all there is. A client that sends no allowed
        // command within the limit gives its slot back; so does one whose
        // backlog here stays full that long. A client that has stopped
        // reading is seen only once its backlog fills, and a quiet client
        // that once sent a command is kept.
        for (int i = 0; i < MAX_CLIENTS; ++i) {
            struct client *c = &g_clients[i];
            if (c->down < 0) {
                continue;
            }
            if (c->outlen < OUT_CAP) {
                c->full_since = 0;
            } else if (!c->full_since) {
                c->full_since = now;
            }
            // A client that sent one ?VERSION and went quiet is "commanded"
            // and never fills its backlog; nothing crossing the session for
            // g_quiet_ms frees its slot too. A watching client gets reports
            // every second, so it never trips this.
            if ((!c->commanded && now - c->since >= g_idle_ms)
                || (c->full_since && now - c->full_since >= g_idle_ms)
                || now - c->active >= g_quiet_ms) {
                drop_client(c);
            }
        }
        if (r <= 0) {
            continue;
        }
        for (int k = 0; k < np; ++k) {
            if (!p[k].revents) {
                continue;
            }
            struct client *c = owner[k];
            if (!c) {
                if (k == nmea_slot) {
                    if (g_nmea >= 0) {
                        nmea_input();
                    }
                } else if (p[k].fd == g_listen) {
                    accept_client();
                }
                continue;
            }
            if (c->down < 0) {
                continue;   // dropped earlier in this pass
            }
            int ok = 1;
            if (p[k].fd == c->down) {
                if (p[k].revents & (POLLIN | POLLHUP | POLLERR)) {
                    ok = client_input(c);
                }
                if (ok && (p[k].revents & POLLOUT) && c->outlen) {
                    ssize_t w = send(c->down, c->out, c->outlen, MSG_NOSIGNAL);
                    if (w > 0) {
                        memmove(c->out, c->out + w, c->outlen - (size_t)w);
                        c->outlen -= (size_t)w;
                        c->active = now;
                    } else if (w < 0 && errno != EAGAIN && errno != EINTR) {
                        ok = 0;
                    }
                }
            } else if (p[k].fd == c->up) {
                if (c->outlen < OUT_CAP) {
                    ssize_t n = read(c->up, c->out + c->outlen, OUT_CAP - c->outlen);
                    if (n > 0) {
                        c->outlen += (size_t)n;
                        c->active = now;
                    } else if (n == 0 || (errno != EAGAIN && errno != EINTR)) {
                        ok = 0;   // gpsd closed the session
                    }
                } else if (p[k].revents & (POLLHUP | POLLERR)) {
                    // poll() reports a hangup even with no events asked for;
                    // left unhandled it would wake the loop forever. gpsd has
                    // dropped this stalled client, so do the same.
                    ok = 0;
                }
            }
            if (!ok) {
                drop_client(c);
            }
        }
    }
}
