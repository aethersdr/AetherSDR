#include "WhatsNewDialog.h"
#include "CanonIndicators.h"
#include "CanonScrollBarStyle.h"
#include "FramelessResizer.h"
#include "ModemChrome.h"
#include "ScopedChildWidget.h"
#include "WhatsNewLinks.h"
#include "core/UpdateChecker.h"
#include "core/VersionNumber.h"
#include "core/AppSettings.h"

#include <QDateTime>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QList>
#include <QPair>
#include <QPointer>
#include <QPushButton>
#include <QScrollBar>
#include <QRegularExpression>
#include <QTextBrowser>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextFragment>
#include <QTextBlockFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextList>
#include <QUrl>
#include <QVBoxLayout>
#include "core/ThemeManager.h"

namespace AetherSDR {
namespace {

constexpr auto kReleaseApiBase =
    "https://api.github.com/repos/aethersdr/AetherSDR/releases/tags/%1";

QString normalizedTag(const QString& version)
{
    QString tag = version.trimmed();
    if (!tag.startsWith('v', Qt::CaseInsensitive))
        tag.prepend('v');
    return tag;
}

QString releaseApiUrl(const QString& tagName)
{
    return QString(kReleaseApiBase).arg(QString::fromLatin1(QUrl::toPercentEncoding(tagName)));
}

QString formattedPublishedAt(const QString& publishedAt)
{
    if (publishedAt.isEmpty())
        return {};

    QDateTime dateTime = QDateTime::fromString(publishedAt, Qt::ISODate);
    if (!dateTime.isValid())
        return publishedAt;

    return QLocale().toString(dateTime.toLocalTime(), QLocale::LongFormat);
}

QString stripDuplicateHeading(QString markdown, const QString& title, const QString& tagName)
{
    markdown = markdown.trimmed();
    if (!markdown.startsWith("# "))
        return markdown;

    const qsizetype newline = markdown.indexOf('\n');
    const QString firstLine = (newline >= 0 ? markdown.left(newline) : markdown).trimmed();
    const QString heading = firstLine.mid(2).trimmed();

    const bool matchesTitle = !title.isEmpty()
        && heading.compare(title, Qt::CaseInsensitive) == 0;
    const bool matchesTag = !tagName.isEmpty()
        && heading.contains(tagName, Qt::CaseInsensitive);
    if (!matchesTitle && !matchesTag)
        return markdown;

    return newline >= 0 ? markdown.mid(newline + 1).trimmed() : QString();
}

QString enrichGitHubReferences(QString markdown)
{
    markdown.replace("\r\n", "\n");
    markdown.replace('\r', '\n');
    markdown.replace(
        QRegularExpression(QStringLiteral(R"((?<![\w\]/\[])#(\d+))")),
        QStringLiteral("[#\\1](https://github.com/aethersdr/AetherSDR/issues/\\1)"));
    markdown.replace(
        QRegularExpression(QStringLiteral(R"((?<![\w\]/\[])@([A-Za-z0-9](?:[A-Za-z0-9-]{0,38}[A-Za-z0-9])?))")),
        QStringLiteral("[@\\1](https://github.com/\\1)"));
    return markdown;
}

QString releaseErrorText(QNetworkReply* reply, const QByteArray& payload)
{
    const QVariant status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    QString message;

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error == QJsonParseError::NoError && doc.isObject())
        message = doc.object().value("message").toString();

    if (message.isEmpty())
        message = reply->errorString();

    // GitHub rate-limit (60/hr unauthenticated, per IP) is a confusing
    // outcome to surface as a raw HTTP 403 — busy networks and shared NAT
    // gateways trip it for users who have done nothing wrong.  Detect the
    // rate-limit case (status 403 + "rate limit" substring in the GitHub
    // error message) and rewrite to a friendlier line that points at the
    // direct release URL as the escape hatch.
    const int statusCode = status.isValid() ? status.toInt() : 0;
    if (statusCode == 403 && message.contains("rate limit", Qt::CaseInsensitive)) {
        return QStringLiteral(
            "GitHub is rate-limiting requests from your network — try again "
            "in a few minutes.  You can also read the release notes directly "
            "at github.com/aethersdr/AetherSDR/releases.");
    }

