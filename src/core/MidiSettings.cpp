#ifdef HAVE_MIDI

#include "MidiSettings.h"

#include <QDir>
#include <QFile>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>
#include <QStandardPaths>
#include <QDebug>

namespace MasterSDR {

namespace {

QString normalizedMidiParamId(const QString& paramId)
{
    if (paramId == QLatin1String("cw.key"))
        return QStringLiteral("cwkey");
    if (paramId == QLatin1String("cw.dit"))
        return QStringLiteral("cwdit");
    if (paramId == QLatin1String("cw.dah"))
        return QStringLiteral("cwdah");
    return paramId;
}

} // namespace

MidiSettings& MidiSettings::instance()
{
    static MidiSettings s;
    return s;
}

QString MidiSettings::settingsFilePath() const
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
           + "/MasterSDR/midi.settings";
}

QString MidiSettings::profileDir() const
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
           + "/MasterSDR/midi";
}

// ── Load / Save ─────────────────────────────────────────────────────────────

void MidiSettings::load()
{
    QFile file(settingsFilePath());
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return;

    QXmlStreamReader xml(&file);
    while (!xml.atEnd()) {
        xml.readNext();
        if (!xml.isStartElement()) continue;
        if (xml.name() == u"LastDevice")
            m_lastDevice = xml.readElementText();
        else if (xml.name() == u"AutoConnect")
            m_autoConnect = (xml.readElementText() == "True");
    }
    qDebug() << "MidiSettings: loaded from" << settingsFilePath()
             << "device:" << m_lastDevice << "autoConnect:" << m_autoConnect;
}

void MidiSettings::save()
{
    QString path = settingsFilePath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    auto bindings = loadBindings();

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qWarning() << "MidiSettings: failed to save" << path;
        return;
    }

    QXmlStreamWriter xml(&file);
    xml.setAutoFormatting(true);
    xml.writeStartDocument();
    xml.writeStartElement("MidiSettings");

    xml.writeTextElement("LastDevice", m_lastDevice);
    xml.writeTextElement("AutoConnect", m_autoConnect ? "True" : "False");

    // Write bindings inline in the main settings file
    xml.writeStartElement("Bindings");
    for (const auto& b : bindings) {
        xml.writeStartElement("Binding");
        xml.writeAttribute("param", normalizedMidiParamId(b.paramId));
        xml.writeAttribute("channel", QString::number(b.channel));
        xml.writeAttribute("type", QString::number(static_cast<int>(b.msgType)));
        xml.writeAttribute("number", QString::number(b.number));
        xml.writeAttribute("inverted", b.inverted ? "True" : "False");
        if (b.relative) xml.writeAttribute("relative", "True");
        xml.writeEndElement();
    }
    xml.writeEndElement(); // Bindings

    xml.writeEndElement(); // MidiSettings
    xml.writeEndDocument();
}

// ── Bindings ────────────────────────────────────────────────────────────────

QVector<MidiBinding> MidiSettings::loadBindings() const
{
    return parseBindingsFromXml(settingsFilePath());
}

void MidiSettings::saveBindings(const QVector<MidiBinding>& bindings)
{
    // Re-save the full file with updated bindings
    QString path = settingsFilePath();
    QDir().mkpath(QFileInfo(path).absolutePath());

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) return;

    QXmlStreamWriter xml(&file);
    xml.setAutoFormatting(true);
    xml.writeStartDocument();
    xml.writeStartElement("MidiSettings");

    xml.writeTextElement("LastDevice", m_lastDevice);
    xml.writeTextElement("AutoConnect", m_autoConnect ? "True" : "False");

    xml.writeStartElement("Bindings");
    for (const auto& b : bindings) {
        xml.writeStartElement("Binding");
        xml.writeAttribute("param", normalizedMidiParamId(b.paramId));
        xml.writeAttribute("channel", QString::number(b.channel));
        xml.writeAttribute("type", QString::number(static_cast<int>(b.msgType)));
        xml.writeAttribute("number", QString::number(b.number));
        xml.writeAttribute("inverted", b.inverted ? "True" : "False");
        if (b.relative) xml.writeAttribute("relative", "True");
        xml.writeEndElement();
    }
    xml.writeEndElement();

    xml.writeEndElement();
    xml.writeEndDocument();
}

QVector<MidiBinding> MidiSettings::parseBindingsFromXml(const QString& filePath)
{
    QVector<MidiBinding> result;
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return result;

    QXmlStreamReader xml(&file);
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement() && xml.name() == u"Binding") {
            MidiBinding b;
            b.paramId  = normalizedMidiParamId(xml.attributes().value("param").toString());
            b.channel  = xml.attributes().value("channel").toInt();
            b.msgType  = static_cast<MidiBinding::MsgType>(
                             xml.attributes().value("type").toInt());
            b.number   = xml.attributes().value("number").toInt();
            b.inverted = (xml.attributes().value("inverted") == u"True");
            b.relative = (xml.attributes().value("relative") == u"True");
            if (!b.paramId.isEmpty())
                result.append(b);
        }
    }
    return result;
}

void MidiSettings::writeBindingsToXml(const QString& filePath,
                                       const QVector<MidiBinding>& bindings)
{
    QDir().mkpath(QFileInfo(filePath).absolutePath());
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) return;

    QXmlStreamWriter xml(&file);
    xml.setAutoFormatting(true);
    xml.writeStartDocument();
    xml.writeStartElement("MidiProfile");
    for (const auto& b : bindings) {
        xml.writeStartElement("Binding");
        xml.writeAttribute("param", normalizedMidiParamId(b.paramId));
        xml.writeAttribute("channel", QString::number(b.channel));
        xml.writeAttribute("type", QString::number(static_cast<int>(b.msgType)));
        xml.writeAttribute("number", QString::number(b.number));
        xml.writeAttribute("inverted", b.inverted ? "True" : "False");
        if (b.relative) xml.writeAttribute("relative", "True");
        xml.writeEndElement();
    }
    xml.writeEndElement();
    xml.writeEndDocument();
}

// ── Profiles ────────────────────────────────────────────────────────────────

QStringList MidiSettings::availableProfiles() const
{
    QDir dir(profileDir());
    QStringList result;
    for (const auto& fi : dir.entryInfoList({"*.xml"}, QDir::Files))
        result.append(fi.baseName());
    return result;
}

void MidiSettings::saveProfile(const QString& name,
                                const QVector<MidiBinding>& bindings)
{
    writeBindingsToXml(profileDir() + "/" + name + ".xml", bindings);
}

QVector<MidiBinding> MidiSettings::loadProfile(const QString& name) const
{
    return parseBindingsFromXml(profileDir() + "/" + name + ".xml");
}

void MidiSettings::deleteProfile(const QString& name)
{
    QFile::remove(profileDir() + "/" + name + ".xml");
}

} // namespace MasterSDR

#endif // HAVE_MIDI
