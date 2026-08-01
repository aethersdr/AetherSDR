#include "BandStackSettings.h"

#include "AppSettings.h"
#include "SettingsPaths.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QStandardPaths>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>

#include <algorithm>

namespace AetherSDR {

namespace {

// Panel-wide preference keys (AppSettings; the legacy file's header fields).
const QString kExpiryKey = QStringLiteral("BandStackAutoExpiryMinutes");
const QString kGroupKey = QStringLiteral("BandStackGroupByBand");
const QString kDwellKey = QStringLiteral("BandStackAutoSaveDwellSeconds");
const QString kPrefsMigratedKey = QStringLiteral("BandStackPrefsMigrated");

QJsonObject entryToJson(const BandStackEntry& e)
{
    QJsonObject o;
    o.insert(QStringLiteral("frequencyMhz"), e.frequencyMhz);
    o.insert(QStringLiteral("mode"), e.mode);
    o.insert(QStringLiteral("filterLow"), e.filterLow);
    o.insert(QStringLiteral("filterHigh"), e.filterHigh);
    o.insert(QStringLiteral("rxAntenna"), e.rxAntenna);
    o.insert(QStringLiteral("txAntenna"), e.txAntenna);
    o.insert(QStringLiteral("agcMode"), e.agcMode);
    o.insert(QStringLiteral("agcThreshold"), e.agcThreshold);
    o.insert(QStringLiteral("audioGain"), e.audioGain);
    o.insert(QStringLiteral("nbOn"), e.nbOn);
    o.insert(QStringLiteral("nbLevel"), e.nbLevel);
    o.insert(QStringLiteral("nrOn"), e.nrOn);
    o.insert(QStringLiteral("nrLevel"), e.nrLevel);
    o.insert(QStringLiteral("wnbOn"), e.wnbOn);
    o.insert(QStringLiteral("wnbLevel"), e.wnbLevel);
    if (e.createdAtMs > 0) {
        o.insert(QStringLiteral("createdAtMs"), static_cast<double>(e.createdAtMs));
    }
    if (e.autoSaved) {
        o.insert(QStringLiteral("autoSaved"), true);
    }
    return o;
}

BandStackEntry entryFromJson(const QJsonObject& o)
{
    BandStackEntry e;
    e.frequencyMhz = o.value(QStringLiteral("frequencyMhz")).toDouble();
    e.mode = o.value(QStringLiteral("mode")).toString();
    e.filterLow = o.value(QStringLiteral("filterLow")).toInt();
    e.filterHigh = o.value(QStringLiteral("filterHigh")).toInt();
    e.rxAntenna = o.value(QStringLiteral("rxAntenna")).toString();
    e.txAntenna = o.value(QStringLiteral("txAntenna")).toString();
    e.agcMode = o.value(QStringLiteral("agcMode")).toString();
    e.agcThreshold = o.value(QStringLiteral("agcThreshold")).toInt();
    e.audioGain = o.value(QStringLiteral("audioGain")).toInt(50);
    e.nbOn = o.value(QStringLiteral("nbOn")).toBool();
    e.nbLevel = o.value(QStringLiteral("nbLevel")).toInt(50);
    e.nrOn = o.value(QStringLiteral("nrOn")).toBool();
    e.nrLevel = o.value(QStringLiteral("nrLevel")).toInt(50);
    e.wnbOn = o.value(QStringLiteral("wnbOn")).toBool();
    e.wnbLevel = o.value(QStringLiteral("wnbLevel")).toInt(50);
    e.createdAtMs =
        static_cast<qint64>(o.value(QStringLiteral("createdAtMs")).toDouble());
    e.autoSaved = o.value(QStringLiteral("autoSaved")).toBool();
    return e;
}

// The legacy XML side file, parsed whole: header preferences + per-section
// entry lists. Reader logic preserved verbatim from the XML era so migration
// reads exactly what that code wrote.
struct LegacyFile {
    int autoExpiryMinutes = 0;
    bool groupByBand = false;
    int autoSaveDwellSeconds = 0;
    QMap<QString, QVector<BandStackEntry>> sections;   // Radio_<sanitized> -> entries
    bool parsed = false;
};

LegacyFile parseLegacyFile(const QString& path)
{
    LegacyFile result;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return result;
    }

