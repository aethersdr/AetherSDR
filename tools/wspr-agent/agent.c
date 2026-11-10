/*
 * AetherWSPR: a WSPR receiver that runs on a FlexRadio as a legacy waveform.
 * Part of AetherSDR, GPL-3.0-or-later.
 *
 * It connects to the radio's API on 127.0.0.1 as a non-GUI client, makes
 * one DIGU slice of its own and takes that slice's DAX audio, so it needs no
 * GUI client slot. Each even UTC minute it records 114 s, shifts the WSPR
 * window down to baseband at 375 samples/s and runs WSJT-X's wsprd decoder
 * (wsprd/, built in) on it in a child process. Spots are kept in memory,
 * uploaded to wsprnet.org when that is on, and served as JSON on TCP 48995:
 *
 *   GET  /v1/status[?since=<unix>]  state, recent cycles and spots
 *   GET  /v1/config                 the settings
 *   POST /v1/config                 new settings (JSON). The first POST is
 *                                   open and returns an admin token; later
 *                                   ones need "Authorization: Bearer <token>"
 *
 * Only LAN callers are answered. It never transmits: it sends no TX, PTT or
 * keying command, and its slice is set tx=0.
 *
 *   aether-wspr                   run
 *   aether-wspr decode ARGS...    wsprd itself (the agent runs this in a child)
 *   aether-wspr selftest [SNR]    synthesize a signal, run it through the
 *                                 capture DSP and the decoder, check the decode
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/timex.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "grayline.h"
#include "wsprd/wsprsim_utils.h"

#define AGENT_VERSION "0.1.7"
#define STATUS_PORT 48995
#define AUDIO_RATE 24000
#define BB_RATE 375
#define DECIM1 16                       /* 24000 -> 1500 */
#define DECIM2 4                        /* 1500 -> 375 */
#define CAPTURE_SECONDS 114
#define CAPTURE_SAMPLES (CAPTURE_SECONDS * BB_RATE) /* 42750 */
#define C2_SAMPLES 45000
#define NTAPS 127
#define MAX_SPOTS 2000
#define MAX_SLICES 8
#define MAX_CYCLE_SPOTS 200   /* MAX_UNIQUES in wsprd.c: all it can print */
#define MAX_CYCLES 60
#define MAX_BANDS 16
#define MAX_UPLOADS 2000

int wsprd_main(int argc, char *argv[]);

/* ------------------------------------------------------------------ util */

static volatile pid_t g_decoder;

static void on_term(int sig)
{
    (void)sig;
    if (g_decoder > 0) kill(g_decoder, SIGKILL);
    _exit(0);
}

static void logf_(const char *fmt, ...)
{
    char ts[32];
    time_t now = time(NULL);
    strftime(ts, sizeof ts, "%H:%M:%S", gmtime(&now));
    fprintf(stderr, "aether-wspr %s ", ts);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

/* For anything measured rather than a time of day. A radio boots with its
 * clock hours off and NTP then steps it, which would otherwise move every
 * timer armed before the step by that much. */
static double mono_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

/* The kernel's view of the clock: synchronized (by the radio's GPS-fed NTP,
 * or NTP over the internet) or not. WSPR needs the cycle start within ~1 s. */
/* The radio told us an NTP source is selected (SmartSDR 4.3.16 publishes
 * `ntp_sources ... state=selected`). Set from the API thread, read here. */
static volatile int g_ntp_selected = 0;

/* Reads the kernel's flag. Separated from the decision below so the decision
 * can be tested on a host whose own clock is synced, where the kernel branch
 * would otherwise always win and hide the fallback. */
static int kernel_clock_synced(void)
{
    struct timex tx;
    memset(&tx, 0, sizeof tx);
    int state = adjtimex(&tx);
    return state != TIME_ERROR && !(tx.status & STA_UNSYNC);
}

/* Indirection so the selftest can hold the kernel at UNSYNC and exercise the
 * REAL clock_synced(). Without it, a build host whose own clock is synced
 * makes the radio operand dead in any test of clock_synced(): the mutation
 * `clock_synced_from(kernel_clock_synced(), 0)` passed for exactly that
 * reason. Production never reassigns this. */
static int (*g_kernel_clock_synced)(void) = kernel_clock_synced;

/* The decision, given the kernel's verdict and the radio's. Pure, so the
 * selftest can put the kernel in either state. */
static int clock_synced_from(int kernel_says, int radio_says)
{
    return kernel_says || radio_says;
}

/* A FLEX-6000 runs kernel 2.6.37, where chronyd disciplines the clock but
 * never clears STA_UNSYNC. Measured on a FLEX-6300 whose selected pool source
 * reported stratum=1 and reach=255 while the kernel still said UNSYNC, and
 * whose wall clock matched UTC to well under a second.
 *
 * Without the radio's verdict the agent never leaves waiting_clock on a 6000
 * -- the hardware this fix is for -- so it captures nothing at all. (The agent
 * also ships as the FLEX-8000/Aurora container, whose kernel clears the flag
 * normally and which returns on the kernel operand.) The
 * radio's own report that it has settled on a source is the better evidence on
 * that kernel, so accept it.
 *
 * Both reads go through clock_synced_from() in ONE call rather than an early
 * return on the kernel branch. A short-circuit here would leave the decision
 * outside the function the truth table tests: a reviewer's `return 0;`
 * mutation on the old tail compiled and PASSED the selftest, because the
 * tested function was no longer the one deciding. Keep this a single call. */
static int clock_synced(void)
{
    return clock_synced_from(g_kernel_clock_synced(), g_ntp_selected);
}

static int lan_caller(const struct sockaddr_in *a)
{
    unsigned long ip = ntohl(a->sin_addr.s_addr);
    return (ip >> 24) == 10 || (ip >> 24) == 127
        || (ip >> 20) == (172UL << 4 | 1)          /* 172.16.0.0/12 */
        || (ip >> 16) == (192UL << 8 | 168)        /* 192.168.0.0/16 */
        || (ip >> 16) == (169UL << 8 | 254);       /* 169.254.0.0/16 */
}

/* Appends to a fixed buffer; on overflow it marks the buffer full and later
 * appends do nothing, so a caller checks once at the end. */
typedef struct { char *p; size_t n, cap; int full; } buf_t;

static void bputs(buf_t *b, const char *s)
{
    size_t l = strlen(s);
    if (b->full || b->n + l + 1 > b->cap) { b->full = 1; return; }
    memcpy(b->p + b->n, s, l + 1);
    b->n += l;
}

static void bprintf(buf_t *b, const char *fmt, ...)
{
    if (b->full) return;
    va_list ap;
    va_start(ap, fmt);
    int l = vsnprintf(b->p + b->n, b->cap - b->n, fmt, ap);
    va_end(ap);
    if (l < 0 || (size_t)l >= b->cap - b->n) { b->full = 1; return; }
    b->n += (size_t)l;
}

/* JSON string: only printable ASCII is kept; quote and backslash escaped. */
static void bjson_str(buf_t *b, const char *s)
{
    bputs(b, "\"");
    char one[3] = {0};
    for (; s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c < 32 || c > 126) continue;
        if (c == '"' || c == '\\') { one[0] = '\\'; one[1] = (char)c; one[2] = 0; }
        else { one[0] = (char)c; one[1] = 0; }
        bputs(b, one);
    }
    bputs(b, "\"");
}

/* ----------------------------------------------------------------- bands */

typedef struct { const char *name; long long dial_hz; } band_t;

static const band_t BANDS[] = {
    {"2200m", 136000},    {"630m", 474200},     {"160m", 1836600},
    {"80m", 3568600},     {"60m", 5287200},     {"40m", 7038600},
    {"30m", 10138700},    {"20m", 14095600},    {"17m", 18104600},
    {"15m", 21094600},    {"12m", 24924600},    {"10m", 28124600},
    {"6m", 50293000},     {"2m", 144489000},
};

static int band_index(const char *name)
{
    for (size_t i = 0; i < sizeof BANDS / sizeof BANDS[0]; i++)
        if (!strcmp(BANDS[i].name, name)) return (int)i;
    return -1;
}

static long long band_dial(const char *name)
{
    for (size_t i = 0; i < sizeof BANDS / sizeof BANDS[0]; i++)
        if (!strcmp(BANDS[i].name, name)) return BANDS[i].dial_hz;
    return 0;
}

/* ---------------------------------------------------------------- config */

/* Band hopping follows WSJT-X: for each time-of-day period (sunrise gray
 * line, day, sunset gray line, night) a set of bands, as bit masks over
 * BANDS[]. */
static const char *const PERIOD_KEY[4] = {"hop_sunrise", "hop_day", "hop_sunset", "hop_night"};
static const char *const PERIOD_NAME[4] = {"sunrise", "day", "sunset", "night"};

/* Decode depth: WSJT-X's three, and "standard" (wsprd's own defaults: the
 * Fano decoder uncapped, no OSD), which decoded best on simulated weak
 * signals and is the default. */
static const char *const DEPTH_NAME[4] = {"standard", "fast", "normal", "deep"};

typedef struct {
    char callsign[16];       /* blank: the radio's callsign */
    char grid[8];            /* blank: the radio's GPS grid */
    char band[8];            /* the band when hopping is off */
    int hopping;
    unsigned hop[4];         /* bands per period, bits over BANDS[] */
    int gray_minutes;        /* gray line width, 1-120 (WSJT-X's range) */
    int depth;               /* index into DEPTH_NAME */
    char rxant[12];          /* blank: the radio's default */
    int upload;              /* send spots to wsprnet.org */
    int dax_channel;
} config_t;

static void config_default(config_t *c)
{
    memset(c, 0, sizeof *c);
    strcpy(c->band, "20m");
    c->gray_minutes = 60;
    c->dax_channel = 4;
}

/* Minimal JSON reader for one flat object of strings, integers, booleans and
 * arrays of strings. Anything else is refused. */
typedef struct { const char *p, *end; } jr_t;

static void jws(jr_t *j) { while (j->p < j->end && (*j->p == ' ' || *j->p == '\t' || *j->p == '\r' || *j->p == '\n')) j->p++; }

static int jstr(jr_t *j, char *out, size_t cap)
{
    jws(j);
    if (j->p >= j->end || *j->p != '"') return 0;
    j->p++;
    size_t n = 0;
    while (j->p < j->end && *j->p != '"') {
        char c = *j->p++;
        if (c == '\\') {
            if (j->p >= j->end) return 0;
            c = *j->p++;
            if (c != '"' && c != '\\' && c != '/') return 0;
        }
        if ((unsigned char)c < 32) return 0;
        if (n + 1 >= cap) return 0;
        out[n++] = c;
    }
    if (j->p >= j->end) return 0;
    j->p++;
    out[n] = 0;
    return 1;
}

static int jlit(jr_t *j, const char *lit)
{
    jws(j);
    size_t l = strlen(lit);
    if ((size_t)(j->end - j->p) < l || strncmp(j->p, lit, l)) return 0;
    j->p += l;
    return 1;
}

static int jint(jr_t *j, long *v)
{
    jws(j);
    char tmp[24];
    size_t n = 0;
    while (j->p < j->end && n + 1 < sizeof tmp && ((*j->p >= '0' && *j->p <= '9') || (n == 0 && *j->p == '-')))
        tmp[n++] = *j->p++;
    tmp[n] = 0;
    if (!n || (n == 1 && tmp[0] == '-')) return 0;
    *v = strtol(tmp, NULL, 10);
    return 1;
}

