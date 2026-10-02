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

// The locks belong to FFTW, not to any one class (#5895). Each precision has
// one process-global planner, neither thread-safe; the double-precision wisdom
// store is global; and fftw_malloc/fftw_alloc_* vs fftw_free across threads
// races (TSan, #5424). Every plan, destroy, wisdom import/export and FFTW
// allocation takes the lock for its precision. WdspChannel::fftwSetupLock()
// forwards here.
// fftw_make_planner_thread_safe() is not a substitute: it covers only the
// planner (per precision), not wisdom or malloc/free, and on macOS/Linux lives
// in libfftw3_threads, which nothing here links (Windows' vendored fftw3.lib
// exports it).

// DOUBLE PRECISION (fftw_*). Held by WdspChannel (open/close and control calls
// that re-plan: RXASetNC, RXASetMP), Hl2Spectrum, AnanPanAnalyzer, SpectralNR.
// Hold it over allocations as well as plans (#5424's frames were memalign and
// free). Never around fftw_execute(): thread-safe and on the real-time path.
// Holds are long (#5895): hundreds of ms for SpectralNR construction, tens of
// seconds for a cold WdspChannel::open(), 38.5 s for the worst FFTW_PATIENT
// plan in generateWisdom(); every other FFTW user waits.
[[nodiscard]] std::unique_lock<std::mutex> fftwPlannerLock();

// The same mutex, unwrapped, for call sites that already own a scoped_lock or
// need to compose it. Prefer fftwPlannerLock().
[[nodiscard]] std::mutex& fftwPlannerMutex();

// SINGLE PRECISION (fftwf_*). This is a separate planner and needs its own
// mutex. RtlSdrDdc holds it across allocation, plan creation, destruction and
// frees. SpecbleachFilter holds it across specbleach_initialize/free, which
// reach the vendored fft_transform.c's FFTW operations. Guarding in the
// wrapper covers every caller, including AudioEngine's direct automation
// probe and all of its filter teardown paths, without editing vendored C.
// Neither fftw_execute nor fftwf_execute takes a planner lock.
[[nodiscard]] std::unique_lock<std::mutex> fftwfPlannerLock();
[[nodiscard]] std::mutex& fftwfPlannerMutex();

} // namespace AetherSDR
