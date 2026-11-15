/*  nnio.h

This file is part of a program that implements a Software-Defined Radio.

Copyright (C) 2026 Warren Pratt, NR0V

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

#ifndef _nnio_h
#define _nnio_h

#include <stddef.h>

typedef struct _nnio* NNIO;

extern NNIO nnio_open (const char* path);

extern NNIO nnio_open_mem (const unsigned char* buf, size_t nbytes,
	const char* what);

extern void nnio_close (NNIO f);

extern int nnio_count (NNIO f);

extern const char* nnio_name (NNIO f, int idx);

extern int nnio_find (NNIO f, const char* name);

extern int nnio_ndim (NNIO f, int idx);

extern int nnio_dim (NNIO f, int idx, int d);

extern int nnio_numel (NNIO f, int idx);

extern const double* nnio_data (NNIO f, int idx);

extern const double* nnio_get (NNIO f, const char* name, int d0, int d1, int d2, int d3);

#endif
