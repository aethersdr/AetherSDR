#include "core/MidiSettings.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <iostream>

using namespace MasterSDR;

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
    QTemporaryDir fakeHome(QDir::tempPath() + "/mastersdr-midi-settings-test-XXXXXX");
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
    QDir(configRoot + "/MasterSDR").removeRecursively();

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

    QFile midiFile(configRoot + "/MasterSDR/midi.settings");
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

    QFile::remove(configRoot + "/MasterSDR/midi.settings");
    QDir(configRoot + "/MasterSDR").removeRecursively();

    return ok ? 0 : 1;
}
