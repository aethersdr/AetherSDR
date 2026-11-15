/*
  FILE...: mpdecode_core.c
  AUTHOR.: Matthew C. Valenti, Rohit Iyer Seshadri, David Rowe
  CREATED: Sep 2016

  Vendored from codec2 for rade_text LDPC encode/decode support.
  USE_ORIGINAL_PHI0 is defined at compile time to avoid phi0.h dependency.
*/

#define USE_ORIGINAL_PHI0 1

#include "mpdecode_core.h"

/* MSVC does not support C99 VLAs — use _alloca for stack arrays of runtime size */
#ifdef _MSC_VER
#  include <malloc.h>
#  define RADE_VLA(type, name, count) type *(name) = (type *)_alloca((size_t)(count) * sizeof(type))
#else
#  define RADE_VLA(type, name, count) type (name)[count]
#endif
#include "debug_alloc.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define QPSK_CONSTELLATION_SIZE 4
#define QPSK_BITS_PER_SYMBOL    2

static COMP S_matrix[] = {
    {1.0f, 0.0f}, {0.0f, 1.0f}, {0.0f, -1.0f}, {-1.0f, 0.0f}};

struct c_sub_node {
  uint16_t index;
  uint16_t socket;
  float    message;
};

struct c_node {
  int degree;
  struct c_sub_node *subs;
};

struct v_sub_node {
  uint16_t index;
  uint16_t socket;
  float    message;
  uint8_t  sign;
};

struct v_node {
  int   degree;
  float initial_value;
  struct v_sub_node *subs;
};

void encode(struct LDPC *ldpc, unsigned char ibits[], unsigned char pbits[]) {
  unsigned int p, i, tmp, par, prev = 0;
  int ind;
  uint16_t *H_rows = ldpc->H_rows;

  for (p = 0; p < (unsigned int)ldpc->NumberParityBits; p++) {
    par = 0;
    for (i = 0; i < (unsigned int)ldpc->max_row_weight; i++) {
      ind = H_rows[p + i * ldpc->NumberParityBits];
      if (ind) par = par + ibits[ind - 1];
    }
    tmp = par + prev;
    tmp &= 1;
    prev = tmp;
    pbits[p] = tmp;
  }
}

static float phi0(float x) {
  float z;
  if (x > 10)
    return 0;
  else if (x < 9.08e-5f)
    return 10;
  else if (x > 9)
    return 1.6881e-4f;
  else if (x > 8)
    return 4.5887e-4f;
  else if (x > 7)
    return 1.2473e-3f;
  else if (x > 6)
    return 3.3906e-3f;
  else if (x > 5)
    return 9.2168e-3f;
  else {
    z = expf(x);
    return logf((z + 1) / (z - 1));
  }
}

#define AJIAN -0.24904163195436f
#define TJIAN  2.50681740420944f

static float max_star0(float delta1, float delta2) {
  float diff = delta2 - delta1;
  if (diff > TJIAN)        return delta2;
  else if (diff < -TJIAN)  return delta1;
  else if (diff > 0)       return delta2 + AJIAN * (diff - TJIAN);
  else                     return delta1 - AJIAN * (diff + TJIAN);
}

