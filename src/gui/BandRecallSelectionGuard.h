#pragma once

#include <QHash>
#include <QString>
#include <QtGlobal>

#include <algorithm>
#include <optional>

namespace AetherSDR {

// Owns the window during which radio-driven active-slice selection is treated
// as synchronization-only (see BandRecallSliceSelectionPolicy.h for what that
// decision is; this decides WHEN it applies).
//
// Deliberately separate from the Kiwi/Center Lock band-recall markers, which
// share a fixed grace timer keyed on band-recall *intent*. Two properties this
// window needs and that one cannot give it:
//
//   * It must not open when the band write never reached the wire. The
//     dispatch signal fires immediately BEFORE sendCommand() so the KiwiSDR
//     mute handoff can bracket the write, so a dropped write (foreign-owned
//     pan, dead WAN session, profile-load hold backstop) would otherwise
//     suppress selection for a reconstruction that is not happening.
//     arm()/cancelArm() are an exact pair for that.
//   * A fixed window is a guess at how long the radio takes to rebuild a pan's
//     slices. Over SmartLink/WAN the rebuild can outrun it, and one late
//     transient old-band active=1 after expiry is enough to recentre the pan
//     back onto the old band — the exact failure this guards. refresh() pushes
//     the window out on each piece of evidence that the rebuild is still
//     running, bounded by the hard cap the arming recall set so a chatty
//     session cannot hold the window open indefinitely.
//
// Pure policy — no timers, no widgets, no clock. The caller injects nowMs and
// owns every side effect. Unit-tested in band_recall_selection_guard_test.
class BandRecallSelectionGuard {
public:
    BandRecallSelectionGuard(int windowMs, int maxWindowMs)
        : m_windowMs(windowMs)
        , m_maxWindowMs(std::max(windowMs, maxWindowMs))
    {
    }

    // A `display pan set <pan> band=` write is about to reach the wire. Opens
    // the window, or extends one an earlier recall left live. Remembers what it
    // replaced so cancelArm() can undo exactly this call.
    void arm(const QString& panId, qint64 nowMs)
    {
        if (panId.isEmpty()) {
            return;
        }
        prune(nowMs);

        m_armedPanId = panId;
        const auto existing = m_windows.constFind(panId);
        m_armReplaced = (existing != m_windows.constEnd())
            ? std::optional<Window>(*existing)
            : std::nullopt;

        Window window;
        window.untilMs = nowMs + m_windowMs;
        window.hardUntilMs = nowMs + m_maxWindowMs;
        if (m_armReplaced) {
            // An overlapping recall never shortens the window the previous one
            // is still owed — the older rebuild may outlast the newer request.
            window.untilMs = std::max(window.untilMs, m_armReplaced->untilMs);
            window.hardUntilMs =
                std::max(window.hardUntilMs, m_armReplaced->hardUntilMs);
        }
        m_windows.insert(panId, window);
    }

    // The write was not dispatched. Restores the state the matching arm()
    // replaced — which is NOT the same as removing the window: an earlier
    // successful recall on this pan may still be rebuilding, and its window
    // must survive a later failed request.
    void cancelArm(const QString& panId)
    {
        if (panId.isEmpty() || m_armedPanId != panId) {
            return;
        }
        if (m_armReplaced) {
            m_windows.insert(panId, *m_armReplaced);
        } else {
            m_windows.remove(panId);
        }
        m_armedPanId.clear();
        m_armReplaced.reset();
    }

    // Evidence the radio is still rebuilding this pan (a slice was recreated,
    // or a transient activation was just suppressed). No-op once the window has
    // closed — a refresh can extend a live window, never reopen a closed one.
    void refresh(const QString& panId, qint64 nowMs)
    {
        auto it = m_windows.find(panId);
        if (it == m_windows.end()) {
            return;
        }
        if (nowMs >= it->untilMs) {
            m_windows.erase(it);
            return;
        }
        it->untilMs = extended(*it, nowMs);
    }

    // A live slice removal is the same evidence, but arrives without a pan id:
    // the slice has already left the model, so the caller cannot say which pan
    // it was on. Refreshing every live window is the safe read — suppression
    // only withholds a pan recentre and an active=1 echo, each window is still
    // bounded by its own hard cap, and a pan with no live window is untouched.
    void refreshAll(qint64 nowMs)
    {
        for (auto it = m_windows.begin(); it != m_windows.end();) {
            if (nowMs >= it->untilMs) {
                it = m_windows.erase(it);
            } else {
                it->untilMs = extended(*it, nowMs);
                ++it;
            }
        }
    }

    bool isActive(const QString& panId, qint64 nowMs) const
    {
        const auto it = m_windows.constFind(panId);
        return it != m_windows.constEnd() && nowMs < it->untilMs;
    }

private:
    struct Window {
        qint64 untilMs{0};
        qint64 hardUntilMs{0};
    };

    // nowMs is wall-clock (QDateTime::currentMSecsSinceEpoch), so an NTP step
    // backwards must not shorten a window that is already open — extending is
    // monotonic, and the hard cap is the only thing that bounds it.
    qint64 extended(const Window& window, qint64 nowMs) const
    {
        return std::max(window.untilMs,
                        std::min(window.hardUntilMs, nowMs + m_windowMs));
    }

    void prune(qint64 nowMs)
    {
        for (auto it = m_windows.begin(); it != m_windows.end();) {
            if (nowMs >= it->untilMs) {
                it = m_windows.erase(it);
            } else {
                ++it;
            }
        }
    }

    QHash<QString, Window> m_windows;
    QString                m_armedPanId;
    std::optional<Window>  m_armReplaced;
    int                    m_windowMs{0};
    int                    m_maxWindowMs{0};
};

}  // namespace AetherSDR
