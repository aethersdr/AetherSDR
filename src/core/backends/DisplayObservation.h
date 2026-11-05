#pragma once

#include <QMetaType>
#include <QString>
#include <QStringList>
#include <atomic>
#include <memory>
#include <optional>

namespace AetherSDR {

// A partial radio report. Absence preserves the previously observed value.
struct DisplayValues {
    std::optional<double> centerMhz;
    std::optional<double> bandwidthMhz;
    std::optional<double> minDbm;
    std::optional<double> maxDbm;
    std::optional<double> noiseFloorRange;
    std::optional<int> rfGain;
    std::optional<int> fps;
    std::optional<int> average;
    std::optional<int> daxIqChannel;
    std::optional<int> wnbLevel;
    std::optional<int> xPixels;
    std::optional<int> yPixels;
    std::optional<int> waterfallRate;
    std::optional<bool> wide;
    std::optional<bool> loopA;
    std::optional<bool> loopB;
    std::optional<bool> weightedAverage;
    std::optional<bool> bandZoom;
    std::optional<bool> segmentZoom;
    std::optional<bool> wnb;
    std::optional<bool> wnbUpdating;
    std::optional<QString> preamp;
    std::optional<QString> rxAntenna;
    std::optional<QStringList> antennas;

    void merge(const DisplayValues& newer)
    {
        if (newer.centerMhz) { centerMhz = newer.centerMhz; }
        if (newer.bandwidthMhz) { bandwidthMhz = newer.bandwidthMhz; }
        if (newer.minDbm) { minDbm = newer.minDbm; }
        if (newer.maxDbm) { maxDbm = newer.maxDbm; }
        if (newer.noiseFloorRange) { noiseFloorRange = newer.noiseFloorRange; }
        if (newer.rfGain) { rfGain = newer.rfGain; }
        if (newer.fps) { fps = newer.fps; }
        if (newer.average) { average = newer.average; }
        if (newer.daxIqChannel) { daxIqChannel = newer.daxIqChannel; }
        if (newer.wnbLevel) { wnbLevel = newer.wnbLevel; }
        if (newer.xPixels) { xPixels = newer.xPixels; }
        if (newer.yPixels) { yPixels = newer.yPixels; }
        if (newer.waterfallRate) { waterfallRate = newer.waterfallRate; }
        if (newer.wide) { wide = newer.wide; }
        if (newer.loopA) { loopA = newer.loopA; }
        if (newer.loopB) { loopB = newer.loopB; }
        if (newer.weightedAverage) { weightedAverage = newer.weightedAverage; }
        if (newer.bandZoom) { bandZoom = newer.bandZoom; }
        if (newer.segmentZoom) { segmentZoom = newer.segmentZoom; }
        if (newer.wnb) { wnb = newer.wnb; }
        if (newer.wnbUpdating) { wnbUpdating = newer.wnbUpdating; }
        if (newer.preamp) { preamp = newer.preamp; }
        if (newer.rxAntenna) { rxAntenna = newer.rxAntenna; }
        if (newer.antennas) { antennas = newer.antennas; }
    }
};

struct DisplayObservation {
    enum class Kind { Panadapter, Waterfall };
    Kind kind{Kind::Panadapter};
    QString id;
    bool removed{false};
    quint32 sourceHandle{0};
    std::optional<quint32> ownerHandle;
    // Child waterfall for a pan; parent pan for a waterfall. Empty means cleared.
    std::optional<QString> pairedId;
    DisplayValues values;
    // Revoked at the transport boundary before disconnect/reconnect. Queued
    // observations retain their original source, never callback-time authority.
    std::shared_ptr<const std::atomic_bool> transportEpoch;

    bool current() const
    {
        return !id.isEmpty() && transportEpoch
            && transportEpoch->load(std::memory_order_acquire);
    }
};

} // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::DisplayObservation)
