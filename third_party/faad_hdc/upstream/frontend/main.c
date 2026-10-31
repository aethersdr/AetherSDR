/*
** FAAD2 - Freeware Advanced Audio (AAC) Decoder including SBR decoding
** Copyright (C) 2003-2005 M. Bakker, Nero AG, http://www.nero.com
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation; either version 2 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
**
** Any non-GPL usage of this software or parts of this software is strictly
** forbidden.
**
** The "appropriate copyright message" mentioned in section 2c of the GPLv2
** must read: "Code from FAAD2 is copyright (c) Nero AG, www.nero.com"
**
** Commercial non-GPL licensing of this software is possible.
** For more info contact Nero AG through Mpeg4AAClicense@nero.com.
**
** $Id: main.c,v 1.89 2015/01/19 09:46:12 knik Exp $
**/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#ifndef __MINGW32__
#define off_t __int64
#endif
#else
#include <time.h>
#endif

#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>

#include <neaacdec.h>

#include "unicode_support.h"
#include "audio.h"
#include "mp4read.h"

#ifndef min
#define min(a,b) ( (a) < (b) ? (a) : (b) )
#endif

#define MAX_CHANNELS 6 /* make this higher to support files with
                          more channels */

#define MAX_PERCENTS 384

static int quiet = 0;

static void faad_fprintf(FILE *stream, const char *fmt, ...)
{
    va_list ap;

    if (!quiet)
    {
        va_start(ap, fmt);
        vfprintf(stream, fmt, ap);
        va_end(ap);
    }

#ifdef _WIN32
    if (!_isatty(_fileno(stream)))
    {
        fflush(stream); /*ensure real-time progress output on Win32*/
    }
#endif
}

/* FAAD file buffering routines */
typedef struct {
    unsigned long bytes_into_buffer;
    unsigned long bytes_consumed;
    unsigned long file_offset;
    unsigned char *buffer;
    FILE *infile;
    int at_eof;
} aac_buffer;


static int fill_buffer(aac_buffer *b)
{
    unsigned long bread;

    if (b->bytes_consumed > 0)
    {
        if (b->bytes_into_buffer)
        {
            memmove((void*)b->buffer, (void*)(b->buffer + b->bytes_consumed),
                b->bytes_into_buffer*sizeof(unsigned char));
        }

        if (!b->at_eof)
        {
            bread = (unsigned long)fread((void*)(b->buffer + b->bytes_into_buffer), 1,
                b->bytes_consumed, b->infile);

            if (bread != b->bytes_consumed)
                b->at_eof = 1;

            b->bytes_into_buffer += bread;
        }

        b->bytes_consumed = 0;

        if (b->bytes_into_buffer > 3)
        {
            if (memcmp(b->buffer, "TAG", 3) == 0)
                b->bytes_into_buffer = 0;
        }
        if (b->bytes_into_buffer > 11)
        {
            if (memcmp(b->buffer, "LYRICSBEGIN", 11) == 0)
                b->bytes_into_buffer = 0;
        }
        if (b->bytes_into_buffer > 8)
        {
            if (memcmp(b->buffer, "APETAGEX", 8) == 0)
                b->bytes_into_buffer = 0;
        }
    }

    return 1;
}

static void advance_buffer(aac_buffer *b, unsigned int bytes)
{
    while ((b->bytes_into_buffer > 0) && (bytes > 0))
    {
        unsigned int chunk = bytes;
        if (b->bytes_into_buffer < chunk)
            chunk = (unsigned int)b->bytes_into_buffer;

        bytes -= chunk;
        b->file_offset += chunk;
        b->bytes_consumed = chunk;
        b->bytes_into_buffer -= chunk;

        if (b->bytes_into_buffer == 0)
            fill_buffer(b);
    }
}

static void lookforheader(aac_buffer *b)
{
    int i = 0;
    while (!b->at_eof )
    {
        if (b->bytes_into_buffer > 4)
        {
            if( ((b->buffer[0+i] == 0xff) && ((b->buffer[1+i] & 0xf6) == 0xf0)) ||
                (b->buffer[0+i] == 'A'    && b->buffer[1+i] == 'D' && b->buffer[2+i] == 'I' && b->buffer[3+i] == 'F'))
            {
                fill_buffer(b);
                break;
            } else {
                i++;
                b->file_offset       += 1;
                b->bytes_consumed    += 1;
                b->bytes_into_buffer -= 1;
            }
        }
        else
        {
            fill_buffer(b);
            i = 0;
        }
    }
}

static int adts_sample_rates[] = {96000,88200,64000,48000,44100,32000,24000,22050,16000,12000,11025,8000,7350,0,0,0};

static int adts_parse(aac_buffer *b, int *bitrate, float *length)
{
    size_t frames;
    unsigned int frame_length;
    size_t t_framelength = 0;
    int samplerate = 0;
    float frames_per_sec, bytes_per_frame;

    /* Read all frames to ensure correct time and bitrate */
    for (frames = 0; /* */; frames++)
    {
        fill_buffer(b);

        if (b->bytes_into_buffer > 7)
        {
            /* check syncword */
            if (!((b->buffer[0] == 0xFF)&&((b->buffer[1] & 0xF6) == 0xF0)))
                break;

            if (frames == 0)
                samplerate = adts_sample_rates[(b->buffer[2]&0x3c)>>2];

            frame_length = ((((unsigned int)b->buffer[3] & 0x3)) << 11)
                | (((unsigned int)b->buffer[4]) << 3) | (b->buffer[5] >> 5);
            if (frame_length == 0)
                break;

            t_framelength += frame_length;

            if (frame_length > b->bytes_into_buffer)
                break;

            advance_buffer(b, frame_length);
        } else {
            break;
        }
    }

    frames_per_sec = (float)samplerate/1024.0f;
    if (frames != 0)
        bytes_per_frame = (float)t_framelength/(float)(frames*1000);
    else
        bytes_per_frame = 0;
    *bitrate = (int)(8. * bytes_per_frame * frames_per_sec + 0.5);
    if (frames_per_sec != 0)
        *length = (float)frames/frames_per_sec;
    else
        *length = 1;

    return 1;
}

