/*
 * AetherFT8: an FT8 receiver that runs on a FlexRadio as a legacy waveform.
 * Part of AetherSDR, GPL-3.0-or-later.
 *
 * Built from AetherWSPR (tools/wspr-agent) -- the shim around the decoder is
 * the same: it connects to the radio's API on 127.0.0.1 as a non-GUI client,
 * makes one DIGU slice of its own and takes that slice's DAX audio, so it needs
 * no GUI client slot. What differs is the cycle and the decoder. Every 15 s it
 * records 12.64 s, decimates the audio to 12 kHz and runs ft8_lib's decoder
 * (ft8_lib/, built in, MIT) on it in-process. Spots are kept in memory and
 * served as JSON on TCP 48997:
 *
 *   GET  /v1/status[?since=<unix>]  state, recent cycles and spots
 *   GET  /v1/config                 the settings
 *   POST /v1/config                 new settings (JSON). The first POST is
 *                                   open and returns an admin token; later
 *                                   ones need "Authorization: Bearer <token>"
 *
 * Only LAN callers are answered.
 *
 * TRANSMIT: this agent does not transmit. It sends no TX, PTT or keying
 * command, its slice is set tx=0, and the vendored decoder has its transmit-side
 * encoder removed (see ft8_lib/PROVENANCE.md). Phase 1 is decode only by
 * design, not by configuration -- there is no TX code path here to disable.
 * A transmitting version is a separate proposal with its own Principle VI
 * analysis; see RFC #7114.
 *
 *   aether-ft8                    run
 *   aether-ft8 selftest           synthesize a signal, run it through the
 *                                 capture DSP and the decoder, check the decode
 *   aether-ft8 decodewav FILE     decode one 15 s WAV and print the messages
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

#include <stdbool.h>
#include <stdint.h>
#include <sys/resource.h>

#include "grayline.h"
#include "ft8_lib/ft8/decode.h"
#include "ft8_lib/ft8/message.h"
#include "ft8_lib/ft8/constants.h"
#include "ft8_lib/common/monitor.h"
#include "ft8_lib/common/wave.h"

#define AGENT_VERSION "0.1.2"
#define STATUS_PORT 48997   /* 48994 radio health, 48995 WSPR, 48997 here */
#define AUDIO_RATE 24000
/* ft8_lib works at 12 kHz, which is the WSJT-X convention and what its own
 * reference recordings use. The radio's DAX audio is 24 kHz real, so the whole
 * DSP chain here is one halfband decimation -- no quadrature mix and no
 * complex baseband, unlike WSPR's 24k -> 375 Hz chain. */
#define FT8_RATE 12000
#define DECIM1 2                        /* 24000 -> 12000 */
#define NTAPS 63                        /* one stage, so fewer taps than WSPR */

/* An FT8 cycle is 15 s and carries 12.64 s of signal (79 symbols at 0.16 s).
 * Capture a little past the end of the tones so a late-arriving packet cannot
 * truncate the last symbol, and leave the rest of the cycle to decode in. */
#define CYCLE_SECONDS 15
#define CAPTURE_SECONDS 13
#define CAPTURE_SAMPLES (CAPTURE_SECONDS * FT8_RATE)  /* 156000 */
/* What is left of the cycle once the capture ends. The decode must fit in it
 * or the next cycle starts late; AetherWSPR had ~6 s, this has 2. */
#define DECODE_BUDGET_MS ((CYCLE_SECONDS - CAPTURE_SECONDS) * 1000)
#define MAX_SPOTS 2000
#define MAX_SLICES 8
#define MAX_CYCLE_SPOTS 64    /* ft8_lib decode cap per cycle */
#define MAX_CYCLES 240  /* 240 x 15 s = 1 h of cycle history */
#define MAX_BANDS 16
#define MAX_UPLOADS 2000


/* ------------------------------------------------------------------ util */

static volatile pid_t g_decoder;

/* The radio API socket and the slice/stream this agent owns, so that an
 * exit can hand them back. The radio keeps a slice allocated against the
 * handle of a client that has gone away: two orphans are enough to fill a
 * FLEX-6300, and every later "slice create" then fails 500000C8 "Maximum
 * slice limit reached" -- including the one this agent makes when it comes
 * back up. */
static int g_api_fd = -1;
static int g_api_slice = -1;
static unsigned g_api_stream = 0;

/* Hand the slice and DAX stream back to the radio before this process goes
 * away. A slice outlives the client that made it, so an agent that exits
 * without doing this leaves one allocated against a handle that no longer
 * exists; `sub client all` then reports no clients while `sub slice all`
 * still shows them in use. Two such orphans fill a FLEX-6300 and every
 * later slice create fails 500000C8 "Maximum slice limit reached".
 *
 * Written with plain write() rather than api_send(): this runs from a
 * signal handler as well as from the restart route, so it must not touch
 * a mutex or the logger. A failed write is ignored -- there is nothing
 * useful to do about it on the way out, and the radio drops the slice
 * eventually anyway. */
static void release_radio_resources(void)
{
    if (g_api_fd < 0) return;
    char buf[96];
    if (g_api_stream) {
        int n = snprintf(buf, sizeof buf, "C9998|stream remove 0x%08X\n", g_api_stream);
        if (n > 0) { ssize_t w = write(g_api_fd, buf, (size_t)n); (void)w; }
    }
    if (g_api_slice >= 0) {
        int n = snprintf(buf, sizeof buf, "C9999|slice remove %d\n", g_api_slice);
        if (n > 0) { ssize_t w = write(g_api_fd, buf, (size_t)n); (void)w; }
    }
    g_api_slice = -1;
    g_api_stream = 0;
}

static void on_term(int sig)
{
    (void)sig;
    release_radio_resources();
    if (g_decoder > 0) kill(g_decoder, SIGKILL);
    _exit(0);
}