static int valid_token(const char *s, const char *extra)
{
    if (!*s) return 1;
    for (; *s; s++)
        if (!((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || strchr(extra, *s)))
            return 0;
    return 1;
}

/* Maidenhead: field letters A-R, digits, then subsquare letters A-X. */
static int valid_grid(const char *s)
{
    size_t n = strlen(s);
    if (n == 0) return 1;
    if (n != 4 && n != 6) return 0;
    if (toupper((unsigned char)s[0]) < 'A' || toupper((unsigned char)s[0]) > 'R'
        || toupper((unsigned char)s[1]) < 'A' || toupper((unsigned char)s[1]) > 'R'
        || !isdigit((unsigned char)s[2]) || !isdigit((unsigned char)s[3])) return 0;
    return n == 4 || (toupper((unsigned char)s[4]) >= 'A' && toupper((unsigned char)s[4]) <= 'X'
                      && toupper((unsigned char)s[5]) >= 'A' && toupper((unsigned char)s[5]) <= 'X');
}

/* Parses a config object into *c (starting from the current settings, so
 * fields left out keep their values). Returns NULL or an error message. */
static const char *config_parse(const char *text, size_t len, config_t *c)
{
    jr_t j = {text, text + len};
    if (!jlit(&j, "{")) return "expected an object";
    jws(&j);
    if (jlit(&j, "}")) return NULL;
    for (;;) {
        char key[24], s[32];
        long v;
        if (!jstr(&j, key, sizeof key) || !jlit(&j, ":")) return "bad key";
        if (!strcmp(key, "callsign")) {
            if (!jstr(&j, s, sizeof s) || strlen(s) >= sizeof c->callsign || !valid_token(s, "/")) return "bad callsign";
            strcpy(c->callsign, s);
            for (char *q = c->callsign; *q; q++) if (*q >= 'a' && *q <= 'z') *q -= 32;
        } else if (!strcmp(key, "grid")) {
            if (!jstr(&j, s, sizeof s) || !valid_grid(s)) return "bad grid";
            strcpy(c->grid, s);
        } else if (!strcmp(key, "rxant")) {
            if (!jstr(&j, s, sizeof s) || strlen(s) >= sizeof c->rxant || !valid_token(s, "_")) return "bad rxant";
            strcpy(c->rxant, s);
        } else if (!strcmp(key, "band")) {
            if (!jstr(&j, s, sizeof s) || band_index(s) < 0) return "unknown band";
            strcpy(c->band, s);
        } else if (!strcmp(key, "hopping")) {
            if (jlit(&j, "true")) c->hopping = 1;
            else if (jlit(&j, "false")) c->hopping = 0;
            else return "hopping must be true or false";
        } else if (!strcmp(key, "gray_minutes")) {
            if (!jint(&j, &v) || v < 1 || v > 120) return "gray_minutes must be 1-120";
            c->gray_minutes = (int)v;
        } else if (!strcmp(key, "depth")) {
            int d = -1;
            if (jstr(&j, s, sizeof s))
                for (int i = 0; i < 4; i++) if (!strcmp(s, DEPTH_NAME[i])) d = i;
            if (d < 0) return "depth must be standard, fast, normal or deep";
            c->depth = d;
        } else if (!strcmp(key, "dax_channel")) {
            if (!jint(&j, &v) || v < 1 || v > 8) return "dax_channel must be 1-8";
            c->dax_channel = (int)v;
        } else if (!strcmp(key, "upload")) {
            if (jlit(&j, "true")) c->upload = 1;
            else if (jlit(&j, "false")) c->upload = 0;
            else return "upload must be true or false";
        } else if (!strncmp(key, "hop_", 4)) {
            int period = -1;
            for (int i = 0; i < 4; i++) if (!strcmp(key, PERIOD_KEY[i])) period = i;
            if (period < 0) return "unknown key";
            if (!jlit(&j, "[")) return "a hop_ period must be an array of bands";
            unsigned mask = 0;
            jws(&j);
            if (!jlit(&j, "]")) {
                for (;;) {
                    int bi;
                    if (!jstr(&j, s, sizeof s) || (bi = band_index(s)) < 0) return "unknown band";
                    mask |= 1u << bi;
                    if (jlit(&j, "]")) break;
                    if (!jlit(&j, ",")) return "bad band list";
                }
            }
            c->hop[period] = mask;
        } else {
            return "unknown key";
        }
        if (jlit(&j, "}")) break;
        if (!jlit(&j, ",")) return "expected , or }";
    }
    jws(&j);
    return j.p == j.end ? NULL : "trailing data";
}

static void config_json(buf_t *b, const config_t *c)
{
    bputs(b, "{\"callsign\":"); bjson_str(b, c->callsign);
    bputs(b, ",\"grid\":"); bjson_str(b, c->grid);
    bputs(b, ",\"band\":"); bjson_str(b, c->band);
    bprintf(b, ",\"hopping\":%s,\"gray_minutes\":%d", c->hopping ? "true" : "false", c->gray_minutes);
    for (int p = 0; p < 4; p++) {
        bprintf(b, ",\"%s\":[", PERIOD_KEY[p]);
        int first = 1;
        for (size_t i = 0; i < sizeof BANDS / sizeof BANDS[0]; i++)
            if (c->hop[p] & (1u << i)) {
                if (!first) bputs(b, ",");
                first = 0;
                bjson_str(b, BANDS[i].name);
            }
        bputs(b, "]");
    }
    bputs(b, ",\"depth\":"); bjson_str(b, DEPTH_NAME[c->depth]);
    bputs(b, ",\"rxant\":");
    bjson_str(b, c->rxant);
    bprintf(b, ",\"upload\":%s,\"dax_channel\":%d}", c->upload ? "true" : "false", c->dax_channel);
}

/* ----------------------------------------------------------------- state */

typedef struct {
    long long t;           /* cycle start, unix seconds */
    int snr, drift, dbm;
    float dt;
    long long hz;
    char call[16], grid[8], band[8];
} spot_t;

typedef struct {
    long long t, dial_hz;
    char band[8];
    int spots, decode_ms, audio_pct, lost_packets;
    float audio_dbfs;
} cycle_t;

static struct {
    pthread_mutex_t mu;
    config_t cfg;
    int cfg_dirty;              /* new settings to apply */
    char token[40];
    char state_path[512];       /* config.json, token */
    char radio_call[16], gps_grid[8];
    const char *state;          /* connecting, waiting_clock, capturing, ... */
    char band[8];
    long long dial_hz;
    int hop_period;             /* period of the tuned cycle, -1 when not hopping */
    int slice_ok, stream_ok;
    spot_t spots[MAX_SPOTS];
    int nspots, spot_head;
    cycle_t cycles[MAX_CYCLES];
    int ncycles, cycle_head;
    long long uploaded, upload_failed;
    int upload_queued;
    char upload_error[80];
    long long started;
} G = { .mu = PTHREAD_MUTEX_INITIALIZER, .state = "starting", .hop_period = -1 };

static void effective_call_grid(char *call, char *grid)
{
    strcpy(call, G.cfg.callsign[0] ? G.cfg.callsign : G.radio_call);
    const char *g = G.cfg.grid[0] ? G.cfg.grid : G.gps_grid;
    snprintf(grid, 8, "%s", g);
}

/* 0 when the settings couldn't be stored. */
static int save_config_locked(void)
{
    if (!G.state_path[0]) return 1;
    char path[540], tmp[550], text[1024];
    buf_t b = {text, 0, sizeof text, 0};
    bputs(&b, "{\"token\":"); bjson_str(&b, G.token);
    bputs(&b, ",\"config\":"); config_json(&b, &G.cfg); bputs(&b, "}\n");
    if (b.full) return 0;
    snprintf(path, sizeof path, "%s/config.json", G.state_path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return 0;
    /* Synced before the rename: a radio can lose power at any moment, and the
     * old settings must survive unless the new ones are whole. */
    int ok = write(fd, text, b.n) == (ssize_t)b.n && fsync(fd) == 0;
    close(fd);
    if (!ok || rename(tmp, path)) { unlink(tmp); return 0; }
    int dir = open(G.state_path, O_RDONLY | O_DIRECTORY);
    if (dir >= 0) { fsync(dir); close(dir); }
    return 1;
}

static void load_config(void)
{
    char path[540], text[2048];
    snprintf(path, sizeof path, "%s/config.json", G.state_path);
    FILE *f = fopen(path, "r");
    if (!f) return;
    size_t n = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    text[n] = 0;
    char *tok = strstr(text, "\"token\":\"");
    if (tok) {
        tok += 9;
        size_t i = 0;
        while (tok[i] && tok[i] != '"' && i + 1 < sizeof G.token) { G.token[i] = tok[i]; i++; }
        G.token[i] = 0;
    }
    char *cfg = strstr(text, "\"config\":");
    char *end = strrchr(text, '}');
    if (cfg && end && end > cfg) {
        cfg += 9;
        config_t c = G.cfg;
        if (!config_parse(cfg, (size_t)(end - cfg), &c)) G.cfg = c;
    }
}

/* Settings and the admin token live beside the program, which survives a
 * reboot: the container's writable layer on a FLEX-8000, /user on a
 * FLEX-6000. Removing the receiver removes them. A full partition falls back
 * to RAM, which outlives a reinstall: naming that directory after this
 * install's binary means a reinstall really starts over, so a computer that
 * lost the token can claim the receiver again, while a restart keeps it. */
static void pick_state_dir(const char *self_dir)
{
    struct stat self;
    long long install = stat("/proc/self/exe", &self) == 0 ? (long long)self.st_ctime : 0;
    char dirs[2][500];
    snprintf(dirs[0], sizeof dirs[0], "%s/aether-wspr-state", self_dir);
    snprintf(dirs[1], sizeof dirs[1], "/tmp/aether-wspr-state-%lld", install);
    for (int i = 0; i < 2; i++) {
        mkdir(dirs[i], 0700);
        char probe[520];
        if (snprintf(probe, sizeof probe, "%s/.w", dirs[i]) >= (int)sizeof probe
            || snprintf(G.state_path, sizeof G.state_path, "%s", dirs[i]) >= (int)sizeof G.state_path) {
            G.state_path[0] = 0;
            continue;
        }
        int fd = open(probe, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd >= 0) {
            close(fd);
            unlink(probe);
            return;
        }
        G.state_path[0] = 0;
    }
}

/* --------------------------------------------------------------- capture */

static float g_h1[NTAPS], g_h2[NTAPS];

static void design_lowpass(float *h, double cutoff_over_fs)
{
    double sum = 0;
    for (int i = 0; i < NTAPS; i++) {
        double m = i - (NTAPS - 1) / 2.0;
        double x = 2 * M_PI * cutoff_over_fs * m;
        double sinc = m == 0 ? 2 * cutoff_over_fs : sin(x) / (M_PI * m);
        double w = 0.42 - 0.5 * cos(2 * M_PI * i / (NTAPS - 1)) + 0.08 * cos(4 * M_PI * i / (NTAPS - 1));
        h[i] = (float)(sinc * w);
        sum += h[i];
    }
    for (int i = 0; i < NTAPS; i++) h[i] = (float)(h[i] / sum);
}

/* Audio (real, 24 kHz) -> mix 1500 Hz to 0 -> two FIR decimators -> complex
 * 375 Hz, in the scale and sign wsprd's own .wav reader produces. */
typedef struct {
    double ph, dph;
    float r1[2][NTAPS], r2[2][NTAPS];
    int i1, i2, k1, k2;
    float *idat, *qdat;
    int n;           /* baseband samples captured */
    long long in;    /* audio samples taken */
    double sumsq;    /* audio power, for the cycle's level */
    int lost;        /* packets missing from the stream, filled with silence */
    int last_count;  /* VITA packet count of the previous packet, -1 none */
    double last_at;  /* when that packet arrived */
} dsp_t;

static void dsp_reset(dsp_t *d)
{
    float *i = d->idat, *q = d->qdat;
    memset(d, 0, sizeof *d);
    d->idat = i;
    d->qdat = q;
    d->dph = 2 * M_PI * 1500.0 / AUDIO_RATE;
    d->last_count = -1;
}

static float fir(const float *ring, int head, const float *h)
{
    float acc = 0;
    int j = head;
    for (int i = 0; i < NTAPS; i++) {
        acc += h[i] * ring[j];
        if (++j == NTAPS) j = 0;
    }
    return acc;
}

/* 2 x 737.28: matches wsprd's readwavfile scaling for 16-bit audio. */
#define BB_SCALE 1474.56f

static void dsp_push(dsp_t *d, float x)
{
    if (d->n >= CAPTURE_SAMPLES) return;
    d->in++;
    d->sumsq += (double)x * x;
    float re = (float)(x * cos(d->ph)), im = (float)(-x * sin(d->ph));
    d->ph += d->dph;
    if (d->ph > 2 * M_PI) d->ph -= 2 * M_PI;
    d->r1[0][d->i1] = re;
    d->r1[1][d->i1] = im;
    if (++d->i1 == NTAPS) d->i1 = 0;
    if (++d->k1 < DECIM1) return;
    d->k1 = 0;
    d->r2[0][d->i2] = fir(d->r1[0], d->i1, g_h1);
    d->r2[1][d->i2] = fir(d->r1[1], d->i1, g_h1);
    if (++d->i2 == NTAPS) d->i2 = 0;
    if (++d->k2 < DECIM2) return;
    d->k2 = 0;
    d->idat[d->n] = BB_SCALE * fir(d->r2[0], d->i2, g_h2);
    d->qdat[d->n] = BB_SCALE * fir(d->r2[1], d->i2, g_h2);
    d->n++;
}

/* The .c2 file wsprd reads: 14-byte name, int minutes, double dial MHz,
 * 45000 complex floats stored as (I, -Q). */
static int write_c2(const char *path, const char *name14, double dial_mhz, const float *idat, const float *qdat, int n)
{
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    char name[14];
    memset(name, 0, sizeof name);
    memcpy(name, name14, strlen(name14) < 14 ? strlen(name14) : 14);
    int trmin = 2;
    int ok = fwrite(name, 1, 14, f) == 14 && fwrite(&trmin, sizeof trmin, 1, f) == 1
        && fwrite(&dial_mhz, sizeof dial_mhz, 1, f) == 1;
    static float buf[2 * C2_SAMPLES];
    for (int i = 0; i < C2_SAMPLES; i++) {
        buf[2 * i] = i < n ? idat[i] : 0.0f;
        buf[2 * i + 1] = i < n ? -qdat[i] : 0.0f;
    }
    ok = ok && fwrite(buf, sizeof(float), 2 * C2_SAMPLES, f) == 2 * C2_SAMPLES;
    return fclose(f) == 0 && ok;
}

/* ---------------------------------------------------------------- decode */

typedef struct {
    pid_t pid;
    int fd;
    char out[16384];
    size_t n;
    long long cycle_t, dial_hz;
    char band[8];
    double started;
    int audio_pct, lost;
    float dbfs;
    /* Load while it ran: the radio's CPU (all cores) and the child's own. */
    unsigned long long cpu0_busy, cpu0_total;
    float sys_busy_pct, child_cpu_s;
    long child_rss_kb, mem_avail_kb;
} decode_t;

/* /proc/stat's first line: busy and total jiffies over all cores. */
static void cpu_jiffies(unsigned long long *busy, unsigned long long *total)
{
    *busy = *total = 0;
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return;
    unsigned long long v[8] = {0};
    if (fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]) >= 4) {
        for (int i = 0; i < 8; i++) *total += v[i];
        *busy = *total - v[3] - v[4];
    }
    fclose(f);
}

