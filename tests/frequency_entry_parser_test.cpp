#include "gui/FrequencyEntryParser.h"

#include <cstdio>
#include <string>

using namespace AetherSDR;

namespace {

int g_failed = 0;

void report(const char* name, bool ok, const std::string& detail = {})
{
    std::printf("%s %-72s %s\n",
                ok ? "[ OK ]" : "[FAIL]",
                name,
                detail.c_str());
    if (!ok) ++g_failed;
}

void expectExplicitMhz(const char* label, const QString& text, bool expected)
{
    const QString clean = FrequencyEntryParser::normalizedMhzText(text);
    const bool actual = FrequencyEntryParser::isExplicitMhzEntry(text, clean);
    report(label,
           actual == expected,
           QStringLiteral("text=%1 clean=%2 actual=%3")
               .arg(text, clean)
               .arg(actual ? QStringLiteral("true") : QStringLiteral("false"))
               .toStdString());
}

void expectNormalized(const char* label, const QString& text, const QString& expected)
{
    const QString actual = FrequencyEntryParser::normalizedMhzText(text);
    report(label,
           actual == expected,
           QStringLiteral("text=%1 actual=%2 expected=%3")
               .arg(text, actual, expected)
               .toStdString());
}

} // namespace

int main()
{
    expectNormalized("display-style entry keeps first MHz dot",
                     QStringLiteral("440.100.000"),
                     QStringLiteral("440.100000"));
    expectExplicitMhz("14.225.000 is explicit MHz", QStringLiteral("14.225.000"), true);
    expectExplicitMhz("144.200.000 is explicit MHz", QStringLiteral("144.200.000"), true);
    expectExplicitMhz("440.100.000 is explicit MHz", QStringLiteral("440.100.000"), true);
    expectExplicitMhz("14.225 is explicit MHz", QStringLiteral("14.225"), true);
    expectExplicitMhz("144.200 is explicit MHz", QStringLiteral("144.200"), true);
    expectExplicitMhz("440.100 is explicit MHz", QStringLiteral("440.100"), true);
    expectExplicitMhz("14225.0 keeps legacy kHz-style parse",
                      QStringLiteral("14225.0"), false);
    expectExplicitMhz("14225 has no explicit MHz marker",
                      QStringLiteral("14225"), false);

    return g_failed == 0 ? 0 : 1;
}
