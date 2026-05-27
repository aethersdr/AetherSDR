/*---------------------------------------------------------------------------*\

  rade_demod_wav.c

  RADAE WAV demodulator.  Reads a WAV file containing received RADE OFDM
  audio and writes a WAV file containing the decoded voice audio.

  Combines real2iq (Hilbert), radae_rx (OFDM demod + neural decoder), and
  the FARGAN vocoder into a single command-line tool.

\*---------------------------------------------------------------------------*/

/*
  Copyright (C) 2024 David Rowe

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions
  are met:

  - Redistributions of source code must retain the above copyright
  notice, this list of conditions and the following disclaimer.

  - Redistributions in binary form must reproduce the above copyright
  notice, this list of conditions and the following disclaimer in the
  documentation and/or other materials provided with the distribution.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
  ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
  A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR
  CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
  EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
  PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
  PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
  LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
  NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
  SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <getopt.h>

#include "rade_api.h"
#include "rade_dsp.h"
#include "rade_text.h"
#include "fargan.h"
#include "lpcnet.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---- Hilbert transform  (coefficients match real2iq.c exactly) ---- */

#define HILBERT_NTAPS   127
#define HILBERT_DELAY   ((HILBERT_NTAPS - 1) / 2)   /* 63 */

static float hilbert_coeffs[HILBERT_NTAPS];

static void init_hilbert(void) {
    int center = HILBERT_DELAY;
    for (int i = 0; i < HILBERT_NTAPS; i++) {
        int n = i - center;
        if (n == 0 || (n & 1) == 0) {
            hilbert_coeffs[i] = 0.0f;
        } else {
            float h = 2.0f / (M_PI * n);
            float w = 0.54f - 0.46f * cosf(2.0f * M_PI * i / (HILBERT_NTAPS - 1));
            hilbert_coeffs[i] = h * w;
        }
    }
}

/* ---- WAV file I/O ---- */

#define WAV_FMT_PCM   1
#define WAV_FMT_FLOAT 3

typedef struct {
    int      sample_rate;
    int      num_channels;
    int      bits_per_sample;
    int      is_float;          /* 1 if IEEE float format */
    long     data_offset;       /* byte offset of audio data in file */
    uint32_t data_size;         /* byte count of audio data */
} wav_info;

/* Parse WAV header.  On success the file position is at the first audio byte. */
static int wav_read_header(FILE *f, wav_info *info) {
    char     tag[4];
    uint32_t riff_size;

    if (fread(tag, 1, 4, f) != 4 || memcmp(tag, "RIFF", 4)) return -1;
    if (fread(&riff_size, 4, 1, f) != 1) return -1;
    if (fread(tag, 1, 4, f) != 4 || memcmp(tag, "WAVE", 4)) return -1;

    info->data_offset = -1;

    while (1) {
        char     chunk_id[4];
        uint32_t chunk_size;
        if (fread(chunk_id, 1, 4, f) != 4) break;
        if (fread(&chunk_size, 4, 1, f) != 1) break;

        if (memcmp(chunk_id, "fmt ", 4) == 0) {
            if (chunk_size < 16) return -1;
            uint8_t buf[16];
            if (fread(buf, 1, 16, f) != 16) return -1;

            uint16_t audio_fmt, nch, bps;
            uint32_t sr;
            memcpy(&audio_fmt, buf + 0,  2);
            memcpy(&nch,       buf + 2,  2);
            memcpy(&sr,        buf + 4,  4);
            memcpy(&bps,       buf + 14, 2);

            info->sample_rate     = (int)sr;
            info->num_channels    = (int)nch;
            info->bits_per_sample = (int)bps;
            info->is_float        = (audio_fmt == WAV_FMT_FLOAT);

            if (chunk_size > 16)
                fseek(f, (long)(chunk_size - 16), SEEK_CUR);

        } else if (memcmp(chunk_id, "data", 4) == 0) {
            info->data_offset = ftell(f);
            info->data_size   = chunk_size;
            break;
        } else {
            /* skip unknown chunk (pad to even byte boundary) */
            fseek(f, (long)((chunk_size + 1) & ~1u), SEEK_CUR);
        }
    }
    return (info->data_offset >= 0) ? 0 : -1;
}

