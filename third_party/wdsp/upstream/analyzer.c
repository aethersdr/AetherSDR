/*  analyzer.c

This file is part of a program that implements a Spectrum Analyzer
used in conjunction with software-defined-radio hardware.

Copyright (C) 2012, 2013, 2014, 2016, 2023, 2025 Warren Pratt, NR0V
Copyright (C) 2012 David McQuate, WA8YWQ - Kaiser window & Bessel function added.

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

#include "comm.h"

DP pdisp[dMAX_DISPLAYS];

double bessi0(double x)
{
	double ax,ans;
	double y;

	if ((ax=fabs(x)) < 3.75) {
		y = x / 3.75,  y = y * y;
		ans = 1.0 + y * (3.5156229 + y * (3.0899424 +  y* (1.2067492
			+ y * (0.2659732 + y * (0.360768e-1 + y * 0.45813e-2)))));
	} else {
		y = 3.75 / ax;
		ans = (exp(ax) / sqrt(ax)) * (0.39894228 + y * (0.1328592e-1
			+ y * (0.225319e-2 + y * (-0.157565e-2 + y * (0.916281e-2
			+ y * (-0.2057706e-1 + y * (0.2635537e-1 + y * (-0.1647633e-1
			+ y * 0.392377e-2))))))));
	}
	return ans;
}

void new_window(int disp, int type, int size, double PiAlpha)
{
	DP a = pdisp[disp];
	int i;
	double arg0, arg1, cgsum, igsum;
	switch (type)
	{
	case 0:					// rectangular window
		{
			a->inv_coherent_gain = 1.0;
			igsum = (double)size;
			for (i = 0; i < size; i++)
				a->window[i] = a->inv_coherent_gain * 1.0;
			break;
		}
	case 1:					// blackman-harris window (4 term)
		{
			arg0 = 2.0 * PI / ((double)size - 1.0);
			cgsum = 0.0;
			igsum = 0.0;
			for (i = 0; i < size; i++)
			{
				arg1 = arg0 * (double)i;
				a->window[i] = 0.35875 - 0.48829 * cos(arg1) + 0.14128 * cos(2.0 * arg1) - 0.01168 * cos(3.0 * arg1);
				cgsum += a->window[i];
				igsum += a->window[i] * a->window[i];
			}
			a->inv_coherent_gain = (double)size / cgsum;
			for (i = 0; i < size; i++)
				a->window[i] *= a->inv_coherent_gain;
			break;
		}
	case 2:					// hann window
		{
			arg0 = 2.0 * PI / ((double)size - 1.0);
			cgsum = 0.0;
			igsum = 0.0;
			for (i = 0; i < size; i++)
			{
				a->window[i] = 0.5 * (1.0 - cos((double)i * arg0));
				cgsum += a->window[i];
				igsum += a->window[i] * a->window[i];
			}
			a->inv_coherent_gain = (double)size / cgsum;
			for (i = 0; i < size; i++)
				a->window[i] *= a->inv_coherent_gain;
			break;
		}
	case 3:					// flat-top window
		{
			arg0 = 2.0 * PI / ((double)size - 1.0);
			cgsum = 0.0;
			igsum = 0.0;
			for (i = 0; i < size; i++)
			{
				arg1 = arg0 * (double)i;
				a->window[i] = 0.21557895 - 0.41663158 * cos(arg1) + 0.277263158 * cos(2.0 * arg1) - 0.083578947 * cos(3.0 * arg1) + 0.006947368 * cos (4.0 * arg1);
				cgsum += a->window[i];
				igsum += a->window[i] * a->window[i];
			}
			a->inv_coherent_gain = (double)size / cgsum;
			for (i = 0; i < size; i++)
				a->window[i] *= a->inv_coherent_gain;
			break;
		}
	case 4:					// hamming window
		{
			arg0 = 2.0 * PI / ((double)size - 1.0);
			cgsum = 0.0;
			igsum = 0.0;
			for (i = 0; i < size; i++)
			{
				a->window[i] = (0.54 - 0.46 * cos((double)i * arg0));
				cgsum += a->window[i];
				igsum += a->window[i] * a->window[i];
			}
			a->inv_coherent_gain = (double)size / cgsum;
			for (i = 0; i < size; i++)
				a->window[i] *= a->inv_coherent_gain;
			break;
		}
	case 5:					// Kaiser window
		{	arg0 = bessi0(PiAlpha);
			arg1 = (double)(size - 1);
			cgsum = 0.0;
			igsum = 0.0;
			for (i = 0; i < size; ++i)
			{
				a->window[i] = bessi0(PiAlpha * sqrt(1.0 - pow(2.0 * (double)i / arg1 - 1.0, 2))) / arg0;
				cgsum += a->window[i];
				igsum += a->window[i] * a->window[i];
			}
			a->inv_coherent_gain = (double)size / cgsum;
			for (i = 0; i < size; i++)
				a->window[i] *= a->inv_coherent_gain;
			break;
		}
	case 6:					// Blackman-Harris window (7-term)
	default:
		{
			arg0 = 2.0 * PI / ((double)size - 1.0);
			cgsum = 0.0;
			igsum = 0.0;
			for (i = 0; i < size; ++i)
			{
				arg1 = cos (arg0 * (double)i);
				a->window[i]   =	+ 6.3964424114390378e-02
						+ arg1 *  ( - 2.3993864599352804e-01
						+ arg1 *  ( + 3.5015956323820469e-01
						+ arg1 *  ( - 2.4774111897080783e-01
						+ arg1 *  ( + 8.5438256055858031e-02
						+ arg1 *  ( - 1.2320203369293225e-02
						+ arg1 *  ( + 4.3778825791773474e-04 ))))));
				cgsum += a->window[i];
				igsum += a->window[i] * a->window[i];
			}
			a->inv_coherent_gain = (double)size / cgsum;
			for (i = 0; i < size; i++)
				a->window[i] *= a->inv_coherent_gain;
			break;
		}
	}
	a->inherent_power_gain = igsum / (double)size;
	a->inv_enb = 1.0 / (a->inherent_power_gain * a->inv_coherent_gain * a->inv_coherent_gain);
	// print_window_gain ("windows.txt", type, a->inv_coherent_gain, a->inherent_power_gain);
}

// spur elimination, REAL input data
void eliminate(int disp, int ss, int LO)
{
	DP a = pdisp[disp];
	int i, k, begin, end, ilim;
	double mag;

	if (ss == a->begin_ss)
		begin = a->fscL + a->clip;
	else
		begin = a->clip;
	if (ss == a->end_ss)
		end = a->out_size - 1 -a->clip - a->fscH;
	else
		end = a->out_size - 1 - a->clip;

	ilim = a->out_size - 1;

	if (a->flip[LO])
		for (i = ilim - begin, k = 0; i > ilim - end; i--, k++)
		{
			mag = (a->fft_out[ss][LO])[i][0] * (a->fft_out[ss][LO])[i][0] + (a->fft_out[ss][LO])[i][1] * (a->fft_out[ss][LO])[i][1];
			if ((a->spec_flag[ss] == 0) || (mag < (a->result[ss])[k]))
				(a->result[ss])[k] = mag;
		}
	else
		for (i = begin, k = 0; i < end; i++, k++)
		{
			mag = (a->fft_out[ss][LO])[i][0] * (a->fft_out[ss][LO])[i][0] + (a->fft_out[ss][LO])[i][1] * (a->fft_out[ss][LO])[i][1];
			if ((a->spec_flag[ss] == 0) || (mag < (a->result[ss])[k]))
				(a->result[ss])[k] = mag;
		}
	a->ss_bins[ss] = k;
}

// spur elimination, COMPLEX input data
void Celiminate(int disp, int ss, int LO)
{
	DP a = pdisp[disp];
	int i, k, begin0, end0, begin1, end1, ilim;
	double mag;

	if (ss == a->begin_ss)
	{
		begin0 = a->out_size / 2 + 1 + a->clip + a->fscL;
		if (begin0 > a->out_size)
			begin1 = begin0 - a->out_size;
		else
			begin1 = 0;
	}
	else
	{
		begin0 = a->out_size / 2 + 1 + a->clip;
		begin1 = 0;
	}
	if (ss == a->end_ss)
	{
		end1 = a->out_size / 2 - a->clip - a->fscH;
		if (end1 < 0)
			end0 = a->out_size + end1;
		else
			end0 = a->out_size;
	}
	else
	{
		end0 = a->out_size;
		end1 = a->out_size / 2 - a->clip;
	}

	ilim = a->out_size - 1;

	if (a->flip[LO])
	{
		for (i = ilim - begin0, k = 0; i > ilim - end0; i--, k++)
		{
			mag = (a->fft_out[ss][LO])[i][0] * (a->fft_out[ss][LO])[i][0] + (a->fft_out[ss][LO])[i][1] * (a->fft_out[ss][LO])[i][1];
			if ((a->spec_flag[ss] == 0) || (mag < (a->result[ss])[k]))
				(a->result[ss])[k] = mag;
		}
		for (i = ilim - begin1; i > ilim - end1; i--, k++)
		{
			mag = (a->fft_out[ss][LO])[i][0] * (a->fft_out[ss][LO])[i][0] + (a->fft_out[ss][LO])[i][1] * (a->fft_out[ss][LO])[i][1];
			if ((a->spec_flag[ss] == 0) || (mag < (a->result[ss])[k]))
				(a->result[ss])[k] = mag;
		}
	}
	else
	{
		for (i = begin0, k = 0; i < end0; i++, k++)
		{
			mag = (a->fft_out[ss][LO])[i][0] * (a->fft_out[ss][LO])[i][0] + (a->fft_out[ss][LO])[i][1] * (a->fft_out[ss][LO])[i][1];
			if ((a->spec_flag[ss] == 0) || (mag < (a->result[ss])[k]))
				(a->result[ss])[k] = mag;
		}
		for (i = begin1; i < end1; i++, k++)
		{
			mag = (a->fft_out[ss][LO])[i][0] * (a->fft_out[ss][LO])[i][0] + (a->fft_out[ss][LO])[i][1] * (a->fft_out[ss][LO])[i][1];
			if ((a->spec_flag[ss] == 0) || (mag < (a->result[ss])[k]))
				(a->result[ss])[k] = mag;
		}
	}
	a->ss_bins[ss] = k;
}

void detector (	int det_type,			// detector type
				int m,					// number of bins
				int num_pixels,			// number of output pixels
				double pix_per_bin,		// pixels per bin
				double bin_per_pix,		// bins per pixel
				double* bins,			// input buffer
				double* pixels,			// output buffer
				double inv_enb,			// inverse equivalent noise bandwidth
				double fsclipL,
				double fsclipH,
				double det_offset
				)
{
	int i, imin, ilim;
	int pix_count = 0;
	int rose, fell, next_pix_count, bcount, last_pix_count;
	double prev_maxi, mini, maxi, psum;
	if (pix_per_bin <= 1.0)
	{
		if (fsclipL == floor(fsclipL)) imin = 0;
		else  imin = 1;
		if (fsclipH == floor(fsclipH)) ilim = m;
		else  ilim = m - 1;
		switch (det_type)
		{

		case 0:		// positive peak
			for (i = 0; i < num_pixels; i++)
				pixels[i]   = - 1.0e300;

			for (i = imin; i < ilim; i++)
			{
				pix_count = (int)(det_offset + (double)i * pix_per_bin);
				if (pix_count >= num_pixels) pix_count = num_pixels - 1;
				if (bins[i] > pixels[pix_count])
					pixels[pix_count] = bins[i];
			}
			break;

		case 1:		// rosenfell
			rose         = 0;
			fell         = 0;
			mini         = + 1.0e300;
			maxi         = - 1.0e300;
			prev_maxi    = - 1.0e300;

			for (i = imin; i < ilim; i++)		// for each FFT bin
			{
				// determine the pixel number that this FFT bin goes into
				pix_count = (int)(det_offset + (double)i * pix_per_bin);
				if (pix_count >= num_pixels) pix_count = num_pixels - 1;
				// determine the pixel number for the NEXT FFT bin
				next_pix_count = (int)((double)(i + 1) * pix_per_bin);
				// update the minimum and maximum of the set of bins within the pixel
				if (bins[i] <   mini)     mini = bins[i];
				if (bins[i] >   maxi)     maxi = bins[i];
				// if the next bin is also within the pixel && there is a next bin,
				//    compare its value with the current bin and update rose and fell
				if (next_pix_count == pix_count && i < ilim - 1)
				{
					// NOTE:  when next_pix_count != pix_count, rose and fell do not get updated;
					//    that's OK because we do NOT need to know if there's a rise or fall across bins
					if (bins[i + 1] > bins[i]) rose    = 1;
					if (bins[i + 1] < bins[i]) fell    = 1;
				}
				// if the next bin is NOT within the pixel || there is no next bin, finalize the pixel 
				//    value and reset parameters
				else
				{
					if (rose && fell)
						if (pix_count & 1)				// odd pixel
							pixels[pix_count] = max (prev_maxi, maxi);
						else							// even pixel
							pixels[pix_count] = mini;
					else
						pixels[pix_count] = maxi;
					rose = 0;
					fell = 0;
					prev_maxi = maxi;
					mini = + 1.0e300;
					maxi = - 1.0e300;
				}
			}
			break;

		case 2:		// average - adjusted for window's equivalent noise bandwidth
			psum = 0.0;
			bcount = 0;
			for (i = imin; i < ilim; i++)
			{
				last_pix_count = pix_count;
				pix_count = (int)(det_offset + (double)i * pix_per_bin);
				if (pix_count >= num_pixels) pix_count = num_pixels - 1;
				if (pix_count == last_pix_count)
				{
					psum += bins[i];
					bcount++;
				}
				else
				{
					pixels[last_pix_count] = psum / (double)bcount * inv_enb;
					psum = bins[i];
					bcount = 1;
				}
				if (i == ilim - 1)
				{
					pixels[pix_count] = psum / (double)bcount * inv_enb;
				}
			}
			break;

		case 3:		// sample - adjusted for window's equivalent noise bandwidth
			bcount = 0;
			for (i = imin; i < ilim; i++)
			{
				last_pix_count = pix_count;
				pix_count = (int)(det_offset + (double)i * pix_per_bin);
				if (pix_count >= num_pixels) pix_count = num_pixels - 1;
				if (pix_count == last_pix_count)
				{
					bcount++;
				}
				else
				{
					pixels[last_pix_count] = bins[i - bcount / 2 - 1] * inv_enb;
					bcount = 1;
				}
				if (i == ilim - 1)
				{
					pixels[pix_count] = bins[i - bcount / 2] * inv_enb;
				}
			}
			break;

		case 4:		// rms
			psum = 0.0;
			bcount = 0;
			for (i = imin; i < ilim; i++)
			{
				last_pix_count = pix_count;
				pix_count = (int)(det_offset + (double)i * pix_per_bin);
				if (pix_count >= num_pixels) pix_count = num_pixels - 1;
				if (pix_count == last_pix_count)
				{
					psum += bins[i] * bins[i];
					bcount++;
				}
				else
				{
					pixels[last_pix_count] = sqrt (psum / (double)bcount) * inv_enb;
					psum = bins[i] * bins[i];
					bcount = 1;
				}
				if (i == ilim - 1)
				{
					pixels[pix_count] = sqrt (psum / (double)bcount) * inv_enb;
				}
			}
			break;
		}
	}
	else
	{
		double frac;
		double pix_pos = fsclipL - floor(fsclipL);
		int ampl_comp = (det_type == 2) || (det_type == 3) || (det_type == 4);
		for (i = 1; i < m; i++)
		{
			while (pix_pos < ((double)i + 1.0e-06) && pix_count < num_pixels)
			{
				frac = pix_pos - (double)(i - 1);
				pixels[pix_count]   = bins[i - 1] * (1.0 - frac) + bins[i] * frac;
				if (ampl_comp) pixels[pix_count] *= inv_enb;
				pix_count++;
				pix_pos += bin_per_pix;
			}
		}
	}
	
}

void avenger (  int av_mode,				// averaging mode
				int num_pixels,				// number of pixels
				int* avail_frames,			// number of available frames for window averaging
				int num_average,			// number of frames to average within a window
				int* av_in_idx,				// in index for av_buff
				int* av_out_idx,			// out index for av_buff
				double av_backmult,			// multiplier for recursive averaging
				double scale,				// scale factor
				double* t_pixels,			// input buffer
				double* av_sum,				// history buffer for averaging
				double** av_buff,			// frame buffer for window averaging
				double* cd,					// correction factor buffer
				int norm,					// if TRUE, normalize to one Hz bandwidth
				double norm_oneHz,			// normalization factor to add
				dOUTREAL* pixels			// output buffer
	)
{
	int i;
	double factor;
	switch (av_mode)
	{
	case -1:	// peak-hold
		{
			for (i = 0; i < num_pixels; i++)
			{
				if (t_pixels[i] > av_sum[i])
					av_sum[i] = t_pixels[i];
				pixels[i] = (dOUTREAL)(10.0 * mlog10(scale * cd[i] * av_sum[i] + 1.0e-60));
			}
			break;
		}
	case 0:		// no averaging
	default:
		{
			for (i = 0; i < num_pixels; i++)
				pixels[i] = (dOUTREAL)(10.0 * mlog10(scale * cd[i] * t_pixels[i] + 1.0e-60));
			break;
		}
	case 1:		// weighted averaging of linear data
		{
			double onem_avb = 1.0 - av_backmult;
			for (i = 0; i < num_pixels; i++)
			{
				av_sum[i] = av_backmult * av_sum[i] + onem_avb * t_pixels[i];
				pixels[i] = (dOUTREAL)(10.0 * mlog10(scale * cd[i] * av_sum[i] + 1.0e-60));
			}
			break;
		}
	case 2:		// window averaging of linear data
		{
			if (*avail_frames < num_average)
			{
				factor = scale / (double)++(*avail_frames);
				for (i = 0; i < num_pixels; i++)
				{
					av_sum[i] += t_pixels[i];
					av_buff[*av_in_idx][i] = t_pixels[i];
					pixels[i] = (dOUTREAL)(10.0 * mlog10(cd[i] * av_sum[i] * factor + 1.0e-60));
				}
			}
			else
			{
				factor = scale / (double)(*avail_frames);
				for (i = 0; i < num_pixels; i++)
				{
					av_sum[i] += t_pixels[i] - (av_buff[*av_out_idx])[i];
					av_buff[*av_in_idx][i] = t_pixels[i];
					pixels[i] = (dOUTREAL)(10.0 * mlog10(cd[i] * av_sum[i] * factor + 1.0e-60));
				}
				if (++(*av_out_idx) == dMAX_AVERAGE)
						*av_out_idx = 0;
			}
			if (++(*av_in_idx) == dMAX_AVERAGE)
				*av_in_idx = 0;
			break;
		}
	case 3:		// weighted averaging of log data - looks nice, not accurate for time-varying signals
		{
			double onem_avb = 1.0 - av_backmult;
			for (i = 0; i < num_pixels; i++)
			{
				av_sum[i] = av_backmult * av_sum[i] + onem_avb * (10.0 * mlog10(scale * cd[i] * t_pixels[i] + 1e-60));
				pixels[i] = (dOUTREAL)av_sum[i];
			}
			break;
		}
	}
	if (norm)
		for (i = 0; i < num_pixels; i++)
			pixels[i] += (dOUTREAL)norm_oneHz;
}

void stitch(int disp)
{
	DP a = pdisp[disp];
	int i, j, k, n, m;
	double* ptr;

	// stitch
	m = 0;
	ptr = a->pre_av_out;
	for (n = a->begin_ss; n <= a->end_ss; n++)
	{
		memcpy(ptr, a->result[n], a->ss_bins[n] * sizeof(double));
		ptr += a->ss_bins[n];
		m += a->ss_bins[n];
	}
	for (i = 0; i < a->num_pixout; i++)	// for each output
	{
		EnterCriticalSection(&a->ResampleSection);
		// if a detection of the same 'det_type' has already been done, use that result
		j = i - 1;
		k = i;
		while (j >= 0)
		{
			if (a->det_type[i] == a->det_type[j])
				k = j;
			j--;
		}
		if (k == i)
			// detect
			detector (a->det_type[i], m, a->num_pixels, a->pix_per_bin, a->bin_per_pix, a->pre_av_out, 
				a->t_pixels[i], a->inv_enb, a->fsclipL, a->fsclipH, a->det_offset);
		else
			memcpy (a->t_pixels[i], a->t_pixels[k], a->num_pixels * sizeof (double));
		// average & convert to dBm
		avenger (a->av_mode[i], a->num_pixels, &a->avail_frames[i], a->num_average[i], &a->av_in_idx[i], &a->av_out_idx[i],
			a->av_backmult[i], a->scale, a->t_pixels[i], a->av_sum[i], a->av_buff[i], a->cd, a->normalize[i], a->norm_oneHz,
			a->pixels[i][a->w_pix_buff[i]]);
		LeaveCriticalSection(&a->ResampleSection);

		EnterCriticalSection(&a->PB_ControlsSection[i]);
			a->last_pix_buff[i] = a->w_pix_buff[i];	
			while ((a->w_pix_buff[i] = (a->w_pix_buff[i] + 1) % dNUM_PIXEL_BUFFS) == a->r_pix_buff[i]);
		LeaveCriticalSection(&a->PB_ControlsSection[i]);
		InterlockedBitTestAndSet(&(a->pb_ready[i][a->last_pix_buff[i]]), 0);
	}
}

DWORD WINAPI spectra (void *pargs)
{
	int i, j;
	int disp = ((int)(uintptr_t)pargs) >> 12;
	int ss = (((int)(uintptr_t)pargs) >> 4) & 255;
	int LO = ((int)(uintptr_t)pargs) & 15;
	DP a = pdisp[disp];

	if (a->stop)
	{
		InterlockedDecrement(a->pnum_threads);
		return 0;
	}

	if ((ss >= a->begin_ss) && (ss <= a->end_ss))
	{
		for (i = 0; i < a->size; i++)
		{
			(a->fft_in[ss][LO])[i] = a->window[i] * (double)((a->I_samples[ss][LO])[a->IQO_idx[ss][LO]]);
			if(++a->IQO_idx[ss][LO] >= a->bsize)
				 a->IQO_idx[ss][LO] -= a->bsize;
		}

		if (a->stop)
		{
			InterlockedDecrement(a->pnum_threads);
			return 0;
		}
		fftw_execute (a->plan[ss][LO]);
	}
	if (a->stop)
	{
		InterlockedDecrement(a->pnum_threads);
		return 0;
	}

	EnterCriticalSection(&(a->EliminateSection[ss]));
	if ((ss >= a->begin_ss) && (ss <= a->end_ss))
		eliminate(disp, ss, LO);
	a->spec_flag[ss] |= 1 << LO;

	if (a->spec_flag[ss] == ((1 << a->num_fft) - 1))
	{
		a->spec_flag[ss] = 0;
		LeaveCriticalSection (&(a->EliminateSection[ss]));

		EnterCriticalSection (&a->StitchSection);
		a->stitch_flag |= ((uint64_t)1) << ss;

		if (a->stitch_flag == ((((uint64_t)1) << a->num_stitch) - 1))
		{
			a->stitch_flag = 0;
			LeaveCriticalSection(&a->StitchSection);
			for (j = 0; j < dMAX_STITCH; j++)
				for (i = 0; i < dMAX_NUM_FFT; i++)
					InterlockedBitTestAndReset(&(a->input_busy[j][i]), 0);
			stitch(disp);
		}
		else
			LeaveCriticalSection(&a->StitchSection);
	}
	else
		LeaveCriticalSection (&(a->EliminateSection[ss]));

	InterlockedDecrement(a->pnum_threads);
	return 1;
}

/********************************************************************************************************
*																										*
*							BEGIN CODE TO GET MAX FFT_BIN WITHIN A FREQ RANGE							*
*																										*
********************************************************************************************************/

