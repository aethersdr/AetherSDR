#pragma once

#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

#include <algorithm>

// Persisted Lock view intent, one versioned object under one root key:
//   {"version":1,"radios":{"<serial>/<station>":{"<panIndex>":true}}}
// Keyed like Center Lock (radio/station, then stable pan index; radio pan ids
// change between sessions, and like Center Lock the whole map is written
// back). Only the flag is stored: the locked range is radio-owned.

namespace AetherSDR::ViewLockSettings {

inline constexpr auto kSettingsKey = "ViewLock";
inline constexpr int kSettingsVersion = 1;

// radio key -> pan indices whose view is locked
using LockedPans = QHash<QString, QSet<int>>;

// Returns an empty map for blank, malformed or unsupported-version input and
// says why in *error (left empty for blank input).
inline LockedPans parse(const QString& raw, QString* error = nullptr)
{
    LockedPans locked;
    const QString trimmed = raw.trimmed();
    if (trimmed.isEmpty()) {
        return locked;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(trimmed.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error) {
            *error = parseError.error != QJsonParseError::NoError
                ? parseError.errorString()
                : QStringLiteral("not a JSON object");
        }
        return locked;
    }

    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("version")).toInt() != kSettingsVersion) {
        if (error) {
            *error = QStringLiteral("unsupported version");
        }
        return locked;
    }

    const QJsonObject radios = root.value(QStringLiteral("radios")).toObject();
    for (auto radioIt = radios.constBegin(); radioIt != radios.constEnd(); ++radioIt) {
        const QString radioKey = radioIt.key().trimmed();
        if (radioKey.isEmpty() || !radioIt.value().isObject()) {
            continue;
        }
        QSet<int> panIndices;
        const QJsonObject panSlots = radioIt.value().toObject();
        for (auto slotIt = panSlots.constBegin(); slotIt != panSlots.constEnd(); ++slotIt) {
            bool indexOk = false;
            const int panIndex = slotIt.key().toInt(&indexOk);
            if (indexOk && panIndex >= 0 && slotIt.value().toBool(false)) {
                panIndices.insert(panIndex);
            }
        }
        if (!panIndices.isEmpty()) {
            locked.insert(radioKey, panIndices);
        }
    }
    return locked;
}

// Deterministic (sorted) compact JSON, or an empty string when nothing is
// locked so the caller can remove the key instead of storing an empty object.
inline QString serialize(const LockedPans& locked)
{
    QJsonObject radios;
    QStringList radioKeys = locked.keys();
    std::sort(radioKeys.begin(), radioKeys.end());
    for (const QString& radioKey : radioKeys) {
        if (radioKey.trimmed().isEmpty()) {
            continue;
        }
        QList<int> panIndices = locked.value(radioKey).values();
        std::sort(panIndices.begin(), panIndices.end());
        QJsonObject panSlots;
        for (int panIndex : panIndices) {
            if (panIndex >= 0) {
                panSlots.insert(QString::number(panIndex), true);
            }
        }
        if (!panSlots.isEmpty()) {
            radios.insert(radioKey, panSlots);
        }
    }
    if (radios.isEmpty()) {
        return QString();
    }

    QJsonObject root;
    root.insert(QStringLiteral("version"), kSettingsVersion);
    root.insert(QStringLiteral("radios"), radios);
    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

} // namespace AetherSDR::ViewLockSettings
