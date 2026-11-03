// The "Outdated Version" prompt shows a GitHub release tag in rich text
// (#6405); the tag is escaped. No network, no sockets: the tag is passed in.

#include "core/VersionNumber.h"
#include "gui/ReleaseTagText.h"

#include <QApplication>
#include <QString>
#include <QTextDocument>

#include <cstdio>

using AetherSDR::VersionNumber;
using AetherSDR::outdatedVersionPromptHtml;

namespace {

int g_failed = 0;

void report(const char* name, bool ok, const QString& detail = {})
{
    std::printf("%s %-66s %s\n", ok ? "[ OK ]" : "[FAIL]", name,
                ok ? "" : qPrintable(detail));
    if (!ok) {
        ++g_failed;
    }
}

const QString kCurrent = QStringLiteral("26.10.2");
const QString kPlainTag = QStringLiteral("26.10.3");
const QString kCraftedTag =
    QStringLiteral("99.0.0<a href=\"http://example.invalid\">x</a>");

// The newer-version test is not the protection: it passes a tag with markup.
void testGateAcceptsMarkup()
{
    const VersionNumber current = VersionNumber::parse(kCurrent);
    for (const QString& tag : {QStringLiteral("99.0.0<b>x"), kCraftedTag}) {
        const VersionNumber latest = VersionNumber::parse(tag);
        report("gate: a tag with markup parses as a version", !latest.isNull(), tag);
        report("gate: a tag with markup counts as newer", current < latest, tag);
    }
}

void testOutdatedVersionBody()
{
    const QString crafted = outdatedVersionPromptHtml(kCurrent, kCraftedTag);
    report("outdated: the tag's markup does not reach the body",
           !crafted.contains(QStringLiteral("<a")), crafted);
    report("outdated: the tag is shown escaped",
           crafted.contains(QStringLiteral(
               "<b>v99.0.0&lt;a href=&quot;http://example.invalid&quot;&gt;x&lt;/a&gt;</b>")),
           crafted);
    report("outdated: the body keeps its own markup",
           crafted.startsWith(QStringLiteral("<p>You are running <b>v26.10.2</b> but <b>v"))
               && crafted.endsWith(QStringLiteral("<p>Continue anyway?</p>")),
           crafted);
    report("outdated: the body is rich text", Qt::mightBeRichText(crafted), crafted);

    const QString markedCurrent =
        outdatedVersionPromptHtml(QStringLiteral("1.0<i>x</i>"), kPlainTag);
    report("outdated: the running version is escaped too",
           markedCurrent.contains(QStringLiteral("<b>v1.0&lt;i&gt;x&lt;/i&gt;</b>"))
               && !markedCurrent.contains(QStringLiteral("<i>")),
           markedCurrent);

    const QString plain = outdatedVersionPromptHtml(kCurrent, kPlainTag);
    report("outdated: a plain tag is shown unchanged",
           plain.contains(QStringLiteral("but <b>v26.10.3</b> is available.")), plain);
}

} // namespace

int main(int argc, char** argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QApplication app(argc, argv);

    testGateAcceptsMarkup();
    testOutdatedVersionBody();

    std::printf("\n%s\n", g_failed == 0 ? "ALL PASS" : "FAILURES PRESENT");
    return g_failed == 0 ? 0 : 1;
}
