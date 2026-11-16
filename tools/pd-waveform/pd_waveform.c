// AetherPD: a PureSignal-style predistortion waveform for FlexRadio radios.
//
// It registers a waveform mode "PD" with the RAW underlying mode, so the
// radio hands it I/Q and takes I/Q back at 24 kHz. With PD selected on a
// slice:
//   receive   RAW I/Q -> USB demodulation -> speaker audio
//   transmit  mic audio -> USB analytic signal -> pd_core_correct -> I/Q
//   feedback  from an external coupler on the transmit side's receive input
//             (RX_A for ANT1), during TX, by one of two routes:
//             audio (default): a receive-only slice the waveform creates,
//               FB_OFFSET_HZ below the carrier, DIGU, AGC off, 0-10 kHz,
//               audio muted, on a DAX channel -> its DAX audio -> analytic
//               signal -> shifted to the carrier. This works for a waveform
//               on the radio itself; DAX IQ doesn't (FLEX-8600: the radio
//               can't resolve its own MAC for it), and a waveform-mode
//               slice's own receive stream stops while transmitting.
//             iq: DAX IQ from a panadapter on that input (LAN host only)
//               -> mixed to the transmit frequency, decimated to 24 kHz
//             -> pd_align -> pd_eq -> pd_core_calibrate
//
// It never keys the radio: it sends transmit samples only in answer to the
// radio's own transmit stream, and only while the interlock is PTT_REQUESTED
// or TRANSMITTING. At UNKEY_REQUESTED it goes silent at once, as the
// waveform-sdk example does. On a FLEX-8600 (2026-11-11), a waveform that
// kept answering held the interlock in UNKEY_REQUESTED for 28 to 531 s after
// each unkey, with the test two-tone still on the air, until the slice left
// PD.
//
// Protocol: the waveform API as FlexRadio's public waveform-sdk uses it
// (waveform create / set, slice and interlock status, VITA-49 float
// packets to UDP 4991, replies on the incoming stream IDs), and FlexLib
// for the DAX IQ stream (Radio.cs "stream create type=dax_iq",
// DAXIQStream.cs "stream set ... daxiq_rate=").
//
// Settings, from the environment (launch.sh sets them on the radio):
//   AETHER_PD_RADIO        radio address (default SSDR_RADIO_ADDRESS, else 127.0.0.1)
//   AETHER_PD_FEEDBACK     audio (default) or iq
//   AETHER_PD_DAX          DAX audio channel for the feedback slice (default 4)
//   AETHER_PD_FB_ANT       the feedback slice's receive input (default RX_A)
//   AETHER_PD_DAXIQ        DAX IQ channel of the coupler's panadapter, iq route (default 1)
//   AETHER_PD_DAXIQ_RATE   its rate, a multiple of 24000 (default 48000)
//   AETHER_PD_PEAK         transmit peak amplitude before correction (default 0.7)
//   AETHER_PD_MIC_GAIN     mic gain (default 1.0)
//   AETHER_PD_MAX_DELAY_MS longest loop delay to search (default 400)
//   AETHER_PD_CORRECT      0 runs the waveform with the correction off (default 1)
//   AETHER_PD_RAW          file: write the first 15 s of reference/feedback as
//                          paired, before alignment (float32 tx_i tx_q rx_i rx_q)
//   AETHER_PD_DUMP         file: write the first 10 s of aligned reference/feedback
//                          pairs (float32 tx_i tx_q rx_i rx_q), for analysis
//   AETHER_PD_TEST_SECONDS optional .25..15s one-shot test, then 5ms fade to mic
//   AETHER_PD_TEST_TONES   "f1,f2" in Hz: while keyed, send an equal two-tone
//                          at f1 and f2 above the carrier instead of the mic,
//                          at AETHER_PD_PEAK (a bench test signal)
//
// Status: one JSON line per TCP connection on port 48996, LAN callers only
// (like AetherHealth on 48994). Read-only.
//
//   pd_waveform            run
//   pd_waveform selftest   synthetic parsing/DSP checks; private local sockets only
#include "pd_align.h"
#include "pd_acquire.h"
#include "pd_local_iq.h"
#include "pd_tx_guard.h"
#include "pd_udp_rx.h"
#include <sys/resource.h>
#include <sched.h>
#include <pthread.h>
#include <sys/prctl.h>
#include "pd_core.h"
#include "pd_core_fence.h"
#include "pd_eq.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include "pd_owner.h"
#include "pd_status.h"
#include "pd_capture.h"
#include "pd_start_gate.h"
#include "pd_calibration.h"
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
// transmission, and each re-acquisition, opens with this much noise instead:
// long enough to cover SETTLE_SAMPLES, an acquisition at the longest delay,
// and EQ_TRAIN_SAMPLES of transmit-path fit after it.
#define TEST_NOISE_SAMPLES (4 * RATE)
// The preamble spans the correction's band, IMD3 to IMD7 of the test
// two-tone (-2860 to +5470 Hz): the transmit-path fit is only right where it
// was excited, and the correction puts its energy there. A 300-3000 Hz
// preamble left the fit wrong there; fits after the first then drifted IMD3
// back from -50 to -42 dBc.
#define TEST_NOISE_LO_HZ (-4500)
#define TEST_NOISE_HI_HZ 7500
// Noise amplitude: the 300-3000 Hz preamble's RMS (about 0.16 at a 0.7 peak)
// over the wider band.
#define TEST_NOISE_GAIN 0.57
// The radio filters the waveform's I/Q before its amplifier, so the
// reference is not the amplifier's input; WDSP then sees the low-amplitude
// pairs scatter in phase and rejects every fit (FLEX-8600: sin^2+cos^2 off by
// 0.06-0.07 against a 0.05 limit). After each delay lock the first
// EQ_TRAIN_SAMPLES aligned pairs fit an EQ_TAPS FIR from reference to
// feedback; calibration then sees the reference through it. On the bench
// capture that took the phase spread below x = 0.1 from 39 to 6 degrees and
// every fit was accepted.
#define EQ_TAPS 31
// The audio route's feedback slice sits this far below the carrier, so its
// 0..FB_AUDIO_HI_HZ audio covers -3.5 to +6.5 kHz around the carrier: the
// test two-tone's IMD3 to IMD7. The radio refuses a DIGU filter above 12 kHz,
// and its DAX audio measured 0-10.3 kHz at that setting.
#define FB_OFFSET_HZ 3500.0
#define FB_AUDIO_LO_HZ 300
#define FB_AUDIO_HI_HZ 10000
#define EQ_TRAIN_SAMPLES RATE
// Block coherence ignores a constant phase error. Let the phase loop settle
// before fitting one FIR across blocks with different residual phase.
#define EQ_PHASE_SETTLE_SAMPLES (RATE / 2)

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

static int calibration_field(const char* body,const char* key,char* value,size_t capacity)
{
    int found=0;size_t length=strlen(key);
    while (*body) {
        while (*body==' ') { ++body; } if (!*body) { break; }
        const char* end=strchr(body,' ');if (!end) { end=body+strlen(body); }
        size_t count=(size_t)(end-body);
        if (count>length && !memcmp(body,key,length) && body[length]=='=') {
            size_t size=count-length-1;if (found || size>=capacity) { return -1; }
            memcpy(value,body+length+1,size);value[size]=0;found=1;
        }
        body=end;
    }
    return found;
}
static int calibration_handle(const char* text,uint32_t* result)
{
    if (!text || strlen(text)!=10 || text[0]!='0' || text[1]!='x') { return 0; }
    uint32_t value=0;
    for (unsigned i=2;i<10;++i) {
        unsigned c=(unsigned char)text[i];
        if (c>='0' && c<='9') { c-='0'; }
        else if (c>='a' && c<='f') { c=c-'a'+10; }
        else if (c>='A' && c<='F') { c=c-'A'+10; }
        else { return 0; }
        value=(value<<4)|c;
    }
    *result=value;return 1;
}

static double parse_mhz(const char* value)
{
    char* end;
    double mhz = strtod(value, &end);
    return end != value && !*end && isfinite(mhz) && mhz > 0 && mhz <= 1000000 ? mhz : 0;
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
    // Type 1 (IF data with stream ID) for the waveform's own streams and DAX
    // IQ; type 3 (extension data with stream ID) for DAX audio (measured on a
    // FLEX-8600, SmartSDR 4.3.16: header 0x385n0107, class 0x534c03e3).
    if (((w0 >> 28) != 1 && (w0 >> 28) != 3) || !(w0 & 0x08000000u))
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
    int api_leased, api_inherited_used, api_lease_failed;
    char rbuf[16384];
    size_t rlen;
    int seq_create, seq_daxiq;
    char daxiq_reply[96];          // last reply to the DAX IQ stream request
    long n_tx_pkts, n_fb_pkts, n_fb_other;   // packets seen, for the status line
    unsigned last_cls, last_sid;   // the last packet that matched no stream
    double slice_mhz[16];          // every slice's frequency, by index
    uint32_t tx_in, rx_in;        // stream IDs from waveform create
    uint32_t daxiq_sid;
    uint32_t feedback_pan;
    uint32_t sealed_feedback_pan, feedback_pan_owner;
    int feedback_binding_failed;
    char model_fields[13][64];
    int native_apd_enabled, native_apd_active;
    struct pd_tx_guard tx_guard;
    uint64_t reference_generation;
    int64_t tx_arrival_ns, tx_age_ns;
    uint64_t tx_rejected, correction_resets, correction_recoveries;
    int active_slice;             // slice in PD mode, or -1
    double tx_mhz;                // that slice's frequency
    double pan_mhz;               // centre of the pan feeding DAX IQ
    int keyed;
    int measurement_key;
    double mic_peak, prelimit_peak;
    uint64_t limited_samples, generated_samples;
    int tx_on;                    // interlock PTT_REQUESTED or TRANSMITTING: may send
    long n_tx_held;               // transmit packets not answered (interlock not keyed)
    int tx_count, rx_count;
    // DSP
    pd_core* core;
    pd_align* align;
    pd_acquire* acquire;
    uint64_t acquisition_generation;
    int acquisition_pending;
    int eq_pending, eq_n, eq_train_len, eq_settle_left;
    double eq_energy, reference_floor;
    uint64_t quiet_blocks;
    cbp_t mod;
    decim_t fb;
    fifo_t ref, fbk;
    double* acq_tx;
    double* acq_rx;
    int acq_n, acq_len, aligned;
    pd_eq* trusted_eq;
    int transport_cache_enabled, transport_pending;
    int trusted_eq_fits;
    int64_t trusted_fit_ns;
    uint64_t transport_recoveries;
    double transport_eq_distance;
    pd_eq* eq;                    // reference -> feedback linear path, per delay lock
    int feedback_ok;              // the feedback covers this transmission
    // Audio route: our own receive-only feedback slice and its DAX audio.
    struct pd_tx_trace_row* tx_trace;
    FILE* tx_trace_file;
    unsigned tx_trace_rows, tx_trace_dropped;
    int64_t tx_handler_start_ns, tx_last_send_ns, tx_max_send_gap_ns, tx_max_work_ns;
    uint64_t tx_send_errors, tx_sent_datagrams, tx_deadline_faults;
    int64_t tx_deadline_ns, key_started_ns;
    int tx_deadline_in_packet;
    int local_iq_mode;
    int local_iq_fault;
    int warm_start_armed, warm_started;
    uint64_t key_iq_frames, warm_starts, cached_packets;
    struct pd_local_iq local_iq;
    struct pd_capture capture;
    int64_t local_min_age_ns, local_age_sum_ns;
    uint64_t local_age_bins[256];
    int fb_audio;                 // 1 = audio route, 0 = DAX IQ
    int dax_ch;                   // DAX audio channel for it
    char fb_ant[16];
    int fb_slice;                 // its index, or -1
    int seq_fbslice, seq_daxrx;
    uint32_t fb_dax_sid;          // its dax_rx stream
    double fb_mhz;                // its frequency, from its status
    cbp_t fb_bp;                  // real audio -> analytic, FB_AUDIO_LO..HI
    double fb_nco;                // shifts that down by FB_OFFSET_HZ
    int weak_blocks;              // consecutive blocks of poor coherence
    int settle_left;              // samples still to skip after key-up
    int daxiq_format_logged;
    struct sockaddr_in radio;     // where VITA packets go
    // Selftest only: sees every transmit block the waveform sends.
    void (*tx_tap)(void* ctx, const float* iq, int n);
    void* tap_ctx;
    double peak, gain;
    double test_f1, test_f2;      // Hz; retirement clears both frequencies
    long long test_n;             // samples of the test two-tone sent so far
    int test_noise_left;          // samples of noise preamble still to send
    int test_duration_samples, test_left, test_started;
    struct pd_calibration calibration;
    int64_t measurement_tone_sent_ns, measurement_guard_ns;
    int calibration_control, calibration_idle, calibration_software_owner;
    uint32_t calibration_gui, calibration_slice_owner[16];
    cbp_t noise_bp;               // band-limits the preamble to the correction band
    unsigned noise_seed;
    int correct, daxiq_ch, daxiq_rate;
    double last_report;
    FILE* dump;                    // AETHER_PD_DUMP: aligned pairs, for analysis
    long dump_left;                // pairs still to write
    FILE* raw;                     // AETHER_PD_RAW: pairs before alignment
    long raw_left;
} wf_t;

static int test_source_active(const wf_t* w)
{
    // A configured measurement can put the lower tone at or below zero Hz.
    // Its upper tone stays positive; retirement clears both frequencies.
    return w->test_f1 > 0 || (w->calibration.purpose == PD_SOURCE_MEASUREMENT &&
        w->calibration.source_id && w->test_f2 > 0);
}

struct pd_tx_trace_row {
    int64_t arrival_ns, age_ns, handler_start_ns, send_before_ns, send_after_ns, key_started_ns;
    uint64_t input_packets, iq_frames, model_resets, cached_packets;
    uint32_t sid, count, samples;
    int result, error, aligned, fault, warm_started;
    double coherence, delay;
};

// Snapshot continuity before invalidation clears alignment and reference state.
// Session maxima include earlier keys; they are context, not event latency.
struct alignment_evidence { char line[1024]; };
static struct alignment_evidence capture_alignment_evidence(const wf_t* w, const char* reason)
{
    struct alignment_evidence evidence = { { 0 } };
    if (!w->aligned && !w->warm_started) { return evidence; }
    snprintf(evidence.line, sizeof(evidence.line),"alignment evidence: reason=%s mono_ns=%lld ref_generation=%llu "
          "delay=%.3f coherence=%.3f tx_packets=%ld tx_discontinuities=%llu tx_dropped=%llu "
          "tx_age_ns=%lld sent=%llu send_errors=%llu deadlines=%llu "
          "session_max_send_gap_ns=%lld session_max_work_ns=%lld "
          "iq_frames=%llu iq_discontinuities=%llu iq_stale=%llu iq_invalid=%llu "
          "iq_duplicates=%llu iq_old=%llu iq_age_ns=%lld ref_fifo=%d fb_fifo=%d",
          reason, (long long)pd_iq_monotonic_ns(), (unsigned long long)w->reference_generation,
          pd_align_delay(w->align), pd_align_coherence(w->align), w->n_tx_pkts,
          (unsigned long long)w->tx_guard.discontinuities, (unsigned long long)w->tx_guard.dropped,
          (long long)w->tx_age_ns, (unsigned long long)w->tx_sent_datagrams,
          (unsigned long long)w->tx_send_errors, (unsigned long long)w->tx_deadline_faults,
          (long long)w->tx_max_send_gap_ns, (long long)w->tx_max_work_ns,
          (unsigned long long)w->local_iq.frames, (unsigned long long)w->local_iq.guard.discontinuities,
          (unsigned long long)w->local_iq.stale, (unsigned long long)w->local_iq.invalid,
          (unsigned long long)w->local_iq.duplicates, (unsigned long long)w->local_iq.old,
          (long long)w->local_iq.last_age_ns, w->ref.n, w->fbk.n);
    return evidence;
}
static void log_alignment_evidence(const struct alignment_evidence* evidence)
{
    if (evidence->line[0]) { logf_("%s", evidence->line); }
}