void init_c_v_nodes(struct c_node *c_nodes, int shift, int NumberParityBits,
                    int max_row_weight, uint16_t *H_rows, int H1,
                    int CodeLength, struct v_node *v_nodes, int NumberRowsHcols,
                    uint16_t *H_cols, int max_col_weight, int dec_type,
                    float *input) {
  int i, j, k, count, cnt, c_index, v_index;

  if (shift == 0) {
    for (i = 0; i < NumberParityBits; i++) {
      count = 0;
      for (j = 0; j < max_row_weight; j++)
        if (H_rows[i + j * NumberParityBits] > 0) count++;
      c_nodes[i].degree = count;
      if (H1) {
        c_nodes[i].degree = (i == 0) ? count + 1 : count + 2;
      }
    }
  } else {
    cnt = 0;
    for (i = 0; i < (NumberParityBits / shift); i++) {
      for (k = 0; k < shift; k++) {
        count = 0;
        for (j = 0; j < max_row_weight; j++)
          if (H_rows[cnt + j * NumberParityBits] > 0) count++;
        c_nodes[cnt].degree = count;
        if ((i == 0) || (i == (NumberParityBits / shift) - 1))
          c_nodes[cnt].degree = count + 1;
        else
          c_nodes[cnt].degree = count + 2;
        cnt++;
      }
    }
  }

  if (H1) {
    if (shift == 0) {
      for (i = 0; i < NumberParityBits; i++) {
        c_nodes[i].subs = CALLOC(c_nodes[i].degree, sizeof(struct c_sub_node));
        assert(c_nodes[i].subs);
        for (j = 0; j < c_nodes[i].degree - 2; j++)
          c_nodes[i].subs[j].index = (H_rows[i + j * NumberParityBits] - 1);
        j = c_nodes[i].degree - 2;
        if (i == 0)
          c_nodes[i].subs[j].index = (H_rows[i + j * NumberParityBits] - 1);
        else
          c_nodes[i].subs[j].index = (CodeLength - NumberParityBits) + i - 1;
        j = c_nodes[i].degree - 1;
        c_nodes[i].subs[j].index = (CodeLength - NumberParityBits) + i;
      }
    }
    if (shift > 0) {
      cnt = 0;
      for (i = 0; i < (NumberParityBits / shift); i++) {
        for (k = 0; k < shift; k++) {
          c_nodes[cnt].subs = CALLOC(c_nodes[cnt].degree, sizeof(struct c_sub_node));
          assert(c_nodes[cnt].subs);
          for (j = 0; j < c_nodes[cnt].degree - 2; j++)
            c_nodes[cnt].subs[j].index = (H_rows[cnt + j * NumberParityBits] - 1);
          j = c_nodes[cnt].degree - 2;
          if ((i == 0) || (i == (NumberParityBits / shift - 1)))
            c_nodes[cnt].subs[j].index = (H_rows[cnt + j * NumberParityBits] - 1);
          else
            c_nodes[cnt].subs[j].index = (CodeLength - NumberParityBits) + k + shift * i;
          j = c_nodes[cnt].degree - 1;
          c_nodes[cnt].subs[j].index = (CodeLength - NumberParityBits) + k + shift * (i + 1);
          if (i == (NumberParityBits / shift - 1))
            c_nodes[cnt].subs[j].index = (CodeLength - NumberParityBits) + k + shift * i;
          cnt++;
        }
      }
    }
  } else {
    for (i = 0; i < NumberParityBits; i++) {
      c_nodes[i].subs = CALLOC(c_nodes[i].degree, sizeof(struct c_sub_node));
      assert(c_nodes[i].subs);
      for (j = 0; j < c_nodes[i].degree; j++)
        c_nodes[i].subs[j].index = (H_rows[i + j * NumberParityBits] - 1);
    }
  }

  for (i = 0; i < (CodeLength - NumberParityBits + shift); i++) {
    count = 0;
    for (j = 0; j < max_col_weight; j++)
      if (H_cols[i + j * NumberRowsHcols] > 0) count++;
    v_nodes[i].degree = count;
  }

  for (i = CodeLength - NumberParityBits + shift; i < CodeLength; i++) {
    if (H1) {
      v_nodes[i].degree = (i != CodeLength - 1) ? 2 : 1;
    } else {
      count = 0;
      for (j = 0; j < max_col_weight; j++)
        if (H_cols[i + j * NumberRowsHcols] > 0) count++;
      v_nodes[i].degree = count;
    }
  }

  if (shift > 0)
    v_nodes[CodeLength - 1].degree = v_nodes[CodeLength - 1].degree + 1;

  for (i = 0; i < CodeLength; i++) {
    v_nodes[i].subs = CALLOC(v_nodes[i].degree, sizeof(struct v_sub_node));
    assert(v_nodes[i].subs);
    v_nodes[i].initial_value = input[i];
    count = 0;
    for (j = 0; j < v_nodes[i].degree; j++) {
      if ((H1) && (i >= CodeLength - NumberParityBits + shift)) {
        v_nodes[i].subs[j].index = i - (CodeLength - NumberParityBits + shift) + count;
        count += (shift == 0) ? 1 : shift;
      } else {
        v_nodes[i].subs[j].index = (H_cols[i + j * NumberRowsHcols] - 1);
      }
      for (c_index = 0; c_index < c_nodes[v_nodes[i].subs[j].index].degree; c_index++)
        if (c_nodes[v_nodes[i].subs[j].index].subs[c_index].index == i) {
          v_nodes[i].subs[j].socket = c_index;
          break;
        }
      if (dec_type == 1)
        v_nodes[i].subs[j].message = fabsf(input[i]);
      else
        v_nodes[i].subs[j].message = phi0(fabsf(input[i]));
      if (input[i] < 0) v_nodes[i].subs[j].sign = 1;
    }
  }

  for (i = 0; i < NumberParityBits; i++) {
    for (j = 0; j < c_nodes[i].degree; j++) {
      for (v_index = 0; v_index < v_nodes[c_nodes[i].subs[j].index].degree; v_index++)
        if (v_nodes[c_nodes[i].subs[j].index].subs[v_index].index == i) {
          c_nodes[i].subs[j].socket = v_index;
          break;
        }
    }
  }
}