// Call in XCreateAnalyzer(...)
// This gets initialized for each 'disp' that is set up.
void Init_DetectMaxBin(int disp)
{
	DP a = pdisp[disp];
	InitializeCriticalSectionAndSpinCount(&a->cs_dmb, 2500);
	a->dmb_run = 0;
	a->dmb_disp = disp;
	a->dmb_ss = 0;
	a->dmb_LO = 0;
	a->dmb_rate = 48000.0;
	a->dmb_fLow = 0.0;
	a->dmb_fHigh = 0.0;
	a->dmb_begin0 = 0;
	a->dmb_end0 = -1;
	a->dmb_begin1 = 0;
	a->dmb_end1 = -1;
	a->dmb_max_dB = -400.0;
}

// Call in DestroyAnalyzer(...)
void Destroy_DetectMaxBin(int disp)
{
	DP a = pdisp[disp];
	DeleteCriticalSection(&a->cs_dmb);
}

// Called from SetupDetectMaxBin(...) AND anytime 'size' changes, e.g., in SetAnalyzer(...)
void calc_dmb(int disp, int size)
{
	DP a = pdisp[disp];
	double bin_spacing, min_freq, max_freq;
	bin_spacing = a->dmb_rate / size;
	min_freq = -a->dmb_rate * (double)((size / 2 - 1)) / (double)size;
	max_freq = -min_freq;
	if (a->dmb_fLow < min_freq) a->dmb_fLow = min_freq;
	if (a->dmb_fLow > max_freq) a->dmb_fLow = max_freq;
	if (a->dmb_fHigh < min_freq) a->dmb_fHigh = min_freq;
	if (a->dmb_fHigh > max_freq) a->dmb_fHigh = max_freq;

	if (a->dmb_fLow < 0.0 && a->dmb_fHigh < 0.0)
	{
		a->dmb_begin0 = size - (int)ceil(-a->dmb_fLow / bin_spacing);
		// Example:  1024, 48K, -2700 => 966
		// Example:  1024, 48K, -23953.125==min_freq => 513
		// NOTE:  since dmb_fLow < 0.0, begin0 will never be greater than a->size - 1
		a->dmb_end0 = size - (int)ceil(-a->dmb_fHigh / bin_spacing);
		// Example:  1024, 48K, -300  => 1017
		a->dmb_begin1 = 0;
		a->dmb_end1 = -1;
	}
	else if (a->dmb_fLow >= 0.0 && a->dmb_fHigh >= 0.0)
	{
		a->dmb_begin0 = (int)round(a->dmb_fLow / bin_spacing);
		// Example:  1024, 48K, +300  => 6
		a->dmb_end0 = (int)round(a->dmb_fHigh / bin_spacing);
		// Example:  1024, 48K, +2700 => 58
		// Example:  1024, 48K, 23953.125==max_freq => 511
		a->dmb_begin1 = 0;
		a->dmb_end1 = -1;
	}
	else	// fLow < 0.0 && fHigh >= 0.0
	{
		a->dmb_begin0 = size - (int)ceil(-a->dmb_fLow / bin_spacing);
		a->dmb_end0 = size - 1;
		a->dmb_begin1 = 0;
		a->dmb_end1 = (int)round(a->dmb_fHigh / bin_spacing);
	}
	a->dmb_decay = exp(-1.0 / (a->dmb_tau * (double)a->dmb_frame_rate));
	a->dmb_max_dB = -400.0;
}


