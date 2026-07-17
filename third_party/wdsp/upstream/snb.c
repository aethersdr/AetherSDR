/*  snb.c

This file is part of a program that implements a Software-Defined Radio.

Copyright (C) 2015, 2016 Warren Pratt, NR0V

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

#define MAXIMP			256

void calc_snba (SNBA d)
{
	if (d->inrate >= d->internalrate)
		d->isize = d->bsize / (d->inrate / d->internalrate);
	else
		d->isize = d->bsize * (d->internalrate / d->inrate);
	d->inbuff  = (double *) malloc0 (d->isize * sizeof (complex));
	d->outbuff = (double *) malloc0 (d->isize * sizeof (complex));
	if (d->inrate != d->internalrate) d->resamprun = 1;
	else                              d->resamprun = 0;
	d->inresamp  = create_resample (d->resamprun, d->bsize, d->in,      d->inbuff, d->inrate,       d->internalrate, 0.0, 0, 2.0);
	setFCLow_resample (d->inresamp, 250.0);
	d->outresamp = create_resample (d->resamprun, d->isize, d->outbuff, d->out,    d->internalrate, d->inrate,       0.0, 0, 2.0);
	setFCLow_resample (d->outresamp, 200.0);
	d->incr = d->xsize / d->ovrlp;
	if (d->incr > d->isize)  d->iasize = d->incr;
	else                     d->iasize = d->isize;
	d->iainidx = 0;
	d->iaoutidx = 0;
	d->inaccum = (double *) malloc0 (d->iasize * sizeof (double));
	d->nsamps = 0;
	if (d->incr > d->isize)
	{
		d->oasize = d->incr;
		d->oainidx = 0;
		d->oaoutidx = d->isize;
	}
	else
	{
		d->oasize = d->isize;
		d->oainidx = 0;
		d->oaoutidx = 0;
	}
	d->init_oaoutidx = d->oaoutidx;
	d->outaccum = (double *) malloc0 (d->oasize * sizeof (double));
}

SNBA create_snba (int run, double* in, double* out, int inrate, int internalrate, int bsize, int ovrlp, int xsize,
	int asize, int npasses, double k1, double k2, int b, int pre, int post, double pmultmin, double out_low_cut, double out_high_cut)
{
	SNBA d = (SNBA) malloc0 (sizeof (snba));
	d->run = run;
	d->in = in;
	d->out = out;
	d->inrate = inrate;
	d->internalrate = internalrate;
	d->bsize = bsize;
	d->ovrlp = ovrlp;
	d->xsize = xsize;
	d->exec.asize = asize;
	d->exec.npasses = npasses;
	d->sdet.k1 = k1;
	d->sdet.k2 = k2;
	d->sdet.b = b;
	d->sdet.pre = pre;
	d->sdet.post = post;
	d->scan.pmultmin = pmultmin;
	d->out_low_cut = out_low_cut;
	d->out_high_cut = out_high_cut;

	calc_snba (d);

	d->xbase    = (double *) malloc0 (2 * d->xsize * sizeof (double));
	d->xaux     = d->xbase + d->xsize;
	d->exec.a       = (double *) malloc0 (d->xsize * sizeof (double));
	d->exec.v       = (double *) malloc0 (d->xsize * sizeof (double));
	d->exec.detout  = (int    *) malloc0 (d->xsize * sizeof (int));
	d->exec.savex   = (double *) malloc0 (d->xsize * sizeof (double));
	d->exec.xHout   = (double *) malloc0 (d->xsize * sizeof (double));
	d->exec.unfixed = (int    *) malloc0 (d->xsize * sizeof (int));
	d->sdet.vp      = (double *) malloc0 (d->xsize * sizeof (double));
	d->sdet.vpwr    = (double *) malloc0 (d->xsize * sizeof (double));

	d->wrk.xHat_a1rows_max = d->xsize + d->exec.asize;
	d->wrk.xHat_a2cols_max = d->xsize + 2 * d->exec.asize;
	d->wrk.xHat_r          = (double *) malloc0 (d->xsize * sizeof(double));
	d->wrk.xHat_ATAI       = (double *) malloc0 (d->xsize * d->xsize * sizeof(double));
	d->wrk.xHat_A1         = (double *) malloc0 (d->wrk.xHat_a1rows_max * d->xsize * sizeof(double));
	d->wrk.xHat_A2         = (double *) malloc0 (d->wrk.xHat_a1rows_max * d->wrk.xHat_a2cols_max * sizeof(double));
	d->wrk.xHat_P1         = (double *) malloc0 (d->xsize * d->wrk.xHat_a2cols_max * sizeof(double));
	d->wrk.xHat_P2         = (double *) malloc0 (d->xsize * sizeof(double));
	d->wrk.trI_y           = (double *) malloc0 ((d->xsize - 1) * sizeof(double));
	d->wrk.trI_v           = (double *) malloc0 ((d->xsize - 1) * sizeof(double));
	d->wrk.dR_z            = (double *) malloc0 ((d->xsize - 2) * sizeof(double));
	d->wrk.asolve_r        = (double *) malloc0 ((d->exec.asize + 1) * sizeof(double));
	d->wrk.asolve_z        = (double *) malloc0 ((d->exec.asize + 1) * sizeof(double));

	return d;
}

void decalc_snba (SNBA d)
{
	destroy_resample (d->outresamp);
	destroy_resample (d->inresamp);
	_aligned_free (d->outbuff);
	_aligned_free (d->inbuff);
	_aligned_free (d->outaccum);
	_aligned_free (d->inaccum);
}

void destroy_snba (SNBA d)
{
	_aligned_free (d->wrk.xHat_r);
	_aligned_free (d->wrk.xHat_ATAI);
	_aligned_free (d->wrk.xHat_A1);
	_aligned_free (d->wrk.xHat_A2);
	_aligned_free (d->wrk.xHat_P1);
	_aligned_free (d->wrk.xHat_P2);
	_aligned_free (d->wrk.trI_y);
	_aligned_free (d->wrk.trI_v);
	_aligned_free (d->wrk.dR_z);
	_aligned_free (d->wrk.asolve_r);
	_aligned_free (d->wrk.asolve_z);

	_aligned_free (d->sdet.vpwr);
	_aligned_free (d->sdet.vp);
	_aligned_free (d->exec.unfixed);
	_aligned_free (d->exec.xHout);
	_aligned_free (d->exec.savex);
	_aligned_free (d->exec.detout);
	_aligned_free (d->exec.v);
	_aligned_free (d->exec.a);

	_aligned_free (d->xbase);

	decalc_snba (d);

	_aligned_free (d);
}

void flush_snba (SNBA d)
{
	d->iainidx = 0;
	d->iaoutidx = 0;
	d->nsamps = 0;
	d->oainidx = 0;
	d->oaoutidx = d->init_oaoutidx;

	memset (d->inaccum,      0, d->iasize * sizeof (double));
	memset (d->outaccum,     0, d->oasize * sizeof (double));
	memset (d->xaux,         0, d->xsize  * sizeof (double));
	memset (d->exec.a,       0, d->xsize  * sizeof (double));
	memset (d->exec.v,       0, d->xsize  * sizeof (double));
	memset (d->exec.detout,  0, d->xsize  * sizeof (int));
	memset (d->exec.savex,   0, d->xsize  * sizeof (double));
	memset (d->exec.xHout,   0, d->xsize  * sizeof (double));
	memset (d->exec.unfixed, 0, d->xsize  * sizeof (int));
	memset (d->sdet.vp,      0, d->xsize  * sizeof (double));
	memset (d->sdet.vpwr,    0, d->xsize  * sizeof (double));

	memset (d->inbuff,       0, d->isize  * sizeof (complex));
	memset (d->outbuff,      0, d->isize  * sizeof (complex));
	flush_resample (d->inresamp);
	flush_resample (d->outresamp);
}

void setBuffers_snba (SNBA a, double* in, double* out)
{
	decalc_snba (a);
	a->in = in;
	a->out = out;
	calc_snba (a);
}

void setSamplerate_snba (SNBA a, int rate)
{
	decalc_snba (a);
	a->inrate = rate;
	calc_snba (a);
}

void setSize_snba (SNBA a, int size)
{
	decalc_snba (a);
	a->bsize = size;
	calc_snba (a);
}

void ATAc0 (int n, int nr, double* A, double* r)
{
    int i, j;
	memset(r, 0, n * sizeof (double));
    for (i = 0; i < n; i++)
        for (j = 0; j < nr; j++)
            r[i] += A[j * n + i] * A[j * n + 0];
}

void multA1TA2(double* a1, double* a2, int m, int n, int q, double* c)
{
	int i, j, k;
    int p = q - m;
	memset (c, 0, m * n * sizeof (double));              
    for (i = 0; i < m; i++)
    {
        for (j = 0; j < n; j++)
        {
            if (j < p)
            {
                for (k = i; k <= min(i + p, j); k++)
                    c[i * n + j] += a1[k * m + i] * a2[k * n + j];
            }
            if (j >= n - p)
            {
                for (k = max(i, q - (n - j)); k <= i + p; k++)
                    c[i * n + j] += a1[k * m + i] * a2[k * n + j];
            }
        }
    }
}

void multXKE(double* a, double* xk, int m, int q, int p, double* vout)
{
    int i, k;
	memset (vout, 0, m * sizeof (double));
    for (i = 0; i < m; i++)
    {
        for (k = i; k < p; k++)
            vout[i] += a[i * q + k] * xk[k];
        for (k = q - p; k <= q - m + i; k++)
            vout[i] += a[i * q + k] * xk[k];
    }
}

void multAv(double* a, double* v, int m, int q, double* vout)
{
	int i, k;
	memset (vout, 0, m * sizeof (double));
    for (i = 0; i < m; i++)
    {
        for (k = 0; k < q; k++)
            vout[i] += a[i * q + k] * v[k];
    }
}

void xHat(int xusize, int asize, double* xk, double* a, double* xout,
	double* r, double* ATAI, double* A1, double* A2, double* P1, double* P2,
	double* trI_y, double* trI_v, double* dR_z)
{
    int i, j, k;
	int a1rows = xusize + asize;
	int a2cols = xusize + 2 * asize;
	memset (r,    0, xusize          * sizeof(double));		// work space
	memset (ATAI, 0, xusize * xusize * sizeof(double));		// work space
	memset (A1,   0, a1rows * xusize * sizeof(double));		// work space
	memset (A2,   0, a1rows * a2cols * sizeof(double));		// work space
	memset (P1,   0, xusize * a2cols * sizeof(double));		// work space
	memset (P2,   0, xusize          * sizeof(double));		// work space

    for (i = 0; i < xusize; i++)
    {
        A1[i * xusize + i] = 1.0;
        k = i + 1;
        for (j = k; j < k + asize; j++)
            A1[j * xusize + i] = - a[j - k];
    }

    for (i = 0; i < asize; i++)
        {
            for (k = asize - i - 1, j = 0; k < asize; k++, j++)
                A2[j * a2cols + i] = a[k];
        }
    for (i = asize + xusize; i < 2 * asize + xusize; i++)
        {
            A2[(i - asize) * a2cols + i] = - 1.0;
            for (j = i - asize + 1, k = 0; j < xusize + asize; j++, k++)
                A2[j * a2cols + i] = a[k];
        }

    ATAc0(xusize, xusize + asize, A1, r);
    trI(xusize, r, ATAI, trI_y, trI_v, dR_z);
    multA1TA2(A1, A2, xusize, 2 * asize + xusize, xusize + asize, P1);
    multXKE(P1, xk, xusize, xusize + 2 * asize, asize, P2);
    multAv(ATAI, P2, xusize, xusize, xout);
}

void invf(int xsize, int asize, double* a, double* x, double* v)
{
    int i, j;
	memset (v, 0, xsize * sizeof (double));
	for (i = asize; i < xsize - asize; i++)
	{
		for (j = 0; j < asize; j++)
			v[i] += a[j] * (x[i - 1 - j] + x[i + 1 + j]);
		v[i] = x[i] - 0.5 * v[i];
	}
	for (i = xsize - asize; i < xsize; i++)
	{
        for (j = 0; j < asize; j++)
            v[i] += a[j] * x[i - 1 - j];
        v[i] = x[i] - v[i];
    }
}

void det(SNBA d, int asize, double* v, int* detout)
{
    int i, j;
    double medpwr, t1, t2;
    int bstate, bcount, bsamp;
    for (i = asize, j = 0; i < d->xsize; i++, j++)
	{
        d->sdet.vpwr[i] = v[i] * v[i];
		d->sdet.vp[j] = d->sdet.vpwr[i];
	}
    median(d->xsize - asize, d->sdet.vp, &medpwr);
    t1 = d->sdet.k1 * medpwr;
    t2 = 0.0;
    for (i = asize; i < d->xsize; i++)
    {
        if (d->sdet.vpwr[i] <= t1)
            t2 += d->sdet.vpwr[i];
        else if (d->sdet.vpwr[i] <= 2.0 * t1)
			t2 += 2.0 * t1 - d->sdet.vpwr[i];
    }
    t2 *= d->sdet.k2 / (double)(d->xsize - asize);
    for (i = asize; i < d->xsize; i++)
    {
        if (d->sdet.vpwr[i] > t2)
            detout[i] = 1;
        else
            detout[i] = 0;
    }
    bstate = 0;
    bcount = 0;
    bsamp = 0;
    for (i = asize; i < d->xsize; i++)
    {
        switch (bstate)
        {
            case 0:
                if (detout[i] == 1) bstate = 1;
                break;
            case 1:
                if (detout[i] == 0)
                {
                    bstate = 2;
                    bsamp = i;
                    bcount = 1;
                }
                break;
            case 2:
                ++bcount;
                if (bcount > d->sdet.b)
                    if (detout[i] == 1)
                        bstate = 1;
                    else
                        bstate = 0;
                else if (detout[i] == 1)
                {
                    for (j = bsamp; j < bsamp + bcount - 1; j++)
                        detout[j] = 1;
                    bstate = 1;
                }
                break;
        }
    }
    for (i = asize; i < d->xsize; i++)
    {
        if (detout[i] == 1)
        {
            for (j = i - 1; j > i - 1 - d->sdet.pre; j--)
                if (j >= asize) detout[j] = 1;
        }
    }
    for (i = d->xsize - 1; i >= asize; i--)
    {
        if (detout[i] == 1)
        {
            for (j = i + 1; j < i + 1 + d->sdet.post; j++)
                if (j < d->xsize) detout[j] = 1;
        }
    }
}

int scanFrame(int xsize, int pval, double pmultmin, int* det, int* bimp, int* limp, 
            int* befimp, int* aftimp, int* p_opt, int* next)
{
    int inflag = 0;
    int i = 0, j = 0, k = 0;
    int nimp = 0;
	double td;
    int ti;
	double merit[MAXIMP] = { 0 };
	int nextlist[MAXIMP] = { 0 };
	memset (befimp, 0, MAXIMP * sizeof (int));
	memset (aftimp, 0, MAXIMP * sizeof (int));
    while (i < xsize && nimp < MAXIMP)
    {
        if (det[i] == 1 && inflag == 0)
        {
            inflag = 1;
            bimp[nimp] = i;
            limp[nimp] = 1;
            nimp++;
        }
        else if (det[i] == 1)
        {
            limp[nimp - 1]++;
        }
        else
        {
            inflag = 0;
            befimp[nimp]++;
            if (nimp > 0)
                aftimp[nimp - 1]++;
        }
        i++;
    }
    for (i = 0; i < nimp; i++)
    {
        if (befimp[i] < aftimp[i])
            p_opt[i] = befimp[i];
        else
            p_opt[i] = aftimp[i];
        if (p_opt[i] > pval)
            p_opt[i] = pval;
        if (p_opt[i] < (int)(pmultmin * limp[i]))
            p_opt[i] = -1;
    }
            
    for (i = 0; i < nimp; i++)
    {
        merit[i] = (double)p_opt[i] / (double)limp[i];
        nextlist[i] = i;
    }
    for (j = 0; j < nimp - 1; j++)
    {
        for (k = 0; k < nimp - j - 1; k++)
        {
            if (merit[k] < merit[k + 1])
            {
                td = merit[k];
                ti = nextlist[k];
                merit[k] = merit[k + 1];
                nextlist[k] = nextlist[k + 1];
                merit[k + 1] = td;
                nextlist[k + 1] = ti;
            }
        }
    }
    i = 1;
    if (nimp > 0)
        while (merit[i] == merit[0] && i < nimp) i++;
    for (j = 0; j < i - 1; j++)
    {
        for (k = 0; k < i - j - 1; k++)
        {
            if (limp[nextlist[k]] < limp[nextlist[k + 1]])
            {
                td = merit[k];
                ti = nextlist[k];
                merit[k] = merit[k + 1];
                nextlist[k] = nextlist[k + 1];
                merit[k + 1] = td;
                nextlist[k + 1] = ti;
            }
        }
    }
    *next = nextlist[0];
    return nimp;
}

void execFrame(SNBA d, double* x)
{
	int i, k;
    int pass;
    int nimp;
	int bimp[MAXIMP];
	int limp[MAXIMP];
	int befimp[MAXIMP];
	int aftimp[MAXIMP];
	int p_opt[MAXIMP];
    int next = 0;
    int p;
	memcpy (d->exec.savex, x, d->xsize * sizeof (double));                    
	asolve(d->xsize, d->exec.asize, x, d->exec.a, d->wrk.asolve_r, d->wrk.asolve_z);
    invf(d->xsize, d->exec.asize, d->exec.a, x, d->exec.v);
    det(d, d->exec.asize, d->exec.v, d->exec.detout);
    for (i = 0; i < d->xsize; i++)
    {
        if (d->exec.detout[i] != 0)
            x[i] = 0.0;
    }
    nimp = scanFrame(d->xsize, d->exec.asize, d->scan.pmultmin, d->exec.detout, bimp, limp, befimp, aftimp, p_opt, &next);
    for (pass = 0; pass < d->exec.npasses; pass++)
    {
		memcpy (d->exec.unfixed, d->exec.detout, d->xsize * sizeof (int));
        for (k = 0; k < nimp; k++)
        {
            if (k > 0)
                scanFrame(d->xsize, d->exec.asize, d->scan.pmultmin, d->exec.unfixed, bimp, limp, befimp, aftimp, p_opt, &next);

            if ((p = p_opt[next]) > 0)
            {      
                asolve(d->xsize, p, x, d->exec.a, d->wrk.asolve_r, d->wrk.asolve_z);
                xHat(limp[next], p, &x[bimp[next] - p], d->exec.a, d->exec.xHout,  
					d->wrk.xHat_r, d->wrk.xHat_ATAI, d->wrk.xHat_A1, d->wrk.xHat_A2, 
					d->wrk.xHat_P1, d->wrk.xHat_P2, d->wrk.trI_y, d->wrk.trI_v, d->wrk.dR_z);
				memcpy (&x[bimp[next]], d->exec.xHout, limp[next] * sizeof (double));
				memset (&d->exec.unfixed[bimp[next]], 0, limp[next] * sizeof (int));
            }
            else
            {
				memcpy (&x[bimp[next]], &d->exec.savex[bimp[next]], limp[next] * sizeof (double));
            }
        }
    }
}

void xsnba (SNBA d)
{
	if (d->run)
	{
		int i;
		xresample (d->inresamp);
		for (i = 0; i < 2 * d->isize; i += 2)
		{
			d->inaccum[d->iainidx] = d->inbuff[i];
			d->iainidx = (d->iainidx + 1) % d->iasize;
		}
		d->nsamps += d->isize;
		while (d->nsamps >= d->incr)
		{
			memcpy (&d->xaux[d->xsize - d->incr], &d->inaccum[d->iaoutidx], d->incr * sizeof (double));
			execFrame (d, d->xaux);
			d->iaoutidx = (d->iaoutidx + d->incr) % d->iasize;
			d->nsamps -= d->incr;
			memcpy (&d->outaccum[d->oainidx], d->xaux, d->incr * sizeof (double));
			d->oainidx = (d->oainidx + d->incr) % d->oasize;
			memmove (d->xbase, &d->xbase[d->incr], (2 * d->xsize - d->incr) * sizeof (double));
		}
		for (i = 0; i < d->isize; i++)
		{
			d->outbuff[2 * i + 0] = d->outaccum[d->oaoutidx];
			d->outbuff[2 * i + 1] = 0.0;
			d->oaoutidx = (d->oaoutidx + 1) % d->oasize;
		}
		xresample (d->outresamp);
	}
	else if (d->out != d->in)
		memcpy (d->out, d->in, d->bsize * sizeof (complex));
}

/********************************************************************************************************
*																										*
*											RXA Properties												*
*																										*
********************************************************************************************************/

