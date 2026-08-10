#include "core/MidiSettings.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSet>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <iostream>

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
    ok &= expect(r1.profileName == "CTR2-Test_v1_0", "map import names profile from file name");
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
    ok &= expect(r2.ok() && r2.profileName == "CTR2-Test_v1_0 (2)",
                 "a name collision gets a suffix, never an overwrite");

    const auto r3 = settings.importProfile(writeImportFile("garbage.map", "hello world\n"),
                                           validator);
    ok &= expect(!r3.ok() && r3.importedCount == 0, "unrecognized content fails loudly");

    {
        // Native XML round trip: the profile the map import just stored.
        const auto r4 = settings.importProfile(
            configRoot + "/AetherSDR/midi/CTR2-Test_v1_0.xml", validator);
        ok &= expect(r4.ok() && r4.importedCount == 6, "native XML profile re-imports");
        ok &= expect(r4.profileName == "CTR2-Test_v1_0 (3)", "XML re-import takes the next suffix");
        const auto again = settings.loadProfile(r4.profileName);
        const auto first = settings.loadProfile("CTR2-Test_v1_0");
        bool same = again.size() == first.size();
        for (int i = 0; same && i < again.size(); ++i)
            same = sameBinding(again[i], first[i]);
        ok &= expect(same, "XML round trip preserves every binding field");
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

    // Export → Import round trip through the user-facing export path.
    const auto exported = settings.exportProfile(
        fakeHome.path() + "/exported.xml", settings.loadProfile("CTR2-Test_v1_0"));
    ok &= expect(exported.ok() && exported.exportedCount == 6,
                 "export writes the current profile");
    const auto r8 = settings.importProfile(fakeHome.path() + "/exported.xml", validator);
    ok &= expect(r8.ok() && r8.importedCount == 6, "exported file re-imports cleanly");
    const auto exportFail = settings.exportProfile(
        fakeHome.path() + "/no-such-dir/out.xml",
        settings.loadProfile("CTR2-Test_v1_0"));
    ok &= expect(!exportFail.ok(), "export to an unwritable path reports an error");

    QFile::remove(configRoot + "/AetherSDR/midi.settings");
    QDir(configRoot + "/AetherSDR").removeRecursively();

    return ok ? 0 : 1;
}
