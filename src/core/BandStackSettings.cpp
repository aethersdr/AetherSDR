#include "BandStackSettings.h"

#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>
#include <QDateTime>
#include <QDebug>

#include <algorithm>

namespace MasterSDR {

BandStackSettings::BandStackSettings()
{
    m_filePath = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
                 + "/MasterSDR/BandStack.settings";
}

BandStackSettings& BandStackSettings::instance()
{
    static BandStackSettings inst;
    return inst;
}

QString BandStackSettings::sanitizeSerial(const QString& serial)
{
    // XML element names: ^[A-Za-z_][A-Za-z0-9_]*$
    QString s = serial;
    s.replace('-', '_');
    s.replace(' ', '_');
    return "Radio_" + s;
}

QVector<BandStackEntry> BandStackSettings::entries(const QString& radioSerial) const
{
    return m_entries.value(sanitizeSerial(radioSerial));
}

void BandStackSettings::addEntry(const QString& radioSerial, const BandStackEntry& entry)
{
    m_entries[sanitizeSerial(radioSerial)].append(entry);
}

void BandStackSettings::removeEntry(const QString& radioSerial, int index)
{
    QString key = sanitizeSerial(radioSerial);
    if (!m_entries.contains(key)) return;
    auto& vec = m_entries[key];
    if (index >= 0 && index < vec.size()) {
        vec.removeAt(index);
    }
}

void BandStackSettings::clearAllEntries(const QString& radioSerial)
{
    QString key = sanitizeSerial(radioSerial);
    m_entries[key].clear();
}

void BandStackSettings::clearBandEntries(const QString& radioSerial,
                                         double lowMhz, double highMhz)
{
    QString key = sanitizeSerial(radioSerial);
    if (!m_entries.contains(key)) return;
    auto& vec = m_entries[key];
    vec.erase(std::remove_if(vec.begin(), vec.end(),
        [lowMhz, highMhz](const BandStackEntry& e) {
            return e.frequencyMhz >= lowMhz && e.frequencyMhz <= highMhz;
        }), vec.end());
}

int BandStackSettings::removeExpiredEntries(const QString& radioSerial,
                                            qint64 maxAgeMs)
{
    QString key = sanitizeSerial(radioSerial);
    if (!m_entries.contains(key)) return 0;
    auto& vec = m_entries[key];
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    int before = vec.size();
    vec.erase(std::remove_if(vec.begin(), vec.end(),
        [now, maxAgeMs](const BandStackEntry& e) {
            // Legacy entries (createdAtMs == 0) never expire
            return e.createdAtMs > 0 && (now - e.createdAtMs) > maxAgeMs;
        }), vec.end());
    return before - vec.size();
}

void BandStackSettings::load()
{
    m_entries.clear();

    QFile file(m_filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return;  // no file yet — first run
    }

    QXmlStreamReader xml(&file);

    // Structure: <BandStack> <Radio_XXXX> <Entry_N> <FrequencyMhz>... </Entry_N> </Radio_XXXX> </BandStack>
    QString currentRadio;
    BandStackEntry currentEntry;
    bool inEntry = false;

    while (!xml.atEnd()) {
        xml.readNext();

        if (xml.isStartElement()) {
            const QString name = xml.name().toString();

            if (name == "BandStack") {
                continue;
            } else if (name == "AutoExpiryMinutes") {
                m_autoExpiryMinutes = xml.readElementText().toInt();
            } else if (name == "GroupByBand") {
                m_groupByBand = xml.readElementText() == "True";
            } else if (name == "AutoSaveDwellSeconds") {
                m_autoSaveDwellSeconds = xml.readElementText().toInt();
            } else if (name.startsWith("Radio_")) {
                currentRadio = name;
            } else if (name.startsWith("Entry_") && !currentRadio.isEmpty()) {
                inEntry = true;
                currentEntry = BandStackEntry{};
            } else if (inEntry) {
                // Read field value
                QString text = xml.readElementText();
                if (name == "FrequencyMhz") {
                    currentEntry.frequencyMhz = text.toDouble();
                } else if (name == "Mode") {
                    currentEntry.mode = text;
                } else if (name == "FilterLow") {
                    currentEntry.filterLow = text.toInt();
                } else if (name == "FilterHigh") {
                    currentEntry.filterHigh = text.toInt();
                } else if (name == "RxAntenna") {
                    currentEntry.rxAntenna = text;
                } else if (name == "TxAntenna") {
                    currentEntry.txAntenna = text;
                } else if (name == "AgcMode") {
                    currentEntry.agcMode = text;
                } else if (name == "AgcThreshold") {
                    currentEntry.agcThreshold = text.toInt();
                } else if (name == "AudioGain") {
                    currentEntry.audioGain = text.toInt();
                } else if (name == "NbOn") {
                    currentEntry.nbOn = text == "True";
                } else if (name == "NbLevel") {
                    currentEntry.nbLevel = text.toInt();
                } else if (name == "NrOn") {
                    currentEntry.nrOn = text == "True";
                } else if (name == "NrLevel") {
                    currentEntry.nrLevel = text.toInt();
                } else if (name == "WnbOn") {
                    currentEntry.wnbOn = text == "True";
                } else if (name == "WnbLevel") {
                    currentEntry.wnbLevel = text.toInt();
                } else if (name == "CreatedAtMs") {
                    currentEntry.createdAtMs = text.toLongLong();
                } else if (name == "AutoSaved") {
                    currentEntry.autoSaved = text == "True";
                }
            }
        } else if (xml.isEndElement()) {
            const QString name = xml.name().toString();
            if (name.startsWith("Entry_") && inEntry) {
                m_entries[currentRadio].append(currentEntry);
                inEntry = false;
            } else if (name.startsWith("Radio_")) {
                currentRadio.clear();
            }
        }
    }

    if (xml.hasError()) {
        qWarning() << "BandStackSettings: XML parse error:" << xml.errorString();
    }
}

void BandStackSettings::save()
{
    QDir().mkpath(QFileInfo(m_filePath).absolutePath());

    const QString tmpPath = m_filePath + ".tmp";
    QFile file(tmpPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qWarning() << "BandStackSettings: cannot write" << tmpPath;
        return;
    }

    QXmlStreamWriter xml(&file);
    xml.setAutoFormatting(true);
    xml.setAutoFormattingIndent(2);
    xml.writeStartDocument();
    xml.writeStartElement("BandStack");

    // Global settings
    xml.writeTextElement("AutoExpiryMinutes", QString::number(m_autoExpiryMinutes));
    xml.writeTextElement("GroupByBand", m_groupByBand ? "True" : "False");
    xml.writeTextElement("AutoSaveDwellSeconds", QString::number(m_autoSaveDwellSeconds));

    QList<QString> radios = m_entries.keys();
    std::sort(radios.begin(), radios.end());

    for (const QString& radio : radios) {
        const auto& vec = m_entries[radio];
        if (vec.isEmpty()) continue;

        xml.writeStartElement(radio);
        for (int i = 0; i < vec.size(); ++i) {
            const BandStackEntry& e = vec[i];
            xml.writeStartElement(QString("Entry_%1").arg(i));
            xml.writeTextElement("FrequencyMhz", QString::number(e.frequencyMhz, 'f', 6));
            xml.writeTextElement("Mode", e.mode);
            xml.writeTextElement("FilterLow", QString::number(e.filterLow));
            xml.writeTextElement("FilterHigh", QString::number(e.filterHigh));
            xml.writeTextElement("RxAntenna", e.rxAntenna);
            xml.writeTextElement("TxAntenna", e.txAntenna);
            xml.writeTextElement("AgcMode", e.agcMode);
            xml.writeTextElement("AgcThreshold", QString::number(e.agcThreshold));
            xml.writeTextElement("AudioGain", QString::number(e.audioGain));
            xml.writeTextElement("NbOn", e.nbOn ? "True" : "False");
            xml.writeTextElement("NbLevel", QString::number(e.nbLevel));
            xml.writeTextElement("NrOn", e.nrOn ? "True" : "False");
            xml.writeTextElement("NrLevel", QString::number(e.nrLevel));
            xml.writeTextElement("WnbOn", e.wnbOn ? "True" : "False");
            xml.writeTextElement("WnbLevel", QString::number(e.wnbLevel));
            if (e.createdAtMs > 0) {
                xml.writeTextElement("CreatedAtMs", QString::number(e.createdAtMs));
            }
            if (e.autoSaved) {
                xml.writeTextElement("AutoSaved", "True");
            }
            xml.writeEndElement();  // Entry_N
        }
        xml.writeEndElement();  // Radio_XXXX
    }

    xml.writeEndElement();  // BandStack
    xml.writeEndDocument();
    file.close();

    // Atomic rename
    QFile::remove(m_filePath);
    QFile::rename(tmpPath, m_filePath);
}

} // namespace MasterSDR