// Call from console, for each 'disp' for which this function is desired.
// Call for initial setup and anytime one of the parameters changes.
// run:			Set to '1' if this is being used; cycles can be saved by setting to
//				'0' if this is not currently in use.
// disp:		Display identifier number.
// ss:			Set to '0' for Thetis use.
// LO:			Set to '0' for Thetis use.
// rate:		Sample_rate of display data, e.g., '192000.0'.
// fLow:		Lowest frequency of frequency range to evaluate, referenced to center_frequency
//				to which DDC is tuned.  For example, for LSB, not using CTUN, this might be
//				'-3000.0'.
// fHigh:		Highest frequency of frequency range to evaluate, referenced to center_frequency
//				to which DDC is tuned.  Example, LSB:  '-300.0'.
// tau:			Metering time constant in seconds.  E.g., '0.5'.
// frame_rate:	Display frame_rate currently in use.  E.g., '60'.
PORT
void SetupDetectMaxBin(int run, int disp, int ss, int LO, double rate, 
	double fLow, double fHigh, double tau, int frame_rate)
{
	// We only allow setup for one (ss,LO) pair at a time for each 'disp'.
	DP a = pdisp[disp];
	
	a->dmb_run = run;
	a->dmb_disp = disp;
	a->dmb_ss = ss;
	a->dmb_LO = LO;
	a->dmb_rate = rate;
	a->dmb_fLow = fLow;
	a->dmb_fHigh = fHigh;
	a->dmb_tau = tau;
	a->dmb_frame_rate = frame_rate;

	calc_dmb(a->dmb_disp, a->size);
}