    if (status.isValid())
        return QString("GitHub returned HTTP %1 (%2).").arg(statusCode).arg(message);

    return message;
}

// One canon token as a CSS colour, resolved in `scope`'s theme container. The
// release notes are rich text, which a widget sheet does not reach, so their
// colours are resolved here and rebuilt on every theme change.
QString tokenColour(const QWidget* scope, const char* token)
{
    return AetherSDR::ThemeManager::instance()
        .color(scope, QString::fromLatin1(token)).name(QColor::HexRgb);
}

QString releaseMarkdownStyleSheet(const QWidget* scope)
{
    const QString ink = tokenColour(scope, "color.canon.ink");
    const QString inkSoft = tokenColour(scope, "color.canon.inkSoft");
    const QString cyan = tokenColour(scope, "color.canon.cyan");
    const QString aqua = tokenColour(scope, "color.canon.aqua");
    const QString raised = tokenColour(scope, "color.canon.raised");
    const QString line = tokenColour(scope, "color.canon.line");
    return QStringLiteral(
        "body { color: %1; font-family: sans-serif; font-size: 13px; line-height: 1.42; }"
        "h1 { color: %2; font-size: 22px; margin-top: 4px; margin-bottom: 12px; }"
        "h2 { color: %3; font-size: 18px; margin-top: 20px; margin-bottom: 8px; }"
        "h3 { color: %2; font-size: 15px; margin-top: 14px; margin-bottom: 6px; }"
        "p { margin-top: 6px; margin-bottom: 10px; }"
        "ul, ol { margin-top: 6px; margin-bottom: 12px; }"
        "li { margin-bottom: 5px; }"
        "blockquote { color: %1; border-left: 3px solid %3; margin-left: 0; padding-left: 12px; }"
        "code { background: %4; color: %2; padding: 1px 4px; }"
        "pre { background: %4; color: %2; padding: 10px; }"
        "a { color: %5; text-decoration: none; }"
        "hr { color: %6; background-color: %6; height: 1px; }")
        .arg(inkSoft, ink, cyan, raised, aqua, line);
}

// The window's widgets on the shared canon dialog sheet, plus the notes pane
// on a nested card, the status line, and the browser's right-click menu (a
// child of the browser that the sheet's transparent rule would otherwise
// leave with no panel).
constexpr const char* kWhatsNewRules = R"(
QTextBrowser {
    color: {{color.canon.inkSoft}};
    background: {{color.canon.nested}};
    border: 1px solid {{color.canon.line}};
    border-radius: 8px;
    selection-background-color: {{color.canon.control}};
    selection-color: {{color.canon.ink}};
}
QLabel#whatsNewEyebrow {
    color: {{color.canon.cyan}};
    font-size: 11px;
    letter-spacing: 3px;
}
QLabel#whatsNewStatus {
    color: {{color.canon.muted}};
    font-size: 12px;
}
QLineEdit {
    selection-background-color: {{color.canon.cyan}};
    selection-color: {{color.canon.onAccent}};
}
QPushButton:default {
    color: {{color.canon.cyan}};
    border-color: {{color.canon.cyan}};
}
QMenu {
    color: {{color.canon.inkSoft}};
    background: {{color.canon.raised}};
    border: 1px solid {{color.canon.lineHi}};
}
QMenu::item {
    padding: 4px 18px;
}
QMenu::item:selected {
    color: {{color.canon.onAccent}};
    background: {{color.canon.cyan}};
}
QMenu::item:disabled {
    color: {{color.control.unavailable}};
}
QMenu::separator {
    height: 1px;
    margin: 4px 8px;
    background: {{color.canon.line}};
}
)";

QString whatsNewSheet()
{
    return ModemChrome::styleSheet(ModemChrome::Scale::Dialog, ModemChrome::Look::Canon)
         + canonScrollBarRules() + QString::fromLatin1(kWhatsNewRules);
}

