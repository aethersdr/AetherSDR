#pragma once

#include "models/PanadapterModel.h"

#include <QPointer>

namespace AetherSDR {

// Snapshot on frame arrival, before receive-presentation delay. A confirmed
// native FFT is cropped to that observation; it cannot be painted against a
// newer range (even if its bin count or eventual center happen to match).
// Flex's independently paced status/data planes retain their existing path.
class PanFrameGuard {
public:
    PanFrameGuard(bool confirmed, PanadapterModel* pan)
        : m_confirmed(confirmed), m_pan(pan),
          m_revision(pan ? pan->geometryRevision() : 0) {}

    bool isCurrent() const {
        return !m_confirmed || (m_pan && m_pan->centerKnown()
            && m_pan->geometryRevision() == m_revision);
    }

private:
    bool m_confirmed;
    QPointer<PanadapterModel> m_pan;
    quint64 m_revision;
};

} // namespace AetherSDR
