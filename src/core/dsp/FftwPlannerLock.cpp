/*  FftwPlannerLock.cpp — see FftwPlannerLock.h for why this is not in
    WdspChannel.

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

#include "core/dsp/FftwPlannerLock.h"

namespace AetherSDR {

// FUNCTION-LOCAL STATICS, not namespace-scope objects, so the mutex is
// constructed on first use rather than at some dynamic-initialisation point
// this file does not control. WdspChannel.cpp binds a reference to
// fftwPlannerMutex() during its own dynamic initialisation, which is only
// safe because of this.
std::mutex& fftwPlannerMutex()
{
    static std::mutex m;
    return m;
}

std::unique_lock<std::mutex> fftwPlannerLock()
{
    return std::unique_lock<std::mutex>(fftwPlannerMutex());
}

std::mutex& fftwfPlannerMutex()
{
    static std::mutex m;
    return m;
}

std::unique_lock<std::mutex> fftwfPlannerLock()
{
    return std::unique_lock<std::mutex>(fftwfPlannerMutex());
}

} // namespace AetherSDR
