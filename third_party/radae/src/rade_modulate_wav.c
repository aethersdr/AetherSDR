/*---------------------------------------------------------------------------*\

  rade_modulate_wav.c

  RADAE WAV modulator.  Reads a WAV file containing speech audio and writes
  a WAV file containing RADE OFDM encoded audio — the transmit counterpart
  to rade_demod.

  Combines LPCNet feature extraction and radae_tx (RADE encoder + OFDM
  modulation) into a single command-line tool.

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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <getopt.h>

#include "rade_api.h"
#include "rade_dsp.h"
#include "lpcnet.h"
#include "arch.h"
#include "cpu_support.h"

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
                fprintf(stderr, "rade_modulate: unsupported WAV format (%d-bit %s)\n",
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

/* ---- Helper: write the real part of an IQ buffer as 16-bit PCM ---- */

static uint32_t write_iq_real(FILE *f, int16_t *out_buf,
                              const RADE_COMP *iq, int n) {
    for (int i = 0; i < n; i++) {
        float v = iq[i].real * 32768.0f;
        if (v >  32767.0f)  v =  32767.0f;
        if (v < -32767.0f)  v = -32767.0f;
        out_buf[i] = (int16_t)floor(0.5 + (double)v);
    }
    fwrite(out_buf, sizeof(int16_t), (size_t)n, f);
    return (uint32_t)(n * (int)sizeof(int16_t));
}

/* ---- Usage ---- */

