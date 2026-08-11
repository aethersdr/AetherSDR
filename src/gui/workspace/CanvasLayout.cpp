#include "gui/workspace/CanvasLayout.h"

#include <algorithm>

namespace AetherSDR {

CanvasItem* CanvasLayout::find(const QString& id)
{
    for (CanvasItem& it : m_items) {
        if (it.id == id) {
            return &it;
        }
    }
    return nullptr;
}

const CanvasItem* CanvasLayout::find(const QString& id) const
{
    for (const CanvasItem& it : m_items) {
        if (it.id == id) {
            return &it;
        }
    }
    return nullptr;
}

void CanvasLayout::normalizeZ()
{
    // Sort a view of the items by (z, insertion index) and hand out 0..n-1.
    // The insertion-index tie-break is what makes this deterministic when two
    // items arrive holding the same z — otherwise the result would depend on
    // std::sort's unspecified handling of equal keys.
    QList<int> order;
    order.reserve(m_items.size());
    for (int i = 0; i < m_items.size(); ++i) {
        order.append(i);
    }

    std::stable_sort(order.begin(), order.end(), [this](int a, int b) {
        return m_items[a].z < m_items[b].z;
    });

    for (int rank = 0; rank < order.size(); ++rank) {
        m_items[order[rank]].z = rank;
    }
}

bool CanvasLayout::addItem(CanvasItem item)
{
    if (item.id.isEmpty() || contains(item.id)) {
        return false;
    }

    // A new item lands on top and inside the unit square.  Assigning z here
    // rather than trusting the caller keeps the dense-contiguous invariant
    // true from the first insert, with no normalize pass needed.  Bounds
    // only — the display clamp handles minimum sizes at render time.
    item.z    = static_cast<int>(m_items.size());
    item.rect = clampToBounds(item.rect);

    m_items.append(item);
    return true;
}

int CanvasLayout::restoreItems(const QList<CanvasItem>& items)
{
    // Sort by stored z FIRST, then insert bottom-to-top, so each insert's
    // assigned rank matches the saved order.  Doing it the other way — insert
    // then normalise — loses the ordering, because normalising collapses the
    // stored z values that the next insert would have been compared against.
    QList<CanvasItem> ordered = items;
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const CanvasItem& a, const CanvasItem& b) {
                         return a.z < b.z;
                     });

    int inserted = 0;
    for (CanvasItem& item : ordered) {
        if (addItem(item)) {
            ++inserted;
        }
    }
    return inserted;
}

bool CanvasLayout::removeItem(const QString& id)
{
    for (int i = 0; i < m_items.size(); ++i) {
        if (m_items[i].id == id) {
            m_items.removeAt(i);
            normalizeZ();   // close the hole the removal left in z
            return true;
        }
    }
    return false;
}

void CanvasLayout::clear()
{
    m_items.clear();
}

bool CanvasLayout::contains(const QString& id) const
{
    return find(id) != nullptr;
}

const CanvasItem* CanvasLayout::item(const QString& id) const
{
    return find(id);
}

QStringList CanvasLayout::ids() const
{
    QStringList out;
    out.reserve(m_items.size());
    for (const CanvasItem& it : m_items) {
        out.append(it.id);
    }
    return out;
}

QList<CanvasItem> CanvasLayout::itemsByZ() const
{
    QList<CanvasItem> out = m_items;
    std::stable_sort(out.begin(), out.end(),
                     [](const CanvasItem& a, const CanvasItem& b) {
                         return a.z < b.z;
                     });
    return out;
}

bool CanvasLayout::setRect(const QString& id, const NormRect& rect)
{
    CanvasItem* it = find(id);
    if (!it) {
        return false;
    }
    it->rect = clampToBounds(rect);
    return true;
}

QString CanvasLayout::hitTest(const QPointF& normPoint) const
{
    const CanvasItem* best = nullptr;
    for (const CanvasItem& it : m_items) {
        if (!it.rect.contains(normPoint.x(), normPoint.y())) {
            continue;
        }
        if (!best || it.z > best->z) {
            best = &it;
        }
    }
    return best ? best->id : QString();
}

int CanvasLayout::zOf(const QString& id) const
{
    const CanvasItem* it = find(id);
    return it ? it->z : -1;
}

bool CanvasLayout::raise(const QString& id)
{
    CanvasItem* it = find(id);
    if (!it) {
        return false;
    }

    // Dense z means the neighbour above is exactly z + 1; swapping with it is
    // the whole operation.
    const int target = it->z + 1;
    for (CanvasItem& other : m_items) {
        if (other.z == target) {
            other.z = it->z;
            it->z   = target;
            return true;
        }
    }
    return false;   // already topmost — nothing changed
}

bool CanvasLayout::lower(const QString& id)
{
    CanvasItem* it = find(id);
    if (!it) {
        return false;
    }

    const int target = it->z - 1;
    for (CanvasItem& other : m_items) {
        if (other.z == target) {
            other.z = it->z;
            it->z   = target;
            return true;
        }
    }
    return false;   // already backmost — nothing changed
}

bool CanvasLayout::bringToFront(const QString& id)
{
    CanvasItem* it = find(id);
    if (!it) {
        return false;
    }
    if (it->z == m_items.size() - 1) {
        return false;   // already frontmost — nothing changed
    }

    // Park it above everything, then re-densify.  Doing it this way rather
    // than shuffling each item down keeps the relative order of everything
    // else exactly as it was.
    it->z = m_items.size();
    normalizeZ();
    return true;
}

bool CanvasLayout::sendToBack(const QString& id)
{
    CanvasItem* it = find(id);
    if (!it) {
        return false;
    }
    if (it->z == 0) {
        return false;   // already backmost — nothing changed
    }

    it->z = -1;
    normalizeZ();
    return true;
}

}  // namespace AetherSDR
