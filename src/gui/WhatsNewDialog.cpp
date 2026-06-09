#include "WhatsNewDialog.h"
#include "core/UpdateChecker.h"
#include "core/VersionNumber.h"
#include "core/AppSettings.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QRegularExpression>
#include <QTextBrowser>
#include <QTextBlock>
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

QUrl githubUrlForMaybeRelativeLink(const QUrl& url)
{
    if (!url.isRelative())
        return url;
    return QUrl("https://github.com/aethersdr/AetherSDR/").resolved(url);
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

QString releaseMarkdownStyleSheet()
{
    return QStringLiteral(
        "body { color: #d6e2ee; font-family: sans-serif; font-size: 13px; line-height: 1.42; }"
        "h1 { color: #f2f7fb; font-size: 22px; margin-top: 4px; margin-bottom: 12px; }"
        "h2 { color: #00b4d8; font-size: 18px; margin-top: 20px; margin-bottom: 8px; }"
        "h3 { color: #f2f7fb; font-size: 15px; margin-top: 14px; margin-bottom: 6px; }"
        "p { margin-top: 6px; margin-bottom: 10px; }"
        "ul, ol { margin-top: 6px; margin-bottom: 12px; }"
        "li { margin-bottom: 5px; }"
        "blockquote { color: #aebfce; border-left: 3px solid #00b4d8; margin-left: 0; padding-left: 12px; }"
        "code { background: #172433; color: #d8eef8; padding: 1px 4px; }"
        "pre { background: #101a26; color: #d8eef8; padding: 10px; }"
        "a { color: #56ccf2; text-decoration: none; }"
        "hr { color: #304050; background-color: #304050; height: 1px; }");
}

QString secondaryButtonStyle()
{
    return QStringLiteral(
        "QPushButton { background: #1a2a3a; color: #d6e2ee; font-weight: bold; "
        "font-size: 13px; border-radius: 6px; padding: 0 24px; "
        "border: 1px solid #304050; }"
        "QPushButton:hover { background: #20384c; }"
        "QPushButton:pressed { background: #162838; }");
}

QString primaryButtonStyle()
{
    return QStringLiteral(
        "QPushButton { background: #00b4d8; color: #071018; font-weight: bold; "
        "font-size: 13px; border-radius: 6px; padding: 0 28px; border: none; }"
        "QPushButton:hover { background: #17c9ea; }"
        "QPushButton:pressed { background: #0798b6; }");
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

} // namespace

WhatsNewDialog::WhatsNewDialog(const QString& lastSeenVersion,
                               const QString& currentVersion,
                               QWidget* parent,
                               bool showUpgrade,
                               bool currentVersionOnly)
    : PersistentDialog("What's New - AetherSDR", "WhatsNewDialogGeometry", parent)
    , m_currentVersion(currentVersion)
{
    theme::setContainer(this, QStringLiteral("dialog/whatsNew"));
    setAttribute(Qt::WA_DeleteOnClose);
    buildUI(lastSeenVersion, currentVersion, showUpgrade, currentVersionOnly);
}

WhatsNewDialog* WhatsNewDialog::showAll(QWidget* parent)
{
    auto* dlg = new WhatsNewDialog("", QCoreApplication::applicationVersion(), parent,
                                   false, true);
    dlg->show();
    return dlg;
}

void WhatsNewDialog::buildUI(const QString& lastSeenVersion,
                             const QString& currentVersion,
                             bool showUpgrade,
                             bool currentVersionOnly)
{
    resize(820, 680);
    setMinimumSize(560, 420);

    auto lastSeen = VersionNumber::parse(lastSeenVersion);
    m_isWelcome = lastSeen.isNull() && !currentVersionOnly;

    auto* layout = new QVBoxLayout(bodyWidget());
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* header = new QLabel;
    header->setAlignment(Qt::AlignCenter);
    const QString heading = m_isWelcome ? "Welcome!" : "What's New";
    header->setText(QString("<div style='padding: 18px 20px 12px 20px;'>"
        "<span style='color: #00b4d8; font-size: 11px; letter-spacing: 3px;'>"
        "AETHERSDR V%1</span><br>"
        "<span style='color: #dce8f3; font-size: 24px; font-weight: bold;'>"
        "%2</span></div>").arg(currentVersion, heading));
    AetherSDR::ThemeManager::instance().applyStyleSheet(header, "QLabel { background: {{color.background.0}}; }");
    layout->addWidget(header);

    m_statusLabel = new QLabel;
    m_statusLabel->setAlignment(Qt::AlignCenter);
    m_statusLabel->setWordWrap(true);
    m_statusLabel->setContentsMargins(18, 0, 18, 8);
    AetherSDR::ThemeManager::instance().applyStyleSheet(m_statusLabel, "QLabel { background: {{color.background.0}}; color: {{color.text.secondary}}; font-size: 12px; }");
    layout->addWidget(m_statusLabel);

    auto* sep = new QWidget;
    sep->setFixedHeight(1);
    AetherSDR::ThemeManager::instance().applyStyleSheet(sep, "background: {{color.background.1}};");
    layout->addWidget(sep);

    m_browser = new QTextBrowser;
    m_browser->setOpenExternalLinks(false);
    m_browser->setOpenLinks(false);
    m_browser->setReadOnly(true);
    AetherSDR::ThemeManager::instance().applyStyleSheet(m_browser, "QTextBrowser { background: {{color.background.0}}; color: #d6e2ee; border: none; "
        "padding: 18px; font-size: 13px; }"
        "QScrollBar:vertical { background: {{color.background.0}}; width: 10px; }"
        "QScrollBar::handle:vertical { background: {{color.background.2}}; border-radius: 5px; }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }");
    connect(m_browser, &QTextBrowser::anchorClicked, this, [](const QUrl& url) {
        QDesktopServices::openUrl(githubUrlForMaybeRelativeLink(url));
    });
    layout->addWidget(m_browser, 1);

    auto* footer = new QWidget;
    AetherSDR::ThemeManager::instance().applyStyleSheet(footer, "background: {{color.background.0}};");
    auto* footerLayout = new QHBoxLayout(footer);
    footerLayout->setContentsMargins(16, 12, 16, 16);
    footerLayout->setSpacing(12);
    footerLayout->addStretch(1);

    auto* findBtn = new QPushButton("Find");
    findBtn->setMinimumWidth(96);
    findBtn->setFixedHeight(36);
    findBtn->setCursor(Qt::PointingHandCursor);
    findBtn->setStyleSheet(secondaryButtonStyle());
    connect(findBtn, &QPushButton::clicked, this, &WhatsNewDialog::promptFind);
    footerLayout->addWidget(findBtn, 0, Qt::AlignCenter);

    if (showUpgrade) {
        auto* upgradeBtn = new QPushButton("Upgrade");
        upgradeBtn->setFixedHeight(36);
        upgradeBtn->setCursor(Qt::PointingHandCursor);
        upgradeBtn->setStyleSheet(secondaryButtonStyle());
        connect(upgradeBtn, &QPushButton::clicked, this, [this] {
            QDesktopServices::openUrl(QUrl(AetherSDR::UpdateChecker::kReleasesPageUrl));
            close();
        });
        footerLayout->addWidget(upgradeBtn, 0, Qt::AlignCenter);

        auto* skipBtn = new QPushButton("Skip this version");
        skipBtn->setFixedHeight(36);
        skipBtn->setCursor(Qt::PointingHandCursor);
        skipBtn->setStyleSheet(secondaryButtonStyle());
        connect(skipBtn, &QPushButton::clicked, this, [currentVersion, this] {
            auto& s = AppSettings::instance();
            s.setValue("LastSeenVersion", currentVersion);
            s.save();
            close();
        });
        footerLayout->addWidget(skipBtn, 0, Qt::AlignCenter);
    }

    auto* closeBtn = new QPushButton("Close");
    closeBtn->setMinimumWidth(96);
    closeBtn->setFixedHeight(36);
    closeBtn->setCursor(Qt::PointingHandCursor);
    closeBtn->setStyleSheet(primaryButtonStyle());
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::close);
    footerLayout->addWidget(closeBtn, 0, Qt::AlignCenter);
    footerLayout->addStretch(1);
    layout->addWidget(footer);

    AetherSDR::ThemeManager::instance().applyStyleSheet(this, "WhatsNewDialog { background: {{color.background.0}}; }");

    showLoadingState();
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
    m_browser->setHtml(
        "<div style='color:#8aa8c0; font-family:sans-serif; font-size:13px; "
        "padding:40px; text-align:center;'>Loading release notes from GitHub...</div>");
}

