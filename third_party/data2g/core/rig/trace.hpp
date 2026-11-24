// A place for the rig path to say what it is doing, below Hamlib.
//
// **This exists because a trace stopped exactly where the interesting
// part began.** `rig::set_debug_sink` gives an app Hamlib's own view --
// which for a radio that never answers is "I wrote six bytes and read
// nothing", repeated. That is one end of a chain whose other end is a
// USB endpoint on a phone, and nothing in between said anything: not
// the loopback bridge, not the transport, not which driver the Android
// USB layer picked for the device. So a failure inside our own code and
// a radio ignoring a correctly delivered command produced *byte for
// byte the same log*.
//
// Qt-free and Hamlib-free, in `data2g_core`, so `bridge.cpp` and the
// Android transport can both write to it -- and so a `--no-rig` build
// still compiles and tests every line of it.
//
// **Off costs one relaxed atomic load.** This sits in the byte pump
// between Hamlib and the radio, so the check has to be cheap enough
// that leaving the calls in is not a decision anybody has to think
// about. Formatting the message is the caller's job and must be guarded
// by `tracing()`, which is why that is public rather than an
// implementation detail.

#ifndef DATA2G_RIG_TRACE_HPP
#define DATA2G_RIG_TRACE_HPP

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace data2g::rig {

// Called once per line, with no trailing newline, from whichever thread
// produced it -- the bridge's two pump threads and the rig worker, so
// possibly three at once. It must not block.
using TraceSink = std::function<void(const std::string& line)>;

// An empty sink turns tracing off, which is the default.
void set_trace_sink(TraceSink sink);

// Whether anything is listening. Guard message construction with it.
bool tracing();

// Emit one line. A no-op when nothing is listening.
void trace(const std::string& line);

// `fe fe a2 e0 03 fd` -- the spelling Hamlib's own `dump_hex` uses, so
// a frame in this log can be compared against one in that log without
// translating between two formats. Long blocks are truncated with a
// count, because a trace is for frames and a picture's worth of bytes
// in one line helps nobody.
std::string hex_bytes(const std::uint8_t* data, std::size_t n, std::size_t max = 32);

}  // namespace data2g::rig

#endif