static void log_measurement_window(const wf_t* w, const char* event);

static void log_bounded_source_end(const wf_t* w, const char* reason)
{
    if (w->calibration.phase == PD_CAL_ARMED || w->calibration.phase == PD_CAL_RUNNING ||
        (test_source_active(w) && w->test_duration_samples)) {
        log_measurement_window(w, reason);
        // One bounded retirement record captures the source and stream state.
        // This does not log packets or change cancellation policy.
        logf_("bounded source ended: reason=%s source=%016llx phase=%s keyed=%d tx_on=%d "
              "samples_left=%d generated=%llu iq_frames=%llu iq_starts=%llu iq_stale=%llu "
              "iq_invalid=%llu iq_age_ns=%lld tx_discontinuities=%llu tx_rejected=%llu deadlines=%llu",
              reason, (unsigned long long)w->calibration.source_id, pd_calibration_name(&w->calibration),
              w->keyed, w->tx_on, w->calibration.samples_left, (unsigned long long)w->generated_samples,
              (unsigned long long)w->local_iq.frames, (unsigned long long)w->local_iq.starts,
              (unsigned long long)w->local_iq.stale, (unsigned long long)w->local_iq.invalid,
              (long long)w->local_iq.last_age_ns, (unsigned long long)w->tx_guard.discontinuities,
              (unsigned long long)w->tx_rejected, (unsigned long long)w->tx_deadline_faults);
    }
}

static void disarm_bounded_test(wf_t* w, const char* reason)
{
    log_bounded_source_end(w, reason);
    w->measurement_tone_sent_ns = 0;
    pd_calibration_cancel(&w->calibration);
    if (w->test_duration_samples) {
        w->test_f1 = w->test_f2 = 0;
        w->test_left = 0;
    }
}

static void local_feedback_invalidate(wf_t* w, int fault);
static void local_feedback_suspend_transport(wf_t* w);
static void local_feedback_validate_transport(wf_t* w);
static void reset_histories(wf_t* w);
static int local_feedback_expired(const wf_t* w, int64_t now);
static int native_apd_blocked(const wf_t* w)
{
    return w->native_apd_enabled || w->native_apd_active;
}

static int correction_ready(const wf_t* w)
{
    int learned = w->aligned && pd_eq_ready(w->eq) && pd_core_current_solution(w->core);
    int cached = w->local_iq_mode && w->warm_start_armed && !w->local_iq_fault &&
        pd_core_cached_solution(w->core) && w->local_iq.frames > w->key_iq_frames;
    return w->keyed && !native_apd_blocked(w) && w->feedback_ok && w->tx_guard.seen && !w->tx_guard.reset_pending &&
        (learned || cached) && (!w->local_iq_mode || !local_feedback_expired(w, pd_iq_monotonic_ns()));
}

static uint64_t measurement_source_id(const wf_t* w)
{
    return w->calibration.purpose==PD_SOURCE_MEASUREMENT ? w->calibration.source_id : 0;
}

static int measurement_tones_ready(const wf_t* w, int64_t now)
{
    // A complete emitted tone packet precedes this window by the configured
    // maximum alignment search delay plus 100 ms for packet/filter settling.
    // Compare against IQ kernel-arrival time, excluding queued preamble data.
    if (!measurement_source_id(w) || w->calibration.phase!=PD_CAL_RUNNING || !w->keyed ||
        !w->local_iq_mode || w->local_iq_fault || !w->aligned || !w->feedback_ok ||
        !pd_measurement_spacing_valid(w->calibration.tone_spacing_hz) ||
        w->test_f1!=pd_calibration_tone1(&w->calibration) || w->test_f2!=pd_calibration_tone2(&w->calibration) ||
        w->test_noise_left || w->test_left<=RATE/200 ||
        w->measurement_tone_sent_ns<=0 || w->measurement_guard_ns<100000000 ||
        now>=w->calibration.deadline_ns || now<w->measurement_tone_sent_ns ||
        w->local_iq.last_valid_ns<=0 || now<w->local_iq.last_valid_ns ||
        now-w->local_iq.last_valid_ns>100000000 || w->local_iq.last_age_ns<0 ||
        w->local_iq.last_age_ns>25000000) { return 0; }
    int64_t arrived=w->local_iq.last_valid_ns-w->local_iq.last_age_ns;
    return arrived>=w->measurement_tone_sent_ns &&
        arrived-w->measurement_tone_sent_ns>=w->measurement_guard_ns;
}

static void log_measurement_window(const wf_t* w, const char* event)
{
    if (!measurement_source_id(w) || (w->calibration.phase != PD_CAL_ARMED &&
        w->calibration.phase != PD_CAL_RUNNING)) { return; }
    int64_t now = pd_iq_monotonic_ns();
    logf_("measurement window: event=%s ready=%d now_ns=%lld phase=%s keyed=%d "
          "local=%d fault=%d aligned=%d feedback=%d noise_left=%d test_left=%d "
          "tone1=%.0f tone2=%.0f tone_sent_ns=%lld guard_ns=%lld iq_valid_ns=%lld "
          "iq_age_ns=%lld deadline_ns=%lld fits=%d solution=%d",
          event, measurement_tones_ready(w, now), (long long)now, pd_calibration_name(&w->calibration),
          w->keyed, w->local_iq_mode, w->local_iq_fault, w->aligned, w->feedback_ok,
          w->test_noise_left, w->test_left, w->test_f1, w->test_f2,
          (long long)w->measurement_tone_sent_ns, (long long)w->measurement_guard_ns,
          (long long)w->local_iq.last_valid_ns, (long long)w->local_iq.last_age_ns,
          (long long)w->calibration.deadline_ns, w->core ? pd_core_fits(w->core) : -1,
          w->core ? pd_core_current_solution(w->core) : -1);
}

static int feedback_binding_valid(const wf_t* w);

static struct pd_capture_meta capture_meta(const wf_t* w)
{
    double hz = w->pan_mhz * 1e6;
    return (struct pd_capture_meta){
        .generation = w->acquisition_generation,
        .center_hz = isfinite(hz) && hz > 0 && hz <= 1e9 ? (uint64_t)llround(hz) : 0,
        .stream = w->local_iq_mode ? w->daxiq_sid : 0,
        .rate = (uint32_t)w->daxiq_rate,
        .keyed = (uint32_t)!!w->keyed,
        .correcting = (uint32_t)(w->correct && !w->local_iq_fault && correction_ready(w)),
        .measurement_id = measurement_source_id(w),
        .measurement_spacing_hz = measurement_source_id(w) ? w->calibration.tone_spacing_hz : 0,
        .measurement_tones = (uint32_t)measurement_tones_ready(w,pd_iq_monotonic_ns()),
        .iq_channel = w->sealed_feedback_pan ? (uint32_t)w->daxiq_ch : 0,
        .feedback_pan = w->sealed_feedback_pan,
        .gui_handle = w->sealed_feedback_pan ? w->calibration_gui : 0,
        .binding_valid = w->sealed_feedback_pan && w->local_iq_mode && feedback_binding_valid(w)
    };
}

static void reference_restart(wf_t* w)
{
    w->tx_last_send_ns = 0;
    ++w->reference_generation;
    memset(&w->tx_guard, 0, sizeof(w->tx_guard));
    if (w->tx_in && w->reference_generation) {
        pd_tx_guard_init(&w->tx_guard, w->tx_in, w->reference_generation, 25000000);
    }
}


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

static double fb_target_mhz(const wf_t* w) { return w->tx_mhz - FB_OFFSET_HZ * 1e-6; }

static void fb_slice_create(wf_t* w)
{
    if (!w->fb_audio || w->fb_slice >= 0 || w->seq_fbslice || w->api < 0 || !(w->tx_mhz > 0))
        return;
    w->seq_fbslice = api_send(w, "slice create freq=%.6f rxant=%s mode=DIGU", fb_target_mhz(w), w->fb_ant);
}

static void fb_slice_follow(wf_t* w)
{
    if (w->fb_slice >= 0 && w->api >= 0 && fabs(w->fb_mhz - fb_target_mhz(w)) > 0.5e-6)
        api_send(w, "slice tune %d %.6f", w->fb_slice, fb_target_mhz(w));
}

static void fb_slice_remove(wf_t* w)
{
    if (w->api >= 0) {
        if (w->fb_dax_sid)
            api_send(w, "stream remove 0x%08X", w->fb_dax_sid);
        if (w->fb_slice >= 0) {
            api_send(w, "slice remove %d", w->fb_slice);
            logf_("feedback slice %d removed", w->fb_slice);
        }
    }
    w->fb_dax_sid = 0;
    w->fb_slice = -1;
    w->seq_fbslice = w->seq_daxrx = 0;
    w->fb_mhz = 0;
}

static int feedback_binding_valid(const wf_t* w)
{
    return !w->sealed_feedback_pan || (!w->feedback_binding_failed &&
        w->feedback_pan==w->sealed_feedback_pan && w->feedback_pan_owner==w->calibration_gui && w->calibration_gui);
}