// Call this function in 'Cspectra(...)', after the FFT.
void DetectMaxBin(int disp, int ss, int LO)
{
	DP a = pdisp[disp];
	int i;
	double mag, dmb_max;
	double dmb_max_dB;
	// If 'run' is set and the FFT Output is from the correct disp, ss, LO ...
	if (a->dmb_run && disp == a->dmb_disp && ss == a->dmb_ss && LO == a->dmb_LO)
	{
		fftw_complex* fft_out = a->fft_out[ss][LO];
		dmb_max = 1.0e-60;
		EnterCriticalSection(&a->cs_dmb);
		for (i = a->dmb_begin0; i <= a->dmb_end0; i++)
		{
			mag = fft_out[i][0] * fft_out[i][0] + fft_out[i][1] * fft_out[i][1];
			if (mag > dmb_max) dmb_max = mag;
		}
		for (i = a->dmb_begin1; i <= a->dmb_end1; i++)
		{
			mag = fft_out[i][0] * fft_out[i][0] + fft_out[i][1] * fft_out[i][1];
			if (mag > dmb_max) dmb_max = mag;
		}

		a->dmb_max_dB -= fabs((1.0 - a->dmb_decay) * a->dmb_max_dB);
		dmb_max_dB = 10.0 * mlog10(a->scale * dmb_max);
		if (dmb_max_dB > a->dmb_max_dB) a->dmb_max_dB = dmb_max_dB;
		LeaveCriticalSection(&a->cs_dmb);
		// for test only.
		// printf("Max Bin = %.5e\n", a->dmb_max_dB);
	}
}

// Call from console, for each 'disp' for which this function is desired.
// Always returns the value from the most recent display frame.
PORT
double GetDetectMaxBin(int disp)
{
	DP a = pdisp[disp];
	double dmb_max_dB;
	EnterCriticalSection(&a->cs_dmb);
	dmb_max_dB = a->dmb_max_dB;
	LeaveCriticalSection(&a->cs_dmb);
	return dmb_max_dB;
}

/********************************************************************************************************
*																										*
*							END CODE TO GET MAXIMUM FFT_BIN WITHIN A FREQ RANGE							*
*																										*
********************************************************************************************************/

DWORD WINAPI Cspectra (void *pargs)
{
	int i, j;
	int disp = ((int)(uintptr_t)pargs) >> 12;
	int ss = (((int)(uintptr_t)pargs) >> 4) & 255;
	int LO = ((int)(uintptr_t)pargs) & 15;
	DP a = pdisp[disp];
	int trans_size = a->size * sizeof(double);

	if (a->stop)
	{
		InterlockedDecrement(a->pnum_threads);
		return 0;
	}

	if ((ss >= a->begin_ss) && (ss <= a->end_ss))
	{
		for (i = 0; i < a->size; i++)
		{
			(a->Cfft_in[ss][LO])[i][0] = a->window[i] * (double)((a->I_samples[ss][LO])[a->IQO_idx[ss][LO]]);
			(a->Cfft_in[ss][LO])[i][1] = a->window[i] * (double)((a->Q_samples[ss][LO])[a->IQO_idx[ss][LO]]);
			if(++a->IQO_idx[ss][LO] >= a->bsize)
				 a->IQO_idx[ss][LO] -= a->bsize;
		}

		if (a->stop)
		{
			InterlockedDecrement(a->pnum_threads);
			return 0;
		}
		fftw_execute (a->Cplan[ss][LO]);

		// Detect value of Max FFT Bin in a freq range
		DetectMaxBin(disp, ss, LO);
		// 

	}

	if (a->stop)
	{
		InterlockedDecrement(a->pnum_threads);
		return 0;
	}

	if (InterlockedBitTestAndReset(&(a->snap[ss][LO]), 0))
	{
		memcpy((char *)(a->snap_buff[ss][LO]), (char *)(a->fft_out[ss][LO]) + trans_size, trans_size);
		memcpy((char *)(a->snap_buff[ss][LO]) + trans_size, (char *)(a->fft_out[ss][LO]), trans_size);
		SetEvent(a->hSnapEvent[ss][LO]);
	}

	EnterCriticalSection(&(a->EliminateSection[ss]));
	if ((ss >= a->begin_ss) && (ss <= a->end_ss))
		Celiminate(disp, ss, LO);
	a->spec_flag[ss] |= 1 << LO;

	if (a->spec_flag[ss] == ((1 << a->num_fft) - 1))
	{
		a->spec_flag[ss] = 0;
		LeaveCriticalSection (&(a->EliminateSection[ss]));

		EnterCriticalSection (&a->StitchSection);
		a->stitch_flag |= ((uint64_t)1) << ss;

		if (a->stitch_flag == ((((uint64_t)1) << a->num_stitch) - 1))
		{
			a->stitch_flag = 0;
			LeaveCriticalSection(&a->StitchSection);
			for (j = 0; j < dMAX_STITCH; j++)
				for (i = 0; i < dMAX_NUM_FFT; i++)
					InterlockedBitTestAndReset(&(a->input_busy[j][i]), 0);
			stitch(disp);
		}
		else
			LeaveCriticalSection(&a->StitchSection);
	}
	else
		LeaveCriticalSection (&(a->EliminateSection[ss]));

	InterlockedDecrement(a->pnum_threads);
	return 1;
}

void interpolate(int disp, int set, double fmin, double fmax, int num_pixels)
{
	DP a = pdisp[disp];
	int i; 
	double f;
	int n = a->n_freqs[set];
	int k;
	int kmin = 0;
	int kmax = n - 1;
	int kdelta;
	double dx;
	double mag;

	for (i = 0; i < num_pixels; i++)
    {
		f = fmin + (double)i * (fmax - fmin) / (double)(num_pixels - 1);
		
		if (f < (a->freqs[set])[0])
            k = 0;
        else if (f > (a->freqs[set])[n - 1])
            k = n - 2;
        else
		{
			kdelta = 1;

			while (f < (a->freqs[set])[kmin])
			{
				kmin = max(0, kmin - kdelta);
				kdelta += kdelta;
			}
			while (f > (a->freqs[set])[kmax])
			{
				kmax = min(n - 1, kmax + kdelta);
				kdelta += kdelta;
			}
			k = (kmin + kmax) / 2;
			while ((kmax - kmin) > 1)
			{
				k = (kmin + kmax) / 2;
				if (f > (a->freqs[set])[k])
					kmin = k;
				else
					kmax = k--;
			}
		}

        dx = f - (a->freqs[set])[k];

        mag = (((a->ac3[set][0])[k] * dx + (a->ac2[set][0])[k]) * dx + (a->ac1[set][0])[k]) * dx + (a->ac0[set][0])[k];
		a->cd[i] = mag * mag;
	}
	return;
}

