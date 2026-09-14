// The producer->consumer join, checked as a join.
//
// MeterSurfaces.h is the one table that knows a meter's whole path, and
// RadioCertification.cpp's kMeterTable is the executable form of the
// certification inventory. They are two hand-maintained tables keyed by the
// same "SRC:NAME" string, and the failure mode when they disagree is recorded
// in both files: kMeterSurfaces was widened to a unit SET, kMeterTable was left
// holding the old single-value column, and `radiocert meters` then reported
// `UNIT MISMATCH ... TX:ALC` on every healthy HL2 run and ranked it above every
// real finding (CERTIFICATION.md 1.38). A concern that never goes away stops
// being read, and takes the real ones with it.
//
// Nothing in the build could notice that, because the two tables never meet at
// compile time: kMeterTable lives in an anonymous namespace inside a
// translation unit that pulls in RadioModel, AudioEngine and the whole
// certification apparatus. So this reads it as TEXT. That is a real limitation
// and worth naming — the check is structural, not semantic, and it proves a row
// EXISTS rather than that its columns are right. It is still the check that
// would have caught 1.38's successor, which is a meter added to one table and
// forgotten in the other.

#include "core/MeterSurfaces.h"

#include <QByteArray>
#include <QFile>
#include <QRegularExpression>
#include <QString>
#include <QStringList>

#include <cstdio>

using namespace AetherSDR;

namespace {

int g_failed = 0;

void check(const char* name, bool ok)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", name);
    if (!ok) {
        ++g_failed;
    }
}

QByteArray readSource(const char* relativePath)
{
    QFile f(QString::fromLatin1(AETHER_SOURCE_DIR) + QString::fromLatin1(relativePath));
    if (!f.open(QIODevice::ReadOnly)) {
        return {};
    }
    return f.readAll();
}

// A kMeterTable row is `{"SRC", "NAME", ...}` with the alignment padding the
// table uses for readability, so match on structure rather than on spacing.
// "+13.8A" is a real meter name and a regex metacharacter, hence the escape.
bool certificationTableHasRow(const QString& table, const QString& source,
                              const QString& name)
{
    const QRegularExpression row(
        QStringLiteral("\\{\\s*\"%1\"\\s*,\\s*\"%2\"\\s*,")
            .arg(QRegularExpression::escape(source),
                 QRegularExpression::escape(name)));
    return row.match(table).hasMatch();
}

// THE REGRESSION 1.38 LEFT BEHIND, in the general form. Every surface the UI
// declares must have a certification row, because the unit verdict is computed
// by joining the two on the key: a surface with no row is never checked at all,
// and a row with no surface "gets no unit verdict, which is the honest answer"
// (RadioCertification.cpp). Only this direction is an error.
void testEverySurfaceHasACertificationRow()
{
    const QByteArray cert = readSource("/src/core/RadioCertification.cpp");
    check("the join test can read RadioCertification.cpp", !cert.isEmpty());
    const QString table = QString::fromUtf8(cert);

    QStringList missing;
    for (const MeterSurface& s : kMeterSurfaces) {
        const QString key = QString::fromLatin1(s.key);
        const int colon = key.indexOf(QLatin1Char(':'));
        if (colon <= 0) {
            missing << key + QStringLiteral(" (malformed key)");
            continue;
        }
        if (!certificationTableHasRow(table, key.left(colon), key.mid(colon + 1))) {
            missing << key;
        }
    }
    if (!missing.isEmpty()) {
        std::printf("       missing from kMeterTable: %s\n",
                    qPrintable(missing.join(QStringLiteral(", "))));
    }
    check("every kMeterSurfaces key has a kMeterTable row", missing.isEmpty());
}

// TX:ALCGAIN — the gain the HL2's ALC is applying, as opposed to TX:ALC, which
// is the post-ALC LEVEL and "sits pinned near the target by definition"
// (Hl2TxDsp::processAudioBlock). Plain dB, with no second unit anywhere: unlike
// TX:ALC, no radio in the tree reports a gain as a percentage.
void testAlcGainSurfaceIsRegisteredInDb()
{
    const MeterSurface* s = meterSurfaceFor(QStringLiteral("TX:ALCGAIN"));
    check("TX:ALCGAIN is a registered meter surface", s != nullptr);
    if (!s) {
        return;
    }
    check("TX:ALCGAIN's consumer accepts dB",
          meterUnitAccepted(QString::fromLatin1(s->acceptedUnits),
                            QStringLiteral("dB")));
    // The 1.38 shape, pinned from the other side: a consumer that also accepted
    // dBFS would silently render a level as a gain rather than report the
    // disagreement.
    check("TX:ALCGAIN's consumer does NOT accept dBFS",
          !meterUnitAccepted(QString::fromLatin1(s->acceptedUnits),
                             QStringLiteral("dBFS")));
    check("TX:ALCGAIN remains producer-only until its GUI change lands",
          !meterHasRenderedSurface(QStringLiteral("TX:ALCGAIN")));
}

// Drop // line comments before searching, so prose cannot satisfy a wiring
// assertion. Without this, a Hl2Backend.cpp that merely MENTIONS ALCGAIN in a
// comment -- which this file's own neighbours now do -- passes the checks below
// with the emit deleted. Crude by design: it also blanks a // inside a string
// literal, which costs nothing here because neither needle contains one.
QByteArray withoutLineComments(const QByteArray& src)
{
    QByteArray out;
    out.reserve(src.size());
    for (const QByteArray& line : src.split('\n')) {
        const int c = line.indexOf("//");
        out += (c < 0 ? line : line.left(c));
        out += '\n';
    }
    return out;
}

// The producer half of the same join. A surface row is a claim about wiring
// that only the wiring can honour, and "defined but never fed" and "fed but
// never defined" are both invisible from the consumer side.
//
// STILL STRUCTURAL, AND STILL NOT SEMANTIC. These assertions prove the two
// call sites exist in code rather than in prose; they do not prove the value
// reaching the emit is the ALC's gain. meter_model_test covers the routing
// behaviourally from MeterModel inwards; the backend half has no seam to hold
// it against without linking the whole certification TU, which is the cost the
// file header records.
void testHl2PublishesAlcGain()
{
    const QByteArray backend =
        withoutLineComments(readSource("/src/core/backends/hl2/Hl2Backend.cpp"));
    check("the join test can read Hl2Backend.cpp", !backend.isEmpty());
    check("Hl2Backend defines a TX ALCGAIN meter",
          backend.contains("QStringLiteral(\"ALCGAIN\")"));
    check("Hl2Backend feeds TX:ALCGAIN from the alcGain signal",
          backend.contains("meterUpdate(QStringLiteral(\"TX:ALCGAIN\")"));
}

}  // namespace

int main()
{
    testEverySurfaceHasACertificationRow();
    testAlcGainSurfaceIsRegisteredInDb();
    testHl2PublishesAlcGain();
    return g_failed == 0 ? 0 : 1;
}