/* Read the entire audio payload into a mono float buffer.
   Multi-channel input is mixed down by averaging.  Caller must free(). */
static float *wav_read_mono_float(FILE *f, const wav_info *info, long *n_out) {
    int   bps  = info->bits_per_sample;
    int   nch  = info->num_channels;
    long  total = (long)info->data_size / (bps / 8);
    long  mono  = total / nch;

    float *buf = malloc((size_t)mono * sizeof(float));
    if (!buf) return NULL;

    for (long i = 0; i < mono; i++) {
        float sum = 0.0f;
        for (int ch = 0; ch < nch; ch++) {
            float v = 0.0f;
            if (info->is_float && bps == 32) {
                float tmp;  fread(&tmp, 4, 1, f);  v = tmp;
            } else if (info->is_float && bps == 64) {
                double tmp; fread(&tmp, 8, 1, f);  v = (float)tmp;
            } else if (bps == 16) {
                int16_t tmp; fread(&tmp, 2, 1, f); v = tmp / 32768.0f;
            } else if (bps == 24) {
                uint8_t b[3]; fread(b, 1, 3, f);
                int32_t raw = ((int32_t)b[2] << 16) | (b[1] << 8) | b[0];
                if (raw & 0x800000) raw |= (int32_t)0xFF000000;
                v = raw / 8388608.0f;
            } else if (bps == 32) {
                int32_t tmp; fread(&tmp, 4, 1, f); v = tmp / 2147483648.0f;
            } else {
                fprintf(stderr, "rade_demod: unsupported WAV format (%d-bit %s)\n",
                        bps, info->is_float ? "float" : "int");
                free(buf);
                return NULL;
            }
            sum += v;
        }
        buf[i] = sum / nch;
    }
    *n_out = mono;
    return buf;
}

/* Write a standard 44-byte PCM WAV header (16-bit, mono). */
static void wav_write_header(FILE *f, int sample_rate, uint32_t data_bytes) {
    uint16_t nch         = 1;
    uint16_t bps         = 16;
    uint16_t fmt         = WAV_FMT_PCM;
    uint32_t fmt_size    = 16;
    uint16_t block_align = (uint16_t)(nch * bps / 8);
    uint32_t byte_rate   = (uint32_t)sample_rate * block_align;
    uint32_t riff_size   = 36 + data_bytes;
    uint32_t sr          = (uint32_t)sample_rate;

    fwrite("RIFF",       1, 4, f);  fwrite(&riff_size,   4, 1, f);
    fwrite("WAVE",       1, 4, f);
    fwrite("fmt ",       1, 4, f);  fwrite(&fmt_size,    4, 1, f);
    fwrite(&fmt,         2, 1, f);  fwrite(&nch,         2, 1, f);
    fwrite(&sr,          4, 1, f);  fwrite(&byte_rate,   4, 1, f);
    fwrite(&block_align, 2, 1, f);  fwrite(&bps,         2, 1, f);
    fwrite("data",       1, 4, f);  fwrite(&data_bytes,  4, 1, f);
}

/* ---- Linear-interpolation resampler ---- */

/* Resample *in (n_in samples at in_rate) to out_rate.
   Returns a malloc'd buffer; caller must free().  Sets *n_out. */
