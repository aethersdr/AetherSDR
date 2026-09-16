/*  nnio.c

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

#define _CRT_SECURE_NO_WARNINGS

#include "comm.h"

#define NNIO_NAMELEN	40
#define NNIO_ENTRYLEN	72
#define NNIO_HDRLEN		32
#define NNIO_MAXDIM		4
#define NNIO_DT_F32		0
#define NNIO_DT_F64		1

typedef struct _nnio_ent
{
	char name[NNIO_NAMELEN + 1];
	int ndim;
	int dims[NNIO_MAXDIM];
	int numel;
	double* data;
} nnio_ent;

typedef struct _nnio
{
	int ntensors;
	nnio_ent* ent;
	double* blob;
	int nblob;
} nnio, * NNIO;

static unsigned int nnio_u32 (const unsigned char* p)
{
	return  (unsigned int)p[0]
		 | ((unsigned int)p[1] <<  8)
		 | ((unsigned int)p[2] << 16)
		 | ((unsigned int)p[3] << 24);
}

static unsigned long long nnio_u64 (const unsigned char* p)
{
	return  (unsigned long long)nnio_u32 (p)
		 | ((unsigned long long)nnio_u32 (p + 4) << 32);
}

static float nnio_f32 (const unsigned char* p)
{
	union { unsigned int u; float f; } v;
	v.u = nnio_u32 (p);
	return v.f;
}

static double nnio_f64 (const unsigned char* p)
{
	union { unsigned long long u; double d; } v;
	v.u = nnio_u64 (p);
	return v.d;
}

static NNIO nnio_parse (const unsigned char* raw, size_t nbytes,
	const char* what)
{
	NNIO f = 0;
	unsigned int version, ntensors;
	unsigned long long data_offset, data_bytes;
	int i, d, k, numel, total, cursor;

	if (nbytes < NNIO_HDRLEN)
	{
		dprintf ("nnio: %s is too short to be a tensor file\n", what);
		return 0;
	}

	if (memcmp (raw, "WDSPNN\0\0", 8) != 0)
	{
		dprintf ("nnio: %s has a bad magic number\n", what);
		return 0;
	}

	version     = nnio_u32 (raw + 8);
	ntensors    = nnio_u32 (raw + 12);
	data_offset = nnio_u64 (raw + 16);
	data_bytes  = nnio_u64 (raw + 24);

	if (version != 1)
	{
		dprintf ("nnio: %s has unsupported version %u\n", what, version);
		return 0;
	}

	if (ntensors == 0 || ntensors > 100000 ||
		(unsigned long long)nbytes < data_offset + data_bytes ||
		(unsigned long long)nbytes <
			NNIO_HDRLEN + (unsigned long long)ntensors * NNIO_ENTRYLEN)
	{
		dprintf ("nnio: %s has an inconsistent header\n", what);
		return 0;
	}

	f = (NNIO) malloc0 (sizeof (nnio));
	f->ntensors = (int)ntensors;
	f->ent = (nnio_ent*) malloc0 (ntensors * sizeof (nnio_ent));

	total = 0;
	for (i = 0; i < (int)ntensors; i++)
	{
		const unsigned char* e = raw + NNIO_HDRLEN + (size_t)i * NNIO_ENTRYLEN;
		unsigned int dtype  = nnio_u32 (e + 40);
		unsigned int ndim   = nnio_u32 (e + 44);
		unsigned int offset = nnio_u32 (e + 64);
		unsigned int nbytes_t = nnio_u32 (e + 68);

		memcpy (f->ent[i].name, e, NNIO_NAMELEN);
		f->ent[i].name[NNIO_NAMELEN] = '\0';

		if (ndim < 1 || ndim > NNIO_MAXDIM)
		{
			dprintf ("nnio: tensor '%s' has bad ndim %u\n", f->ent[i].name, ndim);
			goto fail;
		}

		numel = 1;
		f->ent[i].ndim = (int)ndim;
		for (d = 0; d < NNIO_MAXDIM; d++)
		{
			f->ent[i].dims[d] = (int)nnio_u32 (e + 48 + 4 * d);
			if (d < (int)ndim)
			{
				if (f->ent[i].dims[d] < 1)
				{
					dprintf ("nnio: tensor '%s' has bad dim[%d]\n", f->ent[i].name, d);
					goto fail;
				}
				numel *= f->ent[i].dims[d];
			}
		}
		f->ent[i].numel = numel;

		if ((dtype != NNIO_DT_F32 && dtype != NNIO_DT_F64) ||
			nbytes_t != (unsigned int)numel * (dtype == NNIO_DT_F32 ? 4u : 8u) ||
			(unsigned long long)offset + nbytes_t > data_bytes)
		{
			dprintf ("nnio: tensor '%s' has an inconsistent descriptor\n", f->ent[i].name);
			goto fail;
		}

		total += numel;
	}

	f->nblob = total;
	f->blob = (double*) malloc0 ((size_t)total * sizeof (double));
	cursor = 0;
	for (i = 0; i < (int)ntensors; i++)
	{
		const unsigned char* e = raw + NNIO_HDRLEN + (size_t)i * NNIO_ENTRYLEN;
		unsigned int dtype  = nnio_u32 (e + 40);
		unsigned int offset = nnio_u32 (e + 64);
		const unsigned char* src = raw + data_offset + offset;

		f->ent[i].data = f->blob + cursor;
		if (dtype == NNIO_DT_F32)
			for (k = 0; k < f->ent[i].numel; k++)
				f->ent[i].data[k] = (double) nnio_f32 (src + 4 * (size_t)k);
		else
			for (k = 0; k < f->ent[i].numel; k++)
				f->ent[i].data[k] = nnio_f64 (src + 8 * (size_t)k);
		cursor += f->ent[i].numel;
	}

	dprintf ("nnio: loaded %s - %d tensors, %d elements\n", what,
		f->ntensors, f->nblob);
	return f;

fail:
	_aligned_free (f->ent);
	_aligned_free (f);
	return 0;
}

NNIO nnio_open (const char* path)
{
	FILE* fp;
	unsigned char* raw;
	NNIO f;
	long fsize;

	if ((fp = fopen (path, "rb")) == 0)
	{
		dprintf ("nnio: cannot open %s\n", path);
		return 0;
	}

	fseek (fp, 0, SEEK_END);
	fsize = ftell (fp);
	fseek (fp, 0, SEEK_SET);

	if (fsize <= 0)
	{
		dprintf ("nnio: %s is empty\n", path);
		fclose (fp);
		return 0;
	}

	raw = (unsigned char*) malloc0 ((size_t)fsize);
	if (fread (raw, 1, (size_t)fsize, fp) != (size_t)fsize)
	{
		dprintf ("nnio: short read on %s\n", path);
		_aligned_free (raw);
		fclose (fp);
		return 0;
	}
	fclose (fp);

	f = nnio_parse (raw, (size_t)fsize, path);
	_aligned_free (raw);
	return f;
}

NNIO nnio_open_mem (const unsigned char* buf, size_t nbytes,
	const char* what)
{
	return nnio_parse (buf, nbytes, what ? what : "<built-in>");
}

void nnio_close (NNIO f)
{
	if (f == 0) return;
	_aligned_free (f->blob);
	_aligned_free (f->ent);
	_aligned_free (f);
}

/********************************************************************************************************
*																										*
*											Accessors												    *
*																										*
********************************************************************************************************/