static long mem_available_kb(void)
{
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return -1;
    char line[128];
    long kb = -1;
    while (fgets(line, sizeof line, f))
        if (sscanf(line, "MemAvailable: %ld kB", &kb) == 1) break;
    fclose(f);
    return kb;
}

static char g_self[512];
static char g_work[300];

static int start_decode(decode_t *dc, const char *c2, int depth)
{
    int p[2];
    if (pipe(p)) return 0;
    pid_t pid = fork();
    if (pid < 0) { close(p[0]); close(p[1]); return 0; }
    if (pid == 0) {
        close(p[0]);
        dup2(p[1], 1);
        /* Sockets stay with the agent: a decoder outliving it mustn't hold
         * port 48995 against the next start. */
        for (int fd = 3; fd < 256; fd++) close(fd);
        setpriority(PRIO_PROCESS, 0, 19);
        if (chdir(g_work)) _exit(2);
        /* WSJT-X's arguments for its Fast, Normal and Deep. */
        char *argv[12];
        int argc = 0;
        argv[argc++] = "wsprd";
        if (depth == 1) argv[argc++] = "-q", argv[argc++] = "-B";
        if (depth >= 2) argv[argc++] = "-C", argv[argc++] = "500", argv[argc++] = "-o", argv[argc++] = "4";
        if (depth == 3) argv[argc++] = "-d";
        argv[argc++] = "-a";
        argv[argc++] = g_work;
        argv[argc++] = (char *)c2;
        argv[argc] = NULL;
        int rc = wsprd_main(argc, argv);
        fflush(NULL);   /* stdout is a pipe, so fully buffered */
        _exit(rc == 0 ? 0 : 1);
    }
    close(p[1]);
    fcntl(p[0], F_SETFL, O_NONBLOCK);
    dc->pid = pid;
    g_decoder = pid;
    dc->fd = p[0];
    dc->n = 0;
    dc->started = mono_s();
    cpu_jiffies(&dc->cpu0_busy, &dc->cpu0_total);
    return 1;
}

static const char *band_of(long long hz)
{
    for (size_t i = 0; i < sizeof BANDS / sizeof BANDS[0]; i++)
        if (llabs(hz - BANDS[i].dial_hz) < 10000) return BANDS[i].name;
    return "";
}

/* "HHMM SNR DT FREQ DRIFT CALL GRID DBM" (or "CALL DBM" / "<CALL> GRID6 DBM"). */
static int parse_spot(char *line, long long t, spot_t *s)
{
    char *f[10];
    int n = 0;
    for (char *tok = strtok(line, " \t"); tok && n < 10; tok = strtok(NULL, " \t")) f[n++] = tok;
    if (n < 7 || strlen(f[0]) != 4) return 0;
    memset(s, 0, sizeof *s);
    s->t = t;
    s->snr = atoi(f[1]);
    s->dt = (float)atof(f[2]);
    s->hz = llround(atof(f[3]) * 1e6);
    s->drift = atoi(f[4]);
    const char *call = f[5], *grid = n >= 8 ? f[6] : "", *dbm = f[n - 1];
    /* <K1ABC> is a hash wsprd resolved; <...> one it couldn't. */
    if (!strcmp(call, "<...>")) call = "";
    else if (call[0] == '<') call++;
    snprintf(s->call, sizeof s->call, "%s", call);
    char *gt = strchr(s->call, '>');
    if (gt) *gt = 0;
    snprintf(s->grid, sizeof s->grid, "%s", grid);
    s->dbm = atoi(dbm);
    snprintf(s->band, sizeof s->band, "%s", band_of(s->hz));
    return 1;
}

/* -------------------------------------------------------------- uploader */

typedef struct { char form[512]; long long t; int spot; } upreq_t;

static struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    upreq_t q[MAX_UPLOADS];
    int head, n;
} U = { .mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER };

static void urlenc(buf_t *b, const char *s)
{
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || strchr("-_.~", c))
            bprintf(b, "%c", c);
        else
            bprintf(b, "%%%02X", c);
    }
}

static void form_add(buf_t *b, const char *k, const char *v)
{
    if (b->n) bputs(b, "&");
    bputs(b, k);
    bputs(b, "=");
    urlenc(b, v);
}

static void queue_upload(const char *form, long long t, int spot)
{
    pthread_mutex_lock(&U.mu);
    if (U.n == MAX_UPLOADS) { U.head = (U.head + 1) % MAX_UPLOADS; U.n--; }
    upreq_t *r = &U.q[(U.head + U.n) % MAX_UPLOADS];
    snprintf(r->form, sizeof r->form, "%s", form);
    r->t = t;
    r->spot = spot;
    U.n++;
    pthread_cond_signal(&U.cv);
    pthread_mutex_unlock(&U.mu);
}

/* One cycle's reports, the way WSJT-X sends them: a function=wspr POST per
 * spot, or a function=wsprstat when nothing was heard. */
static void queue_cycle_uploads(long long t, long long dial_hz, const spot_t *s, int n)
{
    char call[16], grid[8];
    pthread_mutex_lock(&G.mu);
    int on = G.cfg.upload;
    effective_call_grid(call, grid);
    pthread_mutex_unlock(&G.mu);
    if (!on || !call[0] || strlen(grid) < 4) return;
    char date[8], hhmm[8], rqrg[24], v[32];
    time_t tt = (time_t)t;
    struct tm tm;
    gmtime_r(&tt, &tm);
    strftime(date, sizeof date, "%y%m%d", &tm);
    strftime(hhmm, sizeof hhmm, "%H%M", &tm);
    snprintf(rqrg, sizeof rqrg, "%.6f", dial_hz / 1e6);
    int sent = 0;
    for (int i = 0; i < n; i++) {
        if (!s[i].call[0] || llabs(s[i].hz - dial_hz) > 10000) continue;
        char form[512];
        buf_t b = {form, 0, sizeof form, 0};
        form[0] = 0;
        form_add(&b, "function", "wspr");
        form_add(&b, "date", date);
        form_add(&b, "time", hhmm);
        snprintf(v, sizeof v, "%d", s[i].snr); form_add(&b, "sig", v);
        snprintf(v, sizeof v, "%.1f", s[i].dt); form_add(&b, "dt", v);
        snprintf(v, sizeof v, "%d", s[i].drift); form_add(&b, "drift", v);
        snprintf(v, sizeof v, "%.6f", s[i].hz / 1e6); form_add(&b, "tqrg", v);
        form_add(&b, "tcall", s[i].call);
        form_add(&b, "tgrid", s[i].grid);
        snprintf(v, sizeof v, "%d", s[i].dbm); form_add(&b, "dbm", v);
        form_add(&b, "version", "AetherWSPR " AGENT_VERSION);
        form_add(&b, "rcall", call);
        form_add(&b, "rgrid", grid);
        form_add(&b, "rqrg", rqrg);
        form_add(&b, "mode", "2");
        if (!b.full) { queue_upload(form, t, 1); sent++; }
    }
    if (!sent) {
        char form[512];
        buf_t b = {form, 0, sizeof form, 0};
        form[0] = 0;
        form_add(&b, "function", "wsprstat");
        form_add(&b, "rcall", call);
        form_add(&b, "rgrid", grid);
        form_add(&b, "rqrg", rqrg);
        form_add(&b, "tpct", "0");
        snprintf(v, sizeof v, "%.6f", (dial_hz + 1500) / 1e6); form_add(&b, "tqrg", v);
        form_add(&b, "dbm", "0");
        form_add(&b, "version", "AetherWSPR " AGENT_VERSION);
        form_add(&b, "mode", "2");
        if (!b.full) queue_upload(form, t, 0);
    }
}

/* 0 accepted, 1 retry later, -1 rejected. */
static int post_wsprnet(const upreq_t *r, char *err, size_t errcap)
{
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM}, *ai = NULL;
    if (getaddrinfo("wsprnet.org", "80", &hints, &ai) || !ai) { snprintf(err, errcap, "DNS lookup failed"); return 1; }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct timeval tv = {15, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    if (fd < 0 || connect(fd, ai->ai_addr, ai->ai_addrlen)) {
        freeaddrinfo(ai);
        if (fd >= 0) close(fd);
        snprintf(err, errcap, "connect failed");
        return 1;
    }
    freeaddrinfo(ai);
    char req[1024];
    int l = snprintf(req, sizeof req,
                     "POST /post/ HTTP/1.0\r\nHost: wsprnet.org\r\nUser-Agent: AetherWSPR/" AGENT_VERSION
                     "\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: %zu\r\n\r\n%s",
                     strlen(r->form), r->form);
    if (l <= 0 || (size_t)l >= sizeof req || write(fd, req, (size_t)l) != l) { close(fd); snprintf(err, errcap, "send failed"); return 1; }
    char resp[4096];
    size_t n = 0;
    ssize_t k;
    while (n + 1 < sizeof resp && (k = read(fd, resp + n, sizeof resp - 1 - n)) > 0) n += (size_t)k;
    resp[n] = 0;
    close(fd);
    int code = 0;
    if (sscanf(resp, "HTTP/%*s %d", &code) != 1) { snprintf(err, errcap, "no HTTP reply"); return 1; }
    if (code >= 500 || code == 429) { snprintf(err, errcap, "HTTP %d", code); return 1; }
    if (code != 200) { snprintf(err, errcap, "HTTP %d", code); return -1; }
    if (r->spot && !strstr(resp, "spot(s) added")) { snprintf(err, errcap, "unexpected reply"); return -1; }
    return 0;
}

/* Turning upload off is a promise that nothing more leaves the radio, so
 * what is waiting for wsprnet goes too. */
static int upload_drop_if_off(void)
{
    pthread_mutex_lock(&G.mu);
    int on = G.cfg.upload;
    pthread_mutex_unlock(&G.mu);
    if (on) return 0;
    pthread_mutex_lock(&U.mu);
    U.head = U.n = 0;
    pthread_mutex_unlock(&U.mu);
    return 1;
}

static void *upload_thread(void *arg)
{
    (void)arg;
    int backoff = 0;
    for (;;) {
        pthread_mutex_lock(&U.mu);
        while (!U.n) pthread_cond_wait(&U.cv, &U.mu);
        pthread_mutex_unlock(&U.mu);
        if (upload_drop_if_off()) { backoff = 0; continue; }
        pthread_mutex_lock(&U.mu);
        upreq_t r = U.q[U.head];
        pthread_mutex_unlock(&U.mu);
        if (time(NULL) - r.t > 24 * 3600) {   /* wsprnet drops old spots anyway */
            pthread_mutex_lock(&U.mu); U.head = (U.head + 1) % MAX_UPLOADS; U.n--; pthread_mutex_unlock(&U.mu);
            continue;
        }
        char err[80] = "";
        int rc = post_wsprnet(&r, err, sizeof err);
        pthread_mutex_lock(&G.mu);
        if (rc == 0 && r.spot) G.uploaded++;
        if (rc < 0) G.upload_failed++;
        snprintf(G.upload_error, sizeof G.upload_error, "%s", err);
        pthread_mutex_unlock(&G.mu);
        if (rc <= 0) {
            pthread_mutex_lock(&U.mu); U.head = (U.head + 1) % MAX_UPLOADS; U.n--; pthread_mutex_unlock(&U.mu);
            backoff = 0;
            usleep(300000);    /* stay well under wsprnet's rate limits */
        } else {
            backoff = backoff ? (backoff * 2 > 600 ? 600 : backoff * 2) : 15;
            sleep((unsigned)backoff);
        }
    }
    return NULL;
}

/* --------------------------------------------------------- status server */

