// In-app links to docs.aethersdr.com (src/gui/DocsLinks.h).
//
// Checks the Help menu's documentation block (labels, order, menu role, the
// URL each item opens), that every docs page the app links to exists in
// docs/user/, and that every bundled resources/help/*.md page carries one
// "Full documentation:" line pointing at a real docs page.
//
// No browser opens: QDesktopServices::setUrlHandler() captures https URLs.
// Build: CMake target `docs_links_test`. Exit 0 = pass.

#include "gui/DocsLinks.h"

#include <QAction>
#include <QApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QMenu>
#include <QRegularExpression>
#include <QStringList>
#include <QUrl>
#include <cstdio>
#include <string>

using namespace AetherSDR;

namespace {

int g_failed = 0;

void report(const char* name, bool ok, const QString& detail = {})
{
    std::printf("%s %-60s %s\n", ok ? "[ OK ]" : "[FAIL]", name,
                detail.toStdString().c_str());
    if (!ok)
        ++g_failed;
}

class UrlCatcher : public QObject {
    Q_OBJECT
public:
    QList<QUrl> urls;
public slots:
    void handle(const QUrl& url) { urls.append(url); }
};

const QString kSource = QStringLiteral(AETHER_SOURCE_DIR);

QString readText(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    return QString::fromUtf8(f.readAll());
}

// The docs page a site path resolves to: "/" is the docs home (index.md); a
// docs page is docs/user/docs/<slug>.md whose front matter declares that slug;
// /log-analyzer is a site page under docs/user/src/pages.
bool docsPageExists(const QString& path, QString* where)
{
    const QString docsDir = kSource + QStringLiteral("/docs/user/docs/");
    if (path == QLatin1String("/")) {
        *where = docsDir + QStringLiteral("index.md");
        return QFile::exists(*where);
    }
    const QString slug = path.mid(1);
    const QString page = docsDir + slug + QStringLiteral(".md");
    if (QFile::exists(page)) {
        *where = page;
        return readText(page).contains(
            QStringLiteral("slug: \"/%1\"").arg(slug));
    }
    const QString sitePage = kSource + QStringLiteral("/docs/user/src/pages/")
                             + slug + QStringLiteral(".js");
    *where = sitePage;
    return QFile::exists(sitePage);
}

void testUrls()
{
    report("base URL", DocsLinks::kBaseUrl == QLatin1String("https://docs.aethersdr.com"));
    report("home", DocsLinks::home() == QLatin1String("https://docs.aethersdr.com/"),
           DocsLinks::home());
    report("printable manual",
           DocsLinks::manualPdf() == QLatin1String("https://docs.aethersdr.com/AetherSDR-Manual.pdf"),
           DocsLinks::manualPdf());
    report("log analyzer",
           DocsLinks::logAnalyzer() == QLatin1String("https://docs.aethersdr.com/log-analyzer"),
           DocsLinks::logAnalyzer());
    report("troubleshooting",
           DocsLinks::troubleshooting() == QLatin1String("https://docs.aethersdr.com/troubleshooting"),
           DocsLinks::troubleshooting());
    report("automation bridge",
           DocsLinks::automationBridge()
               == QLatin1String("https://docs.aethersdr.com/automation-bridge-and-mcp"),
           DocsLinks::automationBridge());

    for (const QString& url : {DocsLinks::home(), DocsLinks::logAnalyzer(),
                               DocsLinks::troubleshooting(),
                               DocsLinks::automationBridge()}) {
        QString where;
        const bool ok = docsPageExists(QUrl(url).path(), &where);
        report(qPrintable(QStringLiteral("docs page exists for ") + QUrl(url).path()),
               ok, where.mid(kSource.size() + 1));
    }
}

void testHelpMenuActions(UrlCatcher& catcher)
{
    QMenu menu;
    DocsLinks::addHelpMenuActions(&menu);
    const QList<QAction*> actions = menu.actions();
    report("help menu gets exactly three docs actions", actions.size() == 3,
           QString::number(actions.size()));
    if (actions.size() != 3)
        return;

    const QStringList expectedText = {
        QStringLiteral("AetherSDR Documentation"),
        QStringLiteral("Printable Manual (PDF)"),
        QStringLiteral("Log Analyzer"),
    };
    const QStringList expectedUrl = {
        DocsLinks::home(), DocsLinks::manualPdf(), DocsLinks::logAnalyzer(),
    };
    for (int i = 0; i < 3; ++i) {
        QAction* a = actions.at(i);
        report(qPrintable(QStringLiteral("action %1 label").arg(i + 1)),
               a->text() == expectedText.at(i), a->text());
        report(qPrintable(QStringLiteral("action %1 has no ellipsis (opens a browser)").arg(i + 1)),
               !a->text().endsWith(QLatin1String("...")));
        report(qPrintable(QStringLiteral("action %1 menu role is NoRole (#883)").arg(i + 1)),
               a->menuRole() == QAction::NoRole);

        catcher.urls.clear();
        a->trigger();
        const bool ok = catcher.urls.size() == 1
                        && catcher.urls.first() == QUrl(expectedUrl.at(i));
        report(qPrintable(QStringLiteral("action %1 opens its docs URL").arg(i + 1)), ok,
               catcher.urls.isEmpty() ? QStringLiteral("(nothing opened)")
                                      : catcher.urls.first().toString());
    }
}

void testBundledHelpPages()
{
    const QDir helpDir(kSource + QStringLiteral("/resources/help"));
    const QStringList pages = helpDir.entryList({QStringLiteral("*.md")}, QDir::Files);
    report("bundled help pages found", !pages.isEmpty(), QString::number(pages.size()));

    static const QRegularExpression kLine(
        QStringLiteral("^Full documentation: \\[[^\\]]+\\]\\((https://docs\\.aethersdr\\.com(/[^)]*))\\)$"),
        QRegularExpression::MultilineOption);
    for (const QString& page : pages) {
        const QString text = readText(helpDir.filePath(page));
        // Near the top: within the first five lines, under the H1.
        const QString head = text.section(QLatin1Char('\n'), 0, 4);
        const QRegularExpressionMatch m = kLine.match(head);
        const int count = int(text.count(QStringLiteral("Full documentation:")));
        report(qPrintable(page + QStringLiteral(": one docs line near the top")),
               m.hasMatch() && count == 1);
        if (!m.hasMatch())
            continue;
        QString where;
        report(qPrintable(page + QStringLiteral(": links a real docs page")),
               docsPageExists(m.captured(2), &where), m.captured(1));
    }
}

} // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    std::printf("DocsLinks test harness\n\n");

    UrlCatcher catcher;
    QDesktopServices::setUrlHandler(QStringLiteral("https"), &catcher, "handle");

    testUrls();
    testHelpMenuActions(catcher);
    testBundledHelpPages();

    QDesktopServices::unsetUrlHandler(QStringLiteral("https"));

    std::printf("\n%s\n", g_failed == 0
                              ? "All tests passed."
                              : (std::to_string(g_failed) + " test(s) failed.").c_str());
    return g_failed == 0 ? 0 : 1;
}

#include "docs_links_test.moc"