int build_interpolants(int disp, int set, int n, int m, double *x, double (*y)[dMAX_M])
{
	DP a = pdisp[disp];
	double dx[dMAX_N];
	double idx[dMAX_N];
	double dmain[dMAX_N];
	double dsub[dMAX_N];
	double dsup[dMAX_N];
	double d[dMAX_N][dMAX_M];
	double S[dMAX_N][dMAX_M];
	double b[dMAX_N];
	double v[dMAX_N][dMAX_M];
	double tmp;
	int i, j;
	if (n < 3) return -1;
    for (i = 0; i < n - 1; i++)
    {
        dx[i] = x[i + 1] - x[i];

        if (dx[i] < 1e-30)
            return -1;
        idx[i] = 1.0 / dx[i];
    }

    for (i = 1; i <= n - 2; i++)
    {
        if (i == 1)
        {
            dsub[i] = 0.0;
            dmain[i] = 3.0 * dx[i - 1] + 2.0 * dx[i];
            dsup[i] = dx[i];
        }
        else if (i == (n - 2))
        {
            dsub[i] = dx[i - 1];
            dmain[i] = 2.0 * dx[i - 1] + 3.0 * dx[i];
            dsup[i] = 0.0;
        }
        else
        {
            dsub[i] = dx[i - 1];
            dmain[i] = 2.0 * (dx[i - 1] + dx[i]);
            dsup[i] = dx[i];
        }
		for (j = 0; j < m; j++)
            d[i][j] = 6.0 * ((y[i + 1][j] - y[i][j]) * idx[i] - (y[i][j] - y[i - 1][j]) * idx[i - 1]);
    }

    b[1] = dmain[1];
    for (j = 0; j < m; j++)
        v[1][j] = d[1][j];

    for (i = 2; i <= n - 2; i++)
    {
        tmp = dsub[i] / b[i - 1];
        b[i] = dmain[i] - tmp * dsup[i - 1];
        for (j = 0; j < m; j++)
            v[i][j] = d[i][j] - tmp * v[i - 1][j];
    }

    for (j = 0; j < m; j++)
        S[n - 2][j] = v[n - 2][j] / b[n - 2];

    for (i = n - 3; i >= 1; i--)
        for (j = 0; j < m; j++)
            S[i][j] = (v[i][j] - dsup[i] * S[i + 1][j]) / b[i];

    for (j = 0; j < m; j++)
    {
        S[0][j] = S[1][j];
        S[n - 1][j] = S[n - 2][j];
    }

    for (i = 0; i < n - 1; i++)
        for (j = 0; j < m; j++)
        {
            (a->ac3[set][j])[i] = (S[i + 1][j] - S[i][j]) / (6.0 * dx[i]);
            (a->ac2[set][j])[i] = 0.5 * S[i][j];
            (a->ac1[set][j])[i] = (y[i + 1][j] - y[i][j]) * idx[i] - (2.0 * dx[i] * S[i][j] + dx[i] * S[i + 1][j]) / 6.0;
            (a->ac0[set][j])[i] = y[i][j];
        }
    return 0;
}

void __cdecl sendbuf(void *arg)
{
	DP a = pdisp[(int)(uintptr_t)arg];
	while(!a->end_dispatcher)
	{
		for (a->ss = 0; a->ss < a->num_stitch; a->ss++)
			for (a->LO = 0; a->LO < a->num_fft; a->LO++)
			{
				if (!_InterlockedAnd(&(a->input_busy[a->ss][a->LO]), 1) && _InterlockedAnd(&(a->buff_ready[a->ss][a->LO]), 1))
				{
					InterlockedBitTestAndSet(&(a->input_busy[a->ss][a->LO]), 0);

					a->IQO_idx[a->ss][a->LO] = a->IQout_index[a->ss][a->LO];
					
					InterlockedIncrement(a->pnum_threads);
					if (a->type == 0)
						QueueUserWorkItem(spectra, (void *)(((uintptr_t)arg << 12) + (a->ss << 4) + a->LO), 0);
					else
						QueueUserWorkItem(Cspectra, (void *)(((uintptr_t)arg << 12) + (a->ss << 4) + a->LO), 0);

					if((a->IQout_index[a->ss][a->LO] += a->incr) >= a->bsize)
						a->IQout_index[a->ss][a->LO] -= a->bsize;

					EnterCriticalSection(&(a->BufferControlSection[a->ss][a->LO]));
					if ((a->have_samples[a->ss][a->LO] -= a->incr) < a->size)
						InterlockedBitTestAndReset(&(a->buff_ready[a->ss][a->LO]), 0);
					LeaveCriticalSection(&(a->BufferControlSection[a->ss][a->LO]));
				}
			}
		Sleep(1);
	}
	InterlockedBitTestAndReset(&a->dispatcher, 0);
	_endthread();
}

void CalcBandwidthNormalization (DP a)
{
	double bin_width;
	bin_width = (double)a->sample_rate / (double)a->size;
	a->norm_oneHz = 10.0 * mlog10 (1.0 / bin_width);
}

PORT
void ResetPixelBuffers(int disp)
{
	//[2.10.2]MW0LGE reset all the pixel and average buffers
	DP a = pdisp[disp];
	int i, j, k;

	EnterCriticalSection(&a->SetAnalyzerSection);
	EnterCriticalSection(&a->ResampleSection);
	for (i = 0; i < dMAX_PIXOUTS; i++)
	{
		for (j = 0; j < dMAX_PIXELS; j++)
			a->t_pixels[i][j] = 0.0;
		for (j = 0; j < dMAX_AVERAGE; j++)
			for (k = 0; k < dMAX_PIXELS; k++)
				a->av_buff[i][j][k] = 0.0f;
		switch (a->av_mode[i])
		{
		case 1:
			for (j = 0; j < dMAX_PIXELS; j++)
				a->av_sum[i][j] = 1.0e-12;
			break;
		case 2:
			//done below
			//a->avail_frames[i] = 0;
			//a->av_in_idx[i] = 0;
			//a->av_out_idx[i] = 0;
			break;
		case 3:
			for (j = 0; j < dMAX_PIXELS; j++)
				a->av_sum[i][j] = -160.0;
			break;
		default:
			memset((void*)a->av_sum[i], 0, sizeof(double) * dMAX_PIXELS);
			break;
		}
		a->avail_frames[i] = 0;
		a->av_in_idx[i] = 0;
		a->av_out_idx[i] = 0;
		EnterCriticalSection(&a->PB_ControlsSection[i]);
		a->w_pix_buff[i] = 0;
		a->r_pix_buff[i] = 0;
		a->last_pix_buff[i] = 0;
		for (j = 0; j < dNUM_PIXEL_BUFFS; j++)
			a->pb_ready[i][j] = 0;
		LeaveCriticalSection(&a->PB_ControlsSection[i]);
	}
	memset((void*)a->pre_av_out, 0, sizeof(double) * a->max_size * a->max_stitch);
	LeaveCriticalSection(&a->ResampleSection);
	EnterCriticalSection(&a->StitchSection);
	for (i = 0; i < dMAX_STITCH; i++)
		for (j = 0; j < dMAX_NUM_FFT; j++)
			a->input_busy[i][j] = 0;
	for (i = 0; i < dMAX_STITCH; i++)
		a->spec_flag[i] = 0;
	a->stitch_flag = 0;
	a->ss = 0;
	a->LO = 0;
	for (i = 0; i < dMAX_STITCH; i++)
		for (j = 0; j < dMAX_NUM_FFT; j++)
		{
			EnterCriticalSection(&(a->BufferControlSection[i][j]));
			a->buff_ready[i][j] = 0;
			a->have_samples[i][j] = 0;
			a->IQin_index[i][j] = 0;
			a->IQout_index[i][j] = 0;
			LeaveCriticalSection(&(a->BufferControlSection[i][j]));
		}
	LeaveCriticalSection(&a->StitchSection);
	LeaveCriticalSection(&a->SetAnalyzerSection);
}