/* MicroSoft channel definitions */
#define SPEAKER_FRONT_LEFT             0x1
#define SPEAKER_FRONT_RIGHT            0x2
#define SPEAKER_FRONT_CENTER           0x4
#define SPEAKER_LOW_FREQUENCY          0x8
#define SPEAKER_BACK_LEFT              0x10
#define SPEAKER_BACK_RIGHT             0x20
#define SPEAKER_FRONT_LEFT_OF_CENTER   0x40
#define SPEAKER_FRONT_RIGHT_OF_CENTER  0x80
#define SPEAKER_BACK_CENTER            0x100
#define SPEAKER_SIDE_LEFT              0x200
#define SPEAKER_SIDE_RIGHT             0x400
#define SPEAKER_TOP_CENTER             0x800
#define SPEAKER_TOP_FRONT_LEFT         0x1000
#define SPEAKER_TOP_FRONT_CENTER       0x2000
#define SPEAKER_TOP_FRONT_RIGHT        0x4000
#define SPEAKER_TOP_BACK_LEFT          0x8000
#define SPEAKER_TOP_BACK_CENTER        0x10000
#define SPEAKER_TOP_BACK_RIGHT         0x20000
#define SPEAKER_RESERVED               0x80000000

static long aacChannelConfig2wavexChannelMask(NeAACDecFrameInfo *hInfo)
{
    if (hInfo->channels == 6 && hInfo->num_lfe_channels)
    {
        return SPEAKER_FRONT_LEFT + SPEAKER_FRONT_RIGHT +
            SPEAKER_FRONT_CENTER + SPEAKER_LOW_FREQUENCY +
            SPEAKER_BACK_LEFT + SPEAKER_BACK_RIGHT;
    } else {
        return 0;
    }
}

static char *position2string(int position)
{
    switch (position)
    {
    case FRONT_CHANNEL_CENTER: return "Center front";
    case FRONT_CHANNEL_LEFT:   return "Left front";
    case FRONT_CHANNEL_RIGHT:  return "Right front";
    case SIDE_CHANNEL_LEFT:    return "Left side";
    case SIDE_CHANNEL_RIGHT:   return "Right side";
    case BACK_CHANNEL_LEFT:    return "Left back";
    case BACK_CHANNEL_RIGHT:   return "Right back";
    case BACK_CHANNEL_CENTER:  return "Center back";
    case LFE_CHANNEL:          return "LFE";
    case UNKNOWN_CHANNEL:      return "Unknown";
    default: return "";
    }

    // return "";
}

static void print_channel_info(NeAACDecFrameInfo *frameInfo)
{
    /* print some channel info */
    int i;
    long channelMask = aacChannelConfig2wavexChannelMask(frameInfo);

    faad_fprintf(stderr, "  ---------------------\n");
    if (frameInfo->num_lfe_channels > 0)
    {
        faad_fprintf(stderr, " | Config: %2d.%d Ch     |", frameInfo->channels-frameInfo->num_lfe_channels, frameInfo->num_lfe_channels);
    } else {
        faad_fprintf(stderr, " | Config: %2d Ch       |", frameInfo->channels);
    }
    if (channelMask)
        faad_fprintf(stderr, " WARNING: channels are reordered according to\n");
    else
        faad_fprintf(stderr, "\n");
    faad_fprintf(stderr, "  ---------------------");
    if (channelMask)
        faad_fprintf(stderr, "  MS defaults defined in WAVE_FORMAT_EXTENSIBLE\n");
    else
        faad_fprintf(stderr, "\n");
    faad_fprintf(stderr, " | Ch |    Position    |\n");
    faad_fprintf(stderr, "  ---------------------\n");
    for (i = 0; i < frameInfo->channels; i++)
    {
        faad_fprintf(stderr, " | %.2d | %-14s |\n", i, position2string((int)frameInfo->channel_position[i]));
    }
    faad_fprintf(stderr, "  ---------------------\n");
    faad_fprintf(stderr, "\n");
}

static int FindAdtsSRIndex(int sr)
{
    int i;

    for (i = 0; i < 16; i++)
    {
        if (sr == adts_sample_rates[i])
            return i;
    }
    return 16 - 1;
}