static float *resample_linear(const float *in, long n_in,
                              int in_rate, int out_rate, long *n_out) {
    if (in_rate == out_rate) {
        float *out = malloc((size_t)n_in * sizeof(float));
        if (out) memcpy(out, in, (size_t)n_in * sizeof(float));
        *n_out = n_in;
        return out;
    }
    if (n_in < 2) { *n_out = 0; return malloc(1); }

    *n_out = (long)((double)n_in * out_rate / in_rate);
    float *out = malloc((size_t)*n_out * sizeof(float));
    if (!out) return NULL;

    double step = (double)in_rate / (double)out_rate;   /* input samples per output sample */
    for (long i = 0; i < *n_out; i++) {
        double pos  = i * step;
        long   idx  = (long)pos;
        float  frac = (float)(pos - idx);
        if (idx + 1 >= n_in) { idx = n_in - 2; frac = 1.0f; }
        out[i] = in[idx] + frac * (in[idx + 1] - in[idx]);
    }
    return out;
}

/* ---- EOO callsign callback ---- */

static void on_callsign_rx(rade_text_t rt, const char *txt, int len, void *state) {
    (void)rt; (void)state;
    fprintf(stderr, "EOO callsign: %.*s\n", len, txt);
}

/* ---- Usage ---- */

static void usage(void) {
    fprintf(stderr,
            "usage: rade_demod_wav [options] <input.wav> <output.wav>\n\n"
            "  Reads a WAV file containing received RADE OFDM audio and writes\n"
            "  a WAV file containing the decoded voice audio.\n\n"
            "  Input WAV : any sample rate, mono or stereo\n"
            "              (resampled to %d Hz / mixed to mono internally)\n"
            "  Output WAV: mono 16-bit PCM @ %d Hz\n\n"
            "options:\n"
            "  -h, --help     Show this help\n"
            "  -v LEVEL       Verbosity: 0=quiet  1=normal (default)  2=verbose\n",
            RADE_FS, RADE_FS_SPEECH);
}

/* ---- Main ---- */

