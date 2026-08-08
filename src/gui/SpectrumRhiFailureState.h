#pragma once

#include "PanadapterMessageOverlay.h"

#include <QString>

namespace AetherSDR {

// Small value object for the latched QRhi failure contract. Keeping the state
// independent from SpectrumWidget makes the first-failure and recovery rules
// testable without constructing the full GPU widget.
class SpectrumRhiFailureState
{
public:
    bool report(const QString& reason)
    {
        if (m_failed) {
            return false;
        }
        m_failed = true;
        m_reason = reason;
        return true;
    }

    bool clear()
    {
        if (!m_failed) {
            return false;
        }
        m_failed = false;
        m_reason.clear();
        return true;
    }

    bool failed() const { return m_failed; }
    const QString& reason() const { return m_reason; }

    QString rendererDescription() const
    {
        return m_failed
            ? QStringLiteral("QRhi failed: %1").arg(m_reason)
            : QString();
    }

    PanadapterOverlayMessage warningMessage(const QString& title,
                                             const QString& detail) const
    {
        PanadapterOverlayMessage message;
        message.id = QStringLiteral("rhi.render-failed");
        message.title = title;
        message.detail = detail;
        message.timeoutMs = 0;
        message.dismissible = false;
        message.collapsible = true;
        message.tone = PanadapterOverlayMessageTone::Warning;
        return message;
    }

private:
    bool m_failed{false};
    QString m_reason;
};

} // namespace AetherSDR
