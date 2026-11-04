#include "core/NetScheduleStore.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <iostream>

using namespace AetherSDR;

namespace {

bool expect(bool condition, const char* message)
{
    if (!condition)
        std::cerr << "FAIL: " << message << '\n';
    return condition;
}

NetEntry sampleNet()
{
    NetEntry e;
    e.id = "fixed-id-1";
    e.name = "Tuesday County ARES Net";
    e.enabled = true;
    e.rrule = "FREQ=WEEKLY;BYDAY=TU";
    e.startDate = "2026-06-02";
    e.timeOfDay = "20:00";
    e.timezone = "America/Chicago";
    e.reminderLeadMinutes = 10;
    e.durationMinutes = 45;
    e.notes = "NCS W1ABC";
    e.preset.freq = 146.94;
    e.preset.mode = "FM";
    e.preset.step = 5000;
    e.preset.rxFilterLow = -8000;
    e.preset.rxFilterHigh = 8000;
    e.preset.offsetDir = "down";
    e.preset.repeaterOffset = 0.6;
    e.preset.toneMode = "ctcss_tx";
    e.preset.toneValue = 103.5;
    e.preset.squelch = true;
    e.preset.squelchLevel = 20;
    return e;
}

} // namespace

int main()
{
    bool ok = true;

    // --- round-trip -----------------------------------------------------
    {
        const QList<NetEntry> nets = {sampleNet()};
        const QByteArray json = NetScheduleStore::serialize(nets, "2026-06-19T14:00:00Z");
        const auto result = NetScheduleStore::parse(json);
        ok &= expect(result.ok(), "round-trip parses without errors");
        ok &= expect(result.version == NetScheduleStore::kFormatVersion, "version preserved");
        ok &= expect(result.nets.size() == 1, "one net round-tripped");

        const NetEntry& g = result.nets.first();
        const NetEntry s = sampleNet();
        ok &= expect(g.id == s.id && g.name == s.name && g.enabled == s.enabled,
                     "scalar fields preserved");
        ok &= expect(g.rrule == s.rrule && g.startDate == s.startDate
                         && g.timeOfDay == s.timeOfDay && g.timezone == s.timezone,
                     "recurrence fields preserved");
        ok &= expect(g.reminderLeadMinutes == s.reminderLeadMinutes
                         && g.durationMinutes == s.durationMinutes && g.notes == s.notes,
                     "reminder/notes preserved");
        ok &= expect(g.preset.freq == s.preset.freq && g.preset.mode == s.preset.mode
                         && g.preset.step == s.preset.step,
                     "preset freq/mode/step preserved");
        ok &= expect(g.preset.rxFilterLow == s.preset.rxFilterLow
                         && g.preset.rxFilterHigh == s.preset.rxFilterHigh,
                     "preset filter preserved");
        ok &= expect(g.preset.offsetDir == s.preset.offsetDir
                         && g.preset.repeaterOffset == s.preset.repeaterOffset
                         && g.preset.toneMode == s.preset.toneMode
                         && g.preset.toneValue == s.preset.toneValue,
                     "preset repeater/tone preserved");
        ok &= expect(g.preset.squelch == s.preset.squelch
                         && g.preset.squelchLevel == s.preset.squelchLevel,
                     "preset squelch preserved");
    }

    // --- tolerant parsing ----------------------------------------------
    {
        // Unknown future field ignored; missing optionals take defaults; missing
        // id gets assigned.
        const QByteArray json = R"({
            "format": "aether.netschedule",
            "version": 1,
            "nets": [
                { "name": "Minimal Net", "rrule": "FREQ=DAILY", "futureField": 42 }
            ]
        })";
        const auto result = NetScheduleStore::parse(json);
        ok &= expect(result.ok(), "tolerant parse of minimal entry");
        ok &= expect(result.nets.size() == 1, "minimal entry parsed");
        ok &= expect(!result.nets.first().id.isEmpty(), "missing id assigned");
        ok &= expect(result.nets.first().timeOfDay == "20:00", "missing timeOfDay defaulted");
        ok &= expect(result.nets.first().enabled, "missing enabled defaults true");
    }

    // --- error handling -------------------------------------------------
    {
        const auto bad = NetScheduleStore::parse("{ not json ");
        ok &= expect(!bad.ok(), "malformed JSON reports an error");

        const QByteArray future = R"({"format":"aether.netschedule","version":999,"nets":[]})";
        const auto fr = NetScheduleStore::parse(future);
        ok &= expect(!fr.ok(), "newer-than-supported version reports an error");
    }

    // Imported preset tokens must remain safe even when callers use nets
    // without checking errors (the startup schedule loader does this).
    {
        const QStringList fields = {"mode", "offsetDir", "toneMode"};
        QStringList invalid = {"USB C2|xmit 1", "USB\nC2|xmit 1", " USB", "USB ",
                               "USB=FM", "USB|FM", QString(65, QLatin1Char('A')),
                               QStringLiteral("USB") + QChar(0x7f),
                               QStringLiteral("USB") + QChar(0xa0),
                               QStringLiteral("USB") + QChar(0x2028)};
        for (ushort control = 0; control < 0x20; ++control) {
            invalid.append(QStringLiteral("USB") + QChar(control));
        }
        for (const QString& field : fields) {
            for (const QString& value : invalid) {
                QJsonObject bad{{"name", "Unsafe net"}, {"preset", QJsonObject{{field, value}}}};
                QJsonObject good{{"name", "Safe net"}, {"preset", QJsonObject{{"mode", "FDVU"}}}};
                const QJsonObject root{{"nets", QJsonArray{good, bad}}};
                const auto parsed = NetScheduleStore::parse(QJsonDocument(root).toJson());
                const QByteArray context = (field + ": " + value.toHtmlEscaped()).toUtf8();
                ok &= expect(!parsed.ok(), context.constData());
                ok &= expect(parsed.nets.size() == 1 && parsed.nets.first().name == "Safe net",
                             "unsafe entry omitted, valid sibling retained");
                ok &= expect(parsed.errors.join(' ').contains(field)
                                 && parsed.errors.join(' ').contains("2"),
                             "error identifies the preset field and one-based net position");
            }
        }
        for (const QString& mode : QStringList{"FDVU", "FDVL", "FDV", "DSTR",
                                               "Future_Waveform-1.2/Test", QString(64, QLatin1Char('A')), ""}) {
            NetEntry net = sampleNet();
            net.preset.mode = mode;
            net.preset.offsetDir.clear();
            net.preset.toneMode.clear();
            const auto parsed = NetScheduleStore::parse(NetScheduleStore::serialize({net}));
            ok &= expect(parsed.ok() && parsed.nets.size() == 1
                             && parsed.nets.first().preset.mode == mode,
                         "safe, future, maximum-length and empty optional tokens preserved");
        }
    }

    // --- merge policies -------------------------------------------------
    {
        QList<NetEntry> existing = {sampleNet()};   // id = fixed-id-1
        NetEntry incoming = sampleNet();            // same id
        incoming.name = "Renamed Net";

        const auto skipped = NetScheduleStore::merge(existing, {incoming},
                                                     NetScheduleStore::MergePolicy::Skip);
        ok &= expect(skipped.size() == 1 && skipped.first().name == "Tuesday County ARES Net",
                     "Skip keeps the existing entry");

        const auto overwritten = NetScheduleStore::merge(existing, {incoming},
                                                         NetScheduleStore::MergePolicy::Overwrite);
        ok &= expect(overwritten.size() == 1 && overwritten.first().name == "Renamed Net",
                     "Overwrite replaces the existing entry");

        const auto duplicated = NetScheduleStore::merge(existing, {incoming},
                                                        NetScheduleStore::MergePolicy::Duplicate);
        ok &= expect(duplicated.size() == 2, "Duplicate adds a second entry");
        ok &= expect(duplicated.at(0).id != duplicated.at(1).id,
                     "Duplicate assigns a fresh id");

        NetEntry brandNew = sampleNet();
        brandNew.id = "different-id";
        const auto added = NetScheduleStore::merge(existing, {brandNew},
                                                   NetScheduleStore::MergePolicy::Skip);
        ok &= expect(added.size() == 2, "new id is inserted regardless of policy");
    }

    return ok ? 0 : 1;
}
