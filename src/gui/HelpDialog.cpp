#include "HelpDialog.h"
#include "CanonIndicators.h"
#include "CanonScrollBarStyle.h"
#include "FramelessResizer.h"
#include "ModemChrome.h"

#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStyle>
#include <QShortcut>
#include <QTextBrowser>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QVBoxLayout>
#include "core/ThemeManager.h"

namespace AetherSDR {

namespace {

// Per-guide geometry key derived from the (stable) help resource path, so two
// guides open at once persist and restore independent positions instead of
// sharing one "HelpDialogGeometry" slot and stacking on top of each other.
QString helpGeometryKey(const QString& resourcePath)
{
    const QString stem = QFileInfo(resourcePath).baseName();
    return stem.isEmpty() ? QStringLiteral("HelpDialogGeometry")
                          : QStringLiteral("HelpDialogGeometry_") + stem;
}

// The guide's widgets on the shared canon dialog sheet, plus: the guide pane
// on a nested card; the eyebrow, subtitle, hint and find status; the find
// field and status in color.accent.danger while a search finds nothing (a
// property the sheet matches); selection ink; and the browser's and field's
// right-click menus, children that the sheet's transparent rule would
// otherwise leave with no panel.
constexpr const char* kHelpRules = R"(
QTextBrowser {
    color: {{color.canon.inkSoft}};
    background: {{color.canon.nested}};
    border: 1px solid {{color.canon.line}};
    border-radius: 8px;
    padding: 6px;
    selection-background-color: {{color.canon.control}};
    selection-color: {{color.canon.ink}};
}
QLabel#helpEyebrow {
    color: {{color.canon.cyan}};
    font-size: 11px;
    letter-spacing: 2px;
}
QLabel#helpSubtitle,
QLabel#helpHint,
QLabel#helpFindStatus {
    color: {{color.canon.muted}};
    font-size: 11px;
}
QLabel#helpFindStatus[noMatch="true"] {
    color: {{color.accent.danger}};
    font-weight: 700;
}
QLineEdit#helpFindEdit[noMatch="true"] {
    border-color: {{color.accent.danger}};
}
QLineEdit {
    selection-background-color: {{color.canon.cyan}};
    selection-color: {{color.canon.onAccent}};
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

void setNoMatch(QWidget* widget, bool noMatch)
{
    if (!widget || widget->property("noMatch").toBool() == noMatch)
        return;
    widget->setProperty("noMatch", noMatch);
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
}

} // namespace

HelpDialog::HelpDialog(const QString& windowTitle,
                       const QString& resourcePath,
                       QWidget* parent)
    : CanonWindow(windowTitle, parent, Kind::Workspace)
{
    setObjectName(QStringLiteral("HelpDialog"));
    theme::setContainer(this, QStringLiteral("canon/help"));
    // Each guide keeps the per-guide key it used as a PersistentDialog, so
    // saved geometry carries over; it resizes from every edge.
    setGeometryKey(helpGeometryKey(resourcePath));
    FramelessResizer::install(this);
    applyCanonSheet(bodyWidget(), [] {
        return ModemChrome::styleSheet(ModemChrome::Scale::Dialog, ModemChrome::Look::Canon)
             + canonScrollBarRules() + QString::fromLatin1(kHelpRules);
    });
    buildUI(resourcePath);
}

