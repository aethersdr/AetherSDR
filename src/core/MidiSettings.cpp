#ifdef HAVE_MIDI

#include "MidiSettings.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
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

// One writer for a <Binding> element, shared by all three documents that
// contain one: the profile store, the user-facing export, and midi.settings.
// A new attribute added here reaches every file that should carry it, which
// is the whole point — three hand-kept copies is how a field ends up in the
// profile a user exports but not in the settings the app reloads.
void writeBindingElement(QXmlStreamWriter& xml, const MidiBinding& b)
{
    xml.writeStartElement("Binding");
    xml.writeAttribute("param", normalizedMidiParamId(b.paramId));
    xml.writeAttribute("channel", QString::number(b.channel));
    xml.writeAttribute("type", QString::number(static_cast<int>(b.msgType)));
    xml.writeAttribute("number", QString::number(b.number));
    xml.writeAttribute("inverted", b.inverted ? "True" : "False");
    if (b.relative) xml.writeAttribute("relative", "True");
    xml.writeEndElement();
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
    for (const auto& b : bindings)
        writeBindingElement(xml, b);
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

    // NonBinding (a section we know carries device feedback) is kept distinct
    // from Unrecognized (a header we couldn't read, which may well be a
    // comment) because the two justify different handling: rows under a known
    // feedback header are never bindings, while rows under an unreadable one
    // still deserve a try. Reporting one as the other sends the operator
    // looking at the wrong line.
    enum class Section { None, Controls, Buttons, NonBinding, Unrecognized };
    Section section = Section::None;
    QString sectionLabel;
    // section label -> rows skipped under it, summarized once after the loop.
    QMap<QString, int> nonBindingRows;

    const QStringList lines = QString::fromUtf8(bytes).split(QLatin1Char('\n'));
    for (int i = 0; i < lines.size(); ++i) {
        const QString line = lines.at(i).trimmed();
        if (line.isEmpty())
            continue;

        if (line.startsWith(QLatin1Char('#'))) {
            // Match the header's FIRST WORD, not the whole line: vendors
            // decorate headers ("# Controls (16 knobs)"), and matching the
            // whole line demotes a decorated header to "not a binding
            // section", losing every row under it.
            sectionLabel = line.mid(1).trimmed();
            QString first;
            for (const QChar& c : sectionLabel) {
                if (!c.isLetter()) break;
                first.append(c.toLower());
            }
            if (first == QLatin1String("controls")) {
                section = Section::Controls;
            } else if (first == QLatin1String("buttons")) {
                section = Section::Buttons;
            } else if (first == QLatin1String("leds")) {
                section = Section::NonBinding;
            } else {
                section = Section::Unrecognized;
            }
            continue;
        }

        const int eq = line.indexOf(QLatin1Char('='));
        if (eq <= 0) {
            result.errors << QStringLiteral("Line %1: not a section header or assignment: \"%2\"")
                                 .arg(i + 1).arg(line);
            continue;
        }

        const QString key = line.left(eq).trimmed();
        // Everything after the first ';' is a flag; flags decorate rows we
        // otherwise fully understand, so they are ignored. Observed in
        // published vendor files: "active" (most rows) and "f"
        // (CTR2-Max B12). No flag has been shown to change what a row binds
        // to, and dropping them round-trips every vendor row correctly — but
        // this is an observation, not a documented guarantee. If a flag ever
        // turns out to mean "assignment disabled", this is the line to fix.
        const QString function = line.mid(eq + 1).section(QLatin1Char(';'), 0, 0).trimmed();
        if (function.isEmpty())
            continue; // unassigned slot ("C0="): no content to import or report

        sawAssignment = true;

        // Named skips carry the whole row, not just its value. A metadata row
        // reads as "1" on its own; "FORMAT_VERSION=1" tells the operator what
        // was skipped, and keeps two different rows that happen to share a
        // value from collapsing into one entry.
        const auto nameSkip = [&](const QString& suffix) {
            const QString note = key + QLatin1Char('=') + function + suffix;
            if (!result.skippedUnknownParam.contains(note))
                result.skippedUnknownParam << note;
        };

        // A section we know carries device feedback is never a binding, and
        // vendors key those in their own dialect ("L1=", "LED1="), so they are
        // not ours to key-validate — reporting them beats failing the file.
        //
        // Counted per section rather than named per row: every row under a
        // known feedback header is the same kind of thing and none of them can
        // ever bind, so a device with sixty LEDs would otherwise put sixty
        // lines in the details list saying nothing the first one didn't. Rows
        // under a header we *couldn't read* get named individually below,
        // because there the operator does need to see which row it was.
        if (section == Section::NonBinding) {
            ++nonBindingRows[sectionLabel];
            continue;
        }

        const QChar kind = key.isEmpty() ? QChar() : key.at(0).toUpper();
        bool numberOk = false;
        const int number = key.mid(1).toInt(&numberOk);
        const bool keyIsBindable = (kind == QLatin1Char('C') || kind == QLatin1Char('B'))
                                   && numberOk && number >= 0 && number <= 127;
        if (!keyIsBindable) {
            // Under a header we couldn't read, a key we can't use is a named
            // skip rather than a file-level error: the header may be a comment
            // or a section this build doesn't know, and neither is a corrupt
            // file. Under Controls/Buttons the file really is malformed.
            if (section == Section::Controls || section == Section::Buttons) {
                result.errors << QStringLiteral("Line %1: bad assignment key \"%2\"")
                                     .arg(i + 1).arg(key);
            } else {
                nameSkip(QStringLiteral(" (not a control or button row)"));
            }
            continue;
        }
        // A bindable row still binds under an unreadable header — that is what
        // keeps a stray comment line mid-file from swallowing the rows after
        // it, which is how a decorated header or a vendor note used to cost
        // real bindings.

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

    for (auto it = nonBindingRows.cbegin(); it != nonBindingRows.cend(); ++it) {
        result.skippedUnknownParam
            << QStringLiteral("%1 section (%2 row(s)) — device feedback, not bindings")
                   .arg(it.key())
                   .arg(it.value());
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

        // `type` follows the same absent-vs-non-numeric rule as its
        // neighbours below: absent keeps MidiBinding's own default (CC),
        // present-but-unreadable is a named skip. Treating absent as a bad
        // value made it the one attribute a hand-written row could not omit,
        // while omitting channel and number was fine — an inconsistency with
        // nothing behind it.
        const auto typeAttr = attrs.value("type");
        bool typeOk = true;
        const int type = typeAttr.isNull() ? int(MidiBinding::CC) : typeAttr.toInt(&typeOk);
        if (!typeOk || type < MidiBinding::CC || type > MidiBinding::PitchBend) {
            result.skippedBadType
                << param + QStringLiteral(" (type \"%1\")").arg(typeAttr.toString());
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
        // Range-checked for every type, Pitch Bend included. Pitch Bend
        // ignores `number` at dispatch (key() substitutes 0xFF and
        // sourceDisplayName() omits it), so an out-of-range value there is
        // inert rather than dangerous — but storing and re-exporting a number
        // no MIDI message can carry leaves a file that says something untrue,
        // and Principle VII asks the boundary to validate ranges, not just
        // the ranges that currently matter.
        const auto numberAttr = attrs.value("number");
        bool numberOk = true;
        const int number = numberAttr.isNull() ? 0 : numberAttr.toInt(&numberOk);
        if (!numberOk || number < 0 || number > 127) {
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
    for (const auto& b : bindings)
        writeBindingElement(xml, b);
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
    for (const auto& b : bindings)
        writeBindingElement(xml, b);
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

    // A profile is kilobytes — the vendor CTR2-Quad map is a few. Two guards,
    // because they cover different failures and neither covers the other:
    //
    //  1. Regular files only. QFile::size() reports 0 for character devices,
    //     FIFOs and most /proc entries, so a size check alone lets exactly the
    //     files with no end through. Pointing the picker at /dev/zero used to
    //     read until the kernel OOM-killed the app (74 GB VM, no dialog, no log
    //     line); a FIFO hung in the read forever. Principle VII: a parser must
    //     not crash, hang, or over-allocate on the input it is handed.
    //  2. A bounded read of one byte past the cap. This is what enforces the
    //     size limit — checking size() first would re-trust the same number
    //     guard 1 exists because we cannot trust.
    const QFileInfo info(filePath);
    if (!info.isFile()) {
        result.errors << QStringLiteral("%1 is not a regular file.")
                             .arg(QDir::toNativeSeparators(filePath));
        return result;
    }

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        result.errors << QStringLiteral("Couldn't open %1 for reading (%2).")
                             .arg(QDir::toNativeSeparators(filePath),
                                  file.errorString());
        return result;
    }
    constexpr qint64 kMaxProfileBytes = 1 << 20;
    const QByteArray bytes = file.read(kMaxProfileBytes + 1);
    file.close();
    if (bytes.size() > kMaxProfileBytes) {
        result.errors << QStringLiteral("%1 is too large to be a MIDI profile (over %2 bytes).")
                             .arg(QDir::toNativeSeparators(filePath))
                             .arg(kMaxProfileBytes);
        return result;
    }

    // Sniff the dialect: native XML leads with '<'; a SmartSDR ".map" is
    // recognized by a "#"-headed section appearing in its first few lines,
    // not merely by its first character — a file that opens with a
    // "FORMAT_VERSION=1"-style preamble is still a map, and rejecting it
    // wholesale sends the operator an error naming a header the file
    // contains three lines further down. Anything else is not a profile.
    QVector<MidiBinding> bindings;
    // A leading UTF-8 BOM (any file round-tripped through a Windows editor)
    // never reaches this comparison: QString::fromUtf8() consumes it. The
    // import test pins that, so a toolchain that ever stops doing it fails
    // loudly here rather than reporting a valid profile as unrecognized.
    const QString head = QString::fromUtf8(bytes.left(1024)).trimmed();
    constexpr int kSniffLines = 8;
    bool looksLikeMap = false;
    const QStringList headLines = head.split(QLatin1Char('\n'));
    for (int i = 0; i < headLines.size() && i < kSniffLines; ++i) {
        if (headLines.at(i).trimmed().startsWith(QLatin1Char('#'))) {
            looksLikeMap = true;
            break;
        }
    }
    if (head.startsWith(QLatin1Char('<'))) {
        bindings = parseProfileXml(bytes, paramValidator, result);
    } else if (looksLikeMap) {
        bindings = parseSmartSdrMap(bytes, paramValidator, result);
    } else {
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
    //
    // completeBaseName() operates on fileName(), so any directory component of
    // the chosen path is already stripped: "../../evil.map" yields "evil". That
    // is what keeps this call site safe, because saveProfile() — and its load
    // and delete siblings — concatenate the name straight into profileDir()
    // with no sanitizing. Do not swap this for a name taken from the document
    // body or from filePath without stripping separators first.
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
    // Refused here, not only in the dialog. An empty set serializes to a
    // childless <MidiProfile/>, which importProfile() then rejects with
    // "File contains no <Binding> elements" — so reporting success would
    // hand the caller a file that cannot come back, and the round-trip
    // contract this API documents lives here, not in the GUI that happens
    // to guard it today.
    if (bindings.isEmpty()) {
        result.error = QStringLiteral("There are no bindings to export.");
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
