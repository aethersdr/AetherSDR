#include "ControlInputPump.h"

#include <QScopeGuard>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <utility>

namespace AetherSDR::control {

ControlInputPump::ControlInputPump(ControlService& service, ControlSession& session,
                                 Read read, Write write, Close close, QObject* parent)
    : QObject(parent), m_service(service), m_session(&session),
      m_read(std::move(read)), m_write(std::move(write)), m_close(std::move(close))
{
    Q_ASSERT(session.thread() == thread());
    Q_ASSERT(m_read && m_write && m_close);
}

ControlInputPump::~ControlInputPump()
{
    finish();
}

void ControlInputPump::finish()
{
    if (QThread::currentThread() != thread() || m_finished) {
        return;
    }
    m_finished = true;
    m_input.clear();
    if (m_session) {
        m_session->endAuthorization();
    }
}

bool ControlInputPump::deliver(const ServiceReply& reply)
{
    const QPointer<ControlInputPump> guard(this);
    if (reply.closeAfterWrite) {
        finish();
    }
    if (!guard) {
        return false;
    }
    // Copy callbacks because either may synchronously destroy this pump.
    const Write write = m_write;
    const bool sent = write(reply.message);
    if (!guard) {
        return false;
    }
    if (!sent || reply.closeAfterWrite) {
        finish();
        if (guard) {
            const Close close = m_close;
            close(!sent);
        }
        return false;
    }
    return !m_finished && m_session && !m_session->isRevoked();
}

void ControlInputPump::oversizedFrame()
{
    const ProtocolError limit{QStringLiteral("transport.limit_exceeded"),
                              QStringLiteral("input frame exceeds maxMessageBytes"), {}, false};
    (void)deliver({ControlProtocolCodec::errorResponse({}, limit), true});
}

void ControlInputPump::schedule()
{
    if (m_scheduled || m_finished) {
        return;
    }
    m_scheduled = true;
    QTimer::singleShot(0, this, [this] {
        m_scheduled = false;
        readAvailable();
    });
}

void ControlInputPump::readAvailable()
{
    if (QThread::currentThread() != thread() || m_finished || m_reading || m_scheduled
        || !m_session || m_session->isRevoked()) {
        return;
    }
    const QPointer<ControlInputPump> guard(this);
    m_reading = true;
    const auto restore = qScopeGuard([guard] {
        if (guard) { guard->m_reading = false; }
    });
    qint64 remaining = kReadBytesPerTurn;
    for (int count = 0; count < kFramesPerTurn; ++count) {
        qsizetype newline = m_input.indexOf('\n');
        if (newline < 0) {
            const qint64 maximum = std::min(remaining,
                ProtocolLimits::kMaxMessageBytes + 1 - static_cast<qint64>(m_input.size()));
            if (maximum <= 0) {
                if (m_input.size() > ProtocolLimits::kMaxMessageBytes) {
                    oversizedFrame();
                } else {
                    schedule();
                }
                return;
            }
            const Read read = m_read;
            const QByteArray bytes = read(maximum);
            if (!guard || m_finished || !m_session || m_session->isRevoked()) { return; }
            if (bytes.size() > maximum) { oversizedFrame(); return; }
            m_input.append(bytes);
            remaining -= bytes.size();
            newline = m_input.indexOf('\n');
            if (newline < 0) {
                if (m_input.size() > ProtocolLimits::kMaxMessageBytes) {
                    oversizedFrame();
                } else if (remaining == 0) {
                    schedule();
                }
                return;
            }
        }
        QByteArray frame = m_input.left(newline);
        m_input.remove(0, newline + 1);
        if (frame.endsWith('\r')) { frame.chop(1); }
        const ServiceReply reply = m_service.handle(frame, m_session);
        if (!guard || m_finished || !m_session) { return; }
        if (!deliver(reply)) { return; }
    }
    schedule();
}

} // namespace AetherSDR::control
