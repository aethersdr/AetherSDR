#pragma once

namespace AetherSDR {

// Name the CALLING thread for the kernel, with no Qt dependency (#2554).
//
// Qt already names the threads it starts, propagating QThread::objectName() to
// the OS in QThreadPrivate::start(). Raw std::thread workers never pass through
// that path, so they must name themselves or appear as unnamed rows in the
// System Info thread table — which is least helpful for exactly the workers
// most worth watching, the CW keyers.
//
// Deliberately Qt-free: IambicKeyer is compiled into a test target that links
// only pthread (tests.cmake, iambic_keyer_test), and that property is worth
// keeping — it is what lets the keyer logic be exercised without an event loop.
// SystemInfo::setCurrentThreadName() wraps this and adds the Qt-side name for
// callers that already have Qt.
//
// Truncated to 15 characters on Linux, which is the kernel's limit.
void setCurrentThreadName(const char* name);

}  // namespace AetherSDR