void applyReleaseMarkdownSpacing(QTextDocument* document)
{
    if (!document)
        return;

    document->setDocumentMargin(18);

    for (QTextBlock block = document->begin(); block.isValid(); block = block.next()) {
        QTextBlockFormat format = block.blockFormat();
        const int headingLevel = format.headingLevel();
        const bool isListItem = block.textList() != nullptr;
        const bool isBlank = block.text().trimmed().isEmpty();

        if (isBlank) {
            format.setTopMargin(0);
            format.setBottomMargin(8);
        } else if (headingLevel > 0) {
            format.setTopMargin(headingLevel == 1 ? 2 : 18);
            format.setBottomMargin(headingLevel == 1 ? 12 : 8);
            format.setLineHeight(125, QTextBlockFormat::ProportionalHeight);
        } else if (isListItem) {
            format.setTopMargin(2);
            format.setBottomMargin(5);
            format.setLineHeight(138, QTextBlockFormat::ProportionalHeight);
        } else {
            format.setTopMargin(0);
            format.setBottomMargin(12);
            format.setLineHeight(145, QTextBlockFormat::ProportionalHeight);
        }

        QTextCursor cursor(block);
        cursor.mergeBlockFormat(format);
    }
}

// Every link fragment in `document`, drawn in `colour`.
void recolourLinks(QTextDocument* document, const QColor& colour)
{
    if (!document)
        return;
    // Collect first: merging a format can split or join the fragments being
    // iterated (the same care HelpDialog::applyLinkColour() takes).
    QList<QPair<int, int>> anchors;
    for (QTextBlock block = document->begin(); block.isValid(); block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (fragment.isValid() && fragment.charFormat().isAnchor())
                anchors.append({fragment.position(), fragment.length()});
        }
    }
    QTextCharFormat format;
    format.setForeground(colour);
    for (const auto& [position, length] : anchors) {
        QTextCursor cursor(document);
        cursor.setPosition(position);
        cursor.setPosition(position + length, QTextCursor::KeepAnchor);
        cursor.mergeCharFormat(format);
    }
}

} // namespace

WhatsNewDialog::WhatsNewDialog(const QString& lastSeenVersion,
                               const QString& currentVersion,
                               QWidget* parent,
                               bool showUpgrade,
                               bool currentVersionOnly)
    : CanonWindow(QStringLiteral("What's New - AetherSDR"), parent)
    , m_currentVersion(currentVersion)
{
    setObjectName(QStringLiteral("WhatsNewDialog"));
    theme::setContainer(this, QStringLiteral("canon/whatsNew"));
    // Size and position persist under the key it used as a PersistentDialog,
    // so saved geometry carries over; it resizes from every edge. Nothing
    // here is a form, so Return clicks no button.
    setGeometryKey(QStringLiteral("WhatsNewDialogGeometry"));
    setReturnClicksDefault(false);
    FramelessResizer::install(this);
    setAttribute(Qt::WA_DeleteOnClose);
    applyCanonSheet(bodyWidget(), [] { return whatsNewSheet(); });
    buildUI(lastSeenVersion, currentVersion, showUpgrade, currentVersionOnly);
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, &WhatsNewDialog::rerender);
}

