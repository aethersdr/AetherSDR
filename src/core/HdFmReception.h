#pragma once

#include <QMetaType>
#include <QString>
#include <QVector>
#include <cmath>
#include <optional>

namespace AetherSDR {
// Operator selection is independent of observed sync and successful audio.
enum class WfmAudioMode { Mono, Stereo, HdStereo };
inline constexpr bool validWfmAudioMode(WfmAudioMode mode)
{
    return mode == WfmAudioMode::Mono || mode == WfmAudioMode::Stereo
        || mode == WfmAudioMode::HdStereo;
}
struct HdFmService {
    int program = -1; // decoder program 0..7; displayed as HD1..HD8
    QString name;
    bool audioAvailable = false; // an actually discovered audio service
    bool operator==(const HdFmService&) const = default;
};
// Owner-thread normalized, complete snapshot from the native NRSC-5 decoder.
// Text is HD SIS/SIG/ID3 data; this type never claims analog RDS/RBDS decoding.
struct HdFmReception {
    bool valid = false;
    quint64 sessionId = 0;
    quint64 receiverEpoch = 0;
    quint64 revision = 0;
    qint64 frequencyHz = 0;
    int selectedProgram = -1;
    bool synced = false;
    bool audioValid = false; // recent unflagged selected-program PCM while synced
    QVector<HdFmService> services;
    QString stationName;
    QString title;
    QString artist;
    // Exact upstream measurements. MER labels use NRSC-5's FM spectral
    // convention, inverted relative to RF lower/upper sidebands.
    std::optional<double> merLowerDb;
    std::optional<double> merUpperDb;
    std::optional<double> cber;
    std::optional<double> frequencyOffsetHz;
    quint64 observationSequence = 0;
    quint32 syncLossCount = 0;
    quint32 reacquisitionCount = 0;
    quint32 syncDurationMs = 0;
    bool operator==(const HdFmReception&) const = default;
};
// Bound external text before scanning. Consumers render as plain text only.
inline QString boundedBroadcastText(const QString& input, qsizetype limit)
{
    if (limit <= 0 || limit > 1024) { return {}; }
    QString output;
    output.reserve(limit);
    for (QChar character : input.left(limit * 4)) {
        if (character.isSpace()) { character = QLatin1Char(' '); }
        if (!character.isPrint() || character.category() == QChar::Other_Format) { continue; }
        output.append(character);
        if (output.size() == limit) { break; }
    }
    return output.simplified();
}
inline HdFmReception normalizedHdFmReception(HdFmReception value)
{
    if (!value.valid || !value.sessionId || !value.receiverEpoch || !value.revision
        || value.frequencyHz <= 0 || value.frequencyHz > 1'766'000'000
        || value.selectedProgram < 0 || value.selectedProgram > 7
        || value.services.size() > 8 || !value.observationSequence) { return {}; }
    for (const auto& metric : {value.merLowerDb, value.merUpperDb, value.cber, value.frequencyOffsetHz}) {
        if (metric && !std::isfinite(*metric)) { return {}; }
    }
    if (value.cber && (*value.cber < 0 || *value.cber > 1)) { return {}; }
    unsigned programs = 0;
    for (HdFmService& service : value.services) {
        if (service.program < 0 || service.program > 7 || (programs & (1u << service.program))) { return {}; }
        programs |= 1u << service.program;
        service.name = boundedBroadcastText(service.name, 64);
    }
    value.stationName = boundedBroadcastText(value.stationName, 64);
    value.title = boundedBroadcastText(value.title, 256);
    value.artist = boundedBroadcastText(value.artist, 128);
    if (!value.synced) {
        value.audioValid = false;
        value.services.clear();
        value.stationName.clear(); value.title.clear(); value.artist.clear();
        value.merLowerDb.reset(); value.merUpperDb.reset(); value.cber.reset();
        value.syncDurationMs = 0;
    }
    return value;
}
} // namespace AetherSDR
Q_DECLARE_METATYPE(AetherSDR::WfmAudioMode)
Q_DECLARE_METATYPE(AetherSDR::HdFmReception)
