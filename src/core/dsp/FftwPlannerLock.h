/*  FftwPlannerLock.h

This file is part of AetherSDR.

Copyright (C) 2024-2026 AetherSDR Contributors

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#pragma once

#include <mutex>

namespace AetherSDR {

// THE LOCK BELONGS TO FFTW, NOT TO ANY ONE CLASS.
//
// FFTW's planner is PROCESS-GLOBAL and not thread-safe. So is its wisdom
// store, and so — per the ThreadSanitizer report in #5424 — is the pairing of
// fftw_malloc/fftw_alloc_* against fftw_free across threads. Every plan,
// destroy, wisdom import/export and FFTW allocation in this process must be
// serialised, and NOT by a mutex each subsystem keeps to itself.
//
// This header exists because that lock used to live inside WdspChannel, which
// made it look like a property of the WDSP channel. It is not. SpectralNR
// has nothing to do with WDSP and had, for that reason, a private static
// mutex of its own (#467, correct when it was written: in March 2026 all the
// FFTW in the tree really was in SpectralNR.cpp; WDSP was vendored in July).
// Two mutexes over one planner serialise nothing, which is #5895. Reaching
// the lock through WdspChannel.h would be a worse dependency edge than the
// bug it fixes, so the lock moved out here and WdspChannel::fftwSetupLock()
// now forwards to it under its existing name.
//
// FFTW's OWN answer, and why it is not a drop-in: fftw_make_planner_thread_safe()
// would cover the planner, but it needs BOTH spellings (fftw_ and fftwf_ are
// independent planners) and covers NEITHER the wisdom database NOR the
// malloc/free edge. It is also not available everywhere this builds. On macOS
// and Linux it lives in libfftw3_threads / libfftw3f_threads -- verified on
// macOS with dlsym, absent from libfftw3 itself -- and NO target in this tree
// links either. On WINDOWS it IS available: CMakeLists.txt's if(WIN32) branch
// links the vendored third_party/fftw3/lib/fftw3.lib, whose libfftw3-3.def
// exports fftw_make_planner_thread_safe, because upstream's Windows DLL folds
// the threads layer into the main library. So adopting it means adding a link
// dependency on the two platforms this is developed and CI'd on, before the
// one call per precision. It would shrink this lock's job, not remove it.

// DOUBLE PRECISION (fftw_*). Held by WdspChannel (open/close and every
// control call that can re-plan: RXASetNC and RXASetMP both do), Hl2Spectrum,
// AnanSpectrum and SpectralNR.
//
// WIDTH: hold it over the ALLOCATIONS as well as the plan. The two frames
// TSan named in #5424 are fftw_malloc_plain's memalign on one thread and a
// free from WDSP's create_fircore on another — neither is the planner, so a
// plan-only scope leaves the proven edge open. fftw_execute() is thread-safe
// and must NOT be serialised: it is on the real-time path.
//
// COST, measured rather than assumed — see #5895. This lock is held for
// hundreds of milliseconds by a SpectralNR construction, for tens of seconds
// by a cold WdspChannel::open(), and for 38.5 s by the single worst
// FFTW_PATIENT plan inside SpectralNR::generateWisdom() (size 262144, macOS
// arm64; the whole sweep is 333 s). Anything you put inside it, every other
// FFTW user in the process waits for.
[[nodiscard]] std::unique_lock<std::mutex> fftwPlannerLock();

// The same mutex, unwrapped, for call sites that already own a scoped_lock or
// need to compose it. Prefer fftwPlannerLock().
[[nodiscard]] std::mutex& fftwPlannerMutex();

// TODO(#5895): SINGLE PRECISION (fftwf_*) IS NOT COVERED BY ANYTHING.
//
// fftwf_ is a SEPARATE planner with separate global state, so the lock above
// does not reach it. Two call sites plan in single precision and hold no lock
// against each other, both linked into aethercore, both reached from a
// connect-path thread:
//
//   - RtlSdrDdc::RtlSdrDdc / ~RtlSdrDdc — fftwf_malloc, fftwf_plan_dft_1d,
//     fftwf_destroy_plan, fftwf_free, all unguarded.
//   - third_party/libspecbleach's fft_transform.c — fft_transform_initialize's
//     fftwf_malloc and fftwf_plan_r2r_1d, and fft_transform_free's
//     fftwf_destroy_plan / fftwf_free — reached through SpecbleachFilter,
//     constructed in AudioEngine::createNr4Filter. Vendored, so the lock goes
//     around the SpecbleachFilter construction and destruction rather than
//     into the vendored C.
//
// NO fftwfPlannerLock() IS DECLARED HERE, DELIBERATELY. An exported symbol
// with no callers reads as "something takes this" and guards nothing; it is a
// promise in a header, which is the shape this PR argues against. The lock
// lands in the same commit as its first caller. This note exists so that the
// next person to touch either site adds it here rather than inventing a third
// mutex, which is exactly how this bug was born.

} // namespace AetherSDR
