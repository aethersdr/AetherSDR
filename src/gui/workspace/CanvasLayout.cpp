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

bool CanvasLayout::addItem(CanvasItem item, const QSize& canvas)
{
    if (item.id.isEmpty() || contains(item.id)) {
        return false;
    }

    // A new item lands on top and inside.  Assigning z here rather than
    // trusting the caller keeps the dense-contiguous invariant true from the
    // first insert.
    item.z    = static_cast<int>(m_items.size());
    item.rect = clampToCanvas(item.rect, item.minimumSize, canvas);

    m_items.append(item);
    return true;
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

bool CanvasLayout::setRect(const QString& id, const NormRect& rect, const QSize& canvas)
{
    CanvasItem* it = find(id);
    if (!it) {
        return false;
    }
    it->rect = clampToCanvas(rect, it->minimumSize, canvas);
    return true;
}

QStringList CanvasLayout::reclampAll(const QSize& canvas)
{
    QStringList moved;
    for (CanvasItem& it : m_items) {
        const NormRect before = it.rect;
        it.rect = clampToCanvas(it.rect, it.minimumSize, canvas);
        if (!(it.rect == before)) {
            moved.append(it.id);
        }
    }
    return moved;
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
    // the whole operation.  Already-topmost is a no-op, not a failure — the
    // caller asked for a state that already holds.
    const int target = it->z + 1;
    for (CanvasItem& other : m_items) {
        if (other.z == target) {
            other.z = it->z;
            it->z   = target;
            break;
        }
    }
    return true;
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
            break;
        }
    }
    return true;
}

bool CanvasLayout::bringToFront(const QString& id)
{
    CanvasItem* it = find(id);
    if (!it) {
        return false;
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

    it->z = -1;
    normalizeZ();
    return true;
}

}  // namespace AetherSDR
