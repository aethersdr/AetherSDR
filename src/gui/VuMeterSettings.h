#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace AetherSDR::VuMeterSettingsCodec {

inline constexpr int kVersion = 3;
inline const QString kSettingsKey = QStringLiteral("VuMeter");
inline const QString kAetherTheme = QStringLiteral("aether-default");
inline const QString kClassicTheme = QStringLiteral("classic-warm");
inline const QString kUplightTheme = QStringLiteral("dark-room-uplight");
inline const QString kDarkTheme = QStringLiteral("graphite-dark");

struct Snapshot {
    int txSelect{0};
    int rxSelect{0};
    bool peakHoldEnabled{false};
    QString peakDecayRate{QStringLiteral("Medium")};
    QString faceTheme{kAetherTheme};
};

// Version 1 temporarily combined the standard and cross-needle meters. Keep
// just enough decode metadata to migrate its face theme into the independent
// PWR applet before rewriting VuMeter as the current standard-only object.
struct LegacyCrossNeedle {
    bool present{false};
    bool selected{false};
    QString faceTheme;
};

const QStringList& txMeterItems();
const QStringList& rxMeterItems();
const QStringList& decayItems();
const QStringList& faceThemeItems();
QString normalizeFaceTheme(const QString& theme);

QString encode(const Snapshot& settings);
Snapshot decode(const QByteArray& encoded, QString* error = nullptr,
                LegacyCrossNeedle* legacyCrossNeedle = nullptr);
Snapshot migrateLegacy(int txSelect, int rxSelect,
                       bool peakHoldEnabled, const QString& peakDecayRate);

} // namespace AetherSDR::VuMeterSettingsCodec
