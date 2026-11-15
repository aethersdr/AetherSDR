/*
libspecbleach - A spectral processing library

Copyright 2022 Luciano Dato <lucianodato@gmail.com>

This library is free software; you can redistribute it and/or
modify it under the terms of the GNU Lesser General Public
License as published by the Free Software Foundation; either
version 2.1 of the License, or (at your option) any later version.

This library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public
License along with this library; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include "general_utils.h"
#include <float.h>
#include <math.h>
#include <stdlib.h>

float sanitize_denormal(float value) {
  if (!isnormal(value)) {
    value = 0.F;
  }
  return value;
}

float from_db_to_coefficient(const float value_db) {
  return expf(value_db / 20.F * logf(10.F));
}

float remap_percentage_log_like_unity(const float value) {
  return 1.F - expf(-3.F * (value));
}

int get_next_divisible_two(int number) {
  int q = number / 2;
  int n1 = 2 * q;
  int n2 = (number * 2) > 0 ? (2 * (q + 1)) : (2 * (q - 1));
  if (abs(number - n1) < abs(number - n2)) {
    return n1;
  }

  return n2;
}

int get_next_power_two(int number) {
  return (int)roundf(powf(2.F, ceilf(log2f((float)number))));
}