static unsigned char *MakeAdtsHeader(int *dataSize, NeAACDecFrameInfo *hInfo, unsigned char old_format)
{
    unsigned char *data;
    int profile = (hInfo->object_type - 1) & 0x3;
    int sr_index = ((hInfo->sbr == SBR_UPSAMPLED) || (hInfo->sbr == NO_SBR_UPSAMPLED)) ?
        FindAdtsSRIndex(hInfo->samplerate / 2) : FindAdtsSRIndex(hInfo->samplerate);
    int skip = (old_format) ? 8 : 7;
    int framesize = skip + hInfo->bytesconsumed;

    if (hInfo->header_type == ADTS)
        framesize -= skip;

    *dataSize = 7;

    data = malloc(*dataSize * sizeof(unsigned char));
    memset(data, 0, *dataSize * sizeof(unsigned char));

    data[0] += 0xFF; /* 8b: syncword */

    data[1] += 0xF0; /* 4b: syncword */
    /* 1b: mpeg id = 0 */
    /* 2b: layer = 0 */
    data[1] += 1; /* 1b: protection absent */

    data[2] += ((profile << 6) & 0xC0); /* 2b: profile */
    data[2] += ((sr_index << 2) & 0x3C); /* 4b: sampling_frequency_index */
    /* 1b: private = 0 */
    data[2] += ((hInfo->channels >> 2) & 0x1); /* 1b: channel_configuration */

    data[3] += ((hInfo->channels << 6) & 0xC0); /* 2b: channel_configuration */
    /* 1b: original */
    /* 1b: home */
    /* 1b: copyright_id */
    /* 1b: copyright_id_start */
    data[3] += ((framesize >> 11) & 0x3); /* 2b: aac_frame_length */

    data[4] += ((framesize >> 3) & 0xFF); /* 8b: aac_frame_length */

    data[5] += ((framesize << 5) & 0xE0); /* 3b: aac_frame_length */
    data[5] += ((0x7FF >> 6) & 0x1F); /* 5b: adts_buffer_fullness */

    data[6] += ((0x7FF << 2) & 0x3F); /* 6b: adts_buffer_fullness */
    /* 2b: num_raw_data_blocks */

    return data;
}

/* globals */
char *progName;

static const char *file_ext[] =
{
    NULL,
    ".wav",
    ".aif",
    ".au",
    ".au",
    ".pcm",
    NULL
};

static void usage(void)
{
    faad_fprintf(stdout, "\nUsage:\n");
    faad_fprintf(stdout, "%s [options] infile.aac\n", progName);
    faad_fprintf(stdout, "Options:\n");
    faad_fprintf(stdout, " -h    Shows this help screen.\n");
    faad_fprintf(stdout, " -i    Shows info about the input file.\n");
    faad_fprintf(stdout, " -a X  Write MPEG-4 AAC ADTS output file.\n");
    faad_fprintf(stdout, " -t    Assume old ADTS format.\n");
    faad_fprintf(stdout, " -o X  Set output filename.\n");
    faad_fprintf(stdout, " -f X  Set output format. Valid values for X are:\n");
    faad_fprintf(stdout, "        1:  Microsoft WAV format (default).\n");
    faad_fprintf(stdout, "        2:  RAW PCM data.\n");
    faad_fprintf(stdout, " -b X  Set output sample format. Valid values for X are:\n");
    faad_fprintf(stdout, "        1:  16 bit PCM data (default).\n");
    faad_fprintf(stdout, "        2:  24 bit PCM data.\n");
    faad_fprintf(stdout, "        3:  32 bit PCM data.\n");
    faad_fprintf(stdout, "        4:  32 bit floating point data.\n");
    faad_fprintf(stdout, "        5:  64 bit floating point data.\n");
    faad_fprintf(stdout, " -s X  Force the samplerate to X (for RAW files).\n");
    faad_fprintf(stdout, " -l X  Set object type. Supported object types:\n");
    faad_fprintf(stdout, "        1:  Main object type.\n");
    faad_fprintf(stdout, "        2:  LC (Low Complexity) object type.\n");
    faad_fprintf(stdout, "        4:  LTP (Long Term Prediction) object type.\n");
    faad_fprintf(stdout, "        23: LD (Low Delay) object type.\n");
    faad_fprintf(stdout, " -d    Down matrix 5.1 to 2 channels\n");
    faad_fprintf(stdout, " -w    Write output to stdio instead of a file.\n");
    faad_fprintf(stdout, " -g    Disable gapless decoding.\n");
    faad_fprintf(stdout, " -q    Quiet - suppresses status messages.\n");
    faad_fprintf(stdout, " -j X  Jump - start output X seconds into track (MP4 files only).\n");
    faad_fprintf(stdout, "Example:\n");
    faad_fprintf(stdout, "       %s infile.aac\n", progName);
    faad_fprintf(stdout, "       %s infile.mp4\n", progName);
    faad_fprintf(stdout, "       %s -o outfile.wav infile.aac\n", progName);
    faad_fprintf(stdout, "       %s -w infile.aac > outfile.wav\n", progName);
    faad_fprintf(stdout, "       %s -a outfile.aac infile.aac\n", progName);
    return;
}