static void logf_(const char *fmt, ...)
{
    char ts[32];
    time_t now = time(NULL);
    strftime(ts, sizeof ts, "%H:%M:%S", gmtime(&now));
    fprintf(stderr, "aether-ft8 %s ", ts);
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

static int clock_synced(void)
{
    struct timex tx;
    memset(&tx, 0, sizeof tx);
    int state = adjtimex(&tx);
    if (state != TIME_ERROR && !(tx.status & STA_UNSYNC))
        return 1;
    /* A FLEX-6000 runs kernel 2.6.37, where chronyd disciplines the clock but
     * never clears STA_UNSYNC: measured on a 6300 whose selected pool source
     * reported stratum 3 and reach 255 while the kernel still said UNSYNC, and
     * whose wall clock matched UTC to well under a second. The radio's own
     * report that a source is selected is the better evidence on that
     * hardware, so accept it. Radios that do set the kernel flag never reach
     * this line.
     *
     * FT8 needs this MORE than WSPR does: a 15 s cycle leaves far less slack
     * for a clock that is merely close, and without this the agent on a 6000
     * sits in waiting_clock forever. */
    return g_ntp_selected;
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

/* The standard FT8 dial frequencies, as WSJT-X ships them. These are NOT the
 * WSPR dials AetherWSPR uses -- 20 m FT8 is 14.074 where 20 m WSPR is 14.0956
 * -- so a band name means a different frequency in the two agents. */
static const band_t BANDS[] = {
    {"2200m", 136000},    {"630m", 474200},     {"160m", 1840000},
    {"80m", 3573000},     {"60m", 5357000},     {"40m", 7074000},
    {"30m", 10136000},    {"20m", 14074000},    {"17m", 18100000},
    {"15m", 21074000},    {"12m", 24915000},    {"10m", 28074000},
    {"6m", 50313000},     {"2m", 144174000},
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
/* ⛔ "RR73", "RRR" and "73" are sign-offs, never a grid to report -- and the
 * field type alone cannot tell them apart from a locator.
 *
 * MEASURED on the air 2026-11-09, and this is the part my earlier fix missed:
 * there are TWO encodings that both render as the text "RR73".
 *
 *   igrid4 = 32403 (MAXGRID4+3)  the sign-off token  -> FTX_FIELD_TOKEN
 *   igrid4 = 32373               the grid square RR73 -> FTX_FIELD_GRID
 *
 * 32373 is simply where R,R,7,3 lands in the locator encoding
 * (((17*18 + 17)*10 + 7)*10 + 3), and it is <= MAXGRID4, so unpackgrid()
 * takes its grid branch and types it GRID. A guard that trusts the type
 * therefore passes it, which is why 9 of 81 gridded spots on 40 m carried
 * grid "RR73" while plain "73" -- not structurally a locator, so only ever a
 * token -- was always handled correctly.
 *
 * ft8_lib's own packgrid() special-cases "RR73" BEFORE its locator branch, so
 * an encode/decode round trip can never reproduce this; only a real
 * transmitter whose encoder does not special-case it will. Checking the text
 * here mirrors the library's own precedence.
 *
 * Grid RR73 is at 83.5 N, 175 E -- Arctic Ocean, ~700 km from the pole, no
 * land and no stations, so nothing real is lost by preferring the sign-off
 * reading. valid_grid() itself must keep accepting it, because an operator
 * may legitimately put any square in the config. */
/* Is a grid-typed field really a locator, or a sign-off?
 *
 * Grid RR73 is at 83.5 N, 175 E -- Arctic Ocean, ~700 km from the pole, no
 * land. A station genuinely there is an expedition with a callsign to
 * match. Measured on the 15 distinct callsigns that leaked on 40 m: N1ECC,
 * W2CG, KP4COD (Puerto Rico, 18 N), EA8UP (Canary Islands, 28 N), VE3RUB,
 * AE1T, K1SM ... every one an ordinary temperate callsign, so for every one
 * of them "RR73" was the sign-off, not a locator.
 *
 * So ask whether the station COULD be up there instead of blacklisting the
 * string, which would silently discard a genuine polar spot.
 *                                        -- the discriminator is Nigel's
 */
static int polar_grid(const char *g)   /* checked by the selftest */
{
    /* Field letter 2 is latitude: 'R' is the top 10-degree band, 'Q' the
     * one below, i.e. 80 N and above. */
    const char c = (char)toupper((unsigned char)g[1]);
    return c == 'R' || c == 'Q';
}

static int polar_capable_call(const char *call)
{
    /* Who really operates above 80 N: Russian Arctic (RI..), Greenland
     * (OX), Svalbard (JW), Jan Mayen (JX), Antarctic (R1AN), plus maritime-
     * and aeronautical-mobile, which can be anywhere. Deliberately short --
     * this only has to cover who could be at 80 N+, not the DXCC list, and
     * it costs no table on a 1-core radio. */
    static const char *POLAR_PREFIX[] = { "RI", "OX", "JW", "JX", "R1AN" };
    for (size_t i = 0; i < sizeof POLAR_PREFIX / sizeof POLAR_PREFIX[0]; i++)
        if (!strncmp(call, POLAR_PREFIX[i], strlen(POLAR_PREFIX[i]))) return 1;
    const size_t n = strlen(call);
    if (n > 3 && (!strcmp(call + n - 3, "/MM") || !strcmp(call + n - 3, "/AM")))
        return 1;
    return 0;
}

/* The sign-off texts. Only "RR73" can also be a locator (igrid4 32373 vs
 * the token 32403); "RRR" and "73" are not structurally locators and so
 * only ever arrive typed TOKEN, where the type check already stops them.
 * They are listed to state the rule, not because they do work here. */
static int signoff_text(const char *s)
{
    return !strcmp(s, "RR73") || !strcmp(s, "RRR") || !strcmp(s, "73");
}

/* Should `extra`, which the library typed as a GRID, be reported as one? */
static int grid_is_real(const char *extra, const char *call)
{
    if (!signoff_text(extra)) return 1;          /* an ordinary locator */
    /* A sign-off text in a grid-typed field. Only "RR73" is also a well-formed
     * locator (igrid4 32373); "RRR" and "73" are not, so they are never a
     * grid whoever sends them -- and polar_grid() must not be consulted about
     * them, since it only reads the latitude letter and "RRR" would look
     * polar. Believe RR73 as a locator only if the station could be up
     * there. */
    if (strcmp(extra, "RR73")) return 0;   /* "RRR" and "73" are never grids */
    /* Only "RR73" reaches here, and RR73 IS a far-north square (83.5 N), so
     * the latitude is already settled -- the open question is only whether
     * THIS station could be there. polar_grid() stays because the selftest
     * pins the latitude rule itself. */
    return polar_capable_call(call);
}

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
    bprintf(b, ",\"dax_channel\":%d}", c->dax_channel);
}

/* ----------------------------------------------------------------- state */

typedef struct {
    long long t;           /* cycle start, unix seconds */
    int snr;               /* ft8_lib candidate score, not a dB figure: see
                            * the note in status_json where it is served */
    float dt;
    long long hz;
    char call[16], grid[8], band[8];
    char msg[32];          /* the decoded message, e.g. "CQ DX K1ABC FN42".
                            * WSPR has no equivalent -- every WSPR transmission
                            * has the same three-field form, where an FT8
                            * message is one side of an exchange. */
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
    int dax_bound;              /* DAX channel actually bound, 0 until it is */
    long long vita_packets;     /* VITA packets seen: proves samples ARRIVE */
    spot_t spots[MAX_SPOTS];
    int nspots, spot_head;
    cycle_t cycles[MAX_CYCLES];
    int ncycles, cycle_head;
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
    snprintf(dirs[0], sizeof dirs[0], "%s/aether-ft8-state", self_dir);
    snprintf(dirs[1], sizeof dirs[1], "/tmp/aether-ft8-state-%lld", install);
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

static float g_h1[NTAPS];

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

/* Audio (real, 24 kHz) -> anti-alias FIR -> decimate by 2 -> real 12 kHz,
 * which is what ft8_lib's monitor wants.
 *
 * This is the whole DSP difference from AetherWSPR. WSPR mixes its 200 Hz
 * window down to a complex 375 Hz baseband because wsprd reads a .c2 file in
 * that form; ft8_lib instead takes real audio and does its own FFT over the
 * 200-3000 Hz passband, so mixing or decimating further would throw away the
 * very signals it is looking for. */
typedef struct {
    float r1[NTAPS];
    int i1, k1;
    float *audio;    /* real samples at FT8_RATE */
    int n;           /* samples captured */
    long long in;    /* audio samples taken */
    double sumsq;    /* audio power, for the cycle's level */
    int lost;        /* packets missing from the stream, filled with silence */
    int last_count;  /* VITA packet count of the previous packet, -1 none */
    double last_at;  /* when that packet arrived */
} dsp_t;

static void dsp_reset(dsp_t *d)
{
    memset(d->r1, 0, sizeof d->r1);
    d->i1 = d->k1 = 0;
    d->n = 0;
    d->in = 0;
    d->sumsq = 0;
    d->lost = 0;
    d->last_count = -1;
    d->last_at = 0;
}

static float fir(const float *ring, int head, const float *h)
{
    float acc = 0;
    int k = head;
    for (int i = NTAPS - 1; i >= 0; i--) {
        acc += ring[k] * h[i];
        if (++k == NTAPS) k = 0;
    }
    return acc;
}

static void dsp_push(dsp_t *d, float x)
{
    if (d->n >= CAPTURE_SAMPLES) return;
    d->in++;
    d->sumsq += (double)x * x;
    d->r1[d->i1] = x;
    if (++d->i1 == NTAPS) d->i1 = 0;
    if (++d->k1 < DECIM1) return;
    d->k1 = 0;
    d->audio[d->n++] = fir(d->r1, d->i1, g_h1);
}


/* ---------------------------------------------------------------- decode */

/* One cycle's worth of work in flight.
 *
 * AetherWSPR forks a child to run wsprd and reads its stdout, because wsprd is
 * a program with a main(). ft8_lib is a library, so this decodes in-process and
 * there is no pid, no pipe and no output parsing -- decoded messages come back
 * as structs. The CPU and memory fields are kept because they are the evidence
 * that the decode fits the cycle, which matters more here than it did for WSPR:
 * the budget is about 2.4 s, not 6 s.
 */
typedef struct {
    int busy;                /* a decode is in flight */
    long long cycle_t, dial_hz;
    char band[8];
    double started;
    int audio_pct, lost;
    float dbfs;
    int nspots;
    spot_t spots[MAX_CYCLE_SPOTS];
    /* Load while it ran: the radio's CPU (all cores) and our own. */
    unsigned long long cpu0_busy, cpu0_total;
    float sys_busy_pct, cpu_s;
    long rss_kb, mem_avail_kb;
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
    if (!f) return 0;
    char k[64];
    long v = 0, out = 0;
    while (fscanf(f, "%63s %ld kB\n", k, &v) == 2)
        if (!strcmp(k, "MemAvailable:")) { out = v; break; }
    fclose(f);
    return out;
}

static char g_self[512];
/* The status listener, so POST /v1/restart can close it before execv().
 * It is not CLOEXEC, and an inherited listening socket would stop the new
 * process binding its own status port. -1 until the listener is up. */
static int g_status_fd = -1;
#ifdef AETHER_WITH_OSD
/* Defined further down, beside the depth setting; declared here because
 * status_json() above reports them. */
extern long long aether_osd_attempted;
extern long long aether_osd_rescued;
extern long long aether_osd_skipped;
#endif
static char g_work[300];

/* This process's own CPU seconds and peak RSS. AetherWSPR read these from its
 * child through wait4(); decoding in-process, we ask about ourselves. */
static void own_usage(float *cpu_s, long *rss_kb)
{
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) == 0) {
        *cpu_s = (float)(ru.ru_utime.tv_sec + ru.ru_stime.tv_sec)
               + (float)(ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) / 1e6f;
        *rss_kb = ru.ru_maxrss;
    } else {
        *cpu_s = 0;
        *rss_kb = 0;
    }
}

static const char *band_of(long long hz)
{
    for (int i = 0; i < (int)(sizeof BANDS / sizeof BANDS[0]); i++)
        if (llabs(hz - BANDS[i].dial_hz) < 20000) return BANDS[i].name;
    return "";
}

/* ft8_lib decode parameters, which are the library's own demo values. They
 * trade decodes against time, and the measured 73% decode rate quoted in the
 * RFC was taken with exactly these -- changing one invalidates that figure. */
#define FT8_MAX_CANDIDATES 140
/* How many of those slots to actually use. ftx_find_candidates keeps the
 * top-N by sync score in a min-heap, so a smaller budget keeps the BEST
 * candidates rather than truncating a list -- which is what makes this a
 * principled cut. Overridable (AETHER_FT8_CAND) so the budget can be swept
 * without a rebuild; see the #7114 measurement. */
/* 100, not the array's 140. Swept over the 22-WAV reference corpus at OSD
 * depth 2, decode rate against total decode time:
 *
 *   140  77.3%  1.3 s      90  77.1%  0.9 s
 *   120  77.3%  1.2 s      85  76.8%  0.8 s
 *   110  77.3%  1.1 s      80  76.2%  0.8 s
 *   100  77.3%  1.0 s      60  75.4%  0.6 s
 *    95  77.1%  0.9 s      40  71.4%  0.5 s
 *
 * 100 is the knee: the full decode rate for 23% less time. Every step
 * below it costs decodes. This is a real cut rather than a truncation --
 * ftx_find_candidates keeps the top-N by sync score in a min-heap, so a
 * smaller budget drops the WORST candidates, which are the ones belief
 * propagation and then OSD were spending their time failing on. */
static int g_max_candidates = 100;
#define FT8_LDPC_ITERS      25
#define FT8_MIN_SCORE       10
#define FT8_FREQ_OSR         2
#define FT8_TIME_OSR         2

/* ft8_lib needs a callsign hash table for type-2 and type-3 messages, which
 * carry a 10, 12 or 22-bit hash in place of a callsign. The library calls us to
 * store each callsign it sees in full and to look one up later. A message whose
 * hash we cannot resolve has no callsign, so it yields no spot -- the same
 * treatment AetherWSPR gives a hashed WSPR callsign.
 */
#define HASH_SLOTS 256
static struct { uint32_t h10, h12, h22; char call[12]; } g_hash[HASH_SLOTS];
static int g_hash_n;

/* The library hands us a callsign and its 22-bit hash; the 12 and 10-bit
 * codes are the top bits of it, which is how WSJT-X defines them. */
static void hash_save(const char *call, uint32_t n22)
{
    for (int i = 0; i < g_hash_n; i++)
        if (!strcmp(g_hash[i].call, call)) return;
    if (g_hash_n >= HASH_SLOTS) g_hash_n = 0;   /* ring: the oldest is replaced */
    int i = g_hash_n++;
    snprintf(g_hash[i].call, sizeof g_hash[i].call, "%s", call);
    g_hash[i].h22 = n22;
    g_hash[i].h12 = n22 >> 10;
    g_hash[i].h10 = n22 >> 12;
}

static bool hash_lookup(ftx_callsign_hash_type_t type, uint32_t hash, char *call)
{
    for (int i = 0; i < g_hash_n; i++) {
        uint32_t want = type == FTX_CALLSIGN_HASH_10_BITS ? g_hash[i].h10
                      : type == FTX_CALLSIGN_HASH_12_BITS ? g_hash[i].h12
                      : g_hash[i].h22;
        if (want == hash) { strcpy(call, g_hash[i].call); return true; }
    }
    call[0] = 0;
    return false;
}

static ftx_callsign_hash_interface_t g_hash_if = { hash_lookup, hash_save };

/* Turn one decoded FT8 message into a spot, or reject it.
 *
 * FT8 messages are not spots in the WSPR sense. A WSPR transmission is a
 * beacon: callsign, grid and power, always in that form. Most FT8 messages are
 * one side of an exchange -- "CQ DX K1ABC FN42", "K1ABC W9XYZ -11",
 * "W9XYZ K1ABC RR73" -- so what a skimmer wants is the TRANSMITTING station and
 * its grid when the message happens to carry one.
 *
 * Fields are "TO FROM EXTRA", space separated, and the sender is the second.
 * Free-text and telemetry messages have no sender and are rejected.
 *
 * Returns 1 and fills *s when a callsign was recovered, 0 otherwise.
 */
static int spot_from_message(const ftx_message_t *msg, long long t, long long dial_hz,
                             float freq_hz, float dt, int snr, spot_t *s)
{
    /* ftx_message_decode_std hands back the three fields already separated and
     * tells us what each one IS, which beats splitting the rendered text on
     * spaces: a report like "R-09" and a grid like "FN42" are both four
     * characters, and only the field type distinguishes them reliably. */
    char call_to[14] = "", call_de[14] = "", extra[20] = "";
    ftx_field_t types[FTX_MAX_MESSAGE_FIELDS];
    memset(types, 0, sizeof types);

    /* Dispatch on the MESSAGE TYPE, not on a return code.
     *
     * A compound or portable callsign (PA0SKT/2, PJ4/KA1ABC) does not fit a
     * standard message, so FT8 sends it as type 4, "nonstandard calls", with
     * the other station reduced to a hash. The trap is that
     * ftx_message_decode_std() on a type-4 message returns FTX_MESSAGE_RC_OK
     * and fills the fields with GARBAGE -- measured on this library at commit
     * 9fec6ca, "CQ PA0SKT/2 JO33" came back as de="C19PFD" extra="PL77".
     * Trusting its return code would publish fictitious callsigns for every
     * portable operator on the band. */
    const ftx_message_type_t mt = ftx_message_get_type(msg);
    ftx_message_rc_t rc;
    if (mt == FTX_MESSAGE_TYPE_NONSTD_CALL)
        rc = ftx_message_decode_nonstd(msg, &g_hash_if, call_to, call_de, extra, types);
    else if (mt == FTX_MESSAGE_TYPE_STANDARD)
        rc = ftx_message_decode_std(msg, &g_hash_if, call_to, call_de, extra, types);
    else
        return 0;      /* telemetry, free text, contest exchanges: no plain spot */
    if (rc != FTX_MESSAGE_RC_OK) return 0;

    /* ⛔ A hashed callsign the table could not resolve comes back bracketed,
     * e.g. "<3E40CDW>". That is the library saying "I don't know who this is",
     * not a callsign, and logging it as one puts a station that does not exist
     * into the spot list -- measured on the air on 20 m, where 2 of 7 apparent
     * stations in one run were the same bracketed hash.
     *
     * This guard was lost when the decoder moved to type-based dispatch; it
     * belongs AFTER the dispatch so it covers the non-standard path too, which
     * is precisely where hashed callsigns arrive. */
    if (!call_de[0] || strchr(call_de, '<') || strchr(call_de, '>')) return 0;

    memset(s, 0, sizeof *s);
    s->t = t;
    s->snr = snr;
    s->dt = dt;
    s->hz = dial_hz + (long long)(freq_hz + 0.5f);
    snprintf(s->call, sizeof s->call, "%s", call_de);
    snprintf(s->band, sizeof s->band, "%s", band_of(dial_hz));

    /* Keep the message as sent, for a consumer that wants to show it. */
    if (call_to[0] && extra[0])
        snprintf(s->msg, sizeof s->msg, "%s %s %s", call_to, call_de, extra);
    else if (call_to[0])
        snprintf(s->msg, sizeof s->msg, "%s %s", call_to, call_de);
    else
        snprintf(s->msg, sizeof s->msg, "%s", call_de);

    /* The grid comes from the THIRD field, so it is types[2] that must say
     * FTX_FIELD_GRID -- not "any of the three fields is a grid".
     *
     * Scanning all three let a sign-off through. Measured on the air: the
     * first live decode on 20 m was "W0SJE 9Z4BM RR73" and was logged with
     * grid "RR73". RR73 is a sign-off, not a grid, and it passes a structural
     * two-letters-two-digits check, which is why valid_grid() alone cannot
     * catch it. The library types it correctly (2 = report, where a real grid
     * such as FN42 is 5); the bug was reading the wrong index.
     *
     * A type-4 non-standard message has no grid field at all, and
     * ftx_message_decode_nonstd does not fill types[] the way decode_std
     * does, so the explicit index also stops a stale value being trusted. */
    if (types[2] == FTX_FIELD_GRID && extra[0] && valid_grid(extra)
        && grid_is_real(extra, call_de))
        snprintf(s->grid, sizeof s->grid, "%s", extra);
    return 1;
}

/* Decode one captured cycle, in-process and synchronously.
 *
 * On a single-core radio there is nothing to gain by threading this, and the
 * measurement that matters is how long this one call takes against the cycle.
 */
static void run_decode(decode_t *dc, const float *audio, int nsamples)
{
    monitor_config_t mcfg = {
        .f_min = 200,
        .f_max = 3000,
        .sample_rate = FT8_RATE,
        .time_osr = FT8_TIME_OSR,
        .freq_osr = FT8_FREQ_OSR,
        .protocol = FTX_PROTOCOL_FT8
    };
    monitor_t mon;
    monitor_init(&mon, &mcfg);
    for (int frame = 0; frame + mon.block_size <= nsamples; frame += mon.block_size)
        monitor_process(&mon, audio + frame);

    ftx_candidate_t cand[FT8_MAX_CANDIDATES];
    int ncand = ftx_find_candidates(&mon.wf, g_max_candidates, cand, FT8_MIN_SCORE);

    dc->nspots = 0;
    ftx_message_t seen[MAX_CYCLE_SPOTS];
    int nseen = 0;

    for (int i = 0; i < ncand && dc->nspots < MAX_CYCLE_SPOTS; i++) {
        ftx_message_t msg;
        ftx_decode_status_t st;
        if (!ftx_decode_candidate(&mon.wf, &cand[i], FT8_LDPC_ITERS, &msg, &st)) continue;

        int dup = 0;
        for (int j = 0; j < nseen; j++)
            if (!memcmp(seen[j].payload, msg.payload, sizeof msg.payload)) { dup = 1; break; }
        if (dup) continue;
        if (nseen < MAX_CYCLE_SPOTS) seen[nseen++] = msg;

        /* Candidate offsets are in oversampled bins; the monitor's symbol
         * period converts them to Hz and seconds. */
        float freq_hz = (cand[i].freq_offset + (float)cand[i].freq_sub / FT8_FREQ_OSR)
                        / mon.symbol_period;
        float dt = (cand[i].time_offset + (float)cand[i].time_sub / FT8_TIME_OSR)
                   * mon.symbol_period;

        spot_t s;
        if (spot_from_message(&msg, dc->cycle_t, dc->dial_hz, freq_hz, dt,
                              (int)cand[i].score, &s))
            dc->spots[dc->nspots++] = s;
    }
    monitor_free(&mon);
}


/* ------------------------------------------------------------- no upload */

/* AetherWSPR uploads spots to wsprnet.org from the radio. There is no
 * equivalent here: PSK Reporter uses a different protocol, and an outbound
 * connection from the radio is its own new public surface under #7112, so it
 * is out of scope for phase 1. The uploader thread, its queue and the
 * `upload` setting are therefore absent rather than disabled. Spots leave
 * this agent only by a LAN caller reading /v1/status.
 */

/* --------------------------------------------------------- status server */

static void status_json(buf_t *b, long long since)
{
    char call[16], grid[8];
    effective_call_grid(call, grid);
    bprintf(b, "{\"agent\":\"AetherFT8\",\"version\":\"%s\",\"state\":", AGENT_VERSION);
    bjson_str(b, G.state);
    /* clock_source says WHY: the kernel's own flag, the radio's selected NTP
     * source where the kernel never clears STA_UNSYNC, or none at all. A UI
     * that only has a bool cannot tell an operator which of those applies. */
    {
        struct timex ctx;
        memset(&ctx, 0, sizeof ctx);
        int cst = adjtimex(&ctx);
        const char *src = (cst != TIME_ERROR && !(ctx.status & STA_UNSYNC)) ? "kernel"
                        : g_ntp_selected ? "radio_ntp" : "none";
        bputs(b, ",\"clock_source\":");
        bjson_str(b, src);
    }
    bprintf(b, ",\"clock_synced\":%s,\"uptime_s\":%lld,\"callsign\":", clock_synced() ? "true" : "false",
            (long long)(mono_s() - G.started));
    bjson_str(b, call);
    bputs(b, ",\"grid\":"); bjson_str(b, grid);
    bputs(b, ",\"band\":"); bjson_str(b, G.band);
    bprintf(b, ",\"dial_hz\":%lld,\"slice\":%s,\"audio\":%s", G.dial_hz, G.slice_ok ? "true" : "false",
            G.stream_ok ? "true" : "false");
    /* Which state directory won. pick_state_dir() prefers the one beside the
     * binary and falls back to a RAM path named after the binary's ctime, and
     * the two behave differently across a reinstall -- the fallback starts
     * over. Without this field there is no way to tell from outside which one
     * is in use, which made a settings-not-applying problem much harder to
     * diagnose than it needed to be. */
    bputs(b, ",\"state_path\":");
    bjson_str(b, G.state_path[0] ? G.state_path : "");
    /* The DAX channel actually BOUND, which is read once at startup. The
     * configured value can differ after a config change until a restart,
     * and that difference is exactly the trap: on a FLEX-6300 the default
     * channel 4 yields silence while audio still reads true. */
    bprintf(b, ",\"dax_channel_bound\":%d,\"vita_packets\":%lld", G.dax_bound, G.vita_packets);
#ifdef AETHER_WITH_OSD
    /* How often OSD ran and how often it recovered a frame. The ratio is
     * what decides whether it earns its cost: belief propagation also
     * fails on every noise candidate, and OSD pays in full for each of
     * those before returning nothing. */
    bprintf(b, ",\"osd_asked\":%lld,\"osd_rescued\":%lld,\"osd_gated\":%lld",
            aether_osd_attempted, aether_osd_rescued, aether_osd_skipped);
#endif
    bputs(b, ",\"hop_period\":");
    bjson_str(b, G.hop_period >= 0 ? PERIOD_NAME[G.hop_period] : "");
    bputs(b, ",\"depth\":");
    bjson_str(b, DEPTH_NAME[G.cfg.depth]);
    bputs(b, ",\"cycles\":[");
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
        /* "score" rather than "snr": ft8_lib reports a candidate score, which
         * is a correlation strength, NOT a dB signal-to-noise ratio. Calling
         * it snr would invite a consumer to plot it beside a WSPR spot's real
         * dB figure, and they are not the same quantity. "mode" lets a
         * consumer tell this feed from the WSPR one. */
        bprintf(b, ",\"score\":%d,\"dt\":%.1f,\"hz\":%lld,\"mode\":\"FT8\",\"call\":",
                s->snr, s->dt, s->hz);
        bjson_str(b, s->call);
        bputs(b, ",\"grid\":"); bjson_str(b, s->grid);
        bputs(b, ",\"msg\":"); bjson_str(b, s->msg);
        bputs(b, "}");
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
    } else if (!strcmp(method, "POST") && !strcmp(path, "/v1/restart")) {
        /* Re-exec this program in place.
         *
         * Some settings are read once at startup and never again -- the DAX
         * channel is the one that bites, because the agent's default (4)
         * does not work on a FLEX-6300, which needs 1. Changing it through
         * POST /v1/config stored it but changed nothing: the capture was
         * already bound to the old channel, and the only way to re-bind was
         * reinstalling the waveform, which is also the one thing that makes
         * the agent forget its settings when it is running from the RAM
         * fallback state directory (that path is named after the binary's
         * ctime, so a reinstall starts over by design). Set the channel and
         * nothing happened; reinstall to apply it and the setting was gone.
         *
         * execv() keeps the binary, so the state directory resolves the same
         * way on the other side and the settings survive. The listening
         * socket is closed first: it is not CLOEXEC, and leaving it open
         * would make the new process fail to bind its own status port.
         *
         * Receive-only agent: nothing is keyed, so there is no transmit to
         * leave hanging. */
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
        save_config_locked();
        pthread_mutex_unlock(&G.mu);

        const char *msg = "{\"restarting\":true}";
        http_reply(fd, 200, msg, strlen(msg));
        /* Let the reply reach the client before the process goes away. */
        shutdown(fd, SHUT_WR);
        close(fd);
        sleep(1);
        /* Give the slice and DAX stream back first. execv() keeps the
         * process id but the radio sees the API connection drop and holds
         * the slice against the old client handle: restarting without this
         * leaks a slice every time, and two leaks fill a FLEX-6300 so the
         * agent that comes back cannot create one at all. Measured doing
         * exactly that on 2026-11-14 before this call existed. */
        release_radio_resources();
        /* Let the removes reach the radio before the socket closes. */
        usleep(500000);
        if (g_api_fd >= 0) close(g_api_fd);
        if (g_status_fd >= 0) close(g_status_fd);
        char *argv[] = { g_self, NULL };
        execv(g_self, argv);
        /* Only reached if execv failed; the caller already has its reply. */
        logf_("restart: execv(%s) failed: %s", g_self, strerror(errno));
        _exit(1);
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
            int bl = snprintf(body, sizeof body, "{\"port\":%d,\"name\":\"AetherFT8\"}", STATUS_PORT);
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
    int want_dax;         /* the "slice set <n> dax=<ch>" whose answer picks the channel */
    int slice;            /* our slice index, -1 none */
    unsigned stream;      /* our DAX stream id, 0 none */
    double stream_retry;  /* mono_s() to ask for a refused stream again, 0 none */
    /* Where the radio says each slice is tuned, by index. All of them: a
     * slice's status can arrive before the reply that names it ours. */
    long long rf_hz[MAX_SLICES];
    long long create_hz;  /* what we asked the next slice to be created at */
    unsigned handle;
    int lost;             /* our slice was removed: remake it now */
    /* The DAX channel we are actually trying, which is not always the
     * configured one: see dax_fallback(). 0 = use the config. */
    int dax_try;
} api_t;

/* The next DAX channel to try after the radio refused one.
 *
 * ⛔ A FLEX-6300 has only channels 1 and 2; 3 and 4 return 50000016
 * (measured). The agent's own default is 4, so a fresh install on a 6300
 * never gets audio -- and the old behaviour was to re-ask for the SAME
 * refused channel every 10 s, forever. Nigel put it plainly: a blank
 * install should just work, with nothing clever needed and no host
 * involved.
 *
 * Walking DOWN rather than up is deliberate: channel 1 exists on every
 * 6000, 7000 and 8000, so the walk always terminates somewhere real. The
 * configured channel is still tried FIRST, so an operator who picked 2 on
 * purpose keeps it.
 *
 * (rxant needs no equivalent: blank already means "the radio's default",
 * which is why a blank install lands on the right antenna already.) */
static int dax_fallback(int tried)
{
    return tried > 1 ? tried - 1 : 0;   /* 0: nothing left to try */
}

static int api_send(api_t *a, const char *fmt, ...);

/* Put the slice on a DAX channel and wait for the radio's answer; the stream
 * is created only once it says yes (api_line, want_dax).
 *
 * ⛔ This answer is the one that matters. A FLEX-6300 REFUSES
 * "slice set <n> dax=4" with 50000016 -- and then ACCEPTS
 * "stream create type=dax_rx dax_channel=4", handing back a stream that
 * carries nothing because no slice feeds it. The agent used to send both at
 * once and read only the second reply, so on a 6300 it sat bound to channel 4
 * with "audio": true and vita_packets 0 for days. The refusal it needed was
 * in the reply it threw away. */
static void dax_ask(api_t *a, int channel)
{
    a->dax_try = channel;
    a->want_stream = 0;
    a->want_dax = api_send(a, "slice set %d dax=%d", a->slice, channel);
}

/* The radio refused channel dax_try, at either step: try the next one down,
 * or, with none left, wait and start again from the configured channel. */
static void dax_refused(api_t *a, const char *what, unsigned long code, const char *msg)
{
    const int next = dax_fallback(a->dax_try);
    if (next) {
        logf_("DAX channel %d refused at %s (%08lx); trying %d", a->dax_try, what, code, next);
        dax_ask(a, next);
        return;
    }
    logf_("DAX audio refused on every channel: %08lx %s", code, msg);
    a->dax_try = 0;        /* start again from the config */
    a->stream_retry = mono_s() + 10;
}

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
    api_send(a, "client program AetherFT8");
    api_send(a, "client udpport %d", udp_port);
    a->want_info = api_send(a, "info");
    api_send(a, "sub gps all");
    api_send(a, "sub slice all");
    /* 4.3.16 and later; older firmware answers with an error and we simply
     * never see an ntp_sources status, leaving the kernel flag as the only
     * evidence, exactly as before. */
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
            g_api_fd = a->fd;        /* so an exit can release these */
            g_api_slice = a->slice;
            api_send(a, "slice set %d tx=0", a->slice);
            /* A new slice always starts from the CONFIGURED channel. It used
             * to reuse whatever dax_try was left from the last slice, so a
             * channel saved from the Configure window was ignored: the slice
             * was remade and asked for the old channel again. */
            dax_ask(a, cfg->dax_channel);
            if (a->slice < MAX_SLICES && !a->rf_hz[a->slice]) a->rf_hz[a->slice] = a->create_hz;
            pthread_mutex_lock(&G.mu); G.slice_ok = 1; pthread_mutex_unlock(&G.mu);
            logf_("slice %d on %s", a->slice, G.band);
        } else if (seq == a->want_dax) {
            a->want_dax = 0;
            /* Step down to a channel this radio has, rather than asking for
             * the refused one again every 10 s. */
            if (code) { dax_refused(a, "the slice", code, msg); return; }
            a->want_stream = api_send(a, "stream create type=dax_rx dax_channel=%d", a->dax_try);
        } else if (seq == a->want_stream) {
            a->want_stream = 0;
            if (code) { dax_refused(a, "the stream", code, msg); return; }
            a->stream_retry = 0;
            a->stream = (unsigned)strtoul(msg, NULL, 16);
            pthread_mutex_lock(&G.mu);
            G.stream_ok = 1;
            /* Record the channel actually bound, which may not be the one
             * configured: the block above steps down when the radio refuses
             * a channel. A stream that OPENS is not a stream that carries
             * samples -- on a FLEX-6300 the default channel 4 can open and
             * stay silent -- so report the bound channel and the VITA packet
             * count separately from "audio": true. */
            G.dax_bound = a->dax_try;
            pthread_mutex_unlock(&G.mu);
            g_api_stream = a->stream;
            logf_("DAX audio stream 0x%08x on channel %d", a->stream, a->dax_try);
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
         * kernel never clears STA_UNSYNC. */
        if (strstr(body, " state=selected"))
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
            a->want_dax = 0;       /* an answer about the old slice picks nothing */
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
    /* Counted before the capturing gate: the question this answers is
     * "is the radio sending us anything on this stream at all", which is
     * separate from whether we are in a capture window. */
    pthread_mutex_lock(&G.mu);
    G.vita_packets++;
    pthread_mutex_unlock(&G.mu);
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
        "\"gray_minutes\":120,\"depth\":\"deep\",\"rxant\":\"XVTA\",\"dax_channel\":3}";
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
        || strcmp(G.cfg.rxant, want.rxant) || G.cfg.dax_channel != want.dax_channel
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

static const char *band_for_cycle(const config_t *c, const char *grid, long long cycle_t, const char *current, int *period_out);
static const char *const COORDINATED[10];

/* The transmitting station and its grid are taken from a decoded message.
 *
 * This is the FT8 equivalent of AetherWSPR's spot-parsing test, and it is the
 * test that matters most here: a WSPR transmission always has the same three
 * fields, where an FT8 message is one side of an exchange, so picking the
 * SENDER (not the station being called) is logic with no WSPR counterpart.
 *
 * The messages are built with the library's own encoder-free path: we pack a
 * standard message from its fields, then decode it back, which exercises
 * exactly the call the live decoder makes. */
/* A blank install must reach audio on any radio.
 *
 * The agent's default DAX channel is 4; a FLEX-6300 has only 1 and 2 and
 * returns 50000016 for the rest. Before dax_fallback() the refusal path
 * re-asked for the SAME channel every 10 s, so a fresh install stayed deaf
 * forever -- measured on the 6300 on 2026-11-09.
 *
 * The walk must terminate at a channel every 6000/7000/8000 has, and must
 * not loop or skip past 1. */
static int selftest_dax_fallback(void)
{
    int fail = 0;
    /* The step-down itself, including the end of the walk. */
    const struct { int tried, want; const char *what; } step[] = {
        {4, 3, "the default 4 steps to 3"},
        {3, 2, "3 steps to 2"},
        {2, 1, "2 steps to 1"},
        {1, 0, "1 is the end of the walk"},
        {8, 7, "the highest the config allows still steps down"},
    };
    for (size_t i = 0; i < sizeof step / sizeof step[0]; i++) {
        const int got = dax_fallback(step[i].tried);
        if (got != step[i].want) {
            printf("selftest FAIL: dax_fallback(%d) gave %d, want %d (%s)\n",
                   step[i].tried, got, step[i].want, step[i].what);
            fail = 1;
        }
    }
    /* The whole walk from the default must reach 1 and stop, visiting each
     * channel once. A loop here would hang a fresh install just as surely as
     * the old repeat did. */
    {
        int ch = 4, seen[9] = {0}, steps = 0;
        while (ch) {
            if (ch < 1 || ch > 8) { printf("selftest FAIL: walk left range at %d\n", ch); fail = 1; break; }
            if (seen[ch]) { printf("selftest FAIL: walk revisited %d\n", ch); fail = 1; break; }
            seen[ch] = 1;
            ch = dax_fallback(ch);
            if (++steps > 8) { printf("selftest FAIL: walk did not terminate\n"); fail = 1; break; }
        }
        if (!fail && !(seen[1] && seen[2])) {
            printf("selftest FAIL: the walk from 4 never tried 1 and 2\n");
            fail = 1;
        }
    }
    return fail;
}

/* Both encodings of "RR73" reach spot_from_message, and neither may become a
 * grid.
 *
 * This is the assertion the earlier fix lacked. The recording test
 * (websdr_test6.wav) cannot cover it: its 20 spots contain no sign-off at
 * all, and it SKIPS entirely unless the binary runs from tools/ft8-agent. A
 * round trip through ftx_message_encode cannot cover it either, because
 * packgrid() special-cases "RR73" and only ever emits the token form.
 *
 * So build a valid message and patch the 15-bit trailing field directly, to
 * put each encoding on the wire exactly as a transmitter would. */
static int selftest_rr73_both_encodings(void)
{
    /* unpackgrid()/packgrid() boundary: values above it are tokens. */
    const uint32_t MAXGRID4 = 32400u;
    struct { const char *what; uint32_t g; int want_grid; } cases[] = {
        /* the locator encoding of R,R,7,3 -- typed GRID by the library, and
         * the one that leaked on the air */
        {"RR73 as a locator (igrid4=32373)", 32373u, 0},
        {"RR73 as a token",   MAXGRID4 + 3u, 0},
        {"RRR as a token",    MAXGRID4 + 2u, 0},
        {"73 as a token",     MAXGRID4 + 4u, 0},
        /* a control: an ordinary square must still come through */
        {"an ordinary locator", 12345u, 1},
    };

    ftx_message_t base;
    ftx_message_init(&base);
    if (ftx_message_encode(&base, &g_hash_if, "KB9RCA N1ECC FN42") != FTX_MESSAGE_RC_OK) {
        printf("selftest FAIL: could not build the RR73 base message\n");
        return 1;
    }

    int fail = 0;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        ftx_message_t m = base;
        const uint32_t g = cases[i].g;
        /* Overwrite ir (1 bit) and igrid4 (15 bits) where decode_std reads
         * them, leaving both callsign slots and i3 untouched. */
        m.payload[7] = (uint8_t)((m.payload[7] & 0xC0u) | ((g >> 10) & 0x1Fu));
        m.payload[8] = (uint8_t)((g >> 2) & 0xFFu);
        m.payload[9] = (uint8_t)((m.payload[9] & 0x3Fu) | ((g & 0x03u) << 6));

        spot_t s;
        if (!spot_from_message(&m, 1700000000LL, 14074000LL, 1500.0f, 0.2f, 20, &s)) {
            printf("selftest FAIL: %s yielded no spot\n", cases[i].what);
            fail = 1;
            continue;
        }
        if (cases[i].want_grid) {
            if (!s.grid[0]) {
                printf("selftest FAIL: %s lost its grid\n", cases[i].what);
                fail = 1;
            }
        } else if (s.grid[0]) {
            printf("selftest FAIL: %s put \"%s\" in the grid\n",
                   cases[i].what, s.grid);
            fail = 1;
        }
    }

    /* The polar exception, both ways. Same grid text; only the callsign
     * differs, so this pins the RULE rather than the string. */
    {
        static const struct { const char *call; int want_grid; } POLAR[] = {
            {"KP4COD", 0},   /* Puerto Rico, 18 N -- cannot be at 83 N */
            {"N1ECC",  0},   /* New England */
            {"RI0B",   1},   /* Russian Arctic: a real RR73 is plausible */
            {"OX3LX",  1},   /* Greenland */
            {"JW7QIA", 1},   /* Svalbard */
        };
        for (size_t i = 0; i < sizeof POLAR / sizeof POLAR[0]; i++) {
            const int got = grid_is_real("RR73", POLAR[i].call);
            if (got != POLAR[i].want_grid) {
                printf("selftest FAIL: RR73 from %s -> grid %s, wanted %s\n",
                       POLAR[i].call, got ? "kept" : "dropped",
                       POLAR[i].want_grid ? "kept" : "dropped");
                fail = 1;
            }
        }
        /* An ordinary locator is never affected by the callsign. */
        /* An ordinary locator is never affected by the callsign. */
        if (!grid_is_real("FN42", "KP4COD") || !grid_is_real("FN42", "RI0B")) {
            printf("selftest FAIL: an ordinary grid was dropped\n");
            fail = 1;
        }
        /* And polar_grid() must actually mean POLAR. A sign-off text whose
         * latitude band is temperate can never be a real locator, whoever
         * sends it -- this pins the latitude test itself, which a widened
         * band would otherwise slip past. */
        if (!polar_grid("RR73") || !polar_grid("QQ10")) {
            printf("selftest FAIL: polar_grid() rejected a polar square\n");
            fail = 1;
        }
        if (polar_grid("FN42") || polar_grid("DN18") || polar_grid("JO91")) {
            printf("selftest FAIL: polar_grid() called a temperate square polar\n");
            fail = 1;
        }
        /* The two tests must BOTH apply. A polar-capable callsign sending a
         * sign-off that is not a polar square is still a sign-off -- without
         * this, dropping polar_grid() from grid_is_real() goes unnoticed.
         * ("RRR" and "73" are not locators at all, so a grid-typed field can
         * only ever carry the RR73 form; this asserts the conjunction via the
         * one text that reaches it.) */
        if (grid_is_real("RRR", "RI0B") || grid_is_real("73", "OX3LX")) {
            printf("selftest FAIL: a non-polar sign-off was kept for a polar call\n");
            fail = 1;
        }
    }
    return fail;
}

