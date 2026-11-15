#pragma once

#include <mutex>

struct pw_context;
struct pw_core;
struct pw_thread_loop;

namespace AetherSDR {

// Process-wide owner of the PipeWire client context used by native RX sources.
// pw_thread_loop runs its own dedicated thread; all pw_* calls outside of
// stream callbacks must be wrapped in lock()/unlock().  Lifetime is shared
// across all RX channels so the context+core is created once on first
// acquire() and destroyed when the last user releases.
class PipeWireNativeContext {
public:
    static PipeWireNativeContext& instance();

    // Increment refcount.  On the first acquire() the pw_init / pw_thread_loop /
    // pw_context / pw_core are created and the loop thread is started.
    // Returns true on success.  Safe to call from any thread.
    bool acquire();

    // Decrement refcount.  On the last release the loop is stopped and the
    // context+core are destroyed.  Safe to call from any thread.
    void release();

    // Lock/unlock the loop.  Required around any pw_* call from outside a
    // stream callback (callbacks already run on the loop thread and must not
    // re-lock).
    void lock();
    void unlock();

    pw_thread_loop* loop() { return m_loop; }
    pw_context*     context() { return m_context; }
    pw_core*        core() { return m_core; }

private:
    PipeWireNativeContext() = default;
    ~PipeWireNativeContext();
    PipeWireNativeContext(const PipeWireNativeContext&) = delete;
    PipeWireNativeContext& operator=(const PipeWireNativeContext&) = delete;

    pw_thread_loop* m_loop{nullptr};
    pw_context*     m_context{nullptr};
    pw_core*        m_core{nullptr};

    // Serializes init/teardown so concurrent acquire()/release() callers
    // never observe a half-initialised context.  Today the lifecycle is
    // driven from the main thread, but the documented contract on these
    // methods is "safe to call from any thread" — the mutex makes that
    // contract real instead of incidentally true.
    std::mutex m_initMutex;
    int        m_refCount{0};
};

} // namespace AetherSDR
