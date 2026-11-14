#include "RadioModel.h"

#include <QPointer>

namespace AetherSDR {
namespace {
template<typename T> void assign(T& target, const std::optional<T>& value)
{
    if (value) { target = *value; }
}
}

void RadioModel::retireSetupObservations()
{
    ++m_setupObservationGeneration;
    for (quint64& revision : m_filterSharpnessRevisions) { ++revision; }
    for (const XvtrInfo& value : m_xvtrList) { if (value.lifetime) { value.lifetime->retire(); } }
    for (const UsbCable& value : m_usbCableModel.cables()) { if (value.lifetime) { value.lifetime->retire(); } }
}

void RadioModel::resetSetupObservations()
{
    retireSetupObservations();
    const quint64 generation = m_setupObservationGeneration;
    m_xvtrList.clear();
    m_licenseSubscriptionLabel.clear();
    m_licenseExpiryTimer.stop();
    m_licenseRadioId.clear();
    m_licenseMaxVersion.clear();
    m_licenseSubscription = {};
    m_filterVoice = m_filterCw = m_filterDigital = 2;
    m_filterVoiceAuto = false;
    m_filterCwAuto = m_filterDigitalAuto = true;
    const bool hadFeatures = !m_licenseFeatures.isEmpty();
    m_licenseFeatures.clear();
    const QPointer<RadioModel> alive(this);
    m_usbCableModel.clear();
    if (!alive || generation != m_setupObservationGeneration) { return; }
    if (hadFeatures) { emit licenseFeaturesChanged(); }
    if (!alive || generation != m_setupObservationGeneration) { return; }
    emit infoChanged();
}

bool RadioModel::refreshLicenseSubscription(const QDateTime& now)
{
    const QString label = m_licenseSubscription.label(now);
    const bool changed = m_licenseSubscriptionLabel != label;
    m_licenseSubscriptionLabel = label;
    return changed;
}

void RadioModel::applySetupObservation(const SetupStatusObservation& observation)
{
    if (!observationSessionCurrent(observation.statusEpoch)) { return; }
    const quint64 generation = m_setupObservationGeneration;
    const QPointer<RadioModel> alive(this);
    const auto current = [&] {
        return alive && generation == alive->m_setupObservationGeneration
            && alive->observationSessionCurrent(observation.statusEpoch);
    };
    switch (observation.kind) {
    case SetupStatusObservation::Kind::Transverter: {
        const auto& value = observation.transverter;
        if (value.id < 0) { return; }
        auto it = m_xvtrList.find(value.id);
        if (value.removed) {
            if (it == m_xvtrList.end()) { return; }
            if (it->lifetime) { it->lifetime->retire(); }
            m_xvtrList.erase(it);
        } else {
            if (it == m_xvtrList.end()) {
                if (m_xvtrList.size() >= 256) { return; }
                XvtrInfo fresh;
                fresh.index = value.id;
                fresh.lifetime = std::make_shared<SetupStatusLifetime>();
                it = m_xvtrList.insert(value.id, fresh);
            }
            assign(it->order, value.order);
            assign(it->name, value.name);
            assign(it->rfFreq, value.rfFreq);
            assign(it->ifFreq, value.ifFreq);
            assign(it->loError, value.loError);
            assign(it->rxGain, value.rxGain);
            assign(it->maxPower, value.maxPower);
            assign(it->rxOnly, value.rxOnly);
            if (value.isValid) { it->isValid = *value.isValid; it->hasIsValid = true; }
        }
        break;
    }
    case SetupStatusObservation::Kind::UsbCable:
        m_usbCableModel.applyObservation(observation.usbCable);
        return;
    case SetupStatusObservation::Kind::FilterSharpness: {
        const auto& value = observation.sharpness;
        const size_t index = static_cast<size_t>(value.mode) * 2;
        if (index + 1 >= m_filterSharpnessRevisions.size()) { return; }
        if (value.level) { ++m_filterSharpnessRevisions[index]; }
        if (value.automatic) { ++m_filterSharpnessRevisions[index + 1]; }
        switch (value.mode) {
        case FilterSharpnessStatusValues::Mode::Voice:
            assign(m_filterVoice, value.level); assign(m_filterVoiceAuto, value.automatic); break;
        case FilterSharpnessStatusValues::Mode::Cw:
            assign(m_filterCw, value.level); assign(m_filterCwAuto, value.automatic); break;
        case FilterSharpnessStatusValues::Mode::Digital:
            assign(m_filterDigital, value.level); assign(m_filterDigitalAuto, value.automatic); break;
        }
        break;
    }
    case SetupStatusObservation::Kind::License: {
        const auto& value = observation.license;
        switch (value.kind) {
        case LicenseStatusValues::Kind::Base:
            assign(m_licenseRadioId, value.radioId);
            assign(m_licenseMaxVersion, value.maximumVersion);
            if (m_licenseSubscription.name.isEmpty()) { m_licenseSubscription.name = QStringLiteral("SmartSDR"); }
            refreshLicenseSubscription(QDateTime::currentDateTime());
            break;
        case LicenseStatusValues::Kind::Subscription:
            if (!value.expiration.isValid()) { return; }
            m_licenseSubscription = mergeLicenseSubscription(m_licenseSubscription, value.name, value.expiration);
            refreshLicenseSubscription(QDateTime::currentDateTime());
            m_licenseExpiryTimer.start();
            break;
        case LicenseStatusValues::Kind::Feature: {
            if (value.name.isEmpty() || value.name.size() > 128 || value.reason.size() > 256) { return; }
            if (!m_licenseFeatures.contains(value.name) && m_licenseFeatures.size() >= 128) { return; }
            const LicenseFeatureState next{true, value.enabled, value.reason};
            const LicenseFeatureState previous = m_licenseFeatures.value(value.name);
            if (!previous.seen || previous.enabled != next.enabled || previous.reason != next.reason) {
                m_licenseFeatures.insert(value.name, next);
                emit licenseFeaturesChanged();
                if (!current()) { return; }
            }
            // A repeated authoritative grant also clears an intervening command refusal.
            const LicenseFeatureState latest = m_licenseFeatures.value(value.name);
            if (next.enabled && latest.seen && latest.enabled && latest.reason == next.reason
                && value.name == DvkModel::kLicenseFeature) { m_dvkModel.clearRefusal(); }
            break;
        }
        }
        break;
    }
    }
    if (current()) { emit infoChanged(); }
}

} // namespace AetherSDR
