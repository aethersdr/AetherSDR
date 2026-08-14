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

// The extension document's domain sub-keys (PR #4614 review): the engine
// gates the extension PER DOMAIN by copying only the sub-object named for a
// declared domain. The CONTENTS of each sub-object stay opaque here — the
// owning backend writes and validates them (Principle VII). Memories is
// deliberately NOT in this vocabulary: the #4590 bank re-targets its own
// feature documents (one domain, one document — PR 6).
constexpr const char* kExtRfGainKey = "rfGain";
constexpr const char* kExtTxSetpointsKey = "txSetpoints";

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
        // A newer binary wrote this document. The fields we know are still
        // readable; the document itself is protected because store() below
        // REFUSES to overwrite a newer version — this binary never rewrites
        // (and thereby truncates or downgrades) a document it cannot fully
        // represent (PR #4614 review).
        qWarning() << "RadioStateMemory: operating-state document schema"
                   << storedVersion << "is newer than" << kSchemaVersion
                   << "— reading known fields only; capture is disabled for"
                      " this radio";
    }

    // Universal fields, gated per declared domain — an undeclared domain is
    // "not restored" even when the stored document carries it.
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
    if (has(caps, Domain::Agc)) {
        state.agcMode = doc.value(QStringLiteral("agcMode")).toString();
        // -1 (not 0) when the key is absent: 0 is a selectable AGC-T, so the
        // default must sit outside the control's range — see RestoredRadioState.
        state.agcThreshold =
            doc.value(QStringLiteral("agcThreshold")).toInt(-1);
    }

    // The extension is gated per domain too: only a declared domain's
    // sub-object is handed over, so a narrowed declaration cannot smuggle
    // another domain's data to the backend (PR #4614 review — previously the
    // whole blob rode on any one of the ext domains).
    const QJsonObject storedExt = doc.value(QStringLiteral("ext")).toObject();
    QJsonObject gatedExt;
    if (has(caps, Domain::RfGain)
        && storedExt.contains(QLatin1String(kExtRfGainKey))) {
        gatedExt.insert(QLatin1String(kExtRfGainKey),
                        storedExt.value(QLatin1String(kExtRfGainKey)));
    }
    if (has(caps, Domain::TxSetpoints)
        && storedExt.contains(QLatin1String(kExtTxSetpointsKey))) {
        gatedExt.insert(QLatin1String(kExtTxSetpointsKey),
                        storedExt.value(QLatin1String(kExtTxSetpointsKey)));
    }
    if (!gatedExt.isEmpty()) {
        state.extension = gatedExt;
        state.extensionSchemaVersion =
            doc.value(QStringLiteral("extVersion")).toInt();
    }
    return state;
}

bool store(const RadioSettingsScope& scope, const RadioCapabilities& caps,
           const RestoredRadioState& state)
{
    if (!shouldEngage(caps) || !scope.isValid()) {
        qDebug() << "RadioStateMemory: store skipped — not engaged for this"
                    " radio (empty domain declaration or invalid scope)";
        return false;
    }

    // Read-only toward the future: never overwrite a document written by a
    // newer schema — a v1 rebuild would drop the fields v1 doesn't know and
    // stamp the version back down (PR #4614 review, three independent
    // reports). Mirrors SettingsDatabase's own newer-schema rule.
    // EXACT row only: the guard judges the document this write would
    // replace. The fallback read would smuggle in the family-wide row's
    // version and could refuse every per-radio capture in the family
    // (PR #4614 review, Ozy311).
    int storedVersion = 0;
    scope.featureExact(featureName(), &storedVersion);
    if (storedVersion > kSchemaVersion) {
        qWarning() << "RadioStateMemory: refusing to overwrite operating-state"
                      " document at schema" << storedVersion
                   << "with schema" << kSchemaVersion
                   << "— capture is read-only toward newer documents";
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
    if (has(caps, Domain::Agc)) {
        if (!state.agcMode.isEmpty()) {
            doc.insert(QStringLiteral("agcMode"), state.agcMode);
        }
        // >= 0, so a threshold of 0 IS written — the sentinel is -1.
        if (state.agcThreshold >= 0) {
            doc.insert(QStringLiteral("agcThreshold"), state.agcThreshold);
        }
    }

    // Extension: same per-domain sub-object gate as load().
    QJsonObject gatedExt;
    if (has(caps, Domain::RfGain)
        && state.extension.contains(QLatin1String(kExtRfGainKey))) {
        gatedExt.insert(QLatin1String(kExtRfGainKey),
                        state.extension.value(QLatin1String(kExtRfGainKey)));
    }
    if (has(caps, Domain::TxSetpoints)
        && state.extension.contains(QLatin1String(kExtTxSetpointsKey))) {
        gatedExt.insert(QLatin1String(kExtTxSetpointsKey),
                        state.extension.value(QLatin1String(kExtTxSetpointsKey)));
    }
    if (!gatedExt.isEmpty()) {
        doc.insert(QStringLiteral("ext"), gatedExt);
        doc.insert(QStringLiteral("extVersion"), state.extensionSchemaVersion);
    }

    if (doc.isEmpty()) {
        qDebug() << "RadioStateMemory: store skipped — nothing declared AND"
                    " present to write";
        return false;   // a deliberate no-op, distinct in the log from failure
    }
    if (!scope.setFeature(featureName(), kSchemaVersion, doc)) {
        qWarning() << "RadioStateMemory: operating-state write failed for"
                   << scope.family() << scope.radioId();
        return false;
    }
    return true;
}

} // namespace RadioStateMemory
} // namespace AetherSDR