static int selftest_spots(void)
{
    struct { const char *to, *de, *extra, *call, *grid; } want[] = {
        /* A CQ carries the sender and its grid. */
        {"CQ",    "K1ABC", "FN42", "K1ABC", "FN42"},
        {"CQ DX", "K1ABC", "FN42", "K1ABC", "FN42"},
        /* A reply carries a report, which must NOT be read as a grid --
         * "R-09" and "FN42" are both four characters. */
        {"W9XYZ", "K1ABC", "-11",  "K1ABC", ""},
        /* ⛔ The real one, off the air on 20 m: the first live decode was
         * "W0SJE 9Z4BM RR73" and the agent logged grid "RR73". A sign-off is
         * structurally two letters and two digits, so valid_grid() passes it;
         * only the library's field type distinguishes it (2 = report, where
         * a grid is 5). Caught in production, not by this test, which is why
         * the exact message is pinned here. */
        {"W0SJE", "9Z4BM", "RR73", "9Z4BM", ""},
        {"W9XYZ", "K1ABC", "RRR",  "K1ABC", ""},
        {"W9XYZ", "K1ABC", "R-09", "K1ABC", ""},
        {"W9XYZ", "K1ABC", "RR73", "K1ABC", ""},
        {"W9XYZ", "K1ABC", "73",   "K1ABC", ""},
        /* A compound callsign is still the sender -- but it travels as a
         * type-4 "nonstandard call" message, which has no grid field at all:
         * 58 bits go on the callsign itself. So the grid is legitimately
         * absent, and a skimmer that expected one here would be wrong about
         * the protocol rather than about this code. Verified against the
         * library: "CQ PA0SKT/2 JO33" renders as "CQ PA0SKT/2". */
        {"CQ",    "PA0SKT/2", "JO33", "PA0SKT/2", ""},
    };
    for (size_t i = 0; i < sizeof want / sizeof want[0]; i++) {
        char text[64];
        snprintf(text, sizeof text, "%s %s %s", want[i].to, want[i].de, want[i].extra);
        ftx_message_t msg;
        ftx_message_init(&msg);
        /* encode() packs a standard message; a compound callsign does not fit
         * one, so fall back to non-standard exactly as a real transmitter
         * would. Both forms must come back out of spot_from_message. */
        if (ftx_message_encode(&msg, &g_hash_if, text) != FTX_MESSAGE_RC_OK
            && ftx_message_encode_nonstd(&msg, &g_hash_if, want[i].to, want[i].de,
                                         want[i].extra) != FTX_MESSAGE_RC_OK) {
            printf("selftest FAIL: could not pack \"%s\"\n", text);
            return 1;
        }
        spot_t s;
        if (!spot_from_message(&msg, 1700000000LL, 14074000LL, 1500.0f, 0.2f, 20, &s)) {
            printf("selftest FAIL: \"%s\" yielded no spot\n", text);
            return 1;
        }
        if (strcmp(s.call, want[i].call) || strcmp(s.grid, want[i].grid)) {
            printf("selftest FAIL: \"%s\" gave call \"%s\" grid \"%s\", wanted \"%s\"/\"%s\"\n",
                   text, s.call, s.grid, want[i].call, want[i].grid);
            return 1;
        }
        /* The reported frequency is the dial plus the audio offset. */
        if (s.hz != 14074000LL + 1500LL) {
            printf("selftest FAIL: \"%s\" gave hz %lld\n", text, s.hz);
            return 1;
        }
        if (strcmp(s.band, "20m")) {
            printf("selftest FAIL: \"%s\" gave band \"%s\"\n", text, s.band);
            return 1;
        }
    }
    return 0;
}

