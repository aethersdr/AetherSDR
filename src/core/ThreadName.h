#pragma once

namespace AetherSDR {

// Name the CALLING thread for the kernel, with no Qt dependency (#2554). Qt
// names QThreads itself; raw std::thread workers (the CW keyers) must call
// this. Qt-free because IambicKeyer's test target links only pthread.
// SystemInfo::setCurrentThreadName() wraps this and sets the Qt name too.
// Linux truncates to 15 characters.
void setCurrentThreadName(const char* name);

}  // namespace AetherSDR
