// AetherPD: a PureSignal-style predistortion waveform for FlexRadio radios.
//
// It registers a waveform mode "PD" with the RAW underlying mode, so the
// radio hands it I/Q and takes I/Q back at 24 kHz. With PD selected on a
// slice:
//   receive   RAW I/Q -> USB demodulation -> speaker audio
//   transmit  mic audio -> USB analytic signal -> pd_core_correct -> I/Q
//   feedback  DAX IQ from a panadapter fed by an external coupler on the
//             transmit side's receive input (RX_A for ANT1), during TX
//             -> mixed to the transmit frequency, decimated to 24 kHz
//             -> pd_align -> pd_eq -> pd_core_calibrate
//
// It never keys the radio: it sends transmit samples only in answer to the
// radio's own transmit stream, and only while the interlock is PTT_REQUESTED
// or TRANSMITTING. At UNKEY_REQUESTED it goes silent at once, as the
// waveform-sdk example does: the radio finishes unkeying only when the
// waveform stops sending (FLEX-8600, 2026-11-11: answering on through
// UNKEY_REQUESTED held a test two-tone on the air until the slice left PD).
//
// Protocol: the waveform API as FlexRadio's public waveform-sdk uses it
// (waveform create / set, slice and interlock status, VITA-49 float
// packets to UDP 4991, replies on the incoming stream IDs), and FlexLib
// for the DAX IQ stream (Radio.cs "stream create type=dax_iq",
// DAXIQStream.cs "stream set ... daxiq_rate=").
//
// Settings, from the environment (launch.sh sets them on the radio):
//   AETHER_PD_RADIO        radio address (default SSDR_RADIO_ADDRESS, else 127.0.0.1)
//   AETHER_PD_DAXIQ        DAX IQ channel of the coupler's panadapter (default 1)
//   AETHER_PD_DAXIQ_RATE   its rate, a multiple of 24000 (default 48000)
//   AETHER_PD_PEAK         transmit peak amplitude before correction (default 0.7)
//   AETHER_PD_MIC_GAIN     mic gain (default 1.0)
//   AETHER_PD_MAX_DELAY_MS longest loop delay to search (default 400)
//   AETHER_PD_CORRECT      0 runs the waveform with the correction off (default 1)
//   AETHER_PD_DUMP         file: write the first 10 s of aligned reference/feedback
//                          pairs (float32 tx_i tx_q rx_i rx_q), for analysis
//   AETHER_PD_TEST_TONES   "f1,f2" in Hz: while keyed, send an equal two-tone
//                          at f1 and f2 above the carrier instead of the mic,
//                          at AETHER_PD_PEAK (a bench test signal)
//
// Status: one JSON line per TCP connection on port 48996, LAN callers only
// (like AetherHealth on 48994). Read-only.
//
//   pd_waveform            run
//   pd_waveform selftest   checks of parsing, packing and DSP; opens no socket
#include "pd_align.h"
#include "pd_core.h"
#include "pd_eq.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define WF_NAME "AetherPD"
#define WF_MODE "PD"
#define WF_VERSION "0.1.0"
#define RATE 24000
#define BLOCK 512
#define TAPS 127
#define MAX_SAMPLES 360          // floats per VITA payload: 180 complex samples
#define VITA_PORT 4991
#define FLEX_OUI 0x00001C2Du
#define INFO_CLASS 0x534Cu
#define CLASS_AUDIO_24K 0x03E3u  // audio, float, 24 kHz, 32-bit, 2 per frame
#define FIFO_LEN (RATE * 2)
#define STATUS_PORT 48996
#define CLASS_FLOAT_BIT 0x0200u  // packet class: 32-bit float samples
// The delay is found again after this many consecutive blocks whose pairs
// don't match well. On the FLEX-8600 the loop delay at key-up differed from
// the settled one by about 4 samples (coherence 0.82 instead of 0.999), and
// an estimate on a two-tone, whose envelope repeats, can lock a wrong lag.
#define WEAK_COHERENCE 0.9
#define WEAK_BLOCKS 8
// Feedback samples discarded at the start of each transmission before the
// delay is measured, so the measurement sees the settled loop.
#define SETTLE_SAMPLES RATE
// A test two-tone repeats (710/1900 Hz every 100 ms), so every multiple of
// that lag correlates perfectly and the loop delay is ambiguous. Each test
// transmission, and each re-acquisition, opens with this much band-limited
// noise instead: long enough to cover SETTLE_SAMPLES and an acquisition.
#define TEST_NOISE_SAMPLES (2 * RATE)
// The radio filters the waveform's I/Q before its amplifier, so the
// reference is not the amplifier's input; WDSP then sees the low-amplitude
// pairs scatter in phase and rejects every fit (FLEX-8600: sin^2+cos^2 off by
// 0.06-0.07 against a 0.05 limit). After each delay lock the first
// EQ_TRAIN_SAMPLES aligned pairs fit an EQ_TAPS FIR from reference to
// feedback; calibration then sees the reference through it. On the bench
// capture that took the phase spread below x = 0.1 from 39 to 6 degrees and
// every fit was accepted.
#define EQ_TAPS 31
#define EQ_TRAIN_SAMPLES RATE

static volatile sig_atomic_t g_stop;
static void on_term(int sig) { (void)sig; g_stop = 1; }

static void logf_(const char* fmt, ...)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    fprintf(stderr, "%ld.%03ld ", (long)ts.tv_sec, ts.tv_nsec / 1000000);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static double env_d(const char* k, double def)
{
    const char* v = getenv(k);
    return v && *v ? atof(v) : def;
}

/* ------------------------------------------------------------ parsing */

// Finds key=value in a space-separated status line; copies the value.
static int kv(const char* line, const char* key, char* out, size_t cap)
{
    size_t kl = strlen(key);
    for (const char* p = line; (p = strstr(p, key)); p += kl) {
        if ((p == line || p[-1] == ' ' || p[-1] == '|') && p[kl] == '=') {
            const char* v = p + kl + 1;
            size_t n = strcspn(v, " \r\n");
            if (n >= cap)
                n = cap - 1;
            memcpy(out, v, n);
            out[n] = 0;
            return 1;
        }
    }
    return 0;
}

static int kv_u32(const char* line, const char* key, uint32_t* out)
{
    char v[32];
    if (!kv(line, key, v, sizeof v))
        return 0;
    *out = (uint32_t)strtoul(v, NULL, 0);
    return 1;
}

/* --------------------------------------------------------------- VITA */

typedef struct {
    uint32_t sid;
    unsigned cls;     // packet class code, e.g. 0x03E3
    int count;        // 4-bit packet count
    const unsigned char* payload;
    int words;        // payload length in 32-bit words
} vita_t;

