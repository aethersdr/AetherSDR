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
    if (m_setupObservationLifetime) { m_setupObservationLifetime->retire(); }
    for (const XvtrInfo& value : m_xvtrList) { if (value.lifetime) { value.lifetime->retire(); } }
    for (const UsbCable& value : m_usbCableModel.cables()) { if (value.lifetime) { value.lifetime->retire(); } }
}

void RadioModel::resetSetupObservations()
{
    retireSetupObservations();
    const quint64 generation = m_setupObservationGeneration;
    m_xvtrList.clear();
    m_licenseSubscriptions.clear();
    m_licenseExpiryTimer.stop();
    m_licenseRadioId.clear();
    m_licenseMaxVersion.clear();
    m_licenseSubscription.clear();
    m_licenseExpirationDate.clear();
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

void RadioModel::refreshLicenseSubscription(const QDateTime& now)
{
    m_licenseSubscription = QStringLiteral("SmartSDR");
    m_licenseExpirationDate.clear();
    for (const QString& name : {QStringLiteral("smartsdr+_early_access"), QStringLiteral("smartsdr+")}) {
        const QDateTime expiration = m_licenseSubscriptions.value(name);
        if (!expiration.isValid() || expiration <= now) { continue; }
        m_licenseSubscription = name.endsWith(QLatin1String("early_access"))
            ? QStringLiteral("SmartSDR+ Early Access") : QStringLiteral("SmartSDR+");
        m_licenseExpirationDate = expiration.date().toString(QStringLiteral("MM/dd/yyyy"));
        return;
    }
}

void RadioModel::applySetupObservation(const SetupStatusObservation& observation)
{
    if (!observation.current()) { return; }
    m_setupObservationLifetime = observation.lifetime;
    const quint64 generation = m_setupObservationGeneration;
    const QPointer<RadioModel> alive(this);
    const auto current = [&] {
        return alive && observation.current() && generation == alive->m_setupObservationGeneration;
    };
    switch (observation.kind) {
    case SetupStatusObservation::Kind::Transverter: {
        const auto& value = observation.transverter;
        if (value.id < 0) { return; }
        if (!value.removed && observation.entityLifetime && !observation.entityLifetime->current()) { return; }
        auto it = m_xvtrList.find(value.id);
        if (value.removed) {
            if (it != m_xvtrList.end() && (!observation.entityLifetime || it->lifetime == observation.entityLifetime)) {
                if (it->lifetime) { it->lifetime->retire(); }
                m_xvtrList.erase(it);
            }
        } else {
            if (it == m_xvtrList.end() || (observation.entityLifetime && it->lifetime != observation.entityLifetime)) {
                if (it == m_xvtrList.end() && m_xvtrList.size() >= 256) { return; }
                if (it != m_xvtrList.end() && it->lifetime) { it->lifetime->retire(); }
                XvtrInfo fresh;
                fresh.index = value.id;
                fresh.lifetime = observation.entityLifetime ? observation.entityLifetime : std::make_shared<SetupStatusLifetime>();
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
        m_usbCableModel.applyObservation(observation.usbCable, observation.entityLifetime);
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
            refreshLicenseSubscription(QDateTime::currentDateTime());
            break;
        case LicenseStatusValues::Kind::Subscription:
            if (!value.expiration.isValid() || (value.name != QLatin1String("smartsdr+")
                && value.name != QLatin1String("smartsdr+_early_access"))) { return; }
            m_licenseSubscriptions.insert(value.name, value.expiration);
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