PORT void SetRXASNBARun (int channel, int run)
{
	SNBA a = rxa[channel].snba.p;
	if (a->run != run)
	{
		RXAbpsnbaCheck (channel, rxa[channel].mode, rxa[channel].ndb.p->master_run);
		RXAbp1Check (channel, rxa[channel].amd.p->run, run, rxa[channel].emnr.p->run, 
			rxa[channel].anf.p->run, rxa[channel].anr.p->run);
		EnterCriticalSection (&ch[channel].csDSP);
		a->run = run;
		RXAbp1Set (channel);
		RXAbpsnbaSet (channel);
		LeaveCriticalSection (&ch[channel].csDSP);
	}
}

PORT void SetRXASNBAovrlp (int channel, int ovrlp)
{
	EnterCriticalSection (&ch[channel].csDSP);
	decalc_snba (rxa[channel].snba.p);
	rxa[channel].snba.p->ovrlp = ovrlp;
	calc_snba (rxa[channel].snba.p);
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT void SetRXASNBAasize (int channel, int size)
{
	EnterCriticalSection (&ch[channel].csDSP);
	rxa[channel].snba.p->exec.asize = size;
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT void SetRXASNBAnpasses (int channel, int npasses)
{
	EnterCriticalSection (&ch[channel].csDSP);
	rxa[channel].snba.p->exec.npasses = npasses;
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT void SetRXASNBAk1 (int channel, double k1)
{
	EnterCriticalSection (&ch[channel].csDSP);
	rxa[channel].snba.p->sdet.k1 = k1;
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT void SetRXASNBAk2 (int channel, double k2)
{
	EnterCriticalSection (&ch[channel].csDSP);
	rxa[channel].snba.p->sdet.k2 = k2;
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT void SetRXASNBAbridge (int channel, int bridge)
{
	EnterCriticalSection (&ch[channel].csDSP);
	rxa[channel].snba.p->sdet.b = bridge;
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT void SetRXASNBApresamps (int channel, int presamps)
{
	EnterCriticalSection (&ch[channel].csDSP);
	rxa[channel].snba.p->sdet.pre = presamps;
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT void SetRXASNBApostsamps (int channel, int postsamps)
{
	EnterCriticalSection (&ch[channel].csDSP);
	rxa[channel].snba.p->sdet.post = postsamps;
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT void SetRXASNBApmultmin (int channel, double pmultmin)
{
	EnterCriticalSection (&ch[channel].csDSP);
	rxa[channel].snba.p->scan.pmultmin = pmultmin;
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT void SetRXASNBAOutputBandwidth (int channel, double flow, double fhigh)
{
	SNBA a;
	RESAMPLE d;
	double f_low, f_high;
	EnterCriticalSection (&ch[channel].csDSP);
	a = rxa[channel].snba.p;
	d = a->outresamp;

	if (flow >= 0 && fhigh >= 0)
	{
		if (fhigh <  a->out_low_cut) fhigh =  a->out_low_cut;
		if (flow  > a->out_high_cut) flow  = a->out_high_cut;
		f_low  = max ( a->out_low_cut, flow);
		f_high = min (a->out_high_cut, fhigh);
	}
	else if (flow <= 0 && fhigh <= 0)
	{
		if (flow  >  -a->out_low_cut) flow  =  -a->out_low_cut;
		if (fhigh < -a->out_high_cut) fhigh = -a->out_high_cut;
		f_low  = max ( a->out_low_cut, -fhigh);
		f_high = min (a->out_high_cut, -flow);
	}
	else if (flow < 0 && fhigh > 0)
	{
		double absmax = max (-flow, fhigh);
		if (absmax <  a->out_low_cut) absmax =  a->out_low_cut;
		f_low = a->out_low_cut;
		f_high = min (a->out_high_cut, absmax);
	}
	else	// (f_low > 0 && f_high < 0) does not occur.
	{
		f_low = a->out_low_cut;
		f_high = a->out_high_cut;
	}

	setBandwidth_resample (d, f_low, f_high);
	LeaveCriticalSection (&ch[channel].csDSP);
}


/********************************************************************************************************
*																										*
*										BPSNBA Bandpass Filter											*
*																										*
********************************************************************************************************/

// This is a thin wrapper for a notched-bandpass filter (nbp).  The basic difference is that it provides
// for its input and output to happen at different points in the processing pipeline.  This means it must 
// include a buffer, 'buff'.  Its input and output are done via functions xbpshbain() and xbpshbaout().

void calc_bpsnba (BPSNBA a)
{
	a->buff = (double *) malloc0 (a->size * sizeof (complex));
	a->bpsnba = create_nbp (
		1,							// run, always runs (use bpsnba 'run')
		a->run_notches,				// run the notches
		0,							// position variable for nbp (not for bpsnba), always 0
		a->size,					// buffer size
		a->nc,						// number of filter coefficients
		a->mp,						// minimum phase flag
		a->buff,					// pointer to input buffer
		a->out,						// pointer to output buffer
		a->f_low,					// lower filter frequency
		a->f_high,					// upper filter frequency
		a->rate,					// sample rate
		a->wintype,					// wintype
		a->gain,					// gain
		a->autoincr,				// auto-increase notch width if below min
		a->maxpb,					// max number of passbands
		a->ptraddr);				// addr of database pointer
}

BPSNBA create_bpsnba (int run, int run_notches, int position, int size, int nc, int mp, double* in, double* out, int rate,  
	double abs_low_freq, double abs_high_freq, double f_low, double f_high, int wintype, double gain, int autoincr, 
	int maxpb, NOTCHDB* ptraddr)
{
	BPSNBA a = (BPSNBA) malloc0 (sizeof (bpsnba));
	a->run = run;
	a->run_notches = run_notches;
	a->position = position;
	a->size = size;
	a->nc = nc;
	a->mp = mp;
	a->in = in;
	a->out = out;
	a->rate = rate;
	a->abs_low_freq = abs_low_freq;
	a->abs_high_freq = abs_high_freq;
	a->f_low = f_low;
	a->f_high = f_high;
	a->wintype = wintype;
	a->gain = gain;
	a->autoincr = autoincr;
	a->maxpb = maxpb;
	a->ptraddr = ptraddr;
	calc_bpsnba (a);
	return a;
}

void decalc_bpsnba (BPSNBA a)
{
	destroy_nbp (a->bpsnba);
	_aligned_free (a->buff);
}

void destroy_bpsnba (BPSNBA a)
{
	decalc_bpsnba (a);
	_aligned_free (a);
}

void flush_bpsnba (BPSNBA a)
{
	memset (a->buff, 0, a->size * sizeof (complex));
	flush_nbp (a->bpsnba);
}

void setBuffers_bpsnba (BPSNBA a, double* in, double* out)
{
	decalc_bpsnba (a);
	a->in = in;
	a->out = out;
	calc_bpsnba (a);
}

void setSamplerate_bpsnba (BPSNBA a, int rate)
{
	decalc_bpsnba (a);
	a->rate = rate;
	calc_bpsnba (a);
}

void setSize_bpsnba (BPSNBA a, int size)
{
	decalc_bpsnba (a);
	a->size = size;
	calc_bpsnba (a);
}

void xbpsnbain (BPSNBA a, int position)
{
	if (a->run && a->position == position)
		memcpy (a->buff, a->in, a->size * sizeof (complex));
}

void xbpsnbaout (BPSNBA a, int position)
{
	if (a->run && a->position == position)
		xnbp (a->bpsnba, 0);
}

void recalc_bpsnba_filter (BPSNBA a, int update)
{
	// Call anytime one of the parameters listed below has been changed in
	// the BPSNBA struct.
	NBP b = a->bpsnba;
	b->fnfrun = a->run_notches;
	b->flow = a->f_low;
	b->fhigh = a->f_high;
	b->wintype = a->wintype;
	b->gain = a->gain;
	b->autoincr = a->autoincr;
	calc_nbp_impulse (b);
	setImpulse_fircore (b->p, b->impulse, update);
	_aligned_free (b->impulse);
}

/********************************************************************************************************
*																										*
*											RXA Properties												*
*																										*
********************************************************************************************************/

PORT
void RXABPSNBASetNC (int channel, int nc)
{
	BPSNBA a;
	EnterCriticalSection (&ch[channel].csDSP);
	a = rxa[channel].bpsnba.p;
	if (a->nc != nc)
	{
		a->nc = nc;
		a->bpsnba->nc = a->nc;
		setNc_nbp (a->bpsnba);
	}
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void RXABPSNBASetMP (int channel, int mp)
{
	BPSNBA a;
	a = rxa[channel].bpsnba.p;
	if (a->mp != mp)
	{
		a->mp = mp;
		a->bpsnba->mp = a->mp;
		setMp_nbp (a->bpsnba);
	}
}