static uint32_t be32(const unsigned char* p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void put32(unsigned char* p, uint32_t v)
{
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

// Parses an IF-data-with-stream-ID packet with class ID and both
// timestamps (the shape every FlexRadio data stream uses).
static int vita_parse(const unsigned char* p, ssize_t n, vita_t* v)
{
    if (n < 28)
        return 0;
    uint32_t w0 = be32(p);
    if ((w0 >> 28) != 1 || !(w0 & 0x08000000u))
        return 0;
    if (be32(p + 8) != FLEX_OUI || (be32(p + 12) >> 16) != INFO_CLASS)
        return 0;
    int len = (int)(w0 & 0xFFFF);
    int trailer = (w0 & 0x04000000u) ? 1 : 0;
    if (len * 4 > n || len < 7 + trailer)
        return 0;
    v->sid = be32(p + 4);
    v->cls = be32(p + 12) & 0xFFFF;
    v->count = (int)((w0 >> 16) & 0xF);
    v->payload = p + 28;
    v->words = len - 7 - trailer;
    return 1;
}

static float word_float(const unsigned char* p)
{
    uint32_t u = be32(p);
    float f;
    memcpy(&f, &u, 4);
    return f;
}

// DAX IQ payloads are little-endian floats, unlike the waveform's own audio
// streams (measured on a FLEX-8600, SmartSDR 4.3.16: bytes 00 00 80 c2 are
// -64.0). The radio's headers stay big-endian.
static float word_float_le(const unsigned char* p)
{
    uint32_t u = (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0];
    float f;
    memcpy(&f, &u, 4);
    return f;
}

// Builds a float-pair packet of class cls like the waveform SDK's
// vita_send_data_packet: UTC + real-time timestamps from the system clock.
static int vita_build(unsigned char* out, uint32_t sid, unsigned cls, int count, const float* pairs, int n)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    int len = 7 + 2 * n;
    // type 1 (IF data with stream ID), class present, TSI UTC (1),
    // TSF real-time (2), packet count, length in words.
    uint32_t w0 = 1u << 28 | 1u << 27 | 1u << 22 | 2u << 20 | (uint32_t)(count & 0xF) << 16 | (uint32_t)len;
    put32(out, w0);
    put32(out + 4, sid);
    put32(out + 8, FLEX_OUI);
    put32(out + 12, INFO_CLASS << 16 | cls);
    put32(out + 16, (uint32_t)ts.tv_sec);
    uint64_t ps = (uint64_t)ts.tv_nsec * 1000u;
    put32(out + 20, (uint32_t)(ps >> 32));
    put32(out + 24, (uint32_t)ps);
    for (int i = 0; i < 2 * n; i++) {
        uint32_t u;
        memcpy(&u, &pairs[i], 4);
        put32(out + 28 + 4 * i, u);
    }
    return 4 * len;
}

/* ---------------------------------------------------------------- DSP */

typedef struct {
    double hr[TAPS], hi[TAPS];   // complex taps
    double z[TAPS];              // real input history
    int pos;
} cbp_t;

// Complex band-pass for USB: a low-pass of half-width (hi-lo)/2 shifted to
// the band's centre. Fed a real signal, it outputs the analytic signal of
// the band lo..hi, which is upper-sideband I/Q.
static void cbp_init(cbp_t* f, double lo, double hi)
{
    memset(f, 0, sizeof *f);
    double fc = (hi - lo) / 2 / RATE, f0 = (hi + lo) / 2 / RATE, sum = 0;
    double lp[TAPS];
    for (int n = 0; n < TAPS; n++) {
        int m = n - (TAPS - 1) / 2;
        double s = m == 0 ? 2 * fc : sin(2 * M_PI * fc * m) / (M_PI * m);
        double w = 0.42 - 0.5 * cos(2 * M_PI * n / (TAPS - 1)) + 0.08 * cos(4 * M_PI * n / (TAPS - 1));
        lp[n] = s * w;
        sum += lp[n];
    }
    for (int n = 0; n < TAPS; n++) {
        int m = n - (TAPS - 1) / 2;
        f->hr[n] = lp[n] / sum * cos(2 * M_PI * f0 * m);
        f->hi[n] = lp[n] / sum * sin(2 * M_PI * f0 * m);
    }
}

static void cbp_step(cbp_t* f, double x, double* i, double* q)
{
    f->z[f->pos] = x;
    double si = 0, sq = 0;
    int k = f->pos;
    for (int n = 0; n < TAPS; n++) {
        si += f->hr[n] * f->z[k];
        sq += f->hi[n] * f->z[k];
        k = k == 0 ? TAPS - 1 : k - 1;
    }
    f->pos = (f->pos + 1) % TAPS;
    // The real input's positive-frequency half carries half its amplitude.
    *i = 2 * si;
    *q = 2 * sq;
}

typedef struct {
    double h[TAPS];
    double zi[TAPS], zq[TAPS];
    int pos, phase, factor;
    double nco, step;            // feedback mix, radians
} decim_t;

// Mixes DAX IQ to the transmit frequency and decimates it to 24 kHz.
static void decim_init(decim_t* d, int in_rate, double shift_hz)
{
    memset(d, 0, sizeof *d);
    d->factor = in_rate / RATE;
    if (d->factor < 1)
        d->factor = 1;
    double fc = 10000.0 / in_rate, sum = 0;
    for (int n = 0; n < TAPS; n++) {
        int m = n - (TAPS - 1) / 2;
        double s = m == 0 ? 2 * fc : sin(2 * M_PI * fc * m) / (M_PI * m);
        double w = 0.42 - 0.5 * cos(2 * M_PI * n / (TAPS - 1)) + 0.08 * cos(4 * M_PI * n / (TAPS - 1));
        d->h[n] = s * w;
        sum += d->h[n];
    }
    for (int n = 0; n < TAPS; n++)
        d->h[n] /= sum;
    d->step = -2 * M_PI * shift_hz / in_rate;
}

// Returns 1 and writes one 24 kHz sample when one is due.
static int decim_step(decim_t* d, double i, double q, double* oi, double* oq)
{
    double c = cos(d->nco), s = sin(d->nco);
    d->nco = fmod(d->nco + d->step, 2 * M_PI);
    d->zi[d->pos] = i * c - q * s;
    d->zq[d->pos] = i * s + q * c;
    int k = d->pos;
    d->pos = (d->pos + 1) % TAPS;
    if (++d->phase < d->factor)
        return 0;
    d->phase = 0;
    double si = 0, sq = 0;
    for (int n = 0; n < TAPS; n++) {
        si += d->h[n] * d->zi[k];
        sq += d->h[n] * d->zq[k];
        k = k == 0 ? TAPS - 1 : k - 1;
    }
    *oi = si;
    *oq = sq;
    return 1;
}

typedef struct {
    double* buf;   // interleaved I/Q
    int head, n;
} fifo_t;

static void fifo_push(fifo_t* f, double i, double q)
{
    if (f->n == FIFO_LEN)   // full: drop the oldest; pairing restarts anyway
        f->head = (f->head + 1) % FIFO_LEN, f->n--;
    int w = (f->head + f->n) % FIFO_LEN;
    f->buf[2 * w] = i;
    f->buf[2 * w + 1] = q;
    f->n++;
}

static void fifo_pop(fifo_t* f, double* out, int n)
{
    for (int k = 0; k < n; k++) {
        out[2 * k] = f->buf[2 * f->head];
        out[2 * k + 1] = f->buf[2 * f->head + 1];
        f->head = (f->head + 1) % FIFO_LEN;
    }
    f->n -= n;
}

/* -------------------------------------------------------------- state */

typedef struct {
    int api, udp, seq;
    char rbuf[16384];
    size_t rlen;
    int seq_create, seq_daxiq;
    char daxiq_reply[96];          // last reply to the DAX IQ stream request
    long n_tx_pkts, n_fb_pkts, n_fb_other;   // packets seen, for the status line
    unsigned last_cls, last_sid;   // the last packet that matched no stream
    double slice_mhz[16];          // every slice's frequency, by index
    uint32_t tx_in, rx_in;        // stream IDs from waveform create
    uint32_t daxiq_sid;
    int active_slice;             // slice in PD mode, or -1
    double tx_mhz;                // that slice's frequency
    double pan_mhz;               // centre of the pan feeding DAX IQ
    int keyed;
    int tx_on;                    // interlock PTT_REQUESTED or TRANSMITTING: may send
    long n_tx_held;               // transmit packets not answered (interlock not keyed)
    int tx_count, rx_count;
    // DSP
    pd_core* core;
    pd_align* align;
    cbp_t mod;
    decim_t fb;
    fifo_t ref, fbk;
    double* acq_tx;
    double* acq_rx;
    int acq_n, acq_len, aligned;
    pd_eq* eq;                    // reference -> feedback linear path, per delay lock
    int feedback_ok;              // the DAX IQ pan covers this transmission
    int weak_blocks;              // consecutive blocks of poor coherence
    int settle_left;              // samples still to skip after key-up
    int daxiq_format_logged;
    struct sockaddr_in radio;     // where VITA packets go
    // Selftest only: sees every transmit block the waveform sends.
    void (*tx_tap)(void* ctx, const float* iq, int n);
    void* tap_ctx;
    double peak, gain;
    double test_f1, test_f2;      // AETHER_PD_TEST_TONES, Hz; 0 = use the mic
    long long test_n;             // samples of the test two-tone sent so far
    int test_noise_left;          // samples of noise preamble still to send
    cbp_t noise_bp;               // band-limits the preamble to 300-3000 Hz
    unsigned noise_seed;
    int correct, daxiq_ch, daxiq_rate;
    double last_report;
    FILE* dump;                    // AETHER_PD_DUMP: aligned pairs, for analysis
    long dump_left;                // pairs still to write
} wf_t;

static int api_send(wf_t* w, const char* fmt, ...)
{
    char line[512];
    int l = snprintf(line, sizeof line, "C%d|", ++w->seq);
    va_list ap;
    va_start(ap, fmt);
    l += vsnprintf(line + l, sizeof line - (size_t)l - 1, fmt, ap);
    va_end(ap);
    if (l <= 0 || (size_t)l >= sizeof line - 1)
        return -1;
    line[l++] = '\n';
    if (write(w->api, line, (size_t)l) != l)
        return -1;
    return w->seq;
}

static double mono(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + t.tv_nsec * 1e-9;
}

static void start_feedback(wf_t* w)
{
    double shift = (w->tx_mhz - w->pan_mhz) * 1e6;
    // The correction spans about +-10 kHz around the carrier; that band has
    // to sit inside the DAX IQ stream.
    double room = w->daxiq_rate / 2.0 - 10000;
    w->feedback_ok = w->daxiq_sid && w->pan_mhz > 0 && fabs(shift) <= room;
    if (!w->feedback_ok) {
        logf_("transmit without feedback: put a panadapter on the coupler's input with DAX IQ "
              "channel %d, centred within %.0f Hz of %.6f MHz", w->daxiq_ch, room, w->tx_mhz);
        return;
    }
    decim_init(&w->fb, w->daxiq_rate, shift);
    w->ref.n = w->fbk.n = 0;
    w->acq_n = 0;
    w->aligned = 0;
    pd_eq_reset(w->eq);
    w->weak_blocks = 0;
    w->settle_left = SETTLE_SAMPLES;
    w->test_noise_left = TEST_NOISE_SAMPLES;
    pd_align_restart(w->align);
    logf_("transmit: feedback shift %.0f Hz (tx %.6f MHz, pan %.6f MHz)", shift, w->tx_mhz, w->pan_mhz);
}

static void set_keyed(wf_t* w, int keyed)
{
    if (keyed == w->keyed)
        return;
    w->keyed = keyed;
    if (keyed && w->active_slice >= 0)
        start_feedback(w);
    pd_core_set_keyed(w->core, keyed && w->active_slice >= 0);
    if (!keyed)
        logf_("receive: %d fits, correcting %d, delay %.2f, offset %.3f Hz",
              pd_core_fits(w->core), pd_core_correcting(w->core),
              pd_align_delay(w->align), pd_align_offset_hz(w->align, RATE));
}

/* Handles one line from the radio. Exposed to the selftest. */
static void api_line(wf_t* w, char* l)
{
    char v[64];
    if (l[0] == 'R') {
        int seq = atoi(l + 1);
        const char* p1 = strchr(l, '|');
        if (!p1)
            return;
        unsigned long code = strtoul(p1 + 1, NULL, 16);
        const char* msg = strchr(p1 + 1, '|');
        msg = msg ? msg + 1 : "";
        if (seq == w->seq_create) {
            if (code) {
                logf_("waveform create refused: %08lx %s", code, msg);
                return;
            }
            kv_u32(msg, "tx_stream_in_id", &w->tx_in);
            kv_u32(msg, "rx_stream_in_id", &w->rx_in);
            logf_("registered %s: tx_in 0x%08x rx_in 0x%08x", WF_NAME, w->tx_in, w->rx_in);
        } else if (seq == w->seq_daxiq) {
            snprintf(w->daxiq_reply, sizeof w->daxiq_reply, "%08lx %s", code, msg);
            if (code)
                logf_("DAX IQ stream refused: %08lx %s", code, msg);
            else {
                w->daxiq_sid = (uint32_t)strtoul(msg, NULL, 16);
                api_send(w, "stream set 0x%08X daxiq_rate=%d", w->daxiq_sid, w->daxiq_rate);
                logf_("DAX IQ channel %d stream 0x%08x at %d Hz", w->daxiq_ch, w->daxiq_sid, w->daxiq_rate);
            }
        } else if (code) {
            logf_("command %d: %08lx %s", seq, code, msg);
        }
        return;
    }
    if (l[0] == 'C') {
        // A command for the waveform: "C<seq>|slice <n> <command...>".
        int seq = atoi(l + 1);
        const char* cmd = strchr(l, '|');
        int ok = 0;
        if (cmd && strstr(cmd, " reset")) {
            pd_core_reset(w->core);
            logf_("correction reset on request");
            ok = 1;
        }
        // The SDK answers a command its callback refuses with 0x50000000 + the
        // callback's code; 1 is "not handled".
        api_send(w, ok ? "waveform response %d|0" : "waveform response %d|50000001", seq);
        return;
    }
    if (l[0] != 'S')
        return;
    const char* m = strchr(l, '|');
    if (!m)
        return;
    m++;
    if (!strncmp(m, "slice ", 6)) {
        int idx = atoi(m + 6);
        char mode[16] = "";
        if (kv(m, "mode", mode, sizeof mode)) {
            if (!strcmp(mode, WF_MODE) && w->active_slice < 0) {
                w->active_slice = idx;
                // The slice's frequency usually came in an earlier status.
                if (idx >= 0 && idx < 16 && w->slice_mhz[idx] > 0)
                    w->tx_mhz = w->slice_mhz[idx];
                logf_("PD selected on slice %d", idx);
                // Ask for the feedback stream again if the first request,
                // made at connect, didn't produce one.
                if (!w->daxiq_sid && w->api >= 0)
                    w->seq_daxiq = api_send(w, "stream create type=dax_iq daxiq_channel=%d", w->daxiq_ch);
            } else if (strcmp(mode, WF_MODE) && w->active_slice == idx) {
                logf_("PD released by slice %d", idx);
                w->active_slice = -1;
                w->tx_on = 0;
                set_keyed(w, 0);
            }
        }
        if (kv(m, "RF_frequency", v, sizeof v)) {
            if (idx >= 0 && idx < 16)
                w->slice_mhz[idx] = atof(v);
            if (idx == w->active_slice)
                w->tx_mhz = atof(v);
        }
        return;
    }
    if (!strncmp(m, "display pan ", 12)) {
        if (kv(m, "daxiq_channel", v, sizeof v) && atoi(v) == w->daxiq_ch && kv(m, "center", v, sizeof v))
            w->pan_mhz = atof(v);
        return;
    }
    if (!strncmp(m, "interlock ", 10) && kv(m, "state", v, sizeof v)) {
        w->tx_on = !strcmp(v, "TRANSMITTING") || !strcmp(v, "PTT_REQUESTED");
        set_keyed(w, !strcmp(v, "TRANSMITTING"));
        return;
    }
}

static void send_pairs(wf_t* w, uint32_t sid, int* count, const float* pairs, int n)
{
    unsigned char pkt[28 + 4 * MAX_SAMPLES];
    for (int off = 0; off < n; off += MAX_SAMPLES / 2) {
        int m = n - off < MAX_SAMPLES / 2 ? n - off : MAX_SAMPLES / 2;
        int len = vita_build(pkt, sid, CLASS_AUDIO_24K, (*count)++, pairs + 2 * off, m);
        if (w->udp >= 0)
            sendto(w->udp, pkt, (size_t)len, 0, (struct sockaddr*)&w->radio, sizeof w->radio);
    }
}

// Pairs reference and feedback blocks while transmitting.
static void calibrate(wf_t* w)
{
    double tx[2 * BLOCK], rx[2 * BLOCK], atx[2 * BLOCK], arx[2 * BLOCK], etx[2 * BLOCK], erx[2 * BLOCK];
    while (w->ref.n >= BLOCK && w->fbk.n >= BLOCK) {
        fifo_pop(&w->ref, tx, BLOCK);
        fifo_pop(&w->fbk, rx, BLOCK);
        if (w->settle_left > 0) {
            w->settle_left -= BLOCK;
            continue;
        }
        if (!w->aligned) {
            int m = BLOCK < w->acq_len - w->acq_n ? BLOCK : w->acq_len - w->acq_n;
            memcpy(w->acq_tx + 2 * w->acq_n, tx, sizeof(double) * 2 * (size_t)m);
            memcpy(w->acq_rx + 2 * w->acq_n, rx, sizeof(double) * 2 * (size_t)m);
            w->acq_n += m;
            if (w->acq_n == w->acq_len) {
                double d = pd_align_estimate_delay(w->align, w->acq_tx, w->acq_rx, w->acq_n);
                if (d < 0) {
                    logf_("no delay lock yet (correlation %.2f): speak, a varying envelope finds it",
                          pd_align_estimate_corr(w->align));
                    w->acq_n = 0;
                } else {
                    pd_align_set_delay(w->align, d);
                    pd_align_restart(w->align);
                    w->aligned = 1;
                    w->weak_blocks = 0;
                    logf_("loop delay %.2f samples (%.2f ms), envelope correlation %.2f, phase correlation %.3f",
                          d, d * 1000.0 / RATE, pd_align_estimate_corr(w->align), pd_align_phase_corr(w->align));
                }
            }
            continue;
        }
        int v = pd_align_push(w->align, tx, rx, BLOCK, atx, arx);
        if (w->dump && w->dump_left > 0 && v > 0) {
            // float32 tx_i, tx_q, rx_i, rx_q per pair, native endian
            for (int k = 0; k < v && w->dump_left > 0; k++, w->dump_left--) {
                float f[4] = { (float)atx[2 * k], (float)atx[2 * k + 1], (float)arx[2 * k], (float)arx[2 * k + 1] };
                fwrite(f, sizeof f, 1, w->dump);
            }
            if (w->dump_left == 0)
                fflush(w->dump);
        }
        if (v > 0 && pd_align_coherence(w->align) < WEAK_COHERENCE) {
            if (++w->weak_blocks >= WEAK_BLOCKS) {
                logf_("feedback stopped matching (coherence %.2f): finding the delay again",
                      pd_align_coherence(w->align));
                w->aligned = 0;
                pd_eq_reset(w->eq);
                w->acq_n = 0;
                w->weak_blocks = 0;
                // Measure again on noise, past the samples already in flight.
                w->test_noise_left = TEST_NOISE_SAMPLES;
                w->settle_left = SETTLE_SAMPLES;
                pd_align_restart(w->align);
            }
            continue;   // don't calibrate on pairs that don't match
        }
        w->weak_blocks = 0;
        if (!pd_eq_ready(w->eq)) {
            pd_eq_train(w->eq, atx, arx, v);
            if (pd_eq_trained(w->eq) >= EQ_TRAIN_SAMPLES) {
                if (pd_eq_solve(w->eq)) {
                    logf_("transmit path fitted: %d taps, %.1f dB of the feedback left unexplained",
                          EQ_TAPS, 10 * log10(pd_eq_residual(w->eq) + 1e-30));
                } else {
                    logf_("transmit path fit failed: training again");
                    pd_eq_reset(w->eq);
                }
            }
            continue;
        }
        int m = pd_eq_apply(w->eq, atx, arx, v, etx, erx);
        pd_core_calibrate(w->core, etx, erx, m);
    }
}

static void on_packet(wf_t* w, const unsigned char* p, ssize_t n)
{
    vita_t v;
    if (!vita_parse(p, n, &v))
        return;
    if (v.sid == w->daxiq_sid && w->daxiq_sid) {
        w->n_fb_pkts++;
        if (!(v.cls & CLASS_FLOAT_BIT)) {
            if (!w->daxiq_format_logged)
                logf_("DAX IQ arrives as class 0x%04x, not float samples: feedback off", v.cls);
            w->daxiq_format_logged = 1;
            return;
        }
        if (!w->keyed || w->active_slice < 0 || !w->feedback_ok)
            return;
        for (int k = 0; k + 1 < v.words; k += 2) {
            double oi, oq;
            if (decim_step(&w->fb, word_float_le(v.payload + 4 * k), word_float_le(v.payload + 4 * k + 4), &oi, &oq))
                fifo_push(&w->fbk, oi, oq);
        }
        calibrate(w);
        return;
    }
    if (v.cls != CLASS_AUDIO_24K || w->active_slice < 0) {
        w->n_fb_other++;
        w->last_cls = v.cls;
        w->last_sid = v.sid;
        return;
    }
    int pairs = v.words / 2;
    if (pairs > MAX_SAMPLES)
        pairs = MAX_SAMPLES;
    float out[2 * MAX_SAMPLES];
    if (v.sid & 1u) {
        if (!w->tx_on) {
            w->n_tx_held++;
            return;
        }
        w->n_tx_pkts++;
        // Transmit: the radio's mic stream, while the operator transmits.
        double iq[2 * MAX_SAMPLES];
        for (int k = 0; k < pairs; k++) {
            double i, q;
            if (w->test_f1 > 0 && w->test_noise_left > 0) {
                // Band-limited noise for an unambiguous delay measurement.
                w->test_noise_left--;
                w->noise_seed = w->noise_seed * 1664525u + 1013904223u;
                double u = ((w->noise_seed >> 8) / 16777216.0) - 0.5;
                cbp_step(&w->noise_bp, 1.2 * w->peak * u, &i, &q);
            } else if (w->test_f1 > 0) {
                // Each tone at half the peak, so the envelope peaks at it.
                double t = (double)w->test_n++ / RATE, a = w->peak / 2;
                i = a * (cos(2 * M_PI * w->test_f1 * t) + cos(2 * M_PI * w->test_f2 * t));
                q = a * (sin(2 * M_PI * w->test_f1 * t) + sin(2 * M_PI * w->test_f2 * t));
            } else {
                cbp_step(&w->mod, w->gain * word_float(v.payload + 8 * k), &i, &q);
            }
            double a = hypot(i, q);
            if (a > w->peak) {        // never past the peak the correction knows
                i *= w->peak / a;
                q *= w->peak / a;
            }
            iq[2 * k] = i;
            iq[2 * k + 1] = q;
        }
        if (w->correct)
            pd_core_correct(w->core, iq, pairs);
        for (int k = 0; k < pairs; k++) {
            double i = iq[2 * k], q = iq[2 * k + 1], a = hypot(i, q);
            if (a > 1.0) {
                i /= a;
                q /= a;
            }
            out[2 * k] = (float)i;
            out[2 * k + 1] = (float)q;
            // The reference is what the radio is given, after this clip:
            // that is the amplifier's real input. AETHER_PD_PEAK (0.7)
            // leaves 3 dB for the correction to expand into before it.
            if (w->keyed && w->feedback_ok)
                fifo_push(&w->ref, i, q);
        }
        send_pairs(w, v.sid, &w->tx_count, out, pairs);
        if (w->tx_tap)
            w->tx_tap(w->tap_ctx, out, pairs);
        if (w->keyed && w->feedback_ok)
            calibrate(w);
    } else {
        // Receive: RAW I/Q at the slice. The real part of upper-sideband
        // I/Q is its audio.
        for (int k = 0; k < pairs; k++) {
            float a = word_float(v.payload + 8 * k);
            out[2 * k] = a;
            out[2 * k + 1] = a;
        }
        send_pairs(w, v.sid, &w->rx_count, out, pairs);
    }
}

static int wf_init(wf_t* w)
{
    memset(w, 0, sizeof *w);
    w->active_slice = -1;
    w->udp = -1;
    w->radio.sin_family = AF_INET;
    w->radio.sin_port = htons(VITA_PORT);
    const char* addr = getenv("AETHER_PD_RADIO");
    if (!addr || !*addr)
        addr = getenv("SSDR_RADIO_ADDRESS");
    if (!addr || !*addr)
        addr = "127.0.0.1";
    if (inet_pton(AF_INET, addr, &w->radio.sin_addr) != 1) {
        logf_("bad radio address %s", addr);
        return 0;
    }
    w->peak = env_d("AETHER_PD_PEAK", 0.7);
    w->gain = env_d("AETHER_PD_MIC_GAIN", 1.0);
    const char* dump = getenv("AETHER_PD_DUMP");
    if (dump && *dump) {
        w->dump = fopen(dump, "wb");
        w->dump_left = 10L * RATE;       // ten seconds of aligned pairs
    }
    const char* tones = getenv("AETHER_PD_TEST_TONES");
    if (tones && *tones && sscanf(tones, "%lf,%lf", &w->test_f1, &w->test_f2) != 2) {
        logf_("AETHER_PD_TEST_TONES must be \"f1,f2\" in Hz");
        return 0;
    }
    if (w->test_f1 < 0 || w->test_f2 < 0 || w->test_f1 > 10000 || w->test_f2 > 10000) {
        logf_("AETHER_PD_TEST_TONES out of range");
        return 0;
    }
    w->correct = (int)env_d("AETHER_PD_CORRECT", 1);
    w->daxiq_ch = (int)env_d("AETHER_PD_DAXIQ", 1);
    w->daxiq_rate = (int)env_d("AETHER_PD_DAXIQ_RATE", 48000);
    if (w->daxiq_rate < RATE || w->daxiq_rate % RATE) {
        logf_("AETHER_PD_DAXIQ_RATE must be a multiple of %d", RATE);
        return 0;
    }
    int max_delay = (int)(env_d("AETHER_PD_MAX_DELAY_MS", 400) * RATE / 1000);
    w->core = pd_core_create(RATE, MAX_SAMPLES, w->peak);
    w->align = pd_align_create(max_delay, max_delay + 4 * BLOCK + 512);
    w->eq = pd_eq_create(EQ_TAPS);
    w->ref.buf = calloc(2 * FIFO_LEN, sizeof(double));
    w->fbk.buf = calloc(2 * FIFO_LEN, sizeof(double));
    w->acq_len = max_delay + 8192;
    w->acq_tx = calloc(2 * (size_t)w->acq_len, sizeof(double));
    w->acq_rx = calloc(2 * (size_t)w->acq_len, sizeof(double));
    if (!w->core || !w->align || !w->eq || !w->ref.buf || !w->fbk.buf || !w->acq_tx || !w->acq_rx)
        return 0;
    cbp_init(&w->mod, 200, 2800);
    cbp_init(&w->noise_bp, 300, 3000);
    w->noise_seed = 12345;
    decim_init(&w->fb, w->daxiq_rate, 0);
    return 1;
}


/* ------------------------------------------------------------- status */

static int lan_caller(const struct sockaddr_in* a)
{
    unsigned long ip = ntohl(a->sin_addr.s_addr);
    return (ip >> 24) == 10 || (ip >> 24) == 127
        || (ip >> 20) == (172UL << 4 | 1)          // 172.16.0.0/12
        || (ip >> 16) == (192UL << 8 | 168)        // 192.168.0.0/16
        || (ip >> 16) == (169UL << 8 | 254);       // 169.254.0.0/16
}

// What the operator needs to see during a transmit test.
static int status_json(const wf_t* w, char* out, size_t cap)
{
    return snprintf(out, cap,
        "{\"name\":\"%s\",\"version\":\"%s\",\"registered\":%s,\"active_slice\":%d,"
        "\"tx_mhz\":%.6f,\"pan_mhz\":%.6f,\"daxiq_stream\":\"0x%08x\",\"daxiq_reply\":\"%s\",\"keyed\":%s,"
        "\"feedback_ok\":%s,\"correction\":%s,\"aligned\":%s,\"delay_samples\":%.2f,"
        "\"delay_corr\":%.3f,\"coherence\":%.3f,\"offset_hz\":%.4f,\"fits\":%d,"
        "\"correcting\":%s,\"calc_state\":%d,\"tx_pkts\":%ld,\"tx_held\":%ld,\"fb_pkts\":%ld,"
        "\"other_pkts\":%ld,\"other_last\":\"sid 0x%08x cls 0x%04x\",\"ref_fifo\":%d,"
        "\"fb_fifo\":%d,\"acq\":%d,\"eq_ready\":%s,\"eq_residual_db\":%.1f}\n",
        WF_NAME, WF_VERSION, w->tx_in ? "true" : "false", w->active_slice, w->tx_mhz, w->pan_mhz,
        w->daxiq_sid, w->daxiq_reply, w->keyed ? "true" : "false", w->feedback_ok ? "true" : "false",
        w->correct ? "true" : "false", w->aligned ? "true" : "false",
        pd_align_delay(w->align), pd_align_estimate_corr(w->align), pd_align_coherence(w->align),
        pd_align_offset_hz(w->align, RATE), pd_core_fits(w->core),
        pd_core_correcting(w->core) ? "true" : "false", pd_core_state(w->core),
        w->n_tx_pkts, w->n_tx_held, w->n_fb_pkts, w->n_fb_other, w->last_sid, w->last_cls, w->ref.n, w->fbk.n, w->acq_n,
        pd_eq_ready(w->eq) ? "true" : "false", 10 * log10(pd_eq_residual(w->eq) + 1e-30));
}

static void serve_status(const wf_t* w, int ls)
{
    struct sockaddr_in a;
    socklen_t al = sizeof a;
    int fd = accept(ls, (struct sockaddr*)&a, &al);
    if (fd < 0)
        return;
    if (lan_caller(&a)) {
        char body[1024];
        int n = status_json(w, body, sizeof body);
        if (n > 0 && (size_t)n < sizeof body && write(fd, body, (size_t)n) != n)
            logf_("status write short");
    }
    close(fd);
}

/* ----------------------------------------------------------- selftest */

static int selftest_loop(void);

static int selftest(void)
{
    int fails = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
    wf_t w;
    CHECK(wf_init(&w));
    // Commands the waveform sends while handling these lines go nowhere.
    w.api = open("/dev/null", O_WRONLY);
    CHECK(w.api >= 0);

    // Registration reply carries the stream IDs.
    w.seq_create = 3;
    char r1[] = "R3|0|tx_stream_in_id=0x84000001 rx_stream_in_id=0x84000000 tx_stream_out_id=0x8400000B";
    api_line(&w, r1);
    CHECK(w.tx_in == 0x84000001u && w.rx_in == 0x84000000u);

    // Selecting PD on a slice, its frequency, the pan, and the interlock.
    char s0[] = "S1|slice 1 in_use=1 mode=USB RF_frequency=14.200000 rxant=ANT1";
    api_line(&w, s0);
    CHECK(w.active_slice == -1 && w.tx_mhz == 0);
    char s1[] = "S1|slice 1 mode=PD";
    api_line(&w, s1);
    CHECK(w.active_slice == 1 && fabs(w.tx_mhz - 14.2) < 1e-9);
    char s2[] = "S1|display pan 0x40000001 center=14.201000 daxiq_channel=1 rxant=RX_A";
    api_line(&w, s2);
    CHECK(fabs(w.pan_mhz - 14.201) < 1e-9);
    char s3[] = "S1|interlock state=TRANSMITTING source=SW";
    api_line(&w, s3);
    CHECK(w.keyed == 1 && !w.feedback_ok);   // no DAX IQ stream yet
    char s3b[] = "S1|interlock state=READY";
    api_line(&w, s3b);
    w.seq_daxiq = 4;
    char r2[] = "R4|0|0x20000001";
    api_line(&w, r2);
    CHECK(w.daxiq_sid == 0x20000001u);
    api_line(&w, s3);
    CHECK(w.keyed == 1 && w.feedback_ok);
    // A pan too far from the carrier for the stream is refused.
    char s3c[] = "S1|interlock state=READY";
    api_line(&w, s3c);
    char s2far[] = "S1|display pan 0x40000001 center=14.250000 daxiq_channel=1";
    api_line(&w, s2far);
    api_line(&w, s3);
    CHECK(w.keyed == 1 && !w.feedback_ok);
    api_line(&w, s2);
    char s4[] = "S1|interlock state=READY";
    api_line(&w, s4);
    CHECK(w.keyed == 0);
    // Another slice changing mode doesn't release ours; ours does.
    char s5[] = "S1|slice 0 mode=USB";
    api_line(&w, s5);
    CHECK(w.active_slice == 1);
    char s6[] = "S1|slice 1 mode=USB";
    api_line(&w, s6);
    CHECK(w.active_slice == -1);
    // A frequency-like key inside another word must not match.
    char s7[] = "S1|slice 1 mode=PD xRF_frequency=1.0";
    api_line(&w, s7);
    CHECK(w.tx_mhz == 14.2);

    // VITA: build then parse round-trips stream, class, count and samples.
    float pairs[6] = { 0.5f, -0.25f, 1.0f, 0.0f, -1.0f, 0.125f };
    unsigned char pkt[28 + 4 * 6];
    int len = vita_build(pkt, 0x84000001u, CLASS_AUDIO_24K, 5, pairs, 3);
    vita_t v;
    CHECK(len == 28 + 24 && vita_parse(pkt, len, &v));
    CHECK(v.sid == 0x84000001u && v.cls == CLASS_AUDIO_24K && v.count == 5 && v.words == 6);
    CHECK(word_float(v.payload + 4) == -0.25f && word_float(v.payload + 20) == 0.125f);
    pkt[8] ^= 1;   // wrong OUI
    CHECK(!vita_parse(pkt, len, &v));

    // The modulator puts a 1 kHz tone at +1 kHz (upper sideband), and
    // nothing at -1 kHz.
    double pr = 0, pi = 0, nr = 0, ni = 0;
    for (int k = 0; k < 4 * RATE / 10; k++) {
        double i, q, ph = 2 * M_PI * 1000.0 * k / RATE;
        cbp_step(&w.mod, cos(ph), &i, &q);
        if (k < TAPS)
            continue;
        pr += i * cos(ph) + q * sin(ph);
        pi += q * cos(ph) - i * sin(ph);
        nr += i * cos(ph) - q * sin(ph);
        ni += q * cos(ph) + i * sin(ph);
    }
    double usb = hypot(pr, pi), lsb = hypot(nr, ni);
    CHECK(usb > 0 && 20 * log10(lsb / usb) < -50);

    // The feedback mixer moves a tone at +1 kHz above the pan centre to
    // 0 Hz when the transmit frequency is 1 kHz above it.
    decim_t d;
    decim_init(&d, 48000, 1000);
    double dc_r = 0, dc_i = 0, pw = 0;
    int got = 0;
    for (int k = 0; k < 48000; k++) {
        double ph = 2 * M_PI * 1000.0 * k / 48000, oi, oq;
        if (decim_step(&d, cos(ph), sin(ph), &oi, &oq) && k > 4 * TAPS) {
            dc_r += oi;
            dc_i += oq;
            pw += oi * oi + oq * oq;
            got++;
        }
    }
    CHECK(got > 0 && hypot(dc_r, dc_i) / got > 0.95 && pw / got > 0.9);

    // Status: valid-looking JSON with the fields a test session reads.
    char js[1024];
    int jn = status_json(&w, js, sizeof js);
    CHECK(jn > 0 && (size_t)jn < sizeof js && js[0] == '{' && js[jn - 2] == '}');
    CHECK(strstr(js, "\"registered\":true") && strstr(js, "\"daxiq_stream\":\"0x20000001\"")
          && strstr(js, "\"delay_samples\":") && strstr(js, "\"coherence\":"));
    struct sockaddr_in lan = { .sin_family = AF_INET }, wan = { .sin_family = AF_INET };
    inet_pton(AF_INET, "192.168.50.20", &lan.sin_addr);
    inet_pton(AF_INET, "100.109.252.61", &wan.sin_addr);
    CHECK(lan_caller(&lan) && !lan_caller(&wan));

    close(w.api);
    pd_core_destroy(w.core);
    w.core = NULL;
    fails += selftest_loop();
    printf(fails ? "selftest FAILED (%d)\n" : "selftest passed\n", fails);
    return fails ? 1 : 0;
#undef CHECK
}


/* The whole transmit loop through on_packet: mic packets in, the waveform's
 * I/Q through an amplifier model, back as 48 kHz DAX IQ packets with a loop
 * delay and the pan 1 kHz above the carrier. The waveform has to find the
 * delay, keep the pairs coherent, and complete fits. */
#define LOOP_DELAY 1234        // samples at 48 kHz on the DAX IQ side
typedef struct {
    wf_t* w;
    double* line;              // delayed PA output at 48 kHz, interleaved
    int pos, len, count;
    double nco, last_i, last_q;
    float pkt[2 * 128];
    int npkt;
} loop_t;

static void loop_emit(loop_t* l, double i, double q)
{
    // delay, then shift by -1 kHz: the pan is centred 1 kHz above the carrier
    int r = l->pos;
    double di = l->line[2 * r], dq = l->line[2 * r + 1];
    l->line[2 * r] = i;
    l->line[2 * r + 1] = q;
    l->pos = (l->pos + 1) % l->len;
    double c = cos(l->nco), s = sin(l->nco);
    l->nco = fmod(l->nco - 2 * M_PI * 1000.0 / 48000, 2 * M_PI);
    l->pkt[2 * l->npkt] = (float)(0.6 * (di * c - dq * s));
    l->pkt[2 * l->npkt + 1] = (float)(0.6 * (di * s + dq * c));
    if (++l->npkt == 128) {
        unsigned char p[28 + 4 * 256];
        int len = vita_build(p, l->w->daxiq_sid, 0x02E4u, l->count++, l->pkt, 128);
        // The radio sends DAX IQ samples little-endian: swap each word.
        for (int o = 28; o < len; o += 4) {
            unsigned char t0 = p[o], t1 = p[o + 1];
            p[o] = p[o + 3];
            p[o + 1] = p[o + 2];
            p[o + 2] = t1;
            p[o + 3] = t0;
        }
        on_packet(l->w, p, len);
        l->npkt = 0;
    }
}

static void loop_tap(void* ctx, const float* iq, int n)
{
    loop_t* l = ctx;
    for (int k = 0; k < n; k++) {
        double i = iq[2 * k], q = iq[2 * k + 1], a = hypot(i, q);
        double g = a > 0 ? 1.0 / pow(1.0 + pow(a, 4), 0.25) : 1;   // Rapp p=2
        double ph = 0.25 * a * a / (1 + a * a);                      // AM/PM
        double yi = g * (i * cos(ph) - q * sin(ph)), yq = g * (i * sin(ph) + q * cos(ph));
        // to 48 kHz by linear interpolation
        loop_emit(l, 0.5 * (l->last_i + yi), 0.5 * (l->last_q + yq));
        loop_emit(l, yi, yq);
        l->last_i = yi;
        l->last_q = yq;
    }
}

static int tapped;
static void count_tap(void* ctx, const float* iq, int n)
{
    (void)iq;
    *(int*)ctx += n;
}

static int selftest_loop(void)
{
    int fails = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
    wf_t w;
    CHECK(wf_init(&w));
    w.api = open("/dev/null", O_WRONLY);
    loop_t l = { .w = &w, .len = LOOP_DELAY };
    l.line = calloc(2 * LOOP_DELAY, sizeof(double));
    w.tx_tap = loop_tap;
    w.tap_ctx = &l;
    w.seq_create = 3;
    w.seq_daxiq = 4;
    char r1[] = "R3|0|tx_stream_in_id=0x84000001 rx_stream_in_id=0x84000000";
    char r2[] = "R4|0|0x20000001";
    char s1[] = "S1|slice 1 mode=PD RF_frequency=14.200000";
    char s2[] = "S1|display pan 0x40000001 center=14.201000 daxiq_channel=1";
    char s3[] = "S1|interlock state=TRANSMITTING";
    api_line(&w, r1);
    api_line(&w, r2);
    api_line(&w, s1);
    api_line(&w, s2);
    api_line(&w, s3);
    CHECK(w.feedback_ok);

    unsigned seed = 7;
    double lp = 0, env = 0.5;
    float mic[2 * 128];
    unsigned char p[28 + 4 * 256];
    int count = 0;
    struct timespec t0, now;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (long k = 0; k < 24000L * 30; k += 128) {
        for (int j = 0; j < 128; j++) {
            // Speech-like: band noise with a slowly wandering envelope.
            seed = seed * 1664525u + 1013904223u;
            double u = ((seed >> 8) / 16777216.0) - 0.5;
            lp = 0.7 * lp + 0.3 * u;
            if (((k + j) % 2400) == 0) {
                seed = seed * 1664525u + 1013904223u;
                env = 0.2 + 0.8 * ((seed >> 8) / 16777216.0);
            }
            mic[2 * j] = mic[2 * j + 1] = (float)(4.0 * env * lp);
        }
        int len = vita_build(p, 0x84000001u, CLASS_AUDIO_24K, count++, mic, 128);
        on_packet(&w, p, len);
        // Let a running fit finish, as real time would.
        for (int wait = 0; wait < 2000 && pd_core_state(w.core) == 6 && !pd_core_swap_pending(w.core); wait++) {
            extern void pscc(int, int, double*, double*);
            double none[2] = { 0, 0 };
            pscc(0, 0, none, none);
            struct timespec d = { 0, 500000 };
            nanosleep(&d, NULL);
        }
        if (pd_core_fits(w.core) >= 3)
            break;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec - t0.tv_sec > 40)
            break;
    }
    // 1234 samples at 48 kHz, plus the feedback decimator's half length,
    // seen at 24 kHz.
    double want = LOOP_DELAY / 2.0 + (TAPS - 1) / 4.0;
    CHECK(w.aligned);
    CHECK(fabs(pd_align_delay(w.align) - want) < 2.0);
    CHECK(pd_align_estimate_corr(w.align) > 0.8);
    CHECK(pd_align_coherence(w.align) > 0.8);
    CHECK(pd_core_fits(w.core) >= 1);
    printf("loop: delay %.2f (want ~%.2f), correlation %.2f, coherence %.2f, fits %d, correcting %d\n",
           pd_align_delay(w.align), want, pd_align_estimate_corr(w.align), pd_align_coherence(w.align),
           pd_core_fits(w.core), pd_core_correcting(w.core));

    // Transmit output stops at UNKEY_REQUESTED and stays off until the
    // interlock asks for transmit again, test tones or not.
    w.test_f1 = 710;
    w.test_f2 = 1900;
    w.tx_tap = count_tap;
    w.tap_ctx = &tapped;
    float quiet[2 * 128] = { 0 };
    const char* states[] = { "UNKEY_REQUESTED", "READY", "PTT_REQUESTED", "TRANSMITTING", "UNKEY_REQUESTED" };
    const int sends[] = { 0, 0, 1, 1, 0 };
    for (int k = 0; k < 5; k++) {
        char line[64];
        snprintf(line, sizeof line, "S1|interlock state=%s", states[k]);
        api_line(&w, line);
        tapped = 0;
        int len = vita_build(p, 0x84000001u, CLASS_AUDIO_24K, count++, quiet, 128);
        on_packet(&w, p, len);
        CHECK((tapped > 0) == sends[k]);
    }
    close(w.api);
    free(l.line);
    return fails;
#undef CHECK
}