static int decodeAACfile(char *aacfile, char *sndfile, char *adts_fn, int to_stdout,
                  int def_srate, unsigned char object_type, unsigned char outputFormat, int fileType,
                  unsigned char downMatrix, int infoOnly, int adts_out, unsigned char old_format,
                  float *song_length)
{
    int tagsize;
    unsigned long samplerate;
    unsigned char channels;
    void *sample_buffer;

    audio_file *aufile = NULL;

    FILE *adtsFile = NULL;
    unsigned char *adtsData;
    int adtsDataSize;

    NeAACDecHandle hDecoder;
    NeAACDecFrameInfo frameInfo;
    NeAACDecConfigurationPtr config;

    char percents[MAX_PERCENTS];
    int percent, old_percent = -1;
    int fileread;
    unsigned long bread;
    long bused;
    int header_type = 0;
    int bitrate = 0;
    float length = 0;

    int first_time = 1;
    int retval;
    int streaminput = 0;

    aac_buffer b;

    memset(&b, 0, sizeof(aac_buffer));

    if (adts_out)
    {
        adtsFile = faad_fopen(adts_fn, "wb");
        if (adtsFile == NULL)
        {
            faad_fprintf(stderr, "Error opening file: %s\n", adts_fn);
            return 1;
        }
    }

    if (0 == strcmp(aacfile, "-"))
    {
        b.infile = stdin;
#ifdef _WIN32
        _setmode(_fileno(stdin), O_BINARY);
#endif

    }
    else
    {
        b.infile = faad_fopen(aacfile, "rb");
        if (b.infile == NULL)
        {
            /* unable to open file */
            faad_fprintf(stderr, "Error opening file: %s\n", aacfile);
            return 1;
        }
    }

    retval = fseek(b.infile, 0, SEEK_END);
#ifdef _WIN32
    if (0 == strcmp(aacfile, "-")) {
        retval = -1;
    }
#endif
    if (retval )
    {
        faad_fprintf(stderr, "Input not seekable %s\n", aacfile);
        fileread = -1;
        streaminput = 1;
    } else {
        fileread = ftell(b.infile);
        fseek(b.infile, 0, SEEK_SET);
    };

    b.buffer = (unsigned char*)malloc(FAAD_MIN_STREAMSIZE*MAX_CHANNELS);
    if (!b.buffer)
    {
        faad_fprintf(stderr, "Memory allocation error\n");
        return 0;
    }
    memset(b.buffer, 0, FAAD_MIN_STREAMSIZE*MAX_CHANNELS);

    bread = (unsigned long)fread(b.buffer, 1, FAAD_MIN_STREAMSIZE*MAX_CHANNELS, b.infile);
    b.bytes_into_buffer = bread;
    b.bytes_consumed = 0;
    b.file_offset = 0;

    if (bread != FAAD_MIN_STREAMSIZE*MAX_CHANNELS)
        b.at_eof = 1;

    tagsize = 0;
    if (!memcmp(b.buffer, "ID3", 3))
    {
        /* high bit is not used */
        tagsize = (b.buffer[6] << 21) | (b.buffer[7] << 14) |
            (b.buffer[8] <<  7) | (b.buffer[9] <<  0);

        tagsize += 10;
        advance_buffer(&b, tagsize);
        fill_buffer(&b);
    }

    hDecoder = NeAACDecOpen();

    /* Set the default object type and samplerate */
    /* This is useful for RAW AAC files */
    config = NeAACDecGetCurrentConfiguration(hDecoder);
    if (def_srate)
        config->defSampleRate = def_srate;
    config->defObjectType = object_type;
    config->outputFormat = outputFormat;
    config->downMatrix = downMatrix;
    config->useOldADTSFormat = old_format;
    //config->dontUpSampleImplicitSBR = 1;
    NeAACDecSetConfiguration(hDecoder, config);

    /* get AAC infos for printing */
    header_type = 0;
    if (streaminput == 1)
        lookforheader(&b);

    if ((b.buffer[0] == 0xFF) && ((b.buffer[1] & 0xF6) == 0xF0))
    {
        if (streaminput == 1)
        {
            int /*frames,*/ frame_length;
            float frames_per_sec, bytes_per_frame;
            channels = 2;
            samplerate = adts_sample_rates[(b.buffer[2]&0x3c)>>2];
            frame_length = ((((unsigned int)b.buffer[3] & 0x3)) << 11)
                | (((unsigned int)b.buffer[4]) << 3) | (b.buffer[5] >> 5);
            frames_per_sec = (float)samplerate/1024.0f;
            bytes_per_frame = (float)frame_length/(float)(1000);
            bitrate = (int)(8. * bytes_per_frame * frames_per_sec + 0.5);
            length = 1;
            faad_fprintf(stderr, "Streamed input format  samplerate %d channels %d.\n", samplerate, channels);
        } else {
            adts_parse(&b, &bitrate, &length);
            fseek(b.infile, tagsize, SEEK_SET);

            bread = (unsigned long)fread(b.buffer, 1, FAAD_MIN_STREAMSIZE*MAX_CHANNELS, b.infile);
            if (bread != FAAD_MIN_STREAMSIZE*MAX_CHANNELS)
                b.at_eof = 1;
            else
                b.at_eof = 0;
            b.bytes_into_buffer = bread;
            b.bytes_consumed = 0;
            b.file_offset = tagsize;
        }

        header_type = 1;
    }
    else if (memcmp(b.buffer, "ADIF", 4) == 0)
    {
        int skip_size = (b.buffer[4] & 0x80) ? 9 : 0;
        bitrate = ((unsigned int)(b.buffer[4 + skip_size] & 0x0F)<<19) |
            ((unsigned int)b.buffer[5 + skip_size]<<11) |
            ((unsigned int)b.buffer[6 + skip_size]<<3) |
            ((unsigned int)b.buffer[7 + skip_size] & 0xE0);

        length = (float)fileread;
        if (length != 0)
        {
            length = ((float)length*8.f)/((float)bitrate) + 0.5f;
        }

        bitrate = (int)((float)bitrate/1000.0f + 0.5f);

        header_type = 2;
    }

    *song_length = length;

    fill_buffer(&b);
    bused = NeAACDecInit(hDecoder, b.buffer, b.bytes_into_buffer, &samplerate, &channels);
    if (bused < 0)
    {
        /* If some error initializing occured, skip the file */
        faad_fprintf(stderr, "Error initializing decoder library.\n");
        if (b.buffer)
            free(b.buffer);
        NeAACDecClose(hDecoder);
        if (b.infile != stdin)
            fclose(b.infile);
        return 1;
    }
    advance_buffer(&b, bused);
    fill_buffer(&b);

    /* print AAC file info */
    faad_fprintf(stderr, "%s file info:\n", aacfile);
    switch (header_type)
    {
    case 0:
        faad_fprintf(stderr, "RAW\n\n");
        break;
    case 1:
        faad_fprintf(stderr, "ADTS, %.3f sec, %d kbps, %d Hz\n\n",
            length, bitrate, samplerate);
        break;
    case 2:
        faad_fprintf(stderr, "ADIF, %.3f sec, %d kbps, %d Hz\n\n",
            length, bitrate, samplerate);
        break;
    }

    // Override the logic of skipping 0-th output frame.
    NeAACDecPostSeekReset(hDecoder, 1);

    if (infoOnly)
    {
        NeAACDecClose(hDecoder);
        if (b.infile != stdin)
            fclose(b.infile);
        if (b.buffer)
            free(b.buffer);
        return 0;
    }

    do
    {
        sample_buffer = NeAACDecDecode(hDecoder, &frameInfo,
            b.buffer, b.bytes_into_buffer);

        if (adts_out == 1)
        {
            int skip = (old_format) ? 8 : 7;
            adtsData = MakeAdtsHeader(&adtsDataSize, &frameInfo, old_format);

            /* write the adts header */
            fwrite(adtsData, 1, adtsDataSize, adtsFile);

            /* write the frame data */
            if (frameInfo.header_type == ADTS)
                fwrite(b.buffer + skip, 1, frameInfo.bytesconsumed - skip, adtsFile);
            else
                fwrite(b.buffer, 1, frameInfo.bytesconsumed, adtsFile);
        }

        /* update buffer indices */
        advance_buffer(&b, frameInfo.bytesconsumed);

        /* check if the inconsistent number of channels */
        if (aufile != NULL && frameInfo.channels != aufile->channels)
            frameInfo.error = 12;

        if (frameInfo.error > 0)
        {
            faad_fprintf(stderr, "Error: %s\n",
                NeAACDecGetErrorMessage(frameInfo.error));
        }

        /* open the sound file now that the number of channels are known */
        if (first_time && !frameInfo.error)
        {
            /* print some channel info */
            print_channel_info(&frameInfo);

            if (!adts_out)
            {
                /* open output file */
                if (!to_stdout)
                {
                    aufile = open_audio_file(sndfile, frameInfo.samplerate, frameInfo.channels,
                        (int)outputFormat, fileType, aacChannelConfig2wavexChannelMask(&frameInfo));
                } else {
                    aufile = open_audio_file("-", frameInfo.samplerate, frameInfo.channels,
                        (int)outputFormat, fileType, aacChannelConfig2wavexChannelMask(&frameInfo));
                }
                if (aufile == NULL)
                {
                    if (b.buffer)
                        free(b.buffer);
                    NeAACDecClose(hDecoder);
                    if (b.infile != stdin)
                        fclose(b.infile);
                    return 0;
                }
            } else {
                faad_fprintf(stderr, "Writing output MPEG-4 AAC ADTS file.\n\n");
            }
            first_time = 0;
        }

        percent = min((int)(b.file_offset*100)/fileread, 100);
        if (percent > old_percent)
        {
            old_percent = percent;
            snprintf(percents, MAX_PERCENTS, "%d%% decoding %s.", percent, aacfile);
            faad_fprintf(stderr, "%s\r", percents);
#ifdef _WIN32
            SetConsoleTitle(percents);
#endif
        }

        if ((frameInfo.error == 0) && (frameInfo.samples > 0) && (!adts_out))
        {
            if (write_audio_file(aufile, sample_buffer, frameInfo.samples) == 0)
                break;
        }

        /* fill buffer */
        fill_buffer(&b);

        if (b.bytes_into_buffer == 0)
            sample_buffer = NULL; /* to make sure it stops now */

    } while (sample_buffer != NULL);

    NeAACDecClose(hDecoder);

    if (adts_out == 1)
    {
        fclose(adtsFile);
    }

    if (b.infile != stdin)
        fclose(b.infile);

    if (!first_time && !adts_out)
        close_audio_file(aufile);

    if (b.buffer)
        free(b.buffer);

    return frameInfo.error;
}