void HelpDialog::buildUI(const QString& resourcePath)
{
    auto* layout = new QVBoxLayout(bodyWidget());
    layout->setContentsMargins(14, 12, 14, 14);
    layout->setSpacing(8);
    layout->addWidget(makeCanonHeader(windowTitle()));

    auto* eyebrow = new QLabel("AETHERSDR OFFLINE HELP", bodyWidget());
    eyebrow->setObjectName(QStringLiteral("helpEyebrow"));
    layout->addWidget(eyebrow);

    auto* subtitle = new QLabel(
        "Bundled help is available even when your station computer is offline.",
        bodyWidget());
    subtitle->setObjectName(QStringLiteral("helpSubtitle"));
    subtitle->setWordWrap(true);
    layout->addWidget(subtitle);

    auto* findRow = new QWidget(bodyWidget());
    findRow->setObjectName("helpFindRow");
    auto* findLayout = new QHBoxLayout(findRow);
    findLayout->setContentsMargins(0, 4, 0, 4);
    findLayout->setSpacing(8);

    auto* findLabel = new QLabel("Find:", findRow);
    findLayout->addWidget(findLabel);

    m_findEdit = new QLineEdit(findRow);
    m_findEdit->setObjectName("helpFindEdit");
    m_findEdit->setPlaceholderText("Subject or term");
    m_findEdit->setClearButtonEnabled(true);
    m_findEdit->setAccessibleName(QStringLiteral("Find in this guide"));
    findLabel->setBuddy(m_findEdit);
    m_findEdit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    findLayout->addWidget(m_findEdit, 1);

    m_findPrevButton = new QPushButton("Previous", findRow);
    m_findPrevButton->setObjectName("helpFindPrevButton");
    m_findPrevButton->setCursor(Qt::PointingHandCursor);
    m_findPrevButton->setAutoDefault(false);
    m_findPrevButton->setDefault(false);
    m_findPrevButton->setEnabled(false);
    m_findPrevButton->setToolTip("Find previous match (Shift+Return)");
    m_findPrevButton->setAccessibleDescription("Find previous match (Shift+Return)");
    findLayout->addWidget(m_findPrevButton);

    m_findButton = new QPushButton("Next", findRow);
    m_findButton->setObjectName("helpFindButton");
    m_findButton->setCursor(Qt::PointingHandCursor);
    m_findButton->setAutoDefault(false);
    m_findButton->setDefault(false);
    m_findButton->setEnabled(false);
    m_findButton->setToolTip("Find next match (Return)");
    m_findButton->setAccessibleDescription("Find next match (Return)");
    findLayout->addWidget(m_findButton);

    m_findStatus = new QLabel(findRow);
    m_findStatus->setObjectName("helpFindStatus");
    m_findStatus->setMinimumWidth(96);
    m_findStatus->setTextFormat(Qt::PlainText);
    findLayout->addWidget(m_findStatus);

    layout->addWidget(findRow);

    m_browser = new QTextBrowser(bodyWidget());
    m_browser->setObjectName("helpBrowser");
    m_browser->setReadOnly(true);
    m_browser->setOpenExternalLinks(true);
    m_browser->setOpenLinks(true);
    m_browser->document()->setDocumentMargin(18);
    m_browser->setAccessibleName(windowTitle());
    m_browser->setMarkdown(loadMarkdown(resourcePath));
    // The Markdown importer colours links from the palette, not the default
    // stylesheet, which left them dark blue on the dark page. Use the theme's
    // link colour, as the About window does, and follow theme changes.
    applyLinkColour();
    connect(&AetherSDR::ThemeManager::instance(), &AetherSDR::ThemeManager::themeChanged,
            this, &HelpDialog::applyLinkColour);
    layout->addWidget(m_browser, 1);

    connect(m_findEdit, &QLineEdit::textChanged, this, [this](const QString& text) {
        const bool hasTerm = !text.isEmpty();
        if (m_findButton)      m_findButton->setEnabled(hasTerm);
        if (m_findPrevButton)  m_findPrevButton->setEnabled(hasTerm);
        resetFindState();
    });
    connect(m_findEdit, &QLineEdit::returnPressed, this, &HelpDialog::findNext);
    connect(m_findButton, &QPushButton::clicked, this, &HelpDialog::findNext);
    connect(m_findPrevButton, &QPushButton::clicked, this, &HelpDialog::findPrevious);
    new QShortcut(QKeySequence::Find, this, [this]() { focusFindField(); });
    // Shift+Return inside the find field → previous match.  Scope to the
    // find field so the shortcut is only active when typing a query.
    auto* prevShortcut = new QShortcut(QKeySequence(Qt::SHIFT | Qt::Key_Return),
                                       m_findEdit,
                                       [this]() { findPrevious(); });
    prevShortcut->setContext(Qt::WidgetShortcut);

    auto* hint = new QLabel(
        "Tip: The Help menu keeps each guide separate so you can reopen just the topic you need.",
        bodyWidget());
    hint->setObjectName(QStringLiteral("helpHint"));
    hint->setWordWrap(true);
    layout->addWidget(hint);

    resize(760, 720);
    setMinimumSize(QSize(520, 460).expandedTo(minimumSizeHint()));
}