static void status_json(buf_t *b, long long since)
{
    char call[16], grid[8];
    effective_call_grid(call, grid);
    pthread_mutex_lock(&U.mu);
    int queued = U.n;
    pthread_mutex_unlock(&U.mu);
    bprintf(b, "{\"agent\":\"AetherWSPR\",\"version\":\"%s\",\"state\":", AGENT_VERSION);
    bjson_str(b, G.state);
    /* clock_source says WHY the clock is trusted: the kernel's own flag, the
     * radio's selected NTP source where the kernel never clears STA_UNSYNC,
     * or neither. An operator looking at a bool cannot tell a GPS-locked
     * radio from a 6000 running on the fallback. */
    {
        /* kernel_clock_synced(), not a second inline adjtimex: two copies of
         * the same test drift apart. A reviewer asked for this.  */
        const char *csrc = kernel_clock_synced() ? "kernel"
                         : g_ntp_selected ? "radio_ntp" : "none";
        bputs(b, ",\"clock_source\":");
        bjson_str(b, csrc);
    }
    bprintf(b, ",\"clock_synced\":%s,\"uptime_s\":%lld,\"callsign\":", clock_synced() ? "true" : "false",
            (long long)(mono_s() - G.started));
    bjson_str(b, call);
    bputs(b, ",\"grid\":"); bjson_str(b, grid);
    bputs(b, ",\"band\":"); bjson_str(b, G.band);
    bprintf(b, ",\"dial_hz\":%lld,\"slice\":%s,\"audio\":%s", G.dial_hz, G.slice_ok ? "true" : "false",
            G.stream_ok ? "true" : "false");
    bputs(b, ",\"hop_period\":");
    bjson_str(b, G.hop_period >= 0 ? PERIOD_NAME[G.hop_period] : "");
    bputs(b, ",\"depth\":");
    bjson_str(b, DEPTH_NAME[G.cfg.depth]);
    bprintf(b, ",\"upload\":{\"enabled\":%s,\"sent\":%lld,\"rejected\":%lld,\"queued\":%d,\"last_error\":",
            G.cfg.upload ? "true" : "false", G.uploaded, G.upload_failed, queued);
    bjson_str(b, G.upload_error);
    bputs(b, "},\"cycles\":[");
    for (int i = 0; i < G.ncycles; i++) {
        const cycle_t *c = &G.cycles[(G.cycle_head + i) % MAX_CYCLES];
        if (c->t < since) continue;
        if (b->n && b->p[b->n - 1] == '}') bputs(b, ",");
        bprintf(b, "{\"t\":%lld,\"band\":", c->t);
        bjson_str(b, c->band);
        bprintf(b, ",\"dial_hz\":%lld,\"spots\":%d,\"decode_ms\":%d,\"audio_pct\":%d,\"lost_packets\":%d,"
                   "\"audio_dbfs\":%.1f}", c->dial_hz, c->spots, c->decode_ms, c->audio_pct, c->lost_packets, c->audio_dbfs);
    }
    bputs(b, "],\"spots\":[");
    int first = 1;
    for (int i = 0; i < G.nspots; i++) {
        const spot_t *s = &G.spots[(G.spot_head + i) % MAX_SPOTS];
        if (s->t < since) continue;
        if (!first) bputs(b, ",");
        first = 0;
        bprintf(b, "{\"t\":%lld,\"band\":", s->t);
        bjson_str(b, s->band);
        bprintf(b, ",\"snr\":%d,\"dt\":%.1f,\"hz\":%lld,\"drift\":%d,\"call\":", s->snr, s->dt, s->hz, s->drift);
        bjson_str(b, s->call);
        bputs(b, ",\"grid\":"); bjson_str(b, s->grid);
        bprintf(b, ",\"dbm\":%d}", s->dbm);
    }
    bputs(b, "]}");
}