void WhatsNewDialog::showReleaseLoadError(const QString& message)
{
    if (!m_browser)
        return;

    setStatusText(QString("Could not load release notes for %1").arg(releaseTag()));
    m_browser->setHtml(QString(
        "<div style='color:#c8d8e8; font-family:sans-serif; font-size:13px; "
        "padding:32px; line-height:1.45;'>"
        "<h3 style='color:#f2f7fb; margin-top:0;'>GitHub release notes unavailable</h3>"
        "<p style='color:#aebfce;'>%1</p>"
        "<p style='color:#8aa8c0;'>Check your network connection and reopen What's New to try again.</p>"
        "</div>").arg(message.toHtmlEscaped()));
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

    m_browser->document()->setDefaultStyleSheet(releaseMarkdownStyleSheet());
    m_browser->document()->setMarkdown(markdown, QTextDocument::MarkdownDialectGitHub);
    applyReleaseMarkdownSpacing(m_browser->document());
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

void WhatsNewDialog::promptFind()
{
    if (!m_browser)
        return;

    bool ok = false;
    const QString text = QInputDialog::getText(this,
                                               "Find",
                                               "Find:",
                                               QLineEdit::Normal,
                                               m_lastFindText,
                                               &ok).trimmed();
    if (!ok || text.isEmpty())
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
