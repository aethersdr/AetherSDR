#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace AetherSDR::VuMeterSettingsCodec {

inline constexpr int kVersion = 2;
inline const QString kSettingsKey = QStringLiteral("VuMeter");

struct Snapshot {
    int txSelect{0};
    int rxSelect{0};
    bool peakHoldEnabled{false};
    QString peakDecayRate{QStringLiteral("Medium")};
};

// Version 1 temporarily combined the standard and cross-needle meters. Keep
// just enough decode metadata to migrate its face theme into the independent
// PWR applet before rewriting VuMeter as the version-2 standard-only object.
struct LegacyCrossNeedle {
    bool present{false};
    bool selected{false};
    QString faceTheme;
};

const QStringList& txMeterItems();
const QStringList& rxMeterItems();
const QStringList& decayItems();

QString encode(const Snapshot& settings);
Snapshot decode(const QByteArray& encoded, QString* error = nullptr,
                LegacyCrossNeedle* legacyCrossNeedle = nullptr);
Snapshot migrateLegacy(int txSelect, int rxSelect,
                       bool peakHoldEnabled, const QString& peakDecayRate);

} // namespace AetherSDR::VuMeterSettingsCodec
