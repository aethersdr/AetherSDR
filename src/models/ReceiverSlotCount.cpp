#include "models/ReceiverSlotCount.h"

#include "core/backends/RadioCapabilities.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <algorithm>

namespace AetherSDR {

ReceiverSlotCount::ReceiverSlotCount(RadioModel* radio, QObject* parent)
    : QObject(parent)
    , m_radio(radio)
{
    if (!m_radio) {
        return;
    }
    // capabilitiesChanged fires on every connect/disconnect edge AND on any
    // mid-session revision the backend announces, so it alone would carry the
    // HL2's post-connect ceiling. The other edges are here because a Flex
    // revises RadioModel's own count from `slices=N` status (infoChanged), and
    // because the floor moves when a receiver is added or removed.
    connect(m_radio, &RadioModel::capabilitiesChanged, this,
            [this](bool, const RadioCapabilities&) { refresh(); });
    connect(m_radio, &RadioModel::connectionStateChanged, this,
            [this](bool) { refresh(); });
    connect(m_radio, &RadioModel::infoChanged, this, &ReceiverSlotCount::refresh);
    connect(m_radio, &RadioModel::sliceAdded, this,
            [this](SliceModel*) { refresh(); });
    connect(m_radio, &RadioModel::sliceRemoved, this,
            [this](int) { refresh(); });
    refresh();
}

int ReceiverSlotCount::forCeiling(int declaredCeiling, const QList<SliceModel*>& slices)
{
    int letters = std::max(declaredCeiling, 0);
    for (const SliceModel* slice : slices) {
        if (slice) {
            letters = std::max(letters, slice->sliceId() + 1);
        }
    }
    return letters;
}

void ReceiverSlotCount::refresh()
{
    const int next = m_radio->isConnected()
        ? forCeiling(m_radio->maxSlices(), m_radio->slices())
        : 0;
    if (next == m_count) {
        return;
    }
    m_count = next;
    emit countChanged(m_count);
}

} // namespace AetherSDR