int main(int argc, char *argv[]) {
    int verbose = 1;
    int opt;
    static struct option long_options[] = {
        {"help", no_argument, NULL, 'h'},
        {NULL,   0,           NULL, 0 }
    };

    while ((opt = getopt_long(argc, argv, "hv:", long_options, NULL)) != -1) {
        switch (opt) {
            case 'h': usage(); return 0;
            case 'v': verbose = atoi(optarg); break;
            default:  usage(); return 1;
        }
    }
    if (argc - optind != 2) { usage(); return 1; }

    const char *input_file  = argv[optind];
    const char *output_file = argv[optind + 1];

    /* ------------------------------------------------------------------ read input WAV */
    FILE *fin = fopen(input_file, "rb");
    if (!fin) {
        fprintf(stderr, "rade_demod: can't open '%s'\n", input_file);
        return 1;
    }

    wav_info wav;
    if (wav_read_header(fin, &wav) != 0) {
        fprintf(stderr, "rade_demod: can't parse '%s' as WAV\n", input_file);
        fclose(fin);
        return 1;
    }
    if (verbose >= 1)
        fprintf(stderr, "Input: %s  %d Hz  %d ch  %d-bit %s\n",
                input_file, wav.sample_rate, wav.num_channels,
                wav.bits_per_sample, wav.is_float ? "float" : "int");

    long  n_mono = 0;
    float *mono  = wav_read_mono_float(fin, &wav, &n_mono);
    fclose(fin);
    if (!mono) return 1;

    /* --------------------------------------------------------- resample → 8 kHz */
    long  n_8k   = 0;
    float *audio;
    if (wav.sample_rate == RADE_FS) {
        audio = mono;   /* already at modem rate – no copy needed */
        n_8k  = n_mono;
    } else {
        audio = resample_linear(mono, n_mono, wav.sample_rate, RADE_FS, &n_8k);
        free(mono);
        if (!audio) {
            fprintf(stderr, "rade_demod: resample failed\n");
            return 1;
        }
    }

    if (verbose >= 1)
        fprintf(stderr, "Modem input: %ld samples @ %d Hz  (%.1f s)\n",
                n_8k, RADE_FS, (double)n_8k / RADE_FS);

    /* --------------------------------------------------------- Hilbert → IQ */
    init_hilbert();
    RADE_COMP *iq = malloc((size_t)n_8k * sizeof(RADE_COMP));
    if (!iq) {
        fprintf(stderr, "rade_demod: malloc failed (IQ buffer)\n");
        free(audio);
        return 1;
    }
    for (long i = 0; i < n_8k; i++) {
        iq[i].real = (i >= HILBERT_DELAY) ? audio[i - HILBERT_DELAY] : 0.0f;

        float imag = 0.0f;
        for (int k = 0; k < HILBERT_NTAPS; k++) {
            long idx = i - k;
            if (idx >= 0 && idx < n_8k)
                imag += hilbert_coeffs[k] * audio[idx];
        }
        iq[i].imag = imag;
    }
    free(audio);

    /* ------------------------------------------------------ open RADE receiver */
    rade_initialize();

    int flags = (verbose < 2) ? RADE_VERBOSE_0 : 0;
    /* model_name is ignored in the nopy build (built-in weights) */
    char *model_name = "model19_check3/checkpoints/checkpoint_epoch_100.pth";
    struct rade *r = rade_open(model_name, flags);
    if (!r) {
        fprintf(stderr, "rade_demod: rade_open failed\n");
        free(iq);
        rade_finalize();
        return 1;
    }

    int nin_max        = rade_nin_max(r);
    int n_features_out = rade_n_features_in_out(r);
    int n_eoo_bits     = rade_n_eoo_bits(r);

    rade_text_t text_obj = rade_text_create();
    if (text_obj)
        rade_text_set_rx_callback(text_obj, on_callsign_rx, NULL);
    else
        fprintf(stderr, "rade_demod: rade_text_create failed — callsign decode disabled\n");

    RADE_COMP *rx_buf     = malloc((size_t)nin_max        * sizeof(RADE_COMP));
    float     *feat_buf   = malloc((size_t)n_features_out * sizeof(float));
    float     *eoo_buf    = malloc((size_t)n_eoo_bits      * sizeof(float));
    if (!rx_buf || !feat_buf || !eoo_buf) {
        fprintf(stderr, "rade_demod: malloc failed\n");
        free(iq); free(rx_buf); free(feat_buf); free(eoo_buf);
        rade_close(r); rade_finalize();
        return 1;
    }

    /* ------------------------------------------------- open FARGAN vocoder */
    FARGANState fargan;
    fargan_init(&fargan);

    /* Buffer for the 5-frame warm-up required by fargan_cont().
       Layout: 5 consecutive NB_TOTAL_FEATURES-float frames. */
    int   fargan_ready  = 0;
    float cont_buf[5 * NB_TOTAL_FEATURES];
    int   cont_frames   = 0;

    /* ---------------------------------------------------- open output WAV */
    FILE *fout = fopen(output_file, "wb");
    if (!fout) {
        fprintf(stderr, "rade_demod: can't open '%s' for writing\n", output_file);
        free(iq); free(rx_buf); free(feat_buf); free(eoo_buf);
        rade_close(r); rade_finalize();
        return 1;
    }
    /* Placeholder header – data_size patched at the end. */
    wav_write_header(fout, RADE_FS_SPEECH, 0);
    uint32_t total_bytes = 0;

    /* ---------------------------------------------------- demodulation loop */
    long iq_pos    = 0;
    int  mf_count  = 0;   /* modem frames fed to RX */
    int  vld_count = 0;   /* valid feature outputs */

    while (iq_pos < n_8k) {
        int  nin       = rade_nin(r);
        long remaining = n_8k - iq_pos;

        /* Copy samples into rx_buf; zero-pad the final short block so the
           last modem frame has a chance to flush. */
        if (remaining < nin) {
            memset(rx_buf, 0, (size_t)nin * sizeof(RADE_COMP));
            memcpy(rx_buf, &iq[iq_pos], (size_t)remaining * sizeof(RADE_COMP));
            iq_pos = n_8k;
        } else {
            memcpy(rx_buf, &iq[iq_pos], (size_t)nin * sizeof(RADE_COMP));
            iq_pos += nin;
        }

        int has_eoo = 0;
        int n_out   = rade_rx(r, feat_buf, &has_eoo, eoo_buf, rx_buf);

        if (has_eoo && verbose >= 1)
            fprintf(stderr, "End-of-over at modem frame %d\n", mf_count);
        if (has_eoo && text_obj)
            rade_text_rx(text_obj, eoo_buf, n_eoo_bits / 2);

        if (n_out > 0) {
            vld_count++;
            int n_frames = n_out / RADE_NB_TOTAL_FEATURES;

            for (int fi = 0; fi < n_frames; fi++) {
                float *feat = &feat_buf[fi * RADE_NB_TOTAL_FEATURES];

                /* ---- fargan_cont warm-up: buffer the first 5 frames ---- */
                if (!fargan_ready) {
                    memcpy(&cont_buf[cont_frames * RADE_NB_TOTAL_FEATURES],
                           feat, (size_t)RADE_NB_TOTAL_FEATURES * sizeof(float));
                    if (++cont_frames >= 5) {
                        /* fargan_cont expects features packed at stride
                           NB_FEATURES – copy only the first NB_FEATURES of
                           each buffered frame, matching lpcnet_demo behaviour. */
                        float packed[5 * NB_FEATURES];
                        for (int i = 0; i < 5; i++)
                            memcpy(&packed[i * NB_FEATURES],
                                   &cont_buf[i * NB_TOTAL_FEATURES],
                                   (size_t)NB_FEATURES * sizeof(float));

                        float zeros[FARGAN_CONT_SAMPLES];
                        memset(zeros, 0, sizeof(zeros));
                        fargan_cont(&fargan, zeros, packed);
                        fargan_ready = 1;
                    }
                    continue;   /* warm-up frames are not synthesised */
                }

                /* ---- synthesise one 10-ms speech frame ---- */
                float   fpcm[LPCNET_FRAME_SIZE];
                int16_t pcm[LPCNET_FRAME_SIZE];

                fargan_synthesize(&fargan, fpcm, feat);

                /* float → int16, matching lpcnet_demo rounding */
                for (int s = 0; s < LPCNET_FRAME_SIZE; s++) {
                    float v = fpcm[s] * 32768.0f;
                    if (v >  32767.0f)  v =  32767.0f;
                    if (v < -32767.0f)  v = -32767.0f;
                    pcm[s] = (int16_t)floor(0.5 + (double)v);
                }

                fwrite(pcm, sizeof(int16_t), (size_t)LPCNET_FRAME_SIZE, fout);
                total_bytes += (uint32_t)(LPCNET_FRAME_SIZE * (int)sizeof(int16_t));
            }
        }
        mf_count++;
    }

    /* -------------------------------------------------------- finalise WAV */
    fseek(fout, 0, SEEK_SET);
    wav_write_header(fout, RADE_FS_SPEECH, total_bytes);
    fclose(fout);

    /* ------------------------------------------------------------ summary */
    if (verbose >= 1) {
        fprintf(stderr, "Modem frames: %d   valid: %d\n", mf_count, vld_count);
        fprintf(stderr, "Output: %s  %.1f s  (%u bytes)\n",
                output_file, (double)total_bytes / (2.0 * RADE_FS_SPEECH), total_bytes);
    }

    /* -----------------------------------------------------------  cleanup */
    free(iq);
    free(rx_buf);
    free(feat_buf);
    free(eoo_buf);
    if (text_obj) rade_text_destroy(text_obj);
    rade_close(r);
    rade_finalize();
    return 0;
}
