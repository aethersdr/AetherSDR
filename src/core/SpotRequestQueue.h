#pragma once

#include "core/backends/RadioControlTypes.h"
#include "core/backends/SpotRequest.h"

#include <QList>
#include <algorithm>

namespace AetherSDR {

struct QueuedSpotRequest {
    SpotRequest request;
    RadioControlSession session;
    quint64 epoch;
};

// Detach before dispatch: callbacks may clear, replace the session, or enqueue.
class SpotRequestQueue {
public:
    void append(const SpotRequest& request, const RadioControlSession& session, quint64 epoch)
    { m_pending.append({request, session, epoch}); }
    void clear() { m_pending.clear(); }
    qsizetype size() const { return m_pending.size(); }
    QList<QueuedSpotRequest> takeBatch(int limit, const RadioControlSession& session, quint64 epoch)
    {
        m_pending.removeIf([&](const QueuedSpotRequest& entry) {
            return entry.session != session || entry.epoch != epoch;
        });
        const qsizetype count = std::min(m_pending.size(), qsizetype(std::max(0, limit)));
        const QList<QueuedSpotRequest> batch = m_pending.first(count);
        m_pending.remove(0, count);
        return batch;
    }
private:
    QList<QueuedSpotRequest> m_pending;
};

} // namespace AetherSDR