static void http_reply(int fd, int code, const char *body, size_t len)
{
    char head[160];
    const char *msg = code == 200 ? "OK" : code == 400 ? "Bad Request" : code == 401 ? "Unauthorized"
                    : code == 404 ? "Not Found" : code == 413 ? "Payload Too Large" : "Error";
    int l = snprintf(head, sizeof head,
                     "HTTP/1.0 %d %s\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                     code, msg, len);
    if (write(fd, head, (size_t)l) == l && len) {
        size_t off = 0;
        while (off < len) {
            ssize_t k = write(fd, body + off, len - off);
            if (k <= 0) break;
            off += (size_t)k;
        }
    }
}

static void http_error(int fd, int code, const char *why)
{
    char body[160];
    buf_t b = {body, 0, sizeof body, 0};
    bputs(&b, "{\"error\":"); bjson_str(&b, why); bputs(&b, "}");
    http_reply(fd, code, body, b.n);
}

static int random_token(char *out, size_t cap)
{
    unsigned char r[16];
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return 0;
    int ok = read(fd, r, sizeof r) == sizeof r;
    close(fd);
    if (!ok) return 0;
    for (size_t i = 0; i < sizeof r && 2 * i + 2 < cap; i++) snprintf(out + 2 * i, 3, "%02x", r[i]);
    return 1;
}

static void serve(int fd)
{
    static char req[8192];
    size_t n = 0;
    struct timeval tv = {3, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    char *body = NULL;
    size_t clen = 0;
    /* One connection at a time, so a slow caller can't hold the server: the
     * socket timeout is per read, this is for the whole request. */
    double deadline = mono_s() + 5;
    for (;;) {
        if (mono_s() > deadline) return;
        ssize_t k = read(fd, req + n, sizeof req - 1 - n);
        if (k <= 0) return;
        n += (size_t)k;
        req[n] = 0;
        char *hend = strstr(req, "\r\n\r\n");
        if (hend) {
            body = hend + 4;
            char *cl = strcasestr(req, "\r\nContent-Length:");
            clen = cl && cl < hend ? strtoul(cl + 17, NULL, 10) : 0;
            if (clen > 2048) { http_error(fd, 413, "body too large"); return; }
            if ((size_t)(req + n - body) >= clen) break;
        }
        if (n + 1 >= sizeof req) { http_error(fd, 413, "request too large"); return; }
    }
    char method[8] = "", path[128] = "";
    sscanf(req, "%7s %127s", method, path);
    char *query = strchr(path, '?');
    if (query) *query++ = 0;

    if (!strcmp(method, "GET") && !strcmp(path, "/v1/status")) {
        long long since = 0;
        if (query && !strncmp(query, "since=", 6)) since = atoll(query + 6);
        static char out[512 * 1024];
        buf_t b = {out, 0, sizeof out, 0};
        pthread_mutex_lock(&G.mu);
        status_json(&b, since);
        pthread_mutex_unlock(&G.mu);
        if (b.full) { http_error(fd, 500, "status too large"); return; }
        http_reply(fd, 200, out, b.n);
    } else if (!strcmp(method, "GET") && !strcmp(path, "/v1/config")) {
        char out[1024];
        buf_t b = {out, 0, sizeof out, 0};
        pthread_mutex_lock(&G.mu);
        bputs(&b, "{\"config\":");
        config_json(&b, &G.cfg);
        bprintf(&b, ",\"claimed\":%s}", G.token[0] ? "true" : "false");
        pthread_mutex_unlock(&G.mu);
        http_reply(fd, 200, out, b.n);
    } else if (!strcmp(method, "POST") && !strcmp(path, "/v1/config")) {
        pthread_mutex_lock(&G.mu);
        if (G.token[0]) {
            char want[64];
            snprintf(want, sizeof want, "Bearer %s", G.token);
            char *auth = strcasestr(req, "\r\nAuthorization:");
            int ok = 0;
            if (auth && auth < body) {
                auth += 16;
                while (*auth == ' ') auth++;
                ok = !strncmp(auth, want, strlen(want)) && (auth[strlen(want)] == '\r');
            }
            if (!ok) { pthread_mutex_unlock(&G.mu); http_error(fd, 401, "admin token required"); return; }
        }
        config_t c = G.cfg;
        const char *err = config_parse(body, clen, &c);
        if (err) { pthread_mutex_unlock(&G.mu); http_error(fd, 400, err); return; }
        int fresh = !G.token[0];
        if (fresh && !random_token(G.token, sizeof G.token)) {
            G.token[0] = 0;
            pthread_mutex_unlock(&G.mu);
            http_error(fd, 500, "no random bytes for a token");
            return;
        }
        config_t old = G.cfg;
        G.cfg = c;
        if (!save_config_locked()) {
            G.cfg = old;
            if (fresh) G.token[0] = 0;
            pthread_mutex_unlock(&G.mu);
            http_error(fd, 500, "the settings could not be stored");
            return;
        }
        G.cfg_dirty = 1;
        char out[1200];
        buf_t b = {out, 0, sizeof out, 0};
        bputs(&b, "{\"config\":");
        config_json(&b, &G.cfg);
        if (fresh) { bputs(&b, ",\"admin_token\":"); bjson_str(&b, G.token); }
        bputs(&b, "}");
        pthread_mutex_unlock(&G.mu);
        http_reply(fd, 200, out, b.n);
    } else {
        http_error(fd, 404, "not found");
    }
}

static void *status_thread(void *arg)
{
    int ls = *(int *)arg;
    for (;;) {
        struct sockaddr_in peer;
        socklen_t pl = sizeof peer;
        int fd = accept(ls, (struct sockaddr *)&peer, &pl);
        if (fd < 0) { if (errno != EINTR) usleep(100000); continue; }
        if (peer.sin_family == AF_INET && lan_caller(&peer)) serve(fd);
        close(fd);
    }
    return NULL;
}

/* ------------------------------------------------ tailnet shim publishing */

/* When the tailnet shim runs on this radio, publish the status port to the
 * tailnet so a remote AetherSDR can read it. The lease is 10 minutes; renew
 * every 5. Without the shim the request just fails. */
static void *publish_thread(void *arg)
{
    (void)arg;
    int logged = -1;
    for (;;) {
        int ok = 0;
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons(48992)};
        inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
        struct timeval tv = {10, 0};
        if (fd >= 0) {
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        }
        if (fd >= 0 && !connect(fd, (struct sockaddr *)&sa, sizeof sa)) {
            char body[64], req[256], resp[512];
            int bl = snprintf(body, sizeof body, "{\"port\":%d,\"name\":\"AetherWSPR\"}", STATUS_PORT);
            int l = snprintf(req, sizeof req,
                             "POST /v1/local/expose HTTP/1.0\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\n"
                             "Content-Length: %d\r\n\r\n%s", bl, body);
            if (write(fd, req, (size_t)l) == l) {
                ssize_t k = read(fd, resp, sizeof resp - 1);
                if (k > 0) {
                    resp[k] = 0;
                    ok = strstr(resp, " 200 ") != NULL;
                }
            }
        }
        if (fd >= 0) close(fd);
        if (ok != logged) {
            logf_(ok ? "status port published on the tailnet by the shim" : "no tailnet shim to publish the status port");
            logged = ok;
        }
        sleep(ok ? 300 : 60);
    }
    return NULL;
}

/* ------------------------------------------------------------ radio API */

typedef struct {
    int fd;
    char in[65536];
    size_t n;
    int seq;
    /* outstanding request numbers we care about */
    int want_slice, want_stream, want_info;
    int slice;            /* our slice index, -1 none */
    unsigned stream;      /* our DAX stream id, 0 none */
    double stream_retry;  /* mono_s() to ask for a refused stream again, 0 none */
    /* Where the radio says each slice is tuned, by index. All of them: a
     * slice's status can arrive before the reply that names it ours. */
    long long rf_hz[MAX_SLICES];
    long long create_hz;  /* what we asked the next slice to be created at */
    unsigned handle;
    int lost;             /* our slice was removed: remake it now */
} api_t;

static int api_send(api_t *a, const char *fmt, ...)
{
    char line[512];
    int l = snprintf(line, sizeof line, "C%d|", ++a->seq);
    va_list ap;
    va_start(ap, fmt);
    l += vsnprintf(line + l, sizeof line - (size_t)l - 1, fmt, ap);
    va_end(ap);
    if (l <= 0 || (size_t)l >= sizeof line - 1) return -1;
    line[l++] = '\n';
    if (write(a->fd, line, (size_t)l) != l) return -1;
    return a->seq;
}

static const char *g_radio_addr = "127.0.0.1";

static int api_connect(api_t *a, int udp_port)
{
    memset(a, 0, sizeof *a);
    a->slice = -1;
    struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons(4992)};
    inet_pton(AF_INET, g_radio_addr, &sa.sin_addr);
    a->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (a->fd < 0) return 0;
    if (connect(a->fd, (struct sockaddr *)&sa, sizeof sa)) { close(a->fd); a->fd = -1; return 0; }
    int one = 1;
    setsockopt(a->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    api_send(a, "client program AetherWSPR");
    api_send(a, "client udpport %d", udp_port);
    a->want_info = api_send(a, "info");
    api_send(a, "sub gps all");
    api_send(a, "sub slice all");
    /* 4.3.16 and later. Older firmware answers with an error and we simply
     * never see an ntp_sources status, leaving the kernel flag as the only
     * evidence, exactly as before this subscription existed. */
    api_send(a, "sub ntp_sources all");
    return 1;
}

static void api_close(api_t *a)
{
    if (a->fd >= 0) close(a->fd);
    a->fd = -1;
}

static void make_slice(api_t *a, long long dial_hz, const char *ant, int dax)
{
    (void)dax;
    a->create_hz = dial_hz;
    if (ant[0]) a->want_slice = api_send(a, "slice create freq=%.6f rxant=%s mode=DIGU", dial_hz / 1e6, ant);
    else a->want_slice = api_send(a, "slice create freq=%.6f mode=DIGU", dial_hz / 1e6);
}

static void field(const char *s, const char *key, char *out, size_t cap)
{
    out[0] = 0;
    const char *p = strstr(s, key);
    if (!p) return;
    p += strlen(key);
    if (*p == '"') p++;
    size_t i = 0;
    while (p[i] && p[i] != '"' && p[i] != ' ' && p[i] != ',' && p[i] != '#' && i + 1 < cap) { out[i] = p[i]; i++; }
    out[i] = 0;
}

/* Handles one line from the radio. */
static void api_line(api_t *a, char *l, const config_t *cfg)
{
    if (l[0] == 'H') { a->handle = (unsigned)strtoul(l + 1, NULL, 16); return; }
    if (l[0] == 'R') {
        int seq = atoi(l + 1);
        char *p1 = strchr(l, '|');
        if (!p1) return;
        unsigned long code = strtoul(p1 + 1, NULL, 16);
        char *msg = strchr(p1 + 1, '|');
        msg = msg ? msg + 1 : (char *)"";
        if (seq == a->want_info && !code) {
            char call[16];
            field(msg, "callsign=", call, sizeof call);
            pthread_mutex_lock(&G.mu);
            for (char *q = call; *q; q++) *q = (char)toupper((unsigned char)*q);
            snprintf(G.radio_call, sizeof G.radio_call, "%s", call);
            pthread_mutex_unlock(&G.mu);
        } else if (seq == a->want_slice) {
            a->want_slice = 0;
            if (code) { logf_("slice create refused: %08lx %s", code, msg); return; }
            a->slice = atoi(msg);
            api_send(a, "slice set %d tx=0", a->slice);
            api_send(a, "slice set %d dax=%d", a->slice, cfg->dax_channel);
            a->want_stream = api_send(a, "stream create type=dax_rx dax_channel=%d", cfg->dax_channel);
            if (a->slice < MAX_SLICES && !a->rf_hz[a->slice]) a->rf_hz[a->slice] = a->create_hz;
            pthread_mutex_lock(&G.mu); G.slice_ok = 1; pthread_mutex_unlock(&G.mu);
            logf_("slice %d on %s", a->slice, G.band);
        } else if (seq == a->want_stream) {
            a->want_stream = 0;
            if (code) {
                logf_("DAX audio refused: %08lx %s", code, msg);
                a->stream_retry = mono_s() + 10;
                return;
            }
            a->stream_retry = 0;
            a->stream = (unsigned)strtoul(msg, NULL, 16);
            pthread_mutex_lock(&G.mu); G.stream_ok = 1; pthread_mutex_unlock(&G.mu);
            logf_("DAX audio stream 0x%08x", a->stream);
        }
        return;
    }
    if (l[0] != 'S') return;
    char *body = strchr(l, '|');
    if (!body) return;
    body++;
    if (!strncmp(body, "gps ", 4)) {
        char grid[8];
        field(body, "grid=", grid, sizeof grid);
        if (strlen(grid) >= 4) {
            pthread_mutex_lock(&G.mu);
            snprintf(G.gps_grid, sizeof G.gps_grid, "%s", grid);
            pthread_mutex_unlock(&G.mu);
        }
    } else if (!strncmp(body, "ntp_sources ", 12)) {
        /* One status per source. A source the radio has settled on reports
         * state=selected; that is enough to trust the clock on a radio whose
         * kernel never clears STA_UNSYNC. Latching is deliberate: chrony can
         * briefly drop a source while the clock stays disciplined, and losing
         * the cycle over that would cost two minutes of capture. */
        /* Match the FIELD, not the text anywhere in the line.
         *
         * The real status carries a free-text description= in quotes, so a
         * plain strstr(" state=selected") also fires on a description that
         * merely contains those words. A reviewer raised that as theoretical;
         * a fixture here turned it into a failing test. Requiring the token to
         * END -- a space, or the end of the line -- fixes it, because a
         * quoted description cannot end the token without closing the quote
         * first.
         *
         * The radio also publishes its own boolean, " selected=1". Checking it
         * too was tempting, but it agrees with this match on every real line,
         * so it would be code doing no work and no fixture could justify it. */
        const char *const TAG = " state=selected";
        const char *const st = strstr(body, TAG);
        const size_t tn = strlen(TAG);
        if (st && (st[tn] == ' ' || st[tn] == '\0'))
            g_ntp_selected = 1;
    } else if (!strncmp(body, "slice ", 6)) {
        int index = atoi(body + 6);
        if (index >= 0 && index < MAX_SLICES) {
            char mhz[24];
            field(body, "RF_frequency=", mhz, sizeof mhz);
            if (mhz[0]) a->rf_hz[index] = llround(atof(mhz) * 1e6);
            if (strstr(body, " in_use=0")) a->rf_hz[index] = 0;
        }
        if (index != a->slice) return;
        /* The radio removes headless slices when its last GUI client
         * disconnects, and anyone may close ours. Its DAX stream is left
         * attached to nothing, so drop that too and make both again. */
        if (strstr(body, " in_use=0")) {
            if (a->stream) api_send(a, "stream remove 0x%08X", a->stream);
            a->slice = -1;
            a->stream = 0;
            a->stream_retry = 0;
            a->lost = 1;
            pthread_mutex_lock(&G.mu); G.slice_ok = G.stream_ok = 0; pthread_mutex_unlock(&G.mu);
            logf_("our slice was removed; making it again");
        }
    }
}

/* The radio reports the slice to the Hz; anything else is another frequency. */
static int on_dial(const api_t *a, long long dial_hz)
{
    return a->slice >= 0 && a->slice < MAX_SLICES && llabs(a->rf_hz[a->slice] - dial_hz) < 2;
}

static int api_read(api_t *a, const config_t *cfg)
{
    ssize_t k = read(a->fd, a->in + a->n, sizeof a->in - 1 - a->n);
    if (k <= 0) return 0;
    a->n += (size_t)k;
    a->in[a->n] = 0;
    char *start = a->in, *nl;
    while ((nl = memchr(start, '\n', (size_t)(a->in + a->n - start)))) {
        *nl = 0;
        if (nl > start && nl[-1] == '\r') nl[-1] = 0;
        api_line(a, start, cfg);
        start = nl + 1;
    }
    a->n = (size_t)(a->in + a->n - start);
    memmove(a->in, start, a->n);
    if (a->n == sizeof a->in - 1) a->n = 0;   /* one absurdly long line: drop it */
    return 1;
}

/* VITA-49 DAX audio: 28-byte header, payload float32 stereo BE (PCC 0x03E3)
 * or int16 mono BE (PCC 0x0123), optional 4-byte trailer. */
/* Packets missing before this one. The 4-bit count wraps every 16 packets
 * and the radio's DAX timestamps don't advance, so a long gap is sized from
 * the arrival time: the count's residue (mod 16) closest to elapsed time. */
static int missing_packets(int last, int count, double elapsed, double per_s)
{
    int residue = (count - last - 1) & 0xF;
    double est = elapsed / per_s - 1;
    if (est < 16) return residue;
    int k = (int)((est - residue) / 16 + 0.5);
    return residue + 16 * (k < 0 ? 0 : k);
}

static void vita_packet(const unsigned char *p, ssize_t n, unsigned want, dsp_t *d, int capturing)
{
    if (n < 28 || !want) return;
    uint32_t w0 = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
    uint32_t sid = (uint32_t)p[4] << 24 | (uint32_t)p[5] << 16 | (uint32_t)p[6] << 8 | p[7];
    if (sid != want) return;
    unsigned pcc = (unsigned)p[14] << 8 | p[15];
    ssize_t end = n - ((w0 & 0x04000000u) ? 4 : 0);
    if (!capturing) return;
    /* A lost packet must become silence of the same length: dropping it
     * would pull every later sample earlier and break WSPR's symbol timing. */
    int count = (int)((w0 >> 16) & 0xF);
    ssize_t per = pcc == 0x03E3 ? (end - 28) / 8 : (end - 28) / 2;
    double at = mono_s();
    if (d->last_count >= 0 && per > 0) {
        int gap = missing_packets(d->last_count, count, at - d->last_at, (double)per / AUDIO_RATE);
        d->lost += gap;
        for (long long z = 0; z < (long long)gap * per; z++) dsp_push(d, 0.0f);
    }
    d->last_count = count;
    d->last_at = at;
    if (pcc == 0x03E3) {
        for (ssize_t o = 28; o + 8 <= end; o += 8) {
            uint32_t u = (uint32_t)p[o] << 24 | (uint32_t)p[o + 1] << 16 | (uint32_t)p[o + 2] << 8 | p[o + 3];
            float x;
            memcpy(&x, &u, 4);
            dsp_push(d, x);
        }
    } else if (pcc == 0x0123) {
        for (ssize_t o = 28; o + 2 <= end; o += 2)
            dsp_push(d, (int16_t)(p[o] << 8 | p[o + 1]) / 32768.0f);
    }
}

/* ------------------------------------------------------------- selftest */

/* The longest settings there are come back whole after a restart, and a
 * damaged file leaves the defaults. */
static int selftest_persistence(void)
{
    char dir[420];
    if (snprintf(dir, sizeof dir, "%s/persist", g_work) >= (int)sizeof dir) return 1;
    mkdir(dir, 0700);
    strcpy(G.state_path, dir);
    strcpy(G.token, "0123456789abcdef0123456789abcdef");
    config_default(&G.cfg);
    config_t c = G.cfg;
    const char *full = "{\"callsign\":\"VP2E/KK7GWY\",\"grid\":\"DN18pg\",\"band\":\"40m\",\"hopping\":true,"
        "\"gray_minutes\":120,\"depth\":\"deep\",\"rxant\":\"XVTA\",\"upload\":true,\"dax_channel\":3}";
    if (config_parse(full, strlen(full), &c)) { printf("selftest FAIL: persistence settings\n"); return 1; }
    for (int p = 0; p < 4; p++) c.hop[p] = (1u << (sizeof BANDS / sizeof BANDS[0])) - 1;
    G.cfg = c;
    save_config_locked();
    config_t want = G.cfg;
    char token[sizeof G.token];
    strcpy(token, G.token);

    config_default(&G.cfg);
    G.token[0] = 0;
    load_config();
    if (strcmp(G.token, token) || strcmp(G.cfg.callsign, want.callsign) || strcmp(G.cfg.grid, want.grid)
        || strcmp(G.cfg.band, want.band) || G.cfg.hopping != want.hopping || G.cfg.gray_minutes != want.gray_minutes
        || strcmp(G.cfg.rxant, want.rxant) || G.cfg.upload != want.upload || G.cfg.dax_channel != want.dax_channel
        || G.cfg.depth != want.depth) { printf("selftest FAIL: settings did not survive a restart\n"); return 1; }
    for (int p = 0; p < 4; p++)
        if (G.cfg.hop[p] != want.hop[p]) { printf("selftest FAIL: hop list %d did not survive a restart\n", p); return 1; }

    char path[540];
    snprintf(path, sizeof path, "%s/config.json", dir);
    FILE *f = fopen(path, "w");
    if (!f) return 1;
    fputs("{\"config\":{\"band\":\"11m\"}}", f);
    fclose(f);
    config_default(&G.cfg);
    config_t def = G.cfg;
    load_config();
    if (strcmp(G.cfg.band, def.band)) { printf("selftest FAIL: damaged settings were used\n"); return 1; }
    unlink(path);
    rmdir(dir);
    config_default(&G.cfg);
    G.state_path[0] = 0;
    G.token[0] = 0;
    return 0;
}

static double gauss(void)
{
    double u = (rand() + 1.0) / (RAND_MAX + 2.0), v = (rand() + 1.0) / (RAND_MAX + 2.0);
    return sqrt(-2 * log(u)) * cos(2 * M_PI * v);
}

static const char *band_for_cycle(const config_t *c, const char *grid, long long cycle_t, const char *current, int *period_out);
static const char *const COORDINATED[10];

/* What wsprd prints for each kind of call. */
static int selftest_spots(void)
{
    struct { const char *line, *call, *grid; } want[] = {
        {"0000 -21  0.1  14.097100  0  K1ABC FN42 37", "K1ABC", "FN42"},
        {"0000 -21  0.1  14.097100  0  <K1ABC> FN42AB 37", "K1ABC", "FN42AB"},
        {"0000 -21  0.1  14.097100  0  <PA0SKT/2> JO33 40", "PA0SKT/2", "JO33"},
        {"0000 -21  0.1  14.097100  0  <...> FN42AB 37", "", "FN42AB"},
    };
    for (size_t i = 0; i < sizeof want / sizeof want[0]; i++) {
        char line[80];
        spot_t s;
        snprintf(line, sizeof line, "%s", want[i].line);
        if (!parse_spot(line, 0, &s) || strcmp(s.call, want[i].call) || strcmp(s.grid, want[i].grid)) {
            printf("selftest FAIL: spot \"%s\" gave call \"%s\"\n", want[i].line, s.call);
            return 1;
        }
    }
    return 0;
}

static void record_cycle(const decode_t *dc);

/* Every decode wsprd can print reaches the spot list. */
static int selftest_busy_cycle(void)
{
    static decode_t dc;
    memset(&dc, 0, sizeof dc);
    for (int i = 0; i < MAX_CYCLE_SPOTS; i++)
        dc.n += (size_t)snprintf(dc.out + dc.n, sizeof dc.out - dc.n,
                                 "0000 -21  0.1  14.097%03d  0  K%dAB FN42 37\n", i, i % 10);
    int before = G.nspots;
    record_cycle(&dc);
    if (G.nspots - before != MAX_CYCLE_SPOTS) { printf("selftest FAIL: busy cycle kept %d of %d\n", G.nspots - before, MAX_CYCLE_SPOTS); return 1; }
    G.nspots = before;
    G.ncycles = 0;
    return 0;
}

/* What the radio says about our slice and stream. */
static int selftest_radio_lines(void)
{
    int p[2];
    if (pipe(p)) return 1;
    fcntl(p[1], F_SETFL, O_NONBLOCK);
    api_t a;
    memset(&a, 0, sizeof a);
    a.fd = p[1];
    a.slice = 0;
    config_t c;
    config_default(&c);
    char l[160];
    a.want_info = 7;
    strcpy(l, "R7|0|model=\"FLEX-8600\",callsign=kk7gwy,name=x");
    api_line(&a, l, &c);
    int fail = strcmp(G.radio_call, "KK7GWY") != 0;
    a.slice = -1;
    strcpy(l, "S1|slice 0 RF_frequency=7.038600 mode=DIGU");   /* before the reply */
    api_line(&a, l, &c);
    a.want_slice = 8;
    a.create_hz = 7000000;   /* the radio's word beats what we asked for */
    strcpy(l, "R8|0|0");
    api_line(&a, l, &c);
    fail |= a.slice != 0 || !on_dial(&a, 7038600) || on_dial(&a, 14095600);
    strcpy(l, "S1|slice 1 RF_frequency=14.095600");   /* someone else's */
    api_line(&a, l, &c);
    fail |= !on_dial(&a, 7038600);
    strcpy(l, "S1|slice 0 RF_frequency=14.095600");   /* someone moved ours */
    api_line(&a, l, &c);
    fail |= on_dial(&a, 7038600) || !on_dial(&a, 14095600);
    /* A slice no status ever described is where it was asked to be. */
    api_t b;
    memset(&b, 0, sizeof b);
    b.fd = p[1];
    b.slice = -1;
    b.want_slice = 3;
    b.create_hz = 3568600;
    strcpy(l, "R3|0|1");
    api_line(&b, l, &c);
    fail |= !on_dial(&b, 3568600);
    strcpy(l, "S1|slice 1 in_use=0");
    api_line(&b, l, &c);
    fail |= b.slice != -1 || b.rf_hz[1] != 0;
    a.want_stream = 9;
    strcpy(l, "R9|50000016|");
    api_line(&a, l, &c);
    fail |= a.want_stream != 0 || a.stream_retry <= 0;
    a.want_stream = 10;
    strcpy(l, "R10|0|04000008");
    api_line(&a, l, &c);
    fail |= a.stream != 0x04000008 || a.stream_retry != 0;
    /* FlexLib's key for the antenna (Radio.cs RequestSlice). */
    char sent[256];
    while (read(p[0], sent, sizeof sent) == (ssize_t)sizeof sent) {}
    make_slice(&a, 14095600, "ANT2", 4);
    ssize_t k = read(p[0], sent, sizeof sent - 1);
    sent[k > 0 ? k : 0] = 0;
    fail |= !strstr(sent, " rxant=ANT2 ");
    close(p[0]);
    close(p[1]);
    G.radio_call[0] = 0;
    G.stream_ok = 0;
    if (fail) { printf("selftest FAIL: radio lines\n"); return 1; }
    return 0;
}

/* The clock fallback for a radio whose kernel never clears STA_UNSYNC.
 *
 * A FLEX-6000 runs kernel 2.6.37: chronyd disciplines the clock, the wall
 * clock is right, and STA_UNSYNC stays set forever. The agent therefore never
 * left waiting_clock on the hardware this fix is for. clock_synced() now
 * accepts the radio's own report that it has settled on a source.
 *
 * clock_synced() reads the kernel through g_kernel_clock_synced, which this
 * test substitutes so the kernel can be held at UNSYNC on a synced build host.
 * It also drives the API line parser -- the same api_line() the radio feeds --
 * and asserts on the latch it sets, which no host clock can influence. */

/* Substituted for the kernel read: always "not synced", the FLEX-6000 state. */
static int kernel_says_unsynced(void) { return 0; }
static int selftest_ntp_fallback(void)
{
    int fail = 0;
    int p[2];
    if (pipe(p)) return 1;
    fcntl(p[1], F_SETFL, O_NONBLOCK);
    api_t a;
    memset(&a, 0, sizeof a);
    a.fd = p[1];
    a.slice = 0;
    config_t c;
    config_default(&c);
    char l[320];   /* the real ntp_sources line is ~180 bytes */
    const int saved = g_ntp_selected;

    /* Source states a radio reports that are NOT evidence. A candidate or a
     * source being measured must not be mistaken for a disciplined clock, and
     * "selected" must be matched as a whole word with its leading space --
     * `state=selectable` is a real chrony state and starts with "selected"
     * only if you compare carelessly. */
    /* The real wire shape, from a FLEX-6300 on 4.3.16.42034 (see below): the
     * first word is `id=`, not an index. An earlier version of this test
     * invented "ntp_sources 1 state=..." and a reviewer caught it. */
    const char *not_evidence[] = {
        "S17B07863|ntp_sources id=default-pool address=0.debian.pool.ntp.org enabled=1 directive=pool selected=0 prefer=0 state=candidate stratum=3 reach=255 description=\"Debian NTP pool\"",
        "S17B07863|ntp_sources id=default-pool address=1.debian.pool.ntp.org enabled=1 directive=pool selected=0 prefer=0 state=selectable stratum=2 reach=255 description=\"Debian NTP pool\"",
        "S17B07863|ntp_sources id=default-pool address=2.debian.pool.ntp.org enabled=1 directive=pool selected=0 prefer=0 state=falseticker stratum=3 reach=255 description=\"Debian NTP pool\"",
        "S17B07863|ntp_sources id=default-pool address=3.debian.pool.ntp.org enabled=1 directive=pool selected=0 prefer=0 state=unreachable stratum=0 reach=0 description=\"Debian NTP pool\"",
        /* A description that merely CONTAINS the word, and an address that
         * starts with "deselected": neither is a selected source. */
        "S17B07863|ntp_sources id=default-pool address=deselected.example.org enabled=1 directive=pool selected=0 prefer=0 state=candidate stratum=3 reach=255 description=\"not state=selected\"",
    };
    for (size_t i = 0; i < sizeof not_evidence / sizeof not_evidence[0]; i++) {
        g_ntp_selected = 0;
        snprintf(l, sizeof l, "%s", not_evidence[i]);
        api_line(&a, l, &c);
        if (g_ntp_selected) {
            printf("selftest FAIL: ntp state accepted as synced: %s\n", not_evidence[i]);
            fail = 1;
        }
    }

    /* VERBATIM from a FLEX-6300, model="FLEX-6300", software_ver=4.3.16.42034,
     * captured 2026-11-09 while its kernel still reported STA_UNSYNC. This is
     * the evidence the fallback accepts. Note stratum=1: the radio had settled
     * on a stratum-1 source and still would not clear the kernel flag. */
    g_ntp_selected = 0;
    strcpy(l, "S17B07863|ntp_sources id=default-pool address=2.debian.pool.ntp.org enabled=1 "
              "directive=pool selected=1 prefer=0 state=selected stratum=1 reach=255 "
              "description=\"Debian NTP pool\"");
    api_line(&a, l, &c);
    if (!g_ntp_selected) {
        printf("selftest FAIL: a selected ntp source was not accepted\n");
        fail = 1;
    }

    /* The decision, with the kernel's answer supplied rather than read.
     * Row 2 is the FLEX-6000 case the fix exists for: kernel says no, radio
     * says a source is selected, and the agent must capture. */
    {
        const struct { int kernel, radio, want; const char *what; } cases[] = {
            {1, 0, 1, "kernel synced, no radio report"},
            {0, 1, 1, "FLEX-6000: kernel UNSYNC, radio source selected"},
            {1, 1, 1, "both agree"},
            {0, 0, 0, "neither: the agent must wait"},
        };
        for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
            const int got = clock_synced_from(cases[i].kernel, cases[i].radio);
            if (got != cases[i].want) {
                printf("selftest FAIL: clock %s gave %d, want %d\n",
                       cases[i].what, got, cases[i].want);
                fail = 1;
            }
        }
    }

    /* And the real clock_synced(), with the kernel held at UNSYNC -- the
     * FLEX-6000 state. This is the assertion that proves the shipped path:
     * with the kernel saying no, the radio's verdict is the only thing that
     * can return true, so dropping it cannot pass. On a synced build host an
     * un-substituted clock_synced() would return 1 whatever the radio said.
     *
     * g_ntp_selected is still 1 here, latched by the lines driven above. */
    {
        int (*const saved_kernel)(void) = g_kernel_clock_synced;
        g_kernel_clock_synced = kernel_says_unsynced;

        if (!clock_synced()) {
            printf("selftest FAIL: clock_synced() false with the kernel UNSYNC "
                   "and an ntp source selected -- the FLEX-6000 case\n");
            fail = 1;
        }
        /* The other half: with no source selected either, it must wait. A
         * clock_synced() hard-wired to 1 fails here. */
        g_ntp_selected = 0;
        if (clock_synced()) {
            printf("selftest FAIL: clock_synced() true with the kernel UNSYNC "
                   "and no ntp source selected\n");
            fail = 1;
        }
        g_ntp_selected = 1;
        g_kernel_clock_synced = saved_kernel;
    }

    /* The latch holds: chrony can drop a source for one poll while the clock
     * stays disciplined, and losing waiting_clock over that would cost two
     * minutes of capture. */
    strcpy(l, "S17B07863|ntp_sources id=default-pool address=2.debian.pool.ntp.org enabled=1 "
              "directive=pool selected=0 prefer=0 state=candidate stratum=1 reach=255 "
              "description=\"Debian NTP pool\"");
    api_line(&a, l, &c);
    if (!g_ntp_selected) {
        printf("selftest FAIL: the ntp latch dropped on a transient state\n");
        fail = 1;
    }

    /* The status carries clock_source so an operator can tell a GPS-locked
     * radio from a 6000 running on this fallback. */
    {
        static char out[512 * 1024];
        buf_t b = {out, 0, sizeof out, 0};
        status_json(&b, 0);
        if (b.full || !strstr(out, "\"clock_source\":")) {
            printf("selftest FAIL: status has no clock_source\n");
            fail = 1;
        }
    }

    /* The agent must actually ASK the radio for ntp_sources. Injecting status
     * lines above proves the parser, not the subscription -- and without the
     * subscription a real radio sends nothing and the agent waits forever.
     *
     * api_connect() dials g_radio_addr:4992, and g_radio_addr defaults to
     * 127.0.0.1, so a listener on loopback:4992 catches the real function's
     * output. This is what went on the wire, not a grep of the source. */
    {
        int srv = socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        struct sockaddr_in sa;
        memset(&sa, 0, sizeof sa);
        sa.sin_family = AF_INET;
        sa.sin_port = htons(4992);
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (srv >= 0)
            setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        if (srv < 0 || bind(srv, (struct sockaddr *)&sa, sizeof sa) || listen(srv, 1)) {
            /* Port 4992 is busy -- a real SmartSDR or another agent is on this
             * host. Say so rather than reporting a pass that did not happen. */
            printf("selftest SKIP: port 4992 busy, subscription not checked\n");
        } else {
            api_t s;
            int ok = api_connect(&s, 0);
            int c = ok ? accept(srv, NULL, NULL) : -1;
            char wire[2048] = "";
            if (c >= 0) {
                struct timeval tv = {1, 0};
                setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
                ssize_t n = recv(c, wire, sizeof wire - 1, 0);
                wire[n > 0 ? n : 0] = 0;
                close(c);
            }
            if (!ok) {
                printf("selftest FAIL: api_connect to the loopback listener failed\n");
                fail = 1;
            } else if (!strstr(wire, "sub ntp_sources all")) {
                printf("selftest FAIL: the agent never subscribes to ntp_sources\n");
                fail = 1;
            }
            if (ok) api_close(&s);
        }
        if (srv >= 0) close(srv);
    }

    g_ntp_selected = saved;
    close(p[0]);
    close(p[1]);
    return fail;
}

