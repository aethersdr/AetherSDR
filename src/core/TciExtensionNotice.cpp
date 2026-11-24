#include "TciExtensionNotice.h"

#include <QMetaObject>

#include <utility>

namespace AetherSDR {

TciExtensionNotice::TciExtensionNotice(QObject* context, int intervalMs,
                                       std::function<bool()> send)
    : m_context(context)
    , m_send(std::move(send))
{
    m_timer.setSingleShot(true);
    m_timer.setInterval(intervalMs);
    QObject::connect(&m_timer, &QTimer::timeout, m_context, [this]() {
        if (m_pending) {
            flush();
        }
    });
}

void TciExtensionNotice::queue()
{
    m_pending = true;
    if (m_posted || m_timer.isActive()) {
        return;
    }
    m_posted = true;
    QMetaObject::invokeMethod(m_context, [this]() {
        m_posted = false;
        flush();
    }, Qt::QueuedConnection);
}

void TciExtensionNotice::flush()
{
    m_pending = false;
    if (m_send()) {
        m_timer.start();
    }
}

} // namespace AetherSDR
