#include "PipeWireNativeContext.h"
#include "LogManager.h"

#include <pipewire/pipewire.h>

namespace AetherSDR {

PipeWireNativeContext& PipeWireNativeContext::instance()
{
    static PipeWireNativeContext s;
    return s;
}

PipeWireNativeContext::~PipeWireNativeContext()
{
    // Refcount should already be zero — release() is responsible for teardown.
    // This is just a safety net for static destruction.
    if (m_loop) {
        pw_thread_loop_stop(m_loop);
        if (m_core) {
            pw_core_disconnect(m_core);
            m_core = nullptr;
        }
        if (m_context) {
            pw_context_destroy(m_context);
            m_context = nullptr;
        }
        pw_thread_loop_destroy(m_loop);
        m_loop = nullptr;
    }
}

bool PipeWireNativeContext::acquire()
{
    std::lock_guard<std::mutex> lock(m_initMutex);
    if (++m_refCount > 1) {
        return m_loop != nullptr;
    }

    // First user — initialize PipeWire.
    pw_init(nullptr, nullptr);

    m_loop = pw_thread_loop_new("aethersdr-dax", nullptr);
    if (!m_loop) {
        qCWarning(lcDax) << "PipeWireNativeContext: pw_thread_loop_new failed";
        m_refCount = 0;
        return false;
    }

    m_context = pw_context_new(pw_thread_loop_get_loop(m_loop), nullptr, 0);
    if (!m_context) {
        qCWarning(lcDax) << "PipeWireNativeContext: pw_context_new failed";
        pw_thread_loop_destroy(m_loop);
        m_loop = nullptr;
        m_refCount = 0;
        return false;
    }

    if (pw_thread_loop_start(m_loop) != 0) {
        qCWarning(lcDax) << "PipeWireNativeContext: pw_thread_loop_start failed";
        pw_context_destroy(m_context);
        m_context = nullptr;
        pw_thread_loop_destroy(m_loop);
        m_loop = nullptr;
        m_refCount = 0;
        return false;
    }

    // Connect to the PipeWire daemon.  The core must be created with the loop
    // locked because the connect runs callbacks on the loop thread.
    pw_thread_loop_lock(m_loop);
    m_core = pw_context_connect(m_context, nullptr, 0);
    pw_thread_loop_unlock(m_loop);

    if (!m_core) {
        qCWarning(lcDax) << "PipeWireNativeContext: pw_context_connect failed (no PipeWire daemon?)";
        pw_thread_loop_stop(m_loop);
        pw_context_destroy(m_context);
        m_context = nullptr;
        pw_thread_loop_destroy(m_loop);
        m_loop = nullptr;
        m_refCount = 0;
        return false;
    }

    qCInfo(lcDax) << "PipeWireNativeContext: connected to PipeWire daemon";
    return true;
}

void PipeWireNativeContext::release()
{
    std::lock_guard<std::mutex> lock(m_initMutex);
    if (m_refCount > 0 && --m_refCount > 0) {
        return;
    }

    if (!m_loop) {
        return;
    }

    pw_thread_loop_lock(m_loop);
    if (m_core) {
        pw_core_disconnect(m_core);
        m_core = nullptr;
    }
    pw_thread_loop_unlock(m_loop);

    pw_thread_loop_stop(m_loop);

    if (m_context) {
        pw_context_destroy(m_context);
        m_context = nullptr;
    }
    pw_thread_loop_destroy(m_loop);
    m_loop = nullptr;

    qCInfo(lcDax) << "PipeWireNativeContext: disconnected from PipeWire";
}

void PipeWireNativeContext::lock()
{
    if (m_loop) {
        pw_thread_loop_lock(m_loop);
    }
}

void PipeWireNativeContext::unlock()
{
    if (m_loop) {
        pw_thread_loop_unlock(m_loop);
    }
}

} // namespace AetherSDR
