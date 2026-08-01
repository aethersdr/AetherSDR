#include "TciTrxMap.h"

#include "TciProtocol.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <algorithm>

namespace AetherSDR {

int TciTrxMap::acquire(int sliceId)
{
    const auto it = m_trxBySliceId.constFind(sliceId);
    if (it != m_trxBySliceId.constEnd())
        return it.value();

    // Lowest free trx keeps the wire indexes dense from 0.
    int trx = 0;
    while (std::find(m_trxBySliceId.cbegin(), m_trxBySliceId.cend(), trx)
           != m_trxBySliceId.cend()) {
        ++trx;
    }
    m_trxBySliceId.insert(sliceId, trx);
    return trx;
}

void TciTrxMap::release(int sliceId)
{
    m_trxBySliceId.remove(sliceId);
}

void TciTrxMap::clear()
{
    m_trxBySliceId.clear();
}

int TciTrxMap::trxForSlice(RadioModel* model, const SliceModel* slice) const
{
    if (!slice)
        return TciProtocol::tciTrxForSlice(model, slice);
    const auto it = m_trxBySliceId.constFind(slice->sliceId());
    if (it != m_trxBySliceId.constEnd())
        return it.value();
    return TciProtocol::tciTrxForSlice(model, slice);
}

SliceModel* TciTrxMap::sliceForTrx(RadioModel* model, int trx) const
{
    if (model) {
        for (auto it = m_trxBySliceId.cbegin(); it != m_trxBySliceId.cend(); ++it) {
            if (it.value() != trx)
                continue;
            if (SliceModel* s = model->slice(it.key()))
                return s;
            // Bound but not live (band-change settle window). The binding is
            // held because this slice is expected back; answering positionally
            // would route an inbound command to whichever slice happens to sit
            // at that index — the very re-pointing #4567 is about, inside the
            // one window a band-changing client is most likely to send
            // commands. Refuse until the recreate reclaims the number or the
            // deferred release frees it; callers already treat a null slice
            // as "unknown receiver" and drop the command (#4577 review).
            return nullptr;
        }
    }
    return TciProtocol::resolveSliceForTrx(model, trx);
}

int TciTrxMap::txSliceTrxOrNone(RadioModel* model) const
{
    if (!model)
        return -1;
    for (SliceModel* slice : model->slices()) {
        if (slice && slice->isTxSlice())
            return trxForSlice(model, slice);
    }
    return -1;
}

bool TciTrxMap::trxHasLiveSlice(RadioModel* model, int trx) const
{
    if (!model)
        return false;
    for (auto* s : model->slices()) {
        if (s && trxForSlice(model, s) == trx)
            return true;
    }
    return false;
}

int TciTrxMap::trxCount(RadioModel* model) const
{
    int highest = -1;
    for (int trx : m_trxBySliceId)
        highest = std::max(highest, trx);
    if (model) {
        for (auto* s : model->slices())
            highest = std::max(highest, trxForSlice(model, s));
    }
    return std::max(highest + 1, 1);
}

} // namespace AetherSDR