void WhatsNewDialog::buildUI(const QString& lastSeenVersion,
                             const QString& currentVersion,
                             bool showUpgrade,
                             bool currentVersionOnly)
{
    auto lastSeen = VersionNumber::parse(lastSeenVersion);
    m_isWelcome = lastSeen.isNull() && !currentVersionOnly;

    auto* layout = new QVBoxLayout(bodyWidget());
    layout->setContentsMargins(14, 12, 14, 14);
    layout->setSpacing(8);
    layout->addWidget(makeCanonHeader(m_isWelcome ? QStringLiteral("Welcome!")
                                                  : QStringLiteral("What's New")));

    auto* eyebrow = new QLabel(QStringLiteral("AETHERSDR V%1").arg(currentVersion));
    eyebrow->setObjectName(QStringLiteral("whatsNewEyebrow"));
    eyebrow->setTextFormat(Qt::PlainText);
    layout->addWidget(eyebrow);

    m_statusLabel = new QLabel;
    m_statusLabel->setObjectName(QStringLiteral("whatsNewStatus"));
    m_statusLabel->setWordWrap(true);
    layout->addWidget(m_statusLabel);

    m_browser = new QTextBrowser;
    m_browser->setObjectName(QStringLiteral("whatsNewNotes"));
    m_browser->setAccessibleName(QStringLiteral("Release notes"));
    m_browser->setOpenExternalLinks(false);
    m_browser->setOpenLinks(false);
    m_browser->setReadOnly(true);
    // A stylesheet reflow can clamp the scrollbar before themeChanged reaches
    // rerender(). Keep the reader's last position, excluding range clamps.
    connect(m_browser->verticalScrollBar(), &QScrollBar::rangeChanged, this,
            [this](int, int maximum) {
        if (!m_rendering && m_browser->verticalScrollBar()->value() > maximum) {
            m_scrollClamping = true;
        }
    });
    connect(m_browser->verticalScrollBar(), &QScrollBar::valueChanged, this,
            [this](int value) {
        if (m_rendering) {
            return;
        }
        if (m_scrollClamping) {
            m_scrollClamping = false;
            return;
        }
        m_notesScroll = value;
    });
    connect(m_browser, &QTextBrowser::anchorClicked, this, [](const QUrl& url) {
        if (const auto target = whatsNewLinkTarget(url)) {
            QDesktopServices::openUrl(*target);
        }
    });
    layout->addWidget(m_browser, 1);

    auto* footerLayout = new QHBoxLayout;
    footerLayout->setSpacing(10);
    footerLayout->addStretch(1);

    auto* findBtn = new QPushButton("Find");
    findBtn->setObjectName(QStringLiteral("whatsNewFind"));
    findBtn->setAccessibleName(QStringLiteral("Find in release notes"));
    findBtn->setAutoDefault(false);
    connect(findBtn, &QPushButton::clicked, this, &WhatsNewDialog::promptFind);
    footerLayout->addWidget(findBtn);

    if (showUpgrade) {
        auto* upgradeBtn = new QPushButton("Upgrade");
        upgradeBtn->setAutoDefault(false);
        connect(upgradeBtn, &QPushButton::clicked, this, [this] {
            QDesktopServices::openUrl(QUrl(AetherSDR::UpdateChecker::kReleasesPageUrl));
            close();
        });
        footerLayout->addWidget(upgradeBtn);

        auto* skipBtn = new QPushButton("Skip this version");
        skipBtn->setAutoDefault(false);
        connect(skipBtn, &QPushButton::clicked, this, [currentVersion, this] {
            auto& s = AppSettings::instance();
            s.setValue("LastSeenVersion", currentVersion);
            s.save();
            close();
        });
        footerLayout->addWidget(skipBtn);
    }
    layout->addLayout(footerLayout);

    resize(820, 720);
    setMinimumSize(QSize(560, 460).expandedTo(minimumSizeHint()));

    showLoadingState();
    if (s_fetchReleaseNotes)
        fetchLiveReleaseNotes();
}

void WhatsNewDialog::fetchLiveReleaseNotes()
{
    if (!m_browser)
        return;

    const QString tagName = releaseTag();
    setStatusText(QString("Loading detailed release notes from GitHub for %1...").arg(tagName));

    QNetworkRequest request{QUrl(releaseApiUrl(tagName))};
    request.setHeader(QNetworkRequest::UserAgentHeader, "AetherSDR");
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setTransferTimeout(15000);

    auto* nam = new QNetworkAccessManager(this);
    auto* reply = nam->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, nam] {
        const QByteArray payload = reply->readAll();
        const bool ok = reply->error() == QNetworkReply::NoError;
        const QString errorText = ok ? QString() : releaseErrorText(reply, payload);
        reply->deleteLater();
        nam->deleteLater();

        if (!ok) {
            showReleaseLoadError(errorText);
            return;
        }

        QJsonParseError parseError;
        const QJsonDocument doc = QJsonDocument::fromJson(payload, &parseError);
        if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
            showReleaseLoadError("GitHub returned unreadable release data.");
            return;
        }

        const QJsonObject obj = doc.object();
        const QString body = obj.value("body").toString().trimmed();
        if (body.isEmpty()) {
            showReleaseLoadError("This GitHub release has no published notes.");
            return;
        }

        showLiveReleaseNotes(obj.value("name").toString(),
                             obj.value("tag_name").toString(),
                             obj.value("published_at").toString(),
                             body);
    });
}

