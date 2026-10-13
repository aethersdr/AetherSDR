// AdifParser field-length bounds (#4411). A <FIELD:len> token is untrusted
// input: an oversized length must read as an absent field, never overflow the
// bounds check or pull in the rest of the record.
//
// Driven through parseFile() with real files; the assertions are on observable
// records, so they hold in an ordinary build as well as under UBSan.

#include "core/AdifParser.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>

#include <cstdio>

using namespace AetherSDR;

namespace {

int failures = 0;

void check(bool condition, const char* name)
{
    if (condition) {
        std::printf("[PASS] %s\n", name);
    } else {
        std::printf("[FAIL] %s\n", name);
        ++failures;
    }
}

QVector<QsoRecord> parse(const QTemporaryDir& dir, const QByteArray& adif)
{
    const QString path = dir.filePath("log.adi");
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        std::printf("[FAIL] could not write %s\n", qPrintable(path));
        ++failures;
        return {};
    }
    f.write(adif);
    f.close();
    return AdifParser::parseFile(path);
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir dir;
    if (!dir.isValid()) {
        std::printf("[FAIL] could not create a temporary directory\n");
        return 1;
    }

    // ── Baseline: a normal export still parses ──────────────────────────
    {
        const auto recs = parse(dir,
            "<EOH>\n"
            "<CALL:5>G3ABC<BAND:3>20m<MODE:2>CW<EOR>\n"
            "<CALL:5>K1ABC<BAND:3>40m<MODE:3>SSB<EOR>\n");
        check(recs.size() == 2, "normal log: both records parse");
        check(recs.size() == 2 && recs[0].callsign == "G3ABC"
                  && recs[0].band == "20m" && recs[0].modeGroup == "CW",
              "normal log: first record fields intact");
        check(recs.size() == 2 && recs[1].callsign == "K1ABC"
                  && recs[1].band == "40m" && recs[1].modeGroup == "PHONE",
              "normal log: second record fields intact");
    }

    // ── A length at INT_MAX ─────────────────────────────────────────────
    {
        const auto recs = parse(dir,
            "<EOH>\n"
            "<CALL:2147483647>G3ABC<BAND:3>20m<MODE:2>CW<EOR>\n");
        check(recs.isEmpty(), "CALL length INT_MAX: record rejected");
    }

    // The same length on a non-CALL field must not poison the whole record:
    // the call survives, the oversized field reads as absent.
    {
        const auto recs = parse(dir,
            "<EOH>\n"
            "<CALL:5>G3ABC<BAND:2147483647>20m<MODE:2>CW<EOR>\n");
        check(recs.size() == 1, "BAND length INT_MAX: record kept");
        check(recs.size() == 1 && recs[0].callsign == "G3ABC",
              "BAND length INT_MAX: callsign intact");
        check(recs.size() == 1 && recs[0].band.isEmpty(),
              "BAND length INT_MAX: oversized field reads as absent");
        check(recs.size() == 1 && recs[0].modeGroup == "CW",
              "BAND length INT_MAX: later fields unaffected");
    }

    // Near-INT_MAX values that still fit in an int but overflow once the
    // start offset is added, not just the literal maximum.
    {
        const auto recs = parse(dir,
            "<EOH>\n"
            "<CALL:2147483640>G3ABC<EOR>\n");
        check(recs.isEmpty(), "CALL length INT_MAX-7: record rejected");
    }

    // ── Boundary: the remaining-space comparison is not off by one ──────
    // The field is the last thing in its record, so len == remaining is the
    // largest legal value and len == remaining + 1 the smallest illegal one.
    {
        const auto exact = parse(dir, "<EOH>\n<CALL:5>G3ABC<EOR>\n");
        check(exact.size() == 1 && exact[0].callsign == "G3ABC",
              "length exactly fills the record: accepted");

        const auto over = parse(dir, "<EOH>\n<CALL:6>G3ABC<EOR>\n");
        check(over.isEmpty(), "length one past the record: rejected");
    }

    // ── Zero-length field reads as absent, not as a crash ───────────────
    {
        const auto recs = parse(dir, "<EOH>\n<CALL:0><BAND:3>20m<EOR>\n");
        check(recs.isEmpty(), "zero-length CALL: record skipped");
    }

    if (failures) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("All adif_parser_test checks passed\n");
    return 0;
}
