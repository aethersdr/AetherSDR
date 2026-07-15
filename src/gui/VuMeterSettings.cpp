#include "VuMeterSettings.h"

#include <QJsonDocument>
#include <QJsonObject>

namespace AetherSDR::VuMeterSettingsCodec {

const QStringList& txMeterItems()
{
    static const QStringList items{
        QStringLiteral("Power"), QStringLiteral("SWR"),
        QStringLiteral("Level"), QStringLiteral("Compression")};
    return items;
}

const QStringList& rxMeterItems()
{
    static const QStringList items{
        QStringLiteral("S-Meter"), QStringLiteral("S-Meter Peak")};
    return items;
}

const QStringList& decayItems()
{
    static const QStringList items{
        QStringLiteral("Fast"), QStringLiteral("Medium"), QStringLiteral("Slow")};
    return items;
}

QString encode(const Snapshot& settings)
{
    QJsonObject standard;
    standard.insert(QStringLiteral("txSelect"), settings.txSelect);
    standard.insert(QStringLiteral("rxSelect"), settings.rxSelect);
    standard.insert(QStringLiteral("peakHoldEnabled"), settings.peakHoldEnabled);
    standard.insert(QStringLiteral("peakDecayRate"), settings.peakDecayRate);

    QJsonObject root;
    root.insert(QStringLiteral("version"), kVersion);
    root.insert(QStringLiteral("standard"), standard);
    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

Snapshot decode(const QByteArray& encoded, QString* error,
                LegacyCrossNeedle* legacyCrossNeedle)
{
    if (error) {
        error->clear();
    }
    if (legacyCrossNeedle) {
        *legacyCrossNeedle = {};
    }

    Snapshot settings;
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(encoded, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error) {
            *error = parseError.error != QJsonParseError::NoError
                ? parseError.errorString()
                : QStringLiteral("root is not an object");
        }
        return settings;
    }

    const QJsonObject root = document.object();
    const int version = root.value(QStringLiteral("version")).toInt();
    if (version != 1 && version != kVersion) {
        if (error) {
            *error = QStringLiteral("unsupported VuMeter settings version");
        }
        return settings;
    }

    if (version == 1 && legacyCrossNeedle) {
        legacyCrossNeedle->present = true;
        legacyCrossNeedle->selected =
            root.value(QStringLiteral("style")).toString() ==
            QStringLiteral("cross-needle");
        legacyCrossNeedle->faceTheme = root.value(QStringLiteral("crossNeedle"))
                                           .toObject()
                                           .value(QStringLiteral("faceTheme"))
                                           .toString();
    }

    const QJsonObject standard = root.value(QStringLiteral("standard")).toObject();
    settings.txSelect = qBound(0, standard.value(QStringLiteral("txSelect")).toInt(),
                               static_cast<int>(txMeterItems().size()) - 1);
    settings.rxSelect = qBound(0, standard.value(QStringLiteral("rxSelect")).toInt(),
                               static_cast<int>(rxMeterItems().size()) - 1);
    settings.peakHoldEnabled =
        standard.value(QStringLiteral("peakHoldEnabled")).toBool(false);
    const QString decay = standard.value(QStringLiteral("peakDecayRate")).toString();
    if (decayItems().contains(decay)) {
        settings.peakDecayRate = decay;
    }
    return settings;
}

Snapshot migrateLegacy(int txSelect, int rxSelect,
                       bool peakHoldEnabled, const QString& peakDecayRate)
{
    Snapshot settings;
    settings.txSelect = qBound(0, txSelect, static_cast<int>(txMeterItems().size()) - 1);
    settings.rxSelect = qBound(0, rxSelect, static_cast<int>(rxMeterItems().size()) - 1);
    settings.peakHoldEnabled = peakHoldEnabled;
    if (decayItems().contains(peakDecayRate)) {
        settings.peakDecayRate = peakDecayRate;
    }
    return settings;
}

} // namespace AetherSDR::VuMeterSettingsCodec
