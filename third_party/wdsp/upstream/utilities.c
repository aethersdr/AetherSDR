/*  utilities.c

This file is part of a program that implements a Software-Defined Radio.

Copyright (C) 2013, 2019, 2024 Warren Pratt, NR0V

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.

The author can be reached by email at  

warren@wpratt.com

*/

#define _CRT_SECURE_NO_WARNINGS
#include "comm.h"

/********************************************************************************************************
*																										*
*											Required Utilities											*
*																										*
********************************************************************************************************/

PORT
void *malloc0 (int size)
{
	int alignment = 16;
	void* p = _aligned_malloc (size, alignment);
	if (p != 0) memset (p, 0, size);
	return p;
}

// Exported calls

PORT void
*NewCriticalSection()
{	// used by VAC
	LPCRITICAL_SECTION cs_ptr;
	cs_ptr = (LPCRITICAL_SECTION)calloc (1,sizeof (CRITICAL_SECTION));
	return (void *)cs_ptr;
}

PORT void
DestroyCriticalSection (LPCRITICAL_SECTION cs_ptr)
{	// used by VAC
	free ((char *)cs_ptr);
}


/********************************************************************************************************
*																										*
*										Test & Debug Utilities											*
*																										*
********************************************************************************************************/

void print_impulse (const char* filename, int N, double* impulse, int rtype, int pr_mode)
{
	int i;
	FILE* file;
	if (pr_mode == 0)
		file = fopen (filename, "w");
	else
		file = fopen (filename, "a");
	if (file)
	{
		if (rtype == 0)
			for (i = 0; i < N; i++)
				fprintf(file, "%.17e\n", impulse[i]);
		else
			for (i = 0; i < N; i++)
				fprintf(file, "%.17e\t%.17e\n", impulse[2 * i + 0], impulse[2 * i + 1]);
		fprintf(file, "\n\n\n\n");
		fflush(file);
		fclose(file);
	}
}

void print_peak_val (const char* filename, int N, double* buff, double thresh)
{
	int i;
	static unsigned int seqnum;
	double peak = 0.0;
	FILE* file;
	for (i = 0; i < N; i++)
		if (buff[i] > peak) peak = buff[i];
	if (peak >= thresh)
	{
		if (file = fopen(filename, "a"))
		{
			fprintf(file, "%d\t\t%.17e\n", seqnum, peak);
			fflush(file);
			fclose(file);
		}
	}
	seqnum++;
}

void print_peak_env (const char* filename, int N, double* buff, double thresh)
{
	int i;
	static unsigned int seqnum;
	double peak = 0.0;
	double new_peak;
	FILE* file;
	for (i = 0; i < N; i++)
	{
		new_peak = sqrt (buff[2 * i + 0] * buff[2 * i + 0] + buff[2 * i + 1] * buff[2 * i + 1]);
		if (new_peak > peak) peak = new_peak;
	}
	if (peak >= thresh)
	{
		if (file = fopen(filename, "a"))
		{
			fprintf(file, "%d\t\t%.17e\n", seqnum, peak);
			fflush(file);
			fclose(file);
		}
	}
	seqnum++;
}

void print_peak_env_f2 (const char* filename, int N, float* Ibuff, float* Qbuff)
{
	int i;
	double peak = 0.0;
	double new_peak;
	FILE* file;
	if (file = fopen(filename, "a"))
	{
		for (i = 0; i < N; i++)
		{
			new_peak = sqrt (Ibuff[i] * Ibuff[i] + Qbuff[i] * Qbuff[i]);
			if (new_peak > peak) peak = new_peak;
		}
		fprintf(file, "%.17e\n", peak);
		fflush(file);
		fclose(file);
	}
}

void print_iqc_values (const char* filename, int state, double env_in, double I, double Q, double ym, double yc, double ys, double thresh)
{
	static unsigned int seqnum;
	double env_out;
	FILE* file;
	env_out = sqrt (I * I + Q * Q);
	if (env_out > thresh)
	{
		if (file = fopen(filename, "a"))
		{
			if (seqnum == 0)
				fprintf(file, "seqnum\tstate\tenv_in\t\tenv_out\t\tym\t\tyc\t\tys\n");
			fprintf(file, "%d\t%d\t%f\t%f\t%f\t%f\t%f\n", seqnum, state, env_in, env_out, ym, yc, ys);
			fflush(file);
			fclose(file);
			seqnum++;
		}
	}
}

