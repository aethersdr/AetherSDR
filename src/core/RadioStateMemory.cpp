#include "RadioStateMemory.h"

#include <QDebug>
#include <QJsonDocument>

namespace AetherSDR {
namespace RadioStateMemory {

namespace {

using Domain = RadioCapabilities::ClientSettingsDomain;

bool has(const RadioCapabilities& caps, Domain domain)
{
    return caps.clientSettingsDomains.testFlag(domain);
}

} // namespace

RestoredRadioState load(const RadioSettingsScope& scope,
                        const RadioCapabilities& caps)
{
    RestoredRadioState state;
    if (!shouldEngage(caps) || !scope.isValid()) {
        return state;
    }

    int storedVersion = 0;
    const QJsonObject doc = scope.feature(featureName(), &storedVersion);
    if (doc.isEmpty()) {
        return state;
    }
    if (storedVersion > kSchemaVersion) {
        // A newer binary wrote this document. Additive-schema policy says the
        // fields we know are still readable; unknown fields stay in `extension`
        // untouched because we never rewrite what we didn't read.
        qWarning() << "RadioStateMemory: operating-state document schema"
                   << storedVersion << "is newer than" << kSchemaVersion
                   << "— reading known fields only";
    }

    // Universal fields, gated per declared domain — an undeclared domain is
    // "not restored" even when an older document carries it.
    if (has(caps, Domain::Tuning)) {
        state.rfFrequencyHz = doc.value(QStringLiteral("rfFrequencyHz")).toDouble();
        state.mode = doc.value(QStringLiteral("mode")).toString();
    }
    if (has(caps, Domain::Passband)) {
        state.filterLowHz = doc.value(QStringLiteral("filterLowHz")).toDouble();
        state.filterHighHz = doc.value(QStringLiteral("filterHighHz")).toDouble();
    }
    if (has(caps, Domain::SpanRate)) {
        state.sampleRateHz = doc.value(QStringLiteral("sampleRateHz")).toInt();
    }

    // The per-family extension document is opaque here (Principle VII: the
    // owning backend validates it). RfGain/TxSetpoints per-band maps ride
    // inside it starting PR 3.
    if (has(caps, Domain::RfGain) || has(caps, Domain::TxSetpoints)
        || has(caps, Domain::Memories)) {
        state.extension = doc.value(QStringLiteral("ext")).toObject();
        state.extensionSchemaVersion =
            doc.value(QStringLiteral("extVersion")).toInt();
    }
    return state;
}

bool store(const RadioSettingsScope& scope, const RadioCapabilities& caps,
           const RestoredRadioState& state)
{
    if (!shouldEngage(caps) || !scope.isValid()) {
        return false;
    }

    QJsonObject doc;
    if (has(caps, Domain::Tuning)) {
        if (state.rfFrequencyHz > 0.0) {
            doc.insert(QStringLiteral("rfFrequencyHz"), state.rfFrequencyHz);
        }
        if (!state.mode.isEmpty()) {
            doc.insert(QStringLiteral("mode"), state.mode);
        }
    }
    if (has(caps, Domain::Passband)
        && (state.filterLowHz != 0.0 || state.filterHighHz != 0.0)) {
        doc.insert(QStringLiteral("filterLowHz"), state.filterLowHz);
        doc.insert(QStringLiteral("filterHighHz"), state.filterHighHz);
    }
    if (has(caps, Domain::SpanRate) && state.sampleRateHz > 0) {
        doc.insert(QStringLiteral("sampleRateHz"), state.sampleRateHz);
    }
    if ((has(caps, Domain::RfGain) || has(caps, Domain::TxSetpoints)
         || has(caps, Domain::Memories))
        && !state.extension.isEmpty()) {
        doc.insert(QStringLiteral("ext"), state.extension);
        doc.insert(QStringLiteral("extVersion"), state.extensionSchemaVersion);
    }

    if (doc.isEmpty()) {
        return false;   // nothing declared AND present — write nothing
    }
    return scope.setFeature(featureName(), kSchemaVersion, doc);
}

} // namespace RadioStateMemory
} // namespace AetherSDR
