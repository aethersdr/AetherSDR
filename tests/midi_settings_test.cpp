#include "core/MidiSettings.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSet>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <iostream>

#ifndef Q_OS_WIN
#include <unistd.h>
#endif

using namespace AetherSDR;

namespace {

bool expect(bool condition, const char* label)
{
    std::cout << (condition ? "[ OK ] " : "[FAIL] ") << label << '\n';
    return condition;
}

bool sameBinding(const MidiBinding& a, const MidiBinding& b)
{
    return a.channel == b.channel
        && a.msgType == b.msgType
        && a.number == b.number
        && a.paramId == b.paramId
        && a.inverted == b.inverted
        && a.relative == b.relative;
}

} // namespace

int main(int argc, char** argv)
{
    QTemporaryDir fakeHome(QDir::tempPath() + "/aether-midi-settings-test-XXXXXX");
    if (!fakeHome.isValid()) {
        std::cerr << "[FAIL] create temporary home\n";
        return 1;
    }
    qputenv("HOME", fakeHome.path().toUtf8());
    qputenv("CFFIXED_USER_HOME", fakeHome.path().toUtf8());
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication app(argc, argv);

    const QString configRoot =
        QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    QDir(configRoot + "/AetherSDR").removeRecursively();

    MidiBinding afGain;
    afGain.channel = 2;
    afGain.msgType = MidiBinding::CC;
    afGain.number = 74;
    afGain.paramId = "rx.afGain";
    afGain.inverted = true;
    afGain.relative = true;

    MidiBinding tuneKnob;
    tuneKnob.channel = -1;
    tuneKnob.msgType = MidiBinding::PitchBend;
    tuneKnob.number = -1;
    tuneKnob.paramId = "rx.tuneKnob";

    MidiBinding cwDitLegacy;
    cwDitLegacy.channel = 0;
    cwDitLegacy.msgType = MidiBinding::NoteOn;
    cwDitLegacy.number = 60;
    cwDitLegacy.paramId = "cw.dit";

    MidiBinding cwDah;
    cwDah.channel = 0;
    cwDah.msgType = MidiBinding::NoteOn;
    cwDah.number = 61;
    cwDah.paramId = "cwdah";

    QVector<MidiBinding> saved{afGain, tuneKnob, cwDitLegacy, cwDah};
    QVector<MidiBinding> expected = saved;
    expected[2].paramId = "cwdit";

    auto& settings = MidiSettings::instance();
    settings.setLastDevice("Initial Controller");
    settings.setAutoConnect(true);
    settings.saveBindings(saved);

    settings.setLastDevice("Renamed Controller");
    settings.setAutoConnect(false);
    settings.save();

    const auto loaded = settings.loadBindings();
    bool ok = true;
    ok &= expect(loaded.size() == saved.size(),
                 "preference-only save preserves binding count");
    if (loaded.size() == expected.size()) {
        for (int i = 0; i < expected.size(); ++i) {
            ok &= expect(sameBinding(loaded[i], expected[i]),
                         "preference-only save preserves normalized binding");
        }
    }

    QFile midiFile(configRoot + "/AetherSDR/midi.settings");
    ok &= expect(midiFile.open(QIODevice::ReadOnly | QIODevice::Text),
                 "MIDI settings file is written");
    const QString xml = midiFile.isOpen() ? QString::fromUtf8(midiFile.readAll()) : QString();
    midiFile.close();
    ok &= expect(xml.contains(QStringLiteral("param=\"cwdit\"")),
                 "legacy CW dit MIDI binding saves as cwdit");
    ok &= expect(xml.contains(QStringLiteral("param=\"cwdah\"")),
                 "CW dah MIDI binding saves as cwdah");
    ok &= expect(!xml.contains(QStringLiteral("param=\"cw.dit\"")),
                 "MIDI settings do not save dotted CW dit ID");

    settings.load();
    ok &= expect(settings.lastDevice() == "Renamed Controller",
                 "preference-only save updates last device");
    ok &= expect(!settings.autoConnect(),
                 "preference-only save updates auto-connect");

    // ── importProfile: native XML + SmartSDR ".map", auto-detected ──────────

    const QSet<QString> registry = {
        "rx.tuneKnob", "rx.afGain", "rx.agcThreshold", "tx.rfPower",
        "rx.nrEnable", "global.bandUp", "global.bandDown", "global.masterMute",
        "global.modeUp", "global.panZoomIn", "global.panZoomOut", "rx.stepUp",
        "tx.atuStart", "cwdit", "cwdah", "cwkey", "cw.ptt", "cw.speed",
    };
    const auto validator = [&registry](const QString& id) { return registry.contains(id); };

    const auto writeImportFile = [&fakeHome](const QString& name, const QByteArray& content) {
        QFile f(fakeHome.path() + "/" + name);
        if (!f.open(QIODevice::WriteOnly))
            return QString();
        f.write(content);
        f.close();
        return f.fileName();
    };

    // Shaped like the vendor CTR2-MIDI file: known + unknown functions, an
    // unassigned slot, flags, a duplicate function, and an LEDs section.
    const QByteArray mapContent =
        "# Controls\n"
        "C100=freq\n"
        "C103=bwset;active\n"
        "C110=cwspeed;active\n"
        "C0=\n"
        "# Buttons\n"
        "B20=leftpaddle\n"
        "B21=rightpaddle;active\n"
        "B13=nr\n"
        "B4=nr\n"
        "B53=openft8\n"
        "B31=ptt\n"
        "# LEDs\n";
    const QString mapPath = writeImportFile("CTR2-Test_v1.0.map", mapContent);

    const auto r1 = settings.importProfile(mapPath, validator);
    ok &= expect(r1.ok(), "map import succeeds");
    ok &= expect(r1.importedCount == 6, "map import count (freq, cwspeed, paddles, nr, ptt)");
    ok &= expect(r1.profileName == "CTR2-Test_v1.0", "map import names profile from file name");
    ok &= expect(r1.skippedUnknownParam.contains("bwset")
                     && r1.skippedUnknownParam.contains("openft8"),
                 "unknown map functions are skipped by name");
    ok &= expect(r1.duplicates == QStringList{"nr"}, "duplicate map function reported by name");
    {
        const auto imported = settings.loadProfile(r1.profileName);
        ok &= expect(imported.size() == 6, "imported map profile loads from the store");
        bool foundFreq = false;
        bool foundDit = false;
        for (const auto& b : imported) {
            if (b.paramId == "rx.tuneKnob")
                foundFreq = b.msgType == MidiBinding::CC && b.number == 100
                            && b.relative && b.channel == -1 && !b.inverted;
            if (b.paramId == "cwdit")
                foundDit = b.msgType == MidiBinding::NoteOn && b.number == 20 && !b.relative;
        }
        ok &= expect(foundFreq, "freq row becomes a relative CC 100 VFO binding");
        ok &= expect(foundDit, "leftpaddle row becomes a NoteOn 20 cwdit binding");
    }

    const auto r2 = settings.importProfile(mapPath, validator);
    ok &= expect(r2.ok() && r2.profileName == "CTR2-Test_v1.0 (2)",
                 "a name collision gets a suffix, never an overwrite");

    // Sections we never bind from carry the vendor's own key dialect. Judging
    // those rows by the control/button key format would fail the whole file;
    // they are named skips, and the bindings around them still import.
    const auto rLeds = settings.importProfile(
        writeImportFile("with-leds.map",
                        "# Controls\n"
                        "C100=freq\n"
                        "# LEDs\n"
                        "L1=txled\n"
                        "LED2=rxled;active\n"),
        validator);
    ok &= expect(rLeds.ok() && rLeds.importedCount == 1,
                 "a vendor-dialect LEDs section doesn't fail the import");
    // Reported as one summary per feedback section rather than one line per
    // row: every row under a known non-binding header is the same kind of
    // thing, so naming each would flood the details list on a real device
    // without telling the operator anything the first line didn't.
    ok &= expect(rLeds.skippedUnknownParam.size() == 1
                     && rLeds.skippedUnknownParam.first().contains("LEDs section")
                     && rLeds.skippedUnknownParam.first().contains("2 row"),
                 "a LEDs section is summarized once, with its row count");

    // A real device has one row per LED; the details list should name the
    // target once rather than once per row.
    const auto rLedsMany = settings.importProfile(
        writeImportFile("many-leds.map",
                        "# Controls\n"
                        "C100=freq\n"
                        "# LEDs\n"
                        "L1=txled\n"
                        "L2=txled\n"
                        "L3=txled\n"
                        "L4=txled\n"),
        validator);
    ok &= expect(rLedsMany.ok() && rLedsMany.skippedUnknownParam.size() == 1
                     && rLedsMany.skippedUnknownParam.first().contains("4 row"),
                 "repeated LEDs rows collapse to one skip that counts them");

    // Two functions on ONE MIDI source: MidiControlManager indexes by
    // MidiBinding::key(), so importing both would leave the earlier one
    // unreachable after Load with nothing reporting it.
    const auto rKeyClash = settings.importProfile(
        writeImportFile("keyclash.map",
                        "# Buttons\n"
                        "B13=nr\n"
                        "B13=banddown\n"),
        validator);
    ok &= expect(rKeyClash.ok() && rKeyClash.importedCount == 1,
                 "two functions on one key import as one binding");
    ok &= expect(rKeyClash.duplicates.size() == 1
                     && rKeyClash.duplicates.first().contains("B13"),
                 "the losing row is named as a duplicate, not dropped silently");
    {
        const auto stored = settings.loadProfile(rKeyClash.profileName);
        QSet<quint32> keys;
        for (const auto& b : stored) keys.insert(b.key());
        ok &= expect(stored.size() == static_cast<int>(keys.size()),
                     "no stored profile binds one MIDI source twice");
    }

    // Same collision in the native XML — likelier there, since addBinding()
    // dedups by param only and our own export can carry it.
    const auto rXmlKeyClash = settings.importProfile(
        writeImportFile("keyclash.xml",
                        "<MidiProfile>"
                        "<Binding param=\"rx.afGain\" channel=\"0\" type=\"0\" number=\"7\"/>"
                        "<Binding param=\"tx.rfPower\" channel=\"0\" type=\"0\" number=\"7\"/>"
                        "</MidiProfile>\n"),
        validator);
    ok &= expect(rXmlKeyClash.ok() && rXmlKeyClash.importedCount == 1,
                 "XML: two bindings on one source import as one");
    ok &= expect(rXmlKeyClash.duplicates.size() == 1
                     && rXmlKeyClash.duplicates.first().contains("CC"),
                 "XML: the losing binding is named with its source");

    const auto r3 = settings.importProfile(writeImportFile("garbage.map", "hello world\n"),
                                           validator);
    ok &= expect(!r3.ok() && r3.importedCount == 0, "unrecognized content fails loudly");

    {
        // Native XML round trip: the profile the map import just stored.
        const auto r4 = settings.importProfile(
            configRoot + "/AetherSDR/midi/CTR2-Test_v1.0.xml", validator);
        ok &= expect(r4.ok() && r4.importedCount == 6, "native XML profile re-imports");
        ok &= expect(r4.profileName == "CTR2-Test_v1.0 (3)", "XML re-import takes the next suffix");
        const auto again = settings.loadProfile(r4.profileName);
        const auto first = settings.loadProfile("CTR2-Test_v1.0");
        bool same = again.size() == first.size();
        for (int i = 0; same && i < again.size(); ++i)
            same = sameBinding(again[i], first[i]);
        ok &= expect(same, "XML round trip preserves every binding field");
    }

    // ── Direct band/mode select vocabulary (#5027) ──────────────────────────
    // The CTR2-Quad and CTR2-Max maps carry one row per band and per mode —
    // the full set measured from the vendor's published packages. Every
    // target param is registered by MainWindow's controller table, so these
    // rows are vocabulary only.
    {
        QSet<QString> bandModeRegistry = {
            "global.band160m", "global.band80m", "global.band60m",
            "global.band40m",  "global.band30m", "global.band20m",
            "global.band17m",  "global.band15m", "global.band12m",
            "global.band10m",  "global.band6m",
            "global.modeCW",   "global.modeLSB", "global.modeUSB",
            "global.modeAM",   "global.modeFM",  "global.modeRTTY",
            "global.modeDIGL", "global.modeDIGU", "global.modeSAM",
        };
        const auto bandModeValidator = [&bandModeRegistry](const QString& id) {
            return bandModeRegistry.contains(id);
        };
        const auto rBandMode = settings.importProfile(
            writeImportFile("bandmode.map",
                            "# Buttons\n"
                            "B37=band160\n"
                            "B38=band80\n"
                            "B39=band60\n"
                            "B40=band40\n"
                            "B41=band30\n"
                            "B42=band20\n"
                            "B43=band17\n"
                            "B44=band15\n"
                            "B45=band12\n"
                            "B46=band10\n"
                            "B47=band6\n"
                            "B25=modecw\n"
                            "B26=modelsb\n"
                            "B27=modeusb\n"
                            "B28=modeam\n"
                            "B29=modefm\n"
                            "B30=modertty\n"
                            "B31=modedigl\n"
                            "B32=modedigu\n"
                            "B33=modesam\n"),
            bandModeValidator);
        ok &= expect(rBandMode.ok() && rBandMode.importedCount == 20,
                     "all 11 band + 9 mode select rows import");
        ok &= expect(rBandMode.skippedUnknownParam.isEmpty()
                         && rBandMode.duplicates.isEmpty(),
                     "band/mode vocabulary leaves no named skips");
        const auto stored = settings.loadProfile(rBandMode.profileName);
        bool found160 = false;
        bool foundCw = false;
        for (const auto& b : stored) {
            if (b.paramId == "global.band160m")
                found160 = b.msgType == MidiBinding::NoteOn && b.number == 37
                           && !b.relative && b.channel == -1;
            if (b.paramId == "global.modeCW")
                foundCw = b.msgType == MidiBinding::NoteOn && b.number == 25;
        }
        ok &= expect(found160, "band160 row becomes a NoteOn 37 global.band160m binding");
        ok &= expect(foundCw, "modecw row becomes a NoteOn 25 global.modeCW binding");
    }

    const auto r5 = settings.importProfile(configRoot + "/AetherSDR/midi.settings", validator);
    ok &= expect(!r5.ok(), "midi.settings itself is rejected (wrong root element)");

    const auto r6 = settings.importProfile(
        writeImportFile("mixed.xml",
                        "<MidiProfile>"
                        "<Binding param=\"rx.afGain\" channel=\"0\" type=\"9\" number=\"10\"/>"
                        "<Binding param=\"no.such\" channel=\"0\" type=\"0\" number=\"11\"/>"
                        "<Binding param=\"rx.afGain\" channel=\"0\" type=\"0\" number=\"12\"/>"
                        "</MidiProfile>\n"),
        validator);
    ok &= expect(r6.ok() && r6.importedCount == 1, "XML: the one good row imports");
    ok &= expect(r6.skippedBadType.size() == 1 && r6.skippedBadType.first().contains("type"),
                 "XML: out-of-range type is skipped by name");
    ok &= expect(r6.skippedUnknownParam == QStringList{"no.such"},
                 "XML: unregistered param is skipped by name");

    const auto r7 = settings.importProfile(
        writeImportFile("trunc.xml", "<MidiProfile><Binding param=\"x\""), validator);
    ok &= expect(!r7.ok(), "truncated XML fails loudly");

    // Oversized files are refused before the read allocates them. The content
    // is valid profile XML followed by padding, so the only thing that can
    // reject it is the size cap — not the dialect sniff or the parser.
    {
        QByteArray huge = "<MidiProfile>"
                          "<Binding param=\"rx.afGain\" channel=\"0\" type=\"0\" number=\"7\"/>"
                          "</MidiProfile>\n";
        huge.append(QByteArray((1 << 20) + 1 - huge.size(), ' '));
        const auto rBig = settings.importProfile(writeImportFile("huge.xml", huge), validator);
        ok &= expect(!rBig.ok() && rBig.importedCount == 0,
                     "an oversized file is refused, not imported");
        ok &= expect(rBig.errors.size() == 1 && rBig.errors.first().contains("too large"),
                     "the oversize refusal is a named error");
    }

    // A row whose value is present but not a number must be a named skip, not
    // a silent 0: channel 0 is MIDI channel 1, not the any-channel wildcard,
    // and number 0 is CC 0 — either would bind the row to the wrong control.
    const auto r9 = settings.importProfile(
        writeImportFile("badnumbers.xml",
                        "<MidiProfile>"
                        "<Binding param=\"rx.afGain\" channel=\"abc\" type=\"0\" number=\"10\"/>"
                        "<Binding param=\"rx.nrEnable\" channel=\"0\" type=\"0\" number=\"xyz\"/>"
                        "<Binding param=\"tx.rfPower\" channel=\"0\" type=\"0\" number=\"12\"/>"
                        "</MidiProfile>\n"),
        validator);
    ok &= expect(r9.ok() && r9.importedCount == 1,
                 "XML: non-numeric channel/number rows don't become bindings");
    ok &= expect(r9.skippedBadType.size() == 2, "XML: both bad-value rows are skipped by name");
    {
        const auto imported = settings.loadProfile(r9.profileName);
        bool wrongBinding = false;
        for (const auto& b : imported)
            if (b.paramId == "rx.afGain" || b.paramId == "rx.nrEnable")
                wrongBinding = true;
        ok &= expect(!wrongBinding, "XML: no wrong binding is stored from a bad-value row");
    }

    // Absent attributes are not the same as bad ones — hand-written files omit
    // them and must still import.
    const auto r10 = settings.importProfile(
        writeImportFile("sparse.xml",
                        "<MidiProfile>"
                        "<Binding param=\"rx.afGain\" type=\"0\" number=\"7\"/>"
                        "</MidiProfile>\n"),
        validator);
    ok &= expect(r10.ok() && r10.importedCount == 1,
                 "XML: an absent attribute keeps its default and still imports");

    // A UTF-8 BOM (any file round-tripped through a Windows editor) must not
    // defeat the dialect sniff. QString::fromUtf8() consumes the BOM, so this
    // holds without a guard in the sniff — pinned here so a toolchain that
    // stops consuming it is caught by this test rather than by a user.
    const auto r11 = settings.importProfile(
        writeImportFile("bom.xml",
                        QByteArray("\xEF\xBB\xBF")
                            + "<MidiProfile>"
                              "<Binding param=\"rx.afGain\" channel=\"0\" type=\"0\" number=\"9\"/>"
                              "</MidiProfile>\n"),
        validator);
    ok &= expect(r11.ok() && r11.importedCount == 1, "a UTF-8 BOM still sniffs as profile XML");

    // Export → Import round trip through the user-facing export path.
    const auto exported = settings.exportProfile(
        fakeHome.path() + "/exported.xml", settings.loadProfile("CTR2-Test_v1.0"));
    ok &= expect(exported.ok() && exported.exportedCount == 6,
                 "export writes the current profile");
    const auto r8 = settings.importProfile(fakeHome.path() + "/exported.xml", validator);
    ok &= expect(r8.ok() && r8.importedCount == 6, "exported file re-imports cleanly");
    const auto exportFail = settings.exportProfile(
        fakeHome.path() + "/no-such-dir/out.xml",
        settings.loadProfile("CTR2-Test_v1.0"));
    ok &= expect(!exportFail.ok(), "export to an unwritable path reports an error");

    // An empty set must not report success: it serializes to a childless
    // <MidiProfile/> that importProfile() rejects, so "exported OK" would
    // hand the caller a file that cannot come back.
    const auto exportEmpty =
        settings.exportProfile(fakeHome.path() + "/empty.xml", QVector<MidiBinding>{});
    ok &= expect(!exportEmpty.ok() && exportEmpty.exportedCount == 0,
                 "exporting an empty binding set is refused, not written as a stub");

    // Pitch Bend round trip (#5024): Learn and manual entry store number = -1
    // (the PB message carries no controller number) and the writer exports it
    // verbatim, so the app's own export must come back as a binding, not a
    // "bad values" skip.
    {
        MidiBinding pb;
        pb.paramId = "rx.afGain";
        pb.channel = 2;
        pb.msgType = MidiBinding::PitchBend;
        pb.number = -1;
        const auto pbExported = settings.exportProfile(
            fakeHome.path() + "/pb.xml", QVector<MidiBinding>{pb});
        ok &= expect(pbExported.ok() && pbExported.exportedCount == 1,
                     "a learned Pitch Bend binding exports");
        const auto rPb = settings.importProfile(fakeHome.path() + "/pb.xml", validator);
        ok &= expect(rPb.ok() && rPb.importedCount == 1
                         && rPb.skippedBadType.isEmpty(),
                     "the app's own Pitch Bend export re-imports as a binding");
        const auto pbStored = settings.loadProfile(rPb.profileName);
        ok &= expect(pbStored.size() == 1
                         && pbStored.first().msgType == MidiBinding::PitchBend
                         && pbStored.first().number == -1
                         && pbStored.first().channel == 2,
                     "the round-tripped Pitch Bend keeps number -1 and its channel");

        // A hand-written PB row may omit the meaningless number entirely —
        // normalize to -1 rather than the other attributes' default of 0.
        const auto rPbAbsent = settings.importProfile(
            writeImportFile("pb-absent.xml",
                            "<MidiProfile>"
                            "<Binding param=\"rx.afGain\" channel=\"0\" type=\"3\"/>"
                            "</MidiProfile>\n"),
            validator);
        ok &= expect(rPbAbsent.ok() && rPbAbsent.importedCount == 1,
                     "a Pitch Bend row without a number imports");
        const auto pbAbsentStored = settings.loadProfile(rPbAbsent.profileName);
        ok &= expect(pbAbsentStored.size() == 1 && pbAbsentStored.first().number == -1,
                     "an absent Pitch Bend number normalizes to -1");

        // An in-range number on a hand-written PB row stays accepted (it
        // always was), but the stored binding normalizes to -1 so the store
        // can never re-export a number no PB message carries.
        const auto rPbInRange = settings.importProfile(
            writeImportFile("pb-inrange.xml",
                            "<MidiProfile>"
                            "<Binding param=\"rx.afGain\" channel=\"0\" type=\"3\" number=\"64\"/>"
                            "</MidiProfile>\n"),
            validator);
        ok &= expect(rPbInRange.ok() && rPbInRange.importedCount == 1,
                     "an in-range Pitch Bend number still imports");
        const auto pbInRangeStored = settings.loadProfile(rPbInRange.profileName);
        ok &= expect(pbInRangeStored.size() == 1 && pbInRangeStored.first().number == -1,
                     "an accepted Pitch Bend row stores number -1, not the file's value");
    }

    // ── Non-regular files: the size cap cannot see them ─────────────────────
    //
    // QFile::size() reports 0 for character devices, FIFOs and most /proc
    // entries, so a size-based cap lets exactly the files with no end through.
    // /dev/null is the safe representative: a character device that reads EOF
    // immediately, so this pins the guard with no chance of hanging the suite
    // the way /dev/zero or a FIFO would. Asserting the *specific* message
    // matters — a bare !ok() check would also pass on "couldn't open".
#ifdef Q_OS_UNIX
    if (QFile::exists(QStringLiteral("/dev/null"))) {
        const auto rDev = settings.importProfile(QStringLiteral("/dev/null"), validator);
        ok &= expect(!rDev.ok() && rDev.importedCount == 0,
                     "a character device is refused, not read");
        ok &= expect(rDev.errors.size() == 1
                         && rDev.errors.first().contains("not a regular file"),
                     "the non-regular-file refusal names the reason");
    }
#endif
    const auto rDir = settings.importProfile(fakeHome.path(), validator);
    ok &= expect(!rDir.ok() && rDir.errors.size() == 1
                     && rDir.errors.first().contains("not a regular file"),
                 "a directory is refused before any read");

    // ── ".map" section headers: decorated, commented, unknown ───────────────

    // Vendors decorate headers. Matching the whole header line demoted these
    // rows to "not a binding row" and lost both bindings.
    const auto rDecorated = settings.importProfile(
        writeImportFile("decorated.map",
                        "# Controls (16 knobs)\n"
                        "C100=freq;active\n"
                        "C110=cwspeed;active\n"),
        validator);
    ok &= expect(rDecorated.ok() && rDecorated.importedCount == 2,
                 "a decorated section header still binds its rows");
    ok &= expect(rDecorated.skippedUnknownParam.isEmpty(),
                 "a decorated header produces no bogus skips");

    // First-word matching also has to keep recognizing a decorated *feedback*
    // header. Bindable rows survive an unreadable header on their own (that is
    // the per-row rule below), so this is the case that actually depends on the
    // header being parsed: "# LEDs (8 indicators)" must still be understood as
    // the LEDs section and summarized, not reported as an unknown row.
    const auto rDecoratedLeds = settings.importProfile(
        writeImportFile("decorated-leds.map",
                        "# Controls\n"
                        "C100=freq\n"
                        "# LEDs (8 indicators)\n"
                        "L1=on\n"
                        "L2=on\n"),
        validator);
    ok &= expect(rDecoratedLeds.ok() && rDecoratedLeds.importedCount == 1,
                 "a decorated LEDs header still imports the controls above it");
    ok &= expect(rDecoratedLeds.skippedUnknownParam.size() == 1
                     && rDecoratedLeds.skippedUnknownParam.first().contains("LEDs")
                     && rDecoratedLeds.skippedUnknownParam.first().contains("2 row"),
                 "a decorated LEDs header is still recognized as a feedback section");

    // A comment line mid-section must not swallow the rows after it: the row
    // still looks like a control row, so it still binds.
    const auto rComment = settings.importProfile(
        writeImportFile("comment.map",
                        "# Controls\n"
                        "C100=freq;active\n"
                        "# the VFO knob sends relative steps\n"
                        "C110=cwspeed;active\n"),
        validator);
    ok &= expect(rComment.ok() && rComment.importedCount == 2,
                 "a comment between control rows doesn't drop the rows after it");

    // A section this build doesn't know, keyed in a dialect we can't use, is a
    // named skip — not a file-level error that loses the bindings around it.
    const auto rUnknownSection = settings.importProfile(
        writeImportFile("unknown-section.map",
                        "# Controls\n"
                        "C100=freq\n"
                        "# Encoders\n"
                        "E1=turn\n"),
        validator);
    ok &= expect(rUnknownSection.ok() && rUnknownSection.importedCount == 1,
                 "an unknown section doesn't fail the file");
    ok &= expect(rUnknownSection.skippedUnknownParam.size() == 1
                     && rUnknownSection.skippedUnknownParam.first().contains("E1=turn"),
                 "an unusable row under an unknown header is named with its key");

    // Under an unreadable header the naming is per row, so two rows that share
    // a value stay distinct — the case that made "FORMAT_VERSION=1" and a
    // dozen other metadata rows all report as the same unreadable "1".
    const auto rDistinct = settings.importProfile(
        writeImportFile("distinct.map",
                        "# Controls\n"
                        "C100=freq\n"
                        "# Metadata\n"
                        "FORMAT_VERSION=1\n"
                        "REVISION=1\n"),
        validator);
    ok &= expect(rDistinct.ok() && rDistinct.skippedUnknownParam.size() == 2
                     && rDistinct.skippedUnknownParam.first().contains("FORMAT_VERSION=1")
                     && rDistinct.skippedUnknownParam.at(1).contains("REVISION=1"),
                 "metadata rows sharing a value are named separately, by key");

    // Skips carry the whole row. "on" alone is unreadable; "L1=on" says what
    // was skipped, and two rows sharing a value stay distinct.
    const auto rSkipNames = settings.importProfile(
        writeImportFile("skipnames.map",
                        "# Controls\n"
                        "C100=freq\n"
                        "# LEDs\n"
                        "L1=on\n"
                        "L2=on\n"),
        validator);
    ok &= expect(rSkipNames.ok() && rSkipNames.importedCount == 1,
                 "a LEDs section still lets the controls import");
    ok &= expect(rSkipNames.skippedUnknownParam.size() == 1
                     && rSkipNames.skippedUnknownParam.first().contains("2 row"),
                 "same-valued LED rows are counted, not collapsed into one name");

    // The sniff scans the first lines for a header rather than testing only
    // the first character, so a preamble doesn't reject the whole file.
    const auto rPreamble = settings.importProfile(
        writeImportFile("preamble.map",
                        "FORMAT_VERSION=1\n"
                        "DEVICE=CTR2-Max\n"
                        "# Controls\n"
                        "C100=freq;active\n"),
        validator);
    ok &= expect(rPreamble.ok() && rPreamble.importedCount == 1,
                 "a KEY=value preamble still sniffs as a map");

    // ── XML attribute handling ──────────────────────────────────────────────

    // Absent `type` now follows the same rule as absent channel/number.
    const auto rNoType = settings.importProfile(
        writeImportFile("notype.xml",
                        "<MidiProfile>"
                        "<Binding param=\"rx.afGain\" channel=\"0\" number=\"7\"/>"
                        "</MidiProfile>\n"),
        validator);
    ok &= expect(rNoType.ok() && rNoType.importedCount == 1,
                 "XML: an absent type attribute defaults to CC and still imports");
    {
        const auto stored = settings.loadProfile(rNoType.profileName);
        ok &= expect(stored.size() == 1 && stored.first().msgType == MidiBinding::CC,
                     "XML: the defaulted type really is CC");
    }
    // A present-but-unreadable type is still a named skip.
    const auto rBadType = settings.importProfile(
        writeImportFile("badtype.xml",
                        "<MidiProfile>"
                        "<Binding param=\"rx.afGain\" channel=\"0\" type=\"zz\" number=\"7\"/>"
                        "</MidiProfile>\n"),
        validator);
    ok &= expect(rBadType.importedCount == 0 && rBadType.skippedBadType.size() == 1,
                 "XML: a non-numeric type is still a named skip, not a default");

    // Pitch Bend ignores `number` at dispatch, but an out-of-range value would
    // be stored and re-exported as a number no MIDI message can carry.
    const auto rPitch = settings.importProfile(
        writeImportFile("pitch.xml",
                        "<MidiProfile>"
                        "<Binding param=\"rx.afGain\" channel=\"0\" type=\"3\" number=\"99999\"/>"
                        "<Binding param=\"tx.rfPower\" channel=\"0\" type=\"3\" number=\"5\"/>"
                        "</MidiProfile>\n"),
        validator);
    ok &= expect(rPitch.ok() && rPitch.importedCount == 1,
                 "XML: an out-of-range Pitch Bend number is skipped, not stored");
    ok &= expect(rPitch.skippedBadType.size() == 1
                     && rPitch.skippedBadType.first().contains("number"),
                 "XML: the out-of-range Pitch Bend row is named");

    // ── Profile store: dotted names list + round-trip (#4974) ───────────────

    settings.saveProfile("CTR2 v1.0", saved);
    ok &= expect(settings.availableProfiles().contains("CTR2 v1.0"),
                 "profile store: dotted name lists untruncated");
    ok &= expect(settings.loadProfile("CTR2 v1.0").size() == saved.size(),
                 "profile store: dotted name loads what was saved");
    settings.deleteProfile("CTR2 v1.0");
    ok &= expect(!settings.availableProfiles().contains("CTR2 v1.0"),
                 "profile store: dotted name deletes its own file");

    // ── Profile store: names are names, not paths (#4975) ───────────────────

    ok &= expect(MidiSettings::isValidProfileName("CTR2 v1.0"),
                 "name guard: ordinary dotted name accepted");
    ok &= expect(MidiSettings::isValidProfileName("Tenerife contest ÉÑ"),
                 "name guard: non-ASCII name accepted");
    ok &= expect(!MidiSettings::isValidProfileName("a/b"),
                 "name guard: forward separator rejected");
    ok &= expect(!MidiSettings::isValidProfileName("a\\b"),
                 "name guard: backslash separator rejected");
    ok &= expect(!MidiSettings::isValidProfileName("."),
                 "name guard: '.' rejected");
    ok &= expect(!MidiSettings::isValidProfileName(".."),
                 "name guard: '..' rejected");
    ok &= expect(!MidiSettings::isValidProfileName(".hidden"),
                 "name guard: leading-dot (hidden-file) name rejected");
    ok &= expect(!MidiSettings::isValidProfileName("   "),
                 "name guard: whitespace-only rejected");

    // The guards hold at the filesystem, not only in the predicate.
    const QString appConfigDir = configRoot + "/AetherSDR";
    settings.saveProfile("../escape", saved);
    ok &= expect(!QFile::exists(appConfigDir + "/escape.xml"),
                 "profile store: save with ../ writes nothing outside the store");
    settings.saveProfile("a/b", saved);
    ok &= expect(!QDir(appConfigDir + "/midi/a").exists(),
                 "profile store: save with a separator creates no directory chain");
    // A leading dot is the Unix hidden-file convention: the listing's
    // QDir::Files scan omits ".hidden.xml", so an accepted save would
    // succeed and then vanish from availableProfiles() (#5083 review).
    // The guard refuses the name before a file exists to hide.
    settings.saveProfile(".hidden", saved);
    ok &= expect(!QFile::exists(appConfigDir + "/midi/.hidden.xml"),
                 "profile store: leading-dot name writes no hidden file");
    ok &= expect(settings.loadProfile(".hidden").isEmpty(),
                 "profile store: leading-dot name loads nothing");

    {
        QFile planted(appConfigDir + "/planted.xml");
        if (planted.open(QIODevice::WriteOnly | QIODevice::Text)) {
            planted.write("<MidiProfile version=\"1\">"
                          "<Binding param=\"rx.afGain\" channel=\"0\" type=\"0\""
                          " number=\"7\" inverted=\"False\"/></MidiProfile>\n");
            planted.close();
        }
    }
    ok &= expect(settings.loadProfile("../planted").isEmpty(),
                 "profile store: load with ../ reads nothing outside the store");
    settings.deleteProfile("../planted");
    ok &= expect(QFile::exists(appConfigDir + "/planted.xml"),
                 "profile store: delete with ../ removes nothing outside the store");
    QFile::remove(appConfigDir + "/planted.xml");

    // A legacy file whose name the guard refuses (backslash is a legal Unix
    // filename character, and the pre-guard GUI could create it) must not
    // list: the list may only hand out names load/delete will serve. The
    // file itself stays on disk.
#ifndef Q_OS_WIN
    {
        QFile legacy(appConfigDir + "/midi/back\\slash.xml");
        QDir().mkpath(appConfigDir + "/midi");
        if (legacy.open(QIODevice::WriteOnly | QIODevice::Text)) {
            legacy.write("<MidiProfile version=\"1\">"
                         "<Binding param=\"rx.afGain\" channel=\"0\" type=\"0\""
                         " number=\"7\" inverted=\"False\"/></MidiProfile>\n");
            legacy.close();
        }
    }
    ok &= expect(!settings.availableProfiles().contains("back\\slash"),
                 "profile store: a legacy separator-named file does not list");
    ok &= expect(QFile::exists(appConfigDir + "/midi/back\\slash.xml"),
                 "profile store: the unlisted legacy file stays on disk");
    QFile::remove(appConfigDir + "/midi/back\\slash.xml");
#endif

    // An import whose file name derives a refused store name ("...map" ->
    // "..") falls back to the default name instead of surfacing a
    // misleading write error.
    const auto rDotsName = settings.importProfile(
        writeImportFile("...map", mapContent), validator);
    ok &= expect(rDotsName.ok()
                     && rDotsName.profileName.startsWith("Imported profile"),
                 "import: a refused derived name falls back to the default");

    // ── Profile store: the write reports its outcome (#5077) ────────────────

    ok &= expect(settings.saveProfile("Outcome", saved),
                 "profile store: a successful save returns true");
    ok &= expect(settings.saveProfile("Outcome", saved),
                 "profile store: overwriting an existing profile returns true");
    ok &= expect(!settings.saveProfile("a/b", saved),
                 "profile store: a refused name returns false");
    // Existence is the filesystem's answer: on a case-insensitive volume
    // "outcome" names the same file as "Outcome", on a case-sensitive one it
    // does not — either way it must agree with the path the store writes.
    ok &= expect(MidiSettings::profileExists("Outcome"),
                 "profile store: a saved profile exists");
    // Compared against what the store itself hands back for that spelling:
    // on a case-insensitive volume "outcome" loads "Outcome", on a
    // case-sensitive one it loads nothing — existence must agree either way.
    ok &= expect(MidiSettings::profileExists("outcome")
                     == !settings.loadProfile("outcome").isEmpty(),
                 "profile store: existence check agrees with loadProfile on case");
    ok &= expect(!MidiSettings::profileExists("a/b"),
                 "profile store: a refused name never exists");
    // An empty set is refused (as exportProfile refuses it) and, refused,
    // leaves the existing profile untouched — Clear All → Save must not
    // replace a profile with nothing.
    ok &= expect(!settings.saveProfile("Outcome", QVector<MidiBinding>{}),
                 "profile store: saving an empty binding set returns false");
    ok &= expect(settings.loadProfile("Outcome").size() == saved.size(),
                 "profile store: the refused empty save left the profile intact");
    settings.deleteProfile("Outcome");
    ok &= expect(!MidiSettings::profileExists("Outcome"),
                 "profile store: a deleted profile no longer exists");
    // Block the store directory with a plain file so mkpath and open both
    // fail: the failure must reach the caller instead of being swallowed.
    QDir(appConfigDir + "/midi").removeRecursively();
    {
        QFile blocker(appConfigDir + "/midi");
        if (blocker.open(QIODevice::WriteOnly)) {
            blocker.write("not a directory");
            blocker.close();
        }
    }
    ok &= expect(!settings.saveProfile("Blocked", saved),
                 "profile store: an unwritable store returns false");
    QFile::remove(appConfigDir + "/midi");
    // A failed overwrite leaves the previous profile intact: write one, make
    // the store read-only, try to overwrite it, and read the original back.
#ifndef Q_OS_WIN
    // Directory permissions do not bind root (CI containers may run as root)
    // and are not honoured the same way on Windows, so this arm is POSIX
    // non-root only; the bool contract itself is covered above on every OS.
    if (::geteuid() != 0) {
    ok &= expect(settings.saveProfile("Keep", saved), "profile store: original written");
    QFile::setPermissions(appConfigDir + "/midi",
                          QFileDevice::ReadOwner | QFileDevice::ExeOwner);
    QVector<MidiBinding> bigger = saved;
    bigger.append(saved.first());
    const bool overwriteFailed = !settings.saveProfile("Keep", bigger);
    QFile::setPermissions(appConfigDir + "/midi",
                          QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    ok &= expect(overwriteFailed, "profile store: overwrite into a read-only store returns false");
    ok &= expect(settings.loadProfile("Keep").size() == saved.size(),
                 "profile store: the failed overwrite left the original profile intact");
    settings.deleteProfile("Keep");
    } else {
        std::cout << "[SKIP] profile store: read-only store arm (running as root)\n";
    }
#endif

    QFile::remove(configRoot + "/AetherSDR/midi.settings");
    QDir(configRoot + "/AetherSDR").removeRecursively();

    return ok ? 0 : 1;
}
