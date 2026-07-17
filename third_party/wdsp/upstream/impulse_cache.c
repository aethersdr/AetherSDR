/*  impulse_cache.c

This file is part of a program that implements a Software-Defined Radio.

Copyright (C) 2013, 2019, 2024 Warren Pratt, NR0V
Copyright (C) 2025 Richard Samphire, MW0LGE

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
mw0lge@grange-lane.co.uk

*/
//
//============================================================================================//
// Dual-Licensing Statement (Applies Only to Author's Contributions, Richard Samphire MW0LGE) //
// ------------------------------------------------------------------------------------------ //
// For any code originally written by Richard Samphire MW0LGE, or for any modifications       //
// made by him, the copyright holder for those portions (Richard Samphire) reserves the       //
// right to use, license, and distribute such code under different terms, including           //
// closed-source and proprietary licences, in addition to the GNU General Public License      //
// granted above. Nothing in this statement restricts any rights granted to recipients under  //
// the GNU GPL. Code contributed by others (not Richard Samphire) remains licensed under      //
// its original terms and is not affected by this dual-licensing statement in any way.        //
// Richard Samphire can be reached by email at :  mw0lge@grange-lane.co.uk                    //
//============================================================================================//

#define _CRT_SECURE_NO_WARNINGS
#include "comm.h"

/********************************************************************************************************
*																										*
*								Impulse Cache implementation											*
*																										*
********************************************************************************************************/

#if defined(_WIN64)
	static const uint64_t FNV_OFFSET_BASIS_64 = 14695981039346656037ULL;  // 0xcbf29ce484222325
	static const uint64_t FNV_PRIME_64 = 1099511628211ULL;				  // 0x100000001b3

	uint64_t fnv1a_hash64(const void* data, size_t len) {
		const uint8_t* bytes = (const uint8_t*)data;
		uint64_t hash = FNV_OFFSET_BASIS_64;
		for (size_t i = 0; i < len; ++i) {
			hash ^= bytes[i];
			hash *= FNV_PRIME_64;
		}
		return hash;
	}
#else
	static const uint32_t FNV_OFFSET_BASIS_32 = 2166136261U;      // 0x811C9DC5
	static const uint32_t FNV_PRIME_32 = 16777619U;               // 0x01000193

	uint32_t fnv1a_hash32(const void* data, size_t len) {
		const uint8_t* bytes = (const uint8_t*)data;
		uint32_t hash = FNV_OFFSET_BASIS_32;
		for (size_t i = 0; i < len; i++) {
			hash ^= bytes[i];
			hash *= FNV_PRIME_32;
		}
		return hash;
	}
#endif

typedef struct _cache_entry {
	HASH_T  hash;
	int		N;							// N complex entries in impulse. Leave as signed int as that is used everywhere
	double* impulse;
	struct _cache_entry* next;
} cache_entry;

static size_t _cache_counts[CACHE_BUCKETS] = { 0 };
static cache_entry* _cache_heads[CACHE_BUCKETS] = { NULL };
static CRITICAL_SECTION _cs_use_cache;
static int _run = 0;
static int _use_cache = 1;

void remove_impulse_cache_tail(size_t bucket)
{
	if (bucket >= CACHE_BUCKETS) return;

	cache_entry** pp = &_cache_heads[bucket];

	while (*pp && (*pp)->next) 
	{
		pp = &(*pp)->next;
	}

	if (*pp) 
	{
		_aligned_free((*pp)->impulse);
		_aligned_free(*pp);
		*pp = NULL;
		_cache_counts[bucket]--;
	}
}

void free_impulse_cache(void)
{
	for (size_t b = 0; b < CACHE_BUCKETS; ++b) {
		cache_entry* e = _cache_heads[b];
		while (e) {
			cache_entry* next = e->next;
			_aligned_free(e->impulse);
			_aligned_free(e);
			e = next;
		}
		_cache_heads[b] = NULL;
		_cache_counts[b] = 0;
	}
}

double* get_impulse_cache_entry(size_t bucket, HASH_T hash, int N)
{
	if (!_run) return NULL;

	int use;
	EnterCriticalSection(&_cs_use_cache);
	use = _use_cache;
	LeaveCriticalSection(&_cs_use_cache);

	if (!use || bucket >= CACHE_BUCKETS) return NULL;

	// lru, least recently used, moves cache hit to head
	// old cache entries will move towards the tail and eventually be dumped
	cache_entry* prev = NULL;
	cache_entry* e = _cache_heads[bucket];
	
	while (e) {
		if (e->hash == hash && e->N == N) 
		{
			if (prev)
			{
				prev->next = e->next;
				e->next = _cache_heads[bucket];
				_cache_heads[bucket] = e;
			}
			double* imp = (double*) malloc0(e->N * sizeof(complex));
			memcpy(imp, e->impulse, e->N * sizeof(complex));
			return imp;
		}
		prev = e;
		e = e->next;
	}

	return NULL;
}

