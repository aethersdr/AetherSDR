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

#ifndef GENERAL_UTILS_H
#define GENERAL_UTILS_H

#include <stdbool.h>
#include <stdint.h>

// Compile-time validation
_Static_assert(sizeof(float) >= 4, "float must be at least 32 bits");
_Static_assert(sizeof(double) >= 8, "double must be at least 64 bits");

__attribute__((warn_unused_result)) float sanitize_denormal(float value);
__attribute__((warn_unused_result)) float from_db_to_coefficient(
    float value_db);
__attribute__((warn_unused_result)) float remap_percentage_log_like_unity(
    float value);
__attribute__((warn_unused_result)) int get_next_divisible_two(int number);
__attribute__((warn_unused_result)) int get_next_power_two(int number);

#endif