    QXmlStreamReader xml(&file);
    QString currentRadio;
    BandStackEntry currentEntry;
    bool inEntry = false;

    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement()) {
            const QString name = xml.name().toString();
            if (name == QLatin1String("BandStack")) {
                continue;
            } else if (name == QLatin1String("AutoExpiryMinutes")) {
                result.autoExpiryMinutes = xml.readElementText().toInt();
            } else if (name == QLatin1String("GroupByBand")) {
                result.groupByBand = xml.readElementText() == QLatin1String("True");
            } else if (name == QLatin1String("AutoSaveDwellSeconds")) {
                result.autoSaveDwellSeconds = xml.readElementText().toInt();
            } else if (name.startsWith(QLatin1String("Radio_"))) {
                currentRadio = name;
            } else if (name.startsWith(QLatin1String("Entry_"))
                       && !currentRadio.isEmpty()) {
                inEntry = true;
                currentEntry = BandStackEntry{};
            } else if (inEntry) {
                const QString text = xml.readElementText();
                if (name == QLatin1String("FrequencyMhz")) {
                    currentEntry.frequencyMhz = text.toDouble();
                } else if (name == QLatin1String("Mode")) {
                    currentEntry.mode = text;
                } else if (name == QLatin1String("FilterLow")) {
                    currentEntry.filterLow = text.toInt();
                } else if (name == QLatin1String("FilterHigh")) {
                    currentEntry.filterHigh = text.toInt();
                } else if (name == QLatin1String("RxAntenna")) {
                    currentEntry.rxAntenna = text;
                } else if (name == QLatin1String("TxAntenna")) {
                    currentEntry.txAntenna = text;
                } else if (name == QLatin1String("AgcMode")) {
                    currentEntry.agcMode = text;
                } else if (name == QLatin1String("AgcThreshold")) {
                    currentEntry.agcThreshold = text.toInt();
                } else if (name == QLatin1String("AudioGain")) {
                    currentEntry.audioGain = text.toInt();
                } else if (name == QLatin1String("NbOn")) {
                    currentEntry.nbOn = text == QLatin1String("True");
                } else if (name == QLatin1String("NbLevel")) {
                    currentEntry.nbLevel = text.toInt();
                } else if (name == QLatin1String("NrOn")) {
                    currentEntry.nrOn = text == QLatin1String("True");
                } else if (name == QLatin1String("NrLevel")) {
                    currentEntry.nrLevel = text.toInt();
                } else if (name == QLatin1String("WnbOn")) {
                    currentEntry.wnbOn = text == QLatin1String("True");
                } else if (name == QLatin1String("WnbLevel")) {
                    currentEntry.wnbLevel = text.toInt();
                } else if (name == QLatin1String("CreatedAtMs")) {
                    currentEntry.createdAtMs = text.toLongLong();
                } else if (name == QLatin1String("AutoSaved")) {
                    currentEntry.autoSaved = text == QLatin1String("True");
                }
            }
        } else if (xml.isEndElement()) {
            const QString name = xml.name().toString();
            if (name.startsWith(QLatin1String("Entry_")) && inEntry) {
                result.sections[currentRadio].append(currentEntry);
                inEntry = false;
            } else if (name.startsWith(QLatin1String("Radio_"))) {
                currentRadio.clear();
            }
        }
    }

    if (xml.hasError()) {
        qWarning() << "BandStackSettings: legacy XML parse error:"
                   << xml.errorString();
        return result;
    }
    result.parsed = true;
    return result;
}

