#include "RemoteEngineMapping.h"

#include <QJsonArray>
#include <QSet>

#include <cmath>

namespace AetherSDR::aetherd {

namespace {

using Authority = SliceFrequencyControl::Authority;

Authority authorityFrom(const QJsonValue& value)
{
    const QString text = value.toString();
    if (text == QLatin1String("radio")) { return Authority::Radio; }
    if (text == QLatin1String("engine")) { return Authority::Engine; }
    return Authority::Unknown;
}

QStringList stringsFrom(const QJsonValue& value)
{
    QStringList out;
    for (const QJsonValue& entry : value.toArray()) {
        if (entry.isString()) { out.append(entry.toString()); }
    }
    return out;
}

bool isInteger(const QJsonValue& value)
{
    const double number = value.toDouble();
    return value.isDouble() && std::isfinite(number) && std::floor(number) == number;
}

std::optional<bool> observedBool(const QJsonObject& observation)
{
    if (!observation.value(QStringLiteral("known")).toBool()) { return std::nullopt; }
    const QJsonValue value = observation.value(QStringLiteral("value"));
    return value.isBool() ? std::optional<bool>(value.toBool()) : std::nullopt;
}

} // namespace

std::optional<int> canonicalSliceId(const QString& id)
{
    bool ok = false;
    const int value = id.toInt(&ok);
    if (!ok || value < 0 || QString::number(value) != id) { return std::nullopt; }
    return value;
}

QString resourceKey(const QString& type, const QString& radioSession, const QString& id)
{
    return type + QLatin1Char('/') + radioSession + QLatin1Char('/') + id;
}

QString resourceKey(const QJsonObject& address)
{
    return resourceKey(address.value(QStringLiteral("type")).toString(),
                       address.value(QStringLiteral("radioSession")).toString(),
                       address.value(QStringLiteral("id")).toString());
}

SliceDelta sliceDeltaFrom(const QJsonObject& slice, const QStringList& modes)
{
    SliceDelta d;
    if (slice.contains(QStringLiteral("panadapterId"))) {
        d.panId = slice.value(QStringLiteral("panadapterId")).toString();
    }
    if (slice.contains(QStringLiteral("letter"))) {
        d.letter = slice.value(QStringLiteral("letter")).toString();
    }
    if (isInteger(slice.value(QStringLiteral("frequencyHz")))) {
        d.frequency = slice.value(QStringLiteral("frequencyHz")).toDouble() / 1.0e6;
    }
    if (slice.value(QStringLiteral("mode")).isString()) {
        d.mode = slice.value(QStringLiteral("mode")).toString();
    }
    if (!modes.isEmpty()) { d.modeList = modes; }
    const QJsonObject filter = slice.value(QStringLiteral("filter")).toObject();
    if (isInteger(filter.value(QStringLiteral("lowHz"))) && isInteger(filter.value(QStringLiteral("highHz")))) {
        d.filterLow = filter.value(QStringLiteral("lowHz")).toInt();
        d.filterHigh = filter.value(QStringLiteral("highHz")).toInt();
    }
    if (slice.value(QStringLiteral("active")).isBool()) { d.active = slice.value(QStringLiteral("active")).toBool(); }
    if (slice.value(QStringLiteral("locked")).isBool()) { d.locked = slice.value(QStringLiteral("locked")).toBool(); }
    d.txSlice = false;
    const QJsonObject audio = slice.value(QStringLiteral("audio")).toObject();
    if (audio.value(QStringLiteral("gain")).isDouble()) { d.audioGain = audio.value(QStringLiteral("gain")).toDouble(); }
    if (audio.value(QStringLiteral("muted")).isBool()) { d.audioMute = audio.value(QStringLiteral("muted")).toBool(); }
    if (isInteger(audio.value(QStringLiteral("pan")))) { d.audioPan = audio.value(QStringLiteral("pan")).toInt(); }
    const QJsonObject receive = slice.value(QStringLiteral("receive")).toObject();
    if (receive.value(QStringLiteral("antenna")).isString()) {
        d.rxAntenna = receive.value(QStringLiteral("antenna")).toString();
    }
    const QJsonObject agc = receive.value(QStringLiteral("agc")).toObject();
    if (agc.value(QStringLiteral("mode")).isString()) { d.agcMode = agc.value(QStringLiteral("mode")).toString(); }
    if (isInteger(agc.value(QStringLiteral("threshold")))) { d.agcThreshold = agc.value(QStringLiteral("threshold")).toInt(); }
    if (isInteger(agc.value(QStringLiteral("offLevel")))) { d.agcOffLevel = agc.value(QStringLiteral("offLevel")).toInt(); }
    const QJsonObject squelch = receive.value(QStringLiteral("squelch")).toObject();
    if (squelch.value(QStringLiteral("enabled")).isBool()) { d.squelchOn = squelch.value(QStringLiteral("enabled")).toBool(); }
    if (isInteger(squelch.value(QStringLiteral("level")))) { d.squelchLevel = squelch.value(QStringLiteral("level")).toInt(); }
    const QJsonObject observed = slice.value(QStringLiteral("receiveObservation")).toObject();
    d.nb = observedBool(observed.value(QStringLiteral("noiseBlanker")).toObject());
    d.nr = observedBool(observed.value(QStringLiteral("noiseReduction")).toObject());
    const QJsonObject rit = observed.value(QStringLiteral("rit")).toObject();
    if (rit.value(QStringLiteral("known")).toBool() && rit.value(QStringLiteral("enabled")).isBool()
        && isInteger(rit.value(QStringLiteral("offsetHz")))) {
        d.ritOn = rit.value(QStringLiteral("enabled")).toBool();
        d.ritFreq = rit.value(QStringLiteral("offsetHz")).toInt();
    }
    return d;
}

std::optional<PanState> panStateFrom(const QJsonObject& panadapter)
{
    const QJsonValue center = panadapter.value(QStringLiteral("centerHz"));
    const QJsonValue bandwidth = panadapter.value(QStringLiteral("bandwidthHz"));
    if (!isInteger(center) || !isInteger(bandwidth) || bandwidth.toDouble() <= 0.0) {
        return std::nullopt;
    }
    PanState state;
    state.centerMhz = center.toDouble() / 1.0e6;
    state.bandwidthMhz = bandwidth.toDouble() / 1.0e6;
    const QJsonValue gain = panadapter.value(QStringLiteral("receive")).toObject().value(QStringLiteral("rfGain"));
    if (isInteger(gain)) { state.rfGain = gain.toInt(); }
    const QJsonObject limits = panadapter.value(QStringLiteral("bandwidthLimitsHz")).toObject();
    if (isInteger(limits.value(QStringLiteral("minimum"))) && isInteger(limits.value(QStringLiteral("maximum")))
        && limits.value(QStringLiteral("maximum")).toDouble() > 0.0) {
        state.minimumBandwidthMhz = limits.value(QStringLiteral("minimum")).toDouble() / 1.0e6;
        state.maximumBandwidthMhz = limits.value(QStringLiteral("maximum")).toDouble() / 1.0e6;
    }
    return state;
}

RadioCapabilities capabilitiesFrom(const QJsonObject& radioSession, const QString& agcMode)
{
    RadioCapabilities c;
    c.family = QStringLiteral("aetherd");
    const QJsonObject identity = radioSession.value(QStringLiteral("identity")).toObject();
    c.model = identity.value(QStringLiteral("model")).toString();
    c.manufacturer = identity.value(QStringLiteral("manufacturer")).toString();
    // Receive-only by construction (Principle VI): nothing the engine says
    // makes this family able to key.
    c.canTransmit = false;
    c.txPowerMaxWatts = 0.0;
    c.hasFmRepeaterOffset = false;
    c.hasCwTune = false;
    c.canCreateSlices = false;
    // Empty: the remote engine owns all operating state (rule 1).
    c.clientSettingsDomains = {};

    const QJsonObject caps = radioSession.value(QStringLiteral("capabilities")).toObject();
    c.maxSlices = std::max(1, caps.value(QStringLiteral("maxSlices")).toInt(1));
    c.maxPanadapters = std::max(1, caps.value(QStringLiteral("maxPanadapters")).toInt(1));
    for (const QJsonValue& rate : caps.value(QStringLiteral("sampleRatesHz")).toArray()) {
        if (isInteger(rate) && rate.toInt() > 0) { c.sampleRatesHz.append(rate.toInt()); }
    }
    const QJsonObject tuning = caps.value(QStringLiteral("tuningRangeHz")).toObject();
    c.tuningMinHz = tuning.value(QStringLiteral("minimum")).toDouble();
    c.tuningMaxHz = tuning.value(QStringLiteral("maximum")).toDouble();
    for (const QJsonValue& band : caps.value(QStringLiteral("declaredBands")).toArray()) {
        const QJsonObject b = band.toObject();
        c.declaredBandRanges.append({b.value(QStringLiteral("name")).toString(),
                                     b.value(QStringLiteral("lowHz")).toDouble(),
                                     b.value(QStringLiteral("highHz")).toDouble()});
    }

    const QJsonObject frequency = caps.value(QStringLiteral("sliceFrequencyControl")).toObject();
    c.sliceFrequencyControl = {authorityFrom(frequency.value(QStringLiteral("authority"))),
                               qint64(frequency.value(QStringLiteral("minimumHz")).toDouble()),
                               qint64(frequency.value(QStringLiteral("maximumHz")).toDouble())};
    if (const QJsonObject mode = caps.value(QStringLiteral("receiveModeControl")).toObject(); !mode.isEmpty()) {
        c.receiveModeControl = ReceiveModeControl{authorityFrom(mode.value(QStringLiteral("authority"))),
                                                  stringsFrom(mode.value(QStringLiteral("modes")))};
    }
    if (const QJsonObject filter = caps.value(QStringLiteral("receiveFilterControl")).toObject(); !filter.isEmpty()) {
        ReceiveFilterControl record{authorityFrom(filter.value(QStringLiteral("authority"))), {}};
        for (const QJsonValue& entry : filter.value(QStringLiteral("modes")).toArray()) {
            const QJsonObject m = entry.toObject();
            record.modes.append({m.value(QStringLiteral("mode")).toString(),
                                 m.value(QStringLiteral("minimumLowHz")).toInt(),
                                 m.value(QStringLiteral("maximumLowHz")).toInt(),
                                 m.value(QStringLiteral("minimumHighHz")).toInt(),
                                 m.value(QStringLiteral("maximumHighHz")).toInt(),
                                 m.value(QStringLiteral("minimumWidthHz")).toInt(),
                                 m.value(QStringLiteral("maximumWidthHz")).toInt()});
        }
        c.receiveFilterControl = record;
    }
    if (const QJsonObject audio = caps.value(QStringLiteral("receiveAudioControl")).toObject(); !audio.isEmpty()) {
        c.receiveAudioControl = ReceiveAudioControl{authorityFrom(audio.value(QStringLiteral("authority")))};
    }
    const auto range = [&caps](const char* name) -> std::optional<ReceivePanRangeControl> {
        const QJsonObject record = caps.value(QLatin1String(name)).toObject();
        if (record.isEmpty()) { return std::nullopt; }
        ReceivePanRangeControl control;
        control.authority = authorityFrom(record.value(QStringLiteral("authority")));
        control.minimumHz = qint64(record.value(QStringLiteral("minimumHz")).toDouble());
        control.maximumHz = qint64(record.value(QStringLiteral("maximumHz")).toDouble());
        return control;
    };
    c.receivePanCenterControl = range("receivePanCenterControl");
    c.receivePanBandwidthControl = range("receivePanBandwidthControl");
    if (const QJsonObject pitch = caps.value(QStringLiteral("receiveCwPitchControl")).toObject(); !pitch.isEmpty()) {
        c.receiveCwPitchControl = ReceiveCwPitchControl{authorityFrom(pitch.value(QStringLiteral("authority"))),
            pitch.value(QStringLiteral("minimumHz")).toInt(100), pitch.value(QStringLiteral("maximumHz")).toInt(6000),
            pitch.value(QStringLiteral("stepHz")).toInt(10)};
        c.cwPitchMinHz = c.receiveCwPitchControl->minimumHz;
        c.cwPitchMaxHz = c.receiveCwPitchControl->maximumHz;
        c.cwPitchStepHz = c.receiveCwPitchControl->stepHz;
    }
    if (const QJsonObject gain = caps.value(QStringLiteral("receivePanRfGainControl")).toObject(); !gain.isEmpty()) {
        c.receivePanRfGainControl = ReceivePanRfGainControl{authorityFrom(gain.value(QStringLiteral("authority"))),
            gain.value(QStringLiteral("minimumDb")).toInt(), gain.value(QStringLiteral("maximumDb")).toInt(),
            std::max(1, gain.value(QStringLiteral("stepDb")).toInt(1))};
    }
    if (const QJsonObject speaker = caps.value(QStringLiteral("radioSpeakerControl")).toObject(); !speaker.isEmpty()) {
        c.radioSpeakerControl = RadioSpeakerControl{authorityFrom(speaker.value(QStringLiteral("authority")))};
    }
    if (const QJsonObject dsp = caps.value(QStringLiteral("receiveDspControl")).toObject(); !dsp.isEmpty()) {
        using Feature = ReceiveDspControl::Feature;
        ReceiveDspControl record;
        record.authority = authorityFrom(dsp.value(QStringLiteral("authority")));
        record.maximumRitOffsetHz = dsp.value(QStringLiteral("maximumRitOffsetHz")).toInt();
        const QJsonObject features = dsp.value(QStringLiteral("features")).toObject();
        const std::pair<const char*, Feature> named[] = {
            {"agcThreshold", Feature::Agc}, {"noiseBlanker", Feature::Nb},
            {"noiseReduction", Feature::Nr}, {"rit", Feature::Rit}};
        QSet<QString> modes;
        for (const auto& [name, feature] : named) {
            const QStringList featureModes = stringsFrom(features.value(QLatin1String(name)));
            if (featureModes.isEmpty()) { continue; }
            record.features.append(feature);
            record.featureModes.insert(feature, featureModes);
            for (const QString& mode : featureModes) { modes.insert(mode); }
        }
        record.modes = QStringList(modes.begin(), modes.end());
        record.modes.sort();
        c.receiveDspControl = record;
        c.hasAgcThreshold = record.features.contains(Feature::Agc);
    }
    // v1 has no AGC-mode method, so the selectors offer only what is running.
    c.agcModes = agcMode.isEmpty() ? QStringList{} : QStringList{agcMode.toLower()};
    return c;
}

} // namespace AetherSDR::aetherd