void add_impulse_to_cache(size_t bucket, HASH_T hash, int N, double* impulse)
{
	if (!_run) return;

	int use;
	EnterCriticalSection(&_cs_use_cache);
	use = _use_cache;
	LeaveCriticalSection(&_cs_use_cache);

	if (!use || bucket >= CACHE_BUCKETS) return;

	if (_cache_counts[bucket] >= MAX_CACHE_ENTRIES) remove_impulse_cache_tail(bucket);

	cache_entry* e = malloc0(sizeof(cache_entry));
	e->hash = hash;
	e->N = N;
	e->impulse = (double *) malloc0(N * sizeof(complex));
	memcpy(e->impulse, impulse, N * sizeof(complex));
	e->next = _cache_heads[bucket];
	_cache_heads[bucket] = e;
	_cache_counts[bucket]++;
}

PORT
int save_impulse_cache(const char* path)
{
	if (!_run) return 0;

	int use;
	EnterCriticalSection(&_cs_use_cache);
	use = _use_cache;
	LeaveCriticalSection(&_cs_use_cache);
	if (!use) return 0;

	FILE* fp = fopen(path, "wb");
	if (!fp) return -1;
	uint32_t buckets = CACHE_BUCKETS;
	if (fwrite(&buckets, sizeof(buckets), 1, fp) != 1) { fclose(fp); return -1; }
	for (size_t b = 0; b < CACHE_BUCKETS; b++) {
		uint32_t count = 0;
		for (cache_entry* e = _cache_heads[b]; e; e = e->next) count++;
		if (fwrite(&count, sizeof(count), 1, fp) != 1) { fclose(fp); return -1; }
		for (cache_entry* e = _cache_heads[b]; e; e = e->next) {
			if (fwrite(&e->hash, sizeof(HASH_T), 1, fp) != 1) { fclose(fp); return -1; }
			if (fwrite(&e->N, sizeof(e->N), 1, fp) != 1) { fclose(fp); return -1; }
			if (fwrite(e->impulse, sizeof(complex), e->N, fp) != (size_t)e->N) { fclose(fp); return -1; }
		}
	}
	fclose(fp);
	return 0;
}

PORT
int read_impulse_cache(const char* path)
{
	if (!_run) return 0;

	free_impulse_cache();

	int use;
	EnterCriticalSection(&_cs_use_cache);
	use = _use_cache;
	LeaveCriticalSection(&_cs_use_cache);
	if (!use) return 0;

	FILE* fp = fopen(path, "rb");
	if (!fp) return -1;
	uint32_t buckets;
	if (fread(&buckets, sizeof(buckets), 1, fp) != 1) { fclose(fp); return -1; }
	if (buckets != CACHE_BUCKETS) { fclose(fp); return -1; }
	for (size_t b = 0; b < buckets; b++) {
		uint32_t count;
		if (fread(&count, sizeof(count), 1, fp) != 1) { fclose(fp); return -1; }
		cache_entry* tail = NULL;
		for (uint32_t i = 0; i < count; i++) {
			HASH_T hash;
			int    N;
			if (fread(&hash, sizeof(HASH_T), 1, fp) != 1) { fclose(fp); return -1; }
			if (fread(&N, sizeof(N), 1, fp) != 1) { fclose(fp); return -1; }
			double* data = (double*)malloc0(N * sizeof(complex));
			if (fread(data, sizeof(complex), N, fp) != (size_t)N) { _aligned_free(data); fclose(fp); return -1; }
			cache_entry* e = (cache_entry*)malloc0(sizeof(cache_entry));
			e->hash = hash;
			e->N = N;
			e->impulse = data;
			e->next = NULL;
			if (tail)       
				tail->next = e;
			else
				_cache_heads[b] = e;
			tail = e;
			_cache_counts[b]++;
		}
	}
	fclose(fp);
	return 0;
}

PORT
void use_impulse_cache(int use) 
{
	EnterCriticalSection(&_cs_use_cache);
	_use_cache = use;
	LeaveCriticalSection(&_cs_use_cache);
}

PORT
void init_impulse_cache(int use)
{
	//InitializeCriticalSection(&_cs_use_cache);
	InitializeCriticalSectionAndSpinCount(&_cs_use_cache, 2500);

	EnterCriticalSection(&_cs_use_cache);
	_use_cache = use;
	LeaveCriticalSection(&_cs_use_cache);

	_run = 1;
}

PORT
void destroy_impulse_cache(void)
{
	_run = 0;

	DeleteCriticalSection(&_cs_use_cache);

	free_impulse_cache();
}