PORT    
void SetAnalyzer (	int disp,			// display identifier
					int n_pixout,		// pixel output identifier
					int n_fft,			// number of LO frequencies = number of ffts used in elimination
					int typ,			// 0 for real input data (I only); 1 for complex input data (I & Q)
					int *flp,			// vector with one elt for each LO frequency, 1 if high-side LO, 0 otherwise 
					int sz,				// size of the fft, i.e., number of input samples
					int bf_sz,			// number of samples transferred for each OpenBuffer()/CloseBuffer()
					int win_type,		// integer specifying which window function to use
					double pi,			// PiAlpha parameter for Kaiser window
					int ovrlp,			// number of samples each fft (other than the first) is to re-use from the previous 
					int clp,			// number of fft output bins to be clipped from EACH side of each sub-span
					double fscLin,		// number of bins to clip from low end of entire span
					double fscHin,		// number of bins to clip from high end of entire span
					int n_pix,			// number of pixel values to return.  may be either <= or > number of bins 
					int n_stch,			// number of sub-spans to concatenate to form a complete span 
					int calset,			// identifier of which set of calibration data to use 
					double fmin,		// frequency at first pixel value 
					double fmax,		// frequency at last pixel value
					int max_w
				 )
{
	DP a = pdisp[disp];
	int i, j;

	EnterCriticalSection(&a->SetAnalyzerSection);
	a->end_dispatcher = 1;
	while (InterlockedAnd(&a->dispatcher, 1))
		Sleep(1);
	a->stop = 1;
	while (_InterlockedAnd(a->pnum_threads, 1023))
		Sleep(1);
	a->num_pixout = n_pixout;
	a->num_fft = n_fft;
	a->type = typ;
	a->buff_size = bf_sz;
	for (i = 0; i < a->num_fft; i++)
		a->flip[i] = *(flp + i);
	a->overlap = ovrlp;
	a->clip = clp;
	a->fsclipL = fscLin;
	a->fsclipH = fscHin;
	a->num_stitch = n_stch;

	if (sz != a->size)
	{
		for (i = 0; i < a->max_stitch; i++)
			for (j = 0; j < a->max_num_fft; j++)
			{
				if (a->plan[i][j])		fftw_destroy_plan (a->plan[i][j]);
				if (a->Cplan[i][j])		fftw_destroy_plan (a->Cplan[i][j]);
				a->plan[i][j] = fftw_plan_dft_r2c_1d(sz, a->fft_in[i][j], a->fft_out[i][j], FFTW_PATIENT);
				a->Cplan[i][j] = fftw_plan_dft_1d(sz, a->Cfft_in[i][j], a->fft_out[i][j], FFTW_FORWARD, FFTW_PATIENT);
			}

		// Setup DetectMaxBin for a 'size' change.
		calc_dmb(disp, sz);
		//

	}

	if ((sz != a->size) || (win_type != a->window_type) || (pi != a->PiAlpha))
		new_window(disp, win_type, sz, pi);

	a->size = sz;
	a->window_type = win_type;
	a->PiAlpha = pi;
	a->max_writeahead = max_w;
	CalcBandwidthNormalization (a);
	if (((fmin != a->f_min) || (fmax != a->f_max)) && ((fmin == 0.0) && (fmax == 0.0)))
		for (i = 0; i < dMAX_PIXELS; i++)
			a->cd[i] = 1.0;

	if (((fmax != 0.0) || (fmin != 0.0)) && ((n_pix != a->num_pixels) || (fmin != a->f_min) || (fmax != a->f_max) || (calset != a->cal_set) || a->cal_changed))
		interpolate(disp, calset, fmin, fmax, n_pix);

	a->incr = a->size - a->overlap;
	a->num_pixels = n_pix;
	a->f_min = fmin;
	a->f_max = fmax;
	a->cal_set = calset;
	a->cal_changed = 0;

	if (a->type == 0)
	{
		a->out_size = a->size / 2 + 1;
		a->scale = 4.0 / ((double)a->size * (double)a->size);
	}
	else
	{
		a->out_size = a->size;
		a->scale = 1.0 / ((double)a->size * (double)a->size);
	}

	a->begin_ss = 0;
	a->end_ss = a->num_stitch - 1;
	a->fscL = (int)a->fsclipL;
	a->fscH = (int)a->fsclipH;
	while (a->fscL >= (a->out_size - 1 - 2 * a->clip))
	{
		a->fscL -= a->out_size - 1 - 2 * a->clip;
		a->ss_bins[a->begin_ss] = 0;
		a->begin_ss++;
	}
	while (a->fscH >= (a->out_size - 1 - 2 * a->clip))
	{
		a->fscH -= a->out_size - 1 - 2 * a->clip;
		a->ss_bins[a->end_ss] = 0;
		a->end_ss--;
	}

	a->pix_per_bin = (double)a->num_pixels / ((double)(a->num_stitch * (a->out_size - 1 - 2 * a->clip)) - a->fsclipL - a->fsclipH - 1.0);
	a->det_offset = -a->pix_per_bin * (a->fsclipL - floor(a->fsclipL));
	a->bin_per_pix = ((double)(a->num_stitch * (a->out_size - 1 - 2 * a->clip)) - 1.0 - a->fsclipL - a->fsclipH) / ((double)a->num_pixels - 1.0);

	for (i = 0; i < dMAX_STITCH; i++)
		for (j = 0; j < dMAX_NUM_FFT; j++)
			a->input_busy[i][j] = 0;

	for (i = 0; i < dMAX_STITCH; i++)
		a->spec_flag[i] = 0;
	a->stitch_flag = 0;
	for (i = 0; i < dMAX_PIXOUTS; i++)
	{
		a->w_pix_buff[i] = 0;
		a->r_pix_buff[i] = 0;
		a->last_pix_buff[i] = 0;
		for (j = 0; j < dNUM_PIXEL_BUFFS; j++)
			a->pb_ready[i][j] = 0;
	}
	a->ss = 0;
	a->LO = 0;
	for (i = 0; i < dMAX_STITCH; i++)
		for (j = 0; j < dMAX_NUM_FFT; j++)
		{
			a->buff_ready[i][j] = 0;
			a->have_samples[i][j] = 0;
			a->IQin_index[i][j] = 0;
			a->IQout_index[i][j] = 0;
		}

	a->stop = 0;
	a->end_dispatcher = 0;
	LeaveCriticalSection(&a->SetAnalyzerSection);
}

PORT
void XCreateAnalyzer(	int disp,
						int *success,
						int m_size,
						int m_num_fft,
						int m_stitch,
						char *app_data_path
						)
{

	int i, j;
	DP a = (DP) malloc0 (sizeof(dp));
	pdisp[disp] = a;

	a->max_size = m_size;
	a->max_num_fft = m_num_fft;
	a->max_stitch = m_stitch;
	
	a->pnum_threads = (LONG*) malloc0 (sizeof (LONG));

	for (i = 0; i < a->max_stitch; i++)
		for (j = 0; j < a->max_num_fft; j++)
		{
			a->hSnapEvent[i][j] = CreateEvent(NULL, FALSE, FALSE, TEXT("snap"));
			a->snap[i][j] = 0;
		}
	InitializeCriticalSectionAndSpinCount(&a->ResampleSection, 0);
	InitializeCriticalSectionAndSpinCount(&a->SetAnalyzerSection, 0);
	InitializeCriticalSectionAndSpinCount(&a->StitchSection, 0);
	for (i = 0; i < dMAX_PIXOUTS; i++)
		InitializeCriticalSectionAndSpinCount(&a->PB_ControlsSection[i], 0);
	for (i = 0; i < dMAX_STITCH; i++)
	{
		InitializeCriticalSectionAndSpinCount(&(a->EliminateSection[i]), 0);
		for (j = 0; j < dMAX_NUM_FFT; j++)
			InitializeCriticalSectionAndSpinCount(&(a->BufferControlSection[i][j]), 0);
	}

	a->window = (double*) malloc0 (sizeof(double) * a->max_size);

	for (i = 0; i < a->max_stitch; i++)
	{
		a->result[i] = (double*) malloc0 (sizeof(double) * a->max_size);

	}
	for (i = 0; i < a->max_stitch; i++)
		for (j = 0; j < a->max_num_fft; j++)
		{
			a->plan[i][j] = 0;
			a->Cplan[i][j] = 0;
			a->fft_in[i][j]   = (double*) malloc0 (sizeof(double) * a->max_size);
			a->Cfft_in[i][j]  = (fftw_complex*) fftw_malloc(sizeof(fftw_complex) * a->max_size);
			a->fft_out[i][j]  = (fftw_complex*) fftw_malloc(sizeof(fftw_complex) * a->max_size);
		}
	a->pre_av_out = (double*) malloc0 (sizeof(double) * a->max_size * a->max_stitch);
	for (i = 0; i < dMAX_PIXOUTS; i++)
	{
		a->det_type[i] = 0;
		a->av_mode[i] = 0;
		a->av_sum[i] = (double*) malloc0 (sizeof(double) * dMAX_PIXELS);
		for (j = 0; j < dMAX_AVERAGE; j++)
			a->av_buff[i][j] = (double*) malloc0 (sizeof(double) * dMAX_PIXELS);
		a->t_pixels[i] = (double*) malloc0 (sizeof(double) * dMAX_PIXELS);
		for (j = 0; j < dNUM_PIXEL_BUFFS; j++)
			a->pixels[i][j] = (dOUTREAL*) malloc0 (sizeof(dOUTREAL) * dMAX_PIXELS);
	}
	
	a->cd = (double*) malloc0 (sizeof(double) * dMAX_PIXELS);
	for (j = 0; j < dMAX_PIXELS; j++)
		a->cd[j] = 1.0;
	for (i = 0; i < dMAX_CAL_SETS; i++)
	{
		a->freqs[i] = (double*) malloc0 (sizeof(double) * dMAX_N);
		for (j = 0; j < dMAX_M; j++)
		{
			a->ac3[i][j] = (double*) malloc0 (sizeof(double) * dMAX_N);
			a->ac2[i][j] = (double*) malloc0 (sizeof(double) * dMAX_N);
			a->ac1[i][j] = (double*) malloc0 (sizeof(double) * dMAX_N);
			a->ac0[i][j] = (double*) malloc0 (sizeof(double) * dMAX_N);
		}
	}
	
	a->size = -1;
	a->window_type = -1;
	a->num_pixels = -1;
	a->cal_set = -1;
	a->f_min = -1.0;
	a->f_max = -1.0;

	a->bsize = a->max_size * dSAMP_BUFF_MULT;
	for (i = 0; i < a->max_stitch; i++)
		for (j = 0; j < a->max_num_fft; j++)
		{
			a->I_samples[i][j] = (dINREAL*) malloc0 (sizeof(dINREAL) * a->bsize);
			a->Q_samples[i][j] = (dINREAL*) malloc0 (sizeof(dINREAL) * a->bsize);
		}

	// Initialize DetectMaxBin functionality
	Init_DetectMaxBin(disp);
	// for test only.
	// SetupDetectMaxBin(1, 0, 0, 0, 192000.0, -3000.0, -300.0, 0.5, 60);
	//

	*success = 0;
}

