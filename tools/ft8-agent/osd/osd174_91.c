/*
 * osd174_91 -- ordered-statistics decoder for the (174,91) FT8 code.
 *
 * C port of WSJT-X's lib/ft8/osd174_91.f90 by Steve Franke K9AN and
 * Joe Taylor K1JT.  See PROVENANCE.md for the upstream revision and what
 * changed in the port.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "osd174_91.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "ft8/constants.h"
#include "ft8/crc.h"

#define N FTX_LDPC_N /* 174 */
#define KMAX FTX_LDPC_K /* 91 */
#define M FTX_LDPC_M /* 83 */

/* ------------------------------------------------------------------ *
 * The generator matrix.
 *
 * The Fortran builds this by encoding 91 unit vectors, which would drag
 * the LDPC ENCODER into this tree.  ft8_lib deliberately removed
 * ft8/encode.c so that "no transmit code path" is a property of the
 * source rather than a claim about it (see ft8_lib/PROVENANCE.md), and
 * reintroducing it for a receive-only feature would undo that.
 *
 * It is not needed.  Encoding a unit vector e_i just selects row/column i
 * of the generator, and kFTX_LDPC_generator is still vendored in
 * constants.c -- it survived the encoder's removal and is referenced
 * nowhere else today.  So gen[i][j], the j-th bit of the codeword for
 * message e_i, is:
 *
 *   j <  91 : 1 when j == i          (systematic part: the identity)
 *   j >= 91 : bit i of parity row (j-91), read MSB-first from the
 *             bitpacked kFTX_LDPC_generator[j-91][]
 *
 * Verified against the corpus rather than by inspection: with OSD at
 * depth 0 (order-0 only, which is pure re-encoding through this matrix)
 * every codeword it returns must still pass CRC, and the corpus score
 * must not move.  A transposed or bit-reversed matrix fails that at once.
 * ------------------------------------------------------------------ */
static uint8_t g_gen[KMAX][N]; /* gen[i][j] as above, one bit per byte */
static int g_gen_ready = 0;

static void build_generator(void)
{
    memset(g_gen, 0, sizeof g_gen);
    for (int i = 0; i < KMAX; i++)
    {
        g_gen[i][i] = 1; /* systematic part */
        for (int r = 0; r < M; r++)
        {
            /* bit i of parity row r, MSB first across 12 bytes */
            uint8_t byte = kFTX_LDPC_generator[r][i / 8];
            uint8_t bit = (uint8_t)((byte >> (7 - (i % 8))) & 1u);
            g_gen[i][KMAX + r] = bit;
        }
    }
}

/* ------------------------------------------------------------------ *
 * Helpers ported from the Fortran.  All arrays are 0-indexed here; the
 * Fortran is 1-indexed, which is the classic way a port of this routine
 * goes subtly wrong, so the loop bounds below were rewritten rather than
 * transliterated.
 * ------------------------------------------------------------------ */

/* indexx: permutation that sorts v ASCENDING.  The Fortran uses a
 * heapsort from the shared library; any stable-enough sort does, since
 * only the ordering matters.  Insertion sort over 174 elements is a few
 * microseconds and keeps this file dependency-free. */
static void indexx(const float* v, int n, int* idx)
{
    for (int i = 0; i < n; i++)
        idx[i] = i;
    for (int i = 1; i < n; i++)
    {
        int cur = idx[i];
        float key = v[cur];
        int j = i - 1;
        while (j >= 0 && v[idx[j]] > key)
        {
            idx[j + 1] = idx[j];
            j--;
        }
        idx[j + 1] = cur;
    }
}

/* mrbencode91: codeword = XOR of the g2 columns selected by me.
 * g2 is the transpose of genmrb, so g2[j][i] == genmrb[i][j]; this takes
 * genmrb directly and indexes it the other way round, which avoids
 * materialising the transpose at all. */
static void mrbencode91(const uint8_t* me, uint8_t* codeword,
                        const uint8_t genmrb[KMAX][N], int k)
{
    memset(codeword, 0, N);
    for (int i = 0; i < k; i++)
    {
        if (me[i])
        {
            const uint8_t* row = genmrb[i];
            for (int j = 0; j < N; j++)
                codeword[j] ^= row[j];
        }
    }
}

/* nextpat91: next test error pattern of the same weight.
 * Returns the index of the lowest-index 1 in mi, or -1 when the patterns
 * of this weight are exhausted. */