int SumProduct(int *parityCheckCount, char DecodedBits[],
               struct c_node c_nodes[], struct v_node v_nodes[], int CodeLength,
               int NumberParityBits, int max_iter, float r_scale_factor,
               float q_scale_factor, int data[]) {
  int result = max_iter;
  int bitErrors, i, j, iter;
  float phi_sum, temp_sum, Qi;
  int sign, ssum;

  (void)r_scale_factor;
  (void)q_scale_factor;
  (void)data;

  for (iter = 0; iter < max_iter; iter++) {
    for (i = 0; i < CodeLength; i++) DecodedBits[i] = 0;
    bitErrors = 0;
    ssum = 0;

    for (j = 0; j < NumberParityBits; j++) {
      sign = v_nodes[c_nodes[j].subs[0].index].subs[c_nodes[j].subs[0].socket].sign;
      phi_sum = v_nodes[c_nodes[j].subs[0].index].subs[c_nodes[j].subs[0].socket].message;
      for (i = 1; i < c_nodes[j].degree; i++) {
        struct c_sub_node *cp = &c_nodes[j].subs[i];
        struct v_sub_node *vp = &v_nodes[cp->index].subs[cp->socket];
        phi_sum += vp->message;
        sign ^= vp->sign;
      }
      if (sign == 0) ssum++;
      for (i = 0; i < c_nodes[j].degree; i++) {
        struct c_sub_node *cp = &c_nodes[j].subs[i];
        struct v_sub_node *vp = &v_nodes[cp->index].subs[cp->socket];
        if (sign ^ vp->sign)
          cp->message = -phi0(phi_sum - vp->message);
        else
          cp->message =  phi0(phi_sum - vp->message);
      }
    }

    for (i = 0; i < CodeLength; i++) {
      Qi = v_nodes[i].initial_value;
      for (j = 0; j < v_nodes[i].degree; j++) {
        struct v_sub_node *vp = &v_nodes[i].subs[j];
        Qi += c_nodes[vp->index].subs[vp->socket].message;
      }
      if (Qi < 0) DecodedBits[i] = 1;
      for (j = 0; j < v_nodes[i].degree; j++) {
        struct v_sub_node *vp = &v_nodes[i].subs[j];
        temp_sum = Qi - c_nodes[vp->index].subs[vp->socket].message;
        vp->message = phi0(fabsf(temp_sum));
        vp->sign    = (temp_sum > 0) ? 0 : 1;
      }
    }

    for (i = 0; i < CodeLength - NumberParityBits; i++)
      if (DecodedBits[i] != 0) bitErrors++;

    *parityCheckCount = ssum;
    if (bitErrors == 0 || ssum == NumberParityBits) {
      result = iter + 1;
      break;
    }
  }
  return result;
}

int run_ldpc_decoder(struct LDPC *ldpc, uint8_t out_char[], float input[],
                     int *parityCheckCount) {
  int i;
  int CodeLength        = ldpc->CodeLength;
  int NumberParityBits  = ldpc->NumberParityBits;
  int NumberRowsHcols   = ldpc->NumberRowsHcols;
  int max_row_weight    = ldpc->max_row_weight;
  int max_col_weight    = ldpc->max_col_weight;

  int shift = (NumberParityBits + NumberRowsHcols) - CodeLength;
  int H1    = (NumberRowsHcols == CodeLength) ? 0 : 1;
  if (!H1) shift = 0;

  char *DecodedBits = CALLOC(CodeLength, sizeof(char));
  assert(DecodedBits);
  struct c_node *c_nodes = CALLOC(NumberParityBits, sizeof(struct c_node));
  assert(c_nodes);
  struct v_node *v_nodes = CALLOC(CodeLength, sizeof(struct v_node));
  assert(v_nodes);

  init_c_v_nodes(c_nodes, shift, NumberParityBits, max_row_weight, ldpc->H_rows,
                 H1, CodeLength, v_nodes, NumberRowsHcols, ldpc->H_cols,
                 max_col_weight, ldpc->dec_type, input);

  int DataLength  = CodeLength - NumberParityBits;
  int *data_int   = CALLOC(DataLength, sizeof(int));

  for (i = 0; i < CodeLength; i++) DecodedBits[i] = 0;

  int iter = SumProduct(parityCheckCount, DecodedBits, c_nodes, v_nodes,
                        CodeLength, NumberParityBits, ldpc->max_iter,
                        ldpc->r_scale_factor, ldpc->q_scale_factor, data_int);

  for (i = 0; i < CodeLength; i++) out_char[i] = DecodedBits[i];

  FREE(DecodedBits);
  FREE(data_int);
  for (i = 0; i < NumberParityBits; i++) FREE(c_nodes[i].subs);
  FREE(c_nodes);
  for (i = 0; i < CodeLength; i++) FREE(v_nodes[i].subs);
  FREE(v_nodes);

  return iter;
}

