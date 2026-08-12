#ifdef HAVE_MIDI

#include "MidiSettings.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QSet>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>
#include <QStandardPaths>
#include <QDebug>

namespace AetherSDR {

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

// One serializer for the <MidiProfile> document, shared by the store's
// profile writer and the user-facing export so the two can never drift.
void writeProfileDocument(QXmlStreamWriter& xml, const QVector<MidiBinding>& bindings)
{
    xml.setAutoFormatting(true);
    xml.writeStartDocument();
    xml.writeStartElement("MidiProfile");
    // Schema hook for files in circulation, cheap now and painful to
    // retrofit — the .map's FORMAT_VERSION analogue.
    xml.writeAttribute("version", "1");
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

// ── SmartSDR iOS/Mac ".map" import ──────────────────────────────────────────
//
// The ".map" is the mapping file consumed by the SmartSDR iOS/Mac family's
// own Import-map tool and published per device by controller vendors
// (e.g. Lynovation's CTR2 config packages). Plain ASCII, "#"-headed
// sections, one assignment per line:
//
//     # Controls
//     C100=freq;active          C<CC#> = <function>[;flags]
//     # Buttons
//     B20=leftpaddle            B<note#> = <function>[;flags]
//     # LEDs                    (device feedback — not bindings)
//
// The function vocabulary is SmartSDR's; this table carries the verified
// CTR2-MIDI + CTR2-Quad vocabulary. Functions absent here are reported as
// named skips, so growing coverage is a data change only. relativeCc marks
// functions whose CC values are relative steps (the VFO knob) rather than
// absolute levels.
struct MapFunctionEntry {
    const char* function;
    const char* paramId;
    bool relativeCc;
};

constexpr MapFunctionEntry kSmartSdrMapFunctions[] = {
    { "agct",          "rx.agcThreshold",   false },
    { "anf",           "rx.anfEnable",      false },
    { "atu",           "tx.atuStart",       false },
    { "balanceslice",  "rx.audioPan",       false },
    { "banddown",      "global.bandDown",   false },
    { "bandup",        "global.bandUp",     false },
    { "cwspeed",       "cw.speed",          false },
    { "freq",          "rx.tuneKnob",       true  },
    { "leftpaddle",    "cwdit",             false },
    { "mainmute",      "global.masterMute", false },
    { "micgain",       "phone.micLevel",    false },
    { "modenext",      "global.modeUp",     false },
    { "modeprev",      "global.modeDown",   false },
    { "nb",            "rx.nbEnable",       false },
    { "nr",            "rx.nrEnable",       false },
    { "power",         "tx.rfPower",        false },
    { "ptt",           "cw.ptt",            false },
    { "rightpaddle",   "cwdah",             false },
    { "rit",           "rx.ritEnable",      false },
    { "slicevolume",   "rx.afGain",         false },
    { "straightkey",   "cwkey",             false },
    { "togglebandzoom","global.bandZoom",   false },
    { "tune",          "tx.tune",           false },
    { "tunestep",      "rx.stepUp",         false },
    { "tunestepprev",  "rx.stepDown",       false },
    { "vox",           "phone.voxEnable",   false },
    { "xit",           "rx.xitEnable",      false },
    { "zoomin",        "global.panZoomIn",  false },
    { "zoomout",       "global.panZoomOut", false },
};

// Message-type name for skip/duplicate reports.  Spelled out here rather than
// calling MidiBinding::sourceDisplayName(), which lives in
// MidiControlManager.cpp — midi_settings_test links this file alone.
QString midiMsgTypeName(MidiBinding::MsgType type)
{
    switch (type) {
    case MidiBinding::CC:        return QStringLiteral("CC");
    case MidiBinding::NoteOn:    return QStringLiteral("Note On");
    case MidiBinding::NoteOff:   return QStringLiteral("Note Off");
    case MidiBinding::PitchBend: return QStringLiteral("Pitch Bend");
    }
    return QStringLiteral("message");
}

const MapFunctionEntry* findMapFunction(const QString& function)
{
    for (const auto& entry : kSmartSdrMapFunctions) {
        if (function.compare(QLatin1String(entry.function), Qt::CaseInsensitive) == 0)
            return &entry;
    }
    return nullptr;
}

// Parse a ".map". Structural problems (a line that isn't a section header,
// an assignment, or blank) are file-level errors — vendor files are
// machine-generated, so a malformed one is a wrong or corrupt file, not
// something to import half of. Unknown functions are named skips.
QVector<MidiBinding> parseSmartSdrMap(const QByteArray& bytes,
                                      const std::function<bool(const QString&)>& paramValidator,
                                      MidiImportResult& result)
{
    QVector<MidiBinding> bindings;
    QSet<QString> importedParams;
    QSet<quint32> importedKeys;
    bool sawAssignment = false;

    enum class Section { None, Controls, Buttons, Other };
    Section section = Section::None;

    const QStringList lines = QString::fromUtf8(bytes).split(QLatin1Char('\n'));
    for (int i = 0; i < lines.size(); ++i) {
        const QString line = lines.at(i).trimmed();
        if (line.isEmpty())
            continue;

        if (line.startsWith(QLatin1Char('#'))) {
            const QString name = line.mid(1).trimmed().toLower();
            if (name == QLatin1String("controls"))
                section = Section::Controls;
            else if (name == QLatin1String("buttons"))
                section = Section::Buttons;
            else
                section = Section::Other;
            continue;
        }

        const int eq = line.indexOf(QLatin1Char('='));
        if (eq <= 0) {
            result.errors << QStringLiteral("Line %1: not a section header or assignment: \"%2\"")
                                 .arg(i + 1).arg(line);
            continue;
        }

        const QString key = line.left(eq).trimmed();
        // Everything after the first ';' is a flag ("active" in vendor
        // files); flags decorate rows we otherwise fully understand, so
        // they are ignored.
        const QString function = line.mid(eq + 1).section(QLatin1Char(';'), 0, 0).trimmed();
        if (function.isEmpty())
            continue; // unassigned slot ("C0="): no content to import or report

        sawAssignment = true;

        // Section first: rows in a section we never bind from are not ours to
        // key-validate. Vendors key their feedback sections in their own
        // dialect ("L1=", "LED1="), and judging those by the control/button
        // key format would fail the whole file instead of naming the skip.
        if (section == Section::Other) {
            // e.g. the LEDs section: device feedback, not a binding.  Named
            // once, not once per row — a real device has one row per LED.
            const QString note = function + QStringLiteral(" (not a control/button row)");
            if (!result.skippedUnknownParam.contains(note))
                result.skippedUnknownParam << note;
            continue;
        }

        const QChar kind = key.isEmpty() ? QChar() : key.at(0).toUpper();
        bool numberOk = false;
        const int number = key.mid(1).toInt(&numberOk);
        if ((kind != QLatin1Char('C') && kind != QLatin1Char('B'))
            || !numberOk || number < 0 || number > 127) {
            result.errors << QStringLiteral("Line %1: bad assignment key \"%2\"")
                                 .arg(i + 1).arg(key);
            continue;
        }

        const MapFunctionEntry* entry = findMapFunction(function);
        if (!entry) {
            if (!result.skippedUnknownParam.contains(function))
                result.skippedUnknownParam << function;
            continue;
        }

        const QString paramId = QLatin1String(entry->paramId);
        if (paramValidator && !paramValidator(paramId)) {
            const QString note =
                function + QStringLiteral(" (param %1 not registered)").arg(paramId);
            if (!result.skippedUnknownParam.contains(note))
                result.skippedUnknownParam << note;
            continue;
        }
        if (importedParams.contains(paramId)) {
            result.duplicates << function;
            continue;
        }

        MidiBinding b;
        b.channel = -1; // the .map carries no channel — accept any
        b.msgType = (kind == QLatin1Char('C')) ? MidiBinding::CC : MidiBinding::NoteOn;
        b.number = number;
        b.paramId = paramId;
        b.inverted = false;
        b.relative = (b.msgType == MidiBinding::CC) && entry->relativeCc;
        // Two rows on one MIDI source is the other way a binding disappears:
        // MidiControlManager indexes by MidiBinding::key(), one entry per
        // key, so the later row would make the earlier one unreachable after
        // Load while both still show in the table. Name it instead.
        if (importedKeys.contains(b.key())) {
            result.duplicates
                << function + QStringLiteral(" (%1 already bound)").arg(key);
            continue;
        }
        bindings.append(b);
        importedParams.insert(paramId);
        importedKeys.insert(b.key());
    }

    if (!sawAssignment)
        result.errors << QStringLiteral("File contains no assignments.");
    if (!result.errors.isEmpty())
        return {};
    return bindings;
}

// Parse native <MidiProfile> XML with the validation the store's own
// parser never needed: reader errors are surfaced, the root element is
// required (so importing midi.settings itself is an error, not a
// surprise), and per-row problems become named skips.
QVector<MidiBinding> parseProfileXml(const QByteArray& bytes,
                                     const std::function<bool(const QString&)>& paramValidator,
                                     MidiImportResult& result)
{
    QVector<MidiBinding> bindings;
    QSet<QString> importedParams;
    QSet<quint32> importedKeys;
    bool sawRoot = false;
    bool sawBindingElement = false;

    QXmlStreamReader xml(bytes);
    while (!xml.atEnd()) {
        xml.readNext();
        if (!xml.isStartElement())
            continue;

        if (!sawRoot) {
            sawRoot = true;
            if (xml.name() != u"MidiProfile") {
                result.errors << QStringLiteral("Root element is <%1>; expected <MidiProfile>.")
                                     .arg(xml.name().toString());
                return {};
            }
            continue;
        }
        if (xml.name() != u"Binding")
            continue;

        sawBindingElement = true;
        const auto attrs = xml.attributes();
        const QString param = normalizedMidiParamId(attrs.value("param").toString());
        if (param.isEmpty()) {
            result.skippedUnknownParam << QStringLiteral("(empty param)");
            continue;
        }

        bool typeOk = false;
        const int type = attrs.value("type").toInt(&typeOk);
        if (!typeOk || type < MidiBinding::CC || type > MidiBinding::PitchBend) {
            result.skippedBadType
                << param + QStringLiteral(" (type \"%1\")").arg(attrs.value("type").toString());
            continue;
        }
        // Absent attributes keep their defaults (our writer always emits both,
        // hand-written files may not); a present-but-non-numeric value is a
        // named skip, never a silent 0 — channel 0 is MIDI channel 1, not the
        // any-channel wildcard, so accepting it would bind the row wrongly.
        const auto channelAttr = attrs.value("channel");
        bool channelOk = true;
        const int channel = channelAttr.isNull() ? 0 : channelAttr.toInt(&channelOk);
        if (!channelOk || channel < -1 || channel > 15) {
            result.skippedBadType
                << param + QStringLiteral(" (channel \"%1\")").arg(channelAttr.toString());
            continue;
        }
        const auto numberAttr = attrs.value("number");
        bool numberOk = true;
        const int number = numberAttr.isNull() ? 0 : numberAttr.toInt(&numberOk);
        if (!numberOk || (type != MidiBinding::PitchBend && (number < 0 || number > 127))) {
            result.skippedBadType
                << param + QStringLiteral(" (number \"%1\")").arg(numberAttr.toString());
            continue;
        }

        if (paramValidator && !paramValidator(param)) {
            result.skippedUnknownParam << param;
            continue;
        }
        if (importedParams.contains(param)) {
            result.duplicates << param;
            continue;
        }

        MidiBinding b;
        b.paramId = param;
        b.channel = channel;
        b.msgType = static_cast<MidiBinding::MsgType>(type);
        b.number = number;
        b.inverted = (attrs.value("inverted") == u"True");
        b.relative = (attrs.value("relative") == u"True");
        // Same MIDI-source collision as the .map path, and likelier here:
        // addBinding() dedups by param only, so our own export can carry two
        // bindings on one source. The later one would win the index after
        // Load and the earlier would go quiet with nothing reporting it.
        if (importedKeys.contains(b.key())) {
            result.duplicates
                << param + QStringLiteral(" (%1 %2 on channel %3 already bound)")
                               .arg(midiMsgTypeName(b.msgType))
                               .arg(number)
                               .arg(channel < 0 ? QStringLiteral("any")
                                                : QString::number(channel + 1));
            continue;
        }
        bindings.append(b);
        importedParams.insert(param);
        importedKeys.insert(b.key());
    }

    if (xml.hasError()) {
        result.errors << QStringLiteral("XML error at line %1: %2")
                             .arg(xml.lineNumber()).arg(xml.errorString());
        return {};
    }
    if (!sawBindingElement) {
        result.errors << QStringLiteral("File contains no <Binding> elements.");
        return {};
    }
    return bindings;
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
           + "/AetherSDR/midi.settings";
}

QString MidiSettings::profileDir() const
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
           + "/AetherSDR/midi";
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
    writeProfileDocument(xml, bindings);
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

// ── Import ──────────────────────────────────────────────────────────────────

MidiImportResult MidiSettings::importProfile(
    const QString& filePath,
    const std::function<bool(const QString&)>& paramValidator)
{
    MidiImportResult result;

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        result.errors << QStringLiteral("Couldn't open %1 for reading (%2).")
                             .arg(QDir::toNativeSeparators(filePath),
                                  file.errorString());
        return result;
    }
    const QByteArray bytes = file.readAll();
    file.close();

    // Sniff the dialect: native XML leads with '<'; a SmartSDR ".map" leads
    // with a "#"-headed section. Anything else is not a profile.
    QVector<MidiBinding> bindings;
    // A leading UTF-8 BOM (any file round-tripped through a Windows editor)
    // never reaches this comparison: QString::fromUtf8() consumes it. The
    // import test pins that, so a toolchain that ever stops doing it fails
    // loudly here rather than reporting a valid profile as unrecognized.
    const QString head = QString::fromUtf8(bytes.left(256)).trimmed();
    if (head.startsWith(QLatin1Char('<')))
        bindings = parseProfileXml(bytes, paramValidator, result);
    else if (head.startsWith(QLatin1Char('#')))
        bindings = parseSmartSdrMap(bytes, paramValidator, result);
    else {
        result.errors << QStringLiteral(
            "Unrecognized file: expected <MidiProfile> XML or a SmartSDR \"# Controls\" map.");
        return result;
    }

    if (!result.ok())
        return result;

    result.importedCount = bindings.size();
    if (bindings.isEmpty())
        return result; // parsed, but every row was a named skip — nothing to store

    // Store name = file base name; never overwrite an existing profile. The
    // prompt-vs-suffix collision policy is an open maintainer call, and a
    // suffix is the reversible default. Dots are replaced because the store
    // round-trips names through QFileInfo::baseName(), which cuts at the
    // first dot — a dotted name would list, load, and collide wrongly.
    QString name = QFileInfo(filePath).completeBaseName().trimmed();
    name.replace(QLatin1Char('.'), QLatin1Char('_'));
    if (name.isEmpty())
        name = QStringLiteral("Imported profile");
    const QStringList existing = availableProfiles();
    const auto taken = [&existing](const QString& candidate) {
        for (const auto& p : existing)
            if (p.compare(candidate, Qt::CaseInsensitive) == 0)
                return true;
        return false;
    };
    QString unique = name;
    for (int n = 2; taken(unique); ++n)
        unique = name + QStringLiteral(" (%1)").arg(n);

    saveProfile(unique, bindings);

    // The write path returns void, so prove the store took it before
    // reporting success.
    if (loadProfile(unique).size() != bindings.size()) {
        result.errors << QStringLiteral("Couldn't write the profile into %1.")
                             .arg(QDir::toNativeSeparators(profileDir()));
        result.importedCount = 0;
        return result;
    }
    result.profileName = unique;
    return result;
}

MidiExportResult MidiSettings::exportProfile(const QString& filePath,
                                             const QVector<MidiBinding>& bindings) const
{
    MidiExportResult result;
    if (filePath.trimmed().isEmpty()) {
        result.error = QStringLiteral("No export path was provided.");
        return result;
    }

    QByteArray xmlBytes;
    {
        QXmlStreamWriter xml(&xmlBytes);
        writeProfileDocument(xml, bindings);
    }

    QSaveFile file(filePath);
    // Some network mounts (SMB, WSL DrvFs) can't create the sidecar temp
    // file QSaveFile normally uses — fall back to a direct write so export
    // succeeds where a plain write would (matches ShortcutManager and
    // KiwiSdrManager).
    file.setDirectWriteFallback(true);
    if (!file.open(QIODevice::WriteOnly)) {
        result.error = QStringLiteral("Couldn't open %1 for writing (%2).")
                           .arg(QDir::toNativeSeparators(filePath), file.errorString());
        return result;
    }
    if (file.write(xmlBytes) != xmlBytes.size()) {
        const QString reason = file.errorString();
        file.cancelWriting();
        result.error = QStringLiteral("Couldn't write the profile to %1 (%2).")
                           .arg(QDir::toNativeSeparators(filePath), reason);
        return result;
    }
    if (!file.commit()) {
        result.error = QStringLiteral("Couldn't save %1 (%2).")
                           .arg(QDir::toNativeSeparators(filePath), file.errorString());
        return result;
    }
    result.exportedCount = bindings.size();
    return result;
}

} // namespace AetherSDR

#endif // HAVE_MIDI
