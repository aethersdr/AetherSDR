#include "AetherTxStockProfiles.h"

#include <QJsonDocument>

namespace AetherSDR {

// The three stock AetherTX presets (2026-11-15), tuned on a FLEX-8600 at 25 W
// into a dummy load: the highest average forward power whose received audio
// stayed copyable (STOI >= 0.75 on the RX-A sampler). Each is the exact
// ChannelStripPresets::captureTxJson() form, so every key is one the engine
// reads back unchanged (aether_tx_stock_profiles_test). No "rn2": RNNoise
// stays the operator's choice.
namespace {

struct Stock {
    const char* name;
    int lowHz;
    int highHz;
    const char* summary;
    const char* json;
};

constexpr const char kSsbJson[] = R"json(
{
 "cfc": {
  "bands": [
   {
    "compDb": 0,
    "eqDb": 7,
    "hz": 200
   },
   {
    "compDb": 5,
    "eqDb": 7,
    "hz": 1000
   },
   {
    "compDb": 10,
    "eqDb": 7,
    "hz": 2000
   },
   {
    "compDb": 10,
    "eqDb": 7,
    "hz": 3000
   },
   {
    "compDb": 5,
    "eqDb": 7,
    "hz": 4000
   }
  ],
  "enabled": false,
  "fftSize": 1024,
  "postEq": false,
  "postGainDb": 0,
  "precompDb": 0
 },
 "chain": [
  "Tube",
  "Eq",
  "Leveler",
  "Cfc",
  "Comp",
  "Enh",
  "Reverb"
 ],
 "comp": {
  "attackMs": 20,
  "driveDb": 0,
  "enabled": true,
  "kneeDb": 6,
  "limiterCeilingDb": -1,
  "limiterEnabled": true,
  "makeupDb": 16,
  "phaseRotatorStages": 6,
  "ratio": 4,
  "releaseMs": 150,
  "thresholdDb": -24
 },
 "eq": {
  "activeBandCount": 10,
  "bands": [
   {
    "enabled": true,
    "freqHz": 100,
    "gainDb": 0,
    "q": 0.7070000171661377,
    "slopeDbPerOct": 24,
    "type": "HighPass"
   },
   {
    "enabled": false,
    "freqHz": 100,
    "gainDb": 0,
    "q": 0.7070000171661377,
    "slopeDbPerOct": 12,
    "type": "LowShelf"
   },
   {
    "enabled": true,
    "freqHz": 250,
    "gainDb": -2,
    "q": 1,
    "slopeDbPerOct": 12,
    "type": "Peak"
   },
   {
    "enabled": false,
    "freqHz": 400,
    "gainDb": 0,
    "q": 0.7070000171661377,
    "slopeDbPerOct": 12,
    "type": "Peak"
   },
   {
    "enabled": false,
    "freqHz": 800,
    "gainDb": 0,
    "q": 0.7070000171661377,
    "slopeDbPerOct": 12,
    "type": "Peak"
   },
   {
    "enabled": true,
    "freqHz": 2200,
    "gainDb": 3,
    "q": 1,
    "slopeDbPerOct": 12,
    "type": "Peak"
   },
   {
    "enabled": false,
    "freqHz": 3000,
    "gainDb": 0,
    "q": 0.7070000171661377,
    "slopeDbPerOct": 12,
    "type": "Peak"
   },
   {
    "enabled": false,
    "freqHz": 5000,
    "gainDb": 0,
    "q": 0.7070000171661377,
    "slopeDbPerOct": 12,
    "type": "Peak"
   },
   {
    "enabled": false,
    "freqHz": 8000,
    "gainDb": 0,
    "q": 0.7070000171661377,
    "slopeDbPerOct": 12,
    "type": "HighShelf"
   },
   {
    "enabled": false,
    "freqHz": 12000,
    "gainDb": 0,
    "q": 0.7070000171661377,
    "slopeDbPerOct": 12,
    "type": "LowPass"
   },
   {
    "enabled": false,
    "freqHz": 1000,
    "gainDb": 0,
    "q": 0.7070000171661377,
    "slopeDbPerOct": 12,
    "type": "Peak"
   },
   {
    "enabled": false,
    "freqHz": 1000,
    "gainDb": 0,
    "q": 0.7070000171661377,
    "slopeDbPerOct": 12,
    "type": "Peak"
   },
   {
    "enabled": false,
    "freqHz": 1000,
    "gainDb": 0,
    "q": 0.7070000171661377,
    "slopeDbPerOct": 12,
    "type": "Peak"
   },
   {
    "enabled": false,
    "freqHz": 1000,
    "gainDb": 0,
    "q": 0.7070000171661377,
    "slopeDbPerOct": 12,
    "type": "Peak"
   },
   {
    "enabled": false,
    "freqHz": 1000,
    "gainDb": 0,
    "q": 0.7070000171661377,
    "slopeDbPerOct": 12,
    "type": "Peak"
   },
   {
    "enabled": false,
    "freqHz": 1000,
    "gainDb": 0,
    "q": 0.7070000171661377,
    "slopeDbPerOct": 12,
    "type": "Peak"
   }
  ],
  "enabled": true,
  "filterFamily": "Butterworth",
  "masterGain": 1
 },
 "finalLimiter": {
  "ceilingDb": -0.5,
  "dcBlock": true,
  "enabled": true,
  "lookaheadMs": 1.5,
  "outputTrimDb": 0
 },
 "leveler": {
  "attackMs": 1,
  "decayMs": 500,
  "enabled": false,
  "hangMs": 0,
  "maxGainDb": 5
 },
 "mblim": {
  "bands": [
   {
    "releaseMs": 100,
    "thresholdDb": -6
   },
   {
    "releaseMs": 100,
    "thresholdDb": -6
   },
   {
    "releaseMs": 100,
    "thresholdDb": -6
   },
   {
    "releaseMs": 100,
    "thresholdDb": -6
   }
  ],
  "enabled": false,
  "x1": 250,
  "x2": 1000,
  "x3": 3000
 },
 "peak": {
  "cessb": true,
  "clipDb": 5,
  "enabled": true,
  "outputDb": -1,
  "route": "modulator"
 },
 "pudu": {
  "dooHarmonicsDb": 6,
  "dooMix": 0.30000001192092896,
  "dooTuneHz": 5000,
  "enabled": false,
  "mode": "Aphex",
  "pooDriveDb": 6,
  "pooMix": 0.30000001192092896,
  "pooTuneHz": 100
 },
 "reverb": {
  "damping": 0.5,
  "decayS": 1.2000000476837158,
  "enabled": false,
  "mix": 0.15000000596046448,
  "preDelayMs": 20,
  "size": 0.5
 },
 "tube": {
  "biasAmount": 0,
  "driveDb": 0,
  "dryWet": 1,
  "enabled": false,
  "envelopeAmount": 0,
  "model": "A",
  "outputGainDb": 0,
  "releaseMs": 35,
  "tone": 0
 }
}
)json";

constexpr const char kDxJson[] = R"json(
{
 "cfc": {
  "bands": [
   {
    "compDb": 0,
    "eqDb": 7,
    "hz": 200
   },
   {
    "compDb": 5,
    "eqDb": 7,
    "hz": 1000
   },
   {
    "compDb": 10,
    "eqDb": 7,
    "hz": 2000
   },
   {
    "compDb": 10,
    "eqDb": 7,
    "hz": 3000
   },
   {
    "compDb": 5,
    "eqDb": 7,
    "hz": 4000
   }
  ],
  "enabled": false,
  "fftSize": 1024,
  "postEq": false,
  "postGainDb": 0,
  "precompDb": 0
 },
 "chain": [
  "Tube",
  "Eq",
  "Leveler",
  "Cfc",
  "Comp",
  "Enh",
  "Reverb"
 ],
 "comp": {
  "attackMs": 20,
  "driveDb": 0,
  "enabled": true,
  "kneeDb": 6,
  "limiterCeilingDb": -1,
  "limiterEnabled": true,
  "makeupDb": 16,
  "phaseRotatorStages": 6,
  "ratio": 4,
  "releaseMs": 150,
  "thresholdDb": -24
 },
 "eq": {
  "activeBandCount": 10,
  "bands": [
   {
    "enabled": true,
    "freqHz": 200,
    "gainDb": 0,
    "q": 0.7070000171661377,
    "slopeDbPerOct": 24,
    "type": "HighPass"
   },
   {
    "enabled": false,
    "freqHz": 100,
    "gainDb": 0,
    "q": 0.7070000171661377,
    "slopeDbPerOct": 12,
    "type": "LowShelf"
   },
   {
    "enabled": false,
    "freqHz": 200,
    "gainDb": 0,
    "q": 0.7070000171661377,
    "slopeDbPerOct": 12,
    "type": "Peak"
   },
   {
    "enabled": false,
    "freqHz": 400,
    "gainDb": 0,
    "q": 0.7070000171661377,
    "slopeDbPerOct": 12,
    "type": "Peak"
   },
   {
    "enabled": false,
    "freqHz": 800,
    "gainDb": 0,
    "q": 0.7070000171661377,
    "slopeDbPerOct": 12,
    "type": "Peak"
   },
   {
    "enabled": true,
    "freqHz": 2200,
    "gainDb": 3,
    "q": 1,
    "slopeDbPerOct": 12,
    "type": "Peak"
   },
   {
    "enabled": false,
    "freqHz": 3000,
    "gainDb": 0,
    "q": 0.7070000171661377,
    "slopeDbPerOct": 12,
    "type": "Peak"
   },
   {
    "enabled": false,
    "freqHz": 5000,
    "gainDb": 0,
    "q": 0.7070000171661377,
    "slopeDbPerOct": 12,
    "type": "Peak"
   },
   {
    "enabled": false,
    "freqHz": 8000,
    "gainDb": 0,
    "q": 0.7070000171661377,
    "slopeDbPerOct": 12,
    "type": "HighShelf"
   },
   {
    "enabled": false,
    "freqHz": 12000,
    "gainDb": 0,
    "q": 0.7070000171661377,
    "slopeDbPerOct": 12,
    "type": "LowPass"
   },
   {
    "enabled": false,
    "freqHz": 1000,
    "gainDb": 0,
    "q": 0.7070000171661377,
    "slopeDbPerOct": 12,
    "type": "Peak"
   },
   {
    "enabled": false,
    "freqHz": 1000,
    "gainDb": 0,
    "q": 0.7070000171661377,
    "slopeDbPerOct": 12,
    "type": "Peak"
   },
   {
    "enabled": false,
    "freqHz": 1000,
    "gainDb": 0,
    "q": 0.7070000171661377,
    "slopeDbPerOct": 12,
    "type": "Peak"
   },
   {
    "enabled": false,
    "freqHz": 1000,
    "gainDb": 0,
    "q": 0.7070000171661377,
    "slopeDbPerOct": 12,
    "type": "Peak"
   },
   {
    "enabled": false,
    "freqHz": 1000,
    "gainDb": 0,
    "q": 0.7070000171661377,
    "slopeDbPerOct": 12,
    "type": "Peak"
   },
   {
    "enabled": false,
    "freqHz": 1000,
    "gainDb": 0,
    "q": 0.7070000171661377,
    "slopeDbPerOct": 12,
    "type": "Peak"
   }
  ],
  "enabled": true,
  "filterFamily": "Butterworth",
  "masterGain": 1
 },
 "finalLimiter": {
  "ceilingDb": -0.5,
  "dcBlock": true,
  "enabled": true,
  "lookaheadMs": 1.5,
  "outputTrimDb": 0
 },
 "leveler": {
  "attackMs": 1,
  "decayMs": 500,
  "enabled": false,
  "hangMs": 0,
  "maxGainDb": 5
 },
 "mblim": {
  "bands": [
   {
    "releaseMs": 100,
    "thresholdDb": -6
   },
   {
    "releaseMs": 100,
    "thresholdDb": -6
   },
   {
    "releaseMs": 100,
    "thresholdDb": -6
   },
   {
    "releaseMs": 100,
    "thresholdDb": -6
   }
  ],
  "enabled": false,
  "x1": 250,
  "x2": 1000,
  "x3": 3000
 },
 "peak": {
  "cessb": true,
  "clipDb": 4,
  "enabled": true,
  "outputDb": -1,
  "route": "modulator"
 },
 "pudu": {
  "dooHarmonicsDb": 6,
  "dooMix": 0.30000001192092896,
  "dooTuneHz": 5000,
  "enabled": false,
  "mode": "Aphex",
  "pooDriveDb": 6,
  "pooMix": 0.30000001192092896,
  "pooTuneHz": 100
 },
 "reverb": {
  "damping": 0.5,
  "decayS": 1.2000000476837158,
  "enabled": false,
  "mix": 0.15000000596046448,
  "preDelayMs": 20,
  "size": 0.5
 },
 "tube": {
  "biasAmount": 0,
  "driveDb": 0,
  "dryWet": 1,
  "enabled": false,
  "envelopeAmount": 0,
  "model": "A",
  "outputGainDb": 0,
  "releaseMs": 35,
  "tone": 0
 }
}
)json";