/* A POST that can't be stored is refused and changes nothing: not the live
 * settings, and not the claim. Driven through the real request handler. */
static int selftest_save_failure(void)
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv)) return 1;
    config_default(&G.cfg);
    G.token[0] = 0;
    snprintf(G.state_path, sizeof G.state_path, "%s/no/such/dir", g_work);
    const char body[] = "{\"band\":\"40m\"}";
    char req[200], reply[256] = "";
    int l = snprintf(req, sizeof req, "POST /v1/config HTTP/1.0\r\nContent-Length: %zu\r\n\r\n%s", strlen(body), body);
    if (write(sv[0], req, (size_t)l) != l) return 1;
    serve(sv[1]);
    ssize_t k = read(sv[0], reply, sizeof reply - 1);
    close(sv[0]);
    close(sv[1]);
    int fail = k <= 0 || !strstr(reply, " 500 ") || strstr(reply, "admin_token")
        || strcmp(G.cfg.band, "20m") || G.token[0];
    config_default(&G.cfg);
    G.state_path[0] = 0;
    G.token[0] = 0;
    if (fail) { printf("selftest FAIL: an unsaved POST was accepted\n"); return 1; }
    return 0;
}

/* Upload off leaves nothing queued; on keeps the queue. */
static int selftest_upload_off(void)
{
    config_default(&G.cfg);
    U.head = 3; U.n = 2;
    G.cfg.upload = 1;
    if (upload_drop_if_off() || U.n != 2) { printf("selftest FAIL: upload on dropped the queue\n"); return 1; }
    G.cfg.upload = 0;
    if (!upload_drop_if_off() || U.n != 0) { printf("selftest FAIL: upload off kept the queue\n"); return 1; }
    U.head = 0;
    return 0;
}

