#include "RadioModel.h"

#include "core/LogManager.h"
#include <QPointer>
#include <QScopeGuard>

namespace AetherSDR {
namespace {
constexpr qsizetype kMaxDeferredDisplays = 128;
constexpr qsizetype kMaxDisplayInventory = 256;
constexpr qint64 kDeferredDisplayAgeMs = 30'000;
constexpr int kDefaultDisplayDimension = 100;
}

bool RadioModel::displayWaterfallOwned(const QString& id, const QString& parent) const
{
    if (m_displayInventoryUncertain) { return false; }
    const auto observed = m_radioDisplayWaterfalls.constFind(id);
    return observed == m_radioDisplayWaterfalls.cend()
        || ((!observed->ownerKnown || (clientHandle() != 0 && observed->clientHandle == clientHandle()))
            && (observed->parentPanId.isEmpty() || observed->parentPanId == parent));
}

void RadioModel::onDisplayObservation(const DisplayObservation& observation)
{
    if (!observation.current()) { return; }
    if (m_displayObservationQueue.size() >= kMaxDeferredDisplays) {
        // Never discard ownership evidence and then authorize a write from the
        // resulting absence. Reconnect is required after an incomplete census.
        m_displayInventoryUncertain = true;
        m_displayObservationQueue.clear();
        ++m_displayHistoryEpoch;
        qCWarning(lcProtocol) << "Display observation queue exhausted; controls held until reconnect";
        return;
    }
    m_displayObservationQueue.append(observation);
    if (m_applyingDisplayObservation) { return; }
    m_applyingDisplayObservation = true;
    const QPointer<RadioModel> self(this);
    const auto finish = qScopeGuard([self] {
        if (self) { self->m_applyingDisplayObservation = false; }
    });
    // Serializing only reentrant reports keeps an old removal notification from
    // deleting a same-id replacement created by another notification handler.
    while (self && !m_displayObservationQueue.isEmpty()) {
        const DisplayObservation next = m_displayObservationQueue.takeFirst();
        if (next.current()) { applyDisplayObservation(next); }
    }
}

void RadioModel::deferDisplayObservation(const DisplayObservation& observation)
{
    if (!m_displayObservationClock.isValid()) { m_displayObservationClock.start(); }
    const qint64 now = m_displayObservationClock.elapsed();
    for (auto it = m_pendingDisplayObservations.begin(); it != m_pendingDisplayObservations.end();) {
        if (!it->observation.current() || now - it->insertedMs > kDeferredDisplayAgeMs) {
            it = m_pendingDisplayObservations.erase(it);
        } else { ++it; }
    }
    auto it = m_pendingDisplayObservations.find(observation.id);
    if (it == m_pendingDisplayObservations.end()) {
        if (m_pendingDisplayObservations.size() >= kMaxDeferredDisplays) { return; }
        m_pendingDisplayObservations.insert(observation.id, {now, observation});
        return;
    }
    if (it->observation.transportEpoch != observation.transportEpoch) {
        *it = {now, observation};
        return;
    }
    it->observation.values.merge(observation.values);
    if (observation.ownerHandle) { it->observation.ownerHandle = observation.ownerHandle; }
    if (observation.pairedId) { it->observation.pairedId = observation.pairedId; }
}

DisplayObservation RadioModel::takeDeferredDisplayObservation(const DisplayObservation& observation)
{
    const PendingDisplayObservation pending = m_pendingDisplayObservations.take(observation.id);
    DisplayObservation result = observation;
    if (pending.observation.current()
        && pending.observation.transportEpoch == observation.transportEpoch
        && m_displayObservationClock.isValid()
        && m_displayObservationClock.elapsed() - pending.insertedMs <= kDeferredDisplayAgeMs) {
        result.values = pending.observation.values;
        result.values.merge(observation.values);
        if (!result.pairedId) { result.pairedId = pending.observation.pairedId; }
    }
    return result;
}

bool RadioModel::applyDisplayValues(PanadapterModel* pan, const DisplayObservation& observation)
{
    const QPointer<RadioModel> self(this);
    const QPointer<PanadapterModel> original(pan);
    const quint64 session = m_sessionGeneration;
    const quint64 generation = m_backendReceiverGeneration;
    const quint64 ownership = pan->ownershipRevision();
    const QString waterfall = pan->waterfallId();
    const auto current = [&] {
        return self && original && observation.current() && session == m_sessionGeneration
            && generation == m_backendReceiverGeneration && m_panadapters.value(pan->panId()) == original
            && pan->ownershipRevision() == ownership && pan->ownedByClient(clientHandle())
            && pan->waterfallId() == waterfall;
    };
    const float beforeMin = pan->minDbm();
    const float beforeMax = pan->maxDbm();
    if (!pan->applyDisplayValues(observation.values, current) || !current()) { return false; }
    const DisplayValues& v = observation.values;
    if (v.antennas && m_antList != *v.antennas) {
        m_antList = *v.antennas;
        emit antListChanged(m_antList);
        if (!current()) { return false; }
    }
    if (v.centerMhz || v.bandwidthMhz) {
        emit panadapterInfoChanged(pan->centerMhz(), pan->bandwidthMhz());
        if (!current()) { return false; }
    }
    if (v.minDbm || v.maxDbm) {
        emit panadapterLevelChanged(pan->minDbm(), pan->maxDbm());
        if (!current()) { return false; }
    }
    if (v.yPixels && *v.yPixels > kDefaultDisplayDimension && pan->setFftYPixels(*v.yPixels)) {
        emit panadapterFftScaleChanged(pan->panId(), *v.yPixels);
        if (!current()) { return false; }
    }
    syncDisplayStream(pan, beforeMin != pan->minDbm() || beforeMax != pan->maxDbm(),
                      v.yPixels && *v.yPixels > kDefaultDisplayDimension);
    const auto tiny = [](const std::optional<int>& size) {
        return size && *size > 0 && *size <= kDefaultDisplayDimension;
    };
    if (tiny(v.xPixels) || tiny(v.yPixels)) {
        emit panDimensionsNeeded(pan->panId());
        if (!current()) { return false; }
    }
    if (!pan->isResized() && isConnected()) {
        pan->setResized(true);
        configurePan(pan->panId());
        if (!current()) { return false; }
    }
    return true;
}

void RadioModel::applyDisplayObservation(const DisplayObservation& observation)
{
    if (!observation.current()) { return; }
    const QString id = observation.id;
    const bool isPan = observation.kind == DisplayObservation::Kind::Panadapter;
    const QPointer<RadioModel> self(this);
    const quint64 session = m_sessionGeneration;
    const quint64 generation = m_backendReceiverGeneration;
    const auto current = [&] {
        return self && observation.current() && session == m_sessionGeneration
            && generation == m_backendReceiverGeneration;
    };
    if (observation.removed) {
        retireDisplayResource(id);
        m_pendingDisplayObservations.remove(id);
        if (isPan) {
            m_radioDisplayPans.remove(id);
            retireDisplayPan(id);
        } else {
            m_radioDisplayWaterfalls.remove(id);
            QList<QPointer<PanadapterModel>> pans;
            for (PanadapterModel* pan : m_panadapters) { pans.append(pan); }
            for (const QPointer<PanadapterModel>& pan : pans) {
                if (!current()) { return; }
                if (pan && m_panadapters.value(pan->panId()) == pan && pan->waterfallId() == id) {
                    replaceDisplayWaterfall(pan, {});
                }
            }
        }
        return;
    }
    if ((isPan && !m_radioDisplayPans.contains(id) && m_radioDisplayPans.size() >= kMaxDisplayInventory)
        || (!isPan && !m_radioDisplayWaterfalls.contains(id)
            && m_radioDisplayWaterfalls.size() >= kMaxDisplayInventory)) {
        m_displayInventoryUncertain = true;
        ++m_displayHistoryEpoch;
        return;
    }
    if (isPan) {
        auto& inventory = m_radioDisplayPans[id];
        if (observation.ownerHandle) {
            if (inventory.ownerKnown && inventory.clientHandle != *observation.ownerHandle) {
                retireDisplayResource(id);
            }
            inventory.clientHandle = *observation.ownerHandle;
            inventory.ownerKnown = true;
        }
        if (observation.pairedId) {
            inventory.waterfallId = *observation.pairedId;
            auto child = m_radioDisplayWaterfalls.find(*observation.pairedId);
            if (child != m_radioDisplayWaterfalls.end() && child->parentPanId.isEmpty()) {
                child->parentPanId = id;
            }
        }
        const bool ownerKnown = inventory.ownerKnown;
        const quint32 owner = inventory.clientHandle;
        // Preamp is shared antenna hardware, including reports on foreign pans.
        if (observation.values.preamp) {
            QList<QPointer<PanadapterModel>> pans;
            for (PanadapterModel* pan : m_panadapters) { pans.append(pan); }
            for (const QPointer<PanadapterModel>& pan : pans) {
                if (!current()) { return; }
                if (pan && m_panadapters.value(pan->panId()) == pan) {
                    pan->setPreamp(*observation.values.preamp);
                }
            }
            if (!current()) { return; }
        }
        if (!ownerKnown) { deferDisplayObservation(observation); return; }
        if (owner == 0 || owner != clientHandle()) {
            m_pendingDisplayObservations.remove(id);
            if (m_stalePanadapters.contains(id)) { retireDisplayPan(id); return; }
            if (PanadapterModel* pan = m_panadapters.value(id)) {
                voidPendingPanWrites(id, QStringLiteral("pan ownership lost"));
                if (!current() || m_panadapters.value(id) != pan) { return; }
                const QPointer<PanadapterModel> held(pan);
                pan->setOwnerHandle(owner);
                if (current() && held && m_panadapters.value(id) == held) { syncDisplayStream(held); }
            }
            return;
        }
        if (m_displayInventoryUncertain) { return; }
        if (observation.values.minDbm || observation.values.maxDbm) {
            noteForeignPanWriteIfAny(id, observation.sourceHandle);
            if (!current()) { return; }
        }
        const DisplayObservation merged = takeDeferredDisplayObservation(observation);
        QPointer<PanadapterModel> pan = m_panadapters.value(id);
        if (!pan) {
            ensureOwnedPanadapter(id, &merged);
            return;
        }
        pan->setOwnerHandle(owner);
        if (!current() || !pan || m_panadapters.value(id) != pan) { return; }
        if (merged.pairedId) {
            replaceDisplayWaterfall(pan, *merged.pairedId);
            if (!current() || !pan || m_panadapters.value(id) != pan) { return; }
        }
        if (!applyDisplayValues(pan, merged) || !current()) { return; }
        const PendingDisplayObservation pending = m_pendingDisplayObservations.take(pan->waterfallId());
        if (pending.observation.current() && m_displayObservationClock.isValid()
            && m_displayObservationClock.elapsed() - pending.insertedMs <= kDeferredDisplayAgeMs) {
            applyDisplayObservation(pending.observation);
        }
        return;
    }

    auto& inventory = m_radioDisplayWaterfalls[id];
    if (observation.ownerHandle) {
        if (inventory.ownerKnown && inventory.clientHandle != *observation.ownerHandle) {
            retireDisplayResource(id);
        }
        inventory.clientHandle = *observation.ownerHandle;
        inventory.ownerKnown = true;
    }
    if (observation.pairedId) {
        if (!inventory.parentPanId.isEmpty() && inventory.parentPanId != *observation.pairedId) {
            retireDisplayResource(id);
        }
        inventory.parentPanId = *observation.pairedId;
    } else if (inventory.parentPanId.isEmpty()) {
        for (auto it = m_radioDisplayPans.cbegin(); it != m_radioDisplayPans.cend(); ++it) {
            if (it->waterfallId == id) { inventory.parentPanId = it.key(); break; }
        }
    }
    const QString parent = inventory.parentPanId;
    const bool ownerKnown = inventory.ownerKnown;
    const bool foreign = ownerKnown
        && (inventory.clientHandle == 0 || inventory.clientHandle != clientHandle());
    QList<QPointer<PanadapterModel>> associated;
    for (PanadapterModel* held : m_panadapters) {
        if (held->waterfallId() == id && (foreign || held->panId() != parent)) {
            associated.append(held);
        }
    }
    for (const QPointer<PanadapterModel>& held : associated) {
        if (!current()) { return; }
        if (held && m_panadapters.value(held->panId()) == held) { replaceDisplayWaterfall(held, {}); }
    }
    if (!current()) { return; }
    QPointer<PanadapterModel> pan = m_panadapters.value(parent);
    if (foreign) {
        m_pendingDisplayObservations.remove(id);
        return;
    }
    if (!pan || !pan->ownedByClient(clientHandle()) || m_displayInventoryUncertain) {
        deferDisplayObservation(observation);
        return;
    }
    // The pan's confirmed child link admits handle-less incremental reports;
    // a waterfall's parent link alone never creates or claims a pan.
    const bool linked = pan->waterfallId() == id;
    if (!linked && (!ownerKnown || !pan->waterfallId().isEmpty()
                    || m_displayRetirements.contains(id) || m_displayRetirements.contains(parent))) {
        deferDisplayObservation(observation);
        return;
    }
    if (!linked) {
        replaceDisplayWaterfall(pan, id);
        if (!current() || !pan || m_panadapters.value(parent) != pan) { return; }
    }
    const DisplayObservation merged = takeDeferredDisplayObservation(observation);
    if (!applyDisplayValues(pan, merged) || !current()) { return; }
    updateStreamFilters();
    if (!pan->isWaterfallConfigured() && isConnected()) {
        pan->setWaterfallConfigured(true);
        configureWaterfall(id);
    }
}

} // namespace AetherSDR
