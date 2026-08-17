#pragma once

#include <QObject>
#include <QString>
#include <QTimer>

namespace AetherSDR {

// Returns one named, direct-child single-shot timer for an owner.  Callers use
// newlyCreated to install the signal path exactly once when an object is wired
// more than once during placeholder-to-live pan replacement.
struct OwnedSingleShotTimer {
    QTimer* timer{nullptr};
    bool newlyCreated{false};
};

inline OwnedSingleShotTimer ensureOwnedSingleShotTimer(QObject* owner,
                                                        const QString& objectName,
                                                        int intervalMs)
{
    Q_ASSERT(owner != nullptr);
    Q_ASSERT(!objectName.isEmpty());

    QTimer* timer = owner->findChild<QTimer*>(objectName, Qt::FindDirectChildrenOnly);
    if (timer != nullptr) {
        return {timer, false};
    }

    timer = new QTimer(owner);
    timer->setObjectName(objectName);
    timer->setSingleShot(true);
    timer->setInterval(intervalMs);
    return {timer, true};
}

} // namespace AetherSDR