static void start_feedback(wf_t* w)
{
    pd_capture_invalidate(&w->capture);
    ++w->acquisition_generation;
    w->acquisition_pending = 0;
    w->eq_pending = w->eq_n = 0;
    w->eq_settle_left = EQ_PHASE_SETTLE_SAMPLES;
    w->eq_energy = 0;
    if (w->fb_audio) {
        double off = (fb_target_mhz(w) - w->fb_mhz) * 1e6;
        w->feedback_ok = w->fb_dax_sid && w->fb_slice >= 0 && fabs(off) < 1.0;
        if (!w->feedback_ok) {
            logf_("transmit without feedback: the feedback slice isn't ready (slice %d, DAX stream 0x%08x, %.0f Hz off)",
                  w->fb_slice, w->fb_dax_sid, off);
            return;
        }
        cbp_init(&w->fb_bp, FB_AUDIO_LO_HZ, FB_AUDIO_HI_HZ);
        w->fb_nco = 0;
        w->ref.n = w->fbk.n = 0;
        w->acq_n = 0;
        w->aligned = 0;
        pd_eq_reset(w->eq);
        w->weak_blocks = 0;
        w->settle_left = SETTLE_SAMPLES;
        w->test_noise_left = TEST_NOISE_SAMPLES;
        pd_align_restart(w->align);
        logf_("transmit: feedback from slice %d DAX %d, %.0f Hz below the carrier (tx %.6f MHz)",
              w->fb_slice, w->dax_ch, FB_OFFSET_HZ, w->tx_mhz);
        return;
    }
    double shift = (w->tx_mhz - w->pan_mhz) * 1e6;
    // The correction spans about +-10 kHz around the carrier; that band has
    // to sit inside the DAX IQ stream.
    double room = w->daxiq_rate / 2.0 - 10000;
    w->feedback_ok = feedback_binding_valid(w) && w->daxiq_sid && w->pan_mhz > 0 && fabs(shift) <= room;
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

static void set_keyed(wf_t* w, int keyed, int clean_unkey)
{
    if (keyed && w->calibration.phase==PD_CAL_ARMED && w->calibration.purpose==PD_SOURCE_MEASUREMENT) {
        w->measurement_key=1;
    }
    if (!keyed && w->keyed) { disarm_bounded_test(w, "unkey"); }
    if (keyed == w->keyed) {
        return;
    }
    if (!keyed) {
        if (w->transport_pending) {
            w->transport_pending = 0;
            pd_eq_reset(w->trusted_eq);
        }
        int retain = clean_unkey && w->local_iq_mode && !w->local_iq_fault &&
            correction_ready(w);
        pd_core_pause(w->core, retain);
        w->local_iq_fault = !pd_core_cached_solution(w->core);
    } else if (w->local_iq_mode && local_feedback_expired(w, pd_iq_monotonic_ns())) {
        local_feedback_invalidate(w, 1);
    }
    w->keyed = keyed;
    if (keyed) {
        int64_t cal_now=pd_iq_monotonic_ns();
        if (w->calibration.phase==PD_CAL_ARMED &&
            (!w->calibration_control || !w->calibration_software_owner ||
             (!w->correct && w->calibration.purpose==PD_SOURCE_CALIBRATION) || native_apd_blocked(w) || w->active_slice<0 ||
             w->local_iq.last_valid_ns<=0 || cal_now<w->local_iq.last_valid_ns || cal_now-w->local_iq.last_valid_ns>100000000)) {
            log_bounded_source_end(w, "keying_ineligible");
            w->measurement_tone_sent_ns = 0;
            pd_calibration_cancel(&w->calibration);
        }
        if (pd_calibration_begin(&w->calibration, cal_now)) {
            w->test_f1 = pd_calibration_tone1(&w->calibration);
            w->test_f2 = pd_calibration_tone2(&w->calibration);
            w->test_duration_samples = w->test_left = PD_CAL_SAMPLES;
            w->test_started = 0; w->test_n = 0; w->measurement_tone_sent_ns = 0;
            cbp_init(&w->noise_bp, TEST_NOISE_LO_HZ, TEST_NOISE_HI_HZ);
            w->noise_seed = 12345;
        }
        w->key_started_ns = pd_iq_monotonic_ns();
        w->mic_peak = w->prelimit_peak = 0;
        w->limited_samples = w->generated_samples = w->quiet_blocks = 0;
    }
    reset_histories(w);
    reference_restart(w);
    w->warm_started = 0;
    w->warm_start_armed = keyed && w->local_iq_mode && !w->local_iq_fault &&
        pd_core_cached_solution(w->core);
    w->key_iq_frames = w->local_iq.frames;
    if (keyed && w->active_slice >= 0) {
        start_feedback(w);
    }
    pd_core_set_keyed(w->core, keyed && w->active_slice >= 0 && !native_apd_blocked(w));
    if (!keyed) {
        logf_("receive: %d fits, cache %d, delay %.2f, offset %.3f Hz",
              pd_core_fits(w->core), pd_core_cached_solution(w->core),
              pd_align_delay(w->align), pd_align_offset_hz(w->align, RATE));
    }
}

static void model_changed(wf_t* w)
{
    if (w->calibration.phase == PD_CAL_ARMED || w->calibration.phase == PD_CAL_RUNNING) { disarm_bounded_test(w, "model_changed"); }
    local_feedback_invalidate(w, 1);
    reference_restart(w);
    pd_core_set_keyed(w->core, w->keyed && w->active_slice >= 0 && !native_apd_blocked(w));
    if (w->keyed && w->active_slice >= 0) {
        start_feedback(w);
    }
}

static int observe_fields(wf_t* w, const char* m, const char* const* keys,
                          unsigned offset, unsigned count)
{
    int changed = 0;
    for (unsigned i = 0; i < count; ++i) {
        char value[64];
        if (kv(m, keys[i], value, sizeof(value)) && strcmp(value, w->model_fields[offset+i])) {
            memcpy(w->model_fields[offset+i], value, strlen(value)+1);
            changed = 1;
        }
    }
    return changed;
}

static int apd_switch_active(const char* value, size_t capacity)
{
    // Only a complete decimal zero means off; malformed or truncated values
    // must not permit a second predistorter to train or apply a curve.
    size_t size = strlen(value);
    return !size || size >= capacity - 1 || strspn(value, "0") != size;
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
            model_changed(w);
            logf_("registered %s: tx_in 0x%08x rx_in 0x%08x", WF_NAME, w->tx_in, w->rx_in);
        } else if (seq && seq == w->seq_fbslice) {
            w->seq_fbslice = 0;
            if (code) {
                logf_("feedback slice refused: %08lx %s", code, msg);
            } else {
                w->fb_slice = atoi(msg);
                w->fb_mhz = fb_target_mhz(w);
                api_send(w, "slice set %d agc_mode=off", w->fb_slice);
                api_send(w, "filt %d 0 %d", w->fb_slice, FB_AUDIO_HI_HZ);
                api_send(w, "slice set %d audio_mute=1", w->fb_slice);
                api_send(w, "slice set %d dax=%d", w->fb_slice, w->dax_ch);
                w->seq_daxrx = api_send(w, "stream create type=dax_rx dax_channel=%d", w->dax_ch);
                logf_("feedback slice %d at %.6f MHz on %s, DAX %d", w->fb_slice, w->fb_mhz, w->fb_ant, w->dax_ch);
            }
        } else if (seq && seq == w->seq_daxrx) {
            w->seq_daxrx = 0;
            if (code)
                logf_("DAX audio stream refused: %08lx %s", code, msg);
            else {
                w->fb_dax_sid = (uint32_t)strtoul(msg, NULL, 16);
                logf_("feedback DAX audio stream 0x%08x", w->fb_dax_sid);
            }
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
            model_changed(w);
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
        if (idx>=0 && idx<16) {
            char owner[32];int present=calibration_field(m,"client_handle",owner,sizeof(owner));
            if (present) {
                uint32_t value=0;if (present!=1 || !calibration_handle(owner,&value)) { value=0; }
                if (value!=w->calibration_slice_owner[idx] && idx==w->active_slice &&
                    (w->calibration.phase==PD_CAL_ARMED || w->calibration.phase==PD_CAL_RUNNING)) { disarm_bounded_test(w, "slice_owner_changed"); }
                w->calibration_slice_owner[idx]=value;
            }
            if (kv(m,"in_use",v,sizeof(v)) && !strcmp(v,"0")) { w->calibration_slice_owner[idx]=0; }
        }
        char mode[16] = "";
        if (idx == w->active_slice && kv(m, "in_use", v, sizeof(v)) && !strcmp(v, "0")) {
            disarm_bounded_test(w, "slice_removed");
            w->tx_on = 0;
            set_keyed(w, 0, 0);
            w->active_slice = -1;
            model_changed(w);
            fb_slice_remove(w);
            return;
        }
        if (kv(m, "mode", mode, sizeof mode)) {
            if (!strcmp(mode, WF_MODE) && w->active_slice < 0) {
                if (idx < 0 || idx >= 16) {
                    return;
                }
                w->active_slice = idx;
                memset(w->model_fields, 0, 7 * sizeof(w->model_fields[0]));
                // The slice's frequency usually came in an earlier status.
                if (idx >= 0 && idx < 16 && w->slice_mhz[idx] > 0)
                    w->tx_mhz = w->slice_mhz[idx];
                model_changed(w);
                logf_("PD selected on slice %d", idx);
                // Ask for the feedback stream again if the first request,
                // made at connect, didn't produce one.
                if (w->fb_audio)
                    fb_slice_create(w);
                else if (!w->local_iq_mode && !w->daxiq_sid && w->api >= 0)
                    w->seq_daxiq = api_send(w, "stream create type=dax_iq daxiq_channel=%d", w->daxiq_ch);
            } else if (strcmp(mode, WF_MODE) && w->active_slice == idx) {
                disarm_bounded_test(w, "pd_mode_released");
                logf_("PD released by slice %d", idx);
                w->active_slice = -1;
                w->tx_on = 0;
                set_keyed(w, 0, 0);
                model_changed(w);
                fb_slice_remove(w);
            }
        }
        if (kv(m, "RF_frequency", v, sizeof v)) {
            if (idx >= 0 && idx < 16)
                w->slice_mhz[idx] = parse_mhz(v);
            if (idx == w->fb_slice && w->fb_slice >= 0)
                w->fb_mhz = parse_mhz(v);
            if (idx == w->active_slice) {
                double frequency = parse_mhz(v);
                if (frequency != w->tx_mhz) {
                    w->tx_mhz = frequency;
                    model_changed(w);
                }
                if (w->fb_audio) {
                    fb_slice_create(w);
                    fb_slice_follow(w);
                }
            }
        }
        if (idx == w->active_slice) {
            static const char* const keys[] = { "rxant", "txant", "rfgain", "tx_offset_freq", "xit_on", "xit_freq", "tx" };
            if (observe_fields(w, m, keys, 0, 7)) {
                model_changed(w);
            }
        }
        if (idx == w->fb_slice && w->fb_slice >= 0 && kv(m, "in_use", v, sizeof v) && !strcmp(v, "0")) {
            logf_("feedback slice %d went away", w->fb_slice);
            w->fb_slice = -1;
            w->fb_dax_sid = 0;
            w->feedback_ok = 0;
            model_changed(w);
        }
        return;
    }
    if (!strncmp(m, "display pan ", 12)) {
        uint32_t id = (uint32_t)strtoul(m + 12, NULL, 0);
        int changed = 0;
        if (w->sealed_feedback_pan) {
            const char* end=strchr(m+12,' ');
            char identity[16];size_t length=end?(size_t)(end-(m+12)):0;uint32_t parsed=0;
            if(!length || length>=sizeof(identity)){w->feedback_binding_failed=1;w->feedback_ok=0;model_changed(w);return;}
            memcpy(identity,m+12,length);identity[length]=0;
            if(!calibration_handle(identity,&parsed) || !parsed){w->feedback_binding_failed=1;w->feedback_ok=0;model_changed(w);return;}
            if(parsed!=w->sealed_feedback_pan){return;}
            char owner[32],channel[16],expected[16];uint32_t handle=0;
            int has_owner=calibration_field(m,"client_handle",owner,sizeof(owner));
            int has_channel=calibration_field(m,"daxiq_channel",channel,sizeof(channel));
            snprintf(expected,sizeof(expected),"%d",w->daxiq_ch);
            if(has_owner<0 || (has_owner && (!calibration_handle(owner,&handle) || handle!=w->calibration_gui)) ||
               has_channel<0 || (has_channel && strcmp(channel,expected)) || !strcmp(end+1,"removed")) {
                w->feedback_binding_failed=1;w->feedback_ok=0;w->feedback_pan=0;w->pan_mhz=0;model_changed(w);return;
            }
            if(w->feedback_binding_failed){return;}
            if(has_owner && w->feedback_pan_owner!=handle){w->feedback_pan_owner=handle;changed=1;}
        }
        if (id && id == w->feedback_pan && strstr(m, " removed")) {
            w->feedback_pan = 0;
            w->pan_mhz = 0;
            changed = 1;
        }
        if (kv(m, "daxiq_channel", v, sizeof(v))) {
            if (atoi(v) == w->daxiq_ch && id != w->feedback_pan) {
                w->feedback_pan = id;
                w->pan_mhz = 0;
                memset(&w->model_fields[7], 0, 2 * sizeof(w->model_fields[0]));
                changed = 1;
            } else if (atoi(v) != w->daxiq_ch && id == w->feedback_pan) {
                w->feedback_pan = 0;
                w->pan_mhz = 0;
                changed = 1;
            }
        }
        if (id && id == w->feedback_pan) {
            if (kv(m, "center", v, sizeof(v)) && parse_mhz(v) != w->pan_mhz) {
                w->pan_mhz = parse_mhz(v);
                changed = 1;
            }
            static const char* const keys[] = { "rxant", "rfgain" };
            changed |= observe_fields(w, m, keys, 7, 2);
        }
        if (changed) {
            model_changed(w);
        }
        return;
    }
    if (!strncmp(m, "transmit ", 9)) {
        // FlexLib Radio.ParseTransmitStatus routes saved band settings separately.
        if (!strncmp(m + 9, "band ", 5)) { return; }
        static const char* const keys[] = { "rfpower", "lo", "hi" };
        if (observe_fields(w, m, keys, 9, 3)) {
            model_changed(w);
        }
        return;
    }
    if (!strncmp(m, "apd ", 4)) {
        // FlexLib 4.3.16 APD.cs separates sampler status from top-level enable
        // and equalizer_active. An unrecognized switch value blocks correction.
        if (!strncmp(m + 4, "sampler ", 8)) {
            return;
        }
        int changed = 0;
        if (kv(m, "enable", v, sizeof(v))) {
            int enabled = apd_switch_active(v, sizeof(v));
            changed |= enabled != w->native_apd_enabled;
            w->native_apd_enabled = enabled;
        }
        if (kv(m, "equalizer_active", v, sizeof(v))) {
            int active = apd_switch_active(v, sizeof(v));
            changed |= active != w->native_apd_active;
            w->native_apd_active = active;
        }
        if (changed) {
            model_changed(w);
            logf_("native APD %s: AetherPD correction and learning %s",
                  native_apd_blocked(w) ? "enabled or active" : "off",
                  native_apd_blocked(w) ? "blocked" : "may restart");
        }
        return;
    }
    if (!strncmp(m, "radio ", 6)) {
        static const char* const keys[] = { "full_duplex_enabled" };
        if (observe_fields(w, m, keys, 12, 1)) {
            model_changed(w);
        }
        return;
    }
    if (!strncmp(m, "interlock ", 10) && kv(m, "state", v, sizeof v)) {
        char mode[32],source[32],reason[64],allowed[8],owner[32];uint32_t handle=0;
        int complete=calibration_field(m,"state",mode,sizeof(mode))==1 &&
            calibration_field(m,"source",source,sizeof(source))==1 && calibration_field(m,"reason",reason,sizeof(reason))==1 &&
            calibration_field(m,"tx_allowed",allowed,sizeof(allowed))==1 && calibration_field(m,"tx_client_handle",owner,sizeof(owner))==1 &&
            calibration_handle(owner,&handle);
        w->calibration_idle=complete && !strcmp(mode,"READY") && !source[0] && !reason[0] && !strcmp(allowed,"1");
        w->calibration_software_owner=complete && w->calibration_gui && handle==w->calibration_gui && !strcmp(source,"SW") && !reason[0] && !strcmp(allowed,"1") &&
            (!strcmp(mode,"TRANSMITTING") || !strcmp(mode,"PTT_REQUESTED"));
        if (w->calibration.phase==PD_CAL_ARMED && w->calibration.purpose==PD_SOURCE_MEASUREMENT &&
            (!strcmp(v,"TRANSMITTING") || !strcmp(v,"PTT_REQUESTED"))) {
            w->measurement_key=1;
        }
        if (w->calibration.phase==PD_CAL_ARMED || w->calibration.phase==PD_CAL_RUNNING) {
            if (!complete || (!w->calibration_idle && !w->calibration_software_owner)) { disarm_bounded_test(w, "interlock_ineligible"); }
        }
        int admitting = !strcmp(v, "TRANSMITTING") || !strcmp(v, "PTT_REQUESTED");
        if (!admitting && strcmp(v, "READY") && strcmp(v, "UNKEY_REQUESTED")) {
            local_feedback_invalidate(w, 1);
        }
        if (admitting && !w->tx_on) {
            reference_restart(w);
        }
        if (!admitting && w->tx_on) { disarm_bounded_test(w, "tx_admission_ended"); }
        w->tx_on = admitting;
        set_keyed(w, !strcmp(v, "TRANSMITTING"), !strcmp(v, "UNKEY_REQUESTED"));
        if (w->calibration_idle) { w->measurement_key=0; }
        return;
    }
}

static void send_pairs(wf_t* w, uint32_t sid, int* count, const float* pairs, int n)
{
    unsigned char pkt[28 + 4 * MAX_SAMPLES];
    for (int off = 0; off < n; off += MAX_SAMPLES / 2) {
        int m = n - off < MAX_SAMPLES / 2 ? n - off : MAX_SAMPLES / 2;
        int len = vita_build(pkt, sid, CLASS_AUDIO_24K, (*count)++, pairs + 2 * off, m);
        if (w->udp >= 0) {
            int64_t before = sid == w->tx_in ? pd_iq_monotonic_ns() : 0;
            if (sid==w->tx_in && w->measurement_key) {
                if (w->calibration.phase==PD_CAL_RUNNING && !pd_calibration_tick(&w->calibration,before)) {
                    disarm_bounded_test(w,"measurement_dispatch_expired");
                }
                if (!test_source_active(w)) { memset(pkt+28,0,(size_t)len-28); }
            }
            ssize_t sent = sendto(w->udp, pkt, (size_t)len, 0,
                                  (struct sockaddr*)&w->radio, sizeof w->radio);
            int send_error = sent < 0 ? errno : 0;
            if (sid == w->tx_in) {
                int64_t after = pd_iq_monotonic_ns();
                int64_t gap = w->tx_last_send_ns ? before - w->tx_last_send_ns : 0;
                int64_t work = after - w->tx_handler_start_ns;
                if (gap > w->tx_max_send_gap_ns) { w->tx_max_send_gap_ns = gap; }
                if (work > w->tx_max_work_ns) { w->tx_max_work_ns = work; }
                w->tx_last_send_ns = before;
                ++w->tx_sent_datagrams;
                if (sent != len) { ++w->tx_send_errors; }
                int late = w->tx_deadline_ns && gap > w->tx_deadline_ns;
                if (w->local_iq_mode && w->keyed && !w->tx_deadline_in_packet &&
                    (late || sent != len)) {
                    if (late) { ++w->tx_deadline_faults; }
                    w->tx_deadline_in_packet = 1;
                    struct alignment_evidence evidence = capture_alignment_evidence(w, "tx_output");
                    if (late && sent == len) { local_feedback_suspend_transport(w); }
                    else { local_feedback_invalidate(w, 1); }
                    log_alignment_evidence(&evidence);
                    if (test_source_active(w)) { w->test_noise_left = TEST_NOISE_SAMPLES; }
                    logf_("TX output discontinuity: gap %.3f ms, sent %ld/%d; correction retired",
                          gap / 1e6, (long)sent, len);
                }
                if (w->tx_trace && w->tx_trace_rows < 8192) {
                    struct pd_tx_trace_row* row = &w->tx_trace[w->tx_trace_rows++];
                    *row = (struct pd_tx_trace_row) {
                        w->tx_arrival_ns, w->tx_age_ns, w->tx_handler_start_ns, before, after, w->key_started_ns,
                        (uint64_t)w->n_tx_pkts, w->local_iq.frames, w->correction_resets, w->cached_packets,
                        sid, (uint32_t)((*count - 1) & 15), (uint32_t)m,
                        (int)sent, send_error, w->aligned, w->local_iq_fault, w->warm_started,
                        pd_align_coherence(w->align), pd_align_delay(w->align)
                    };
                } else if (w->tx_trace) { ++w->tx_trace_dropped; }
            }
        }
    }
}

