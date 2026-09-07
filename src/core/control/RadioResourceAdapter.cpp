#include "RadioResourceAdapter.h"

#include "core/backends/RadioCapabilities.h"
#include "models/PanadapterModel.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QJsonArray>
#include <QtMath>

#include <utility>

namespace AetherSDR::control {
namespace {

QJsonArray strings(const auto& values)
{
    QJsonArray result;
    for (const auto& value : values) {
        result.append(value);
    }
    return result;
}

QJsonObject capabilityValue(const RadioCapabilities& capabilities)
{
    QJsonArray sampleRates;
    for (int sampleRate : capabilities.sampleRatesHz) {
        sampleRates.append(sampleRate);
    }
    QJsonArray bands;
    for (const DeclaredBandRange& band : capabilities.declaredBandRanges) {
        bands.append(QJsonObject{{QStringLiteral("name"), band.name},
                                 {QStringLiteral("lowHz"), band.lowHz},
                                 {QStringLiteral("highHz"), band.highHz}});
    }
    return {
        {QStringLiteral("maxSlices"), capabilities.maxSlices},
        {QStringLiteral("maxPanadapters"), capabilities.maxPanadapters},
        {QStringLiteral("sampleRatesHz"), sampleRates},
        {QStringLiteral("tuningRangeHz"), QJsonObject{
             {QStringLiteral("minimum"), capabilities.tuningMinHz},
             {QStringLiteral("maximum"), capabilities.tuningMaxHz}}},
        {QStringLiteral("declaredBands"), bands},
        {QStringLiteral("canTransmit"), capabilities.canTransmit},
        {QStringLiteral("maximumTransmitWatts"), capabilities.txPowerMaxWatts},
        {QStringLiteral("hasTuner"), capabilities.hasTuner},
        {QStringLiteral("hasAmplifier"), capabilities.hasAmplifier},
        {QStringLiteral("extensions"), strings(capabilities.extensionNamespaces)}
    };
}

} // namespace

RadioResourceAdapter::RadioResourceAdapter(
    RadioModel* radio, ControlResourceStore* resources,
    QString radioSessionId, QObject* parent)
    : QObject(parent),
      m_radio(radio),
      m_resources(resources),
      m_radioSessionId(std::move(radioSessionId))
{
    Q_ASSERT(m_radio);
    Q_ASSERT(m_resources);
    Q_ASSERT(!m_radioSessionId.isEmpty());

    connect(m_radio, &RadioModel::infoChanged,
            this, &RadioResourceAdapter::publishRadioSession);
    connect(m_radio, &RadioModel::connectionStateChanged,
            this, [this](bool connected) {
                publishRadioSession();
                if (connected) {
                    publishAll();
                } else {
                    clearDynamicResources();
                }
            });
    connect(m_radio, &RadioModel::capabilitiesChanged,
            this, [this] { publishRadioSession(); });
    connect(m_radio, &RadioModel::backendRebuilt,
            this, [this] { publishRadioSession(); });

    connect(m_radio, &RadioModel::sliceAdded,
            this, &RadioResourceAdapter::attachSlice);
    connect(m_radio, &RadioModel::sliceRemoved, this, [this](int sliceId) {
        // Match the object, not just the id. pruneStaleSessionModels() removes a
        // *stale* SliceModel whose id a live one may already have reclaimed; an
        // id-only match would unbind the live slice and publish a spurious
        // resource.removed for a resource that still exists.
        SliceModel* live = m_radio->slice(sliceId);
        SliceModel* removed = nullptr;
        for (SliceModel* slice : std::as_const(m_slices)) {
            if (slice && slice != live && slice->sliceId() == sliceId) {
                removed = slice;
                break;
            }
        }
        if (!removed) {
            return;
        }
        QObject::disconnect(removed, nullptr, this, nullptr);
        m_slices.remove(removed);
        m_resources->remove({QStringLiteral("slice"), m_radioSessionId,
                             QString::number(sliceId)});
    });
    connect(m_radio, &RadioModel::slotOccupancyChanged, this, [this](int sliceId) {
        refreshSlice(m_radio->slice(sliceId));
    });

    connect(m_radio, &RadioModel::panadapterAdded,
            this, &RadioResourceAdapter::attachPanadapter);
    connect(m_radio, &RadioModel::panadapterReclaimed,
            this, &RadioResourceAdapter::refreshPanadapter);
    connect(m_radio, &RadioModel::panadapterRemoved,
            this, [this](const QString& panId) {
                PanadapterModel* removed = nullptr;
                for (PanadapterModel* panadapter : std::as_const(m_panadapters)) {
                    if (panadapter && panadapter->panId() == panId) {
                        removed = panadapter;
                        break;
                    }
                }
                if (removed) {
                    QObject::disconnect(removed, nullptr, this, nullptr);
                }
                m_panadapters.remove(removed);
                m_resources->remove({QStringLiteral("panadapter"), m_radioSessionId,
                                     panId});
            });

    publishAll();
}

void RadioResourceAdapter::publishAll()
{
    publishRadioSession();
    for (SliceModel* slice : m_radio->slices()) {
        attachSlice(slice);
    }
    for (PanadapterModel* panadapter : m_radio->panadapters()) {
        attachPanadapter(panadapter);
    }
}

void RadioResourceAdapter::attachSlice(SliceModel* slice)
{
    if (!slice || m_slices.contains(slice)) {
        return;
    }
    m_slices.insert(slice);
    const auto refresh = [this, slice] { publishSlice(slice); };
    connect(slice, &SliceModel::letterChanged, this, refresh);
    connect(slice, &SliceModel::frequencyChanged, this, refresh);
    connect(slice, &SliceModel::panIdChanged, this, refresh);
    connect(slice, &SliceModel::modeChanged, this, refresh);
    connect(slice, &SliceModel::filterChanged, this, refresh);
    connect(slice, &SliceModel::activeChanged, this, refresh);
    connect(slice, &SliceModel::txSliceChanged, this, refresh);
    connect(slice, &SliceModel::audioGainChanged, this, refresh);
    connect(slice, &SliceModel::audioPanChanged, this, refresh);
    connect(slice, &SliceModel::audioMuteChanged, this, refresh);
    connect(slice, &SliceModel::rxAntennaChanged, this, refresh);
    connect(slice, &SliceModel::lockedChanged, this, refresh);
    connect(slice, &SliceModel::rfGainChanged, this, refresh);
    connect(slice, &SliceModel::agcModeChanged, this, refresh);
    connect(slice, &SliceModel::agcThresholdChanged, this, refresh);
    connect(slice, &SliceModel::agcOffLevelChanged, this, refresh);
    connect(slice, &SliceModel::squelchChanged, this, refresh);
    // While external receive-audio replacement is active (a Kiwi RX source
    // replacing the radio's audio) SliceModel's AGC and squelch setters take an
    // early-return branch that emits only these signals, but receiveAgcMode(),
    // receiveAgcThreshold(), receiveAgcOffLevel(), receiveSquelchOn() and
    // receiveSquelchLevel() switch to the external values. Without them
    // receive.agc.* and receive.squelch.* would publish the pre-replacement
    // state forever. (audio.* needs no equivalent: setAudioGain/Pan/Mute reuse
    // the same signal in both branches.)
    connect(slice, &SliceModel::externalReceiveAgcModeChanged, this, refresh);
    connect(slice, &SliceModel::externalReceiveAgcThresholdChanged, this, refresh);
    connect(slice, &SliceModel::externalReceiveAgcOffLevelChanged, this, refresh);
    connect(slice, &SliceModel::externalReceiveSquelchChanged, this, refresh);
    connect(slice, &QObject::destroyed, this, [this, slice] {
        m_slices.remove(slice);
    });
    publishSlice(slice);
}

void RadioResourceAdapter::refreshSlice(SliceModel* slice)
{
    if (!slice) {
        return;
    }
    if (!m_slices.contains(slice)) {
        attachSlice(slice);
        return;
    }
    publishSlice(slice);
}

void RadioResourceAdapter::attachPanadapter(PanadapterModel* panadapter)
{
    if (!panadapter || m_panadapters.contains(panadapter)) {
        return;
    }
    m_panadapters.insert(panadapter);
    const auto refresh = [this, panadapter] { publishPanadapter(panadapter); };
    connect(panadapter, &PanadapterModel::infoChanged, this, refresh);
    connect(panadapter, &PanadapterModel::levelChanged, this, refresh);
    connect(panadapter, &PanadapterModel::bandwidthLimitsChanged, this, refresh);
    connect(panadapter, &PanadapterModel::rxAntennaChanged, this, refresh);
    connect(panadapter, &PanadapterModel::rfGainChanged, this, refresh);
    connect(panadapter, &PanadapterModel::fpsChanged, this, refresh);
    connect(panadapter, &PanadapterModel::averageChanged, this, refresh);
    // The first valid report can be false, matching the model's default value.
    // Listen to Reported so weightedAverageKnown is published on that edge;
    // ControlResourceStore deduplicates later identical snapshots.
    connect(panadapter, &PanadapterModel::weightedAverageReported, this, refresh);
    connect(panadapter, &PanadapterModel::waterfallLineDurationChanged, this, refresh);
    connect(panadapter, &QObject::destroyed, this, [this, panadapter] {
        m_panadapters.remove(panadapter);
    });
    publishPanadapter(panadapter);
}

void RadioResourceAdapter::refreshPanadapter(PanadapterModel* panadapter)
{
    if (!panadapter) {
        return;
    }
    if (!m_panadapters.contains(panadapter)) {
        attachPanadapter(panadapter);
        return;
    }
    publishPanadapter(panadapter);
}

void RadioResourceAdapter::clearDynamicResources()
{
    const QSet<SliceModel*> slices = m_slices;
    m_slices.clear();
    for (SliceModel* slice : slices) {
        if (!slice) {
            continue;
        }
        QObject::disconnect(slice, nullptr, this, nullptr);
        m_resources->remove({QStringLiteral("slice"), m_radioSessionId,
                             QString::number(slice->sliceId())});
    }

    const QSet<PanadapterModel*> panadapters = m_panadapters;
    m_panadapters.clear();
    for (PanadapterModel* panadapter : panadapters) {
        if (!panadapter) {
            continue;
        }
        QObject::disconnect(panadapter, nullptr, this, nullptr);
        m_resources->remove({QStringLiteral("panadapter"), m_radioSessionId,
                             panadapter->panId()});
    }
}

void RadioResourceAdapter::publishRadioSession()
{
    const RadioCapabilities capabilities = m_radio->backendCapabilities();
    const QJsonObject value{
        {QStringLiteral("id"), m_radioSessionId},
        {QStringLiteral("connected"), m_radio->isConnected()},
        {QStringLiteral("family"), m_radio->family()},
        {QStringLiteral("identity"), QJsonObject{
             {QStringLiteral("name"), m_radio->name()},
             {QStringLiteral("model"), m_radio->model()},
             {QStringLiteral("serial"), m_radio->serial()},
             {QStringLiteral("version"), m_radio->version()},
             {QStringLiteral("manufacturer"), capabilities.manufacturer}}},
        {QStringLiteral("capabilities"), capabilityValue(capabilities)}};
    m_resources->upsert({QStringLiteral("radioSession"), {}, m_radioSessionId}, value);
}

void RadioResourceAdapter::publishSlice(SliceModel* slice)
{
    if (!slice || !m_slices.contains(slice)) {
        return;
    }
    const QJsonObject value{
        {QStringLiteral("id"), QString::number(slice->sliceId())},
        {QStringLiteral("letter"), slice->letter()},
        {QStringLiteral("panadapterId"), slice->panId()},
        {QStringLiteral("owned"), m_radio->isSlotOurs(slice->sliceId())},
        {QStringLiteral("frequencyHz"), qRound64(slice->frequency() * 1'000'000.0)},
        {QStringLiteral("mode"), slice->mode()},
        {QStringLiteral("filter"), QJsonObject{
             {QStringLiteral("lowHz"), slice->filterLow()},
             {QStringLiteral("highHz"), slice->filterHigh()}}},
        {QStringLiteral("active"), slice->isActive()},
        {QStringLiteral("txSlice"), slice->isTxSlice()},
        {QStringLiteral("locked"), slice->isLocked()},
        {QStringLiteral("audio"), QJsonObject{
             {QStringLiteral("gain"), slice->audioGain()},
             {QStringLiteral("pan"), slice->audioPan()},
             {QStringLiteral("muted"), slice->audioMute()}}},
        {QStringLiteral("receive"), QJsonObject{
             {QStringLiteral("antenna"), slice->rxAntenna()},
             {QStringLiteral("rfGain"), slice->rfGain()},
             {QStringLiteral("agc"), QJsonObject{
                  {QStringLiteral("mode"), slice->receiveAgcMode()},
                  {QStringLiteral("threshold"), slice->receiveAgcThreshold()},
                  {QStringLiteral("offLevel"), slice->receiveAgcOffLevel()}}},
             {QStringLiteral("squelch"), QJsonObject{
                  {QStringLiteral("enabled"), slice->receiveSquelchOn()},
                  {QStringLiteral("level"), slice->receiveSquelchLevel()}}}}}};
    m_resources->upsert({QStringLiteral("slice"), m_radioSessionId,
                         QString::number(slice->sliceId())}, value);
}

void RadioResourceAdapter::publishPanadapter(PanadapterModel* panadapter)
{
    if (!panadapter || !m_panadapters.contains(panadapter)) {
        return;
    }
    const QJsonObject value{
        {QStringLiteral("id"), panadapter->panId()},
        {QStringLiteral("centerHz"), qRound64(panadapter->centerMhz() * 1'000'000.0)},
        {QStringLiteral("centerKnown"), panadapter->centerKnown()},
        {QStringLiteral("bandwidthHz"),
         qRound64(panadapter->bandwidthMhz() * 1'000'000.0)},
        {QStringLiteral("dbmRange"), QJsonObject{
             {QStringLiteral("minimum"), panadapter->minDbm()},
             {QStringLiteral("maximum"), panadapter->maxDbm()}}},
        {QStringLiteral("bandwidthLimitsHz"), QJsonObject{
             {QStringLiteral("minimum"),
              qRound64(panadapter->minBandwidthMhz() * 1'000'000.0)},
             {QStringLiteral("maximum"),
              qRound64(panadapter->maxBandwidthMhz() * 1'000'000.0)}}},
        {QStringLiteral("receive"), QJsonObject{
             {QStringLiteral("antenna"), panadapter->rxAntenna()},
             {QStringLiteral("rfGain"), panadapter->rfGain()}}},
        {QStringLiteral("displayCadence"), QJsonObject{
             {QStringLiteral("fps"), panadapter->fps()},
             {QStringLiteral("averageFrames"), panadapter->average()},
             {QStringLiteral("weightedAverage"), panadapter->weightedAverage()},
             {QStringLiteral("weightedAverageKnown"),
              panadapter->weightedAverageKnown()},
             {QStringLiteral("waterfallRate"),
              panadapter->waterfallLineDuration()}}}};
    m_resources->upsert({QStringLiteral("panadapter"), m_radioSessionId,
                         panadapter->panId()}, value);
}

} // namespace AetherSDR::control