PORT
void print_buffer_parameters (const char* filename, int channel)
{
	IOB a = ch[channel].iob.pc;
	FILE* file;
	if (file = fopen(filename, "a"))
	{
		fprintf(file, "channel            = %d\n", channel);
		fprintf(file, "in_size            = %d\n", a->in_size);
		fprintf(file, "r1_outsize         = %d\n", a->r1_outsize);
		fprintf(file, "r1_size            = %d\n", a->r1_size);
		fprintf(file, "r2_size            = %d\n", a->r2_size);
		fprintf(file, "out_size           = %d\n", a->out_size);
		fprintf(file, "r2_insize          = %d\n", a->r2_insize);
		fprintf(file, "r1_active_buffsize = %d\n", a->r1_active_buffsize);
		fprintf(file, "f2_active_buffsize = %d\n", a->r2_active_buffsize);
		fprintf(file, "r1_inidx           = %d\n", a->r1_inidx);
		fprintf(file, "r1_outidx          = %d\n", a->r1_outidx);
		fprintf(file, "r1_unqueuedsamps   = %d\n", a->r1_unqueuedsamps);
		fprintf(file, "r2_inidx           = %d\n", a->r2_inidx);
		fprintf(file, "r2_outidx          = %d\n", a->r2_outidx);
		fprintf(file, "r2_havesamps       = %d\n", a->r2_havesamps);
		fprintf(file, "in_rate            = %d\n", ch[channel].in_rate);
		fprintf(file, "dsp_rate           = %d\n", ch[channel].dsp_rate);
		fprintf(file, "out_rate           = %d\n", ch[channel].out_rate);
		fprintf(file, "\n");
		fflush(file);
		fclose(file);
	}
}

void print_meter (const char* filename, double* meter, int enum_av, int enum_pk, int enum_gain)
{
	FILE* file;
	if (file = fopen(filename, "a"))
	{
		if (enum_gain >= 0)
			fprintf(file, "%.4e\t%.4e\t%.4e\n", meter[enum_av], meter[enum_pk], meter[enum_gain]);
		else
			fprintf(file, "%.4e\t%.4e\n", meter[enum_av], meter[enum_pk]);
		fflush(file);
		fclose(file);
	}
}

void print_message (const char* filename, const char* message, int p0, int p1, int p2)
{
	FILE* file;
	if (file = fopen(filename, "a"))
	{
		const char* msg = message;
		fprintf(file, "%s     %d     %d     %d\n", msg, p0, p1, p2);
		fflush(file);
		fclose(file);
	}
}

void print_window_gain (const char* filename, int wintype, double inv_coherent_gain, double inherent_power_gain)
{
	FILE* file;
	if (file = fopen(filename, "a"))
	{
		double enb = inherent_power_gain * inv_coherent_gain * inv_coherent_gain;
		switch (wintype)
		{
		case 0:
			fprintf(file, "Rectangular             %.4f\t%.4f\t%.4f\n", inv_coherent_gain, inherent_power_gain, enb);
			break;
		case 1:
			fprintf(file, "Blackman-Harris 4-term  %.4f\t%.4f\t%.4f\n", inv_coherent_gain, inherent_power_gain, enb);
			break;
		case 2:
			fprintf(file, "Hann                    %.4f\t%.4f\t%.4f\n", inv_coherent_gain, inherent_power_gain, enb);
			break;
		case 3:
			fprintf(file, "Flat Top                %.4f\t%.4f\t%.4f\n", inv_coherent_gain, inherent_power_gain, enb);
			break;
		case 4:
			fprintf(file, "Hamming                 %.4f\t%.4f\t%.4f\n", inv_coherent_gain, inherent_power_gain, enb);
			break;
		case 5:
			fprintf(file, "Kaiser                  %.4f\t%.4f\t%.4f\n", inv_coherent_gain, inherent_power_gain, enb);
			break;
		case 6:
			fprintf(file, "Blackman-Harris 7-term  %.4f\t%.4f\t%.4f\n", inv_coherent_gain, inherent_power_gain, enb);
			break;
		default:
			fprintf(file, "Specified Window Type is Invalid\n");
			break;
		}
		fflush(file);
		fclose(file);
	}
}

