// Unit tests for MemoryCsvCompat — the SmartSDR memory CSV round-trip and the
// CHIRP-next generic CSV import path (dialect detection, field mapping, and
// unit scaling). No radio, no Qt event loop required.

#include "core/MemoryCsvCompat.h"

#include <QByteArray>
#include <QFile>
#include <QString>

#include <cmath>
#include <iostream>

namespace {

int g_failures = 0;

bool expect(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++g_failures;
    }
    return condition;
}

bool nearly(double a, double b) { return std::fabs(a - b) < 1e-6; }

using AetherSDR::MemoryCsvCompat;
using AetherSDR::MemoryCsvFormat;
using AetherSDR::MemoryCsvParseResult;
using AetherSDR::MemoryCsvRecord;

// A representative CHIRP-next generic CSV export (the full 21-column header).
const char* kChirpCsv =
    "Location,Name,Frequency,Duplex,Offset,Tone,rToneFreq,cToneFreq,DtcsCode,"
    "DtcsPolarity,RxDtcsCode,CrossMode,Mode,TStep,Skip,Power,Comment,URCALL,"
    "RPT1CALL,RPT2CALL,DVCODE\r\n"
    "0,2m Calling,146.520000,,0.000000,,88.5,88.5,023,NN,023,Tone->Tone,FM,5.00,,5.0W,simplex,,,,\r\n"
    "1,W1AW Rptr,146.940000,-,0.600000,Tone,100.0,88.5,023,NN,023,Tone->Tone,FM,5.00,,50W,rptr,,,,\r\n"
    "2,GMRS 18,462.550000,+,5.000000,TSQL,141.3,131.8,023,NN,023,Tone->Tone,NFM,12.50,,50W,,,,,\r\n"
    "3,40m FT8,7.074000,,0.000000,,88.5,88.5,023,NN,023,Tone->Tone,USB,1.00,,,dig,,,,\r\n"
    "4,DStar RP,145.670000,,0.000000,,88.5,88.5,023,NN,023,Tone->Tone,DV,5.00,,,dv,,,,\r\n"
    "5,DTCS,446.000000,,0.000000,DTCS,88.5,88.5,051,NN,051,Tone->Tone,NFM,12.50,,,dcs,,,,\r\n";

void testChirpImport()
{
    const MemoryCsvParseResult r = MemoryCsvCompat::parse(QByteArray(kChirpCsv));

    expect(r.format == MemoryCsvFormat::Chirp, "CHIRP header is detected as CHIRP dialect");
    expect(MemoryCsvCompat::formatName(r.format) == "CHIRP", "formatName reports CHIRP");
    if (!expect(r.records.size() == 6, "all six CHIRP rows parse")) {
        std::cerr << "  got " << r.records.size() << " records\n";
        for (const QString& e : r.errors)
            std::cerr << "  error: " << e.toStdString() << '\n';
        return;
    }

    // Row 0 — simplex FM, no tone. Frequency stays MHz; TStep kHz -> Hz.
    const AetherSDR::MemoryEntry& m0 = r.records.at(0).memory;
    expect(m0.name == "2m Calling", "row0 name");
    expect(nearly(m0.freq, 146.52), "row0 frequency stays in MHz");
    expect(m0.mode == "FM", "row0 FM maps to FM");
    expect(m0.step == 5000, "row0 TStep 5.00 kHz scales to 5000 Hz");
    expect(m0.offsetDir == "simplex", "row0 blank duplex is simplex");
    expect(nearly(m0.repeaterOffset, 0.0), "row0 simplex offset is 0");
    expect(m0.toneMode == "off", "row0 no tone -> off");

    // Row 1 — negative repeater shift, TX CTCSS tone.
    const AetherSDR::MemoryEntry& m1 = r.records.at(1).memory;
    expect(m1.offsetDir == "down", "row1 '-' duplex maps to down");
    expect(nearly(m1.repeaterOffset, 0.6), "row1 offset 0.6 MHz preserved");
    expect(m1.toneMode == "ctcss_tx", "row1 Tone -> ctcss_tx");
    expect(nearly(m1.toneValue, 100.0), "row1 uses rToneFreq (TX tone)");

    // Row 2 — positive shift, TSQL uses the RX (cToneFreq) tone, NFM, 12.5 kHz.
    const AetherSDR::MemoryEntry& m2 = r.records.at(2).memory;
    expect(m2.offsetDir == "up", "row2 '+' duplex maps to up");
    expect(nearly(m2.repeaterOffset, 5.0), "row2 offset 5 MHz preserved");
    expect(m2.mode == "NFM", "row2 NFM maps to NFM");
    expect(m2.step == 12500, "row2 TStep 12.50 kHz scales to 12500 Hz");
    expect(m2.toneMode == "ctcss_tx", "row2 TSQL -> ctcss_tx");
    expect(nearly(m2.toneValue, 131.8), "row2 TSQL uses cToneFreq");

    // Row 3 — HF USB, sub-5kHz step.
    const AetherSDR::MemoryEntry& m3 = r.records.at(3).memory;
    expect(nearly(m3.freq, 7.074), "row3 HF frequency preserved");
    expect(m3.mode == "USB", "row3 USB maps to USB");
    expect(m3.step == 1000, "row3 TStep 1.00 kHz scales to 1000 Hz");

    // Row 4 — DV maps to the Flex D-STAR mode token.
    expect(r.records.at(4).memory.mode == "DSTR", "row4 DV maps to DSTR");

    // Row 5 — DTCS has no Flex equivalent and degrades to tone-off.
    expect(r.records.at(5).memory.toneMode == "off", "row5 DTCS degrades to off");
}

