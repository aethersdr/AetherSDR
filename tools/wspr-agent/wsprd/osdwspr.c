/* The ordered-statistics decoder (OSD) pass of WSJT-X's wsprd, translated
 * from lib/wsprd/osdwspr.f90 and lib/indexx.f90 (WSJT-X 3.0.2; Steven
 * Franke K9AN and Joe Taylor K1JT, GPL-3.0-or-later). Part of AetherSDR,
 * GPL-3.0-or-later.
 *
 * wsprd calls it as osdwspr_() for symbols the Fano decoder gave up on,
 * when run with -o <depth>. The translation keeps the Fortran's 1-based
 * indexing, evaluation order and single-precision sums so that it returns
 * the same codeword, hard-error count and distance as the original; the
 * test in test_osd.sh compares the two on random received words. */
#include <math.h>
#include <string.h>

#define N 162
#define K 50
#define L 32

typedef signed char i1;

/* Numerical Recipes' indexx, as WSJT-X has it: indx[1..n] orders arr[1..n]
 * ascending. Its insertion sort runs down to 1 rather than to l. */
static void indexx(const float *arr, int n, int *indx)
{
    enum { M = 7, NSTACK = 50 };
    int istack[NSTACK + 1];
    int i, indxt, ir, itemp, j, jstack, k, l;
    float a;
    for (j = 1; j <= n; j++) indx[j] = j;
    jstack = 0;
    l = 1;
    ir = n;
    for (;;) {
        if (ir - l < M) {
            for (j = l + 1; j <= ir; j++) {
                indxt = indx[j];
                a = arr[indxt];
                for (i = j - 1; i >= 1; i--) {
                    if (arr[indx[i]] <= a) break;
                    indx[i + 1] = indx[i];
                }
                if (i < 1) i = 0;
                indx[i + 1] = indxt;
            }
            if (jstack == 0) return;
            ir = istack[jstack];
            l = istack[jstack - 1];
            jstack -= 2;
        } else {
            k = (l + ir) / 2;
            itemp = indx[k]; indx[k] = indx[l + 1]; indx[l + 1] = itemp;
            if (arr[indx[l + 1]] > arr[indx[ir]]) { itemp = indx[l + 1]; indx[l + 1] = indx[ir]; indx[ir] = itemp; }
            if (arr[indx[l]] > arr[indx[ir]]) { itemp = indx[l]; indx[l] = indx[ir]; indx[ir] = itemp; }
            if (arr[indx[l + 1]] > arr[indx[l]]) { itemp = indx[l + 1]; indx[l + 1] = indx[l]; indx[l] = itemp; }
            i = l + 1;
            j = ir;
            indxt = indx[l];
            a = arr[indxt];
            for (;;) {
                do i++; while (arr[indx[i]] < a);
                do j--; while (arr[indx[j]] > a);
                if (j < i) break;
                itemp = indx[i]; indx[i] = indx[j]; indx[j] = itemp;
            }
            indx[l] = indx[j];
            indx[j] = indxt;
            jstack += 2;
            if (jstack > NSTACK) return; /* the Fortran stops; n=162 never gets here */
            if (ir - i + 1 >= j - l) {
                istack[jstack] = ir;
                istack[jstack - 1] = i;
                ir = j - 1;
            } else {
                istack[jstack] = j - 1;
                istack[jstack - 1] = l;
                l = i;
            }
        }
    }
}

/* g2 is the transposed generator, g2[n][k] for n=1..N, k=1..K. */
static void mrbencode(const i1 *me, i1 *codeword, i1 g2[N + 1][K + 1])
{
    for (int n = 1; n <= N; n++) codeword[n] = 0;
    for (int i = 1; i <= K; i++)
        if (me[i] == 1)
            for (int n = 1; n <= N; n++) codeword[n] ^= g2[n][i];
}

static void nextpat(i1 *mi, int k, int iorder, int *iflag)
{
    i1 ms[K + 1];
    int ind = -1;
    for (int i = 1; i <= k - 1; i++)
        if (mi[i] == 0 && mi[i + 1] == 1) ind = i;
    if (ind < 0) {
        *iflag = ind;
        return;
    }
    memset(ms, 0, sizeof ms);
    for (int i = 1; i <= ind - 1; i++) ms[i] = mi[i];
    ms[ind] = 1;
    ms[ind + 1] = 0;
    if (ind + 1 < k) {
        int s = 0;
        for (int i = 1; i <= k; i++) s += ms[i];
        int nz = iorder - s;
        for (int i = k - nz + 1; i <= k; i++) ms[i] = 1;
    }
    for (int i = 1; i <= k; i++) mi[i] = ms[i];
    for (int i = 1; i <= k; i++)
        if (mi[i] == 1) {
            *iflag = i;
            break;
        }
}

