#include "SettingsBootstrap.h"

#include "SettingsDatabase.h"
#include "SettingsPaths.h"

#include <QFile>
#include <QXmlStreamReader>

namespace AetherSDR {
namespace SettingsBootstrap {

namespace {

// Legacy-XML fallback: scan the flat <Settings> document for one element.
// Mirrors the parse rules of the XML-era AppSettings reader (top-level
// elements only; the per-station section is named by the station and is not
// scanned — no bootstrap consumer reads station-scoped keys).
bool readFromLegacyXml(const QString& path, const QString& key, QString& value)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return false;
    }
    QXmlStreamReader xml(&file);
    int depth = 0;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement()) {
            ++depth;
            if (depth == 2 && xml.name().toString() == key) {
                value = xml.readElementText();
                return true;
            }
            // readElementText() consumes the matching end element; otherwise
            // skip nested content (the station section) without descending.
            if (depth == 2) {
                xml.skipCurrentElement();
                --depth;
            }
        } else if (xml.isEndElement()) {
            --depth;
        }
    }
    return false;
}

} // namespace

QString readValue(const QString& key, const QString& defaultValue)
{
    QString value;
    const QString dbPath = SettingsPaths::databasePath();
    if (QFile::exists(dbPath)
        && SettingsDatabase::readAppValueFromFile(dbPath, key, value)) {
        return value;
    }
    if (readFromLegacyXml(SettingsPaths::legacyXmlPath(), key, value)) {
        return value;
    }
    return defaultValue;
}

} // namespace SettingsBootstrap
} // namespace AetherSDR