static int decodeMP4file(char *mp4file, char *sndfile, char *adts_fn, int to_stdout,
                  unsigned char outputFormat, int fileType, unsigned char downMatrix, int noGapless,
                  int infoOnly, int adts_out, float *song_length, float seek_to)
{
    /*int track;*/
    unsigned long samplerate;
    unsigned char channels;
    void *sample_buffer;

    uint32_t sampleId, startSampleId;

    audio_file *aufile = NULL;

    FILE *adtsFile = NULL;
    unsigned char *adtsData;
    int adtsDataSize;

    NeAACDecHandle hDecoder;
    NeAACDecConfigurationPtr config;
    NeAACDecFrameInfo frameInfo = {0};  // should no-frames situation be flagged as an error?
    mp4AudioSpecificConfig mp4ASC = {0};

    char percents[MAX_PERCENTS];
    int percent, old_percent = -1;

    int first_time = 1;

    /* for gapless decoding */
    unsigned int useAacLength = 1;
    unsigned int framesize;
    unsigned decoded;

    if (strcmp(mp4file, "-") == 0 ) {
        faad_fprintf(stderr, "Cannot open stdin for MP4 input \n");
        return 1;
    }

    if (!quiet)
    {
        mp4config.verbose.header = 1;
        mp4config.verbose.tags = 1;
    }
    if (mp4read_open(mp4file))
    {
        /* unable to open file */
        faad_fprintf(stderr, "Error opening file: %s\n", mp4file);
        return 1;
    }

    hDecoder = NeAACDecOpen();

    /* Set configuration */
    config = NeAACDecGetCurrentConfiguration(hDecoder);
    config->outputFormat = outputFormat;
    config->downMatrix = downMatrix;
    //config->dontUpSampleImplicitSBR = 1;
    NeAACDecSetConfiguration(hDecoder, config);

    if (adts_out)
    {
        adtsFile = faad_fopen(adts_fn, "wb");
        if (adtsFile == NULL)
        {
            faad_fprintf(stderr, "Error opening file: %s\n", adts_fn);
            return 1;
        }
    }

    if(NeAACDecInit2(hDecoder, mp4config.asc.buf, mp4config.asc.size,
                    &samplerate, &channels) < 0)
    {
        /* If some error initializing occured, skip the file */
        faad_fprintf(stderr, "Error initializing decoder library.\n");
        NeAACDecClose(hDecoder);
        mp4read_close();
        return 1;
    }

    framesize = 1024;
    useAacLength = 0;
    decoded = 0;

    if (mp4config.asc.size)
    {
        if (NeAACDecAudioSpecificConfig(mp4config.asc.buf, mp4config.asc.size, &mp4ASC) >= 0)
        {
            if (mp4ASC.frameLengthFlag == 1) framesize = 960;
            if (mp4ASC.sbr_present_flag == 1 || mp4ASC.forceUpSampling) framesize *= 2;
        }
    }

    /* print some mp4 file info */
    faad_fprintf(stderr, "%s file info:\n\n", mp4file);
    {
        /*int k, j;*/
        char *ot[6] = { "NULL", "MAIN AAC", "LC AAC", "SSR AAC", "LTP AAC", "HE AAC" };
        float seconds;
        seconds = (float)mp4config.samples/(float)mp4ASC.samplingFrequency;

        *song_length = seconds;

        faad_fprintf(stderr, "%s\t%.3f secs, %d ch, %d Hz\n\n", ot[(mp4ASC.objectTypeIndex > 5)?0:mp4ASC.objectTypeIndex],
            seconds, mp4ASC.channelsConfiguration, mp4ASC.samplingFrequency);
    }

    if (infoOnly)
    {
        NeAACDecClose(hDecoder);
        mp4read_close();
        return 0;
    }

    startSampleId = 0;
    if (seek_to > 0.1) {
        int64_t sample = (int64_t)(seek_to * mp4config.samplerate / framesize);
        uint32_t limit = ~0u;
        startSampleId = sample < limit ? (uint32_t)sample : limit;
    }

    mp4read_seek(startSampleId);
    for (sampleId = startSampleId; sampleId < mp4config.frame.nsamples; sampleId++)
    {
        /*int rc;*/
        unsigned long dur;
        unsigned int sample_count;

        if (mp4read_frame())
            break;

        sample_buffer = NeAACDecDecode(hDecoder, &frameInfo, mp4config.bitbuf.data, mp4config.bitbuf.size);

        if (!sample_buffer) {
            /* unable to decode file, abort */
            break;
        }

        if (adts_out == 1)
        {
            adtsData = MakeAdtsHeader(&adtsDataSize, &frameInfo, 0);

            /* write the adts header */
            fwrite(adtsData, 1, adtsDataSize, adtsFile);

            fwrite(mp4config.bitbuf.data, 1, frameInfo.bytesconsumed, adtsFile);
        }

        dur = frameInfo.samples / frameInfo.channels;
        decoded += dur;

        if (decoded > mp4config.samples)
            dur -= decoded - mp4config.samples;

        if (dur > framesize)
        {
            faad_fprintf(stderr, "Warning: excess frame detected in MP4 file.\n");
            dur = framesize;
        }

        if (!noGapless)
        {
            if (useAacLength || (mp4config.samplerate != samplerate)) {
                sample_count = frameInfo.samples;
            } else {
                sample_count = (unsigned int)(dur * frameInfo.channels);
                if (sample_count > frameInfo.samples)
                    sample_count = frameInfo.samples;
            }
        } else {
            sample_count = frameInfo.samples;
        }

        /* open the sound file now that the number of channels are known */
        if (first_time && !frameInfo.error)
        {
            /* print some channel info */
            print_channel_info(&frameInfo);

            if (!adts_out)
            {
                /* open output file */
                if(!to_stdout)
                {
                    aufile = open_audio_file(sndfile, frameInfo.samplerate, frameInfo.channels,
                        (int)outputFormat, fileType, aacChannelConfig2wavexChannelMask(&frameInfo));
                } else {
#ifdef _WIN32
                    _setmode(_fileno(stdout), O_BINARY);
#endif
                    aufile = open_audio_file("-", frameInfo.samplerate, frameInfo.channels,
                        (int)outputFormat, fileType, aacChannelConfig2wavexChannelMask(&frameInfo));
                }
                if (aufile == NULL)
                {
                    NeAACDecClose(hDecoder);
                    mp4read_close();
                    return 0;
                }
            }
            first_time = 0;
        }

        percent = min((int)(sampleId*100)/mp4config.frame.nsamples, 100);
        if (percent > old_percent)
        {
            old_percent = percent;
            snprintf(percents, MAX_PERCENTS, "%d%% decoding %s.", percent, mp4file);
            faad_fprintf(stderr, "%s\r", percents);
#ifdef _WIN32
            SetConsoleTitle(percents);
#endif
        }

        if ((frameInfo.error == 0) && (sample_count > 0) && (!adts_out))
        {
            if (write_audio_file(aufile, sample_buffer, sample_count) == 0)
                break;
        }

        if (frameInfo.error > 0)
        {
            faad_fprintf(stderr, "Warning: %s\n",
                NeAACDecGetErrorMessage(frameInfo.error));
        }
    }

    NeAACDecClose(hDecoder);

    if (adts_out == 1)
    {
        fclose(adtsFile);
    }

    mp4read_close();

    if (!first_time && !adts_out)
        close_audio_file(aufile);

    return frameInfo.error;
}