// Rewrite the legacy file WITHOUT the claimed section (or delete the file
// outright when no sections remain). The header preferences are preserved so
// a downgrade still finds them.
void rewriteLegacyWithout(const QString& path, const LegacyFile& legacy,
                          const QString& claimedSection)
{
    QMap<QString, QVector<BandStackEntry>> remaining = legacy.sections;
    remaining.remove(claimedSection);

    if (remaining.isEmpty()) {
        QFile::remove(path);
        QFile::remove(path + QStringLiteral(".tmp"));
        qDebug() << "BandStackSettings: legacy side file retired —"
                    " every radio section has been migrated";
        return;
    }

    const QString tmpPath = path + QStringLiteral(".tmp");
    QFile file(tmpPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qWarning() << "BandStackSettings: cannot rewrite legacy file" << tmpPath;
        return;
    }
    QXmlStreamWriter xml(&file);
    xml.setAutoFormatting(true);
    xml.setAutoFormattingIndent(2);
    xml.writeStartDocument();
    xml.writeStartElement(QStringLiteral("BandStack"));
    xml.writeTextElement(QStringLiteral("AutoExpiryMinutes"),
                         QString::number(legacy.autoExpiryMinutes));
    xml.writeTextElement(QStringLiteral("GroupByBand"),
                         legacy.groupByBand ? QStringLiteral("True")
                                            : QStringLiteral("False"));
    xml.writeTextElement(QStringLiteral("AutoSaveDwellSeconds"),
                         QString::number(legacy.autoSaveDwellSeconds));
    for (auto it = remaining.constBegin(); it != remaining.constEnd(); ++it) {
        if (it.value().isEmpty()) {
            continue;
        }
        xml.writeStartElement(it.key());
        for (int i = 0; i < it.value().size(); ++i) {
            const BandStackEntry& e = it.value().at(i);
            xml.writeStartElement(QStringLiteral("Entry_%1").arg(i));
            xml.writeTextElement(QStringLiteral("FrequencyMhz"),
                                 QString::number(e.frequencyMhz, 'f', 6));
            xml.writeTextElement(QStringLiteral("Mode"), e.mode);
            xml.writeTextElement(QStringLiteral("FilterLow"),
                                 QString::number(e.filterLow));
            xml.writeTextElement(QStringLiteral("FilterHigh"),
                                 QString::number(e.filterHigh));
            xml.writeTextElement(QStringLiteral("RxAntenna"), e.rxAntenna);
            xml.writeTextElement(QStringLiteral("TxAntenna"), e.txAntenna);
            xml.writeTextElement(QStringLiteral("AgcMode"), e.agcMode);
            xml.writeTextElement(QStringLiteral("AgcThreshold"),
                                 QString::number(e.agcThreshold));
            xml.writeTextElement(QStringLiteral("AudioGain"),
                                 QString::number(e.audioGain));
            xml.writeTextElement(QStringLiteral("NbOn"),
                                 e.nbOn ? QStringLiteral("True")
                                        : QStringLiteral("False"));
            xml.writeTextElement(QStringLiteral("NbLevel"),
                                 QString::number(e.nbLevel));
            xml.writeTextElement(QStringLiteral("NrOn"),
                                 e.nrOn ? QStringLiteral("True")
                                        : QStringLiteral("False"));
            xml.writeTextElement(QStringLiteral("NrLevel"),
                                 QString::number(e.nrLevel));
            xml.writeTextElement(QStringLiteral("WnbOn"),
                                 e.wnbOn ? QStringLiteral("True")
                                         : QStringLiteral("False"));
            xml.writeTextElement(QStringLiteral("WnbLevel"),
                                 QString::number(e.wnbLevel));
            if (e.createdAtMs > 0) {
                xml.writeTextElement(QStringLiteral("CreatedAtMs"),
                                     QString::number(e.createdAtMs));
            }
            if (e.autoSaved) {
                xml.writeTextElement(QStringLiteral("AutoSaved"),
                                     QStringLiteral("True"));
            }
            xml.writeEndElement();  // Entry_N
        }
        xml.writeEndElement();  // Radio_XXXX
    }
    xml.writeEndElement();  // BandStack
    xml.writeEndDocument();
    file.close();

    QFile::remove(path);
    QFile::rename(tmpPath, path);
}

} // namespace

BandStackSettings::BandStackSettings()
{
    // Through SettingsPaths, not raw QStandardPaths: the settings dir has ONE
    // source of truth (and the AETHER_SETTINGS_DIR test-isolation override
    // must reach this file too — found by bandstack_scoped_test landing its
    // fixture in a different directory than this constructor looked in).
    m_legacyFilePath =
        SettingsPaths::configDir() + QStringLiteral("/BandStack.settings");
}

BandStackSettings& BandStackSettings::instance()
{
    static BandStackSettings inst;
    return inst;
}

QString BandStackSettings::sanitizeSerial(const QString& serial)
{
    // XML element names: ^[A-Za-z_][A-Za-z0-9_]*$ — the legacy convention.
    QString s = serial;
    s.replace(QLatin1Char('-'), QLatin1Char('_'));
    s.replace(QLatin1Char(' '), QLatin1Char('_'));
    return QStringLiteral("Radio_") + s;
}