PORT   
void DestroyAnalyzer(int disp)
{
	DP a = pdisp[disp];
	int i, j;

	a->end_dispatcher = 1;
	while (InterlockedAnd(&a->dispatcher, 1))
		Sleep(1);

	for (i = 0; i < a->max_stitch; i++)
		for (j = 0; j < a->max_num_fft; j++)
		{
			_aligned_free  (a->I_samples[i][j]);
			_aligned_free  (a->Q_samples[i][j]);
		}
	
	for (i = 0; i < dMAX_CAL_SETS; i++)
	{
		_aligned_free (a->freqs[i]);
		for (j = 0; j < dMAX_M; j++)
		{
			_aligned_free  (a->ac3[i][j]);
			_aligned_free  (a->ac2[i][j]);
			_aligned_free  (a->ac1[i][j]);
			_aligned_free  (a->ac0[i][j]);
		}
	}
	_aligned_free (a->cd);
	
	for (i = 0; i < dMAX_PIXOUTS; i++)
	{
		for (j = 0; j < dNUM_PIXEL_BUFFS; j++)
			_aligned_free (a->pixels[i][j]);
		_aligned_free  (a->t_pixels[i]);
		for (j = 0; j < dMAX_AVERAGE; j++)
			_aligned_free (a->av_buff[i][j]);
		_aligned_free (a->av_sum[i]);
	}
	
	_aligned_free (a->pre_av_out);
	for (i = 0; i < a->max_stitch; i++)
		for (j = 0; j < a->max_num_fft; j++)
		{
			fftw_destroy_plan (a->plan[i][j]);
			fftw_destroy_plan (a->Cplan[i][j]);
			fftw_free (a->Cfft_in[i][j]);
			_aligned_free (a->fft_in[i][j]);
			fftw_free (a->fft_out[i][j]);
		}
	
	for (i = 0; i < a->max_stitch; i++)
		_aligned_free (a->result[i]);
	_aligned_free (a->window);

	for (i = 0; i < dMAX_STITCH; i++)
	{
		DeleteCriticalSection(&(a->EliminateSection[i]));
		for (j = 0; j < dMAX_NUM_FFT; j++)
			DeleteCriticalSection(&(a->BufferControlSection[i][j]));
	}
	for (i = 0; i < dMAX_PIXOUTS; i++)
		DeleteCriticalSection(&a->PB_ControlsSection[i]);
	DeleteCriticalSection(&a->StitchSection);
	DeleteCriticalSection(&a->SetAnalyzerSection);
	DeleteCriticalSection(&a->ResampleSection);

	for (i = 0; i < a->max_stitch; i++)
		for (j = 0; j < a->max_num_fft; j++)
			CloseHandle(a->hSnapEvent[i][j]);

	_aligned_free ((void *) a->pnum_threads);

	// Destroy DetectMaxBin functionality.
	Destroy_DetectMaxBin(disp);
	//

	_aligned_free (a);
}

PORT   
void GetPixels	(	int disp,
					int pixout,
					dOUTREAL *pix,		//if new pixel values avail, copies to pix and sets flag = 1
					int *flag			//else, returns 0 (try again later)
				)
{
	DP a = pdisp[disp];
	EnterCriticalSection(&a->PB_ControlsSection[pixout]);
		a->r_pix_buff[pixout] = a->last_pix_buff[pixout];
	LeaveCriticalSection(&a->PB_ControlsSection[pixout]);

	if (_InterlockedAnd(&(a->pb_ready[pixout][a->r_pix_buff[pixout]]), 1))
	{
		memcpy (pix, a->pixels[pixout][a->r_pix_buff[pixout]], a->num_pixels * sizeof(dOUTREAL));
		*flag = 1;
		InterlockedBitTestAndReset(&(a->pb_ready[pixout][a->r_pix_buff[pixout]]), 0);
	}
	else
		*flag = 0;
}

PORT
void SnapSpectrum(	int disp,
					int ss,
					int LO,
					double *snap_buff)
{
	DP a = pdisp[disp];
	a->snap_buff[ss][LO] = snap_buff;
	InterlockedBitTestAndSet(&(a->snap[ss][LO]), 0);
	WaitForSingleObject(a->hSnapEvent[ss][LO], INFINITE);
}

PORT
void SnapSpectrumTimeout(	int disp,
							int ss,
							int LO,
							double* snap_buff,
							DWORD timeout,
							int* flag)
{
	DP a = pdisp[disp];
	a->snap_buff[ss][LO] = snap_buff;
	ResetEvent(a->hSnapEvent[ss][LO]);
	InterlockedBitTestAndSet(&(a->snap[ss][LO]), 0);
	if (!WaitForSingleObject(a->hSnapEvent[ss][LO], timeout))
		*flag = 1;
	else
	{
		InterlockedBitTestAndReset(&(a->snap[ss][LO]), 0);
		*flag = 0;
	}
}

int calcompare (const void * a, const void * b)
{
	if (*(double*)a < *(double*)b)
		return -1;
	else if (*(double*)a == *(double*)b)
		return 0;
	else
		return 1;
}

PORT   
void SetCalibration (	int disp,
						int set_num,				//identifier for this calibration data set
						int n_points,				//number of calibration points in the set
						double (*cal)[dMAX_M+1]		//pointer to the calibration table, first
					)								//   column is frequency, add'l columns are
													//	 data for variables being calibrated
{
	DP a = pdisp[disp];
	int i, j;
	int k = 0;
	double y [dMAX_N][dMAX_M];

	qsort (cal, n_points, (dMAX_M+1) * sizeof(double), calcompare);

	for (i = 0; i < n_points; i++)
	{
		if ((i == n_points - 1) || (cal[i][0] != cal[i + 1][0]))
		{
			(a->freqs[set_num])[k] = cal[i][0];
			for (j = 0; j < dMAX_M; j++)
				y[k][j] = cal[i][j + 1];
			k++;
		}
	}

	a->n_freqs[set_num] = k;

	build_interpolants(disp, set_num, a->n_freqs[set_num], dMAX_M, a->freqs[set_num], y);
	a->cal_changed = 1;
}

PORT   
void OpenBuffer(int disp, int ss, int LO, void **Ipointer, void **Qpointer)
{
	DP a = pdisp[disp];
	EnterCriticalSection(&a->SetAnalyzerSection);
	*Ipointer = &((a->I_samples[ss][LO])[a->IQin_index[ss][LO]]);
	*Qpointer = &((a->Q_samples[ss][LO])[a->IQin_index[ss][LO]]);
	LeaveCriticalSection(&a->SetAnalyzerSection);
}

PORT   
void CloseBuffer(int disp, int ss, int LO)
{
	DP a = pdisp[disp];
	EnterCriticalSection(&a->SetAnalyzerSection);
	EnterCriticalSection(&(a->BufferControlSection[ss][LO]));
		if (a->have_samples[ss][LO] > a->max_writeahead)
			{
				//if we're receiving samples too much faster than we're consuming them, skip some
				if ((a->IQout_index[ss][LO] += a->have_samples[ss][LO] - a->max_writeahead) >= a->bsize)
						a->IQout_index[ss][LO] -= a->bsize;
				a->have_samples[ss][LO] = a->max_writeahead;
			}
		if ((a->have_samples[ss][LO] += a->buff_size) >= a->size)
			InterlockedBitTestAndSet(&(a->buff_ready[ss][LO]), 0);
	LeaveCriticalSection(&(a->BufferControlSection[ss][LO]));
	if((a->IQin_index[ss][LO] += a->buff_size) >= a->bsize)	//REQUIRES buff_size IS A SUB-MULTIPLE OF SIZE OF INPUT SAMPLE BUFFS!
		a->IQin_index[ss][LO] = 0;

	if (!InterlockedAnd(&a->dispatcher, 1))
	{
		InterlockedBitTestAndSet (&a->dispatcher, 0);
		LeaveCriticalSection(&a->SetAnalyzerSection);
		_beginthread(sendbuf, 0, (void *)(uintptr_t)disp);
	}
	else
		LeaveCriticalSection(&a->SetAnalyzerSection);
}