void testChirpColumnOrderIndependence()
{
    // A trimmed, reordered CHIRP header — only the columns we consume, in a
    // different order — must still map by name.
    const char* csv =
        "Location,Frequency,Mode,Name,Duplex,Offset,TStep\r\n"
        "7,442.100000,NFM,Reordered,+,5.000000,25.00\r\n";
    const MemoryCsvParseResult r = MemoryCsvCompat::parse(QByteArray(csv));
    expect(r.format == MemoryCsvFormat::Chirp, "reordered CHIRP header still detected");
    if (!expect(r.records.size() == 1, "reordered CHIRP row parses"))
        return;
    const AetherSDR::MemoryEntry& m = r.records.at(0).memory;
    expect(m.name == "Reordered", "name resolved by column name, not position");
    expect(nearly(m.freq, 442.1), "frequency resolved by name");
    expect(m.mode == "NFM", "mode resolved by name");
    expect(m.offsetDir == "up", "duplex resolved by name");
    expect(m.step == 25000, "TStep 25.00 kHz -> 25000 Hz");
}

void testSmartSdrStillWorks()
{
    // Round-trip: serialize a record, parse it back, confirm SmartSDR dialect.
    AetherSDR::MemoryEntry m;
    m.name = "Test";
    m.freq = 14.074;
    m.mode = "DIGU";
    m.step = 10;
    m.offsetDir = "simplex";
    m.toneMode = "off";
    MemoryCsvRecord rec;
    rec.memory = m;

    const QByteArray bytes = MemoryCsvCompat::serialize({rec});
    const MemoryCsvParseResult r = MemoryCsvCompat::parse(bytes);
    expect(r.format == MemoryCsvFormat::SmartSdr, "serialized CSV parses as SmartSDR dialect");
    expect(r.ok(), "SmartSDR round-trip has no errors");
    if (expect(r.records.size() == 1, "SmartSDR round-trip yields one record")) {
        expect(r.records.at(0).memory.name == "Test", "SmartSDR round-trip preserves name");
        expect(nearly(r.records.at(0).memory.freq, 14.074), "SmartSDR round-trip preserves freq");
    }
}

void testChirpMissingToneFreqFallsToOff()
{
    // Tone=Tone but no rToneFreq column at all (a trimmed export): the tone
    // frequency parse misses, and the import must fall back to tone-off, not
    // ctcss_tx with an invalid 0 Hz tone (#4129 review).
    const char* csv =
        "Location,Name,Frequency,Duplex,Offset,Mode,TStep,Tone\r\n"
        "0,NoToneCol,146.520000,,0.000000,FM,5.00,Tone\r\n";
    const MemoryCsvParseResult r = MemoryCsvCompat::parse(QByteArray(csv));
    if (!expect(r.records.size() == 1, "tone-column-less CHIRP row parses"))
        return;
    expect(r.records.at(0).memory.toneMode == "off",
           "missing rToneFreq degrades Tone=Tone to off, not ctcss_tx/0");

    // Same fallback for an out-of-range (sub-CTCSS) tone value.
    const char* csv2 =
        "Location,Name,Frequency,Duplex,Offset,Mode,TStep,Tone,rToneFreq\r\n"
        "0,ZeroTone,146.520000,,0.000000,FM,5.00,Tone,0.0\r\n";
    const MemoryCsvParseResult r2 = MemoryCsvCompat::parse(QByteArray(csv2));
    if (expect(r2.records.size() == 1, "zero-tone CHIRP row parses"))
        expect(r2.records.at(0).memory.toneMode == "off",
               "0 Hz tone value degrades to off");
}

#ifdef CHIRP_SAMPLE_CSV
void testSampleFixtureParses()
{
    // The in-repo sample (docs/automation/sample-chirp-memories.csv) is the
    // manual-import fixture; parsing it here keeps it from drifting from the
    // importer (#4129 review — it was orphaned when the csv bridge verb was
    // removed for the engine-boundary violation).
    QFile f(QStringLiteral(CHIRP_SAMPLE_CSV));
    if (!expect(f.open(QIODevice::ReadOnly), "sample fixture opens"))
        return;
    const MemoryCsvParseResult r = MemoryCsvCompat::parse(f.readAll());
    expect(r.format == MemoryCsvFormat::Chirp, "sample fixture detected as CHIRP");
    expect(r.ok(), "sample fixture parses without errors");
    expect(r.records.size() == 6, "sample fixture yields six records");
}
#endif

void testUnknownHeaderRejected()
{
    const char* csv = "Foo,Bar,Baz\r\n1,2,3\r\n";
    const MemoryCsvParseResult r = MemoryCsvCompat::parse(QByteArray(csv));
    expect(!r.ok(), "an unrecognized header is rejected");
    expect(r.format == MemoryCsvFormat::Unknown, "unrecognized header yields Unknown format");
}

} // namespace

int main()
{
    testChirpImport();
    testChirpColumnOrderIndependence();
    testChirpMissingToneFreqFallsToOff();
#ifdef CHIRP_SAMPLE_CSV
    testSampleFixtureParses();
#endif
    testSmartSdrStillWorks();
    testUnknownHeaderRejected();

    if (g_failures == 0)
        std::cout << "memory_csv_compat_test: all assertions passed\n";
    return g_failures == 0 ? 0 : 1;
}