static void record_cycle(const decode_t *dc);

/* A full cycle's worth of spots all reach the spot list. */
static int selftest_busy_cycle(void)
{
    static decode_t dc;
    memset(&dc, 0, sizeof dc);
    dc.cycle_t = 1700000000LL;
    dc.dial_hz = 14074000LL;
    snprintf(dc.band, sizeof dc.band, "20m");
    dc.nspots = MAX_CYCLE_SPOTS;
    dc.started = mono_s();     /* else record_cycle computes an absurd decode_ms */
    for (int i = 0; i < MAX_CYCLE_SPOTS; i++) {
        dc.spots[i].t = dc.cycle_t;
        dc.spots[i].hz = dc.dial_hz + 500 + i;
        dc.spots[i].snr = 20;
        snprintf(dc.spots[i].call, sizeof dc.spots[i].call, "K%dAB", i % 10);
        snprintf(dc.spots[i].band, sizeof dc.spots[i].band, "20m");
    }
    int before = G.nspots;
    record_cycle(&dc);
    if (G.nspots - before != MAX_CYCLE_SPOTS) {
        printf("selftest FAIL: busy cycle kept %d of %d\n", G.nspots - before, MAX_CYCLE_SPOTS);
        return 1;
    }
    G.nspots = before;
    G.ncycles = 0;
    return 0;
}

