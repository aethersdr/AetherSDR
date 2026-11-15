// AetherHealth: reports a FlexRadio's free waveform storage, memory and CPU
// to AetherSDR on the local network. Part of AetherSDR, GPL-3.0.
//
// It runs as a legacy waveform on FLEX-6000 (ARMv7) and FLEX-8000/Aurora
// (arm64) radios, and is built statically so it needs nothing from the radio.
// Each TCP connection on port 48994 gets one JSON line and is closed:
//
//   {"agent":"0.1.0","cpu_idle_pct":48,"cpu_cores":1,"mem_total_kb":480512,
//    "mem_available_kb":301234,"mem_estimated":false,
//    "storage":[{"path":"/user","free_bytes":6815744,"total_bytes":19922944}]}
//
// It reads, never writes: no command is accepted, and only callers on a
// private, link-local or loopback address are answered.
//
//   aether-health [-p PORT] PATH...   (free space is reported for each PATH
//                                      that can be read; others are skipped)

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/statfs.h>
#include <time.h>
#include <unistd.h>

#define AGENT_VERSION "0.1.0"
#define DEFAULT_PORT 48994
#define SAMPLE_MS 5000
#define MAX_PATHS 4

static unsigned long long g_idle, g_total;   // last /proc/stat sample
static int g_idle_pct = -1;                  // -1 until two samples exist
static int g_cores;
static char g_stat[256] = "/proc/stat", g_meminfo[256] = "/proc/meminfo";

// Reads up to cap-1 bytes of a /proc file. 0 on failure.
static size_t read_file(const char *path, char *buf, size_t cap)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return 0;
    }
    size_t n = 0;
    while (n < cap - 1) {
        ssize_t r = read(fd, buf + n, cap - 1 - n);
        if (r <= 0) {
            break;
        }
        n += (size_t)r;
    }
    close(fd);
    buf[n] = '\0';
    return n;
}

