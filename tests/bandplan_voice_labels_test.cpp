// Band-plan segment labels are load-bearing, not display text.
//
// MainWindow builds the S-History / QRM voice-scan mask by running every
// segment label of the active plan through isVoiceSegmentLabel(), a substring
// match on PHONE / SSB / USB / AM / FM/RPT (VoiceSignalDetector.cpp), and
// detectVoiceSignals() then masks bins to those ranges whenever the resulting
// vector is non-empty. A segment whose label carries none of those tokens is
// silently excluded from voice detection — no warning, no visible error, just
// markers that never appear on that spectrum.
//
// #4723 hit exactly this: the new 5351.5-5366.5 kHz segment was first labelled
// "WRC-15 9.15W", which matches nothing, so all 15 kHz dropped out of voice
// detection — including 5.3570-5.3598, which had been scanned before because
// the channel it replaced was labelled "USB".
//
// 60m is the case worth pinning because its labels have to carry a power limit
// as well as an emission list, which is what tempted the regulatory text into
// the label in the first place. Every US 60m allocation permits any emission
// within 2.8 kHz, so every 60m segment in the US plan must stay recognisable
// as voice.
//
// Reads the shipped resource, not a fixture: the invariant is about the file
// that actually ships.

#include "core/VoiceSignalDetector.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

#include <cstdio>

using namespace AetherSDR;

namespace {

int g_failed = 0;
int g_total = 0;

void report(const QString& label, bool ok)
{
    ++g_total;
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", qPrintable(label));
    if (!ok)
        ++g_failed;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    QFile f(QStringLiteral(":/bandplans/arrl-us.json"));
    if (!f.open(QIODevice::ReadOnly)) {
        std::printf("[FAIL] cannot open :/bandplans/arrl-us.json\n");
        return 1;
    }
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        std::printf("[FAIL] arrl-us.json does not parse: %s\n",
                    qPrintable(err.errorString()));
        return 1;
    }

    const QJsonArray segments = doc.object().value(QStringLiteral("segments")).toArray();
    report(QStringLiteral("arrl-us.json has segments"), !segments.isEmpty());

    // Every segment in the 5 MHz band. The US 60m allocation is any emission
    // within 2.8 kHz on both the four 100 W ERP channels and the 9.15 W ERP
    // WRC-15 segment, so all of them are voice spectrum.
    int found = 0;
    for (const auto& v : segments) {
        const QJsonObject seg = v.toObject();
        const double low = seg.value(QStringLiteral("low")).toDouble();
        if (low < 5.0 || low > 5.5)
            continue;
        ++found;
        const QString label = seg.value(QStringLiteral("label")).toString();
        report(QStringLiteral("60m segment at %1 is voice-detected (label \"%2\")")
                   .arg(low, 0, 'f', 4).arg(label),
               isVoiceSegmentLabel(label));
    }

    // If a future edit restructures 60m, this test must not quietly pass by
    // finding nothing to check.
    report(QStringLiteral("found all five 60m segments (got %1)").arg(found),
           found == 5);

    // The tokens themselves, so a change to isVoiceSegmentLabel() that drops
    // one is caught here rather than by the data assertions above alone.
    report(QStringLiteral("\"CW/USB/DATA 9.15W\" is recognised"),
           isVoiceSegmentLabel(QStringLiteral("CW/USB/DATA 9.15W")));
    report(QStringLiteral("\"WRC-15 9.15W\" is NOT recognised (the #4723 defect)"),
           !isVoiceSegmentLabel(QStringLiteral("WRC-15 9.15W")));

    if (g_failed == 0) {
        std::printf("\nAll %d band-plan voice-label tests passed.\n", g_total);
        return 0;
    }
    std::printf("\n%d of %d band-plan voice-label tests failed.\n", g_failed, g_total);
    return 1;
}
