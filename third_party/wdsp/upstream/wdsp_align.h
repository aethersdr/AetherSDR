/*  wdsp_align.h  --  aligned allocation, on either platform.

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

/*  WHY THIS EXISTS

    _aligned_malloc and _aligned_free are Microsoft names.  They do not
    exist in glibc at all.  A file that calls them and includes only
    <stdlib.h> compiles cleanly under MSVC, because the CRT declares them,
    and falls back to an IMPLICIT DECLARATION under gcc - which assumes the
    function returns int.

    On a 64-bit target that truncates the returned pointer to 32 bits.  The
    program then crashes on first use, which is what a Linux port reported
    happening at the first TXA open.  Nothing was wrong on the Windows side;
    the names simply are not there on the other platform.

    Including comm.h would also fix it, but drags in the whole library's
    interface where a few source files need only an allocator.  This header
    is the smallest thing that does the job:

        #include "wdsp_align.h"

    and nothing else changes.  The platform test lives here, once, rather
    than in every file that allocates.

    ALIGNMENT.  posix_memalign requires the alignment to be a power of two
    AND a multiple of sizeof(void*).  _aligned_malloc requires only a power
    of two, so an alignment of 4 or 8 on a 64-bit system is legal for one
    and not the other.  The shim rounds up rather than failing, since a
    stronger alignment is always acceptable to the caller.
*/

#ifndef _wdsp_align_h
#define _wdsp_align_h

#include <stdlib.h>

#if defined(_WIN32) || defined(_WIN64)

/*  MSVC declares both in <malloc.h>, which <stdlib.h> also pulls in.  It is
    named explicitly here so the dependency is visible rather than inherited.
*/
#include <malloc.h>

#else

/*  posix_memalign is declared by <stdlib.h> only when a POSIX feature-test
    macro is in effect.  Under -std=c99 or -std=c++17, which select STRICT
    ISO mode, it is hidden - and a header that then calls it gets an
    implicit declaration, which is the very failure this file exists to
    prevent.

    Defining _POSIX_C_SOURCE here would work only if this header were
    included before any system header, which cannot be guaranteed.
    Declaring the function is exact, self-contained, and legal whether or
    not <stdlib.h> has already declared it - identical declarations are
    permitted.                                                             */
#if defined(__cplusplus)
extern "C"
#endif
int posix_memalign (void** memptr, size_t alignment, size_t size);

/*  static inline, so each translation unit gets its own copy and there is
    no symbol to add to a build file.  A caller sees exactly the Microsoft
    signatures.                                                            */

static inline void* _aligned_malloc (size_t size, size_t alignment)
{
	void* p = 0;

	/*  posix_memalign is stricter than _aligned_malloc about what it will
	    accept.  Raising the alignment is always safe for the caller.      */
	if (alignment < sizeof (void*))
		alignment = sizeof (void*);

	if (posix_memalign (&p, alignment, size) != 0)
		return 0;

	return p;
}

static inline void _aligned_free (void* p)
{
	/*  Memory from posix_memalign is released with plain free.  This is
	    guaranteed, unlike the Windows pair, where _aligned_free is
	    REQUIRED and free would corrupt the heap.                        */
	free (p);
}

#endif

#endif