void WhatsNewDialog::showLoadingState()
{
    if (!m_browser)
        return;

    setStatusText(QString("Loading release notes for %1...").arg(releaseTag()));
    m_content = Content::Loading;
    rerender();
}

void WhatsNewDialog::showReleaseLoadError(const QString& message)
{
    if (!m_browser)
        return;

    setStatusText(QString("Could not load release notes for %1").arg(releaseTag()));
    m_content = Content::Error;
    m_errorMessage = message;
    rerender();
}

void WhatsNewDialog::showLiveReleaseNotes(const QString& title,
                                          const QString& tagName,
                                          const QString& publishedAt,
                                          const QString& bodyMarkdown)
{
    if (!m_browser)
        return;

    QString markdown = enrichGitHubReferences(stripDuplicateHeading(bodyMarkdown, title, tagName));
    if (markdown.isEmpty())
        markdown = QString("_No detailed release notes were published for %1._")
            .arg(tagName.isEmpty() ? releaseTag() : tagName);

    m_content = Content::Notes;
    m_notesMarkdown = markdown;
    rerender();
    m_browser->moveCursor(QTextCursor::Start);

    const QString releaseTitle = title.isEmpty()
        ? (tagName.isEmpty() ? releaseTag() : tagName)
        : title;
    const QString dateText = formattedPublishedAt(publishedAt);
    const QString source = dateText.isEmpty()
        ? releaseTitle
        : QString("%1\n\nReleased %2").arg(releaseTitle, dateText);
    setStatusText(source);
}

void WhatsNewDialog::rerender()
{
    if (!m_browser)
        return;
    const int scroll = m_notesScroll;
    const QTextCursor selection = m_browser->textCursor();
    const int anchor = selection.anchor();
    const int position = selection.position();
    m_rendering = true;
    const QString muted = tokenColour(this, "color.canon.muted");
    switch (m_content) {
    case Content::Loading:
        m_browser->setHtml(QStringLiteral(
            "<div style='color:%1; font-family:sans-serif; font-size:13px; "
            "padding:40px; text-align:center;'>Loading release notes from GitHub...</div>").arg(muted));
        break;
    case Content::Error:
        m_browser->setHtml(QStringLiteral(
            "<div style='color:%1; font-family:sans-serif; font-size:13px; "
            "padding:32px; line-height:1.45;'>"
            "<h3 style='color:%2; margin-top:0;'>GitHub release notes unavailable</h3>"
            "<p style='color:%1;'>%3</p>"
            "<p style='color:%4;'>Check your network connection and reopen What's New to try again.</p>"
            "</div>")
            .arg(tokenColour(this, "color.canon.inkSoft"), tokenColour(this, "color.canon.ink"),
                 m_errorMessage.toHtmlEscaped(), muted));
        break;
    case Content::Notes: {
        // setMarkdown() does not read the document's default stylesheet: the
        // text takes the browser's ink, and the importer bakes Qt's default
        // link blue into every link's format, unreadable on the canon ground.
        // Recolour the links in canon cyan after each import, so a theme
        // change follows. The stylesheet still serves any HTML in the notes.
        m_browser->document()->setDefaultStyleSheet(releaseMarkdownStyleSheet(this));
        m_browser->document()->setMarkdown(m_notesMarkdown, QTextDocument::MarkdownDialectGitHub);
        applyReleaseMarkdownSpacing(m_browser->document());
        recolourLinks(m_browser->document(),
                      ThemeManager::instance().color(this, QStringLiteral("color.canon.cyan")));
        break;
    }
    }
    if (m_content == Content::Notes) {
        QTextCursor restored(m_browser->document());
        const int last = m_browser->document()->characterCount() - 1;
        restored.setPosition(qMin(anchor, last));
        restored.setPosition(qMin(position, last), QTextCursor::KeepAnchor);
        m_browser->setTextCursor(restored);
    }
    if (m_browser->verticalScrollBar()) {
        m_browser->verticalScrollBar()->setValue(scroll);
    }
    m_notesScroll = m_browser->verticalScrollBar()->value();
    m_scrollClamping = false;
    m_rendering = false;
}