constexpr const char kEssbJson[] = R"json(
{
 "cfc": {
  "bands": [
   {
    "compDb": 0,
    "eqDb": 7,
    "hz": 200
   },
   {
    "compDb": 5,
    "eqDb": 7,
    "hz": 1000
   },
   {
    "compDb": 10,
    "eqDb": 7,
    "hz": 2000
   },
   {
    "compDb": 10,
    "eqDb": 7,
    "hz": 3000
   },
   {
    "compDb": 5,
    "eqDb": 7,
    "hz": 4000
   }
  ],
  "enabled": false,
  "fftSize": 1024,
  "postEq": false,
  "postGainDb": 0,
  "precompDb": 0
 },
 "chain": [
  "Tube",
  "Eq",
  "Leveler",
  "Cfc",
  "Comp",
  "Enh",
  "Reverb"
 ],
 "comp": {
  "attackMs": 25,
  "driveDb": 0,
  "enabled": true,
  "kneeDb": 6,
  "limiterCeilingDb": -1,
  "limiterEnabled": true,
  "makeupDb": 14,
  "phaseRotatorStages": 6,
  "ratio": 3,
  "releaseMs": 200,
  "thresholdDb": -22
 },
 "eq": {
  "activeBandCount": 10,
  "bands": [
   {
    "enabled": true,
    "freqHz": 50,
    "gainDb": 0,
    "q": 0.7070000171661377,
    "slopeDbPerOct": 12,
    "type": "HighPass"
   },
   {
    "enabled": true,
    "freqHz": 120,
    "gainDb": 2,
    "q": 0.699999988079071,
    "slopeDbPerOct": 12,
    "type": "LowShelf"
   },
   {
    "enabled": false,
    "freqHz": 200,
    "gainDb": 0,
    "q": 0.7070000171661377,
    "slopeDbPerOct": 12,
    "type": "Peak"
   },
   {
    "enabled": false,
    "freqHz": 400,
    "gainDb": 0,
    "q": 0.7070000171661377,
    "slopeDbPerOct": 12,
    "type": "Peak"
   },
   {
    "enabled": false,
    "freqHz": 800,
    "gainDb": 0,
    "q": 0.7070000171661377,
    "slopeDbPerOct": 12,
    "type": "Peak"
   },
   {
    "enabled": false,
    "freqHz": 1500,
    "gainDb": 0,
    "q": 0.7070000171661377,
    "slopeDbPerOct": 12,
    "type": "Peak"
   },
   {
    "enabled": false,
    "freqHz": 3000,
    "gainDb": 0,
    "q": 0.7070000171661377,
    "slopeDbPerOct": 12,
    "type": "Peak"
   },
   {
    "enabled": false,
    "freqHz": 5000,
    "gainDb": 0,
    "q": 0.7070000171661377,
    "slopeDbPerOct": 12,
    "type": "Peak"
   },
   {
    "enabled": true,
    "freqHz": 4000,
    "gainDb": 3,
    "q": 0.699999988079071,
    "slopeDbPerOct": 12,
    "type": "HighShelf"
   },
   {
    "enabled": false,
    "freqHz": 12000,
    "gainDb": 0,
    "q": 0.7070000171661377,
    "slopeDbPerOct": 12,
    "type": "LowPass"
   },
   {
    "enabled": false,
    "freqHz": 1000,
    "gainDb": 0,
    "q": 0.7070000171661377,
    "slopeDbPerOct": 12,
    "type": "Peak"
   },
   {
    "enabled": false,
    "freqHz": 1000,
    "gainDb": 0,
    "q": 0.7070000171661377,
    "slopeDbPerOct": 12,
    "type": "Peak"
   },
   {
    "enabled": false,
    "freqHz": 1000,
    "gainDb": 0,
    "q": 0.7070000171661377,
    "slopeDbPerOct": 12,
    "type": "Peak"
   },
   {
    "enabled": false,
    "freqHz": 1000,
    "gainDb": 0,
    "q": 0.7070000171661377,
    "slopeDbPerOct": 12,
    "type": "Peak"
   },
   {
    "enabled": false,
    "freqHz": 1000,
    "gainDb": 0,
    "q": 0.7070000171661377,
    "slopeDbPerOct": 12,
    "type": "Peak"
   },
   {
    "enabled": false,
    "freqHz": 1000,
    "gainDb": 0,
    "q": 0.7070000171661377,
    "slopeDbPerOct": 12,
    "type": "Peak"
   }
  ],
  "enabled": true,
  "filterFamily": "Butterworth",
  "masterGain": 1
 },
 "finalLimiter": {
  "ceilingDb": -0.5,
  "dcBlock": true,
  "enabled": true,
  "lookaheadMs": 1.5,
  "outputTrimDb": 0
 },
 "leveler": {
  "attackMs": 1,
  "decayMs": 500,
  "enabled": false,
  "hangMs": 0,
  "maxGainDb": 5
 },
 "mblim": {
  "bands": [
   {
    "releaseMs": 100,
    "thresholdDb": -6
   },
   {
    "releaseMs": 100,
    "thresholdDb": -6
   },
   {
    "releaseMs": 100,
    "thresholdDb": -6
   },
   {
    "releaseMs": 100,
    "thresholdDb": -6
   }
  ],
  "enabled": false,
  "x1": 250,
  "x2": 1000,
  "x3": 3000
 },
 "peak": {
  "cessb": true,
  "clipDb": 5,
  "enabled": true,
  "outputDb": -1,
  "route": "modulator"
 },
 "pudu": {
  "dooHarmonicsDb": 6,
  "dooMix": 0.30000001192092896,
  "dooTuneHz": 5000,
  "enabled": false,
  "mode": "Aphex",
  "pooDriveDb": 6,
  "pooMix": 0.30000001192092896,
  "pooTuneHz": 100
 },
 "reverb": {
  "damping": 0.5,
  "decayS": 1.2000000476837158,
  "enabled": false,
  "mix": 0.15000000596046448,
  "preDelayMs": 20,
  "size": 0.5
 },
 "tube": {
  "biasAmount": 0,
  "driveDb": 0,
  "dryWet": 1,
  "enabled": false,
  "envelopeAmount": 0,
  "model": "A",
  "outputGainDb": 0,
  "releaseMs": 35,
  "tone": 0
 }
}
)json";