void BandStackSettings::load()
{
    // One-shot: the legacy file's panel-wide preferences move to AppSettings.
    auto& settings = AppSettings::instance();
    if (settings.value(kPrefsMigratedKey).toString() == QLatin1String("True")) {
        return;
    }
    const LegacyFile legacy = parseLegacyFile(m_legacyFilePath);
    if (legacy.parsed) {
        if (!settings.contains(kExpiryKey)) {
            settings.setValue(kExpiryKey,
                              QString::number(legacy.autoExpiryMinutes));
        }
        if (!settings.contains(kGroupKey)) {
            settings.setValue(kGroupKey, legacy.groupByBand
                                             ? QStringLiteral("True")
                                             : QStringLiteral("False"));
        }
        if (!settings.contains(kDwellKey)) {
            settings.setValue(kDwellKey,
                              QString::number(legacy.autoSaveDwellSeconds));
        }
    }
    // Stamp only when the migration actually happened (file parsed) or there
    // is genuinely nothing to migrate — a transiently unreadable side file
    // must retry next launch, matching ensureScopeMigrated()'s behavior for
    // the entries in the very same file (PR #4621 review, Ozy311).
    if (legacy.parsed || !QFile::exists(m_legacyFilePath)) {
        settings.setValue(kPrefsMigratedKey, QStringLiteral("True"));
    }
    settings.save();
}

void BandStackSettings::ensureScopeMigrated(const RadioSettingsScope& scope)
{
    if (!scope.isValid() || scope.radioId().isEmpty()) {
        return;
    }
    const QString memo = scope.family() + QLatin1Char('/') + scope.radioId();
    if (m_scopesChecked.contains(memo)) {
        return;
    }

    // Already migrated (or born scoped): the document exists. This includes a
    // stack the operator deliberately EMPTIED — {"entries": []} is a present
    // document, distinguishable from an absent row, and it must block
    // re-import (no bookmark resurrection from a restored side file).
    if (!scope.featureExact(featureName()).isEmpty()) {
        m_scopesChecked.insert(memo);
        return;
    }
    // NO memo when there is nothing to claim yet: a side file restored from a
    // backup mid-session must still be claimable on this radio's next access
    // (PR #4621 review, Ozy311).
    if (!QFile::exists(m_legacyFilePath)) {
        return;
    }
    const LegacyFile legacy = parseLegacyFile(m_legacyFilePath);
    if (!legacy.parsed) {
        return;   // unreadable legacy file: leave it, retry next access
    }
    const QString section = sanitizeSerial(scope.radioId());
    if (!legacy.sections.contains(section)) {
        m_scopesChecked.insert(memo);
        return;
    }
    if (!writeEntries(scope, legacy.sections.value(section))) {
        // The claim failed (read-only store?): leave the legacy section in
        // place so a later access can retry.
        return;
    }
    m_scopesChecked.insert(memo);
    rewriteLegacyWithout(m_legacyFilePath, legacy, section);
    qDebug() << "BandStackSettings: migrated" << legacy.sections.value(section).size()
             << "bookmarks for" << scope.family() << scope.radioId()
             << "into the scoped store";
}

QVector<BandStackEntry> BandStackSettings::readEntries(const RadioSettingsScope& scope)
{
    QVector<BandStackEntry> out;
    // The class already decided an empty radioId is not a BandStack target
    // (ensureScopeMigrated guards it); the readers/writers must agree, or a
    // mutation landing before the serial is known would write one radio's
    // bookmarks as the FAMILY-WIDE default row (PR #4621 review, Ozy311).
    if (!scope.isValid() || scope.radioId().isEmpty()) {
        return out;
    }
    const QJsonArray array =
        scope.featureExact(featureName()).value(QStringLiteral("entries")).toArray();
    out.reserve(array.size());
    for (const QJsonValue& value : array) {
        out.append(entryFromJson(value.toObject()));
    }
    return out;
}

bool BandStackSettings::writeEntries(const RadioSettingsScope& scope,
                                     const QVector<BandStackEntry>& entries)
{
    if (!scope.isValid() || scope.radioId().isEmpty()) {
        qWarning() << "BandStackSettings: refusing a bookmark write without a"
                      " radio identity (would create a family-wide row)";
        return false;
    }
    QJsonArray array;
    for (const BandStackEntry& e : entries) {
        array.append(entryToJson(e));
    }
    return scope.setFeature(featureName(), kSchemaVersion,
                            QJsonObject{{QStringLiteral("entries"), array}});
}

QVector<BandStackEntry> BandStackSettings::entries(const RadioSettingsScope& scope)
{
    ensureScopeMigrated(scope);
    return readEntries(scope);
}

