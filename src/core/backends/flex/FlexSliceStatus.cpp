#include "FlexSliceStatus.h"
#include "FlexKvCarry.h"

#include <QRegularExpression>
#include <algorithm>
#include <cmath>

namespace AetherSDR {
namespace {
QStringList uniqueCommaList(const QString& value)
{
    QStringList result;
    const QStringList parts = value.split(',', Qt::SkipEmptyParts);
    for (const QString& part : parts) {
        const QString item = part.trimmed();
        if (!item.isEmpty() && !result.contains(item)) {
            result.append(item);
        }
    }
    return result;
}

} // namespace
using namespace flexkv;

bool FlexSliceStatus::isSlice(const QString& object)
{
    static const QRegularExpression pattern(QStringLiteral(R"(^slice\s+\d+$)"));
    return object.size() <= 64 && pattern.match(object).hasMatch();
}

std::optional<SliceObservation> FlexSliceStatus::decode(
    const QString& object, const QMap<QString, QString>& fields)
{
    if (!isSlice(object) || fields.size() > 256) { return {}; }
    bool valid = false;
    const int id = object.section(QRegularExpression(QStringLiteral(R"(\s+)")), 1, 1).toInt(&valid);
    if (!valid || id < 0) { return {}; }
    qsizetype size = 0;
    for (auto it = fields.cbegin(); it != fields.cend(); ++it) {
        size += it.key().size() + it.value().size();
        if (size > 65536) { return {}; }
    }
    SliceObservation observation;
    observation.sliceId = id;
    observation.values = decodeValues(fields);
    if (fields.value(QStringLiteral("removed")) == QLatin1String("1")) {
        observation.values.inUse = false;
    }
    observation.ownerHandle = parseUInt(fields, "client_handle", 16);
    carry(fields, "mode_list", observation.reportedModeList);
    carry(fields, "max_internal_pa_power", observation.internalPaPower);
    return observation;
}

SliceDelta FlexSliceStatus::decodeValues(const QMap<QString, QString>& kvs)
{
    SliceDelta d;

    // Identity / tuning
    carry(kvs, "pan", d.panId);
    carry(kvs, "index_letter", d.letter);
    carry(kvs, "RF_frequency", d.frequency);
    carry(kvs, "mode", d.mode);
    if (kvs.contains(QStringLiteral("chain_mode"))) {
        const QString value = kvs.value(QStringLiteral("chain_mode"));
        // FlexLib 4.3.16 Slice.ChainMode is independent of the selected mode.
        // Keep future chain names, but bound this diagnostic token at ingress.
        const bool valid = value.size() <= 32
            && std::all_of(value.cbegin(), value.cend(), [](QChar ch) {
                return ch.unicode() >= 0x21 && ch.unicode() <= 0x7e;
            });
        d.chainMode = valid ? value : QString();
    }
    carry(kvs, "filter_lo", d.filterLow);
    carry(kvs, "filter_hi", d.filterHigh);
    if (kvs.contains(QStringLiteral("mode_list"))) {
        d.modeList = uniqueCommaList(kvs.value(QStringLiteral("mode_list")));
    }

    // Core state
    carry(kvs, "active", d.active);
    carry(kvs, "tx", d.txSlice);
    carry(kvs, "rfgain", d.rfGain);
    carry(kvs, "audio_level", d.audioGain);
    carry(kvs, "audio_pan", d.audioPan);
    carry(kvs, "audio_mute", d.audioMute);
    if (kvs.value(QStringLiteral("in_use")) == QLatin1String("1")) { d.inUse = true; }
    else if (kvs.value(QStringLiteral("in_use")) == QLatin1String("0")) { d.inUse = false; }
    carry(kvs, "lock", d.locked);
    carry(kvs, "qsk", d.qsk);

    // Diversity group
    carry(kvs, "diversity_child", d.diversityChild);
    carry(kvs, "diversity_parent", d.diversityParent);
    carry(kvs, "diversity", d.diversity);
    carry(kvs, "diversity_index", d.diversityIndex);

    // ESC (diversity beamforming — "1"/"on" → true)
    if (kvs.contains(QStringLiteral("esc"))) {
        const QString v = kvs.value(QStringLiteral("esc"));
        d.esc = v == QLatin1String("1") || v == QLatin1String("on");
    }
    carry(kvs, "esc_gain", d.escGain);
    carry(kvs, "esc_phase_shift", d.escPhaseShift);

    // Antennas (rx_ant_list takes precedence over ant_list, then split+trim)
    if (kvs.contains(QStringLiteral("rx_ant_list")) || kvs.contains(QStringLiteral("ant_list"))) {
        d.rxAntennaList = splitList(kvs.value(QStringLiteral("rx_ant_list"),
                                              kvs.value(QStringLiteral("ant_list"))));
    }
    if (kvs.contains(QStringLiteral("tx_ant_list"))) {
        d.txAntennaList = splitList(kvs.value(QStringLiteral("tx_ant_list")));
    }
    carry(kvs, "rxant", d.rxAntenna);
    carry(kvs, "txant", d.txAntenna);

    // DSP toggles
    carry(kvs, "nb", d.nb);
    carry(kvs, "nr", d.nr);
    carry(kvs, "anf", d.anf);
    carry(kvs, "nrl", d.nrl);
    carry(kvs, "nrs", d.nrs);
    carry(kvs, "rnn", d.rnn);
    carry(kvs, "nrf", d.nrf);
    carry(kvs, "anfl", d.anfl);
    carry(kvs, "anft", d.anft);
    carry(kvs, "apf", d.apf);
    // DSP levels
    carry(kvs, "apf_level", d.apfLevel);
    carry(kvs, "nb_level", d.nbLevel);
    carry(kvs, "nr_level", d.nrLevel);
    carry(kvs, "anf_level", d.anfLevel);
    // The radio reports NRL/NRS/ANFL levels under status names that differ
    // from the command names (FlexLib v4.3.16 Slice.cs:3678,3791,2904); the
    // command names are kept as a fallback (#6447).
    carry(kvs, "nrl_level", d.nrlLevel);
    if (!kvs.contains(QStringLiteral("nrl_level"))) {
        carry(kvs, "lms_nr_level", d.nrlLevel);
    }
    carry(kvs, "nrs_level", d.nrsLevel);
    if (!kvs.contains(QStringLiteral("nrs_level"))) {
        carry(kvs, "speex_nr_level", d.nrsLevel);
    }
    carry(kvs, "nrf_level", d.nrfLevel);
    carry(kvs, "anfl_level", d.anflLevel);
    if (!kvs.contains(QStringLiteral("anfl_level"))) {
        carry(kvs, "lms_anf_level", d.anflLevel);
    }

    // AGC / squelch / RIT / XIT
    carry(kvs, "agc_mode", d.agcMode);
    carry(kvs, "agc_threshold", d.agcThreshold);
    // FlexLib 4.3.16 accepts exactly these case-sensitive status values.
    const QString thresholdMode = kvs.value(QStringLiteral("agc_threshold_mode"));
    if (thresholdMode == QLatin1String("auto")) {
        d.agcThresholdMode = AgcThresholdMode::Automatic;
    } else if (thresholdMode == QLatin1String("manual")) {
        d.agcThresholdMode = AgcThresholdMode::Manual;
    }
    carry(kvs, "agc_off_level", d.agcOffLevel);
    carry(kvs, "squelch", d.squelchOn);
    carry(kvs, "squelch_level", d.squelchLevel);
    carry(kvs, "rit_on", d.ritOn);
    carry(kvs, "rit_freq", d.ritFreq);
    carry(kvs, "xit_on", d.xitOn);
    carry(kvs, "xit_freq", d.xitFreq);

    // DAX / RTTY / DIG offsets
    carry(kvs, "dax", d.daxChannel);
    carry(kvs, "rtty_mark", d.rttyMark);
    carry(kvs, "rtty_shift", d.rttyShift);
    carry(kvs, "digl_offset", d.diglOffset);
    carry(kvs, "digu_offset", d.diguOffset);

    // Record / playback (play is 3-state disabled/1/0 — carry raw, model interprets)
    carry(kvs, "record", d.recordOn);
    carry(kvs, "play", d.play);

    // FM duplex/repeater (lowercase normalization stays wire-side)
    if (kvs.contains(QStringLiteral("fm_tone_mode"))) {
        d.fmToneMode = kvs.value(QStringLiteral("fm_tone_mode")).toLower();
    }
    carry(kvs, "fm_tone_value", d.fmToneValue);  // model formats to 1 decimal
    if (kvs.contains(QStringLiteral("repeater_offset_dir"))) {
        d.repeaterOffsetDir = kvs.value(QStringLiteral("repeater_offset_dir")).toLower();
    }
    carry(kvs, "fm_repeater_offset_freq", d.fmRepeaterOffsetFreq);
    carry(kvs, "tx_offset_freq", d.txOffsetFreq);
    carry(kvs, "fm_deviation", d.fmDeviation);

    // Step (step_list carried raw — model builds the QVector<int>)
    carry(kvs, "step", d.step);
    carry(kvs, "step_list", d.stepList);

    for (std::optional<double>* value : {&d.frequency, &d.rfGain, &d.audioGain,
             &d.escGain, &d.escPhaseShift, &d.fmToneValue,
             &d.fmRepeaterOffsetFreq, &d.txOffsetFreq}) {
        if (*value && !std::isfinite(**value)) { value->reset(); }
    }
    return d;
}

} // namespace AetherSDR