static const char *BAD_GRIDS[] = {"ZZ99", "AA00zz", "D18P", "DN1", "DN18p", "1234", "DN18-g"};

static int selftest(double snr_db)
{
    if (selftest_persistence() || selftest_spots() || selftest_upload_off()
        || selftest_busy_cycle() || selftest_radio_lines() || selftest_ntp_fallback()
        || selftest_save_failure()) return 1;
    for (size_t i = 0; i < sizeof BAD_GRIDS / sizeof BAD_GRIDS[0]; i++)
        if (valid_grid(BAD_GRIDS[i])) { printf("selftest FAIL: grid %s\n", BAD_GRIDS[i]); return 1; }
    if (!valid_grid("") || !valid_grid("DN18") || !valid_grid("dn18pg") || !valid_grid("RR99xx")) { printf("selftest FAIL: good grid\n"); return 1; }
    /* Hopping: every coordinated band ticked follows the slot exactly; a
     * subset only ever yields ticked bands; nothing ticked stays put. */
    {
        config_t c;
        config_default(&c);
        c.hopping = 1;
        for (int p = 0; p < 4; p++)
            for (int i = 0; i < 10; i++) c.hop[p] |= 1u << band_index(COORDINATED[i]);
        long long t0 = 1793836800; /* 2026-11-04 00:00 UTC */
        for (int k = 0; k < 720; k++) {
            int period;
            long long t = t0 + 120LL * k;
            const char *b = band_for_cycle(&c, "DN18pg", t, "", &period);
            const char *want = COORDINATED[((t / 60) % 60 % 20) / 2];
            if (strcmp(b, want) || period < 0) { printf("selftest FAIL: hop %d gave %s, want %s\n", k, b, want); return 1; }
        }
        unsigned sub = (1u << band_index("40m")) | (1u << band_index("6m"));
        for (int p = 0; p < 4; p++) c.hop[p] = sub;
        int seen6 = 0;
        for (int k = 0; k < 720; k++) {
            int period;
            const char *b = band_for_cycle(&c, "DN18pg", t0 + 120LL * k, "", &period);
            if (strcmp(b, "40m") && strcmp(b, "6m")) { printf("selftest FAIL: subset hop gave %s\n", b); return 1; }
            seen6 += !strcmp(b, "6m");
        }
        if (!seen6) { printf("selftest FAIL: a non-coordinated ticked band never came up\n"); return 1; }
        for (int p = 0; p < 4; p++) c.hop[p] = 0;
        int period;
        if (strcmp(band_for_cycle(&c, "DN18pg", t0, "17m", &period), "17m")) { printf("selftest FAIL: empty hop moved\n"); return 1; }
        c.hopping = 0;
        if (strcmp(band_for_cycle(&c, "DN18pg", t0, "17m", &period), "20m") || period != -1) { printf("selftest FAIL: hopping off\n"); return 1; }
        /* Settings round trip. */
        config_t r;
        config_default(&r);
        const char *js = "{\"band\":\"30m\",\"hopping\":true,\"gray_minutes\":45,\"hop_day\":[\"20m\",\"6m\"],\"hop_night\":[],\"depth\":\"deep\"}";
        if (config_parse(js, strlen(js), &r) || strcmp(r.band, "30m") || !r.hopping || r.gray_minutes != 45 || r.depth != 3
            || r.hop[1] != ((1u << band_index("20m")) | (1u << band_index("6m")))) { printf("selftest FAIL: settings parse\n"); return 1; }
        const char *bad[] = {"{\"gray_minutes\":0}", "{\"depth\":\"max\"}", "{\"hop_noon\":[]}", "{\"band\":\"11m\"}", "{\"bands\":[\"20m\"]}"};
        for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
            if (!config_parse(bad[i], strlen(bad[i]), &r)) { printf("selftest FAIL: accepted %s\n", bad[i]); return 1; }
    }

    /* Gap sizing: in-order, small gaps, and gaps past the 4-bit wrap. */
    const double per = 128.0 / AUDIO_RATE;
    struct { int last, count; double elapsed; int want; } gaps[] = {
        {5, 6, per, 0}, {5, 8, 3 * per, 2}, {15, 0, per, 0},
        {5, (5 + 41) & 15, 41 * per, 40}, {5, (5 + 41) & 15, 41 * per * 1.15, 40}, {5, 6, 17 * per, 16},
    };
    for (size_t i = 0; i < sizeof gaps / sizeof gaps[0]; i++) {
        int got = missing_packets(gaps[i].last, gaps[i].count, gaps[i].elapsed, per);
        if (got != gaps[i].want) { printf("selftest FAIL: gap case %zu gave %d, want %d\n", i, got, gaps[i].want); return 1; }
    }

    unsigned char sym[162];
    char msg[] = "K1ABC FN42 37";
    char *hashtab = calloc(32768 * 13, 1), *loctab = calloc(32768 * 5, 1);
    if (get_wspr_channel_symbols(msg, hashtab, loctab, sym) != 1) { fprintf(stderr, "symbol generation failed\n"); return 1; }
    const double f0 = 55.0, df = 375.0 / 256.0, tsym = 256.0 / 375.0;
    /* Noise sigma 1 at 24 kHz: noise power in 2500 Hz is 2500/12000. */
    double amp = sqrt(2 * pow(10, snr_db / 10) * 2500.0 / 12000.0);
    static float idat[CAPTURE_SAMPLES], qdat[CAPTURE_SAMPLES];
    dsp_t d;
    d.idat = idat;
    d.qdat = qdat;
    dsp_reset(&d);
    srand(1234);
    double ph = 0;
    long long total = (long long)CAPTURE_SECONDS * AUDIO_RATE;
    for (long long i = 0; i < total; i++) {
        double t = (double)i / AUDIO_RATE - 1.0;  /* WSPR starts 1 s in */
        double s = 0;
        if (t >= 0 && t < 162 * tsym) {
            int k = (int)(t / tsym);
            ph += 2 * M_PI * (1500.0 + f0 + (sym[k] - 1.5) * df) / AUDIO_RATE;
            s = amp * cos(ph);
        }
        dsp_push(&d, (float)(0.01 * (s + gauss())));
    }
    char c2[400];
    snprintf(c2, sizeof c2, "%s/260101_0000.c2", g_work);
    if (!write_c2(c2, "260101_0000.c2", 14.0956, idat, qdat, d.n)) { fprintf(stderr, "write c2 failed\n"); return 1; }
    decode_t dc = {0};
    if (!start_decode(&dc, c2, 0)) return 1;
    int st;
    for (;;) {
        ssize_t k = read(dc.fd, dc.out + dc.n, sizeof dc.out - 1 - dc.n);
        if (k > 0) { dc.n += (size_t)k; continue; }
        if (k == 0) break;
        if (errno == EAGAIN) { usleep(20000); continue; }
        break;
    }
    waitpid(dc.pid, &st, 0);
    dc.out[dc.n] = 0;
    printf("selftest at %.1f dB (%d baseband samples):\n%s", snr_db, d.n, dc.out);
    int ok = strstr(dc.out, "K1ABC FN42 37") && strstr(dc.out, "14.097155");
    printf("selftest %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

/* ------------------------------------------------------------------ main */


static void record_cycle(const decode_t *dc)
{
    char *line, *save = NULL;
    spot_t got[MAX_CYCLE_SPOTS];
    int n = 0;
    for (line = strtok_r((char *)dc->out, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (strstr(line, "<DecodeFinished>")) continue;
        char tmp[256];
        snprintf(tmp, sizeof tmp, "%s", line);
        if (n < MAX_CYCLE_SPOTS && parse_spot(tmp, dc->cycle_t, &got[n])) n++;
    }
    pthread_mutex_lock(&G.mu);
    for (int i = 0; i < n; i++) {
        int idx = (G.spot_head + G.nspots) % MAX_SPOTS;
        if (G.nspots == MAX_SPOTS) G.spot_head = (G.spot_head + 1) % MAX_SPOTS; else G.nspots++;
        G.spots[idx] = got[i];
    }
    int cidx = (G.cycle_head + G.ncycles) % MAX_CYCLES;
    if (G.ncycles == MAX_CYCLES) G.cycle_head = (G.cycle_head + 1) % MAX_CYCLES; else G.ncycles++;
    cycle_t *c = &G.cycles[cidx];
    c->t = dc->cycle_t;
    c->dial_hz = dc->dial_hz;
    snprintf(c->band, sizeof c->band, "%s", dc->band);
    c->spots = n;
    c->decode_ms = (int)((mono_s() - dc->started) * 1000);
    c->audio_pct = dc->audio_pct;
    c->lost_packets = dc->lost;
    c->audio_dbfs = dc->dbfs;
    pthread_mutex_unlock(&G.mu);
    logf_("cycle %lld %s: %d spot(s), decode %d ms (%.1f s CPU, %ld KB peak, radio %.0f%% busy), audio %d%%, "
          "%d packet(s) lost, level %.1f dBFS, %ld KB free", dc->cycle_t, dc->band, n, c->decode_ms, dc->child_cpu_s,
          dc->child_rss_kb, dc->sys_busy_pct, dc->audio_pct, dc->lost, dc->dbfs, dc->mem_avail_kb);
    queue_cycle_uploads(dc->cycle_t, dc->dial_hz, got, n);
}


/* WSJT-X's coordinated band hopping: slot (minute % 20) / 2 of each
 * two-minute cycle names one of these ten bands, used if it is ticked for
 * the period of the day; otherwise the next band of a random permutation of
 * the period's ticked bands. Hopping off, no grid or nothing ticked: stay. */
static const char *const COORDINATED[10] = {"160m", "80m", "60m", "40m", "30m", "20m", "17m", "15m", "12m", "10m"};
static int g_perm[MAX_BANDS], g_nperm;   /* main thread only */

/* *period is the hopping period used, -1 when not hopping. */
static const char *band_for_cycle(const config_t *c, const char *grid, long long cycle_t, const char *current, int *period_out)
{
    *period_out = -1;
    if (!c->hopping || strlen(grid) < 4) return c->band;
    time_t tt = (time_t)cycle_t;
    struct tm tm;
    gmtime_r(&tt, &tm);
    float uth = (float)tm.tm_hour + (float)tm.tm_min / 60.0f + (float)tm.tm_sec / 3600.0f;
    int period = wspr_grayline(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, uth, grid, c->gray_minutes);
    *period_out = period;
    unsigned mask = c->hop[period];
    if (!mask) return current[0] ? current : c->band;
    int ci = band_index(COORDINATED[(tm.tm_min % 20) / 2]);
    if (mask & (1u << ci)) {
        /* As WSJT-X does, keep a coordinated band from also coming up in
         * the random permutation too soon after. */
        for (int i = 0; i < g_nperm; i++)
            if (g_perm[i] == ci) { memmove(&g_perm[i], &g_perm[i + 1], sizeof g_perm[0] * (size_t)(g_nperm - i - 1)); g_nperm--; break; }
        return BANDS[ci].name;
    }
    int stale = !g_nperm;
    for (int i = 0; i < g_nperm; i++) if (!(mask & (1u << g_perm[i]))) stale = 1;
    if (stale) {
        g_nperm = 0;
        for (size_t i = 0; i < sizeof BANDS / sizeof BANDS[0]; i++) if (mask & (1u << i)) g_perm[g_nperm++] = (int)i;
        for (int i = g_nperm - 1; i > 0; i--) {
            int j = rand() % (i + 1), t = g_perm[i];
            g_perm[i] = g_perm[j];
            g_perm[j] = t;
        }
    }
    int b = g_perm[0];
    memmove(&g_perm[0], &g_perm[1], sizeof g_perm[0] * (size_t)(g_nperm - 1));
    g_nperm--;
    return BANDS[b].name;
}

int main(int argc, char *argv[])
{
    if (argc >= 2 && !strcmp(argv[1], "decode")) return wsprd_main(argc - 1, argv + 1);
    signal(SIGPIPE, SIG_IGN);
    /* PID 1 in a container: the kernel drops SIGTERM unless it is handled. */
    signal(SIGTERM, on_term);
    design_lowpass(g_h1, 800.0 / AUDIO_RATE);
    design_lowpass(g_h2, 187.5 / (AUDIO_RATE / DECIM1));
    snprintf(g_self, sizeof g_self, "%s", argv[0]);
    char self_dir[400];
    snprintf(self_dir, sizeof self_dir, "%s", argv[0]);
    char *slash = strrchr(self_dir, '/');
    if (slash) *slash = 0; else strcpy(self_dir, ".");
    /* Decoder scratch (the .c2 file, wsprd's hash table) lives in RAM. */
    const char *tmp = getenv("TMPDIR") && *getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
    snprintf(g_work, sizeof g_work, "%s/aether-wspr", tmp);
    mkdir(g_work, 0700);
    if (argc >= 2 && !strcmp(argv[1], "selftest")) return selftest(argc >= 3 ? atof(argv[2]) : -24);
    /* The radio tells a container its API address. */
    if (getenv("SSDR_RADIO_ADDRESS") && *getenv("SSDR_RADIO_ADDRESS")) g_radio_addr = getenv("SSDR_RADIO_ADDRESS");
    if (getenv("AETHER_WSPR_RADIO")) g_radio_addr = getenv("AETHER_WSPR_RADIO");

    srand((unsigned)time(NULL) ^ (unsigned)getpid());
    config_default(&G.cfg);
    pick_state_dir(self_dir);
    load_config();
    G.started = (long long)mono_s();
    logf_("AetherWSPR %s, state in %s", AGENT_VERSION, G.state_path[0] ? G.state_path : "(none)");

    int ls = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in la = {.sin_family = AF_INET, .sin_port = htons(STATUS_PORT), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (bind(ls, (struct sockaddr *)&la, sizeof la) || listen(ls, 8)) { logf_("cannot listen on %d: %s", STATUS_PORT, strerror(errno)); return 1; }
    pthread_t th;
    pthread_create(&th, NULL, status_thread, &ls);
    pthread_create(&th, NULL, upload_thread, NULL);
    if (!getenv("AETHER_WSPR_RADIO")) pthread_create(&th, NULL, publish_thread, NULL);

    int udp = socket(AF_INET, SOCK_DGRAM, 0);
    int rcv = 1 << 20;
    setsockopt(udp, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof rcv);
    struct sockaddr_in ua = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_ANY)};
    bind(udp, (struct sockaddr *)&ua, sizeof ua);
    socklen_t ual = sizeof ua;
    getsockname(udp, (struct sockaddr *)&ua, &ual);
    int udp_port = ntohs(ua.sin_port);

    static float idat[CAPTURE_SAMPLES], qdat[CAPTURE_SAMPLES];
    static unsigned char pkt[65536];
    dsp_t d;
    d.idat = idat;
    d.qdat = qdat;
    dsp_reset(&d);

    api_t a = {.fd = -1, .slice = -1};
    decode_t dc = {.pid = 0, .fd = -1};
    long long capture_t = 0;   /* cycle being recorded, 0 none */
    long long capture_dial = 0; /* what it was tuned to when it started: */
    char capture_band[8] = "";  /* by its end the slice may be moving on */
    long long tuned_for = 0;   /* cycle the slice is tuned for */
    long long skipped = 0;     /* cycle already logged as off its dial */
    double next_connect = 0;
    config_t cfg = G.cfg;

    for (;;) {
        double now = now_s();
        int old_dax = cfg.dax_channel;
        char old_rxant[sizeof cfg.rxant];
        snprintf(old_rxant, sizeof old_rxant, "%s", cfg.rxant);
        pthread_mutex_lock(&G.mu);
        cfg = G.cfg;
        int dirty = G.cfg_dirty;
        G.cfg_dirty = 0;
        pthread_mutex_unlock(&G.mu);
        if (dirty && a.fd >= 0 && a.slice >= 0 && cfg.rxant[0])
            api_send(&a, "slice set %d rxant=%s", a.slice, cfg.rxant);
        if (dirty && a.fd >= 0 && a.slice >= 0
            && (cfg.dax_channel != old_dax || (old_rxant[0] && !cfg.rxant[0]))) {
            /* A new DAX channel needs a new slice and stream, and only a new
             * slice can go back to the radio's default antenna; bands and
             * the rest apply at the next cycle without one. */
            api_send(&a, "slice remove %d", a.slice);
            if (a.stream) api_send(&a, "stream remove 0x%08X", a.stream);
            a.slice = -1;
            a.stream = 0;
            a.stream_retry = 0;
            tuned_for = 0;
            pthread_mutex_lock(&G.mu); G.slice_ok = G.stream_ok = 0; pthread_mutex_unlock(&G.mu);
        }

        if (a.fd < 0 && mono_s() >= next_connect) {
            pthread_mutex_lock(&G.mu); G.state = "connecting"; pthread_mutex_unlock(&G.mu);
            if (api_connect(&a, udp_port)) logf_("connected to the radio at %s", g_radio_addr);
            else next_connect = mono_s() + 5;
            tuned_for = 0;
        }

        if (a.lost) {
            a.lost = 0;
            tuned_for = 0;
            if (capture_t) logf_("cycle %lld abandoned: the slice was removed mid-cycle", capture_t);
            capture_t = 0;
        }
        long long cyc = (long long)now / 120 * 120;
        long long next = cyc + 120;
        int synced = clock_synced();

        /* Slice for the next cycle: made or retuned once the capture has
         * ended (second 114), so it settles before the cycle starts and the
         * capture keeps its band to the last sample. */
        if (a.fd >= 0 && synced && !a.want_slice && !capture_t) {
            long long target = (now - cyc) >= CAPTURE_SECONDS ? next : cyc;
            if (target != tuned_for) {
                char grid[8], cur[8];
                pthread_mutex_lock(&G.mu);
                snprintf(grid, sizeof grid, "%s", cfg.grid[0] ? cfg.grid : G.gps_grid);
                snprintf(cur, sizeof cur, "%s", G.band);
                pthread_mutex_unlock(&G.mu);
                int period;
                const char *band = band_for_cycle(&cfg, grid, target, cur, &period);
                long long dial = band_dial(band);
                if (a.slice < 0) make_slice(&a, dial, cfg.rxant, cfg.dax_channel);
                else {
                    api_send(&a, "slice tune %d %.6f", a.slice, dial / 1e6);
                    if (cfg.rxant[0]) api_send(&a, "slice set %d rxant=%s", a.slice, cfg.rxant);
                }
                pthread_mutex_lock(&G.mu);
                snprintf(G.band, sizeof G.band, "%s", band);
                G.dial_hz = dial;
                G.hop_period = period;
                pthread_mutex_unlock(&G.mu);
                tuned_for = target;
            }
        }

        if (a.fd >= 0 && a.slice >= 0 && !a.stream && !a.want_stream && a.stream_retry
            && mono_s() >= a.stream_retry) {
            a.stream_retry = 0;
            a.want_stream = api_send(&a, "stream create type=dax_rx dax_channel=%d", cfg.dax_channel);
        }

        /* Start a capture at the top of an even minute, if the slice is
         * tuned for it and audio is flowing. A slice that isn't where the
         * cycle says, because the radio refused the band or another client
         * moved it, would publish its decodes under the wrong frequency. */
        if (!capture_t && synced && a.stream && tuned_for == cyc && now - cyc < 1.0 && skipped != cyc) {
            pthread_mutex_lock(&G.mu);
            long long want = G.dial_hz;
            pthread_mutex_unlock(&G.mu);
            if (!on_dial(&a, want)) {
                logf_("cycle %lld skipped: the slice isn't at %lld Hz", cyc, want);
                skipped = cyc;
                continue;
            }
            dsp_reset(&d);
            capture_t = cyc;
            pthread_mutex_lock(&G.mu);
            G.state = "capturing";
            capture_dial = G.dial_hz;
            snprintf(capture_band, sizeof capture_band, "%s", G.band);
            pthread_mutex_unlock(&G.mu);
        }
        if (capture_t && (d.n >= CAPTURE_SAMPLES || now - capture_t >= CAPTURE_SECONDS + 1)) {
            int pct = (int)(100.0 * d.n / CAPTURE_SAMPLES);
            if (!on_dial(&a, capture_dial)) {
                logf_("cycle %lld discarded: the slice moved during it", capture_t);
            } else if (dc.pid) {
                logf_("previous decode still running; skipping cycle %lld", capture_t);
            } else if (pct < 80) {
                logf_("cycle %lld: only %d%% of the audio arrived; not decoded", capture_t, pct);
            } else {
                char name[20], path[400];
                time_t tt = (time_t)capture_t;
                struct tm tm;
                gmtime_r(&tt, &tm);
                strftime(name, sizeof name, "%y%m%d_%H%M.c2", &tm);
                snprintf(path, sizeof path, "%s/%s", g_work, name);
                pthread_mutex_lock(&G.mu); G.state = "decoding"; pthread_mutex_unlock(&G.mu);
                if (write_c2(path, name, capture_dial / 1e6, idat, qdat, d.n) && start_decode(&dc, path, cfg.depth)) {
                    dc.cycle_t = capture_t;
                    dc.dial_hz = capture_dial;
                    snprintf(dc.band, sizeof dc.band, "%s", capture_band);
                    dc.audio_pct = pct;
                    dc.lost = d.lost;
                    dc.dbfs = d.in ? (float)(10 * log10(d.sumsq / (double)d.in + 1e-20)) : -200.0f;
                }
            }
            capture_t = 0;
        }

        pthread_mutex_lock(&G.mu);
        if (!synced) G.state = "waiting_clock";
        else if (a.fd >= 0 && !capture_t && !dc.pid) G.state = a.stream ? "waiting_cycle" : "starting_audio";
        pthread_mutex_unlock(&G.mu);

        struct pollfd pf[3];
        int np = 0;
        pf[np++] = (struct pollfd){.fd = udp, .events = POLLIN};
        int ai = -1, di = -1;
        if (a.fd >= 0) { ai = np; pf[np++] = (struct pollfd){.fd = a.fd, .events = POLLIN}; }
        if (dc.pid) { di = np; pf[np++] = (struct pollfd){.fd = dc.fd, .events = POLLIN}; }
        poll(pf, (nfds_t)np, 100);

        if (pf[0].revents & POLLIN) {
            for (int burst = 0; burst < 64; burst++) {
                ssize_t k = recv(udp, pkt, sizeof pkt, MSG_DONTWAIT);
                if (k <= 0) break;
                vita_packet(pkt, k, a.stream, &d, capture_t != 0);
            }
        }
        if (ai >= 0 && (pf[ai].revents & (POLLIN | POLLHUP | POLLERR))) {
            if (!api_read(&a, &cfg)) {
                logf_("radio API connection closed");
                api_close(&a);
                capture_t = 0;
                pthread_mutex_lock(&G.mu); G.slice_ok = G.stream_ok = 0; pthread_mutex_unlock(&G.mu);
                next_connect = mono_s() + 5;
            }
        }
        if (di >= 0 && (pf[di].revents & (POLLIN | POLLHUP | POLLERR))) {
            ssize_t k = read(dc.fd, dc.out + dc.n, sizeof dc.out - 1 - dc.n);
            if (k > 0) dc.n += (size_t)k;
            else if (k == 0 || dc.n >= sizeof dc.out - 1) {
                int st;
                struct rusage ru;
                memset(&ru, 0, sizeof ru);
                wait4(dc.pid, &st, 0, &ru);
                unsigned long long b1, t1;
                cpu_jiffies(&b1, &t1);
                dc.sys_busy_pct = t1 > dc.cpu0_total ? 100.0f * (float)(b1 - dc.cpu0_busy) / (float)(t1 - dc.cpu0_total) : -1;
                dc.child_cpu_s = (float)(ru.ru_utime.tv_sec + ru.ru_stime.tv_sec)
                               + (float)(ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) / 1e6f;
                dc.child_rss_kb = ru.ru_maxrss;
                dc.mem_avail_kb = mem_available_kb();
                close(dc.fd);
                dc.out[dc.n] = 0;
                record_cycle(&dc);
                /* wsprd also appends to these on every run; only the hash
                 * table is worth keeping. */
                char p[400];
                const char *junk[] = {"ALL_WSPR.TXT", "wspr_spots.txt", "wspr_timer.out", "fftwf_wisdom.dat"};
                for (size_t i = 0; i < sizeof junk / sizeof junk[0]; i++) {
                    snprintf(p, sizeof p, "%s/%s", g_work, junk[i]);
                    unlink(p);
                }
                char c2name[20];
                time_t tt = (time_t)dc.cycle_t;
                struct tm tm;
                gmtime_r(&tt, &tm);
                strftime(c2name, sizeof c2name, "%y%m%d_%H%M.c2", &tm);
                snprintf(p, sizeof p, "%s/%s", g_work, c2name);
                unlink(p);
                dc.pid = 0;
                g_decoder = 0;
                dc.fd = -1;
            }
        }
    }
}