/* --------------------------------------------------------------- main */

static int connect_api(wf_t* w, const char* addr, int udp_port)
{
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(4992) };
    inet_pton(AF_INET, addr, &sa.sin_addr);
    w->api = socket(AF_INET, SOCK_STREAM, 0);
    if (w->api < 0 || connect(w->api, (struct sockaddr*)&sa, sizeof sa)) {
        if (w->api >= 0)
            close(w->api);
        w->api = -1;
        return 0;
    }
    int one = 1;
    setsockopt(w->api, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    w->rlen = 0;
    api_send(w, "client program %s", WF_NAME);
    api_send(w, "client udpport %d", udp_port);
    api_send(w, "sub slice all");
    api_send(w, "sub pan all");
    api_send(w, "sub radio all");
    api_send(w, "sub tx all");
    api_send(w, "sub daxiq all");
    w->seq_create = api_send(w, "waveform create name=%s mode=%s underlying_mode=RAW version=%s",
                             WF_NAME, WF_MODE, WF_VERSION);
    api_send(w, "waveform set %s tx=1", WF_NAME);
    api_send(w, "waveform set %s udpport=%d", WF_NAME, udp_port);
    w->seq_daxiq = api_send(w, "stream create type=dax_iq daxiq_channel=%d", w->daxiq_ch);
    return 1;
}