/* The Fortran's COMMON /boxes/ and fetchit's SAVEd state. */
static int box_indexes[4000 + 1][3], box_fp[525000 + 1], box_np[4000 + 1];
static int fetch_lastpat, fetch_inext;

static int pattern(const i1 *e2, int ntau)
{
    int ipat = 0;
    for (int i = 1; i <= ntau; i++)
        if (e2[i] == 1) ipat += 1 << (ntau - i);
    return ipat;
}

static void boxit(int *reset, const i1 *e2, int ntau, int npindex, int i1v, int i2v)
{
    if (*reset) {
        for (int i = 0; i <= 525000; i++) box_fp[i] = -1;
        for (int i = 0; i <= 4000; i++) box_np[i] = -1, box_indexes[i][1] = box_indexes[i][2] = -1;
        *reset = 0;
    }
    box_indexes[npindex][1] = i1v;
    box_indexes[npindex][2] = i2v;
    int ipat = pattern(e2, ntau);
    int ip = box_fp[ipat];
    if (ip == -1) {
        box_fp[ipat] = npindex;
    } else {
        while (box_np[ip] != -1) ip = box_np[ip];
        box_np[ip] = npindex;
    }
}

static void fetchit(int *reset, const i1 *e2, int ntau, int *i1v, int *i2v)
{
    if (*reset) {
        fetch_lastpat = -1;
        *reset = 0;
    }
    int ipat = pattern(e2, ntau);
    int index = box_fp[ipat];
    if (fetch_lastpat != ipat && index > 0) {
        *i1v = box_indexes[index][1];
        *i2v = box_indexes[index][2];
        fetch_inext = box_np[index];
    } else if (fetch_lastpat == ipat && fetch_inext > 0) {
        *i1v = box_indexes[fetch_inext][1];
        *i2v = box_indexes[fetch_inext][2];
        fetch_inext = box_np[fetch_inext];
    } else {
        *i1v = -1;
        *i2v = -1;
        fetch_inext = -1;
    }
    fetch_lastpat = ipat;
}