static int faad_main(int argc, char *argv[])
{
    int result;
    int infoOnly = 0;
    int writeToStdio = 0;
    int readFromStdin = 0;
    unsigned char object_type = LC;
    int def_srate = 0;
    unsigned char downMatrix = 0;
    int format = 1;
    unsigned char outputFormat = FAAD_FMT_16BIT;
    int outfile_set = 0;
    int adts_out = 0;
    unsigned char old_format = 0;
    int showHelp = 0;
    int mp4file = 0;
    int noGapless = 0;
    char *fnp;
    char *aacFileName = NULL;
    char *audioFileName = NULL;
    char *adtsFileName = NULL;
    float seekTo = 0;
    unsigned char header[8];
    size_t bread;
    float length = 0;
    FILE *hMP4File;
    char *faad_id_string;
    char *faad_copyright_string;

/* System dependant types */
#ifdef _WIN32
    long begin;
#else
    clock_t begin;
#endif

    unsigned long cap = NeAACDecGetCapabilities();


    /* begin process command line */
    progName = argv[0];
    while (1) {
        int c = -1;
        int option_index = 0;
        static struct option long_options[] = {
            { "quiet",      0, 0, 'q' },
            { "outfile",    0, 0, 'o' },
            { "adtsout",    0, 0, 'a' },
            { "oldformat",  0, 0, 't' },
            { "format",     0, 0, 'f' },
            { "bits",       0, 0, 'b' },
            { "samplerate", 0, 0, 's' },
            { "objecttype", 0, 0, 'l' },
            { "downmix",    0, 0, 'd' },
            { "info",       0, 0, 'i' },
            { "stdio",      0, 0, 'w' },
            { "stdio",      0, 0, 'g' },
            { "seek",       1, 0, 'j' },
            { "help",       0, 0, 'h' },
            { 0, 0, 0, 0 }
        };

        c = getopt_long(argc, argv, "o:a:s:f:b:l:j:wgdhitq",
            long_options, &option_index);

        if (c == -1)
            break;

        switch (c) {
        case 'o':
            if (optarg)
            {
                outfile_set = 1;
                audioFileName = (char *) malloc(sizeof(char) * (strlen(optarg) + 1));
                if (audioFileName == NULL)
                {
                    faad_fprintf(stderr, "Error allocating memory for audioFileName.\n");
                    return 1;
                }
                strcpy(audioFileName, optarg);
            }
            break;
        case 'a':
            if (optarg)
            {
                adts_out = 1;
                adtsFileName = (char *) malloc(sizeof(char) * (strlen(optarg) + 1));
                if (adtsFileName == NULL)
                {
                    faad_fprintf(stderr, "Error allocating memory for adtsFileName.\n");
                    return 1;
                }
                strcpy(adtsFileName, optarg);
            }
            break;
        case 's':
            if (optarg)
            {
                char dr[10];
                if (sscanf(optarg, "%s", dr) < 1) {
                    def_srate = 0;
                } else {
                    def_srate = atoi(dr);
                }
            }
            break;
        case 'f':
            if (optarg)
            {
                char dr[10];
                if (sscanf(optarg, "%s", dr) < 1)
                {
                    format = 1;
                } else {
                    format = atoi(dr);
                    if ((format < 1) || (format > 2))
                        showHelp = 1;
                }
            }
            break;
        case 'b':
            if (optarg)
            {
                char dr[10];
                if (sscanf(optarg, "%s", dr) < 1)
                {
                    outputFormat = FAAD_FMT_16BIT; /* just use default */
                } else {
                    int val = atoi(dr);
                    if ((val < 1) || (val > 5))
                        showHelp = 1;
                    if (val == 5)  // Not yet unsupported by "audio".
                        showHelp = 1;
                    outputFormat = (unsigned char)val;
                }
            }
            break;
        case 'l':
            if (optarg)
            {
                char dr[10];
                if (sscanf(optarg, "%s", dr) < 1)
                {
                    object_type = LC; /* default */
                } else {
                    int val = atoi(dr);
                    if ((val != LC) &&
                        (val != MAIN) &&
                        (val != LTP) &&
                        (val != LD) &&
                        (val != HDC_LC))
                    {
                        showHelp = 1;
                    }
                    object_type = (unsigned char)val;
                }
            }
            break;
        case 'j':
            if (optarg)
            {
                seekTo = (float)atof(optarg);
            }
            break;
        case 't':
            old_format = 1;
            break;
        case 'd':
            downMatrix = 1;
            break;
        case 'w':
            writeToStdio = 1;
            break;
        case 'g':
            noGapless = 1;
            break;
        case 'i':
            infoOnly = 1;
            break;
        case 'h':
            showHelp = 1;
            break;
        case 'q':
            quiet = 1;
            break;
        default:
            break;
        }
    }

    NeAACDecGetVersion(&faad_id_string, &faad_copyright_string);

    faad_fprintf(stderr, " *********** Ahead Software MPEG-4 AAC Decoder V%s ******************\n\n", faad_id_string);
#ifndef BUILD_DATE
#define BUILD_DATE __DATE__
#endif
    faad_fprintf(stderr, " Build: %s\n", BUILD_DATE);
#undef BUILD_DATE
    faad_fprintf(stderr, "%s", faad_copyright_string);
    if (cap & FIXED_POINT_CAP)
        faad_fprintf(stderr, " Fixed point version\n");
    else
        faad_fprintf(stderr, " Floating point version\n");
    faad_fprintf(stderr, "\n");
    faad_fprintf(stderr, " This program is free software; you can redistribute it and/or modify\n");
    faad_fprintf(stderr, " it under the terms of the GNU General Public License.\n");
    faad_fprintf(stderr, "\n");
    faad_fprintf(stderr, " **************************************************************************\n\n");


    /* check that we have at least two non-option arguments */
    /* Print help if requested */
    if (((argc - optind) < 1) || showHelp)
    {
        usage();
        return 1;
    }

#if 0
    /* only allow raw data on stdio */
    if (writeToStdio == 1)
    {
        format = 2;
    }
#endif

    /* point to the specified file name */
    aacFileName = (char *) malloc(sizeof(char) * (strlen(argv[optind]) + 1));
    if (aacFileName == NULL)
    {
        faad_fprintf(stderr, "Error allocating memory for aacFileName.\n");
        return 1;
    }
    strcpy(aacFileName, argv[optind]);

#ifdef _WIN32
    begin = GetTickCount();
#else
    begin = clock();
#endif

    /* Only calculate the path and open the file for writing if
       we are not writing to stdout.
     */
    if(!writeToStdio && !outfile_set)
    {
        audioFileName = (char *) malloc(sizeof(char) * (strlen(aacFileName) + strlen(file_ext[format]) + 1));
        if (audioFileName == NULL)
        {
            faad_fprintf(stderr, "Error allocating memory for audioFileName.\n");
            return 1;
        }
        strcpy(audioFileName, aacFileName);

        fnp = (char *)strrchr(audioFileName,'.');

        if (fnp)
            fnp[0] = '\0';

        strcat(audioFileName, file_ext[format]);
    }

    /* check for mp4 file */
    if (0 == strcmp(aacFileName, "-")) {
        faad_fprintf(stderr, "Reading from stdin: %s\n", aacFileName);
        readFromStdin = 1;
        hMP4File  = stdin;
#ifdef _WIN32
        _setmode(_fileno(stdin), O_BINARY);
#endif

    } else {

        mp4file = 0;
        hMP4File = faad_fopen(aacFileName, "rb");
        if (!hMP4File)
        {
            faad_fprintf(stderr, "Error opening file: %s\n", aacFileName);
            return 1;
        }
    }

    bread = fread(header, 1, 8, hMP4File);

    if (! readFromStdin )
      fclose(hMP4File);

    if (bread != 8) {
        faad_fprintf(stderr, "Error reading file.\n");
        return 1;
    }

    if (header[4] == 'f' && header[5] == 't' && header[6] == 'y' && header[7] == 'p')
        mp4file = 1;

    if (!mp4file && seekTo != 0) {
        faad_fprintf(stderr, "Warning: can only seek in MP4 files\n");
    }

    if (mp4file)
    {
        result = decodeMP4file(aacFileName, audioFileName, adtsFileName, writeToStdio,
            outputFormat, format, downMatrix, noGapless, infoOnly, adts_out, &length, seekTo);
    } else {

    if (readFromStdin == 1) {
        ungetc(header[7],hMP4File);
        ungetc(header[6],hMP4File);
        ungetc(header[5],hMP4File);
        ungetc(header[4],hMP4File);
        ungetc(header[3],hMP4File);
        ungetc(header[2],hMP4File);
        ungetc(header[1],hMP4File);
        ungetc(header[0],hMP4File);
    }

        result = decodeAACfile(aacFileName, audioFileName, adtsFileName, writeToStdio,
            def_srate, object_type, outputFormat, format, downMatrix, infoOnly, adts_out,
            old_format, &length);
    }

    if (audioFileName != NULL)
      free (audioFileName);
    if (adtsFileName != NULL)
      free (adtsFileName);

    if (!result && !infoOnly)
    {
#ifdef _WIN32
        float dec_length = (float)(GetTickCount()-begin)/1000.0f;
        SetConsoleTitle("FAAD");
#else
        /* clock() grabs time since the start of the app but when we decode
           multiple files, each file has its own starttime (begin).
         */
        float dec_length = (float)(clock() - begin)/(float)CLOCKS_PER_SEC;
#endif
        faad_fprintf(stderr, "Decoding %s took: %5.2f sec. %5.2fx real-time.\n", aacFileName,
            dec_length, (dec_length > 0.01) ? (length/dec_length) : 0.);
    }

    free (aacFileName);
    return 0;
}

int main(int argc, char *argv[])
{
#if defined WIN32 || defined _WIN32 || defined WIN64 || defined _WIN64
    int argc_utf8, exit_code;
    char **argv_utf8;
    (void)argc;
    (void)argv;
    init_console_utf8(stderr);
    init_commandline_arguments_utf8(&argc_utf8, &argv_utf8);
    exit_code = faad_main(argc_utf8, argv_utf8);
    free_commandline_arguments_utf8(&argc_utf8, &argv_utf8);
    uninit_console_utf8();
    return exit_code;
#else
    return faad_main(argc, argv);
#endif
}