PORT
void Spectrum(int disp, int ss, int LO, dINREAL* pI, dINREAL* pQ)
{
	dINREAL *Ipointer;
	dINREAL *Qpointer;
	DP a = pdisp[disp];
	EnterCriticalSection(&a->SetAnalyzerSection);
	Ipointer = &((a->I_samples[ss][LO])[a->IQin_index[ss][LO]]);
	Qpointer = &((a->Q_samples[ss][LO])[a->IQin_index[ss][LO]]);
	LeaveCriticalSection(&a->SetAnalyzerSection);

	memcpy(Ipointer, pI, a->buff_size * sizeof(dINREAL));
	memcpy(Qpointer, pQ, a->buff_size * sizeof(dINREAL));

	EnterCriticalSection(&a->SetAnalyzerSection);
	EnterCriticalSection(&(a->BufferControlSection[ss][LO]));
		if (a->have_samples[ss][LO] > a->max_writeahead)
			{
				//if we're receiving samples too much faster than we're consuming them, skip some
				if ((a->IQout_index[ss][LO] += a->have_samples[ss][LO] - a->max_writeahead) >= a->bsize)
						a->IQout_index[ss][LO] -= a->bsize;
				a->have_samples[ss][LO] = a->max_writeahead;
			}
		if ((a->have_samples[ss][LO] += a->buff_size) >= a->size)
			InterlockedBitTestAndSet(&(a->buff_ready[ss][LO]), 0);
	LeaveCriticalSection(&(a->BufferControlSection[ss][LO]));
	if((a->IQin_index[ss][LO] += a->buff_size) >= a->bsize)	//REQUIRES buff_size IS A SUB-MULTIPLE OF SIZE OF INPUT SAMPLE BUFFS!
		a->IQin_index[ss][LO] = 0;

	if (!InterlockedAnd(&a->dispatcher, 1))
	{
		InterlockedBitTestAndSet(&a->dispatcher, 0);
		LeaveCriticalSection(&a->SetAnalyzerSection);
		_beginthread(sendbuf, 0, (void *)(uintptr_t)disp);
	}
	else
		LeaveCriticalSection(&a->SetAnalyzerSection);
}

PORT
void Spectrum2(int run, int disp, int ss, int LO, dINREAL* pbuff)
{
	if (run)
	{
		int i;
		dINREAL *Ipointer;
		dINREAL *Qpointer;
		DP a = pdisp[disp];
		EnterCriticalSection(&a->SetAnalyzerSection);
		Ipointer = &((a->I_samples[ss][LO])[a->IQin_index[ss][LO]]);
		Qpointer = &((a->Q_samples[ss][LO])[a->IQin_index[ss][LO]]);
		LeaveCriticalSection(&a->SetAnalyzerSection);

		for (i = 0; i < a->buff_size; i++)
		{
			Ipointer[i] = pbuff[2 * i + 1];
			Qpointer[i] = pbuff[2 * i + 0];
		}

		EnterCriticalSection(&a->SetAnalyzerSection);
		EnterCriticalSection(&(a->BufferControlSection[ss][LO]));
			if (a->have_samples[ss][LO] > a->max_writeahead)
				{
					//if we're receiving samples too much faster than we're consuming them, skip some
					if ((a->IQout_index[ss][LO] += a->have_samples[ss][LO] - a->max_writeahead) >= a->bsize)
							a->IQout_index[ss][LO] -= a->bsize;
					a->have_samples[ss][LO] = a->max_writeahead;
				}
			if ((a->have_samples[ss][LO] += a->buff_size) >= a->size)
				InterlockedBitTestAndSet(&(a->buff_ready[ss][LO]), 0);
		LeaveCriticalSection(&(a->BufferControlSection[ss][LO]));
		if((a->IQin_index[ss][LO] += a->buff_size) >= a->bsize)	//REQUIRES buff_size IS A SUB-MULTIPLE OF SIZE OF INPUT SAMPLE BUFFS!
			a->IQin_index[ss][LO] = 0;

		if (!InterlockedAnd(&a->dispatcher, 1))
		{
			InterlockedBitTestAndSet(&a->dispatcher, 0);
			LeaveCriticalSection(&a->SetAnalyzerSection);
			_beginthread(sendbuf, 0, (void *)(uintptr_t)disp);
		}
		else
			LeaveCriticalSection(&a->SetAnalyzerSection);
	}
}

PORT
void Spectrum0(int run, int disp, int ss, int LO, double* pbuff)
{
	if (run)
	{
		int i;
		dINREAL *Ipointer;
		dINREAL *Qpointer;
		DP a = pdisp[disp];
		EnterCriticalSection(&a->SetAnalyzerSection);
		Ipointer = &((a->I_samples[ss][LO])[a->IQin_index[ss][LO]]);
		Qpointer = &((a->Q_samples[ss][LO])[a->IQin_index[ss][LO]]);
		LeaveCriticalSection(&a->SetAnalyzerSection);

		for (i = 0; i < a->buff_size; i++)
		{
			Ipointer[i] = (dINREAL)pbuff[2 * i + 1];
			Qpointer[i] = (dINREAL)pbuff[2 * i + 0];
		}

		EnterCriticalSection(&a->SetAnalyzerSection);
		EnterCriticalSection(&(a->BufferControlSection[ss][LO]));
			if (a->have_samples[ss][LO] > a->max_writeahead)
				{
					//if we're receiving samples too much faster than we're consuming them, skip some
					if ((a->IQout_index[ss][LO] += a->have_samples[ss][LO] - a->max_writeahead) >= a->bsize)
					 	a->IQout_index[ss][LO] -= a->bsize;
					a->have_samples[ss][LO] = a->max_writeahead;
				}
			if ((a->have_samples[ss][LO] += a->buff_size) >= a->size)
				InterlockedBitTestAndSet(&(a->buff_ready[ss][LO]), 0);
		LeaveCriticalSection(&(a->BufferControlSection[ss][LO]));
		if((a->IQin_index[ss][LO] += a->buff_size) >= a->bsize)	//REQUIRES buff_size IS A SUB-MULTIPLE OF SIZE OF INPUT SAMPLE BUFFS!
			a->IQin_index[ss][LO] = 0;

		if (!InterlockedAnd(&a->dispatcher, 1))
		{
			InterlockedBitTestAndSet(&a->dispatcher, 0);
			LeaveCriticalSection(&a->SetAnalyzerSection);
			_beginthread(sendbuf, 0, (void *)(uintptr_t)disp);
		}
		else
			LeaveCriticalSection(&a->SetAnalyzerSection);
	}
}

PORT
void SetDisplayDetectorMode (int disp, int pixout, int mode)
{
	DP a = pdisp[disp];
	if (a->det_type[pixout] != mode)
	{
		EnterCriticalSection (&a->ResampleSection);
		a->det_type[pixout] = mode;
		LeaveCriticalSection (&a->ResampleSection);
	}
}

PORT
void SetDisplayAverageMode (int disp, int pixout, int mode)
{
	int i;
	DP a = pdisp[disp];
	if (a->av_mode[pixout] != mode)
	{
		EnterCriticalSection (&a->ResampleSection);
		a->av_mode[pixout] = mode;
		switch (mode)
		{
		case 1:
			for (i = 0; i < dMAX_PIXELS; i++)
				a->av_sum[pixout][i] = 1.0e-12;
			break;
		case 2:
			a->avail_frames[pixout] = 0;
			a->av_in_idx[pixout] = 0;
			a->av_out_idx[pixout] = 0;
			break;
		case 3:
			for (i = 0; i < dMAX_PIXELS; i++)
				a->av_sum[pixout][i] = -160.0;
			break;
		default:
			memset ((void *)a->av_sum[pixout], 0, sizeof(double) * dMAX_PIXELS);
			break;
		}
		LeaveCriticalSection (&a->ResampleSection);
	}
}

PORT
void SetDisplayNumAverage (int disp, int pixout, int num)
{
	DP a = pdisp[disp];
	if (a->num_average[pixout] != num)
	{
		EnterCriticalSection (&a->ResampleSection);
		a->num_average[pixout] = num;
		a->avail_frames[pixout] = 0;
		a->av_in_idx[pixout] = 0;
		a->av_out_idx[pixout] = 0;
		LeaveCriticalSection (&a->ResampleSection);
	}
}

PORT
void SetDisplayAvBackmult (int disp, int pixout, double mult)
{
	DP a = pdisp[disp];
	if (a->av_backmult[pixout] != mult)
	{
		EnterCriticalSection (&a->ResampleSection);
		a->av_backmult[pixout] = mult;
		LeaveCriticalSection (&a->ResampleSection);
	}
}

PORT
void SetDisplaySampleRate (int disp, int rate)
{
	DP a = pdisp[disp];
	if (a->sample_rate != rate)
	{
		EnterCriticalSection (&a->ResampleSection);
		a->sample_rate = rate;
		CalcBandwidthNormalization (a);
		LeaveCriticalSection (&a->ResampleSection);
	}
}

PORT
void SetDisplayNormOneHz (int disp, int pixout, int norm)
{
	DP a = pdisp[disp];
	if (a->normalize[pixout] != norm)
	{
		EnterCriticalSection (&a->ResampleSection);
		a->normalize[pixout] = norm;
		LeaveCriticalSection (&a->ResampleSection);
	}
}

PORT
double GetDisplayENB (int disp)
{
	DP a = pdisp[disp];
	double enb;
	EnterCriticalSection(&a->SetAnalyzerSection);
	enb = 1.0 / a->inv_enb;
	LeaveCriticalSection(&a->SetAnalyzerSection);
	return enb;
}
