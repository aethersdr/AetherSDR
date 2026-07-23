/*  fir.c

This file is part of a program that implements a Software-Defined Radio.

Copyright (C) 2013, 2016, 2022, 2025 Warren Pratt, NR0V

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

warren@pratt.one
*/

#define _CRT_SECURE_NO_WARNINGS
#include "comm.h"

double* fftcv_mults (int NM, double* c_impulse)
{
	double* mults        = (double *) malloc0 (NM * sizeof (complex));
	double* cfft_impulse = (double *) malloc0 (NM * sizeof (complex));
	fftw_plan ptmp = fftw_plan_dft_1d(NM, (fftw_complex *) cfft_impulse,
			(fftw_complex *) mults, FFTW_FORWARD, FFTW_PATIENT);
	memset (cfft_impulse, 0, NM * sizeof (complex));
	// store complex coefs right-justified in the buffer
	memcpy (&(cfft_impulse[NM - 2]), c_impulse, (NM / 2 + 1) * sizeof(complex));
	fftw_execute (ptmp);
	fftw_destroy_plan (ptmp);
	_aligned_free (cfft_impulse);
	return mults;
}

static double bessel_I0(double x)
{
	double sum = 1.0;
	double term = 1.0;
	double xhalf = x * 0.5;
	for (int k = 1; k <= 40; ++k)
	{
		term *= (xhalf / k);
		term *= (xhalf / k);
		sum += term;
		if (term < 1e-20 * sum) break;
	}
	return sum;
}

static double* get_fsamp_window(int N, int wintype)
{
	int i;
	double arg0, arg1;
	double* window = (double *) malloc0 (N * sizeof(double));
	switch (wintype)
	{
	case 0:
		arg0 = 2.0 * PI / ((double)N - 1.0);
		for (i = 0; i < N; i++)
		{
			arg1 = cos(arg0 * (double)i);
			window[i]  =   +0.21747
				+ arg1 *  (-0.45325
				+ arg1 *  (+0.28256
				+ arg1 *  (-0.04672)));
		}
		break;
	case 1:
		arg0 = 2.0 * PI / ((double)N - 1.0);
		for (i = 0; i < N; ++i)
		{
			arg1 = cos(arg0 * (double)i);
			window[i]  =   +6.3964424114390378e-02
				+ arg1 *  (-2.3993864599352804e-01
				+ arg1 *  (+3.5015956323820469e-01
				+ arg1 *  (-2.4774111897080783e-01
				+ arg1 *  (+8.5438256055858031e-02
				+ arg1 *  (-1.2320203369293225e-02
				+ arg1 *  (+4.3778825791773474e-04))))));
		}
		break;
	case 2:
	{
		const double beta = 8.0;
		const double inv_I0_beta = 1.0 / bessel_I0(beta);
		const double N1 = (double)(N - 1);
		for (int n = 0; n < N; n++)
		{
			double x = 2.0 * n / N1 - 1.0;
			double arg = beta * sqrt(1.0 - x * x);
			window[n] = bessel_I0(arg) * inv_I0_beta;
		}
	}
		break;
	default:
		for (i = 0; i < N; i++)
			window[i] = 1.0;
	}
	return window;
}

double* fir_fsamp_odd (int N, double* A, int rtype, double scale, int wintype)
{
	int i, j;
	int mid = (N - 1) / 2;
	double mag, phs;
	double* window;
	double *fcoef     = (double *) malloc0 (N * sizeof (complex));
	double *c_impulse = (double *) malloc0 (N * sizeof (complex));
	fftw_plan ptmp = fftw_plan_dft_1d(N, (fftw_complex *)fcoef, (fftw_complex *)c_impulse, FFTW_BACKWARD, FFTW_PATIENT);
	double local_scale = 1.0 / (double)N;
	for (i = 0; i <= mid; i++)
	{
		mag = A[i] * local_scale;
		phs = - (double)mid * TWOPI * (double)i / (double)N;
		fcoef[2 * i + 0] = mag * cos (phs);
		fcoef[2 * i + 1] = mag * sin (phs);
	}
	for (i = mid + 1, j = 0; i < N; i++, j++)
	{
		fcoef[2 * i + 0] = + fcoef[2 * (mid - j) + 0];
		fcoef[2 * i + 1] = - fcoef[2 * (mid - j) + 1];
	}
	fftw_execute (ptmp);
	fftw_destroy_plan (ptmp);
	_aligned_free (fcoef);
	window = get_fsamp_window(N, wintype);
	switch (rtype)
	{
	case 0:
		for (i = 0; i < N; i++)
			c_impulse[i] = scale * c_impulse[2 * i] * window[i];
		break;
	case 1:
		for (i = 0; i < N; i++)
		{
			c_impulse[2 * i + 0] *= scale * window[i];
			c_impulse[2 * i + 1] = 0.0;
		}
		break;
	}
	_aligned_free (window);
	return c_impulse;
}

