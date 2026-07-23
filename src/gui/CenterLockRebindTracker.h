#pragma once

#include <QHash>
#include <QString>
#include <QVector>
#include <QtGlobal>

namespace AetherSDR {

// Resolves Center Lock after the intentional slice teardown/rebuild performed
// by a FLEX band-stack recall. MainWindow owns persistence, the grace timer,
// and UI side effects; this class owns the generation-safe identity policy.
class CenterLockRebindTracker {
public:
    struct SliceIdentity {
        QString panId;
        int sliceId{-1};
        QString letter;
        bool owned{false};
    };

    struct Resolution {
        enum class Kind {
            NoAction,
            Restore,
            ReleaseMissing,
            ReleaseAmbiguous,
        };

        Kind kind{Kind::NoAction};
        int sliceId{-1};
    };

    quint64 noteBandRecall(const QString& panId, int sliceId,
                           const QString& letter, int ownedSliceCount)
    {
        if (panId.isEmpty()) {
            return 0;
        }

        const quint64 generation = ++m_generation;
        if (sliceId >= 0) {
            PendingRecall pending;
            pending.sliceId = sliceId;
            pending.letter = letter.trimmed().toUpper();
            pending.ownedSliceCount = qMax(1, ownedSliceCount);
            pending.generation = generation;
            m_pending.insert(panId, pending);
            return generation;
        }

        // A second band selection can arrive while the first recall has the
        // locked slice temporarily absent. Carry the original identity into a
        // new generation so the older timer cannot resolve the newer recall.
        auto it = m_pending.find(panId);
        if (it == m_pending.end()) {
            return 0;
        }
        it->generation = generation;
        return generation;
    }

    bool onSliceRemoved(const QString& panId, int sliceId)
    {
        auto it = m_pending.find(panId);
        if (it == m_pending.end() || it->sliceId != sliceId) {
            return false;
        }

        it->removed = true;
        return true;
    }

    Resolution resolveAfterGrace(const QString& panId, quint64 generation,
                                 const QVector<SliceIdentity>& slices)
    {
        auto it = m_pending.find(panId);
        if (it == m_pending.end() || it->generation != generation) {
            return {};
        }

        const PendingRecall pending = *it;
        m_pending.erase(it);
        if (!pending.removed) {
            return {};
        }

        QVector<SliceIdentity> candidates;
        for (const SliceIdentity& slice : slices) {
            if (slice.owned && slice.panId == panId && slice.sliceId >= 0) {
                candidates.append(slice);
            }
        }

        if (candidates.isEmpty()) {
            return {Resolution::Kind::ReleaseMissing, -1};
        }

        // The common one-slice case has an unambiguous user-visible receiver
        // even when FLEX assigns a different numeric id or per-client letter.
        if (pending.ownedSliceCount == 1 && candidates.size() == 1) {
            return {Resolution::Kind::Restore, candidates.first().sliceId};
        }

        // If either topology contains multiple slices, id or letter alone can
        // be recycled during the rebuild. Require both to identify one result.
        int matchingSliceId = -1;
        int matchCount = 0;
        for (const SliceIdentity& candidate : candidates) {
            if (candidate.sliceId == pending.sliceId
                && candidate.letter.trimmed().compare(
                       pending.letter, Qt::CaseInsensitive) == 0) {
                matchingSliceId = candidate.sliceId;
                ++matchCount;
            }
        }
        if (matchCount == 1) {
            return {Resolution::Kind::Restore, matchingSliceId};
        }

        return {Resolution::Kind::ReleaseAmbiguous, -1};
    }

    bool cancel(const QString& panId)
    {
        return m_pending.remove(panId) > 0;
    }

    bool hasPending(const QString& panId) const
    {
        return m_pending.contains(panId);
    }

private:
    struct PendingRecall {
        int sliceId{-1};
        QString letter;
        int ownedSliceCount{0};
        quint64 generation{0};
        bool removed{false};
    };

    QHash<QString, PendingRecall> m_pending;
    quint64 m_generation{0};
};

}  // namespace AetherSDR