int nnio_count (NNIO f)
{
	return f->ntensors;
}

const char* nnio_name (NNIO f, int idx)
{
	if (idx < 0 || idx >= f->ntensors) return "";
	return f->ent[idx].name;
}

int nnio_find (NNIO f, const char* name)
{
	int i;
	for (i = 0; i < f->ntensors; i++)
		if (strcmp (f->ent[i].name, name) == 0) return i;
	return -1;
}

int nnio_ndim (NNIO f, int idx)
{
	if (idx < 0 || idx >= f->ntensors) return 0;
	return f->ent[idx].ndim;
}

int nnio_dim (NNIO f, int idx, int d)
{
	if (idx < 0 || idx >= f->ntensors || d < 0 || d >= NNIO_MAXDIM) return 0;
	return f->ent[idx].dims[d];
}

int nnio_numel (NNIO f, int idx)
{
	if (idx < 0 || idx >= f->ntensors) return 0;
	return f->ent[idx].numel;
}

const double* nnio_data (NNIO f, int idx)
{
	if (idx < 0 || idx >= f->ntensors) return 0;
	return f->ent[idx].data;
}

const double* nnio_get (NNIO f, const char* name, int d0, int d1, int d2, int d3)
{
	int want[NNIO_MAXDIM];
	int idx, d, ndim;

	if ((idx = nnio_find (f, name)) < 0)
	{
		dprintf ("nnio: tensor '%s' not found\n", name);
		return 0;
	}

	want[0] = d0;  want[1] = d1;  want[2] = d2;  want[3] = d3;

	ndim = 0;
	for (d = 0; d < NNIO_MAXDIM; d++)
		if (want[d] != 0) ndim = d + 1;

	if (ndim != f->ent[idx].ndim)
	{
		dprintf ("nnio: tensor '%s' has ndim %d, expected %d\n",
			name, f->ent[idx].ndim, ndim);
		return 0;
	}

	for (d = 0; d < ndim; d++)
	{
		if (want[d] < 0) continue;					// -1 means "any"
		if (want[d] != f->ent[idx].dims[d])
		{
			dprintf ("nnio: tensor '%s' dim[%d] is %d, expected %d\n",
				name, d, f->ent[idx].dims[d], want[d]);
			return 0;
		}
	}

	return f->ent[idx].data;
}
