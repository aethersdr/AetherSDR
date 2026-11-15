// A hook for the threads that move audio: the sound card's capture and playback threads (or the pipe threads
// that stand in for them) and the transmit thread call thread_init(role) first thing on their own thread, and
// whatever the application installed runs there. The station installs one that asks for real-time scheduling
// (app/realtime.hpp); with none installed nothing happens, so the core knows nothing about schedulers.
#pragma once

#include <functional>

namespace data2g::audio {

using ThreadInit = std::function<void(const char* role)>;

// Install (or, with an empty function, remove) the hook. Not while audio threads are starting.
void set_thread_init(ThreadInit f);

// The calling thread says what it is: "capture", "playback", "tx".
void thread_init(const char* role);

}  // namespace data2g::audio