void print_deviation (const char* filename, double dpmax, double rate)
{
	FILE* file;
	if (file = fopen(filename, "a"))
	{
		double peak = dpmax * rate / TWOPI;
		fprintf(file, "Peak Dev = %.4f\n", peak);
		fflush(file);
		fclose(file);
	}
}

// Audacity:  Import Raw Data, Signed 32-bit PCM, Little-endian, Mono/Stereo per mode selection, 48K rate

int audiocount = 0;
int* data = 0;
int ready = 0;
int done = 0;

void WriteAudioFile(void* arg)
{
	byte* dat = (byte *)arg;
	FILE* file;
	// reverse bits of each byte (possibly needed on some platforms)
	// int i;
	// byte b;
	// for (i = 0; i < 4 * audiocount; i++)
	// {
	//	b = dat[i];
	// 	b = ((b >> 1) & 0x55) | ((b << 1) & 0xaa);
	// 	b = ((b >> 2) & 0x33) | ((b << 2) & 0xcc);
	// 	b = ((b >> 4) & 0x0f) | ((b << 4) & 0xf0);
	// 	dat[i] = b;
	// }
	if (file = fopen("AudioFile", "wb"))
	{
		fwrite((int*)dat, sizeof(int), audiocount, file);
		fflush(file);
		fclose(file);
	}
	_aligned_free(data);
	data = 0;
	_endthread();
}

void WriteAudioWDSP (double seconds, int rate, int size, double* indata, int mode, double gain)
{
	// seconds - number of seconds of audio to record
	// rate - sample rate
	// size - number of complex samples
	// indata - pointer to data
	static int n;
	int i;
	const double conv = 2147483647.0 * gain;
	if (!ready)
	{
		if (mode != 3)
			n = (int)(seconds * rate);
		else
			n = 2 * (int)(seconds * rate);
		ready = 1;
	}
	if (!data) data = (int*)malloc0(n * sizeof(int));
	for (i = 0; i < size; i++)
	{
		if (audiocount < n)
		{
			switch (mode)
			{
			case 0:	// I only (mono)
				data[audiocount++] = (int)(conv * indata[2 * i + 0]);
				break;
			case 1: // Q only (mono)
				data[audiocount++] = (int)(conv * indata[2 * i + 1]);
				break;
			case 2: // envelope (mono)
				data[audiocount++] = (int)(conv * sqrt(indata[2 * i + 0] * indata[2 * i + 0] + indata[2 * i + 1] * indata[2 * i + 1]));
				break;
			case 3:	// complex samples (stereo)
				data[audiocount++] = (int)(conv * indata[2 * i + 0]);
				data[audiocount++] = (int)(conv * indata[2 * i + 1]);
				break;
			case 4: // double samples (mono)
				data[audiocount++] = (int)(conv * indata[i]);
				break;
			default:	// invalid mode/datatype
				data[audiocount++] = 0;
				break;
			}
		}
	}
	if ((audiocount == n) && !done)
	{
		done = 1;
		_beginthread(WriteAudioFile, 0, (void *)data);
	}
}

void WriteScaledAudioFile (void* arg)
{
	typedef struct
	{
		int n;
		double* ddata;
	} *dstr;
	dstr dstruct = (dstr)arg;

	FILE* file;
	int i;
	double max = 0.0;
	double abs_val;
	const double conv = 2147483647.0;
	int *idata = (int *) malloc0 (dstruct->n * sizeof (int));

	for (i = 0; i < dstruct->n; i++)
	{
		abs_val = fabs (dstruct->ddata[i]);
		if (abs_val > max)
			max = abs_val;
	}
	for (i = 0; i < dstruct->n; i++)
		idata[i] = dstruct->ddata[i] >= 0.0 ? (int)floor(conv * dstruct->ddata[i] / max + 0.5) : (int)ceil(conv * dstruct->ddata[i] / max - 0.5);
	if (file = fopen("AudioFile", "wb"))
	{
		fwrite(idata, sizeof(int), dstruct->n, file);
		fflush(file);
		fclose(file);
	}
	_aligned_free (dstruct->ddata);
	_aligned_free (dstruct);
	_aligned_free (idata);
	_endthread();
}

