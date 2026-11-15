/*  lmath.c

This file is part of a program that implements a Software-Defined Radio.

Copyright (C) 2015, 2016, 2023 Warren Pratt, NR0V

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

void dR (int n, double* r, double* y, double* z)
{
	int i, j, k;
    double alpha, beta, gamma;
	memset (z, 0, (n - 1) * sizeof (double));	// work space
    y[0] = -r[1];
    alpha = -r[1];
    beta = 1.0;
    for (k = 0; k < n - 1; k++)
    {
        beta *= 1.0 - alpha * alpha;
        gamma = 0.0;
        for (i = k + 1, j = 0; i > 0; i--, j++)
            gamma += r[i] * y[j];
        alpha = - (r[k + 2] + gamma) / beta;
        for (i = 0, j = k; i <= k; i++, j--)
            z[i] = y[i] + alpha * y[j];
		memcpy (y, z, (k + 1) * sizeof (double));
        y[k + 1] = alpha;
    }
}

void trI (
    int n,
    double* r,
    double* B,
	double* y,
	double* v,
	double* dR_z
    )
{
    int i, j, ni, nj;
    double gamma, t, scale, b;
	memset (y, 0, (n - 1) * sizeof (double));	// work space
	memset (v, 0, (n - 1) * sizeof (double));	// work space
    scale = 1.0 / r[0];
    for (i = 0; i < n; i++)
        r[i] *= scale;
    dR(n - 1, r, y, dR_z);

    t = 0.0;
    for (i = 0; i < n - 1; i++)
        t += r[i + 1] * y[i];
    gamma = 1.0 / (1.0 + t);
    for (i = 0, j = n - 2; i < n - 1; i++, j--)
        v[i] = gamma * y[j];
    B[0] = gamma;
    for (i = 1, j = n - 2; i < n; i++, j--)
        B[i] = v[j];
    for (i = 1; i <= (n - 1) / 2; i++)
        for (j = i; j < n - i; j++)
            B[i * n + j] = B[(i - 1) * n + (j - 1)] + (v[n - j - 1] * v[n - i - 1] - v[i - 1] * v[j - 1]) / gamma;
    for (i = 0; i <= (n - 1)/2; i++)
        for (j = i; j < n - i; j++)
        {
            b = B[i * n + j] *= scale;
            B[j * n + i] = b;
            ni = n - i - 1;
            nj = n - j - 1;
            B[ni * n + nj] = b;
            B[nj * n + ni] = b;
        }
}

void asolve(int xsize, int asize, double* x, double* a, double* r, double* z)
{
    int i, j, k;
    double beta, alpha, t;
	memset(r, 0, (asize + 1) * sizeof(double));		// work space
	memset(z, 0, (asize + 1) * sizeof(double));		// work space
    for (i = 0; i <= asize; i++)
    {
		for (j = 0; j < xsize; j++)
			r[i] += x[j] * x[j - i];
    }
    z[0] = 1.0;
    beta = r[0];
    for (k = 0; k < asize; k++)
    {
        alpha = 0.0;
        for (j = 0; j <= k; j++)
            alpha -= z[j] * r[k + 1 - j];
        alpha /= beta;
        for (i = 0; i <= (k + 1) / 2; i++)
        {
            t = z[k + 1 - i] + alpha * z[i];
            z[i] = z[i] + alpha * z[k + 1 - i];
            z[k + 1 - i] = t;
        }
        beta *= 1.0 - alpha * alpha;
    }
    for (i = 0; i < asize; i++)
	{
        a[i] = - z[i + 1];
		if (a[i] != a[i]) a[i] = 0.0;
	}
}

void median (int n, double* a, double* med)
{
    int S0, S1, i, j, m, k;
    double x, t;
    S0 = 0;
    S1 = n - 1;
    k = n / 2;
    while (S1 > S0 + 1)
    {
        m = (S0 + S1) / 2;
        t = a[m];
        a[m] = a[S0 + 1];
        a[S0 + 1] = t;
        if (a[S0] > a[S1])
        {
            t = a[S0];
            a[S0] = a[S1];
            a[S1] = t;
        }
        if (a[S0 + 1] > a[S1])
        {
            t = a[S0 + 1];
            a[S0 + 1] = a[S1];
            a[S1] = t;
        }
        if (a[S0] > a[S0 + 1])
        {
            t = a[S0];
            a[S0] = a[S0 + 1];
            a[S0 + 1] = t;
        }
        i = S0 + 1;
        j = S1;
        x = a[S0 + 1];
		do i++; while (a[i] < x);
        do j--; while (a[j] > x);
        while (j >= i)
        {
            t = a[i];
            a[i] = a[j];
            a[j] = t;
			do i++; while (a[i] < x);
            do j--; while (a[j] > x);
        }
        a[S0 + 1] = a[j];
        a[j] = x;
        if (j >= k) S1 = j - 1;
        if (j <= k) S0 = i;
    }
    if (S1 == S0 + 1 && a[S1] < a[S0])
    {
        t = a[S0];
        a[S0] = a[S1];
        a[S1] = t;
    }
	*med = a[k];
}


BLDR create_builder(int points, int ints)
{
	// for the create function, 'points' and 'ints' are the MAXIMUM values that will be encountered
	BLDR a = (BLDR)malloc0 (sizeof(bldr));
	a->catxy = (double*)malloc0(2 * points * sizeof(double));
	a->sx    = (double*)malloc0(    points * sizeof(double));
	a->sy    = (double*)malloc0(    points * sizeof(double));
	a->h     = (double*)malloc0(    ints   * sizeof(double));
	a->p     = (int*)   malloc0(    ints   * sizeof(int));
	a->np    = (int*)   malloc0(    ints   * sizeof(int));
	a->taa   = (double*)malloc0(    ints   * sizeof(double));
	a->tab   = (double*)malloc0(    ints   * sizeof(double));
	a->tag   = (double*)malloc0(    ints   * sizeof(double));
	a->tad   = (double*)malloc0(    ints   * sizeof(double));
	a->tbb   = (double*)malloc0(    ints   * sizeof(double));
	a->tbg   = (double*)malloc0(    ints   * sizeof(double));
	a->tbd   = (double*)malloc0(    ints   * sizeof(double));
	a->tgg   = (double*)malloc0(    ints   * sizeof(double));
	a->tgd   = (double*)malloc0(    ints   * sizeof(double));
	a->tdd   = (double*)malloc0(    ints   * sizeof(double));
	int nsize = 3 * ints + 1;
	int intp1 = ints + 1;
	int intm1 = ints - 1;
	a->A     = (double*)malloc0(intp1 * intp1 * sizeof(double));
	a->B     = (double*)malloc0(intp1 * intp1 * sizeof(double));
	a->C     = (double*)malloc0(intm1 * intp1 * sizeof(double));
	a->D     = (double*)malloc0(intp1         * sizeof(double));
	a->E     = (double*)malloc0(intp1 * intp1 * sizeof(double));
	a->F     = (double*)malloc0(intm1 * intp1 * sizeof(double));
	a->G     = (double*)malloc0(intp1         * sizeof(double));
	a->MAT   = (double*)malloc0(nsize * nsize * sizeof(double));
	a->RHS   = (double*)malloc0(nsize         * sizeof(double));
	a->SLN   = (double*)malloc0(nsize         * sizeof(double));
	a->z     = (double*)malloc0(intp1         * sizeof(double));
	a->zp    = (double*)malloc0(intp1         * sizeof(double));
	a->wrk   = (double*)malloc0(nsize         * sizeof(double));
	a->ipiv  = (int*)   malloc0(nsize         * sizeof(int));
	return a;
}

void destroy_builder(BLDR a)
{
	_aligned_free(a->ipiv);
	_aligned_free(a->wrk);
	_aligned_free(a->catxy);
	_aligned_free(a->sx);
	_aligned_free(a->sy);
	_aligned_free(a->h);
	_aligned_free(a->p);
	_aligned_free(a->np);

	_aligned_free(a->taa);
	_aligned_free(a->tab);
	_aligned_free(a->tag);
	_aligned_free(a->tad);
	_aligned_free(a->tbb);
	_aligned_free(a->tbg);
	_aligned_free(a->tbd);
	_aligned_free(a->tgg);
	_aligned_free(a->tgd);
	_aligned_free(a->tdd);

	_aligned_free(a->A);
	_aligned_free(a->B);
	_aligned_free(a->C);
	_aligned_free(a->D);
	_aligned_free(a->E);
	_aligned_free(a->F);
	_aligned_free(a->G);

	_aligned_free(a->MAT);
	_aligned_free(a->RHS);
	_aligned_free(a->SLN);

	_aligned_free(a->z);
	_aligned_free(a->zp);
	
	_aligned_free(a);
}

void flush_builder(BLDR a, int points, int ints)
{
	memset(a->catxy, 0, 2 * points * sizeof(double));
	memset(a->sx,    0, points * sizeof(double));
	memset(a->sy,    0, points * sizeof(double));
	memset(a->h,     0, ints * sizeof(double));
	memset(a->p,     0, ints * sizeof(int));
	memset(a->np,    0, ints * sizeof(int));
	memset(a->taa,   0, ints * sizeof(double));
	memset(a->tab,   0, ints * sizeof(double));
	memset(a->tag,   0, ints * sizeof(double));
	memset(a->tad,   0, ints * sizeof(double));
	memset(a->tbb,   0, ints * sizeof(double));
	memset(a->tbg,   0, ints * sizeof(double));
	memset(a->tbd,   0, ints * sizeof(double));
	memset(a->tgg,   0, ints * sizeof(double));
	memset(a->tgd,   0, ints * sizeof(double));
	memset(a->tdd,   0, ints * sizeof(double));
	int nsize = 3 * ints + 1;
	int intp1 = ints + 1;
	int intm1 = ints - 1;
	memset(a->A,     0, intp1 * intp1 * sizeof(double));
	memset(a->B,     0, intp1 * intp1 * sizeof(double));
	memset(a->C,     0, intm1 * intp1 * sizeof(double));
	memset(a->D,     0, intp1         * sizeof(double));
	memset(a->E,     0, intp1 * intp1 * sizeof(double));
	memset(a->F,     0, intm1 * intp1 * sizeof(double));
	memset(a->G,     0, intp1         * sizeof(double));
	memset(a->MAT,   0, nsize * nsize * sizeof(double));
	memset(a->RHS,   0, nsize * sizeof(double));
	memset(a->SLN,   0, nsize * sizeof(double));
	memset(a->z,     0, intp1 * sizeof(double));
	memset(a->zp,    0, intp1 * sizeof(double));
	memset(a->wrk,   0, nsize * sizeof(double));
	memset(a->ipiv,  0, nsize * sizeof(int));
}

int fcompare(const void* a, const void* b)
{
	if (*(double*)a < *(double*)b)
		return -1;
	else if (*(double*)a == *(double*)b)
		return 0;
	else
		return 1;
}

void decomp(int n, double* a, int* piv, int* info, double* wrk)
{
	int i, j, k;
	int t_piv;
	double m_row, mt_row, m_col, mt_col;
	*info = 0;
	for (i = 0; i < n; i++)
	{
		piv[i] = i;
		m_row = 0.0;
		for (j = 0; j < n; j++)
		{
			mt_row = a[n * i + j];
			if (mt_row < 0.0)  mt_row = -mt_row;
			if (mt_row > m_row)  m_row = mt_row;
		}
		if (m_row == 0.0)
		{
			*info = i;
			goto cleanup;
		}
		wrk[i] = m_row;
	}
	for (k = 0; k < n - 1; k++)
	{
		j = k;
		m_col = a[n * piv[k] + k] / wrk[piv[k]];
		if (m_col < 0)  m_col = -m_col;
		for (i = k + 1; i < n; i++)
		{
			mt_col = a[n * piv[i] + k] / wrk[piv[k]];
			if (mt_col < 0.0)  mt_col = -mt_col;
			if (mt_col > m_col)
			{
				m_col = mt_col;
				j = i;
			}
		}
		if (m_col == 0)
		{
			*info = -k;
			goto cleanup;
		}
		t_piv = piv[k];
		piv[k] = piv[j];
		piv[j] = t_piv;
		for (i = k + 1; i < n; i++)
		{
			a[n * piv[i] + k] /= a[n * piv[k] + k];
			for (j = k + 1; j < n; j++)
				a[n * piv[i] + j] -= a[n * piv[i] + k] * a[n * piv[k] + j];
		}
	}
	if (a[n * n - 1] == 0.0)
		*info = -n;
cleanup:
	return;
}

void dsolve(int n, double* a, int* piv, double* b, double* x)
{
	int j, k;
	double sum;

	for (k = 0; k < n; k++)
	{
		sum = 0.0;
		for (j = 0; j < k; j++)
			sum += a[n * piv[k] + j] * x[j];
		x[k] = b[piv[k]] - sum;
	}

	for (k = n - 1; k >= 0; k--)
	{
		sum = 0.0;
		for (j = k + 1; j < n; j++)
			sum += a[n * piv[k] + j] * x[j];
		x[k] = (x[k] - sum) / a[n * piv[k] + k];
	}
}

void cull(int* n, int ints, double* x, double* t, double ptol)
{
	int k = 0;
	int i = *n;
	int ntopint;
	int npx;

	while (x[i - 1] > t[ints - 1])
		i--;
	ntopint = *n - i;
	npx = (int)(ntopint * (1.0 - ptol));
	i = *n;
	while ((k < npx) && (x[--i] > t[ints]))
		k++;
	*n -= k;
}

void xbuilder(BLDR a, int points, double* x, double* y, int ints, double* t, int* info, double* c, double ptol)
{
	double u, v, alpha, beta, gamma, delta;
	int nsize = 3 * ints + 1;
	int intp1 = ints + 1;
	int intm1 = ints - 1;
	int i, j, k, m;
	int dinfo;
	flush_builder(a, points, ints);
	for (i = 0; i < points; i++)
	{
		a->catxy[2 * i + 0] = x[i];
		a->catxy[2 * i + 1] = y[i];
	}
	qsort(a->catxy, points, 2 * sizeof(double), fcompare);
	for (i = 0; i < points; i++)
	{
		a->sx[i] = a->catxy[2 * i + 0];
		a->sy[i] = a->catxy[2 * i + 1];
	}
	cull(&points, ints, a->sx, t, ptol);
	if (points <= 0 || a->sx[points - 1] > t[ints])
	{
		*info = -1000;
		goto cleanup;
	}
	else *info = 0;

	for (j = 0; j < ints; j++)
		a->h[j] = t[j + 1] - t[j];
	a->p[0] = 0;
	j = 0;
	for (i = 0; i < points; i++)
	{
		if (a->sx[i] <= t[j + 1])
			a->np[j]++;
		else
		{
			a->p[++j] = i;
			while (a->sx[i] > t[j + 1])
				a->p[++j] = i;
			a->np[j] = 1;
		}
	}
	for (i = 0; i < ints; i++)
		for (j = a->p[i]; j < a->p[i] + a->np[i]; j++)
		{
			u = (a->sx[j] - t[i]) / a->h[i];
			v = u - 1.0;
			alpha = (2.0 * u + 1.0) * v * v;
			beta = u * u * (1.0 - 2.0 * v);
			gamma = a->h[i] * u * v * v;
			delta = a->h[i] * u * u * v;
			a->taa[i] += alpha * alpha;
			a->tab[i] += alpha * beta;
			a->tag[i] += alpha * gamma;
			a->tad[i] += alpha * delta;
			a->tbb[i] += beta * beta;
			a->tbg[i] += beta * gamma;
			a->tbd[i] += beta * delta;
			a->tgg[i] += gamma * gamma;
			a->tgd[i] += gamma * delta;
			a->tdd[i] += delta * delta;
			a->D[i + 0] += 2.0 * a->sy[j] * alpha;
			a->D[i + 1] += 2.0 * a->sy[j] * beta;
			a->G[i + 0] += 2.0 * a->sy[j] * gamma;
			a->G[i + 1] += 2.0 * a->sy[j] * delta;
		}
	for (i = 0; i < ints; i++)
	{
		a->A[(i + 0) * intp1 + (i + 0)] += 2.0 * a->taa[i];
		a->A[(i + 1) * intp1 + (i + 1)] = 2.0 * a->tbb[i];
		a->A[(i + 0) * intp1 + (i + 1)] = 2.0 * a->tab[i];
		a->A[(i + 1) * intp1 + (i + 0)] = 2.0 * a->tab[i];
		a->B[(i + 0) * intp1 + (i + 0)] += 2.0 * a->tag[i];
		a->B[(i + 1) * intp1 + (i + 1)] = 2.0 * a->tbd[i];
		a->B[(i + 0) * intp1 + (i + 1)] = 2.0 * a->tbg[i];
		a->B[(i + 1) * intp1 + (i + 0)] = 2.0 * a->tad[i];
		a->E[(i + 0) * intp1 + (i + 0)] += 2.0 * a->tgg[i];
		a->E[(i + 1) * intp1 + (i + 1)] = 2.0 * a->tdd[i];
		a->E[(i + 0) * intp1 + (i + 1)] = 2.0 * a->tgd[i];
		a->E[(i + 1) * intp1 + (i + 0)] = 2.0 * a->tgd[i];
	}
	for (i = 0; i < intm1; i++)
	{
		a->C[i * intp1 + (i + 0)] = +3.0 * a->h[i + 1] / a->h[i];
		a->C[i * intp1 + (i + 2)] = -3.0 * a->h[i] / a->h[i + 1];
		a->C[i * intp1 + (i + 1)] = -a->C[i * intp1 + (i + 0)] - a->C[i * intp1 + (i + 2)];
		a->F[i * intp1 + (i + 0)] = a->h[i + 1];
		a->F[i * intp1 + (i + 1)] = 2.0 * (a->h[i] + a->h[i + 1]);
		a->F[i * intp1 + (i + 2)] = a->h[i];
	}
	for (i = 0, k = 0; i < intp1; i++, k++)
	{
		for (j = 0, m = 0; j < intp1; j++, m++)
			a->MAT[k * nsize + m] = a->A[i * intp1 + j];
		for (j = 0, m = intp1; j < intp1; j++, m++)
			a->MAT[k * nsize + m] = a->B[j * intp1 + i];
		for (j = 0, m = 2 * intp1; j < intm1; j++, m++)
			a->MAT[k * nsize + m] = a->C[j * intp1 + i];
		a->RHS[k] = a->D[i];
	}
	for (i = 0, k = intp1; i < intp1; i++, k++)
	{
		for (j = 0, m = 0; j < intp1; j++, m++)
			a->MAT[k * nsize + m] = a->B[i * intp1 + j];
		for (j = 0, m = intp1; j < intp1; j++, m++)
			a->MAT[k * nsize + m] = a->E[i * intp1 + j];
		for (j = 0, m = 2 * intp1; j < intm1; j++, m++)
			a->MAT[k * nsize + m] = a->F[j * intp1 + i];
		a->RHS[k] = a->G[i];
	}
	for (i = 0, k = 2 * intp1; i < intm1; i++, k++)
	{
		for (j = 0, m = 0; j < intp1; j++, m++)
			a->MAT[k * nsize + m] = a->C[i * intp1 + j];
		for (j = 0, m = intp1; j < intp1; j++, m++)
			a->MAT[k * nsize + m] = a->F[i * intp1 + j];
		for (j = 0, m = 2 * intp1; j < intm1; j++, m++)
			a->MAT[k * nsize + m] = 0.0;
		a->RHS[k] = 0.0;
	}
	decomp(nsize, a->MAT, a->ipiv, &dinfo, a->wrk);
	dsolve(nsize, a->MAT, a->ipiv, a->RHS, a->SLN);
	if (dinfo != 0)
	{
		*info = dinfo;
		goto cleanup;
	}

	for (i = 0; i <= ints; i++)
	{
		a->z[i] = a->SLN[i];
		a->zp[i] = a->SLN[i + ints + 1];
	}
	for (i = 0; i < ints; i++)
	{
		c[4 * i + 0] = a->z[i];
		c[4 * i + 1] = a->zp[i];
		c[4 * i + 2] = -3.0 / (a->h[i] * a->h[i]) * (a->z[i] - a->z[i + 1]) - 1.0 / a->h[i] * (2.0 * a->zp[i] + a->zp[i + 1]);
		c[4 * i + 3] = 2.0 / (a->h[i] * a->h[i] * a->h[i]) * (a->z[i] - a->z[i + 1]) + 1.0 / (a->h[i] * a->h[i]) * (a->zp[i] + a->zp[i + 1]);
	}
cleanup:
	return;
}
