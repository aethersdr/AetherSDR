#include "HelpDialog.h"

#include <QDialogButtonBox>
#include <QFile>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QShortcut>
#include <QTextBrowser>
#include <QTextCursor>
#include <QVBoxLayout>

namespace MasterSDR {

namespace {

const char* kFindEditStyle =
    "QLineEdit {"
    "  background: #111d29;"
    "  color: #d8e4ee;"
    "  border: 1px solid #304050;"
    "  border-radius: 4px;"
    "  min-height: 26px;"
    "  padding: 2px 8px;"
    "}"
    "QLineEdit:focus { border-color: #00b4d8; }";

const char* kFindEditNoMatchStyle =
    "QLineEdit {"
    "  background: #111d29;"
    "  color: #d8e4ee;"
    "  border: 1px solid #c65050;"
    "  border-radius: 4px;"
    "  min-height: 26px;"
    "  padding: 2px 8px;"
    "}"
    "QLineEdit:focus { border-color: #e07070; }";

} // namespace

HelpDialog::HelpDialog(const QString& windowTitle,
                       const QString& resourcePath,
                       QWidget* parent)
    : QDialog(parent)
{
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
    setWindowTitle(windowTitle);
    buildUI(resourcePath);
}

void HelpDialog::buildUI(const QString& resourcePath)
{
    resize(760, 680);
    setMinimumSize(520, 420);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* header = new QWidget(this);
    header->setStyleSheet("background: #0a0a14;");
    auto* headerLayout = new QVBoxLayout(header);
    headerLayout->setContentsMargins(20, 18, 20, 14);
    headerLayout->setSpacing(6);

    auto* eyebrow = new QLabel("MasterSDR OFFLINE HELP", header);
    eyebrow->setStyleSheet("color: #00b4d8; font-size: 11px; letter-spacing: 2px;");
    headerLayout->addWidget(eyebrow);

    auto* title = new QLabel(windowTitle(), header);
    title->setWordWrap(true);
    title->setStyleSheet("color: #c8d8e8; font-size: 22px; font-weight: 700;");
    headerLayout->addWidget(title);

    auto* subtitle = new QLabel(
        "Bundled help is available even when your station computer is offline.",
        header);
    subtitle->setWordWrap(true);
    subtitle->setStyleSheet("color: #8aa8c0; font-size: 12px;");
    headerLayout->addWidget(subtitle);

    auto* findRow = new QWidget(header);
    findRow->setObjectName("helpFindRow");
    auto* findLayout = new QHBoxLayout(findRow);
    findLayout->setContentsMargins(0, 8, 0, 0);
    findLayout->setSpacing(8);

    auto* findLabel = new QLabel("Find:", findRow);
    findLabel->setStyleSheet("color: #8aa8c0; font-size: 12px; font-weight: 700;");
    findLayout->addWidget(findLabel);

    m_findEdit = new QLineEdit(findRow);
    m_findEdit->setObjectName("helpFindEdit");
    m_findEdit->setPlaceholderText("Subject or term");
    m_findEdit->setClearButtonEnabled(true);
    m_findEdit->setStyleSheet(kFindEditStyle);
    m_findEdit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    findLayout->addWidget(m_findEdit, 1);

    const QString kFindButtonStyle =
        "QPushButton {"
        "  background: #203040;"
        "  color: #d8e4ee;"
        "  border: 1px solid #38506a;"
        "  border-radius: 4px;"
        "  min-width: 72px;"
        "  min-height: 28px;"
        "  padding: 0 12px;"
        "}"
        "QPushButton:hover:enabled { background: #28435a; }"
        "QPushButton:disabled { color: #5f7080; border-color: #263442; }";

    m_findPrevButton = new QPushButton("Previous", findRow);
    m_findPrevButton->setObjectName("helpFindPrevButton");
    m_findPrevButton->setCursor(Qt::PointingHandCursor);
    m_findPrevButton->setAutoDefault(false);
    m_findPrevButton->setDefault(false);
    m_findPrevButton->setEnabled(false);
    m_findPrevButton->setToolTip("Find previous match (Shift+Return)");
    m_findPrevButton->setStyleSheet(kFindButtonStyle);
    findLayout->addWidget(m_findPrevButton);

    m_findButton = new QPushButton("Next", findRow);
    m_findButton->setObjectName("helpFindButton");
    m_findButton->setCursor(Qt::PointingHandCursor);
    m_findButton->setAutoDefault(false);
    m_findButton->setDefault(false);
    m_findButton->setEnabled(false);
    m_findButton->setToolTip("Find next match (Return)");
    m_findButton->setStyleSheet(kFindButtonStyle);
    findLayout->addWidget(m_findButton);

    m_findStatus = new QLabel(findRow);
    m_findStatus->setObjectName("helpFindStatus");
    m_findStatus->setMinimumWidth(96);
    m_findStatus->setStyleSheet("color: #7f93a7; font-size: 11px;");
    findLayout->addWidget(m_findStatus);

    headerLayout->addWidget(findRow);

    layout->addWidget(header);

    auto* separator = new QWidget(this);
    separator->setFixedHeight(1);
    separator->setStyleSheet("background: #203040;");
    layout->addWidget(separator);

    m_browser = new QTextBrowser(this);
    m_browser->setObjectName("helpBrowser");
    m_browser->setReadOnly(true);
    m_browser->setOpenExternalLinks(true);
    m_browser->setOpenLinks(true);
    m_browser->document()->setDocumentMargin(18);
    m_browser->document()->setDefaultStyleSheet(
        "body { color: #c8d8e8; font-size: 12px; font-family: sans-serif; }"
        "h1 { color: #00b4d8; font-size: 22px; font-weight: 700; margin: 0 0 16px 0; }"
        "h2 { color: #c8d8e8; font-size: 18px; font-weight: 700; margin: 18px 0 8px 0; }"
        "h3 { color: #8898a8; font-size: 14px; font-weight: 700; margin: 14px 0 6px 0; }"
        "h4 { color: #8898a8; font-size: 12px; font-weight: 700; margin: 12px 0 4px 0; }"
        "p { color: #c8d8e8; font-size: 12px; line-height: 1.5; margin: 0 0 10px 0; }"
        "ul, ol { margin: 0 0 12px 20px; }"
        "li { color: #c8d8e8; font-size: 12px; line-height: 1.55; margin: 3px 0; }"
        "ol li::marker { color: #00b4d8; font-weight: 700; }"
        "ul li::marker { color: #40c060; }"
        "strong { color: #dfeaf3; font-weight: 700; }"
        "em { color: #8898a8; font-style: italic; }"
        "code { color: #d8e4ee; background-color: #152230; }"
        "a { color: #00b4d8; text-decoration: none; }");
    m_browser->setMarkdown(loadMarkdown(resourcePath));
    m_browser->setStyleSheet(
        "QTextBrowser {"
        "  background: #0f0f1a;"
        "  color: #d8e4ee;"
        "  border: none;"
        "  padding: 10px;"
        "  selection-background-color: #245a7a;"
        "  font-size: 14px;"
        "  line-height: 1.5;"
        "}"
        "QScrollBar:vertical {"
        "  background: #0a0a14;"
        "  width: 10px;"
        "  margin: 8px 2px 8px 2px;"
        "}"
        "QScrollBar::handle:vertical {"
        "  background: #304050;"
        "  min-height: 28px;"
        "  border-radius: 5px;"
        "}"
        "QScrollBar::handle:vertical:hover {"
        "  background: #3f5870;"
        "}"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {"
        "  height: 0px;"
        "}"
        "QScrollBar:horizontal {"
        "  background: #0a0a14;"
        "  height: 10px;"
        "  margin: 2px 8px 2px 8px;"
        "}"
        "QScrollBar::handle:horizontal {"
        "  background: #304050;"
        "  min-width: 28px;"
        "  border-radius: 5px;"
        "}"
        "QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal {"
        "  width: 0px;"
        "}");
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

    auto* footer = new QWidget(this);
    footer->setStyleSheet("background: #0a0a14;");
    auto* footerLayout = new QHBoxLayout(footer);
    footerLayout->setContentsMargins(16, 12, 16, 16);

    auto* hint = new QLabel(
        "Tip: The Help menu keeps each guide separate so you can reopen just the topic you need.",
        footer);
    hint->setWordWrap(true);
    hint->setStyleSheet("color: #7f93a7; font-size: 11px;");
    footerLayout->addWidget(hint, 1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, footer);
    auto* closeButton = buttons->button(QDialogButtonBox::Close);
    closeButton->setCursor(Qt::PointingHandCursor);
    closeButton->setAutoDefault(false);
    closeButton->setDefault(false);
    closeButton->setStyleSheet(
        "QPushButton {"
        "  background: #00b4d8;"
        "  color: #0f0f1a;"
        "  font-weight: 700;"
        "  border: none;"
        "  border-radius: 16px;"
        "  min-width: 96px;"
        "  min-height: 32px;"
        "  padding: 0 18px;"
        "}"
        "QPushButton:hover { background: #18c8ea; }");
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
    footerLayout->addWidget(buttons, 0, Qt::AlignRight);

    layout->addWidget(footer);

    setStyleSheet("HelpDialog { background: #0f0f1a; }");
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
            "MasterSDR could not open this bundled help topic.\n\n"
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
        m_findStatus->setStyleSheet(noMatch
            ? "color: #e07070; font-size: 11px; font-weight: 700;"
            : "color: #7f93a7; font-size: 11px;");
    }

    if (m_findEdit)
        m_findEdit->setStyleSheet(noMatch ? kFindEditNoMatchStyle : kFindEditStyle);
}

} // namespace MasterSDR