double* fir_fsamp (int N, double* A, int rtype, double scale, int wintype)
{
	int n, i, j, k;
	double sum;
	double* window;
	double *c_impulse = (double *) malloc0 (N * sizeof (complex));

	if (N & 1)
	{
		int M = (N - 1) / 2;
		for (n = 0; n < M + 1; n++)
		{
			sum = 0.0;
			for (k = 1; k < M + 1; k++)
				sum += 2.0 * A[k] * cos(TWOPI * (n - M) * k / N);
			c_impulse[2 * n + 0] = (1.0 / N) * (A[0] + sum);
			c_impulse[2 * n + 1] = 0.0;
		}
		for (n = M + 1, j = 1; n < N; n++, j++)
		{
			c_impulse[2 * n + 0] = c_impulse[2 * (M - j) + 0];
			c_impulse[2 * n + 1] = 0.0;
		}
	}
	else
	{
		double M = (double)(N - 1) / 2.0;
		for (n = 0; n < N / 2; n++)
		{
			sum = 0.0;
			for (k = 1; k < N / 2; k++)
				sum += 2.0 * A[k] * cos(TWOPI * (n - M) * k / N);
			c_impulse[2 * n + 0] = (1.0 / N) * (A[0] + sum);
			c_impulse[2 * n + 1] = 0.0;
		}
		for (n = N / 2, j = 1; n < N; n++, j++)
		{
			c_impulse[2 * n + 0] = c_impulse[2 * (N / 2 - j) + 0];
			c_impulse[2 * n + 1] = 0.0;
		}
	}
	window = get_fsamp_window (N, wintype);
	switch (rtype)
	{
	case 0:
		for (i = 0; i < N; i++)
			c_impulse[i] = scale * c_impulse[2 * i] * window[i];
		break;
	case 1:
		for (i = 0; i < N; i++)
			{
				c_impulse[2 * i + 0] *= scale * window[i];
				c_impulse[2 * i + 1] = 0.0;
			}
		break;
	}
	_aligned_free (window);
	return c_impulse;
}