void osdwspr_(float ss_[], unsigned char apmask_[], int *ndeep, unsigned char cw_[], int *nhardmin, float *dmin)
{
    static const i1 gg[64] = {1, 1, 0, 1, 0, 1, 0, 0, 1, 0, 0, 0, 1, 1, 0, 0, 1, 0, 1, 0, 0, 1, 0, 1, 1, 1, 0, 1, 1, 0, 0, 0,
                              0, 1, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1, 1, 1, 1, 0, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 1, 1, 1, 1, 1, 1};
    static int first = 1;
    static i1 gen[K + 1][N + 1];
    static i1 genmrb[K + 1][N + 1], g2[N + 1][K + 1];
    i1 temp[K + 1], m0[K + 1], me[K + 1], mi[K + 1], misub[K + 1];
    i1 e2sub[N - K + 1], e2[N - K + 1], ui[N - K + 1], r2pat[N - K + 1];
    i1 apmaskr[N + 1], cw[N + 1], ce[N + 1], c0[N + 1], hdec[N + 1], nxor[N + 1], t[N + 1];
    int indices[N + 1], indx[N + 1], ti[N + 1];
    float rx[N + 1], absrx[N + 1], tf[N + 1];
    int i, id, icol, ii, iflag, itmp;
    int nord = 0, npre1 = 0, npre2 = 0, nt = 0, ntheta = 0, ntau = 0, ntotal;

    if (first) { /* fill the generator matrix */
        memset(gen, 0, sizeof gen);
        for (i = 1; i <= 2 * L; i++) gen[1][i] = gg[i - 1];
        for (i = 2; i <= K; i++) /* gen(i,:) = cshift(gen(i-1,:), -2) */
            for (int j = 1; j <= N; j++) gen[i][j] = gen[i - 1][((j - 1 - 2 + N) % N) + 1];
        first = 0;
    }

    for (i = 1; i <= N; i++) {
        rx[i] = ss_[i - 1] / 127.0f;
        apmaskr[i] = (i1)apmask_[i - 1];
    }
    for (i = 1; i <= N; i++) hdec[i] = rx[i] >= 0 ? 1 : 0;
    for (i = 1; i <= N; i++) absrx[i] = fabsf(rx[i]);
    indexx(absrx, N, indx);

    /* Columns in order of decreasing reliability. */
    for (i = 1; i <= N; i++) {
        for (int k = 1; k <= K; k++) genmrb[k][i] = gen[k][indx[N + 1 - i]];
        indices[i] = indx[N + 1 - i];
    }

    /* Gaussian elimination: the most reliable bits in positions 1:K. */
    for (id = 1; id <= K; id++) {
        for (icol = id; icol <= K + 20; icol++) {
            if (genmrb[id][icol] == 1) {
                if (icol != id) {
                    for (int k = 1; k <= K; k++) temp[k] = genmrb[k][id];
                    for (int k = 1; k <= K; k++) genmrb[k][id] = genmrb[k][icol];
                    for (int k = 1; k <= K; k++) genmrb[k][icol] = temp[k];
                    itmp = indices[id];
                    indices[id] = indices[icol];
                    indices[icol] = itmp;
                }
                for (ii = 1; ii <= K; ii++)
                    if (ii != id && genmrb[ii][id] == 1)
                        for (int n = 1; n <= N; n++) genmrb[ii][n] ^= genmrb[id][n];
                break;
            }
        }
    }
    for (int n = 1; n <= N; n++)
        for (int k = 1; k <= K; k++) g2[n][k] = genmrb[k][n];

    /* Permute into MRB order (each right-hand side read before writing). */
    for (i = 1; i <= N; i++) t[i] = hdec[indices[i]];
    memcpy(hdec, t, sizeof hdec);
    for (i = 1; i <= K; i++) m0[i] = hdec[i];
    for (i = 1; i <= N; i++) tf[i] = absrx[indices[i]];
    memcpy(absrx, tf, sizeof absrx);
    for (i = 1; i <= N; i++) tf[i] = rx[indices[i]];
    memcpy(rx, tf, sizeof rx);
    for (i = 1; i <= N; i++) t[i] = apmaskr[indices[i]];
    memcpy(apmaskr, t, sizeof apmaskr);

    mrbencode(m0, c0, g2);
    int nh = 0;
    float dd, d1;
    *dmin = 0.0f;
    for (i = 1; i <= N; i++) {
        nxor[i] = c0[i] ^ hdec[i];
        nh += nxor[i];
    }
    for (i = 1; i <= N; i++) *dmin += nxor[i] * absrx[i];
    *nhardmin = nh;
    memcpy(cw, c0, sizeof cw);
    ntotal = 0;

    if (*ndeep <= 0) goto done;
    if (*ndeep > 5) *ndeep = 5;
    switch (*ndeep) {
    case 1: nord = 1; npre1 = 0; npre2 = 0; nt = 66; ntheta = 16; break;
    case 2: nord = 1; npre1 = 1; npre2 = 0; nt = 66; ntheta = 22; ntau = 16; break;
    case 3: nord = 1; npre1 = 1; npre2 = 1; nt = 66; ntheta = 22; ntau = 16; break;
    case 4: nord = 2; npre1 = 1; npre2 = 1; nt = 66; ntheta = 22; ntau = 16; break;
    case 5: nord = 3; npre1 = 1; npre2 = 1; nt = 66; ntheta = 22; ntau = 16; break;
    }

    d1 = 0.0f;
    for (int iorder = 1; iorder <= nord; iorder++) {
        for (i = 1; i <= K - iorder; i++) misub[i] = 0;
        for (i = K - iorder + 1; i <= K; i++) misub[i] = 1;
        iflag = K - iorder + 1;
        while (iflag >= 0) {
            int iend = (iorder == nord && npre1 == 0) ? iflag : 1;
            d1 = 0.0f;
            for (int n1 = iflag; n1 >= iend; n1--) {
                int nd1Kpt, skip = 0;
                memcpy(mi, misub, sizeof mi);
                mi[n1] = 1;
                for (i = 1; i <= K; i++)
                    if ((apmaskr[i] & mi[i]) == 1) { skip = 1; break; }
                if (skip) continue;
                ntotal++;
                for (i = 1; i <= K; i++) me[i] = m0[i] ^ mi[i];
                if (n1 == iflag) {
                    mrbencode(me, ce, g2);
                    nd1Kpt = 1;
                    for (i = 1; i <= N - K; i++) {
                        e2sub[i] = ce[K + i] ^ hdec[K + i];
                        e2[i] = e2sub[i];
                    }
                    for (i = 1; i <= nt; i++) nd1Kpt += e2sub[i];
                    d1 = 0.0f;
                    for (i = 1; i <= K; i++) d1 += (me[i] ^ hdec[i]) * absrx[i];
                } else {
                    nd1Kpt = 2;
                    for (i = 1; i <= N - K; i++) e2[i] = e2sub[i] ^ g2[K + i][n1];
                    for (i = 1; i <= nt; i++) nd1Kpt += e2[i];
                }
                if (nd1Kpt <= ntheta) {
                    mrbencode(me, ce, g2);
                    for (i = 1; i <= N; i++) nxor[i] = ce[i] ^ hdec[i];
                    float s = 0.0f;
                    if (n1 == iflag) {
                        for (i = 1; i <= N - K; i++) s += e2sub[i] * absrx[K + i];
                        dd = d1 + s;
                    } else {
                        for (i = 1; i <= N - K; i++) s += e2[i] * absrx[K + i];
                        dd = d1 + (ce[n1] ^ hdec[n1]) * absrx[n1] + s;
                    }
                    if (dd < *dmin) {
                        *dmin = dd;
                        memcpy(cw, ce, sizeof cw);
                        nh = 0;
                        for (i = 1; i <= N; i++) nh += nxor[i];
                        *nhardmin = nh;
                    }
                }
            }
            nextpat(misub, K, iorder, &iflag);
        }
    }

    if (npre2 == 1) {
        int reset = 1;
        ntotal = 0;
        for (int a = K; a >= 1; a--)
            for (int b = a - 1; b >= 1; b--) {
                ntotal++;
                for (i = 1; i <= ntau; i++) mi[i] = g2[K + i][a] ^ g2[K + i][b];
                boxit(&reset, mi, ntau, ntotal, a, b);
            }
        reset = 1;
        for (i = 1; i <= K - nord; i++) misub[i] = 0;
        for (i = K - nord + 1; i <= K; i++) misub[i] = 1;
        iflag = K - nord + 1;
        while (iflag >= 0) {
            for (i = 1; i <= K; i++) me[i] = m0[i] ^ misub[i];
            mrbencode(me, ce, g2);
            for (i = 1; i <= N - K; i++) e2sub[i] = ce[K + i] ^ hdec[K + i];
            for (int i2 = 0; i2 <= ntau; i2++) {
                memset(ui, 0, sizeof ui);
                if (i2 > 0) ui[i2] = 1;
                for (i = 1; i <= N - K; i++) r2pat[i] = e2sub[i] ^ ui[i];
                for (;;) { /* label 778 */
                    int in1, in2;
                    fetchit(&reset, r2pat, ntau, &in1, &in2);
                    if (!(in1 > 0 && in2 > 0)) break;
                    memcpy(mi, misub, sizeof mi);
                    mi[in1] = 1;
                    mi[in2] = 1;
                    int s = 0, masked = 0;
                    for (i = 1; i <= K; i++) {
                        s += mi[i];
                        if ((apmaskr[i] & mi[i]) == 1) masked = 1;
                    }
                    /* The Fortran's `cycle` here leaves the 778 loop and
                     * moves on to the next i2. */
                    if (s < nord + npre1 + npre2 || masked) break;
                    for (i = 1; i <= K; i++) me[i] = m0[i] ^ mi[i];
                    mrbencode(me, ce, g2);
                    float dsum = 0.0f;
                    for (i = 1; i <= N; i++) nxor[i] = ce[i] ^ hdec[i];
                    for (i = 1; i <= N; i++) dsum += nxor[i] * absrx[i];
                    dd = dsum;
                    if (dd < *dmin) {
                        *dmin = dd;
                        memcpy(cw, ce, sizeof cw);
                        nh = 0;
                        for (i = 1; i <= N; i++) nh += nxor[i];
                        *nhardmin = nh;
                    }
                }
            }
            nextpat(misub, K, nord, &iflag);
        }
    }

done:
    /* Back to as-received order: cw(indices) = cw. */
    for (i = 1; i <= N; i++) t[i] = cw[i];
    for (i = 1; i <= N; i++) cw[indices[i]] = t[i];
    for (i = 0; i < N; i++) cw_[i] = (unsigned char)cw[i + 1];
    (void)ti;
}