static int nextpat91(uint8_t* mi, int k, int iorder)
{
    int ind = -1;
    for (int i = 0; i < k - 1; i++)
        if (mi[i] == 0 && mi[i + 1] == 1)
            ind = i;
    if (ind < 0)
        return -1;

    uint8_t ms[KMAX];
    memset(ms, 0, sizeof ms);
    for (int i = 0; i < ind; i++)
        ms[i] = mi[i];
    ms[ind] = 1;
    if (ind + 1 < k)
        ms[ind + 1] = 0;
    if (ind + 2 < k)
    {
        int sum = 0;
        for (int i = 0; i < k; i++)
            sum += ms[i];
        int nz = iorder - sum;
        for (int i = k - nz; i < k; i++)
            if (i >= 0)
                ms[i] = 1;
    }
    memcpy(mi, ms, (size_t)k);
    for (int i = 0; i < k; i++)
        if (mi[i] == 1)
            return i;
    return -1;
}

/* ------------------------------------------------------------------ *
 * The box tables used by preprocessing rule 2 (npre2), i.e. depth >= 3.
 *
 * The Fortran holds these in a COMMON block sized fp(0:525000), about
 * 2 MB of int.  That is a lot for a radio whose whole waveform package is
 * bounded at 512 KB, so they are allocated on demand and freed, and only
 * when the depth actually uses them.
 * ------------------------------------------------------------------ */
typedef struct
{
    int (*indexes)[2]; /* [nmax][2] */
    int* fp;           /* [1 << ntau] */
    int* np;           /* [nmax] */
    int nmax;
    int ntau;
    int lastpat;
    int inext;
} boxes_t;

static int boxes_alloc(boxes_t* b, int ntau, int nmax)
{
    b->ntau = ntau;
    b->nmax = nmax;
    size_t nfp = (size_t)1u << ntau;
    b->indexes = malloc(sizeof(int) * 2 * (size_t)nmax);
    b->fp = malloc(sizeof(int) * nfp);
    b->np = malloc(sizeof(int) * (size_t)nmax);
    if (!b->indexes || !b->fp || !b->np)
        return 0;
    memset(b->indexes, -1, sizeof(int) * 2 * (size_t)nmax);
    for (size_t i = 0; i < nfp; i++)
        b->fp[i] = -1;
    for (int i = 0; i < nmax; i++)
        b->np[i] = -1;
    b->lastpat = -1;
    b->inext = -1;
    return 1;
}

static void boxes_free(boxes_t* b)
{
    free(b->indexes);
    free(b->fp);
    free(b->np);
    b->indexes = NULL;
    b->fp = NULL;
    b->np = NULL;
}

static int pattern_index(const uint8_t* e2, int ntau)
{
    int ipat = 0;
    for (int i = 0; i < ntau; i++)
        if (e2[i])
            ipat += 1 << (ntau - 1 - i);
    return ipat;
}

static void boxit91(boxes_t* b, const uint8_t* e2, int npindex, int i1, int i2)
{
    if (npindex >= b->nmax)
        return;
    b->indexes[npindex][0] = i1;
    b->indexes[npindex][1] = i2;
    int ipat = pattern_index(e2, b->ntau);
    int ip = b->fp[ipat];
    if (ip == -1)
    {
        b->fp[ipat] = npindex;
    }
    else
    {
        while (b->np[ip] != -1)
            ip = b->np[ip];
        b->np[ip] = npindex;
    }
}

static void fetchit91(boxes_t* b, const uint8_t* e2, int* i1, int* i2)
{
    int ipat = pattern_index(e2, b->ntau);
    int index = b->fp[ipat];

    if (b->lastpat != ipat && index > 0)
    {
        *i1 = b->indexes[index][0];
        *i2 = b->indexes[index][1];
        b->inext = b->np[index];
    }
    else if (b->lastpat == ipat && b->inext > 0)
    {
        *i1 = b->indexes[b->inext][0];
        *i2 = b->indexes[b->inext][1];
        b->inext = b->np[b->inext];
    }
    else
    {
        *i1 = -1;
        *i2 = -1;
        b->inext = -1;
    }
    b->lastpat = ipat;
}

/* ------------------------------------------------------------------ */