int main(int argc, char** argv)
{
    if (argc >= 2 && !strcmp(argv[1], "selftest"))
        return selftest();

    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);
    signal(SIGPIPE, SIG_IGN);
    wf_t w;
    if (!wf_init(&w)) {
        logf_("setup failed");
        return 1;
    }
    const char* addr = getenv("AETHER_PD_RADIO");
    if (!addr || !*addr)
        addr = getenv("SSDR_RADIO_ADDRESS");
    if (!addr || !*addr)
        addr = "127.0.0.1";

    w.udp = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in ua = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_ANY) };
    bind(w.udp, (struct sockaddr*)&ua, sizeof ua);
    socklen_t ual = sizeof ua;
    getsockname(w.udp, (struct sockaddr*)&ua, &ual);
    int udp_port = ntohs(ua.sin_port);
    logf_("%s %s starting, radio %s, udp %d, DAX IQ %d at %d Hz, peak %.2f, correction %s",
          WF_NAME, WF_VERSION, addr, udp_port, w.daxiq_ch, w.daxiq_rate, w.peak, w.correct ? "on" : "off");

    int ls = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in la = { .sin_family = AF_INET, .sin_port = htons(STATUS_PORT), .sin_addr.s_addr = htonl(INADDR_ANY) };
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (ls < 0 || bind(ls, (struct sockaddr*)&la, sizeof la) || listen(ls, 4)) {
        logf_("status port %d unavailable: %s", STATUS_PORT, strerror(errno));
        if (ls >= 0)
            close(ls);
        ls = -1;
    }

    double next_try = 0;
    w.api = -1;
    while (!g_stop) {
        if (w.api < 0 && mono() >= next_try) {
            if (!connect_api(&w, addr, udp_port)) {
                next_try = mono() + 5;
                logf_("radio not answering at %s; retrying", addr);
            }
        }
        struct pollfd pf[3] = { { w.udp, POLLIN, 0 }, { ls, POLLIN, 0 }, { w.api, POLLIN, 0 } };
        int np = w.api >= 0 ? 3 : 2;
        if (poll(pf, (nfds_t)np, 200) < 0 && errno != EINTR)
            break;
        if (ls >= 0 && (pf[1].revents & POLLIN))
            serve_status(&w, ls);
        if (pf[0].revents & POLLIN) {
            unsigned char p[2048];
            ssize_t n;
            while ((n = recv(w.udp, p, sizeof p, MSG_DONTWAIT)) > 0)
                on_packet(&w, p, n);
        }
        if (np == 3 && (pf[2].revents & (POLLIN | POLLHUP | POLLERR))) {
            ssize_t n = read(w.api, w.rbuf + w.rlen, sizeof w.rbuf - w.rlen - 1);
            if (n <= 0) {
                logf_("radio closed the API connection");
                close(w.api);
                w.api = -1;
                set_keyed(&w, 0);
                w.active_slice = -1;
                next_try = mono() + 2;
                continue;
            }
            w.rlen += (size_t)n;
            w.rbuf[w.rlen] = 0;
            char* s = w.rbuf;
            char* e;
            while ((e = strchr(s, '\n'))) {
                *e = 0;
                if (e > s && e[-1] == '\r')
                    e[-1] = 0;
                api_line(&w, s);
                s = e + 1;
            }
            w.rlen = strlen(s);
            memmove(w.rbuf, s, w.rlen);
            if (w.rlen >= sizeof w.rbuf - 1)
                w.rlen = 0;   // a line longer than the buffer: drop it
        }
        if (w.keyed && mono() - w.last_report > 2) {
            w.last_report = mono();
            logf_("tx: aligned %d, fits %d, correcting %d, state %d, offset %.3f Hz",
                  w.aligned, pd_core_fits(w.core), pd_core_correcting(w.core),
                  pd_core_state(w.core), pd_align_offset_hz(w.align, RATE));
        }
    }
    logf_("stopping");
    if (w.api >= 0)
        close(w.api);   // the radio drops the waveform with its client
    if (ls >= 0)
        close(ls);
    pd_core_destroy(w.core);
    pd_align_destroy(w.align);
    pd_eq_destroy(w.eq);
    return 0;
}