double* fir_bandpass (int N, double f_low, double f_high, double samplerate, int wintype, int rtype, double scale)
{
	// check for previous in the cache
	struct Params 
	{
		int N;
		int wintype;
		int rtype;
		double f_low;
		double f_high;
		double samplerate;
		double scale;
	};

	struct Params params;
	memset(&params, 0, sizeof (params));
	params.N = N;
	params.wintype = wintype;
	params.rtype = rtype;
	params.f_low = f_low;
	params.f_high = f_high;
	params.samplerate = samplerate;
	params.scale = scale;

	HASH_T h = fnv1a_hash(&params, sizeof(params));
	double* imp = get_impulse_cache_entry(FIR_CACHE, h, N);
	if (imp) return imp;
	//

	double *c_impulse = (double *) malloc0 (N * sizeof (complex));
	double ft = (f_high - f_low) / (2.0 * samplerate);
	double ft_rad = TWOPI * ft;
	double w_osc = PI * (f_high + f_low) / samplerate;
	int i, j;
	double m = 0.5 * (double)(N - 1);
	double delta = PI / m;
	double cosphi;
	double posi, posj;
	double sinc, window, coef;

	if (N & 1)
	{
		switch (rtype)
		{
		case 0:
			c_impulse[N >> 1] = scale * 2.0 * ft;
			break;
		case 1:
			c_impulse[N - 1] = scale * 2.0 * ft;
			c_impulse[  N  ] = 0.0;
			break;
		}
	}
	for (i = (N + 1) / 2, j = N / 2 - 1; i < N; i++, j--)
	{
		posi = (double)i - m;
		posj = (double)j - m;
		sinc = sin (ft_rad * posi) / (PI * posi);
		switch (wintype)
		{
		case 0:	// Blackman-Harris 4-term
			cosphi = cos (delta * i);
			window  =             + 0.21747
					+ cosphi *  ( - 0.45325
					+ cosphi *  ( + 0.28256
					+ cosphi *  ( - 0.04672 )));
			break;
		case 1:	// Blackman-Harris 7-term
		default:
			cosphi = cos (delta * i);
			window	=			  + 6.3964424114390378e-02
					+ cosphi *  ( - 2.3993864599352804e-01
					+ cosphi *  ( + 3.5015956323820469e-01
					+ cosphi *	( - 2.4774111897080783e-01
					+ cosphi *  ( + 8.5438256055858031e-02
					+ cosphi *	( - 1.2320203369293225e-02
					+ cosphi *	( + 4.3778825791773474e-04 ))))));
			break;
		}
		coef = scale * sinc * window;
		switch (rtype)
		{
		case 0:
			c_impulse[i] = + coef * cos (posi * w_osc);
			c_impulse[j] = + coef * cos (posj * w_osc);
			break;
		case 1:
			c_impulse[2 * i + 0] = + coef * cos (posi * w_osc);
			c_impulse[2 * i + 1] = - coef * sin (posi * w_osc);
			c_impulse[2 * j + 0] = + coef * cos (posj * w_osc);
			c_impulse[2 * j + 1] = - coef * sin (posj * w_osc);
			break;
		}
	}

	// store in cache
	add_impulse_to_cache(FIR_CACHE, h, N, c_impulse);

	return c_impulse;
}

double *fir_read (int N, const char *filename, int rtype, double scale)
	// N = number of real or complex coefficients (see rtype)
	// *filename = filename
	// rtype = 0:  real coefficients
	// rtype = 1:  complex coefficients
	// scale = a scale factor that will be applied to the returned coefficients;
	//		if this is not needed, set it to 1.0
	// NOTE:  The number of values in the file must NOT exceed those implied by N and rtype
{
	FILE *file;
	int i;
	double I, Q;
	double *c_impulse = (double *) malloc0 (N * sizeof (complex));
	if (file = fopen(filename, "r"))
	{
		int error = 0;
		for (i = 0; i < N; i++)
		{
			// read in the complex impulse response
			// NOTE:  IF the freq response is symmetrical about 0, the imag coeffs will all be zero.
			switch (rtype)
			{
			case 0:
				if (error == 0 && fscanf(file, "%le", &I) != 1) error = 1;
				if (error == 0) 
					c_impulse[i] = +scale * I;
				break;
			case 1:
				if (error == 0 && (fscanf(file, "%le", &I) != 1 || fscanf(file, "%le", &Q) != 1)) error = 1;
				if (error == 0)
				{
					c_impulse[2 * i + 0] = +scale * I;
					c_impulse[2 * i + 1] = -scale * Q;
				}
				break;
			}
		}
		fclose (file);
	}
	return c_impulse;
}

void analytic (int N, double* in, double* out)
{
	int i;
	double inv_N = 1.0 / (double)N;
	double two_inv_N = 2.0 * inv_N;
	double* x = (double *) malloc0 (N * sizeof (complex));
	fftw_plan pfor = fftw_plan_dft_1d (N, (fftw_complex *) in,
			(fftw_complex *) x, FFTW_FORWARD, FFTW_PATIENT);
	fftw_plan prev = fftw_plan_dft_1d (N, (fftw_complex *) x,
			(fftw_complex *) out, FFTW_BACKWARD, FFTW_PATIENT);
	fftw_execute (pfor);
	x[0] *= inv_N;
	x[1] *= inv_N;
	for (i = 1; i < N / 2; i++)
	{
		x[2 * i + 0] *= two_inv_N;
		x[2 * i + 1] *= two_inv_N;
	}
	x[N + 0] *= inv_N;
	x[N + 1] *= inv_N;
	memset (&x[N + 2], 0, (N - 2) * sizeof (double));
	fftw_execute (prev);
	fftw_destroy_plan (prev);
	fftw_destroy_plan (pfor);
	_aligned_free (x);
}