void WriteScaledAudio (
	double seconds,			// number of seconds of audio to record
	int rate,				// sample rate
	int size,				// incoming buffer size
	double* indata )		// pointer to incoming data buffer
{
	static int ready;
	typedef struct
	{
		int n;
		double* ddata;
	} dstr, *DSTR;
	static DSTR dstruct;

	static int count, complete;
	int i;
	
	if (!ready)
	{
		dstruct = (DSTR) malloc0 (sizeof (dstr));
		dstruct->n = 2 * (int)(seconds * rate);
		dstruct->ddata = (double *) malloc0 (dstruct->n * sizeof (double));
		ready = 1;
	}
	for (i = 0; i < size; i++)
	{
		if (count < dstruct->n)
		{
			dstruct->ddata[count++] = indata[2 * i + 0];
			dstruct->ddata[count++] = indata[2 * i + 1];
		}
	}
	if ((count >= dstruct->n) && !complete)
	{
		complete = 1;
		_beginthread (WriteScaledAudioFile, 0, (void *)dstruct);
	}
}

/********************************************************************************************************
*																										*
*								Bandpass Filter Characterization Utility								*
*																										*
********************************************************************************************************/

double* model_bandpass(int nc, double f_low, double f_high, double rate, int wtype, int points)
{
	double* h = fir_bandpass(nc, f_low, f_high, rate, wtype, 1, 1.0 / (double)nc);
	double* in = (double*)malloc0(points * sizeof(complex));
	double* out = (double*)malloc0(points * sizeof(complex));
	memcpy(in, h, nc * sizeof(complex));
	fftw_plan p = fftw_plan_dft_1d(points, (fftw_complex*)in, (fftw_complex*)out, FFTW_FORWARD, FFTW_PATIENT);
	fftw_execute(p);
	fftw_destroy_plan(p);
	double* mag = (double*)malloc0(points * sizeof(double));
	double mult = 1.0/sqrt(out[0] * out[0] + out[1] * out[1]);
	for (int i = 0; i < points; i++)
	{
		mag[i] = mult * sqrt(out[2 * i + 0] * out[2 * i + 0] + out[2 * i + 1] * out[2 * i + 1]);
		if (mag[i] > 1.0e-300)
			mag[i] = 20.0 * log10(mag[i]);
		else
			mag[i] = -200.0;
	}
	// reverse normal-order
	double* magrev = (double*)malloc0(points * sizeof(double));
	memcpy(magrev, &mag[points / 2], points / 2 * sizeof(double));
	memcpy(&magrev[points / 2], mag, points / 2 * sizeof(double));
	_aligned_free(mag);
	_aligned_free(out);
	_aligned_free(in);
	_aligned_free(h);
	return magrev;
}

void print_bandpass_response (const char* filename, int points, double* response)
{
	int i;
	FILE* file;
	if (file = fopen(filename, "w"))
	{
		for (i = 0; i < points; i++)
			fprintf(file, "%.17e\n", response[i]);
		fflush(file);
		fclose(file);
	}
}

BFCU pbfcu[4];