static void usage(void) {
    fprintf(stderr,
            "usage: rade_modulate_wav [options] <input.wav> <output.wav>\n\n"
            "  Reads a WAV file containing speech audio and writes a WAV\n"
            "  file containing RADE OFDM encoded audio.\n\n"
            "  Input WAV : any sample rate, mono or stereo\n"
            "              (resampled to %d Hz / mixed to mono internally)\n"
            "  Output WAV: mono 16-bit PCM @ %d Hz (RADE modulated signal)\n\n"
            "options:\n"
            "  -h, --help     Show this help\n"
            "  -v LEVEL       Verbosity: 0=quiet  1=normal (default)  2=verbose\n",
            RADE_FS_SPEECH, RADE_FS);
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
        fprintf(stderr, "rade_modulate: can't open '%s'\n", input_file);
        return 1;
    }

    wav_info wav;
    if (wav_read_header(fin, &wav) != 0) {
        fprintf(stderr, "rade_modulate: can't parse '%s' as WAV\n", input_file);
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

    /* --------------------------------------------------------- resample → 16 kHz (speech rate) */
    long  n_16k  = 0;
    float *audio;
    if (wav.sample_rate == RADE_FS_SPEECH) {
        audio = mono;   /* already at speech rate – no copy needed */
        n_16k = n_mono;
    } else {
        audio = resample_linear(mono, n_mono, wav.sample_rate, RADE_FS_SPEECH, &n_16k);
        free(mono);
        if (!audio) {
            fprintf(stderr, "rade_modulate: resample failed\n");
            return 1;
        }
    }

    if (verbose >= 1)
        fprintf(stderr, "Speech input: %ld samples @ %d Hz  (%.1f s)\n",
                n_16k, RADE_FS_SPEECH, (double)n_16k / RADE_FS_SPEECH);

    /* --------------------------------------------------------- init LPCNet feature extractor */
    int arch = opus_select_arch();
    LPCNetEncState *net = lpcnet_encoder_create();
    if (!net) {
        fprintf(stderr, "rade_modulate: lpcnet_encoder_create failed\n");
        free(audio);
        return 1;
    }

    /* ------------------------------------------------------ open RADE transmitter */
    rade_initialize();

    int flags = (verbose < 2) ? RADE_VERBOSE_0 : 0;
    /* model_name is ignored in the nopy build (built-in weights) */
    char *model_name = "model19_check3/checkpoints/checkpoint_epoch_100.pth";
    struct rade *r = rade_open(model_name, flags);
    if (!r) {
        fprintf(stderr, "rade_modulate: rade_open failed\n");
        lpcnet_encoder_destroy(net);
        free(audio);
        rade_finalize();
        return 1;
    }

    int n_features_in  = rade_n_features_in_out(r);
    int n_tx_out       = rade_n_tx_out(r);
    int n_eoo_out      = rade_n_tx_eoo_out(r);
    int frames_per_mf  = n_features_in / RADE_NB_TOTAL_FEATURES;  /* 12 feature frames per modem frame */

    float     *features_in = malloc((size_t)n_features_in  * sizeof(float));
    RADE_COMP *tx_out      = malloc((size_t)n_tx_out       * sizeof(RADE_COMP));
    RADE_COMP *eoo_out     = malloc((size_t)n_eoo_out      * sizeof(RADE_COMP));

    /* int16 output scratch — sized to the larger of tx / eoo frames */
    int      out_buf_n = (n_eoo_out > n_tx_out) ? n_eoo_out : n_tx_out;
    int16_t *out_buf   = malloc((size_t)out_buf_n * sizeof(int16_t));

    if (!features_in || !tx_out || !eoo_out || !out_buf) {
        fprintf(stderr, "rade_modulate: malloc failed\n");
        free(features_in); free(tx_out); free(eoo_out); free(out_buf);
        free(audio);
        lpcnet_encoder_destroy(net);
        rade_close(r); rade_finalize();
        return 1;
    }

    /* ---------------------------------------------------- open output WAV */
    FILE *fout = fopen(output_file, "wb");
    if (!fout) {
        fprintf(stderr, "rade_modulate: can't open '%s' for writing\n", output_file);
        free(features_in); free(tx_out); free(eoo_out); free(out_buf);
        free(audio);
        lpcnet_encoder_destroy(net);
        rade_close(r); rade_finalize();
        return 1;
    }
    /* Placeholder header – data_size patched at the end. */
    wav_write_header(fout, RADE_FS, 0);
    uint32_t total_bytes = 0;

    /* ---------------------------------------------------- modulation loop */
    long pcm_pos   = 0;     /* position in 16 kHz speech buffer */
    int  feat_idx  = 0;     /* feature frames buffered in features_in */
    int  mf_count  = 0;     /* modem frames transmitted */

    while (pcm_pos + LPCNET_FRAME_SIZE <= n_16k) {
        /* float → int16 for LPCNet (matching lpcnet_demo rounding) */
        opus_int16 pcm[LPCNET_FRAME_SIZE];
        for (int i = 0; i < LPCNET_FRAME_SIZE; i++) {
            float v = audio[pcm_pos + i] * 32768.0f;
            if (v >  32767.0f)  v =  32767.0f;
            if (v < -32767.0f)  v = -32767.0f;
            pcm[i] = (opus_int16)floor(0.5 + (double)v);
        }
        pcm_pos += LPCNET_FRAME_SIZE;

        /* extract one 10-ms feature frame directly into the TX buffer */
        lpcnet_compute_single_frame_features(net,
            pcm, &features_in[feat_idx * RADE_NB_TOTAL_FEATURES], arch);
        feat_idx++;

        /* full modem frame accumulated – encode + modulate */
        if (feat_idx >= frames_per_mf) {
            int n_out = rade_tx(r, tx_out, features_in);
            total_bytes += write_iq_real(fout, out_buf, tx_out, n_out);
            feat_idx = 0;
            mf_count++;
        }
    }

    /* ------------------------------------------------ flush partial modem frame */
    if (feat_idx > 0) {
        /* zero-pad remaining feature slots so the last speech segment is encoded */
        memset(&features_in[feat_idx * RADE_NB_TOTAL_FEATURES], 0,
               (size_t)(frames_per_mf - feat_idx) * RADE_NB_TOTAL_FEATURES * sizeof(float));
        int n_out = rade_tx(r, tx_out, features_in);
        total_bytes += write_iq_real(fout, out_buf, tx_out, n_out);
        mf_count++;
    }

    /* ---------------------------------------------------- end-of-over frame */
    {
        int n_out = rade_tx_eoo(r, eoo_out);
        total_bytes += write_iq_real(fout, out_buf, eoo_out, n_out);
    }

    /* -------------------------------------------------------- finalise WAV */
    fseek(fout, 0, SEEK_SET);
    wav_write_header(fout, RADE_FS, total_bytes);
    fclose(fout);

    /* ------------------------------------------------------------ summary */
    if (verbose >= 1) {
        fprintf(stderr, "Modem frames: %d + EOO\n", mf_count);
        fprintf(stderr, "Output: %s  %.1f s  (%u bytes)\n",
                output_file, (double)total_bytes / (2.0 * RADE_FS), total_bytes);
    }

    /* -----------------------------------------------------------  cleanup */
    free(features_in);
    free(tx_out);
    free(eoo_out);
    free(out_buf);
    free(audio);
    lpcnet_encoder_destroy(net);
    rade_close(r);
    rade_finalize();
    return 0;
}