constexpr Stock kStock[] = {
    {"SSB 3 kHz", 100, 3100, "Everyday SSB: clean, full-sounding voice with dense average power.", kSsbJson},
    {"DX Punch 2.7 kHz", 200, 2900, "Narrow and forward for weak-signal DX and pileups; trims the lows for presence.", kDxJson},
    {"ESSB 6 kHz", 50, 6050, "Wide, natural ESSB with extended lows and air, for rag-chews on open bands.", kEssbJson},
};

const Stock* find(const QString& name)
{
    for (const Stock& s : kStock) {
        if (name.compare(QLatin1String(s.name), Qt::CaseInsensitive) == 0) {
            return &s;
        }
    }
    return nullptr;
}

} // namespace

QStringList AetherTxStockProfiles::names()
{
    QStringList out;
    for (const Stock& s : kStock) {
        out << QString::fromLatin1(s.name);
    }
    return out;
}

bool AetherTxStockProfiles::contains(const QString& name)
{
    return find(name.trimmed()) != nullptr;
}

QString AetherTxStockProfiles::canonicalName(const QString& name)
{
    const Stock* s = find(name.trimmed());
    return s ? QString::fromLatin1(s->name) : QString();
}

QJsonObject AetherTxStockProfiles::profile(const QString& name)
{
    const Stock* s = find(name.trimmed());
    return s ? QJsonDocument::fromJson(QByteArray(s->json)).object() : QJsonObject();
}

std::optional<AetherTxStockProfiles::TxFilter> AetherTxStockProfiles::txFilter(const QString& name)
{
    const Stock* s = find(name.trimmed());
    if (!s) {
        return std::nullopt;
    }
    return TxFilter{s->lowHz, s->highHz};
}

QString AetherTxStockProfiles::summary(const QString& name)
{
    const Stock* s = find(name.trimmed());
    return s ? QString::fromUtf8(s->summary) : QString();
}

} // namespace AetherSDR