/* A decode that overruns its budget is recorded as such rather than hidden.
 * The budget is the whole point of RFC #7114, so the agent must not be able
 * to report a 3 s decode on a 2 s budget as if it were fine. */
static int selftest_budget_flagged(void)
{
    if (DECODE_BUDGET_MS != (CYCLE_SECONDS - CAPTURE_SECONDS) * 1000) {
        printf("selftest FAIL: budget constant is %d ms\n", DECODE_BUDGET_MS);
        return 1;
    }
    if (DECODE_BUDGET_MS <= 0) {
        printf("selftest FAIL: capture leaves no time to decode\n");
        return 1;
    }
    /* The captured audio must be long enough to hold a whole transmission:
     * 79 symbols at 0.16 s is 12.64 s, and a capture shorter than that cannot
     * decode a signal that started on time. */
    if ((double)CAPTURE_SECONDS < 12.64) {
        printf("selftest FAIL: capture %d s is shorter than an FT8 transmission\n", CAPTURE_SECONDS);
        return 1;
    }
    if (CAPTURE_SAMPLES != CAPTURE_SECONDS * FT8_RATE) {
        printf("selftest FAIL: capture sample count disagrees with the rate\n");
        return 1;
    }
    /* The cycle boundary must be computed from CYCLE_SECONDS.
     *
     * This caught a real bug on the radio. AetherWSPR hardcodes 120 in its
     * main loop, and defining CYCLE_SECONDS 15 did not change it, so the FT8
     * agent kept WSPR's two-minute rhythm while every constant said 15. It
     * still decoded, and the decode times looked plausible, so nothing
     * failed -- it simply missed seven of every eight transmissions. Measured
     * cycle starts were 120 s apart on a live FLEX-6300. */
    {
        const long long now = 1794192687LL;      /* a real observed timestamp */
        const long long cyc = now / CYCLE_SECONDS * CYCLE_SECONDS;
        if (cyc % CYCLE_SECONDS != 0 || now - cyc >= CYCLE_SECONDS) {
            printf("selftest FAIL: cycle boundary %lld for %lld\n", cyc, now);
            return 1;
        }
        if (CYCLE_SECONDS != 15) {
            printf("selftest FAIL: FT8 cycle is 15 s, not %d\n", CYCLE_SECONDS);
            return 1;
        }
    }
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
    /* The DAX channel is picked by the radio's answer to
     * "slice set <n> dax=<ch>", and the stream is created only after a yes.
     * A FLEX-6300 refuses that for channels 3 and 4 (50000016) yet ACCEPTS a
     * stream create on them, so an agent that read only the stream's reply
     * sat on channel 4 with no audio. Every step below reads what was
     * actually written to the radio. */
    char sent[256];
    fcntl(p[0], F_SETFL, O_NONBLOCK);
#define DRAIN() do { while (read(p[0], sent, sizeof sent) > 0) {} sent[0] = 0; } while (0)
#define SENT() do { ssize_t k_ = read(p[0], sent, sizeof sent - 1); sent[k_ > 0 ? k_ : 0] = 0; } while (0)
    /* A new slice asks for the CONFIGURED channel -- not the one left over
     * from the last slice, which is how a saved channel used to be ignored --
     * and does not create the stream yet. */
    a.want_info = 0;           /* request numbers below must not match the
                                * replies this test faked above (R7, R8...) */
    a.seq = 1000;
    a.slice = -1;
    a.stream = 0;
    a.dax_try = 4;             /* left over from an earlier slice */
    c.dax_channel = 1;         /* what the operator saved since */
    a.want_slice = 20;
    DRAIN();
    strcpy(l, "R20|0|0");
    api_line(&a, l, &c);
    SENT();
    if (a.dax_try != 1 || !a.want_dax || a.want_stream || !strstr(sent, "slice set 0 dax=1")
        || strstr(sent, "stream create")) {
        printf("selftest FAIL: a new slice did not ask for the saved DAX channel first: %s\n", sent);
        fail = 1;
    }
    /* A 6300 with the default 4: refused at the slice twice, then 2 is
     * accepted, and only then is a stream asked for -- on 2. */
    c.dax_channel = 4;
    dax_ask(&a, c.dax_channel);
    for (int ch = 4; ch > 2; ch--) {
        DRAIN();
        snprintf(l, sizeof l, "R%d|50000016|", a.want_dax);
        api_line(&a, l, &c);
        SENT();
        char want[32];
        snprintf(want, sizeof want, "slice set 0 dax=%d", ch - 1);
        if (a.dax_try != ch - 1 || !a.want_dax || a.want_stream || !strstr(sent, want)
            || strstr(sent, "stream create")) {
            printf("selftest FAIL: channel %d refused at the slice did not step to %d: %s\n", ch, ch - 1, sent);
            fail = 1;
        }
    }
    DRAIN();
    snprintf(l, sizeof l, "R%d|0|", a.want_dax);
    api_line(&a, l, &c);
    SENT();
    if (a.want_dax || !a.want_stream || !strstr(sent, "stream create type=dax_rx dax_channel=2")) {
        printf("selftest FAIL: an accepted channel 2 did not create its stream on 2: %s\n", sent);
        fail = 1;
    }
    snprintf(l, sizeof l, "R%d|0|04000008", a.want_stream);
    api_line(&a, l, &c);
    if (a.stream != 0x04000008 || a.stream_retry != 0 || G.dax_bound != 2) {
        printf("selftest FAIL: the stream on channel 2 was not recorded as bound to 2 (%d)\n", G.dax_bound);
        fail = 1;
    }
    /* A stream the radio refuses steps down the same way, through the slice. */
    a.stream = 0;
    dax_ask(&a, 2);
    snprintf(l, sizeof l, "R%d|0|", a.want_dax);
    api_line(&a, l, &c);
    DRAIN();
    snprintf(l, sizeof l, "R%d|50000016|", a.want_stream);
    api_line(&a, l, &c);
    SENT();
    if (a.dax_try != 1 || !a.want_dax || a.want_stream || !strstr(sent, "slice set 0 dax=1")) {
        printf("selftest FAIL: a refused stream on 2 did not step to channel 1: %s\n", sent);
        fail = 1;
    }
    /* ...and when channel 1 is refused there is nothing left, so it arms the
     * retry timer and starts again from the config rather than looping. */
    a.stream_retry = 0;
    snprintf(l, sizeof l, "R%d|50000016|", a.want_dax);
    api_line(&a, l, &c);
    if (a.dax_try != 0 || a.want_dax || a.want_stream || a.stream_retry <= 0) {
        printf("selftest FAIL: a refusal of channel 1 did not fall back to the retry timer\n");
        fail = 1;
    }
    a.stream_retry = 0;
    G.dax_bound = 0;
    /* FlexLib's key for the antenna (Radio.cs RequestSlice). */
    DRAIN();
#undef DRAIN
    make_slice(&a, 14095600, "ANT2", 4);
    SENT();
#undef SENT
    fail |= !strstr(sent, " rxant=ANT2 ");
    close(p[0]);
    close(p[1]);
    G.radio_call[0] = 0;
    G.stream_ok = 0;
    if (fail) { printf("selftest FAIL: radio lines\n"); return 1; }
    return 0;
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

static const char *BAD_GRIDS[] = {"ZZ99", "AA00zz", "D18P", "DN1", "DN18p", "1234", "DN18-g"};

static int selftest(void)
{
    if (selftest_dax_fallback() || selftest_rr73_both_encodings()) return 1;
    if (selftest_persistence() || selftest_spots() || selftest_budget_flagged()
        || selftest_busy_cycle() || selftest_radio_lines() || selftest_save_failure()) return 1;
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
            long long t = t0 + (long long)CYCLE_SECONDS * k;
            const char *b = band_for_cycle(&c, "DN18pg", t, "", &period);
            const char *want = COORDINATED[((t / 60) % 60 % 20) / 2];
            if (strcmp(b, want) || period < 0) { printf("selftest FAIL: hop %d gave %s, want %s\n", k, b, want); return 1; }
        }
        unsigned sub = (1u << band_index("40m")) | (1u << band_index("6m"));
        for (int p = 0; p < 4; p++) c.hop[p] = sub;
        int seen6 = 0;
        for (int k = 0; k < 720; k++) {
            int period;
            const char *b = band_for_cycle(&c, "DN18pg", t0 + (long long)CYCLE_SECONDS * k, "", &period);
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

    /* End to end, on a real recording.
     *
     * AetherWSPR synthesizes a WSPR signal at a chosen SNR, pushes it through
     * the capture DSP and checks the decode. The equivalent here uses one of
     * ft8_lib's own reference recordings instead, because a real off-air file
     * tests the decoder against signals we did not generate -- a synthesized
     * tone can pass while the decoder is subtly wrong about the real thing.
     *
     * The file is 12 kHz mono, which is the rate the agent feeds the decoder,
     * so this exercises run_decode() exactly as a live cycle does. It does not
     * exercise the 24 kHz decimation in dsp_push(); that is covered by
     * selftest_decimation() below.
     *
     * The reference .txt names 30 messages. We require a substantial majority
     * rather than all 30: ft8_lib has no OSD and no signal subtraction, so it
     * finds roughly three quarters of what WSJT-X finds, and that is a
     * documented property of the dependency, not a defect here. A hard floor
     * catches a real regression without pretending the decoder is WSJT-X.
     */
    {
        const char *wav = "ft8_lib/test/wav/websdr_test6.wav";
        static float audio[15 * FT8_RATE];
        int nsamples = (int)(sizeof audio / sizeof audio[0]);
        int rate = 0;
        if (load_wav(audio, &nsamples, &rate, wav) < 0) {
            printf("selftest SKIP: cannot read %s (run from tools/ft8-agent)\n", wav);
        } else if (rate != FT8_RATE) {
            printf("selftest FAIL: %s is %d Hz, expected %d\n", wav, rate, FT8_RATE);
            return 1;
        } else {
            decode_t dc;
            memset(&dc, 0, sizeof dc);
            dc.cycle_t = 1700000000LL;
            dc.dial_hz = 14074000LL;
            snprintf(dc.band, sizeof dc.band, "20m");
            double t0 = mono_s();
            run_decode(&dc, audio, nsamples);
            double took = mono_s() - t0;

            printf("selftest: %s -> %d spot(s) in %.0f ms (budget %d ms)\n",
                   wav, dc.nspots, took * 1000, DECODE_BUDGET_MS);
            for (int i = 0; i < dc.nspots && i < 8; i++)
                printf("   %-12s %-6s %8lld Hz  score %d  %s\n", dc.spots[i].call,
                       dc.spots[i].grid, dc.spots[i].hz, dc.spots[i].snr, dc.spots[i].msg);

            /* ⛔ A sign-off is NOT a grid, and this is the test that proves it
             * through the REAL decode path rather than a message we built
             * ourselves. RR73 is structurally two letters and two digits, so
             * valid_grid() passes it; only the library's field type separates
             * them (TOKEN vs GRID). This recording carries three "73"
             * sign-offs, the same token class.
             *
             * Checked over EVERY spot, not the eight printed above: the
             * failure that put RR73 on 15 spots on 40 m would have hidden
             * below a top-8 listing. */
            for (int i = 0; i < dc.nspots; i++) {
                if (!strcmp(dc.spots[i].grid, "RR73") || !strcmp(dc.spots[i].grid, "RRR")
                    || !strcmp(dc.spots[i].grid, "73")) {
                    printf("selftest FAIL: \"%s\" put the sign-off \"%s\" in the grid\n",
                           dc.spots[i].msg, dc.spots[i].grid);
                    return 1;
                }
            }

            if (dc.nspots < 15) {
                printf("selftest FAIL: only %d spots from a file with 30 reference messages\n",
                       dc.nspots);
                return 1;
            }
            /* Every spot must carry a callsign and a band, or it is not a spot. */
            for (int i = 0; i < dc.nspots; i++) {
                if (!dc.spots[i].call[0]) {
                    printf("selftest FAIL: spot %d has no callsign\n", i);
                    return 1;
                }
                if (dc.spots[i].hz <= dc.dial_hz) {
                    printf("selftest FAIL: spot %d has hz %lld, not above the dial\n",
                           i, dc.spots[i].hz);
                    return 1;
                }
            }
        }
    }

    /* The 24 kHz -> 12 kHz decimation keeps a tone where it was.
     *
     * This is the one DSP stage the recording above cannot reach. A 1500 Hz
     * tone sampled at 24 kHz must still be a 1500 Hz tone after dsp_push(),
     * and the sample count must halve. Getting this wrong would shift every
     * decoded signal in frequency, which no decode-rate check would catch
     * because ft8_lib searches the whole passband anyway.
     */
    {
        static float out[CAPTURE_SAMPLES];
        dsp_t d;
        d.audio = out;
        dsp_reset(&d);
        const double tone = 1500.0;
        const int fed = 2 * FT8_RATE;            /* 1 s of 24 kHz audio */
        for (int i = 0; i < fed; i++)
            dsp_push(&d, (float)(0.5 * sin(2 * M_PI * tone * i / AUDIO_RATE)));

        if (d.n != fed / DECIM1) {
            printf("selftest FAIL: %d samples in gave %d out, expected %d\n",
                   fed, d.n, fed / DECIM1);
            return 1;
        }
        /* Count zero crossings over the settled part, past the FIR's delay. */
        int crossings = 0;
        for (int i = NTAPS + 1; i < d.n; i++)
            if ((out[i - 1] < 0) != (out[i] < 0)) crossings++;
        double secs = (double)(d.n - NTAPS - 1) / FT8_RATE;
        double got = crossings / (2.0 * secs);
        if (got < tone * 0.97 || got > tone * 1.03) {
            printf("selftest FAIL: 1500 Hz tone measured %.0f Hz after decimation\n", got);
            return 1;
        }
        /* And the level must survive: a unity-gain filter, not an attenuator. */
        double peak = 0;
        for (int i = NTAPS + 1; i < d.n; i++)
            if (fabs(out[i]) > peak) peak = fabs(out[i]);
        if (peak < 0.4 || peak > 0.6) {
            printf("selftest FAIL: 0.5 amplitude tone came out at %.3f\n", peak);
            return 1;
        }
    }

    printf("selftest PASS\n");
    return 0;
}


/* ------------------------------------------------------------------ main */


static void record_cycle(const decode_t *dc)
{
    /* The spots are already decoded: run_decode() filled dc->spots in this
     * process. AetherWSPR parsed wsprd's stdout here instead, and queued each
     * spot for wsprnet; there is no uploader here (see "no upload" above). */
    const spot_t *got = dc->spots;
    const int n = dc->nspots;
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
    /* decode_ms against the decode budget is the number RFC #7114 turns on, so
     * every cycle logs it and says plainly when it overran. */
    logf_("cycle %lld %s: %d spot(s), decode %d of %d ms budget%s (%.1f s CPU, %ld KB peak, "
          "radio %.0f%% busy), audio %d%%, %d packet(s) lost, level %.1f dBFS, %ld KB free",
          dc->cycle_t, dc->band, n, c->decode_ms, DECODE_BUDGET_MS,
          c->decode_ms > DECODE_BUDGET_MS ? " OVER" : "",
          dc->cpu_s, dc->rss_kb, dc->sys_busy_pct, dc->audio_pct, dc->lost, dc->dbfs,
          dc->mem_avail_kb);
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

/* Decode one 15 s WAV and print what it found, one message per line.
 *
 * This is the harness the #7114 OSD ruling needs: "measure the decode rate
 * against the same reference corpus with and without it". It runs the SAME
 * run_decode() the live agent runs, so a corpus score measures the decoder
 * the radio ships, not a separate test path.
 *
 * Output is one line per spot: frequency, SNR, then the message text. A
 * scoring harness compares the message text against ft8_lib's reference
 * .txt files, which carry their metadata columns in a different format.
 *
 * The dial is fixed at the 20 m FT8 frequency because a WAV carries no dial:
 * it only sets the absolute frequency printed, never what decodes. */
#ifdef AETHER_WITH_OSD
#include "osd/osd174_91.h"
/* OSD search depth. Read by ft8_lib's decode.c when belief propagation
 * fails. Settable from the environment so the "with and without" the
 * #7114 ruling asks for is one binary and two runs, not two builds --
 * a rebuild between arms would leave the comparison open to a compiler
 * or source difference nobody checked for.
 *
 * AETHER_FT8_OSD=0 disables it, matching the pre-OSD decoder exactly:
 * depth 0 still returns early, so nothing new runs and nothing new is
 * found. That is the control arm. */
static osd_depth_t g_osd_depth = OSD_DEPTH_2;

/* Map the agent's four depth names onto OSD search depths.
 *
 * The names come from the WSPR agent this one was built from, where they
 * were stored and reported but wired to nothing. OSD gives them a meaning:
 *
 *   fast      OSD off        -- belief propagation only, the pre-OSD decoder
 *   standard  OSD depth 2    -- the default; measured 77.3% on the corpus
 *   normal    OSD depth 3    -- +0.9 points for about 2x the time
 *   deep      OSD depth 4    -- WSJT-X's own FT8 setting
 *
 * ⚠ "deep" is very unlikely to fit a FLEX-6300's ~2 s budget: on x86 it is
 * roughly 16x the cost of depth 2, and this radio already runs depth 2 at
 * about 1.4 s. It is offered because the ruling asked for WSJT-X's setting
 * to be measured, not because it is a sensible choice on a 6000.
 *
 * AETHER_FT8_OSD still overrides, for a host-side scoring run where there
 * is no config API in play. */
static int g_osd_env_set = 0;

/* How often OSD was asked, and how often it recovered a frame. Read by
 * decodewav's summary line. The ratio is the whole question on a live
 * band: belief propagation fails on every noise candidate too, and OSD
 * pays its full cost on each of those before returning nothing. */
long long aether_osd_attempted = 0;
long long aether_osd_rescued = 0;
/* Candidates the gate kept OSD away from. */
long long aether_osd_skipped = 0;

/* Temporary instrumentation for choosing the gate threshold. Histograms
 * the candidate score and the post-BP unsatisfied-check count, split by
 * whether OSD rescued anything, so the threshold comes from the data
 * rather than from a guess. Printed by decodewav. */
#define PROBE_SC 12
#define PROBE_LE 50
static long long g_probe_score[2][PROBE_SC];
static long long g_probe_lerr[2][PROBE_LE];

void aether_osd_probe(int score, int ldpc_errors, int rescued)
{
    int r = rescued ? 1 : 0;
    int s = score / 10;          /* 0-9, 10-19, ... */
    if (s < 0) s = 0;
    if (s >= PROBE_SC) s = PROBE_SC - 1;
    g_probe_score[r][s]++;
    int e = ldpc_errors;
    if (e < 0) e = 0;
    if (e >= PROBE_LE) e = PROBE_LE - 1;
    g_probe_lerr[r][e]++;
}

static void probe_dump(void)
{
    fprintf(stderr, "  score  wasted rescued\n");
    for (int i = 0; i < PROBE_SC; i++)
        if (g_probe_score[0][i] || g_probe_score[1][i])
            fprintf(stderr, "   %3d+  %6lld %7lld\n", i * 10,
                    g_probe_score[0][i], g_probe_score[1][i]);
    fprintf(stderr, "  lderr  wasted rescued\n");
    for (int i = 0; i < PROBE_LE; i++)
        if (g_probe_lerr[0][i] || g_probe_lerr[1][i])
            fprintf(stderr, "   %3d   %6lld %7lld\n", i,
                    g_probe_lerr[0][i], g_probe_lerr[1][i]);
}

osd_depth_t aether_osd_depth(void)
{
    if (g_osd_env_set) return g_osd_depth;
    /* Called from the decoder, which holds no lock of its own, while the
     * config POST thread can be writing G.cfg. Read it under the mutex. */
    pthread_mutex_lock(&G.mu);
    const int d = G.cfg.depth;
    pthread_mutex_unlock(&G.mu);
    switch (d) {
    case 1:  return OSD_DEPTH_OFF; /* fast */
    case 2:  return OSD_DEPTH_3;   /* normal */
    case 3:  return OSD_DEPTH_4;   /* deep */
    default: return OSD_DEPTH_2;   /* standard */
    }
}

static void osd_depth_from_env(void)
{
    const char *mc = getenv("AETHER_FT8_CAND");
    if (mc && *mc) {
        int v = atoi(mc);
        /* Upper bound is the array size: ftx_find_candidates writes up
         * to the budget it is given, so a larger value would overflow. */
        if (v >= 8 && v <= FT8_MAX_CANDIDATES) g_max_candidates = v;
    }
    const char *s = getenv("AETHER_FT8_OSD");
    if (!s || !*s) return;
    int v = atoi(s);
    if (v < 0) v = 0;
    if (v > 6) v = 6;
    g_osd_depth = (osd_depth_t)v;
    g_osd_env_set = 1;
}
#endif

static int decode_wav_file(const char *path)
{
    static float audio[15 * FT8_RATE];
    int nsamples = (int)(sizeof audio / sizeof audio[0]);
    int rate = 0;
    if (load_wav(audio, &nsamples, &rate, path) < 0) {
        fprintf(stderr, "decodewav: cannot read %s\n", path);
        return 1;
    }
    if (rate != FT8_RATE) {
        fprintf(stderr, "decodewav: %s is %d Hz, expected %d\n", path, rate, FT8_RATE);
        return 1;
    }
    decode_t dc;
    memset(&dc, 0, sizeof dc);
    dc.cycle_t = 1700000000LL;
    dc.dial_hz = 14074000LL;
    snprintf(dc.band, sizeof dc.band, "20m");
    double t0 = mono_s();
    run_decode(&dc, audio, nsamples);
    double took = mono_s() - t0;
    for (int i = 0; i < dc.nspots; i++)
        printf("%8lld %4d %s\n", dc.spots[i].hz, dc.spots[i].snr, dc.spots[i].msg);
    fprintf(stderr, "decodewav: %s -> %d spot(s) in %.0f ms", path, dc.nspots, took * 1000);
#ifdef AETHER_WITH_OSD
    fprintf(stderr, " [osd %lld asked, %lld rescued, %lld gated]", aether_osd_attempted, aether_osd_rescued, aether_osd_skipped);
#endif
    fputc('\n', stderr);
#ifdef AETHER_WITH_OSD
    if (getenv("AETHER_FT8_PROBE")) probe_dump();
#endif
    return 0;
}

int main(int argc, char *argv[])
{
    signal(SIGPIPE, SIG_IGN);
    /* PID 1 in a container: the kernel drops SIGTERM unless it is handled. */
    signal(SIGTERM, on_term);
    /* One anti-alias stage before decimating by 2. The cutoff sits above
     * FT8's 3 kHz passband and below the 6 kHz Nyquist of the 12 kHz output,
     * so nothing the decoder wants is lost and nothing folds back. */
    design_lowpass(g_h1, 5000.0 / AUDIO_RATE);
    snprintf(g_self, sizeof g_self, "%s", argv[0]);
    char self_dir[400];
    snprintf(self_dir, sizeof self_dir, "%s", argv[0]);
    char *slash = strrchr(self_dir, '/');
    if (slash) *slash = 0; else strcpy(self_dir, ".");
    /* Decoder scratch (the .c2 file, wsprd's hash table) lives in RAM. */
    const char *tmp = getenv("TMPDIR") && *getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
    snprintf(g_work, sizeof g_work, "%s/aether-wspr", tmp);
    mkdir(g_work, 0700);
#ifdef AETHER_WITH_OSD
    osd_depth_from_env();
#endif
    if (argc >= 2 && !strcmp(argv[1], "selftest")) return selftest();
    /* decodewav: one WAV in, its messages out, nothing else. Dispatched here
     * beside selftest and BEFORE the status listener is bound, because it is
     * a batch tool -- a scoring run invokes it once per file, and binding
     * STATUS_PORT would make every invocation after the first fail with
     * "Address in use" instead of decoding. It touches no socket and no
     * radio. Exit status is 0 when the file decoded, 1 when it could not be
     * read, so a harness can tell "no messages" from "no decode". */
    if (argc >= 3 && !strcmp(argv[1], "decodewav")) return decode_wav_file(argv[2]);
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
    g_status_fd = ls; /* POST /v1/restart closes this before execv() */
    pthread_t th;
    pthread_create(&th, NULL, status_thread, &ls);
    if (!getenv("AETHER_WSPR_RADIO")) pthread_create(&th, NULL, publish_thread, NULL);

    int udp = socket(AF_INET, SOCK_DGRAM, 0);
    int rcv = 1 << 20;
    setsockopt(udp, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof rcv);
    struct sockaddr_in ua = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_ANY)};
    bind(udp, (struct sockaddr *)&ua, sizeof ua);
    socklen_t ual = sizeof ua;
    getsockname(udp, (struct sockaddr *)&ua, &ual);
    int udp_port = ntohs(ua.sin_port);

    static float audio[CAPTURE_SAMPLES];
    static unsigned char pkt[65536];
    dsp_t d;
    d.audio = audio;
    dsp_reset(&d);

    api_t a = {.fd = -1, .slice = -1};
    decode_t dc = {.busy = 0};
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
            a.want_dax = 0;
            a.dax_try = 0;         /* the new slice starts from the saved channel */
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
        /* FT8 cycles start every 15 s on the UTC second, not every 120 s as
         * WSPR does. This was the one place the cycle length stayed a literal
         * in AetherWSPR, so CYCLE_SECONDS must be used here or the agent
         * silently keeps WSPR's two-minute rhythm. */
        long long cyc = (long long)now / CYCLE_SECONDS * CYCLE_SECONDS;
        long long next = cyc + CYCLE_SECONDS;
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

        if (a.fd >= 0 && a.slice >= 0 && !a.stream && !a.want_stream && !a.want_dax
            && a.stream_retry && mono_s() >= a.stream_retry) {
            a.stream_retry = 0;
            dax_ask(&a, cfg.dax_channel);   /* the whole walk again, from the config */
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
            } else if (pct < 80) {
                logf_("cycle %lld: only %d%% of the audio arrived; not decoded", capture_t, pct);
            } else {
                /* Decode in this thread, now. AetherWSPR wrote a .c2 file and
                 * forked wsprd, so it had to poll a pipe for the result; a
                 * library call needs none of that. The cost is that the loop
                 * stops here for the length of the decode, which is exactly
                 * the thing being measured -- a decode that overruns the
                 * cycle shows up as a late next capture, not as a silent
                 * backlog. */
                dc.busy = 1;
                dc.cycle_t = capture_t;
                dc.dial_hz = capture_dial;
                snprintf(dc.band, sizeof dc.band, "%s", capture_band);
                dc.audio_pct = pct;
                dc.lost = d.lost;
                dc.dbfs = d.in ? (float)(10 * log10(d.sumsq / (double)d.in + 1e-20)) : -200.0f;
                dc.started = mono_s();
                cpu_jiffies(&dc.cpu0_busy, &dc.cpu0_total);
                float cpu0 = 0;
                long rss0 = 0;
                own_usage(&cpu0, &rss0);

                pthread_mutex_lock(&G.mu); G.state = "decoding"; pthread_mutex_unlock(&G.mu);
                run_decode(&dc, audio, d.n);

                unsigned long long b1, t1;
                cpu_jiffies(&b1, &t1);
                dc.sys_busy_pct = t1 > dc.cpu0_total
                    ? (float)(100.0 * (b1 - dc.cpu0_busy) / (double)(t1 - dc.cpu0_total)) : 0.0f;
                float cpu1 = 0;
                long rss1 = 0;
                own_usage(&cpu1, &rss1);
                dc.cpu_s = cpu1 - cpu0;
                dc.rss_kb = rss1;
                dc.mem_avail_kb = mem_available_kb();
                record_cycle(&dc);
                dc.busy = 0;
            }
            capture_t = 0;
        }

        pthread_mutex_lock(&G.mu);
        if (!synced) G.state = "waiting_clock";
        else if (a.fd >= 0 && !capture_t && !dc.busy) G.state = a.stream ? "waiting_cycle" : "starting_audio";
        pthread_mutex_unlock(&G.mu);

        struct pollfd pf[3];
        int np = 0;
        pf[np++] = (struct pollfd){.fd = udp, .events = POLLIN};
        int ai = -1;
        if (a.fd >= 0) { ai = np; pf[np++] = (struct pollfd){.fd = a.fd, .events = POLLIN}; }
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
    }
}
