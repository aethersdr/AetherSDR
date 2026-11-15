#include "BandSettings.h"

// Band persistence is deprecated (issue #9). Save/load are no-ops.
#include <cstring>

namespace AetherSDR {

BandSettings::BandSettings(QObject* parent)
    : QObject(parent)
{
}

QString BandSettings::bandForFrequency(double freqMhz)
{
    for (int i = 0; i < kBandCount; ++i) {
        if (freqMhz >= kBands[i].lowMhz && freqMhz <= kBands[i].highMhz)
            return QString::fromLatin1(kBands[i].name);
    }
    return QStringLiteral("GEN");
}

const BandDef& BandSettings::bandDef(const QString& name)
{
    if (name == "WWV") return kWwvBand;
    for (int i = 0; i < kBandCount; ++i) {
        if (name == QLatin1String(kBands[i].name))
            return kBands[i];
    }
    return kGenBand;
}

void BandSettings::saveBandState(const QString& bandName, const BandSnapshot& snap)
{
    m_bandStates[bandName] = snap;
}

BandSnapshot BandSettings::loadBandState(const QString& bandName) const
{
    if (m_bandStates.contains(bandName))
        return m_bandStates[bandName];

    // Return defaults from band definition
    const auto& def = bandDef(bandName);
    BandSnapshot snap;
    snap.frequencyMhz    = def.defaultFreqMhz;
    snap.mode            = QString::fromLatin1(def.defaultMode);
    snap.minDbm          = -130.0f;
    snap.maxDbm          = -40.0f;
    snap.spectrumFrac    = 0.40f;
    return snap;
}

bool BandSettings::hasSavedState(const QString& bandName) const
{
    return m_bandStates.contains(bandName);
}

void BandSettings::saveToFile() const
{
    // Deprecated — band persistence pending redesign (issue #9)
}

void BandSettings::loadFromFile()
{
    // Deprecated — band persistence pending redesign (issue #9)
}

} // namespace AetherSDR