PORT
int create_bfcu(int id, int min_size, int max_size, double rate, double corner, int points)
{
	// id - from 0 through 3
	// min_size = minimum impulse response size for filters (power of two)
	// max_size = maximum impulse response size for filters (power of two)
	// rate = sample-rate at which filters run
	// corner = -6dB corner frequency; bandpass filter is symmetrical about zero
	//    two corners, one at +corner and the other at -corner
	// points = number of points to generate for each filter response (power of two; >= max_size)
	if (max_size > points) return -1;
	BFCU a = (BFCU)malloc0(sizeof(bfcu));
	int nc, i_wtype, i_dataset;
	int i_corner_offset;
	a->id = id;
	a->min_size = min_size;
	a->max_size = max_size;
	a->rate = rate;
	a->corner = corner;
	a->points = points;
	nc = a->min_size;
	while (nc <= a->max_size)
	{
		for (i_wtype = 0; i_wtype < 2; i_wtype++)
		{
			i_dataset = 2 * (int)log2(nc / a->min_size) + i_wtype;
			a->dataset[i_dataset] = model_bandpass(nc, -a->corner, +a->corner, a->rate, i_wtype, a->points);
		}
		nc *= 2;
	}
	i_corner_offset = (int)round(a->corner / a->rate * a->points);
	a->i_lower_corner = (a->points / 2) - i_corner_offset;
	a->i_upper_corner = (a->points / 2) + i_corner_offset;
	pbfcu[a->id] = a;
	return 0;
}

PORT
void destroy_bfcu(int id)
{
	BFCU a = pbfcu[id];
	int nc = a->min_size;
	while (nc <= a->max_size)
	{
		int i_dataset = 2 * (int)log2(nc / a->min_size) + 0;
		_aligned_free(a->dataset[i_dataset]);
		_aligned_free(a->dataset[i_dataset + 1]);
		nc *= 2;
	}
	_aligned_free(a);
}

PORT
void getFilterCorners(int id, int* lower_index, int* upper_index)
{
	// stores the index of the lower corner and the index of the upper corner at the pointers given.
	BFCU a = pbfcu[id];
	*lower_index = a->i_lower_corner;
	*upper_index = a->i_upper_corner;
}

PORT 
void getFilterCurve(int id, int size, int w_type, int index_low, int index_high, double* segment)
{
	// size = filter_size
	// w_type = window_type (0 -> bh4; 1->bh7)
	// index_low = lower index of the segment you want (range is 0 through points-1)
	// index_high = upper index of the segment you want (range is 0 through points-1)
	// segment = pointer to location where you want the result stored
	BFCU a = pbfcu[id];
	int i_dataset = 2 * (int)log2(size / a->min_size) + w_type;
	memcpy(segment, &a->dataset[i_dataset][index_low], (index_high - index_low + 1) * sizeof(double));
}

void test_bfcu()
{
	create_bfcu(0, 1024, 16384, 48000.0, 1000.0, 16384);
	int lower_corner, upper_corner;
	getFilterCorners(0, &lower_corner, &upper_corner);
	double* segment = (double*)malloc0(1025 * sizeof(double));
	getFilterCurve(0, 4096, 1, upper_corner - 512, upper_corner + 512, segment);
	print_bandpass_response("response", 1025, segment);
	_aligned_free(segment);
	destroy_bfcu(0);
}


/********************************************************************************************************
*																										*
*								           General Debug Utilities								        *
*																										*
********************************************************************************************************/

// Prints to the Visual Studio Debug Window; works like printf(...)
#include <stdarg.h>
void dprintf(const char* format, ...) 
{
	char buffer[512];
	va_list args;
	va_start(args, format);
	// Safely format the string
	vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);
	// Send to Visual Studio Output window
	OutputDebugStringA(buffer);
}
// Usage example:
// dprintf("Error Code: %d at %s\n", 404, "main.cpp");

// Converts a uint32_t to a binary string representation; buf must have space for 
// at least 43 characters (3 for "0b ", 32 for bits, and 1 for null terminator, 7 for
// spaces between groups of 4 bits).
// The string can then be printed to the Visual Studio Debug Window using dprintf, e.g.:
char* uint32_to_bitstr(uint32_t n, char* buf) 
{
	int char_idx = 0;
	buf[char_idx++] = '0';
	buf[char_idx++] = 'b';
	buf[char_idx++] = ' ';

	for (int i = 31; i >= 0; i--) 
	{
		buf[char_idx++] = ((n >> i) & 1) ? '1' : '0';

		// Optional: Add spaces every 4 bits for readability
		if (i > 0 && i % 4 == 0) 
		{
			buf[char_idx++] = ' ';
		}
	}
	buf[char_idx] = '\0';
	return buf;
}