void HelpDialog::applyLinkColour()
{
    if (!m_browser)
        return;

    QTextDocument* doc = m_browser->document();
    // Collect first: merging a format can split or join the fragments being
    // iterated.
    QList<QPair<int, int>> anchors;
    for (QTextBlock block = doc->begin(); block.isValid(); block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (fragment.isValid() && fragment.charFormat().isAnchor())
                anchors.append({fragment.position(), fragment.length()});
        }
    }

    QTextCharFormat linkFormat;
    linkFormat.setForeground(AetherSDR::ThemeManager::instance().color(
        m_browser, QStringLiteral("color.canon.cyan")));
    for (const auto& [position, length] : anchors) {
        QTextCursor cursor(doc);
        cursor.setPosition(position);
        cursor.setPosition(position + length, QTextCursor::KeepAnchor);
        cursor.mergeCharFormat(linkFormat);
    }
}

void HelpDialog::focusFindField()
{
    if (!m_findEdit)
        return;

    m_findEdit->setFocus(Qt::ShortcutFocusReason);
    m_findEdit->selectAll();
}

void HelpDialog::findNext()
{
    runFind(/*backward=*/false);
}

void HelpDialog::findPrevious()
{
    runFind(/*backward=*/true);
}

void HelpDialog::runFind(bool backward)
{
    if (!m_browser || !m_findEdit)
        return;

    const QString term = m_findEdit->text();
    if (term.isEmpty()) {
        updateFindFeedback(QString(), false);
        focusFindField();
        return;
    }

    const QTextDocument::FindFlags flags = backward
        ? QTextDocument::FindFlags(QTextDocument::FindBackward)
        : QTextDocument::FindFlags();

    if (m_browser->find(term, flags)) {
        updateFindFeedback(QString(), false);
        m_findEdit->setFocus(Qt::OtherFocusReason);
        return;
    }

    // No match from the current cursor position.  Wrap to the opposite
    // end of the document (top for forward, bottom for backward) and
    // retry so Return/Shift+Return both circle through matches.
    QTextCursor cursor = m_browser->textCursor();
    cursor.movePosition(backward ? QTextCursor::End : QTextCursor::Start);
    cursor.clearSelection();
    m_browser->setTextCursor(cursor);

    if (m_browser->find(term, flags)) {
        updateFindFeedback(backward ? "Wrapped to bottom" : "Wrapped to top", false);
    } else {
        cursor = m_browser->textCursor();
        cursor.movePosition(QTextCursor::Start);
        cursor.clearSelection();
        m_browser->setTextCursor(cursor);
        updateFindFeedback("No matches", true);
    }

    m_findEdit->setFocus(Qt::OtherFocusReason);
}

QString HelpDialog::loadMarkdown(const QString& resourcePath) const
{
    QFile file(resourcePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return QStringLiteral(
            "# Help file not available\n\n"
            "AetherSDR could not open this bundled help topic.\n\n"
            "Please reinstall the application or report the missing help asset.");
    }

    return QString::fromUtf8(file.readAll());
}

void HelpDialog::resetFindState()
{
    if (!m_browser)
        return;

    QTextCursor cursor = m_browser->textCursor();
    cursor.movePosition(QTextCursor::Start);
    cursor.clearSelection();
    m_browser->setTextCursor(cursor);
    updateFindFeedback(QString(), false);
}

void HelpDialog::updateFindFeedback(const QString& message, bool noMatch)
{
    if (m_findStatus) {
        m_findStatus->setText(message);
        setNoMatch(m_findStatus, noMatch);
    }
    setNoMatch(m_findEdit, noMatch);
}

} // namespace AetherSDR
