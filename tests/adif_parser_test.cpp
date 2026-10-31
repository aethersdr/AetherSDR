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

    // ADIF IV.A.1 permits an optional type indicator after the length.
    {
        const auto recs = parse(dir,
            "<CALL:6:S>WN4AZY<BAND:3:E>20M<MODE:2:E>CW<EOR>\n"
            "<call:6:s>WN4AZY<freq:6:n>14.074<mode:4:e>MFSK<submode:3:s>FT8<eor>\n");
        check(recs.size() == 2, "typed log: both records parse");
        check(recs.size() == 2 && recs[0].callsign == "WN4AZY"
                  && recs[0].band == "20m" && recs[0].modeGroup == "CW",
              "typed CALL, BAND and MODE retain their values");
        check(recs.size() == 2 && recs[1].callsign == "WN4AZY"
                  && recs[1].band == "20m" && recs[1].modeGroup == "DATA",
              "lowercase typed FREQ and SUBMODE retain their values");
        const auto submode = parse(dir,
            "<CALL:6>WN4AZY<BAND:3>20m<MODE:3>SSB<SUBMODE:3:S>FT8<EOR>");
        check(submode.size() == 1 && submode[0].modeGroup == "DATA",
              "typed SUBMODE takes precedence over MODE");
        const auto over = parse(dir, "<CALL:2147483647:S>WN4AZY<EOR>");
        check(over.isEmpty(), "typed CALL length INT_MAX: record rejected");
        const auto exact = parse(dir, "<CALL:6:S>WN4AZY<EOR>");
        check(exact.size() == 1 && exact[0].callsign == "WN4AZY",
              "typed length exactly fills the record: accepted");
        const auto shortData = parse(dir, "<CALL:7:S>WN4AZY<EOR>");
        check(shortData.isEmpty(), "typed length one past the record: rejected");
        const auto malformed = parse(dir, "<CALL:6:S:6>WN4AZY<EOR>");
        check(malformed.isEmpty(), "extra length after type: rejected");
    }

    // ADIF IV.A.1: every field's declared length includes literal tag text,
    // even for fields the worked-status importer does not use.
    {
        const QByteArray contact = "<CALL:6>WN4AZY<BAND:3>20M<MODE:2>CW<EOR>";
        const auto embeddedCall = parse(dir,
            "<COMMENT:18><CALL:6>VK9XYZ hi!" + contact);
        check(embeddedCall.size() == 1 && embeddedCall[0].callsign == "WN4AZY",
              "CALL text inside COMMENT does not replace the real callsign");
        const auto embeddedFields = parse(dir,
            "<APP_LOGGER_NOTE:26:S><BAND:3>40M<MODE:3>SSB hi!" + contact);
        check(embeddedFields.size() == 1 && embeddedFields[0].band == "20m"
                  && embeddedFields[0].modeGroup == "CW",
              "typed application field hides embedded BAND and MODE tags");
        for (const QByteArray delimiter : {QByteArray("<EOR>"), QByteArray("<EOH>"),
                                           QByteArray("<eor>"), QByteArray("<eoh>")}) {
            const auto recs = parse(dir, "<CALL:6>WN4AZY<COMMENT:5>" + delimiter
                + "<BAND:3>20M<MODE:2>CW<EOR>");
            check(recs.size() == 1 && recs[0].callsign == "WN4AZY"
                      && recs[0].band == "20m" && recs[0].modeGroup == "CW",
                  "delimiter text inside COMMENT does not end the record or header");
        }
        const auto header = parse(dir,
            "Generated log\n<PROGRAMID:10><EOH><EOR><CALL:6>HEADER<EOH>" + contact);
        check(header.size() == 1 && header[0].callsign == "WN4AZY",
              "header fields consume embedded delimiters before the real EOH");
        const auto records = parse(dir,
            "<COMMENT:5><EOR>" + contact
            + "<COMMENT:5><EOH><CALL:5>K1ABC<BAND:3>40m<MODE:3>SSB<eor>");
        check(records.size() == 2 && records[0].callsign == "WN4AZY"
                  && records[0].modeGroup == "CW" && records[1].callsign == "K1ABC"
                  && records[1].band == "40m" && records[1].modeGroup == "PHONE",
              "real EOR boundaries preserve multiple records around embedded tags");
        const auto zero = parse(dir, "<COMMENT:0>" + contact);
        check(zero.size() == 1 && zero[0].callsign == "WN4AZY",
              "zero-length ignored field does not consume the next field");
        const auto truncated = parse(dir, "<COMMENT:2147483647>" + contact);
        check(truncated.size() == 1 && truncated[0].callsign == "WN4AZY",
              "oversized ignored field preserves malformed-length recovery");
        const auto overflow = parse(dir,
            "<CALL:6>WN4AZY<BAND:9999999999999999999999>20m<MODE:2>CW<EOR>");
        check(overflow.size() == 1 && overflow[0].band.isEmpty()
                  && overflow[0].modeGroup == "CW",
              "unrepresentable length reads as absent without hiding later fields");
        const auto duplicate = parse(dir,
            "<CALL:6>WN4AZY<CALL:5>K1ABC<BAND:3>20m<BAND:3>40m<MODE:2>CW<EOR>");
        check(duplicate.size() == 1 && duplicate[0].callsign == "WN4AZY"
                  && duplicate[0].band == "20m",
              "duplicate fields keep their first value");
        const auto duplicateInvalid = parse(dir,
            "<CALL:2147483647>bad<CALL:6>WN4AZY<EOR>");
        check(duplicateInvalid.isEmpty(), "invalid first CALL is not replaced by a duplicate");
        const auto missingEnd = parse(dir, "<CALL:6>WN4AZY<COMMENT:5><EOR>");
        check(missingEnd.isEmpty(), "a record needs a real EOR outside field data");
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