int osd174_91_decode(const float llr[174], int k, osd_depth_t depth,
                     uint8_t out_msg91[91], int* out_nhard)
{
    if (k < 77 || k > 91)
        return 0;

    if (!g_gen_ready)
    {
        build_generator();
        g_gen_ready = 1;
    }

    float rx[N], absrx[N];
    uint8_t hdec[N];
    int indx[N], indices[N];

    for (int i = 0; i < N; i++)
    {
        rx[i] = llr[i];
        hdec[i] = (uint8_t)(rx[i] >= 0.0f ? 1 : 0);
        absrx[i] = fabsf(rx[i]);
    }

    /* Order the columns by DECREASING reliability.  indexx sorts
     * ascending, so walk it backwards -- the Fortran's indx(N+1-i). */
    indexx(absrx, N, indx);
    static uint8_t genmrb[KMAX][N];
    for (int i = 0; i < N; i++)
    {
        int src = indx[N - 1 - i];
        indices[i] = src;
        for (int r = 0; r < k; r++)
            genmrb[r][i] = g_gen[r][src];
    }

    /* Gaussian elimination so the k most reliable bits sit in 0..k-1.
     * The "+20" slack is the Fortran's, and is ad hoc there too. */
    for (int id = 0; id < k; id++)
    {
        int limit = k + 20 < N ? k + 20 : N;
        for (int icol = id; icol < limit; icol++)
        {
            if (genmrb[id][icol] == 1)
            {
                if (icol != id)
                {
                    for (int r = 0; r < k; r++)
                    {
                        uint8_t t = genmrb[r][id];
                        genmrb[r][id] = genmrb[r][icol];
                        genmrb[r][icol] = t;
                    }
                    int it = indices[id];
                    indices[id] = indices[icol];
                    indices[icol] = it;
                }
                for (int ii = 0; ii < k; ii++)
                {
                    if (ii != id && genmrb[ii][id] == 1)
                        for (int j = 0; j < N; j++)
                            genmrb[ii][j] ^= genmrb[id][j];
                }
                break;
            }
        }
    }

    /* Permute the received quantities into the same order. */
    uint8_t hdecp[N];
    float absrxp[N];
    for (int i = 0; i < N; i++)
    {
        hdecp[i] = hdec[indices[i]];
        absrxp[i] = absrx[indices[i]];
    }

    uint8_t m0[KMAX], cw[N], c0[N], ce[N];
    memcpy(m0, hdecp, (size_t)k);
    mrbencode91(m0, c0, genmrb, k);

    int nhardmin = 0;
    float dmin = 0.0f;
    for (int i = 0; i < N; i++)
    {
        int x = c0[i] ^ hdecp[i];
        nhardmin += x;
        dmin += (float)x * absrxp[i];
    }
    memcpy(cw, c0, N);

    int nord = 0, npre1 = 0, npre2 = 0, nt = 40, ntheta = 12, ntau = 0;
    switch (depth)
    {
    case OSD_DEPTH_OFF: nord = 0; break;
    case OSD_DEPTH_1: nord = 1; nt = 40; ntheta = 12; break;
    case OSD_DEPTH_2: nord = 1; npre1 = 1; nt = 40; ntheta = 10; break;
    case OSD_DEPTH_3: nord = 1; npre1 = 1; npre2 = 1; nt = 40; ntheta = 12; ntau = 14; break;
    case OSD_DEPTH_4: nord = 2; npre1 = 1; npre2 = 1; nt = 40; ntheta = 12; ntau = 17; break;
    case OSD_DEPTH_5: nord = 3; npre1 = 1; npre2 = 1; nt = 40; ntheta = 12; ntau = 15; break;
    case OSD_DEPTH_6: nord = 4; npre1 = 1; npre2 = 1; nt = 95; ntheta = 12; ntau = 15; break;
    default: nord = 0; break;
    }

    for (int iorder = 1; iorder <= nord; iorder++)
    {
        uint8_t misub[KMAX];
        memset(misub, 0, sizeof misub);
        for (int i = k - iorder; i < k; i++)
            misub[i] = 1;
        int iflag = k - iorder;

        while (iflag >= 0)
        {
            int iend = (iorder == nord && npre1 == 0) ? iflag : 0;
            float d1 = 0.0f;
            uint8_t e2sub[N], e2[N];
            memset(e2sub, 0, sizeof e2sub);

            for (int n1 = iflag; n1 >= iend; n1--)
            {
                uint8_t mi[KMAX], me[KMAX];
                memcpy(mi, misub, (size_t)k);
                mi[n1] = 1;

                for (int i = 0; i < k; i++)
                    me[i] = (uint8_t)(m0[i] ^ mi[i]);

                int nd1kpt;
                if (n1 == iflag)
                {
                    mrbencode91(me, ce, genmrb, k);
                    int s = 0;
                    for (int i = k; i < N; i++)
                    {
                        e2sub[i - k] = (uint8_t)(ce[i] ^ hdecp[i]);
                        e2[i - k] = e2sub[i - k];
                    }
                    for (int i = 0; i < nt && i < N - k; i++)
                        s += e2sub[i];
                    nd1kpt = s + 1;
                    d1 = 0.0f;
                    for (int i = 0; i < k; i++)
                        d1 += (float)(me[i] ^ hdecp[i]) * absrxp[i];
                }
                else
                {
                    int s = 0;
                    for (int i = 0; i < N - k; i++)
                        e2[i] = (uint8_t)(e2sub[i] ^ genmrb[n1][k + i]);
                    for (int i = 0; i < nt && i < N - k; i++)
                        s += e2[i];
                    nd1kpt = s + 2;
                }

                if (nd1kpt <= ntheta)
                {
                    mrbencode91(me, ce, genmrb, k);
                    float dd;
                    if (n1 == iflag)
                    {
                        dd = d1;
                        for (int i = 0; i < N - k; i++)
                            dd += (float)e2sub[i] * absrxp[k + i];
                    }
                    else
                    {
                        dd = d1 + (float)(ce[n1] ^ hdecp[n1]) * absrxp[n1];
                        for (int i = 0; i < N - k; i++)
                            dd += (float)e2[i] * absrxp[k + i];
                    }
                    if (dd < dmin)
                    {
                        dmin = dd;
                        memcpy(cw, ce, N);
                        int nh = 0;
                        for (int i = 0; i < N; i++)
                            nh += ce[i] ^ hdecp[i];
                        nhardmin = nh;
                    }
                }
            }
            iflag = nextpat91(misub, k, iorder);
        }
    }

    /* Preprocessing rule 2. */
    if (npre2 == 1 && ntau > 0)
    {
        boxes_t boxes;
        memset(&boxes, 0, sizeof boxes);
        int nmax = k * (k - 1) / 2 + 2;
        if (boxes_alloc(&boxes, ntau, nmax))
        {
            int ntotal = 0;
            uint8_t tmp[KMAX];
            for (int i1 = k - 1; i1 >= 0; i1--)
            {
                for (int i2 = i1 - 1; i2 >= 0; i2--)
                {
                    ntotal++;
                    for (int t = 0; t < ntau; t++)
                        tmp[t] = (uint8_t)(genmrb[i1][k + t] ^ genmrb[i2][k + t]);
                    boxit91(&boxes, tmp, ntotal, i1, i2);
                }
            }

            uint8_t misub[KMAX];
            memset(misub, 0, sizeof misub);
            for (int i = k - nord; i < k; i++)
                misub[i] = 1;
            int iflag = k - nord;
            boxes.lastpat = -1;

            while (iflag >= 0)
            {
                uint8_t me[KMAX], e2sub[N], r2pat[N];
                for (int i = 0; i < k; i++)
                    me[i] = (uint8_t)(m0[i] ^ misub[i]);
                mrbencode91(me, ce, genmrb, k);
                for (int i = k; i < N; i++)
                    e2sub[i - k] = (uint8_t)(ce[i] ^ hdecp[i]);

                for (int i2 = 0; i2 <= ntau; i2++)
                {
                    for (int i = 0; i < ntau; i++)
                        r2pat[i] = e2sub[i];
                    if (i2 > 0)
                        r2pat[i2 - 1] ^= 1u;

                    for (;;)
                    {
                        int in1 = -1, in2 = -1;
                        fetchit91(&boxes, r2pat, &in1, &in2);
                        if (in1 <= 0 || in2 <= 0)
                            break;
                        uint8_t mi[KMAX];
                        memcpy(mi, misub, (size_t)k);
                        mi[in1] = 1;
                        mi[in2] = 1;
                        int sum = 0;
                        for (int i = 0; i < k; i++)
                            sum += mi[i];
                        if (sum < nord + npre1 + npre2)
                            continue;
                        for (int i = 0; i < k; i++)
                            me[i] = (uint8_t)(m0[i] ^ mi[i]);
                        mrbencode91(me, ce, genmrb, k);
                        float dd = 0.0f;
                        int nh = 0;
                        for (int i = 0; i < N; i++)
                        {
                            int x = ce[i] ^ hdecp[i];
                            nh += x;
                            dd += (float)x * absrxp[i];
                        }
                        if (dd < dmin)
                        {
                            dmin = dd;
                            memcpy(cw, ce, N);
                            nhardmin = nh;
                        }
                    }
                }
                iflag = nextpat91(misub, k, nord);
            }
            boxes_free(&boxes);
        }
    }

    /* Undo the reliability permutation. */
    uint8_t cw_out[N];
    for (int i = 0; i < N; i++)
        cw_out[indices[i]] = cw[i];

    /* Check the CRC the way ft8_lib does: pack the 91 bits, mask the
     * zero-extension, compare the embedded CRC against a recomputation. */
    uint8_t a91[FTX_LDPC_K_BYTES];
    memset(a91, 0, sizeof a91);
    for (int i = 0; i < KMAX; i++)
        if (cw_out[i])
            a91[i / 8] |= (uint8_t)(0x80u >> (i % 8));

    uint16_t crc_extracted = ftx_extract_crc(a91);
    uint8_t chk[FTX_LDPC_K_BYTES];
    memcpy(chk, a91, sizeof chk);
    chk[9] &= 0xF8;
    chk[10] &= 0x00;
    uint16_t crc_calculated = ftx_compute_crc(chk, 96 - 14);

    int ok = (crc_extracted == crc_calculated);
    if (out_nhard)
        *out_nhard = ok ? nhardmin : -nhardmin;
    if (ok && out_msg91)
        memcpy(out_msg91, cw_out, KMAX);
    return ok;
}