static void sample_cpu(void)
{
    char buf[4096];
    if (!read_file(g_stat, buf, sizeof buf) || strncmp(buf, "cpu ", 4) != 0) {
        return;
    }
    unsigned long long v[8] = {0};
    sscanf(buf + 4, "%llu %llu %llu %llu %llu %llu %llu %llu",
           &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
    unsigned long long idle = v[3] + v[4], total = 0;   // idle + iowait
    for (int i = 0; i < 8; ++i) {
        total += v[i];
    }
    if (g_total && total > g_total && idle >= g_idle) {
        g_idle_pct = (int)(100 * (idle - g_idle) / (total - g_total));
    }
    g_idle = idle;
    g_total = total;

    int cores = 0;
    for (const char *p = buf; (p = strstr(p, "\ncpu")) != NULL; ++p) {
        if (p[4] >= '0' && p[4] <= '9') {
            ++cores;
        }
    }
    g_cores = cores;
}

// A /proc/meminfo value in kB, or -1 when absent.
static long long meminfo(const char *buf, const char *key)
{
    const char *p = buf;
    while ((p = strstr(p, key)) != NULL && p != buf && p[-1] != '\n') {
        ++p;   // "Cached:" inside "SwapCached:"
    }
    if (!p) {
        return -1;
    }
    long long kb = -1;
    sscanf(p + strlen(key), " %lld", &kb);
    return kb;
}

static int lan_caller(const struct sockaddr_in *a)
{
    unsigned long ip = ntohl(a->sin_addr.s_addr);
    return (ip >> 24) == 10 || (ip >> 24) == 127
        || (ip >> 20) == (172UL << 4 | 1)          // 172.16.0.0/12
        || (ip >> 16) == (192UL << 8 | 168)        // 192.168.0.0/16
        || (ip >> 16) == (169UL << 8 | 254);       // 169.254.0.0/16
}

static void answer(int fd, char **paths, int npaths)
{
    char mem[4096], out[1024];
    long long total = -1, avail = -1;
    int estimated = 0;
    if (read_file(g_meminfo, mem, sizeof mem)) {
        total = meminfo(mem, "MemTotal:");
        avail = meminfo(mem, "MemAvailable:");
        if (avail < 0) {   // before Linux 3.14 (a FLEX-6000 runs 2.6.37)
            long long f = meminfo(mem, "MemFree:"), b = meminfo(mem, "Buffers:"),
                      c = meminfo(mem, "Cached:");
            if (f >= 0) {
                avail = f + (b > 0 ? b : 0) + (c > 0 ? c : 0);
                estimated = 1;
            }
        }
    }
    int n = snprintf(out, sizeof out,
                     "{\"agent\":\"" AGENT_VERSION "\",\"cpu_idle_pct\":%d,\"cpu_cores\":%d,"
                     "\"mem_total_kb\":%lld,\"mem_available_kb\":%lld,\"mem_estimated\":%s,"
                     "\"storage\":[",
                     g_idle_pct, g_cores, total, avail, estimated ? "true" : "false");
    int first = 1;
    for (int i = 0; i < npaths && n > 0 && (size_t)n < sizeof out; ++i) {
        struct statfs st;
        if (statfs(paths[i], &st) != 0) {
            continue;
        }
        n += snprintf(out + n, sizeof out - (size_t)n,
                      "%s{\"path\":\"%s\",\"free_bytes\":%llu,\"total_bytes\":%llu}",
                      first ? "" : ",", paths[i],
                      (unsigned long long)st.f_bavail * (unsigned long long)st.f_bsize,
                      (unsigned long long)st.f_blocks * (unsigned long long)st.f_bsize);
        first = 0;
    }
    if (n > 0 && (size_t)n < sizeof out - 3) {
        n += snprintf(out + n, sizeof out - (size_t)n, "]}\n");
        ssize_t w = write(fd, out, (size_t)n);
        (void)w;
    }
}

int main(int argc, char **argv)
{
    int port = DEFAULT_PORT, a = 1;
    if (a + 1 < argc && strcmp(argv[a], "-p") == 0) {
        port = atoi(argv[a + 1]);
        a += 2;
    }
    char *paths[MAX_PATHS];
    int npaths = 0;
    for (; a < argc && npaths < MAX_PATHS; ++a) {
        // Paths go into JSON unescaped, so take only plain ones.
        if (strlen(argv[a]) < 200 && !strpbrk(argv[a], "\"\\") ) {
            paths[npaths++] = argv[a];
        }
    }
    // Tests point this at a fixture; on a radio it is unset.
    const char *proc = getenv("AETHER_HEALTH_PROC");
    if (proc && strlen(proc) < 200) {
        snprintf(g_stat, sizeof g_stat, "%s/stat", proc);
        snprintf(g_meminfo, sizeof g_meminfo, "%s/meminfo", proc);
    }
    signal(SIGPIPE, SIG_IGN);

    int ls = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    if (ls < 0 || bind(ls, (struct sockaddr *)&sa, sizeof sa) != 0 || listen(ls, 4) != 0) {
        fprintf(stderr, "aether-health: cannot listen on %d: %s\n", port, strerror(errno));
        return 1;
    }
    fcntl(ls, F_SETFD, FD_CLOEXEC);

    sample_cpu();
    struct timespec last;
    clock_gettime(CLOCK_MONOTONIC, &last);
    for (;;) {
        struct pollfd p = {ls, POLLIN, 0};
        int r = poll(&p, 1, SAMPLE_MS);
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if ((now.tv_sec - last.tv_sec) * 1000 + (now.tv_nsec - last.tv_nsec) / 1000000 >= SAMPLE_MS) {
            sample_cpu();
            last = now;
        }
        if (r <= 0) {
            continue;
        }
        struct sockaddr_in peer;
        socklen_t len = sizeof peer;
        int fd = accept(ls, (struct sockaddr *)&peer, &len);
        if (fd < 0) {
            continue;
        }
        if (peer.sin_family == AF_INET && lan_caller(&peer)) {
            answer(fd, paths, npaths);
        }
        close(fd);
    }
}