void mp_imp (int N, double* fir, double* mpfir, int pfactor, int polarity)
{
	// check for previous in the cache
	struct Params 
	{
		int N;
		int pfactor;
		int polarity;
	};

	struct Params params;
	memset(&params, 0, sizeof(params));
	params.N = N;
	params.pfactor = pfactor;
	params.polarity = polarity;

	HASH_T h = fnv1a_hash(&params, sizeof(params));

	size_t arr_len = N * sizeof(complex);
	HASH_T hf = fnv1a_hash((uint8_t*)fir, arr_len);
	h ^= hf + GOLDEN_RATIO + (h << 6) + (h >> 2);

	double* imp = get_impulse_cache_entry(MP_CACHE, h, N);
	if (imp)
	{
		memcpy(mpfir, imp, N * sizeof(complex));
		_aligned_free (imp);
		return;
	}
	//

	int i;
	int size = N * pfactor;
	double inv_PN = 1.0 / (double)size;
	double* firpad  = (double *) malloc0 (size * sizeof (complex));
	double* firfreq = (double *) malloc0 (size * sizeof (complex));
	double* mag     = (double *) malloc0 (size * sizeof (double));
	double* ana     = (double *) malloc0 (size * sizeof (complex));
	double* impulse = (double *) malloc0 (size * sizeof (complex));
	double* newfreq = (double *) malloc0 (size * sizeof (complex));
	memcpy (firpad, fir, N * sizeof (complex));
	fftw_plan pfor = fftw_plan_dft_1d (size, (fftw_complex *) firpad,
			(fftw_complex *) firfreq, FFTW_FORWARD, FFTW_PATIENT);
	fftw_plan prev = fftw_plan_dft_1d (size, (fftw_complex *) newfreq,
			(fftw_complex *) impulse, FFTW_BACKWARD, FFTW_PATIENT);
	// print_impulse("orig_imp.txt", N, fir, 1, 0);
	fftw_execute (pfor);
	for (i = 0; i < size; i++)
	{
		mag[i] = sqrt (firfreq[2 * i + 0] * firfreq[2 * i + 0] + firfreq[2 * i + 1] * firfreq[2 * i + 1]) * inv_PN;
		if (mag[i] > 0.0)
			ana[2 * i + 0] = log (mag[i]);
		else
			ana[2 * i + 0] = log (1.0e-300);
	}
	analytic (size, ana, ana);
	for (i = 0; i < size; i++)
	{
		newfreq[2 * i + 0] = + mag[i] * cos (ana[2 * i + 1]);
		if (polarity)
			newfreq[2 * i + 1] = + mag[i] * sin (ana[2 * i + 1]);
		else
			newfreq[2 * i + 1] = - mag[i] * sin (ana[2 * i + 1]);
	}
	fftw_execute (prev);
	if (polarity)
		memcpy (mpfir, &impulse[2 * (pfactor - 1) * N], N * sizeof (complex));
	else
		memcpy (mpfir, impulse, N * sizeof (complex));
	// print_impulse("min_imp.txt", N, mpfir, 1, 0);
	fftw_destroy_plan (prev);
	fftw_destroy_plan (pfor);
	_aligned_free (newfreq);
	_aligned_free (impulse);
	_aligned_free (ana);
	_aligned_free (mag);
	_aligned_free (firfreq);
	_aligned_free (firpad);

	// store in cache
	add_impulse_to_cache(MP_CACHE, h, N, mpfir);
}

