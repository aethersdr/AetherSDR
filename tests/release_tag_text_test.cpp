// The two prompts that show a GitHub release tag (#6405): the "Outdated
// Version" body, which is rich text, and the "Update Available" box, which is
// plain text. No network, no sockets: the tag is passed in.

#include "core/VersionNumber.h"
#include "gui/ReleaseTagText.h"

#include <QApplication>
#include <QLabel>
#include <QMessageBox>
#include <QString>
#include <QTextDocument>

#include <cstdio>

using AetherSDR::VersionNumber;
using AetherSDR::outdatedVersionPromptHtml;
using AetherSDR::setUpdateAvailableText;

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

QString formatName(Qt::TextFormat format)
{
    return QStringLiteral("textFormat=%1").arg(static_cast<int>(format));
}

void testUpdateAvailableBox()
{
    QMessageBox box;
    setUpdateAvailableText(box, kCurrent, kCraftedTag);

    // Control: left to Qt::AutoText, this text would be rendered as markup.
    report("update: the crafted text would be rich under AutoText",
           Qt::mightBeRichText(box.text()), box.text());
    report("update: the box is plain text", box.textFormat() == Qt::PlainText,
           formatName(box.textFormat()));
    const QLabel* label = box.findChild<QLabel*>(QStringLiteral("qt_msgbox_label"));
    report("update: the label that draws the text is plain text",
           label && label->textFormat() == Qt::PlainText,
           label ? formatName(label->textFormat()) : QStringLiteral("no qt_msgbox_label"));
    report("update: the tag is shown verbatim",
           box.text() == QStringLiteral("AetherSDR v%1 is available.").arg(kCraftedTag),
           box.text());
    report("update: the running version is the informative text",
           box.informativeText() == QStringLiteral("You are running v26.10.2."),
           box.informativeText());

    QMessageBox plainBox;
    setUpdateAvailableText(plainBox, kCurrent, kPlainTag);
    report("update: a plain tag is shown unchanged",
           plainBox.text() == QStringLiteral("AetherSDR v26.10.3 is available."),
           plainBox.text());
}

// Why the box is plain text instead of escaped: an escaped tag without "&lt;"
// is not detected as rich text, so its entities would be shown literally.
void testEscapingIsNotEnoughUnderAutoText()
{
    const QString escaped = QStringLiteral("AetherSDR v%1 is available.")
        .arg(QStringLiteral("99.0.0&x").toHtmlEscaped());
    report("update: an escaped '&' tag stays plain under AutoText",
           escaped.contains(QStringLiteral("&amp;")) && !Qt::mightBeRichText(escaped),
           escaped);
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
    testUpdateAvailableBox();
    testEscapingIsNotEnoughUnderAutoText();

    std::printf("\n%s\n", g_failed == 0 ? "ALL PASS" : "FAILURES PRESENT");
    return g_failed == 0 ? 0 : 1;
}
