// Green Heron Everyware wire-protocol tests.
//
// Every fixture below is a VERBATIM byte string captured off a live Everyware
// server — not hand-written. That is the point of this file: it pins the
// parser against what the device actually emits, so the protocol claims in
// core/GreenHeronProtocol.h are evidence rather than assertion (Constitution
// Principle VIII). The fixture lengths are asserted first; if they drift, the
// fixtures are no longer what the device sent and every test below is
// measuring the wrong thing.
//
// Run:  ./build/green_heron_protocol_test

#include "core/GreenHeronProtocol.h"

#include <QByteArray>
#include <QStringList>

#include <algorithm>
#include <cstdio>
#include <string>

using namespace AetherSDR;
using namespace AetherSDR::GreenHeron;

namespace {

int g_failed = 0;

void report(const char* name, bool ok, const std::string& detail = {})
{
    std::printf("%s %-62s %s\n", ok ? "[ OK ]" : "[FAIL]", name, detail.c_str());
    if (!ok) {
        ++g_failed;
    }
}

// One SWITCHUPDATE, exactly as it arrived (the recurring 33-byte segment).
const QByteArray kUpdate33 =
    QByteArrayLiteral("SWITCHUPDATE\x1f" "AS-84F-2\x1f" "OFF\x1f" "0\x1f" "-27\r\n");

// The recurring 109-byte segment: three records in one read, and note it
// begins mid-cycle with a LOCKS rather than an UPDATE.
const QByteArray kTriple109 =
    QByteArrayLiteral("SWITCHLOCKS\x1f" "AS-84F-2\x1f" "OFF\x1f" "OFF\x1f" "OFF\x1f" "OFF\r\n")
    + QByteArrayLiteral("SWITCHUPDATE\x1f" "AS-84F-1\x1f" "OFF\x1f" "0\x1f" "-27\r\n")
    + QByteArrayLiteral("SWITCHLOCKS\x1f" "AS-84F-1\x1f" "OFF\x1f" "OFF\x1f" "OFF\x1f" "OFF\r\n");

const QByteArray kAdd195 =
    QByteArrayLiteral("SWITCHADD\x1f" "1\x1f" "AS-84F-1")
    + QByteArrayLiteral("\x1f" "Beam-10\x1d" "0\x1d" "0\x1d" "false")
    + QByteArrayLiteral("\x1f" "Beam-15\x1d" "0\x1d" "0\x1d" "false")
    + QByteArrayLiteral("\x1f" "Beam-20\x1d" "0\x1d" "0\x1d" "false")
    + QByteArrayLiteral("\x1f" "EFHW-40\x1d" "0\x1d" "0\x1d" "false")
    + QByteArrayLiteral("\x1f" "EFHW-80\x1d" "0\x1d" "0\x1d" "false")
    + QByteArrayLiteral("\x1f" "Dipole-6\x1d" "0\x1d" "0\x1d" "false")
    + QByteArrayLiteral("\x1f" "Vertical-NoFilters\x1d" "0\x1d" "0\x1d" "false")
    + QByteArrayLiteral("\x1f" "Dummy Load\x1d" "0\x1d" "0\x1d" "false")
    + QByteArrayLiteral("\x1f" "OFF\x1d" "0\x1d" "0\x1d" "false\r\n");

QStringList portNames(const Record& record)
{
    QStringList names;
    for (const Port& port : record.ports) {
        names.append(port.name);
    }
    return names;
}

void testFixturesStillMatchTheWire()
{
    report("UPDATE fixture is the 33-byte segment", kUpdate33.size() == 33,
           std::to_string(kUpdate33.size()));
    report("TRIPLE fixture is the 109-byte segment", kTriple109.size() == 109,
           std::to_string(kTriple109.size()));
    report("SWITCHADD fixture is the 195-byte roster", kAdd195.size() == 195,
           std::to_string(kAdd195.size()));
}

// ── Framing ─────────────────────────────────────────────────────────────────

void testOneSegmentCanHoldSeveralRecords()
{
    QByteArray buffer = kTriple109;
    const QVector<QByteArray> records = splitRecords(buffer);
    report("one read yields three records", records.size() == 3,
           std::to_string(records.size()));
    report("nothing left over", buffer.isEmpty());
}

void testPartialRecordIsHeldBackNotParsed()
{
    QByteArray buffer = QByteArrayLiteral("SWITCHUPDATE\x1f" "AS-84F-2\x1f" "OFF\x1f" "0\x1f" "-2");
    const QByteArray before = buffer;
    const QVector<QByteArray> records = splitRecords(buffer);
    report("no record from a partial line", records.isEmpty());
    report("the partial line is kept verbatim", buffer == before);
}

void testRecordSplitAtEveryByteStillParses()
{
    // A record arriving in two pieces must survive wherever TCP cut it —
    // including between the CR and the LF.
    const Record whole = parse(kUpdate33.left(kUpdate33.size() - 2));
    bool allOk = true;
    int firstBadCut = -1;

    for (int cut = 1; cut < kUpdate33.size(); ++cut) {
        QByteArray buffer = kUpdate33.left(cut);
        QVector<QByteArray> records = splitRecords(buffer);
        // The only cut that can complete a record early is the last one.
        if (cut < kUpdate33.size() && !records.isEmpty()) {
            allOk = false;
            firstBadCut = cut;
            break;
        }
        buffer += kUpdate33.mid(cut);
        records = splitRecords(buffer);
        if (records.size() != 1) {
            allOk = false;
            firstBadCut = cut;
            break;
        }
        const Record rejoined = parse(records.first());
        if (rejoined.type != whole.type || rejoined.switchName != whole.switchName
            || rejoined.selected != whole.selected
            || rejoined.wirelessSignal != whole.wirelessSignal) {
            allOk = false;
            firstBadCut = cut;
            break;
        }
    }

    report("a record cut at any byte still parses", allOk,
           allOk ? std::string{} : "first bad cut " + std::to_string(firstBadCut));
}

void testRecordsAccumulateAcrossReads()
{
    QByteArray buffer;
    QVector<QByteArray> seen;
    const QVector<QByteArray> chunks = {
        kAdd195.left(100),
        kAdd195.mid(100) + kTriple109.left(40),
        kTriple109.mid(40),
    };
    for (const QByteArray& chunk : chunks) {
        buffer += chunk;
        seen += splitRecords(buffer);
    }

    QStringList types;
    for (const QByteArray& raw : seen) {
        switch (parse(raw).type) {
        case RecordType::SwitchAdd:    types.append(QStringLiteral("Add")); break;
        case RecordType::SwitchUpdate: types.append(QStringLiteral("Update")); break;
        case RecordType::SwitchLocks:  types.append(QStringLiteral("Locks")); break;
        case RecordType::Unknown:      types.append(QStringLiteral("Unknown")); break;
        }
    }
    const QString joined = types.join(QLatin1Char(','));
    report("records accumulate across reads in order",
           buffer.isEmpty() && joined == QLatin1String("Add,Locks,Update,Locks"),
           joined.toStdString());
}

// ── Parsing ─────────────────────────────────────────────────────────────────

void testParseSwitchUpdate()
{
    const Record rec = parse(kUpdate33.left(kUpdate33.size() - 2));
    report("SWITCHUPDATE recognised", rec.type == RecordType::SwitchUpdate);
    report("SWITCHUPDATE switch", rec.switchName == QLatin1String("AS-84F-2"),
           rec.switchName.toStdString());
    report("SWITCHUPDATE selection", rec.selected == QLatin1String("OFF"),
           rec.selected.toStdString());
    // Carried through verbatim, never interpreted.
    report("SWITCHUPDATE unknown field carried", rec.unknownC == QLatin1String("0"));
    report("SWITCHUPDATE wireless signal carried",
           rec.wirelessSignal == QLatin1String("-27"),
           rec.wirelessSignal.toStdString());
}

void testParseSwitchLocks()
{
    QByteArray buffer = kTriple109;
    const QVector<QByteArray> records = splitRecords(buffer);
    const Record rec = parse(records.first());
    report("SWITCHLOCKS recognised", rec.type == RecordType::SwitchLocks);
    report("SWITCHLOCKS switch", rec.switchName == QLatin1String("AS-84F-2"),
           rec.switchName.toStdString());
    report("SWITCHLOCKS carries four slots",
           rec.locks == QStringList({"OFF", "OFF", "OFF", "OFF"}),
           rec.locks.join(QLatin1Char(',')).toStdString());
}

void testParseSwitchAddRoster()
{
    const Record rec = parse(kAdd195.left(kAdd195.size() - 2));
    report("SWITCHADD recognised", rec.type == RecordType::SwitchAdd);
    report("SWITCHADD switch", rec.switchName == QLatin1String("AS-84F-1"),
           rec.switchName.toStdString());
    report("SWITCHADD leading group carried", rec.unknownGroup == QLatin1String("1"));

    const QStringList expected = {
        "Beam-10", "Beam-15", "Beam-20", "EFHW-40", "EFHW-80",
        "Dipole-6", "Vertical-NoFilters", "Dummy Load", "OFF",
    };
    report("SWITCHADD roster in announcement order", portNames(rec) == expected,
           portNames(rec).join(QLatin1Char(',')).toStdString());

    // "Dummy Load" has a space in it: split on US, never on whitespace.
    report("a port name containing a space survives",
           rec.ports.size() == 9 && rec.ports.at(7).name == QLatin1String("Dummy Load"));

    bool subfieldsCarried = true;
    for (const Port& port : rec.ports) {
        if (port.unknownA != QLatin1String("0") || port.unknownB != QLatin1String("0")
            || port.unknownFlag != QLatin1String("false")) {
            subfieldsCarried = false;
        }
    }
    report("unestablished SWITCHADD subfields are carried, not dropped", subfieldsCarried);
}

void testUnknownVerbIsSurfacedNotFatal()
{
    // A capture of one idle installation is a thin basis for claiming the
    // whole vocabulary, so an unseen verb must surface rather than be an error.
    const Record rec = parse(QByteArrayLiteral("SWITCHTHING\x1f" "AS-84F-1\x1f" "whatever"));
    report("unknown verb surfaced", rec.type == RecordType::Unknown);
    report("unknown verb name kept", rec.verb == QLatin1String("SWITCHTHING"),
           rec.verb.toStdString());
    report("unknown verb fields kept",
           rec.fields == QStringList({"AS-84F-1", "whatever"}),
           rec.fields.join(QLatin1Char(',')).toStdString());

    const Record garbage = parse(QByteArrayLiteral("\x00\xff not a record"));
    report("garbage does not become a switch record",
           garbage.type == RecordType::Unknown);

    // A truncated but well-formed verb must not be mistaken for the real thing.
    const Record shortUpdate = parse(QByteArrayLiteral("SWITCHUPDATE\x1f" "AS-84F-1"));
    report("a SWITCHUPDATE missing its selection is not accepted",
           shortUpdate.type == RecordType::Unknown);
}

// ── Boundary caps (Constitution Principle VII) ──────────────────────────────

void testBoundaryCaps()
{
    // A peer that never terminates a record must not grow our buffer forever.
    QByteArray buffer(kMaxPendingBytes + 1024, 'x');
    bool dropped = false;
    const QVector<QByteArray> records = splitRecords(buffer, &dropped);
    report("an unterminated flood yields no records", records.isEmpty());
    report("an unterminated flood is discarded, not buffered", buffer.isEmpty());
    report("the discard is reported to the caller", dropped);

    // A single terminated record above the cap is dropped, and the records
    // around it still arrive.
    QByteArray withGiant = kUpdate33;
    withGiant += QByteArray(kMaxRecordBytes + 1, 'y');
    withGiant += QByteArrayLiteral("\r\n");
    withGiant += kUpdate33;
    bool giantDropped = false;
    const QVector<QByteArray> kept = splitRecords(withGiant, &giantDropped);
    report("an oversized record is dropped", kept.size() == 2,
           std::to_string(kept.size()));
    report("dropping an oversized record is reported", giantDropped);

    // A record claiming more fields than the cap is truncated, not allocated for.
    QByteArray manyFields = QByteArrayLiteral("SWITCHLOCKS\x1f" "AS-84F-1");
    for (int i = 0; i < kMaxFieldsPerRecord * 4; ++i) {
        manyFields += kUnitSeparator;
        manyFields += "OFF";
    }
    const Record capped = parse(manyFields);
    report("field count is capped", capped.locks.size() <= kMaxFieldsPerRecord,
           std::to_string(capped.locks.size()));

    // Same for the SWITCHADD port roster.
    QByteArray manyPorts = QByteArrayLiteral("SWITCHADD\x1f" "1\x1f" "AS-84F-1");
    for (int i = 0; i < kMaxPortsPerSwitch * 4; ++i) {
        manyPorts += kUnitSeparator;
        manyPorts += "Beam-10\x1d" "0\x1d" "0\x1d" "false";
    }
    const Record cappedPorts = parse(manyPorts);
    report("port count is capped", cappedPorts.ports.size() <= kMaxPortsPerSwitch,
           std::to_string(cappedPorts.ports.size()));
}

// ── Command encoding ────────────────────────────────────────────────────────

void testEncodeSelectMatchesTheWire()
{
    // These three are the bytes captured from the vendor's own client, and the
    // lengths the very first capture showed when only packet sizes were
    // available. Byte-identical output is what makes this client's commands
    // and the vendor client's indistinguishable to the device.
    const QByteArray beam = encodeSelect(QStringLiteral("AS-84F-4"), QStringLiteral("Beam-20"));
    report("SET_SWITCH Beam-20 is byte-identical",
           beam == QByteArrayLiteral("SET_SWITCH\x1f" "AS-84F-4\x1f" "Beam-20\r\n"),
           beam.toStdString());
    report("SET_SWITCH Beam-20 is 29 bytes", beam.size() == 29,
           std::to_string(beam.size()));

    const QByteArray dummy = encodeSelect(QStringLiteral("AS-84F-4"), QStringLiteral("Dummy Load"));
    report("SET_SWITCH Dummy Load is 32 bytes", dummy.size() == 32,
           std::to_string(dummy.size()));

    const QByteArray off = encodeSelect(QStringLiteral("AS-84F-4"), QStringLiteral("OFF"));
    report("SET_SWITCH OFF is 25 bytes", off.size() == 25, std::to_string(off.size()));

    // Whatever we encode must be something the same framing code can take
    // apart again.
    QByteArray buffer = beam;
    const QVector<QByteArray> records = splitRecords(buffer);
    const Record round = parse(records.value(0));
    report("an encoded command round-trips through the parser",
           buffer.isEmpty() && records.size() == 1
               && round.fields == QStringList({"AS-84F-4", "Beam-20"}),
           round.fields.join(QLatin1Char(',')).toStdString());
}

void testEncodeRefusesFramingBytes()
{
    // Switch and port names reach us from the device's own roster, so echoing
    // them back unchecked would let a hostile or broken server inject extra
    // fields or extra records into our command (Principle VII).
    report("a port name with US is refused",
           encodeSelect(QStringLiteral("AS-84F-1"),
                        QStringLiteral("Beam\x1f" "20")).isEmpty());
    report("a port name with CRLF is refused",
           encodeSelect(QStringLiteral("AS-84F-1"),
                        QStringLiteral("Beam\r\nSET_SWITCH")).isEmpty());
    report("a switch name with GS is refused",
           encodeSelect(QStringLiteral("AS\x1d" "84F"), QStringLiteral("OFF")).isEmpty());
    report("an empty name is refused",
           encodeSelect(QString{}, QStringLiteral("OFF")).isEmpty());
}

// ── Display ordering ────────────────────────────────────────────────────────

void testDisplayOrdering()
{
    QStringList names = {"AS-84F-10", "Tornado", "AS-84F-2", "AS-84F-1"};
    std::sort(names.begin(), names.end(), displayOrderLessThan);
    report("numbered switches sort numerically, unnumbered last",
           names == QStringList({"AS-84F-1", "AS-84F-2", "AS-84F-10", "Tornado"}),
           names.join(QLatin1Char(',')).toStdString());
}

} // namespace

int main()
{
    testFixturesStillMatchTheWire();
    testOneSegmentCanHoldSeveralRecords();
    testPartialRecordIsHeldBackNotParsed();
    testRecordSplitAtEveryByteStillParses();
    testRecordsAccumulateAcrossReads();
    testParseSwitchUpdate();
    testParseSwitchLocks();
    testParseSwitchAddRoster();
    testUnknownVerbIsSurfacedNotFatal();
    testBoundaryCaps();
    testEncodeSelectMatchesTheWire();
    testEncodeRefusesFramingBytes();
    testDisplayOrdering();

    std::printf(g_failed ? "\n%d check(s) FAILED\n" : "\nAll checks passed\n", g_failed);
    return g_failed ? 1 : 0;
}