// Pairs reference and feedback blocks while transmitting.
static void calibrate(wf_t* w)
{
    if (native_apd_blocked(w)) {
        return;
    }
    double tx[2 * BLOCK], rx[2 * BLOCK], atx[2 * BLOCK], arx[2 * BLOCK], etx[2 * BLOCK], erx[2 * BLOCK];
    while (w->ref.n >= BLOCK && w->fbk.n >= BLOCK) {
        fifo_pop(&w->ref, tx, BLOCK);
        fifo_pop(&w->fbk, rx, BLOCK);
        if (w->raw && w->raw_left > 0) {
            for (int k = 0; k < BLOCK && w->raw_left > 0; k++, w->raw_left--) {
                float f[4] = { (float)tx[2 * k], (float)tx[2 * k + 1], (float)rx[2 * k], (float)rx[2 * k + 1] };
                fwrite(f, sizeof f, 1, w->raw);
            }
            if (w->raw_left == 0)
                fflush(w->raw);
        }
        if (w->settle_left > 0) {
            w->settle_left -= BLOCK;
            continue;
        }
        if (!w->aligned) {
            struct pd_acquire_result result;
            int acquired = pd_acquire_take(w->acquire, w->acquisition_generation, &result);
            if (acquired) {
                w->acquisition_pending = 0;
                w->eq_pending = w->eq_n = 0;
                w->eq_settle_left = EQ_PHASE_SETTLE_SAMPLES;
                w->eq_energy = 0;
            }
            if (acquired == 1) {
                if (result.delay < 0) {
                    w->acq_n = 0;
                    logf_("no delay lock yet (correlation %.2f)", result.envelope_correlation);
                } else {
                    pd_align_apply_estimate(w->align, result.delay, result.envelope_correlation,
                                            result.phase_correlation, result.offset_hz, RATE);
                    w->aligned = 1;
                    w->weak_blocks = 0;
                    logf_("loop delay %.2f samples (%.2f ms), envelope correlation %.2f, phase correlation %.3f",
                          result.delay, result.delay * 1000.0 / RATE,
                          result.envelope_correlation, result.phase_correlation);
                    continue;
                }
            }
            if (w->acquisition_pending) {
                continue;
            }
            int m = BLOCK < w->acq_len - w->acq_n ? BLOCK : w->acq_len - w->acq_n;
            memcpy(w->acq_tx + 2 * w->acq_n, tx, sizeof(double) * 2 * (size_t)m);
            memcpy(w->acq_rx + 2 * w->acq_n, rx, sizeof(double) * 2 * (size_t)m);
            w->acq_n += m;
            if (w->acq_n == w->acq_len) {
                if (pd_acquire_submit(w->acquire, w->acquisition_generation,
                                      w->acq_tx, w->acq_rx, w->acq_n)) {
                    w->acquisition_pending = 1;
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
        int signal_present = pd_align_signal_present(w->align);
        if (v > 0 && !signal_present) { ++w->quiet_blocks; }
        if (v > 0 && signal_present && pd_align_coherence(w->align) < WEAK_COHERENCE) {
            if (++w->weak_blocks >= WEAK_BLOCKS) {
                logf_("feedback stopped matching (coherence %.2f): finding the delay again",
                      pd_align_coherence(w->align));
                struct alignment_evidence evidence = capture_alignment_evidence(w, "coherence");
                local_feedback_invalidate(w, 1);
                log_alignment_evidence(&evidence);
                // Measure again on noise, past the samples already in flight.
                w->test_noise_left = TEST_NOISE_SAMPLES;
                w->settle_left = SETTLE_SAMPLES;
                pd_align_restart(w->align);
            }
            continue;   // don't calibrate on pairs that don't match
        }
        if (signal_present) { w->weak_blocks = 0; }
        if (!pd_eq_ready(w->eq)) {
            if (w->eq_settle_left > 0) {
                if (signal_present) { w->eq_settle_left -= v; }
                continue;
            }
            int result=pd_acquire_take_eq(w->acquire,w->acquisition_generation,w->eq);
            if (result) { w->eq_pending=0; }
            if (result==1) {
                local_feedback_validate_transport(w);
                logf_("transmit path fitted: %d taps, %.1f dB of the feedback left unexplained",
                      EQ_TAPS,10*log10(pd_eq_residual(w->eq)+1e-30));
                continue;
            }
            if (result) { w->eq_n=0; w->eq_energy=0; }
            if (!w->eq_pending) {
                int m=v<w->eq_train_len-w->eq_n ? v : w->eq_train_len-w->eq_n;
                memcpy(w->acq_tx+2*w->eq_n,atx,(size_t)m*2*sizeof(double));
                memcpy(w->acq_rx+2*w->eq_n,arx,(size_t)m*2*sizeof(double));
                for (int i=0;i<m;++i) {
                    w->eq_energy+=atx[2*i]*atx[2*i]+atx[2*i+1]*atx[2*i+1];
                }
                w->eq_n+=m;
                if (w->eq_n==w->eq_train_len) {
                    if (w->eq_energy/w->eq_n < w->reference_floor) {
                        w->eq_n=0;w->eq_energy=0;
                    } else if (pd_acquire_submit_eq(w->acquire,w->acquisition_generation,w->acq_tx,w->acq_rx,w->eq_n)) {
                        w->eq_pending=1;
                    }
                }
            }
            continue;
        }
        int m = pd_eq_apply(w->eq, atx, arx, v, etx, erx);
        // Advance the FIR even through pauses; only useful signal trains WDSP.
        if (signal_present) { pd_core_calibrate(w->core, etx, erx, m); }
    }
}

static void on_packet(wf_t* w, const unsigned char* p, ssize_t n)
{
    if (n >= 8 && w->tx_in && be32(p + 4) == w->tx_in) {
        if (!w->tx_on || w->active_slice < 0) {
            ++w->n_tx_held;
            return;
        }
        w->tx_deadline_in_packet = 0;
        w->tx_handler_start_ns = pd_iq_monotonic_ns();
        int seen = w->tx_guard.seen;
        enum pd_tx_result result = pd_tx_guard_check(&w->tx_guard, w->reference_generation,
                                                    p, (size_t)n, w->tx_arrival_ns, w->tx_age_ns);
        if (result == PD_TX_INVALID || result == PD_TX_STALE) {
            ++w->tx_rejected;
            struct alignment_evidence evidence = capture_alignment_evidence(w, result == PD_TX_STALE ? "tx_stale" : "tx_invalid");
            local_feedback_invalidate(w, 1);
            log_alignment_evidence(&evidence);
            return;
        }
        if (result == PD_TX_DROP || result == PD_TX_FOREIGN) {
            ++w->tx_rejected;
            return;
        }
        if (result == PD_TX_START && seen) {
            struct alignment_evidence evidence = capture_alignment_evidence(w, "tx_continuity");
            local_feedback_invalidate(w, 1);
            log_alignment_evidence(&evidence);
        }
        if (w->local_iq_mode && w->keyed && w->tx_deadline_ns && w->tx_last_send_ns &&
            w->tx_handler_start_ns - w->tx_last_send_ns > w->tx_deadline_ns) {
            ++w->tx_deadline_faults;
            w->tx_deadline_in_packet = 1;
            struct alignment_evidence evidence = capture_alignment_evidence(w, "tx_dispatch");
            local_feedback_suspend_transport(w);
            log_alignment_evidence(&evidence);
            if (test_source_active(w)) { w->test_noise_left = TEST_NOISE_SAMPLES; }
            logf_("TX dispatch deadline missed: %.3f ms; correction retired",
                  (w->tx_handler_start_ns - w->tx_last_send_ns) / 1e6);
        }
    }
    vita_t v;
    if (!vita_parse(p, n, &v))
        return;
    if (w->fb_dax_sid && v.sid == w->fb_dax_sid) {
        w->n_fb_pkts++;
        if (!w->keyed || w->active_slice < 0 || !w->feedback_ok)
            return;
        // DAX audio: big-endian float stereo frames at 24 kHz; the left
        // channel is the slice's audio, the RF FB_OFFSET_HZ below the carrier
        // up. Analytic signal, then down by FB_OFFSET_HZ to the carrier.
        const double step = 2 * M_PI * FB_OFFSET_HZ / RATE;
        for (int k = 0; k + 1 < v.words; k += 2) {
            double ai, aq;
            cbp_step(&w->fb_bp, word_float(v.payload + 4 * k), &ai, &aq);
            double c = cos(w->fb_nco), s = sin(w->fb_nco);
            fifo_push(&w->fbk, ai * c + aq * s, aq * c - ai * s);
            w->fb_nco = fmod(w->fb_nco + step, 2 * M_PI);
        }
        calibrate(w);
        return;
    }
    if (v.sid == w->daxiq_sid && w->daxiq_sid) {
        w->n_fb_pkts++;
        if (!(v.cls & CLASS_FLOAT_BIT)) {
            pd_capture_invalidate(&w->capture);
            if (!w->daxiq_format_logged)
                logf_("DAX IQ arrives as class 0x%04x, not float samples: feedback off", v.cls);
            w->daxiq_format_logged = 1;
            return;
        }
        if (w->local_iq_mode && w->capture.state == PD_CAPTURE_PENDING) {
            struct pd_capture_meta meta = capture_meta(w);
            pd_capture_feed(&w->capture, &meta, v.payload, (size_t)v.words * 4,
                            pd_iq_monotonic_ns());
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
    if (v.cls != CLASS_AUDIO_24K || w->active_slice < 0 || (v.sid != w->tx_in && v.sid != w->rx_in)) {
        w->n_fb_other++;
        w->last_cls = v.cls;
        w->last_sid = v.sid;
        return;
    }
    int pairs = v.words / 2;
    if (pairs > MAX_SAMPLES)
        pairs = MAX_SAMPLES;
    float out[2 * MAX_SAMPLES];
    if (v.sid == w->tx_in) {
        if (!w->tx_on) {
            w->n_tx_held++;
            return;
        }
        w->n_tx_pkts++;
        if (w->calibration.phase == PD_CAL_RUNNING &&
            !pd_calibration_tick(&w->calibration, pd_iq_monotonic_ns())) { disarm_bounded_test(w, "source_deadline"); }
        // Transmit: the radio's mic stream, while the operator transmits.
        double iq[2 * MAX_SAMPLES];
        for (int k = 0; k < pairs; k++) {
            double i, q, mi = 0, mq = 0;
            int testing = test_source_active(w);
            if (!w->measurement_key && (!testing || w->test_duration_samples)) {
                double mic = word_float(v.payload + 8 * k);
                if (fabs(mic) > w->mic_peak) { w->mic_peak = fabs(mic); }
                cbp_step(&w->mod, w->gain * mic, &mi, &mq);
            }
            if (testing) { w->test_started = 1; }
            if (testing && w->test_noise_left > 0) {
                // Band-limited noise for an unambiguous delay measurement.
                w->test_noise_left--;
                w->noise_seed = w->noise_seed * 1664525u + 1013904223u;
                double u = ((w->noise_seed >> 8) / 16777216.0) - 0.5;
                cbp_step(&w->noise_bp, TEST_NOISE_GAIN * w->peak * u, &i, &q);
            } else if (testing) {
                // Each tone at half the peak, so the envelope peaks at it.
                double t = (double)w->test_n++ / RATE, a = w->peak / 2;
                i = a * (cos(2 * M_PI * w->test_f1 * t) + cos(2 * M_PI * w->test_f2 * t));
                q = a * (sin(2 * M_PI * w->test_f1 * t) + sin(2 * M_PI * w->test_f2 * t));
            } else {
                i = mi;
                q = mq;
            }
            if (testing && w->test_duration_samples) {
                // Voice calibration blends back to the microphone. A measurement
                // blends to silence and holds it until a complete READY status.
                double weight = w->test_left < RATE/200 ? (double)w->test_left/(RATE/200) : 1;
                i = weight*i + (1-weight)*mi;
                q = weight*q + (1-weight)*mq;
                if (w->keyed) {
                    pd_calibration_consume(&w->calibration, 1);
                    if (--w->test_left == 0) { disarm_bounded_test(w, "source_samples_complete"); }
                }
            }
            double a = hypot(i, q);
            if (a > w->prelimit_peak) { w->prelimit_peak = a; }
            ++w->generated_samples;
            if (a > w->peak) {        // never past the peak the correction knows
                ++w->limited_samples;
                i *= w->peak / a;
                q *= w->peak / a;
            }
            iq[2 * k] = i;
            iq[2 * k + 1] = q;
        }
        int ready = correction_ready(w);
        if (ready && pd_core_current_solution(w->core) && pd_eq_ready(w->eq) && w->trusted_eq &&
            (!pd_eq_ready(w->trusted_eq) || w->trusted_eq_fits != pd_core_fits(w->core))) {
            (void)pd_eq_copy_solution(w->trusted_eq, w->eq);
            w->trusted_eq_fits = pd_core_fits(w->core);
            w->trusted_fit_ns = pd_iq_monotonic_ns();
        }
        if (ready && w->local_iq_fault) {
            w->local_iq_fault = 0;
            ++w->correction_recoveries;
        }
        {
            if (!w->correct || !ready) {
                double discarded[2 * MAX_SAMPLES];
                memcpy(discarded, iq, (size_t)pairs * 2 * sizeof(double));
                pd_core_correct(w->core, discarded, pairs);
            } else if (!(w->aligned && pd_eq_ready(w->eq) && pd_core_current_solution(w->core))) {
                pd_core_correct_cached(w->core, iq, pairs);
                ++w->cached_packets;
                if (!w->warm_started) {
                    w->warm_started = 1;
                    ++w->warm_starts;
                }
            } else {
                pd_core_correct(w->core, iq, pairs);
            }
        }
        for (int k = 0; k < pairs; k++) {
            double i = iq[2 * k], q = iq[2 * k + 1], a = hypot(i, q);
            // Retired measurement authority cannot expose microphone audio or
            // residual correction output while independent unkey is pending.
            if (w->measurement_key && !test_source_active(w)) { i=q=0; a=0; }
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
        if (w->calibration.phase==PD_CAL_RUNNING && w->calibration.purpose==PD_SOURCE_MEASUREMENT &&
            !w->test_noise_left && w->test_left>RATE/200 && !w->measurement_tone_sent_ns &&
            !w->tx_deadline_in_packet && w->tx_last_send_ns>0) {
            w->measurement_tone_sent_ns=pd_iq_monotonic_ns();
        }
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


static void reset_histories(wf_t* w)
{
    pd_capture_invalidate(&w->capture);
    ++w->acquisition_generation;
    w->acquisition_pending = 0;
    w->eq_pending = w->eq_n = 0;
    w->eq_settle_left = EQ_PHASE_SETTLE_SAMPLES;
    w->eq_energy = 0;
    w->aligned = 0;
    w->acq_n = 0;
    w->weak_blocks = 0;
    w->ref.head = w->ref.n = w->fbk.head = w->fbk.n = 0;
    w->settle_left = SETTLE_SAMPLES;
    pd_align_restart(w->align);
    pd_eq_reset(w->eq);
    decim_init(&w->fb, w->daxiq_rate, (w->tx_mhz - w->pan_mhz) * 1e6);
}

static void local_feedback_invalidate(wf_t* w, int fault)
{
    if (fault && (w->test_started || w->calibration.phase == PD_CAL_ARMED || w->calibration.phase == PD_CAL_RUNNING)) { disarm_bounded_test(w, "feedback_invalidated"); }
    ++w->correction_resets;
    pd_core_invalidate(w->core);
    w->transport_pending = 0;
    w->trusted_fit_ns = 0;
    if (w->trusted_eq) { pd_eq_reset(w->trusted_eq); }
    w->warm_start_armed = w->warm_started = 0;
    reset_histories(w);
    if (fault) {
        w->local_iq_fault = 1;
    }
}

static int transport_curve_recent(const wf_t* w)
{
    int64_t now = pd_iq_monotonic_ns();
    return w->trusted_fit_ns > 0 && now >= w->trusted_fit_ns &&
        now - w->trusted_fit_ns <= INT64_C(30000000000);
}

static void local_feedback_validate_transport(wf_t* w)
{
    if (w->transport_pending) {
        double distance = pd_eq_solution_distance(w->trusted_eq, w->eq);
        w->transport_eq_distance = isfinite(distance) ? distance : -1;
        int accepted = transport_curve_recent(w) && w->keyed && w->aligned && w->feedback_ok &&
            !native_apd_blocked(w) && w->tx_guard.seen && !w->tx_guard.reset_pending &&
            distance <= 0.05 && pd_eq_residual(w->eq) <= 0.01 &&
            w->local_iq.frames > w->key_iq_frames &&
            !local_feedback_expired(w, pd_iq_monotonic_ns()) &&
            pd_core_resume_transport(w->core);
        w->transport_pending = 0;
        if (accepted) {
            w->warm_start_armed = 1;
            w->local_iq_fault = 0;
            ++w->transport_recoveries;
            ++w->correction_recoveries;
        } else {
            pd_core_invalidate(w->core);
            pd_eq_reset(w->trusted_eq);
        }
        logf_("transport PA candidate %s: fresh EQ change %.6f, residual %.6f",
              accepted ? "accepted" : "rejected", distance, pd_eq_residual(w->eq));
    }
}

// Experimental, opt-in: no output uses the quarantined PA curve while a
// fresh alignment and equivalent linear-path fit are still unverified.
static void local_feedback_suspend_transport(wf_t* w)
{
    if (!w->transport_cache_enabled || w->transport_pending || !w->keyed ||
        !w->correct || !w->feedback_ok || w->local_iq_fault || native_apd_blocked(w) ||
        !w->trusted_eq || !pd_eq_ready(w->trusted_eq) || !transport_curve_recent(w) ||
        !pd_core_suspend_transport(w->core)) {
        local_feedback_invalidate(w, 1);
        return;
    }
    if (w->test_started || w->calibration.phase==PD_CAL_ARMED || w->calibration.phase==PD_CAL_RUNNING) { disarm_bounded_test(w, "transport_suspended"); }
    ++w->correction_resets;
    w->warm_start_armed = w->warm_started = 0;
    w->transport_pending = 1;
    w->transport_eq_distance = -1;
    w->key_iq_frames = w->local_iq.frames;
    reset_histories(w);
    w->local_iq_fault = 1;
}

static int local_feedback_start(wf_t* w)
{
    const char* value = getenv("AETHER_PD_IQ_FD");
    if (!w->local_iq_mode || !value || !*value) {
        return 0;
    }
    char* end;
    errno = 0;
    long fd = strtol(value, &end, 10);
    if (errno || *end || fd < 3 || fd > 1023 || prctl(PR_SET_DUMPABLE, 0, 0, 0, 0) ||
        !pd_local_iq_init(&w->local_iq, (int)fd)) {
        return 0;
    }
    w->daxiq_sid = w->local_iq.guard.sid;
    w->daxiq_rate = (int)w->local_iq.guard.rate;
    return 1;
}

static int local_feedback_expired(const wf_t* w, int64_t now)
{
    return (w->keyed || pd_core_cached_solution(w->core)) && (w->local_iq.last_valid_ns == 0 ||
        now < w->local_iq.last_valid_ns || now - w->local_iq.last_valid_ns > 100000000);
}

static int local_feedback_poll(wf_t* w)
{
    unsigned char packet[PD_IQ_MAX_PACKET];
    for (unsigned batch = 0; batch < 64; ++batch) {
        size_t bytes = 0;
        int received = pd_local_iq_receive(&w->local_iq, packet, sizeof(packet), &bytes);
        if (received < 0) {
            local_feedback_invalidate(w, 1);
            return 0;
        }
        if (!received) {
            break;
        }
        if (received == 2) {
            struct alignment_evidence evidence = capture_alignment_evidence(w, "iq_continuity_or_rejection");
            local_feedback_invalidate(w, !bytes || w->local_iq.frames > 1);
            log_alignment_evidence(&evidence);
        }
        if (bytes) {
            int64_t age = w->local_iq.last_age_ns;
            if (age < w->local_min_age_ns) {
                w->local_min_age_ns = age;
            }
            w->local_age_sum_ns += age;
            unsigned bin = (unsigned)(age / 100000);
            if (bin > 255) {
                bin = 255;
            }
            ++w->local_age_bins[bin];
            on_packet(w, packet, (ssize_t)bytes);
        }
    }
    if (local_feedback_expired(w, pd_iq_monotonic_ns())) {
        if (!w->local_iq_fault) {
            struct alignment_evidence evidence = capture_alignment_evidence(w, "iq_expired");
            local_feedback_invalidate(w, 1);
            log_alignment_evidence(&evidence);
        }
    }
    return 1;
}

static int local_feedback_probe(wf_t* w, int seconds)
{
    if (!local_feedback_start(w)) {
        fprintf(stderr, "local IQ endpoint unavailable\n");
        return 2;
    }
    int64_t deadline = pd_iq_monotonic_ns() + (int64_t)seconds * 1000000000;
    int healthy = 1;
    while (!g_stop && pd_iq_monotonic_ns() < deadline) {
        struct pollfd p = { w->local_iq.fd, POLLIN, 0 };
        int ready = poll(&p, 1,100);
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        if (ready < 0 || p.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            healthy = 0;
            break;
        }
        if (ready && !local_feedback_poll(w)) {
            healthy = 0;
            break;
        }
    }
    uint64_t cumulative = 0, p99 = 0;
    for (unsigned i = 0; i < 256; ++i) {
        cumulative += w->local_age_bins[i];
        if (cumulative >= (w->local_iq.frames * 99 + 99) / 100) {
            p99 = (i + 1) * 100000;
            break;
        }
    }
    unsigned long resident = 0;
    FILE* status = fopen("/proc/self/status", "r");
    if (status) {
        char line[256];
        while (fgets(line, sizeof line, status)) {
            if (sscanf(line, "VmRSS: %lu", &resident) == 1) {
                break;
            }
        }
        fclose(status);
    }
    struct rusage usage = { 0 };
    getrusage(RUSAGE_SELF, &usage);
    printf("{\"mode\":\"waveform-local-iq-probe\",\"frames\":%llu,\"feedback_handler_packets\":%ld,"
           "\"stale\":%llu,\"invalid\":%llu,\"starts\":%llu,\"duplicates\":%llu,\"old\":%llu,"
           "\"foreign\":%llu,\"age_min_ns\":%lld,\"age_mean_ns\":%lld,\"age_p99_upper_bound_ns\":%llu,"
           "\"max_age_seen_ns\":%lld,\"ref_fifo\":%d,\"feedback_fifo\":%d,\"keyed\":%s,"
           "\"max_rss_kib\":%ld,\"rss_current_kib\":%lu,\"max_rss_may_include_launcher\":true,\"rf_commands\":[]}\n",
           (unsigned long long)w->local_iq.frames, w->n_fb_pkts,
           (unsigned long long)w->local_iq.stale, (unsigned long long)w->local_iq.invalid,
           (unsigned long long)w->local_iq.starts, (unsigned long long)w->local_iq.duplicates,
           (unsigned long long)w->local_iq.old, (unsigned long long)w->local_iq.foreign,
           (long long)(w->local_iq.frames ? w->local_min_age_ns : 0),
           (long long)(w->local_iq.frames ? w->local_age_sum_ns / w->local_iq.frames : 0),
           (unsigned long long)p99, (long long)w->local_iq.max_age_seen_ns,
           w->ref.n, w->fbk.n, w->keyed ? "true" : "false", usage.ru_maxrss, resident);
    close(w->local_iq.fd);
    pd_acquire_destroy(w->acquire);
    pd_core_destroy(w->core);
    pd_align_destroy(w->align);
    pd_eq_destroy(w->eq);
    pd_eq_destroy(w->trusted_eq);
    free(w->ref.buf); free(w->fbk.buf); free(w->acq_tx); free(w->acq_rx);
    return healthy && w->local_iq.frames > 100 ? 0 : 1;
}

static int wf_init(wf_t* w)
{
    memset(w, 0, sizeof *w);
    w->calibration.tone_spacing_hz = PD_TONE_DEFAULT_SPACING_HZ;
    w->active_slice = -1;
    w->fb_slice = -1;
    const char* gui=getenv("AETHER_PD_GUI_HANDLE");
    if (gui && *gui && (!calibration_handle(gui,&w->calibration_gui) || !w->calibration_gui)) { return 0; }
    const char* pan=getenv("AETHER_PD_FEEDBACK_PAN");
    if(pan && (!calibration_handle(pan,&w->sealed_feedback_pan) || !w->sealed_feedback_pan || !w->calibration_gui)){return 0;}
    const char* fbm = getenv("AETHER_PD_FEEDBACK");
    w->local_iq_mode = fbm && !strcmp(fbm, "local");
    w->local_iq.fd = -1;
    w->local_min_age_ns = INT64_MAX;
    w->fb_audio = !w->local_iq_mode && !(fbm && !strcmp(fbm, "iq"));
    if (fbm && *fbm && strcmp(fbm, "iq") && strcmp(fbm, "audio") && strcmp(fbm, "local")) {
        logf_("AETHER_PD_FEEDBACK must be audio, iq or local, not %s", fbm);
        return 0;
    }
    w->dax_ch = (int)env_d("AETHER_PD_DAX", 4);
    const char* fba = getenv("AETHER_PD_FB_ANT");
    snprintf(w->fb_ant, sizeof w->fb_ant, "%s", fba && *fba ? fba : "RX_A");
    for (const char* c = w->fb_ant; *c; c++)       // it goes into a radio command
        if (!((*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '_')) {
            logf_("bad AETHER_PD_FB_ANT %s", w->fb_ant);
            return 0;
        }
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
    const char* deadline = getenv("AETHER_PD_TX_DEADLINE_MS");
    if (deadline && *deadline) {
        char* end;
        double ms = strtod(deadline, &end);
        if (*end || !isfinite(ms) || ms < 2 || ms > 25) {
            logf_("AETHER_PD_TX_DEADLINE_MS must be 2..25 ms");
            return 0;
        }
        w->tx_deadline_ns = (int64_t)(ms * 1000000);
    }
    const char* trace = getenv("AETHER_PD_TX_TRACE");
    if (trace && *trace) {
        w->tx_trace = calloc(8192, sizeof(*w->tx_trace));
        w->tx_trace_file = fopen(trace, "wb");
        if (!w->tx_trace || !w->tx_trace_file) { return 0; }
    }
    w->peak = env_d("AETHER_PD_PEAK", 0.7);
    w->gain = env_d("AETHER_PD_MIC_GAIN", 1.0);
    const char* raw = getenv("AETHER_PD_RAW");
    if (raw && *raw) {
        w->raw = fopen(raw, "wb");
        w->raw_left = 15L * RATE;
    }
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
    const char* test_seconds = getenv("AETHER_PD_TEST_SECONDS");
    if (test_seconds && *test_seconds) {
        char* end;
        double seconds = strtod(test_seconds, &end);
        if (*end || !isfinite(seconds) || seconds < .25 || seconds > 15 ||
            !(w->test_f1 > 0) || !(w->test_f2 > 0) ||
            !isfinite(w->test_f1) || !isfinite(w->test_f2)) {
            logf_("AETHER_PD_TEST_SECONDS requires valid test tones and .25..15s");
            return 0;
        }
        w->test_duration_samples = w->test_left = (int)(seconds*RATE);
    }
    w->correct = (int)env_d("AETHER_PD_CORRECT", 1);
    w->transport_cache_enabled = env_d("AETHER_PD_TRANSPORT_CACHE", 0) == 1;
    w->transport_eq_distance = -1;
    w->daxiq_ch = (int)env_d("AETHER_PD_DAXIQ", 1);
    w->daxiq_rate = (int)env_d("AETHER_PD_DAXIQ_RATE", 48000);
    if (w->daxiq_rate < RATE || w->daxiq_rate % RATE) {
        logf_("AETHER_PD_DAXIQ_RATE must be a multiple of %d", RATE);
        return 0;
    }
    int max_delay = (int)(env_d("AETHER_PD_MAX_DELAY_MS", 400) * RATE / 1000);
    w->measurement_guard_ns=(int64_t)max_delay*1000000000/RATE+100000000;
    w->core = pd_core_create(RATE, MAX_SAMPLES, w->peak);
    w->align = pd_align_create(max_delay, max_delay + 4 * BLOCK + 512);
    w->eq = pd_eq_create(EQ_TAPS);
    w->trusted_eq = pd_eq_create(EQ_TAPS);
    w->ref.buf = calloc(2 * FIFO_LEN, sizeof(double));
    w->fbk.buf = calloc(2 * FIFO_LEN, sizeof(double));
    w->acq_len = max_delay + 8192;
    w->eq_train_len=((EQ_TRAIN_SAMPLES+EQ_TAPS+BLOCK-1)/BLOCK)*BLOCK;
    w->eq_settle_left = EQ_PHASE_SETTLE_SAMPLES;
    int capacity=w->acq_len>w->eq_train_len ? w->acq_len : w->eq_train_len;
    w->acq_tx = calloc(2 * (size_t)capacity, sizeof(double));
    w->acq_rx = calloc(2 * (size_t)capacity, sizeof(double));
    if (!w->core || !w->align || !w->eq || !w->trusted_eq || !w->ref.buf || !w->fbk.buf || !w->acq_tx || !w->acq_rx)
        return 0;
    w->reference_floor = w->peak*w->peak*1e-4;
    if (!pd_align_set_reference_floor(w->align, w->reference_floor)) { return 0; }
    w->acquire = pd_acquire_create_with_eq(max_delay, capacity, RATE, EQ_TAPS);
    if (!w->acquire) {
        return 0;
    }
    w->acquisition_generation = 1;
    w->reference_generation = 1;
    cbp_init(&w->mod, 200, 2800);
    cbp_init(&w->noise_bp, TEST_NOISE_LO_HZ, TEST_NOISE_HI_HZ);
    w->noise_seed = 12345;
    decim_init(&w->fb, w->daxiq_rate, 0);
    return 1;
}


static struct pd_calibration_reply operator_calibration_command(wf_t* w,
    const unsigned char* request, size_t size, int64_t now)
{
    int correction_selection=size==9 && request && (request[0]=='0' || request[0]=='1');
    int measurement_arm=request && ((size==9 && request[0]=='M') || (size==11 && request[0]=='Q'));
    int eligible=w->calibration_control && w->calibration_idle && w->calibration_gui && !w->keyed && !w->tx_on && w->api>=0 && w->api_leased && w->tx_in &&
        w->active_slice>=0 && w->active_slice<16 && w->calibration_slice_owner[w->active_slice]==w->calibration_gui &&
        (w->correct || correction_selection || measurement_arm) && !native_apd_blocked(w) && w->local_iq_mode &&
        feedback_binding_valid(w) && w->local_iq.frames>0 && w->daxiq_sid && w->tx_mhz>0 && w->pan_mhz>0 &&
        w->local_iq.last_valid_ns>0 && now>=w->local_iq.last_valid_ns && now-w->local_iq.last_valid_ns<=100000000 &&
        !test_source_active(w);
    uint64_t previous_nonce=w->calibration.nonce;
    struct pd_calibration_reply reply=pd_calibration_command(&w->calibration,request,(size_t)size,now,eligible);
    if (w->calibration.nonce>previous_nonce && reply.result==PD_CAL_BLOCKED && request && ((size==9 && request[0]=='A') || measurement_arm)) {
        logf_("source arm refused: purpose=%c control=%d idle=%d gui=%08x keyed=%d tx_on=%d api=%d leased=%d registered=%d slice=%d owner=%08x correction=%d native_apd=%d local_iq=%d frames=%llu stream=%08x tx_mhz=%.6f pan_mhz=%.6f iq_age_ns=%lld test_active=%d",
            request[0],w->calibration_control,w->calibration_idle,w->calibration_gui,w->keyed,w->tx_on,w->api>=0,w->api_leased,w->tx_in!=0,
            w->active_slice,w->active_slice>=0 && w->active_slice<16?w->calibration_slice_owner[w->active_slice]:0,
            w->correct,native_apd_blocked(w),w->local_iq_mode,(unsigned long long)w->local_iq.frames,w->daxiq_sid,w->tx_mhz,w->pan_mhz,
            (long long)(w->local_iq.last_valid_ns>0 && now>=w->local_iq.last_valid_ns?now-w->local_iq.last_valid_ns:-1),test_source_active(w));
    }
    if (reply.applied && reply.result==PD_CAL_OK) {
        if (request[0]=='A') {
            // Model invalidation cancels an old arm; retain this
            // explicitly admitted new arm and its replay counter.
            struct pd_calibration accepted=w->calibration;
            w->test_started=0;model_changed(w);w->calibration=accepted;
        } else if (request[0]=='X') { disarm_bounded_test(w, "operator_cancel"); }
        else if (correction_selection && w->correct != (request[0]=='1')) {
            w->correct=request[0]=='1';
            model_changed(w);
        }
    }
    return reply;
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
static int status_feedback_binding(const wf_t* w,char* out,size_t cap)
{
    if(!w->sealed_feedback_pan){if(cap){out[0]=0;}return 0;}
    return snprintf(out,cap,"\"feedback_binding\":{\"transport\":\"%s\",\"channel\":%d,\"pan\":%u,\"gui_handle\":%u,\"valid\":%s,\"failed\":%s},",
        w->local_iq_mode?"local_dax_iq":"unsupported",w->daxiq_ch,w->sealed_feedback_pan,w->calibration_gui,
        w->local_iq_mode && feedback_binding_valid(w)?"true":"false",w->feedback_binding_failed?"true":"false");
}

static int status_json(const wf_t* w, char* out, size_t cap)
{
    struct sched_param scheduling = {0};
    int policy = -1;
    if (pthread_getschedparam(pthread_self(), &policy, &scheduling)) {
        policy = -1;
        scheduling.sched_priority = -1;
    }
    int counts[16] = {0};
    int full_bins = pd_core_collection_counts(w->core, counts);
    char counts_json[96] = {0};
    size_t count_size = 0;
    for (int i = 0; i < 16; ++i) {
        int written = snprintf(counts_json + count_size, sizeof(counts_json) - count_size,
                               i ? ",%d" : "%d", counts[i]);
        if (written < 0 || (size_t)written >= sizeof(counts_json) - count_size) return -1;
        count_size += (size_t)written;
    }
    char binding[192];int binding_size=status_feedback_binding(w,binding,sizeof(binding));
    if(binding_size<0 || binding_size>=(int)sizeof(binding)){return -1;}
    return snprintf(out, cap,
        "{%s\"name\":\"%s\",\"version\":\"%s\",\"registered\":%s,\"active_slice\":%d,"
        "\"tx_mhz\":%.6f,\"pan_mhz\":%.6f,\"daxiq_stream\":\"0x%08x\",\"daxiq_reply\":\"%s\",\"keyed\":%s,"
        "\"feedback_ok\":%s,\"correction\":%s,\"aligned\":%s,\"delay_samples\":%.2f,"
        "\"delay_corr\":%.3f,\"coherence\":%.3f,\"offset_hz\":%.4f,\"fits\":%d,"
        "\"correcting\":%s,\"calc_state\":%d,\"tx_pkts\":%ld,\"tx_held\":%ld,\"fb_pkts\":%ld,"
        "\"other_pkts\":%ld,\"other_last\":\"sid 0x%08x cls 0x%04x\",\"ref_fifo\":%d,"
        "\"fb_fifo\":%d,\"acq\":%d,\"eq_ready\":%s,\"eq_residual_db\":%.1f,"
        "\"feedback\":\"%s\",\"fb_slice\":%d,\"fb_mhz\":%.6f,\"fb_dax_stream\":\"0x%08x\",\"local_iq_fault\":%s,\"local_iq_frames\":%llu,\"local_iq_stale\":%llu,\"tx_rejected\":%llu,\"model_resets\":%llu,\"model_recoveries\":%llu,\"rejected_fits\":%u,\"cached_model\":%s,\"warm_starts\":%llu,\"cached_packets\":%llu,\"tx_sent_datagrams\":%llu,\"tx_send_errors\":%llu,\"tx_max_send_gap_ns\":%lld,\"tx_max_work_ns\":%lld,\"tx_trace_rows\":%u,\"tx_trace_dropped\":%u,\"scheduler_policy\":%d,\"scheduler_priority\":%d,\"tx_deadline_faults\":%llu,\"api_leased\":%s,\"native_apd_blocked\":%s,\"collection_full_bins\":%d,\"collection_bin_counts\":[%s],\"transport_cache_enabled\":%s,\"transport_pending\":%s,\"transport_recoveries\":%llu,\"transport_eq_distance\":%.6f,\"mic_peak\":%.6f,\"prelimit_peak\":%.6f,\"limited_samples\":%llu,\"generated_samples\":%llu,\"quiet_blocks\":%llu,\"reference_power\":%.9f,\"signal_present\":%s,\"test_active\":%s,\"test_remaining_samples\":%d,\"mic_gain\":%.6f,\"base_peak\":%.6f,\"measurement\":{\"source_id\":\"%016llx\",\"tones\":%s},\"measurement_control\":%s,\"correction_control\":%s,\"measurement_tone_control\":%s,\"measurement_tone_capture_version\":3,\"measurement_tone_config\":{\"min_spacing_hz\":%u,\"max_spacing_hz\":%u,\"center_hz\":%d,\"spacing_hz\":%u,\"source_id\":\"%016llx\"},\"source_purpose\":\"%s\",\"calibration_control\":%s,\"calibration_state\":\"%s\",\"calibration_samples_left\":%d}\n",
        binding, WF_NAME, WF_VERSION, w->tx_in ? "true" : "false", w->active_slice, w->tx_mhz, w->pan_mhz,
        w->daxiq_sid, w->daxiq_reply, w->keyed ? "true" : "false", w->feedback_ok ? "true" : "false",
        w->correct ? "true" : "false", w->aligned ? "true" : "false",
        pd_align_delay(w->align), pd_align_estimate_corr(w->align), pd_align_coherence(w->align),
        pd_align_offset_hz(w->align, RATE), pd_core_fits(w->core),
        w->correct && !w->local_iq_fault && correction_ready(w) ? "true" : "false", pd_core_state(w->core),
        w->n_tx_pkts, w->n_tx_held, w->n_fb_pkts, w->n_fb_other, w->last_sid, w->last_cls, w->ref.n, w->fbk.n, w->acq_n,
        pd_eq_ready(w->eq) ? "true" : "false", 10 * log10(pd_eq_residual(w->eq) + 1e-30),
        w->local_iq_mode ? "local" : w->fb_audio ? "audio" : "iq", w->fb_slice, w->fb_mhz, w->fb_dax_sid,
        w->local_iq_fault ? "true" : "false", (unsigned long long)w->local_iq.frames,
        (unsigned long long)w->local_iq.stale, (unsigned long long)w->tx_rejected,
        (unsigned long long)w->correction_resets, (unsigned long long)w->correction_recoveries,
        pd_core_rejected_fits(w->core), pd_core_cached_solution(w->core) ? "true" : "false",
        (unsigned long long)w->warm_starts, (unsigned long long)w->cached_packets,
        (unsigned long long)w->tx_sent_datagrams, (unsigned long long)w->tx_send_errors,
        (long long)w->tx_max_send_gap_ns, (long long)w->tx_max_work_ns,
        w->tx_trace_rows, w->tx_trace_dropped, policy, scheduling.sched_priority,
        (unsigned long long)w->tx_deadline_faults, w->api_leased ? "true" : "false",
        native_apd_blocked(w) ? "true" : "false", full_bins, counts_json, w->transport_cache_enabled ? "true" : "false", w->transport_pending ? "true" : "false", (unsigned long long)w->transport_recoveries, w->transport_eq_distance, w->mic_peak, w->prelimit_peak,
        (unsigned long long)w->limited_samples, (unsigned long long)w->generated_samples,
        (unsigned long long)w->quiet_blocks, pd_align_reference_power(w->align),
        pd_align_signal_present(w->align) ? "true" : "false", test_source_active(w) ? "true" : "false",
        test_source_active(w) && !w->test_duration_samples ? -1 : w->test_left, w->gain, w->peak, (unsigned long long)measurement_source_id(w),
        measurement_tones_ready(w,pd_iq_monotonic_ns()) ? "true" : "false", w->calibration_control ? "true" : "false", w->calibration_control ? "true" : "false",
        w->calibration_control ? "true" : "false", (unsigned)PD_TONE_MIN_SPACING_HZ, (unsigned)PD_TONE_MAX_SPACING_HZ,
        PD_TONE_CENTER_HZ, w->calibration.tone_spacing_hz, (unsigned long long)measurement_source_id(w),
        w->calibration.purpose==PD_SOURCE_MEASUREMENT ? "measurement" : "calibration", w->calibration_control ? "true" : "false",
        pd_calibration_name(&w->calibration), w->calibration.samples_left);
}

static void serve_status(const wf_t* w, int ls)
{
    struct sockaddr_in a;
    socklen_t al = sizeof a;
    int fd = accept(ls, (struct sockaddr*)&a, &al);
    if (fd < 0)
        return;
    if (lan_caller(&a)) {
        char body[4096];
        int n = status_json(w, body, sizeof body);
        if (n > 0 && (size_t)n < sizeof body && write(fd, body, (size_t)n) != n)
            logf_("status write short");
    }
    close(fd);
}

/* ----------------------------------------------------------- selftest */

static int selftest_loop(int audio);

static int selftest(void)
{
    int fails = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
    // Publication from work queued before a reset cannot revive its model.
    pd_core* fence = pd_core_create(RATE, BLOCK, 0.7);
    CHECK(fence != NULL);
    if (!fence) {
        return 1;
    }
    pd_fit_request(0);
    CHECK(pd_fit_publish(0) == 2);
    pd_fit_request(0);
    CHECK(pd_fit_publish(0) == 1);
    pd_fit_request(0);
    pd_core_invalidate(fence);
    CHECK(!pd_core_current_solution(fence));
    CHECK(pd_fit_publish(0) == 0 && pd_core_rejected_fits(fence) == 1);
    double clean[4] = { 0.1, -0.2, 0.3, -0.4 };
    pd_core_correct(fence, clean, 2);
    CHECK(fabs(clean[0] - 0.1) < 1e-12 && fabs(clean[3] + 0.4) < 1e-12);
    pd_fit_request(0);
    CHECK(pd_fit_publish(0) == 2);
    pd_core_destroy(fence);
    wf_t w;
    CHECK(wf_init(&w));
    w.fb_audio = 0;     // these checks cover the DAX IQ route's pan guard
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

    // Partial status updates retire the model only for its bound resources.
    uint64_t resets = w.correction_resets;
    char partial_pan[] = "S1|display pan 0x40000001 center=14.202000";
    api_line(&w, partial_pan);
    CHECK(fabs(w.pan_mhz - 14.202) < 1e-9 && w.correction_resets > resets);
    resets = w.correction_resets;
    api_line(&w, partial_pan);
    CHECK(w.correction_resets == resets);
    char other_pan[] = "S1|display pan 0x40000002 center=50.0 rxant=ANT2";
    api_line(&w, other_pan);
    CHECK(w.correction_resets == resets && w.feedback_pan == 0x40000001u);
    char antenna[] = "S1|slice 1 txant=ANT2";
    api_line(&w, antenna);
    CHECK(w.correction_resets > resets);
    resets = w.correction_resets;
    char frequency[] = "S1|slice 1 RF_frequency=14.201000";
    api_line(&w, frequency);
    CHECK(w.correction_resets > resets && fabs(w.tx_mhz-14.201) < 1e-9);
    resets = w.correction_resets;
    char receiver_gain[] = "S1|display pan 0x40000001 rfgain=10";
    api_line(&w, receiver_gain);
    CHECK(w.correction_resets > resets);
    char unbind[] = "S1|display pan 0x40000001 daxiq_channel=0";
    api_line(&w, unbind);
    CHECK(!w.feedback_pan && w.pan_mhz == 0);
    char removed_slice[] = "S1|slice 1 in_use=0";
    api_line(&w, removed_slice);
    CHECK(w.active_slice == -1 && !w.tx_on && !w.keyed);
    api_line(&w, s7);
    api_line(&w, s2);
    char removed_pan[] = "S1|display pan 0x40000001 removed";
    api_line(&w, removed_pan);
    CHECK(!w.feedback_pan && w.pan_mhz == 0);
    CHECK(parse_mhz("nan") == 0 && parse_mhz("14.2bad") == 0);

    // VITA: build then parse round-trips stream, class, count and samples.
    float pairs[6] = { 0.5f, -0.25f, 1.0f, 0.0f, -1.0f, 0.125f };
    unsigned char pkt[28 + 4 * 6];
    int len = vita_build(pkt, 0x84000001u, CLASS_AUDIO_24K, 5, pairs, 3);
    vita_t v;
    CHECK(len == 28 + 24 && vita_parse(pkt, len, &v));
    CHECK(v.sid == 0x84000001u && v.cls == CLASS_AUDIO_24K && v.count == 5 && v.words == 6);
    CHECK(word_float(v.payload + 4) == -0.25f && word_float(v.payload + 20) == 0.125f);
    w.tx_on = 1;
    long tx_before = w.n_tx_pkts;
    put32(pkt+4, 0x84000003u); // same class/parity, another stream
    on_packet(&w, pkt, len);
    CHECK(w.n_tx_pkts == tx_before);
    put32(pkt+4, w.tx_in);
    on_packet(&w, pkt, len);
    CHECK(w.n_tx_pkts == tx_before+1);
    on_packet(&w, pkt, len); // exact duplicate
    CHECK(w.n_tx_pkts == tx_before+1 && w.tx_rejected == 1);
    w.tx_on = 0;
    on_packet(&w, pkt, len);
    CHECK(w.n_tx_pkts == tx_before+1);
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
    char js[2048];
    int jn = status_json(&w, js, sizeof js);
    CHECK(jn > 0 && (size_t)jn < sizeof js && js[0] == '{' && js[jn - 2] == '}');
    CHECK(strstr(js, "\"registered\":true") && strstr(js, "\"daxiq_stream\":\"0x20000001\"")
          && strstr(js, "\"delay_samples\":") && strstr(js, "\"coherence\":"));
    struct sockaddr_in lan = { .sin_family = AF_INET }, wan = { .sin_family = AF_INET };
    inet_pton(AF_INET, "192.168.50.20", &lan.sin_addr);
    inet_pton(AF_INET, "100.109.252.61", &wan.sin_addr);
    CHECK(lan_caller(&lan) && !lan_caller(&wan));

    w.local_iq_mode = 1;
    w.keyed = 1;
    w.aligned = 1;
    w.ref.n = w.fbk.n = 100;
    uint64_t previous_generation = w.acquisition_generation;
    local_feedback_invalidate(&w, 1);
    CHECK(w.local_iq_fault && !w.aligned && !w.ref.n && !w.fbk.n);
    CHECK(w.acquisition_generation > previous_generation);
    local_feedback_invalidate(&w, 0);
    CHECK(w.local_iq_fault);
    w.local_iq.last_valid_ns = 1000;
    w.local_iq.last_receive_ns = 100001001;
    CHECK(local_feedback_expired(&w, 100001001));
    CHECK(!local_feedback_expired(&w, 100001000));
    w.keyed = 0;
    close(w.api);
    pd_acquire_destroy(w.acquire);
    pd_core_destroy(w.core);
    pd_align_destroy(w.align);
    pd_eq_destroy(w.eq);
    pd_eq_destroy(w.trusted_eq);
    free(w.ref.buf); free(w.fbk.buf); free(w.acq_tx); free(w.acq_rx);
    w.core = NULL;
    fails += selftest_loop(0);
    fails += selftest_loop(1);
    fails += selftest_loop(2);
    fails += selftest_loop(3);
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
    int local_fd, drop_one;
    int audio;                 // feedback as DAX audio from a slice FB_OFFSET_HZ below
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
        if (l->local_fd >= 0) {
            uint64_t stamp = (uint64_t)(l->count-1) * 128 * UINT64_C(1000000000000) / 48000;
            put32(p+16, (uint32_t)(1794440000 + stamp/UINT64_C(1000000000000)));
            put32(p+20, (uint32_t)((stamp%UINT64_C(1000000000000)) >> 32));
            put32(p+24, (uint32_t)(stamp%UINT64_C(1000000000000)));
            if (l->drop_one) {
                l->drop_one = 0;
            } else {
                unsigned char envelope[PD_IQ_HEADER + sizeof(p)];
                memcpy(envelope, "PDIQ", 4); pd_iq_put32(envelope+4, 1);
                pd_iq_put64(envelope+8, 1);
                struct timespec real;
                clock_gettime(CLOCK_REALTIME, &real);
                uint64_t rt = (uint64_t)real.tv_sec*1000000000 + real.tv_nsec;
                uint64_t mt = (uint64_t)pd_iq_monotonic_ns();
                pd_iq_put64(envelope+16, rt); pd_iq_put64(envelope+24, rt);
                pd_iq_put64(envelope+32, mt); pd_iq_put64(envelope+40, mt);
                pd_iq_put32(envelope+48, (uint32_t)len); pd_iq_put32(envelope+52, 0);
                memcpy(envelope+PD_IQ_HEADER, p, (size_t)len);
                if (send(l->local_fd, envelope, PD_IQ_HEADER+(size_t)len, 0) < 0 || !local_feedback_poll(l->w)) {
                    abort();
                }
            }
        } else {
            on_packet(l->w, p, len);
        }
        l->npkt = 0;
    }
}

// The audio route: the delayed PA output as a USB slice FB_OFFSET_HZ below
// the carrier hears it, real audio at 24 kHz, sent as DAX audio frames.
static void loop_emit_audio(loop_t* l, double i, double q)
{
    int r = l->pos;
    double di = l->line[2 * r], dq = l->line[2 * r + 1];
    l->line[2 * r] = i;
    l->line[2 * r + 1] = q;
    l->pos = (l->pos + 1) % l->len;
    double a = 0.6 * (di * cos(l->nco) - dq * sin(l->nco));
    l->nco = fmod(l->nco + 2 * M_PI * FB_OFFSET_HZ / RATE, 2 * M_PI);
    l->pkt[2 * l->npkt] = l->pkt[2 * l->npkt + 1] = (float)a;
    if (++l->npkt == 128) {
        unsigned char p[28 + 4 * 256];
        int len = vita_build(p, l->w->fb_dax_sid, CLASS_AUDIO_24K, l->count++, l->pkt, 128);
        p[0] = (unsigned char)((p[0] & 0x0F) | 0x30);   // DAX audio is VITA type 3
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
        if (l->audio) {
            loop_emit_audio(l, yi, yq);
        } else {
            // to 48 kHz by linear interpolation
            loop_emit(l, 0.5 * (l->last_i + yi), 0.5 * (l->last_q + yq));
            loop_emit(l, yi, yq);
        }
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

static int selftest_loop(int mode)
{
    const int audio = mode == 1;
    const int local = mode >= 2;
    const int warm = mode == 3;
    int fails = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
    wf_t w;
    CHECK(wf_init(&w));
    w.fb_audio = audio;
    w.local_iq_mode = local;
    w.api = open("/dev/null", O_WRONLY);
    // The audio route runs at 24 kHz: half the samples for the same delay.
    loop_t l = { .w = &w, .len = audio ? LOOP_DELAY / 2 : LOOP_DELAY, .audio = audio, .local_fd = -1 };
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
    if (audio) {
        // Selecting PD creates the feedback slice; answer as the radio would.
        api_line(&w, s1);
        CHECK(w.active_slice == 1 && w.seq_fbslice > 0);
        char rs[32], rd[48], fs[64];
        snprintf(rs, sizeof rs, "R%d|0|5", w.seq_fbslice);
        api_line(&w, rs);
        CHECK(w.fb_slice == 5 && w.seq_daxrx > 0);
        snprintf(rd, sizeof rd, "R%d|0|0x04000008", w.seq_daxrx);
        api_line(&w, rd);
        CHECK(w.fb_dax_sid == 0x04000008u);
        snprintf(fs, sizeof fs, "S1|slice 5 in_use=1 RF_frequency=14.196500 mode=DIGU");
        api_line(&w, fs);
        CHECK(fabs(w.fb_mhz - 14.1965) < 1e-9);
        api_line(&w, s3);
    } else {
        api_line(&w, r2);
        api_line(&w, s1);
        api_line(&w, s2);
        if (local) {
            int endpoints[2];
            CHECK(socketpair(AF_UNIX, SOCK_DGRAM, 0, endpoints) == 0);
            l.local_fd = endpoints[0];
            w.local_iq.fd = endpoints[1];
            CHECK(pd_guard_init(&w.local_iq.guard, w.daxiq_sid, 1, 48000, 25000000));
            // Start the receive stream before PTT, as the broker does.
            for (int j = 0; j < 128; ++j) {
                loop_emit(&l, 0, 0);
            }
        }
        api_line(&w, s3);
    }
    CHECK(w.feedback_ok);

    int fault_stage = 0;
    uint64_t last_recovery = 0;
    unsigned seed = 7;
    double lp = 0, env = 0.5;
    float mic[2 * 128];
    unsigned char p[28 + 4 * 256];
    int count = 0;
    struct timespec t0, now;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (long k = 0; k < 24000L * (local ? 150 : 30); k += 128) {
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
        int inject = 0;
        if (local && !warm && pd_core_current_solution(w.core) && !w.local_iq_fault &&
            w.correction_recoveries > last_recovery) {
            if (!fault_stage) {
                CHECK(w.aligned && pd_eq_ready(w.eq));
                ++count; // lose one complete TX reference packet
                inject = 1;
            } else if (fault_stage == 1) {
                l.drop_one = 1; // lose one broker IQ datagram
                inject = 1;
            } else if (fault_stage == 2) {
                w.tx_age_ns = 150000000; // stale TX packet after a service stall
                inject = 1;
            } else if (fault_stage == 3) {
                char power[] = "S1|transmit rfpower=9";
                api_line(&w, power);
                CHECK(!pd_core_current_solution(w.core) && !w.aligned);
                inject = 1;
            }
            if (inject) {
                last_recovery = w.correction_recoveries;
                ++fault_stage;
            }
        }
        int len = vita_build(p, 0x84000001u, CLASS_AUDIO_24K, count++, mic, 128);
        on_packet(&w, p, len);
        w.tx_age_ns = 0;
        if (inject) {
            CHECK(w.local_iq_fault && !pd_core_current_solution(w.core) && !w.aligned);
        }
        // The unpaced simulator yields while the background EQ fit runs.
        if (w.eq_pending) {
            struct timespec delay = { 0, 1000000 };
            nanosleep(&delay, NULL);
        }
        // Let a running fit finish, as real time would.
        for (int wait = 0; wait < 2000 && pd_core_state(w.core) == 6 && !pd_core_swap_pending(w.core); wait++) {
            extern void pscc(int, int, double*, double*);
            double none[2] = { 0, 0 };
            pscc(0, 0, none, none);
            struct timespec d = { 0, 500000 };
            nanosleep(&d, NULL);
        }
        if (((!local || warm) && pd_core_fits(w.core) >= 3 && !w.local_iq_fault) ||
            (local && !warm && fault_stage == 4 && w.correction_recoveries > last_recovery && !w.local_iq_fault))
            break;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec - t0.tv_sec > 40)
            break;
    }
    // 1234 samples at 48 kHz, plus the feedback decimator's half length,
    // seen at 24 kHz; on the audio route, 617 at 24 kHz plus the analytic
    // filter's half length.
    double want = audio ? LOOP_DELAY / 2 + (TAPS - 1) / 2.0 : LOOP_DELAY / 2.0 + (TAPS - 1) / 4.0;
    CHECK(w.aligned);
    CHECK(local || fabs(pd_align_delay(w.align) - want) < 2.0);
    CHECK(pd_align_estimate_corr(w.align) > 0.8);
    CHECK(pd_align_coherence(w.align) > 0.8);
    CHECK(pd_core_fits(w.core) >= 1);
    if (local && !warm) {
        CHECK(fault_stage == 4 && w.correction_recoveries >= 5 && !w.local_iq_fault);
        CHECK(w.tx_rejected >= 1 && w.local_iq.starts >= 2);
        printf("local recovery: stages %d, recoveries %llu, TX rejected %llu, IQ restarts %llu\n",
               fault_stage, (unsigned long long)w.correction_recoveries,
               (unsigned long long)w.tx_rejected, (unsigned long long)w.local_iq.starts);
    }
    printf("loop (%s): delay %.2f (want ~%.2f), correlation %.2f, coherence %.2f, fits %d, correcting %d\n",
           audio ? "DAX audio" : "DAX IQ", pd_align_delay(w.align), want, pd_align_estimate_corr(w.align), pd_align_coherence(w.align),
           pd_core_fits(w.core), pd_core_correcting(w.core));

    if (warm) {
        int learned_fits = pd_core_fits(w.core);
        int64_t maximum_warm_ns = 0;
        for (int cycle = 0; cycle < 6; ++cycle) {
            char unkey[] = "S1|interlock state=UNKEY_REQUESTED";
            char idle[] = "S1|interlock state=READY";
            api_line(&w, unkey); api_line(&w, idle);
            CHECK(!w.keyed && !w.tx_on && pd_core_cached_solution(w.core));
            int64_t idle_start = pd_iq_monotonic_ns();
            for (int frame = 0; frame < 16; ++frame) {
                int64_t deadline = idle_start + (int64_t)frame * 128 * 1000000000 / 48000;
                struct timespec when = { deadline / 1000000000, deadline % 1000000000 };
                clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &when, NULL);
                for (int i = 0; i < 128; ++i) { loop_emit(&l, 0, 0); }
            }
            char ptt[] = "S1|interlock state=PTT_REQUESTED";
            api_line(&w, ptt); api_line(&w, s3);
            CHECK(w.warm_start_armed && !correction_ready(&w));
            uint64_t cached_before = w.cached_packets;
            int64_t burst_start = pd_iq_monotonic_ns(), first_cached_ns = 0;
            for (int frame = 0; frame < 38; ++frame) {
                int64_t deadline = burst_start + (int64_t)frame * 128 * 1000000000 / RATE;
                struct timespec when = { deadline / 1000000000, deadline % 1000000000 };
                clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &when, NULL);
                int bytes = vita_build(p, w.tx_in, CLASS_AUDIO_24K, count++, mic, 128);
                on_packet(&w, p, bytes);
                if (!first_cached_ns && w.cached_packets > cached_before) {
                    first_cached_ns = pd_iq_monotonic_ns() - burst_start;
                }
            }
            if (first_cached_ns > maximum_warm_ns) { maximum_warm_ns = first_cached_ns; }
            CHECK(first_cached_ns > 0 && first_cached_ns < 100000000);
            CHECK(w.cached_packets > cached_before && w.warm_started && !w.aligned);
            CHECK(pd_core_fits(w.core) == learned_fits);
        }
        printf("warm starts: %llu, cached packets %llu, six 203 ms bursts, max key-to-cached dispatch %.3f ms\n",
               (unsigned long long)w.warm_starts, (unsigned long long)w.cached_packets, maximum_warm_ns/1e6);
        CHECK(w.warm_starts == 6);
        char changed[] = "S1|transmit rfpower=8";
        api_line(&w, changed);
        CHECK(!pd_core_cached_solution(w.core) && !w.warm_start_armed && !correction_ready(&w));
        int bytes = vita_build(p, w.tx_in, CLASS_AUDIO_24K, count++, mic, 128);
        uint64_t cached_before = w.cached_packets;
        on_packet(&w, p, bytes);
        CHECK(w.cached_packets == cached_before);
    }

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
    if (audio) {
        // Releasing PD removes the feedback slice.
        char rel[] = "S1|slice 1 mode=USB";
        api_line(&w, rel);
        CHECK(w.fb_slice == -1 && w.fb_dax_sid == 0);
    }
    close(w.api);
    if (local) {
        close(l.local_fd);
        close(w.local_iq.fd);
    }
    pd_acquire_destroy(w.acquire);
    pd_core_destroy(w.core);
    pd_align_destroy(w.align);
    pd_eq_destroy(w.eq);
    pd_eq_destroy(w.trusted_eq);
    free(w.ref.buf); free(w.fbk.buf); free(w.acq_tx); free(w.acq_rx);
    free(l.line);
    return fails;
#undef CHECK
}

/* --------------------------------------------------------------- main */

static int connect_api(wf_t* w, const char* addr, int udp_port)
{
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(4992) };
    inet_pton(AF_INET, addr, &sa.sin_addr);
    const char* inherited = getenv("AETHER_PD_API_FD");
    if (inherited && *inherited) {
        w->api_leased = 1;
        char* end;
        errno = 0;
        long fd = strtol(inherited, &end, 10);
        struct sockaddr_in peer = {0};
        socklen_t peer_size = sizeof(peer), type_size = sizeof(int);
        int type = 0;
        if (w->api_inherited_used || errno || *end || fd < 3 || fd > 1023 ||
            fd == w->local_iq.fd ||
            getsockopt((int)fd, SOL_SOCKET, SO_TYPE, &type, &type_size) || type != SOCK_STREAM ||
            getpeername((int)fd, (struct sockaddr*)&peer, &peer_size) ||
            peer_size != sizeof(peer) || peer.sin_family != AF_INET ||
            peer.sin_port != sa.sin_port || peer.sin_addr.s_addr != sa.sin_addr.s_addr) {
            logf_("invalid or already consumed inherited API endpoint");
            w->api_lease_failed = 1;
            g_stop = 1;
            return 0;
        }
        int flags = fcntl((int)fd, F_GETFL);
        if (flags < 0 || fcntl((int)fd, F_SETFL, flags & ~O_NONBLOCK) ||
            fcntl((int)fd, F_SETFD, FD_CLOEXEC)) {
            w->api_lease_failed = 1;
            g_stop = 1;
            return 0;
        }
        w->api = (int)fd;
        w->api_inherited_used = 1;
    } else {
        w->api = socket(AF_INET, SOCK_STREAM, 0);
        if (w->api < 0 || connect(w->api, (struct sockaddr*)&sa, sizeof sa)) {
            if (w->api >= 0) { close(w->api); }
            w->api = -1;
            return 0;
        }
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
    api_send(w, "sub apd all");
    api_send(w, "sub daxiq all");
    w->seq_create = api_send(w, "waveform create name=%s mode=%s underlying_mode=RAW version=%s",
                             WF_NAME, WF_MODE, WF_VERSION);
    api_send(w, "waveform set %s tx=1", WF_NAME);
    api_send(w, "waveform set %s udpport=%d", WF_NAME, udp_port);
    if (!w->fb_audio && !w->local_iq_mode)
        w->seq_daxiq = api_send(w, "stream create type=dax_iq daxiq_channel=%d", w->daxiq_ch);
    return 1;
}

int main(int argc, char** argv)
{
    if (argc >= 2 && !strcmp(argv[1], "selftest"))
        return selftest();

    if (argc != 1 && !(argc == 3 && !strcmp(argv[1], "--feedback-probe"))) {
        fprintf(stderr, "usage: pd_waveform [selftest | --feedback-probe SECONDS]\n");
        return 2;
    }
    int owner=pd_owner_open(getenv("AETHER_PD_OWNER_FD"));
    if (owner==-2 || (owner>=0 && !pd_owner_unprivileged())) { logf_("invalid shim lifetime pipe");return 2; }
    if (owner>=0 && prctl(PR_SET_DUMPABLE,0,0,0,0)) { return 2; }
    int private_status=pd_status_open(getenv("AETHER_PD_STATUS_FD"));
    if (private_status==-2 || (private_status>=0 && (owner<0 || private_status==owner))) {
        logf_("invalid private status endpoint");return 2;
    }
    int calibration=pd_status_open(getenv("AETHER_PD_CALIBRATION_FD"));
    if (calibration==-2 || (calibration>=0 && (owner<0 || private_status<0 || calibration==owner || calibration==private_status))) {
        logf_("invalid private calibration endpoint");return 2;
    }
    int start_gate=pd_status_open(getenv("AETHER_PD_START_FD"));
    if (start_gate==-2 || (start_gate>=0 &&
        (argc!=1 || owner<0 || private_status<0 || calibration<0 ||
         start_gate==owner || start_gate==private_status || start_gate==calibration))) { return 2; }
    if (pd_owner_closed(owner)) { close(owner);return 0; }
    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);
    signal(SIGPIPE, SIG_IGN);
    wf_t w;
    if (!wf_init(&w)) {
        logf_("setup failed");
        return 1;
    }
    if (calibration>=0 && !w.calibration_gui) { return 2; }
    if (start_gate>=0 && !w.local_iq_mode) { return 2; }
    w.calibration_control = calibration>=0;
    if (argc == 3 && !strcmp(argv[1], "--feedback-probe")) {
        char* end;
        long seconds = strtol(argv[2], &end, 10);
        if (*end || seconds < 1 || seconds > 60) {
            return 2;
        }
        return local_feedback_probe(&w, (int)seconds);
    }
    if (w.local_iq_mode && !local_feedback_start(&w)) {
        logf_("local IQ endpoint unavailable");
        return 1;
    }
    if (private_status>=0 && private_status==w.local_iq.fd) { return 2; }
    if (calibration>=0 && calibration==w.local_iq.fd) { return 2; }
    if (start_gate>=0 && start_gate==w.local_iq.fd) { return 2; }
    /* Complete the private IQ handshake before its capture-side deadline.
     * Go must arm its guardians before this process registers with the API. */
    int released=pd_start_gate_wait(start_gate,owner);
    if (start_gate>=0) { close(start_gate); }
    if (released<=0) { return released==0 ? 0 : 2; }
    if (start_gate>=0) {
        int discarded=pd_local_iq_discard_startup(&w.local_iq);
        if (discarded<0) { logf_("prepared IQ startup backlog did not drain");return 2; }
        if (discarded) { logf_("discarded %d pre-activation IQ datagrams",discarded); }
    }
    if (g_stop || pd_owner_closed(owner)) { return 0; }

    const char* addr = getenv("AETHER_PD_RADIO");
    if (!addr || !*addr)
        addr = getenv("SSDR_RADIO_ADDRESS");
    if (!addr || !*addr)
        addr = "127.0.0.1";

    w.udp = socket(AF_INET, SOCK_DGRAM, 0);
    if (w.udp < 0 || !pd_udp_timestamp_enable(w.udp)) {
        logf_("UDP kernel timestamps unavailable: refusing TX input");
        return 1;
    }
    struct sockaddr_in ua = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_ANY) };
    bind(w.udp, (struct sockaddr*)&ua, sizeof ua);
    socklen_t ual = sizeof ua;
    getsockname(w.udp, (struct sockaddr*)&ua, &ual);
    int udp_port = ntohs(ua.sin_port);
    if (w.fb_audio)
        logf_("%s %s starting, radio %s, udp %d, feedback DAX audio %d from %s, peak %.2f, correction %s",
              WF_NAME, WF_VERSION, addr, udp_port, w.dax_ch, w.fb_ant, w.peak, w.correct ? "on" : "off");
    else
        logf_("%s %s starting, radio %s, udp %d, feedback DAX IQ %d at %d Hz, peak %.2f, correction %s",
              WF_NAME, WF_VERSION, addr, udp_port, w.daxiq_ch, w.daxiq_rate, w.peak, w.correct ? "on" : "off");

    int ls = -1;
    if (private_status<0) {
        ls = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in la = { .sin_family = AF_INET, .sin_port = htons(STATUS_PORT), .sin_addr.s_addr = htonl(INADDR_ANY) };
        int one = 1;
        setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        if (ls < 0 || bind(ls, (struct sockaddr*)&la, sizeof la) || listen(ls, 4)) {
            logf_("status port %d unavailable: %s", STATUS_PORT, strerror(errno));
            if (ls >= 0)
                close(ls);
            ls = -1;
            if (owner>=0) { return 1; }
        }
    }

    double next_try = 0;
    w.api = -1;
    while (!g_stop) {
        if (pd_owner_closed(owner)) { logf_("shim session ended");break; }
        if (w.api < 0 && mono() >= next_try) {
            if (!connect_api(&w, addr, udp_port)) {
                next_try = mono() + 5;
                logf_("radio not answering at %s; retrying", addr);
            }
        }
        struct pollfd pf[7] = { { w.udp, POLLIN, 0 }, { ls, POLLIN, 0 },
                                { w.api, POLLIN, 0 }, { w.local_iq.fd, POLLIN, 0 }, { owner, 0, 0 }, { private_status, POLLIN, 0 }, { calibration, POLLIN, 0 } };
        int np = 7;
        if (poll(pf, (nfds_t)np, 200) < 0 && errno != EINTR)
            break;
        if (pd_owner_closed(owner)) {
            disarm_bounded_test(&w, "shim_session_ended");logf_("shim session ended");break;
        }
        if (w.local_iq_mode && !local_feedback_poll(&w)) {
            logf_("local IQ transport failed");
            break;
        }
        if (private_status>=0 && (pf[5].revents&POLLIN)) {
            for (unsigned batch=0;batch<4;++batch) {
                struct pd_status_query query;
                int request=pd_status_query(private_status,&query);
                if (!request) { break; }
                if (request>0 && query.operation=='?') {
                    char body[4096];int size=status_json(&w,body,sizeof(body));
                    if (size>0 && (size_t)size<sizeof(body)) {
                        (void)pd_status_reply(private_status,query.nonce,body,(size_t)size);
                    }
                } else if (request>0) {
                    int64_t now=pd_iq_monotonic_ns();
                    if (query.operation=='B') {
                        struct pd_capture_meta meta=capture_meta(&w);
                        pd_capture_begin(&w.capture,query.capture_id,&meta,now);
                    }
                    unsigned char body[PD_CAPTURE_REPLY];
                    size_t size=pd_capture_read(&w.capture,query.capture_id,query.capture_offset,now,body,sizeof(body));
                    if (size) { (void)pd_status_reply(private_status,query.nonce,(const char*)body,size); }
                }
            }
        }
        if (ls >= 0 && (pf[1].revents & POLLIN))
            serve_status(&w, ls);
        if (w.api >= 0 && (pf[2].revents & (POLLIN | POLLHUP | POLLERR))) {
            ssize_t n = read(w.api, w.rbuf + w.rlen, sizeof w.rbuf - w.rlen - 1);
            if (n <= 0) {
                logf_("radio closed the API connection");
                disarm_bounded_test(&w, "api_closed");
                close(w.api);
                w.api = -1;
                w.tx_on = 0;
                set_keyed(&w, 0, 0);
                model_changed(&w);
                w.tx_in = w.rx_in = 0;
                w.active_slice = -1;
                if (w.api_leased) {
                    w.api_lease_failed = 1;
                    break;
                }
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
        if ((w.calibration.phase == PD_CAL_ARMED || w.calibration.phase == PD_CAL_RUNNING) &&
            !pd_calibration_tick(&w.calibration, pd_iq_monotonic_ns())) { disarm_bounded_test(&w, "source_deadline"); }
        if (calibration>=0 && (pf[6].revents&POLLIN)) {
            for (unsigned batch=0;batch<4;++batch) {
                unsigned char request[12];ssize_t size=recv(calibration,request,sizeof(request),MSG_DONTWAIT);
                if (size<0) { break; }
                struct pd_calibration_reply reply=operator_calibration_command(&w,request,(size_t)size,pd_iq_monotonic_ns());
                if (size==9 || size==11) {
                    unsigned char response[9];memcpy(response,request+1,8);response[8]=(unsigned char)reply.result;
                    (void)send(calibration,response,sizeof(response),MSG_DONTWAIT|MSG_NOSIGNAL);
                }
            }
        }
        if (pf[0].revents & POLLIN) {
            unsigned char p[4096];
            for (unsigned batch = 0; batch < 64; ++batch) {
                ssize_t n = pd_udp_receive(w.udp, p, sizeof(p), &w.tx_arrival_ns, &w.tx_age_ns);
                if (n <= 0) {
                    break;
                }
                on_packet(&w, p, n);
            }
        }
        if (w.keyed && mono() - w.last_report > 2) {
            w.last_report = mono();
            log_measurement_window(&w, "periodic");
            logf_("tx: aligned %d, fits %d, correcting %d, state %d, offset %.3f Hz",
                  w.aligned, pd_core_fits(w.core), w.correct && correction_ready(&w),
                  pd_core_state(w.core), pd_align_offset_hz(w.align, RATE));
            if (!correction_ready(&w)) {
                int counts[16] = {0};
                int full = pd_core_collection_counts(w.core, counts);
                logf_("calibration: key %.3f s, EQ %d residual %.1f dB, phase %d, "
                      "coverage %d/16, bins [%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d], "
                      "prelimit peak %.6f, limited %llu, acquisition pending %d, EQ pending %d, fit flags 0x%x, rejected %u",
                      (pd_iq_monotonic_ns() - w.key_started_ns) / 1e9,
                      pd_eq_ready(w.eq), 10 * log10(pd_eq_residual(w.eq) + 1e-30),
                      (int)w.calibration.phase, full,
                      counts[0], counts[1], counts[2], counts[3], counts[4], counts[5], counts[6], counts[7],
                      counts[8], counts[9], counts[10], counts[11], counts[12], counts[13], counts[14], counts[15],
                      w.prelimit_peak, (unsigned long long)w.limited_samples,
                      w.acquisition_pending, w.eq_pending, pd_core_fit_flags(w.core), pd_core_rejected_fits(w.core));
            }
        }
    }
    logf_("stopping");
    if (w.tx_trace_file) {
        fprintf(w.tx_trace_file, "arrival_ns,age_ns,handler_start_ns,send_before_ns,send_after_ns,key_started_ns,input_packets,iq_frames,model_resets,cached_packets,sid,count,samples,result,error,aligned,fault,warm_started,coherence,delay\n");
        for (unsigned i = 0; i < w.tx_trace_rows; ++i) {
            const struct pd_tx_trace_row* r = &w.tx_trace[i];
            fprintf(w.tx_trace_file, "%lld,%lld,%lld,%lld,%lld,%lld,%llu,%llu,%llu,%llu,%u,%u,%u,%d,%d,%d,%d,%d,%.9g,%.9g\n",
                    (long long)r->arrival_ns, (long long)r->age_ns, (long long)r->handler_start_ns,
                    (long long)r->send_before_ns, (long long)r->send_after_ns, (long long)r->key_started_ns,
                    (unsigned long long)r->input_packets, (unsigned long long)r->iq_frames,
                    (unsigned long long)r->model_resets, (unsigned long long)r->cached_packets, r->sid, r->count, r->samples,
                    r->result, r->error, r->aligned, r->fault, r->warm_started, r->coherence, r->delay);
        }
        fclose(w.tx_trace_file);
        free(w.tx_trace);
    }
    fb_slice_remove(&w);
    if (w.api >= 0)
        close(w.api);   // the radio drops the waveform with its client
    if (ls >= 0)
        close(ls);
    if (w.local_iq.fd >= 0) {
        close(w.local_iq.fd);
    }
    pd_acquire_destroy(w.acquire);
    pd_core_destroy(w.core);
    pd_align_destroy(w.align);
    pd_eq_destroy(w.eq);
    pd_eq_destroy(w.trusted_eq);
    if (private_status>=0) { close(private_status); }
    if (calibration>=0) { close(calibration); }
    if (owner>=0) { close(owner); }
    // The owning shim distinguishes API loss from generic failure or SIGKILL.
    // Keep this dedicated code in sync with pdAPILeaseFailureExit in the shim.
    enum { kAPILeaseFailureExit = 7 };
    return w.api_lease_failed ? kAPILeaseFailureExit : 0;
}