// impulse response of a zero frequency filter comprising a cascade of two resonators, 
//    each followed by a detrending filter
double* zff_impulse(int nc, double scale)
{
	// nc = number of coefficients (power of two)
	int n_resdet = nc / 2 - 1;			// size of single zero-frequency resonator with detrender
	int n_dresdet = 2 * n_resdet - 1;	// size of two cascaded units; when we convolve these we get 2 * n - 1 length
	// allocate the single and make the values
	double* resdet = (double*)malloc0 (n_resdet * sizeof(double));
	for (int i = 1, j = 0, k = n_resdet - 1; i < nc / 4; i++, j++, k--)
		resdet[j] = resdet[k] = (double)(i * (i + 1) / 2);
	resdet[nc / 4 - 1] = (double)(nc / 4 * (nc / 4 + 1) / 2);
	// print_impulse ("resdet", n_resdet, resdet, 0, 0);
	// allocate the double and complex versions and make the values
	double* dresdet = (double*)malloc0 (n_dresdet * sizeof(double));
	double div = (double)((nc / 2 + 1) * (nc / 2 + 1));					// calculate divisor
	double* c_dresdet = (double*)malloc0 (nc * sizeof(complex));
	for (int n = 0; n < n_dresdet; n++)	// convolve to make the cascade
	{
		for (int k = 0; k < n_resdet; k++)
			if ((n - k) >= 0 && (n - k) < n_resdet)
				dresdet[n] += resdet[k] * resdet[n - k];
		dresdet[n] /= div;
		c_dresdet[2 * n + 0] = dresdet[n] * scale;
		c_dresdet[2 * n + 1] = 0.0;
	}
	// print_impulse("dresdet", n_dresdet, dresdet, 0, 0);
	// print_impulse("c_dresdet", nc, c_dresdet, 1, 0);
	_aligned_free(dresdet);
	_aligned_free(resdet);
	return c_dresdet;
}

typedef struct _minphase
{
    int N;
    int pfactor;
    int size;
    double inv_size;
    double* firpad;
    double* firfreq;
    double* mag;
    double* ana;
    double* anax;
    double* newfreq;
    double* impulse;
    fftw_plan p_fir;
    fftw_plan p_anafor;
    fftw_plan p_anainv;
    fftw_plan p_imp;
} minphase, *MINPHASE;

MINPHASE create_minphase (int N, int pfactor)
{
    MINPHASE a  = (MINPHASE) malloc0 (sizeof (minphase));
    a->N        = N;
    a->pfactor  = pfactor;
    a->size     = N * pfactor;
    a->inv_size = 1.0 / (double) a->size;

    a->firpad  = (double *) malloc0 (a->size * sizeof (complex));
    a->firfreq = (double *) malloc0 (a->size * sizeof (complex));
    a->mag     = (double *) malloc0 (a->size * sizeof (double));
    a->ana     = (double *) malloc0 (a->size * sizeof (complex));
    a->anax    = (double *) malloc0 (a->size * sizeof (complex));
    a->newfreq = (double *) malloc0 (a->size * sizeof (complex));
    a->impulse = (double *) malloc0 (a->size * sizeof (complex));

    a->p_fir    = fftw_plan_dft_1d (a->size, (fftw_complex *) a->firpad,  (fftw_complex *) a->firfreq, FFTW_FORWARD,  FFTW_PATIENT);
    a->p_anafor = fftw_plan_dft_1d (a->size, (fftw_complex *) a->ana,     (fftw_complex *) a->anax,    FFTW_FORWARD,  FFTW_PATIENT);
    a->p_anainv = fftw_plan_dft_1d (a->size, (fftw_complex *) a->anax,    (fftw_complex *) a->ana,     FFTW_BACKWARD, FFTW_PATIENT);
    a->p_imp    = fftw_plan_dft_1d (a->size, (fftw_complex *) a->newfreq, (fftw_complex *) a->impulse, FFTW_BACKWARD, FFTW_PATIENT);

    memset (a->firpad, 0, a->size * sizeof (complex));
    return a;
}

void destroy_minphase (MINPHASE a)
{
    fftw_destroy_plan (a->p_imp);
    fftw_destroy_plan (a->p_anainv);
    fftw_destroy_plan (a->p_anafor);
    fftw_destroy_plan (a->p_fir);
    _aligned_free (a->impulse);
    _aligned_free (a->newfreq);
    _aligned_free (a->anax);
    _aligned_free (a->ana);
    _aligned_free (a->mag);
    _aligned_free (a->firfreq);
    _aligned_free (a->firpad);
    _aligned_free (a);
}

