#pragma once

#include "AppSettings.h"

#include <QJsonObject>
#include <QString>

namespace AetherSDR {

// A (family, radio) handle into the radio-scoped feature-document store
// (RFC #4603 proposal A). RadioModel exposes the scope for the connected
// radio; feature code holds the scope instead of re-deriving identity, so
// "which radio does this state belong to" is decided in exactly one place.
//
// radioId is the family's canonical identity — Flex serial, HL2 MAC (from
// Hl2Discovery::macToSerial()), Kiwi profile UUID. An empty radioId reads and
// writes the family-wide default rows. Reads fall back:
// exact radio → family-wide → empty document.
class RadioSettingsScope {
public:
    RadioSettingsScope() = default;
    RadioSettingsScope(QString family, QString radioId)
        : m_family(std::move(family)), m_radioId(std::move(radioId))
    {
    }

    bool isValid() const { return !m_family.isEmpty(); }
    QString family() const { return m_family; }
    QString radioId() const { return m_radioId; }

    QJsonObject feature(const QString& name, int* schemaVersionOut = nullptr) const
    {
        if (!isValid()) {
            return {};
        }
        return AppSettings::instance().radioFeature(m_family, m_radioId, name,
                                                    schemaVersionOut);
    }

    // Exact-row read, no family-wide fallback — for writers judging the row
    // they are about to replace (PR #4614 review).
    QJsonObject featureExact(const QString& name,
                             int* schemaVersionOut = nullptr) const
    {
        if (!isValid()) {
            return {};
        }
        return AppSettings::instance().radioFeatureExact(m_family, m_radioId,
                                                         name, schemaVersionOut);
    }

    bool setFeature(const QString& name, int schemaVersion,
                    const QJsonObject& doc) const
    {
        if (!isValid()) {
            return false;
        }
        return AppSettings::instance().setRadioFeature(m_family, m_radioId, name,
                                                       schemaVersion, doc);
    }

    bool removeFeature(const QString& name) const
    {
        if (!isValid()) {
            return false;
        }
        return AppSettings::instance().removeRadioFeature(m_family, m_radioId,
                                                          name);
    }

private:
    QString m_family;
    QString m_radioId;
};

} // namespace AetherSDR
