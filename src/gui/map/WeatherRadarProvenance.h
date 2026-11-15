#pragma once

#include "WeatherRadarSource.h"
#include <QImage>

namespace AetherSDR {
inline int requestedRadarProviders(const WeatherRadarSource& source)
{
    switch (source.provider()) {
    case WeatherRadarSource::Provider::NoaaMrms: return 1;
    case WeatherRadarSource::Provider::Eccc: return 2;
    case WeatherRadarSource::Provider::Opera: return 4;
    case WeatherRadarSource::Provider::LibreWxr: return 8;
    case WeatherRadarSource::Provider::Composite: return source.enabledProviders();
    }
    return 0;
}

// Persist the providers actually used by a composite, including transparent
// successful images. Selection alone cannot identify a live fallback image.
inline void setRadarImageProviders(QImage& image, int providers)
{
    image.setText(QStringLiteral("AetherSDR.RadarProviders"), QString::number(providers & 15));
}

inline int radarImageProviders(const QImage& image, const WeatherRadarSource& source)
{
    if (source.provider() != WeatherRadarSource::Provider::Composite) {
        return requestedRadarProviders(source);
    }
    bool ok = false;
    const int providers = image.text(QStringLiteral("AetherSDR.RadarProviders")).toInt(&ok);
    if (ok && providers > 0 && (providers & ~15) == 0) {
        return providers;
    }
    return 0;
}
}
