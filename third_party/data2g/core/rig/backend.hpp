// What the controller needs a radio to do.
//
// A seam: it keeps the part with the hard-won *behaviour* -- one handle
// on one thread, PTT that never queues behind a stale poll, a teardown
// that does not block -- in `data2g_core` (RigController), buildable and
// testable with no libhamlib and no radio. `core/rig/hamlib/hamlib.hpp`
// (data2g_rig) is the real implementation; `tests/test_rig.cpp` uses a
// fake that accepts and never answers, which is the scenario SSTVAE's
// `test_rig_controller.py` exists for.
//
// Every method here **blocks**, and may block for the rig's timeout.
// That is the whole problem this subsystem is arranged around.

#ifndef DATA2G_RIG_BACKEND_HPP
#define DATA2G_RIG_BACKEND_HPP

#include <stdexcept>
#include <string>

namespace data2g::rig {

class RigError : public std::runtime_error {
public:
    explicit RigError(const std::string& what) : std::runtime_error(what) {}
};

class RigBackend {
public:
    virtual ~RigBackend() = default;

    // Open the radio. Throws RigError with something an operator can act
    // on -- the wrong device path and a busy port are both routine.
    virtual void open() = 0;

    // Close it. Called from the worker thread as it exits, and must not
    // throw: by then there is nobody left to tell.
    virtual void close() noexcept = 0;

    virtual void set_ptt(bool on) = 0;

    // Dial frequency in Hz.
    virtual double frequency_hz() = 0;

    // For status text. Need not be the model name the user picked.
    virtual std::string description() const = 0;
};

}  // namespace data2g::rig

#endif