void mp_imp_exec (MINPHASE a, double* fir, double* mpfir)
{
    int i, size = a->size, N = a->N;
    double inv_size = a->inv_size;
    double two_inv_size = 2.0 * inv_size;
    memcpy (a->firpad, fir, N * sizeof (complex));
    fftw_execute (a->p_fir);
    for (i = 0; i < size; i++)
    {
        double re = a->firfreq[2 * i + 0];
        double im = a->firfreq[2 * i + 1];
        double m  = sqrt (re * re + im * im) * inv_size;
        a->mag[i]         = m;
        a->ana[2 * i + 0] = (m > 0.0) ? log (m) : log (1.0e-300);
        a->ana[2 * i + 1] = 0.0;
    }
    fftw_execute (a->p_anafor);
    a->anax[0] *= inv_size;
    a->anax[1] *= inv_size;
    for (i = 1; i < size / 2; i++)
    {
        a->anax[2 * i + 0] *= two_inv_size;
        a->anax[2 * i + 1] *= two_inv_size;
    }
    a->anax[size + 0] *= inv_size;
    a->anax[size + 1] *= inv_size;
    memset (&a->anax[size + 2], 0, (size - 2) * sizeof (double));
    fftw_execute (a->p_anainv);
    for (i = 0; i < size; i++)
    {
        double ph = a->ana[2 * i + 1];
        a->newfreq[2 * i + 0] = + a->mag[i] * cos (ph);
        a->newfreq[2 * i + 1] = - a->mag[i] * sin (ph);
    }
    fftw_execute (a->p_imp);
    memcpy (mpfir,  a->impulse, N * sizeof (complex));
}

typedef struct _fsamp
{
    int N;
    int wintype;
    int nh;
    int even;
    double* cosr;
    double* sinr;
    double* window;
    double* X;
    double* impulse;
    fftw_plan p_c2r;
} fsamp, *FSAMP;

FSAMP create_fsamp (int N, int wintype)
{
    FSAMP a = (FSAMP) malloc0 (sizeof (fsamp));
    a->N       = N;
    a->wintype = wintype;
    a->nh      = N / 2 + 1;
    a->even    = !(N & 1);

    double d     = (double)(N - 1) / 2.0;
    double inv_N = 1.0 / (double) N;

    a->cosr   = (double *) malloc0 (a->nh * sizeof (double));
    a->sinr   = (double *) malloc0 (a->nh * sizeof (double));
    {
        int k;
        for (k = 0; k < a->nh; k++)
        {
            double phs = - TWOPI * d * (double) k / (double) N;
            a->cosr[k] = cos (phs) * inv_N;
            a->sinr[k] = sin (phs) * inv_N;
        }
    }
    a->window  = get_fsamp_window (N, wintype);
    a->X       = (double *) malloc0 (a->nh * sizeof (complex));
    a->impulse = (double *) malloc0 (N      * sizeof (double));
    a->p_c2r = fftw_plan_dft_c2r_1d (N, (fftw_complex *) a->X, a->impulse, FFTW_PATIENT);
    return a;
}

void destroy_fsamp (FSAMP a)
{
    fftw_destroy_plan (a->p_c2r);
    _aligned_free (a->impulse);
    _aligned_free (a->X);
    _aligned_free (a->window);
    _aligned_free (a->sinr);
    _aligned_free (a->cosr);
    _aligned_free (a);
}

void fsamp_exec (FSAMP a, double* A, double* coef, int rtype, double scale)
{
    int k, i, N = a->N, nh = a->nh;
    for (k = 0; k < nh; k++)
    {
        a->X[2 * k + 0] = A[k] * a->cosr[k];
        a->X[2 * k + 1] = A[k] * a->sinr[k];
    }
    if (a->even)
    {
        a->X[2 * (nh - 1) + 0] = 0.0;
        a->X[2 * (nh - 1) + 1] = 0.0;
    }
    fftw_execute (a->p_c2r);
    switch (rtype)
    {
    case 0:
        for (i = 0; i < N; i++)
            coef[i] = scale * a->impulse[i] * a->window[i];
        break;
    case 1:
        for (i = 0; i < N; i++)
        {
            coef[2 * i + 0] = scale * a->impulse[i] * a->window[i];
            coef[2 * i + 1] = 0.0;
        }
        break;
    }
}