void BandStackSettings::addEntry(const RadioSettingsScope& scope,
                                 const BandStackEntry& entry)
{
    ensureScopeMigrated(scope);
    QVector<BandStackEntry> all = readEntries(scope);
    all.append(entry);
    if (!writeEntries(scope, all)) {
        // The write-through IS the durability story — a refused write must be
        // loud, not a bookmark that silently repaints as absent on the next
        // panel reload (PR #4621 review, Ozy311).
        qWarning() << "BandStackSettings: bookmark add did NOT persist for"
                   << scope.family() << scope.radioId()
                   << "— the settings store refused the write";
    }
}

void BandStackSettings::removeEntry(const RadioSettingsScope& scope, int index)
{
    ensureScopeMigrated(scope);
    QVector<BandStackEntry> all = readEntries(scope);
    if (index < 0 || index >= all.size()) {
        return;
    }
    all.removeAt(index);
    if (!writeEntries(scope, all)) {
        qWarning() << "BandStackSettings: bookmark removal did NOT persist for"
                   << scope.family() << scope.radioId();
    }
}

void BandStackSettings::clearAllEntries(const RadioSettingsScope& scope)
{
    ensureScopeMigrated(scope);
    if (!writeEntries(scope, {})) {
        qWarning() << "BandStackSettings: bookmark clear did NOT persist for"
                   << scope.family() << scope.radioId();
    }
}

void BandStackSettings::clearBandEntries(const RadioSettingsScope& scope,
                                         double lowMhz, double highMhz)
{
    ensureScopeMigrated(scope);
    QVector<BandStackEntry> all = readEntries(scope);
    const int before = all.size();
    all.erase(std::remove_if(all.begin(), all.end(),
                             [lowMhz, highMhz](const BandStackEntry& e) {
                                 return e.frequencyMhz >= lowMhz
                                        && e.frequencyMhz <= highMhz;
                             }),
              all.end());
    if (all.size() != before && !writeEntries(scope, all)) {
        qWarning() << "BandStackSettings: band clear did NOT persist for"
                   << scope.family() << scope.radioId();
    }
}

int BandStackSettings::removeExpiredEntries(const RadioSettingsScope& scope,
                                            qint64 maxAgeMs)
{
    ensureScopeMigrated(scope);
    QVector<BandStackEntry> all = readEntries(scope);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const int before = all.size();
    all.erase(std::remove_if(all.begin(), all.end(),
                             [now, maxAgeMs](const BandStackEntry& e) {
                                 // Legacy entries (createdAtMs == 0) never expire.
                                 return e.createdAtMs > 0
                                        && (now - e.createdAtMs) > maxAgeMs;
                             }),
              all.end());
    const int removed = before - all.size();
    if (removed > 0 && !writeEntries(scope, all)) {
        // The caller acts on this count (MainWindow reloads the panel) — a
        // prune that did not persist must report 0, or the panel repaints
        // the expired entries right back (PR #4621 review, Ozy311).
        qWarning() << "BandStackSettings: expiry prune did NOT persist for"
                   << scope.family() << scope.radioId();
        return 0;
    }
    return removed;
}

int BandStackSettings::autoExpiryMinutes() const
{
    return AppSettings::instance().value(kExpiryKey, QStringLiteral("0"))
        .toString()
        .toInt();
}

void BandStackSettings::setAutoExpiryMinutes(int minutes)
{
    AppSettings::instance().setValue(kExpiryKey, QString::number(minutes));
    AppSettings::instance().save();
}

bool BandStackSettings::groupByBand() const
{
    return AppSettings::instance().value(kGroupKey, QStringLiteral("False"))
               .toString()
           == QLatin1String("True");
}

void BandStackSettings::setGroupByBand(bool grouped)
{
    AppSettings::instance().setValue(
        kGroupKey, grouped ? QStringLiteral("True") : QStringLiteral("False"));
    AppSettings::instance().save();
}

int BandStackSettings::autoSaveDwellSeconds() const
{
    return AppSettings::instance().value(kDwellKey, QStringLiteral("0"))
        .toString()
        .toInt();
}

void BandStackSettings::setAutoSaveDwellSeconds(int seconds)
{
    AppSettings::instance().setValue(kDwellKey, QString::number(seconds));
    AppSettings::instance().save();
}

} // namespace AetherSDR