void WhatsNewDialog::promptFind()
{
    if (!m_browser)
        return;

    // A small canon dialog in this window's scope: the field, then Cancel and
    // Find; Find is the default, so Return searches and Esc or the ✕ cancel.
    const QPointer<WhatsNewDialog> self(this);
    ScopedChildWidget<CanonWindow> promptOwner(QStringLiteral("Find"), this);
    CanonWindow& prompt = *promptOwner.get();
    prompt.setObjectName(QStringLiteral("whatsNewFindPrompt"));
    prompt.setModal(true);
    applyCanonSheet(prompt.bodyWidget(), [] { return whatsNewSheet(); });
    auto* root = new QVBoxLayout(prompt.bodyWidget());
    root->setContentsMargins(14, 12, 14, 14);
    root->setSpacing(10);
    root->addWidget(makeCanonHeader(QStringLiteral("Find")));
    auto* field = new QLineEdit(m_lastFindText, prompt.bodyWidget());
    field->setObjectName(QStringLiteral("whatsNewFindField"));
    field->setAccessibleName(QStringLiteral("Text to find in the release notes"));
    field->setPlaceholderText(QStringLiteral("Text to find"));
    field->setMinimumWidth(280);
    field->selectAll();
    root->addWidget(field);
    auto* buttons = new QHBoxLayout;
    buttons->addStretch(1);
    auto* cancel = new QPushButton(QStringLiteral("Cancel"), prompt.bodyWidget());
    cancel->setObjectName(QStringLiteral("whatsNewFindCancel"));
    cancel->setAutoDefault(false);
    auto* find = new QPushButton(QStringLiteral("Find"), prompt.bodyWidget());
    find->setObjectName(QStringLiteral("whatsNewFindConfirm"));
    find->setDefault(true);
    connect(cancel, &QPushButton::clicked, &prompt, &QDialog::reject);
    connect(find, &QPushButton::clicked, &prompt, &QDialog::accept);
    buttons->addWidget(cancel);
    buttons->addWidget(find);
    root->addLayout(buttons);
    field->setFocus();
    const int result = prompt.exec();
    if (!self || !promptOwner || result != QDialog::Accepted)
        return;
    const QString text = field->text().trimmed();
    if (text.isEmpty())
        return;

    m_lastFindText = text;
    if (!findInNotes(text))
        setStatusText(QString("No matches for \"%1\"").arg(text));
}

bool WhatsNewDialog::findInNotes(const QString& text)
{
    if (!m_browser)
        return false;

    if (m_browser->find(text))
        return true;

    QTextCursor cursor = m_browser->textCursor();
    cursor.movePosition(QTextCursor::Start);
    m_browser->setTextCursor(cursor);
    return m_browser->find(text);
}

void WhatsNewDialog::setStatusText(const QString& text)
{
    if (!m_statusLabel)
        return;
    // Avoid Qt::convertFromPlainText — it wraps in <p> and on some Qt
    // versions includes <!--StartFragment--> clipboard-format artifacts.
    // Predictable plain-text→HTML: escape special characters, then map
    // newlines to <br/> so multi-line status messages render as
    // intended without surprises across Qt versions.
    m_statusLabel->setText(text.toHtmlEscaped().replace('\n', "<br/>"));
}

QString WhatsNewDialog::releaseTag() const
{
    return normalizedTag(m_currentVersion);
}

} // namespace AetherSDR