void sd_to_llr(float llr[], float sd[], int n) {
  double sum, mean, sign, sumsq, estvar, estEsN0, x;
  int i;
  sum = 0.0;
  for (i = 0; i < n; i++) sum += fabs(sd[i]);
  mean = sum / n;
  sum = sumsq = 0.0;
  for (i = 0; i < n; i++) {
    sign = (sd[i] > 0.0) - (sd[i] < 0.0);
    x = ((double)sd[i] / mean - sign);
    sum += x;
    sumsq += x * x;
  }
  estvar  = (n * sumsq - sum * sum) / (n * (n - 1));
  estEsN0 = 1.0 / (2.0 * estvar + 1E-3);
  for (i = 0; i < n; i++) llr[i] = (float)(4.0 * estEsN0 * sd[i]);
}

void Demod2D(float symbol_likelihood[], COMP r[], COMP S_matrix_arg[],
             float EsNo, float fading[], float mean_amp, int number_symbols) {
  int M = QPSK_CONSTELLATION_SIZE;
  int i, j;
  float tempsr, tempsi, Er, Ei;
  for (i = 0; i < number_symbols; i++) {
    for (j = 0; j < M; j++) {
      tempsr = fading[i] * S_matrix_arg[j].real / mean_amp;
      tempsi = fading[i] * S_matrix_arg[j].imag / mean_amp;
      Er = r[i].real / mean_amp - tempsr;
      Ei = r[i].imag / mean_amp - tempsi;
      symbol_likelihood[i * M + j] = -EsNo * (Er * Er + Ei * Ei);
    }
  }
}

void Somap(float bit_likelihood[], float symbol_likelihood[], int M, int bps,
           int number_symbols) {
  int n, i, j, k, mask;
  RADE_VLA(float, num, bps); RADE_VLA(float, den, bps);
  float metric;
  for (n = 0; n < number_symbols; n++) {
    for (k = 0; k < bps; k++) { num[k] = -1000000; den[k] = -1000000; }
    for (i = 0; i < M; i++) {
      metric = symbol_likelihood[n * M + i];
      mask = 1 << (bps - 1);
      for (k = 0; k < bps; k++) {
        if (mask & i) num[k] = max_star0(num[k], metric);
        else          den[k] = max_star0(den[k], metric);
        mask >>= 1;
      }
    }
    for (k = 0; k < bps; k++)
      bit_likelihood[bps * n + k] = num[k] - den[k];
  }
}

void symbols_to_llrs(float llr[], COMP rx_qpsk_symbols[], float rx_amps[],
                     float EsNo, float mean_amp, int nsyms) {
  int i;
  RADE_VLA(float, symbol_likelihood, nsyms * QPSK_CONSTELLATION_SIZE);
  RADE_VLA(float, bit_likelihood, nsyms * QPSK_BITS_PER_SYMBOL);
  Demod2D(symbol_likelihood, rx_qpsk_symbols, S_matrix, EsNo, rx_amps, mean_amp, nsyms);
  Somap(bit_likelihood, symbol_likelihood, QPSK_CONSTELLATION_SIZE,
        QPSK_BITS_PER_SYMBOL, nsyms);
  for (i = 0; i < nsyms * QPSK_BITS_PER_SYMBOL; i++)
    llr[i] = -bit_likelihood[i];
}

void fsk_rx_filt_to_llrs(float llr[], float rx_filt[], float v_est,
                         float SNRest, int M, int nsyms) {
  (void)llr; (void)rx_filt; (void)v_est; (void)SNRest; (void)M; (void)nsyms;
}

void ldpc_print_info(struct LDPC *ldpc) {
  fprintf(stderr, "ldpc->CodeLength = %d\n", ldpc->CodeLength);
  fprintf(stderr, "ldpc->NumberParityBits = %d\n", ldpc->NumberParityBits);
}
