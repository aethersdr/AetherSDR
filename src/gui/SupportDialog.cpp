#include "SupportDialog.h"
#include "DocsLinks.h"
#include "FramelessMessageBox.h"
#include "CanonIndicators.h"
#include "CanonScrollBarStyle.h"
#include "FramelessResizer.h"
#include "ModemChrome.h"
#include "ResetSettingsPrompts.h"
#include "core/AppSettings.h"
#include "core/AudioEngine.h"
#include "core/ExternalFilePath.h"
#include "core/SettingsPaths.h"
#include "core/IssueReport.h"
#include "core/LogManager.h"
#include "core/SupportBundle.h"
#include "models/RadioModel.h"
#include "core/backends/flex/RadioConnection.h"

#include <QCheckBox>
#include <QCursor>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QScrollArea>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QStringList>
#include <QUrl>
#include <QUrlQuery>
#include <QClipboard>
#include <QApplication>
#include <QVBoxLayout>
#include "core/ThemeManager.h"

namespace AetherSDR {

namespace {

// What the shared canon dialog sheet (buttons, the canon check boxes) leaves
// out: the logging group as MQTT Settings has its group boxes; the log pane on
// a nested card; the path, size and hint lines; selection ink; and the log
// pane's right-click menu, a child that the sheet's transparent rule would
// otherwise leave with no panel.
constexpr const char* kSupportRules = R"(
QGroupBox {
    background: {{color.canon.nested}};
    border: 1px solid {{color.canon.line}};
    border-radius: 8px;
    margin-top: 14px;
    padding-top: 6px;
    font-weight: 700;
}
QGroupBox::title {
    subcontrol-origin: margin;
    left: 10px;
    padding: 0 4px;
    color: {{color.canon.cyan}};
}
QLineEdit {
    selection-background-color: {{color.canon.cyan}};
    selection-color: {{color.canon.onAccent}};
}
QScrollArea#supportCategoryScroll,
QScrollArea#supportCategoryScroll > QWidget > QWidget {
    background: transparent;
}
QPlainTextEdit {
    color: {{color.canon.inkSoft}};
    background: {{color.canon.nested}};
    border: 1px solid {{color.canon.line}};
    border-radius: 8px;
    font-family: "SF Mono", "Menlo", "Consolas", monospace;
    font-size: 11px;
    selection-background-color: {{color.canon.control}};
    selection-color: {{color.canon.ink}};
}
QLabel#supportLogCaption,
QLabel#supportLogSize,
QLabel#supportDocsLabel {
    color: {{color.canon.muted}};
    font-size: 11px;
}
QLabel#supportInstructions {
    color: {{color.canon.inkSoft}};
    font-size: 13px;
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

} // namespace

SupportDialog::SupportDialog(QWidget* parent)
    : CanonWindow(QStringLiteral("Support & Diagnostics"), parent)
{
    setObjectName(QStringLiteral("SupportDialog"));
    theme::setContainer(this, QStringLiteral("canon/support"));
    // Size and position persist under the key it used as a PersistentDialog,
    // so saved geometry carries over; it resizes from every edge. Nothing
    // here is a form, so Return clicks no button.
    setGeometryKey(QStringLiteral("SupportDialogGeometry"));
    setReturnClicksDefault(false);
    FramelessResizer::install(this);
    applyCanonSheet(bodyWidget(), [] {
        return ModemChrome::styleSheet(ModemChrome::Scale::Dialog, ModemChrome::Look::Canon)
             + canonScrollBarRules() + QString::fromLatin1(kSupportRules);
    });
    buildUI();
    syncCheckboxes();
    refreshLog();
}

void SupportDialog::buildUI()
{
    auto* layout = new QVBoxLayout(bodyWidget());
    layout->setContentsMargins(14, 12, 14, 14);
    layout->setSpacing(8);
    layout->addWidget(makeCanonHeader(QStringLiteral("Support & Diagnostics")));

    // ── Diagnostic Logging group ──────────────────────────────────────────
    // The grid is built unparented and installed once, inside groupLayout:
    // parenting it to the group box and then setting groupLayout on the box
    // gave the grid two parents and the box two layouts (Qt warned twice).
    auto* logGroup = new QGroupBox("Diagnostic Logging");
    // The 48-odd categories scroll inside the group: laid out in full they
    // need some 430 px, which with the rest of the window would not fit a
    // 768 px screen, and the old fixed floor crushed the rows instead.
    auto* categoryPage = new QWidget;
    auto* gridLayout = new QGridLayout(categoryPage);
    gridLayout->setContentsMargins(0, 0, 0, 0);
    gridLayout->setSpacing(4);

    auto& mgr = LogManager::instance();
    const auto cats = mgr.categories();
    int col = 0, row = 0;
    constexpr int COLS = 3;

    for (const auto& cat : cats) {
        auto* cb = new QCheckBox(cat.label);
        cb->setToolTip(cat.description);
        cb->setChecked(cat.enabled);
        gridLayout->addWidget(cb, row, col);

        connect(cb, &QCheckBox::toggled, this, [id = cat.id](bool on) {
            LogManager::instance().setEnabled(id, on);
        });

        m_checkboxes[cat.id] = cb;
        if (++col >= COLS) { col = 0; ++row; }
    }

    auto* btnRow = new QHBoxLayout;
    auto* enableAllBtn = new QPushButton("Enable All");
    auto* disableAllBtn = new QPushButton("Disable All");
    connect(enableAllBtn, &QPushButton::clicked, this, &SupportDialog::enableAll);
    connect(disableAllBtn, &QPushButton::clicked, this, &SupportDialog::disableAll);
    btnRow->addWidget(enableAllBtn);
    btnRow->addWidget(disableAllBtn);
    btnRow->addStretch();

    auto* categoryScroll = new QScrollArea;
    categoryScroll->setObjectName(QStringLiteral("supportCategoryScroll"));
    categoryScroll->setAccessibleName(QStringLiteral("Diagnostic logging categories"));
    categoryScroll->setFrameShape(QFrame::NoFrame);
    categoryScroll->setWidgetResizable(true);
    categoryScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    categoryScroll->setWidget(categoryPage);

    auto* groupLayout = new QVBoxLayout(logGroup);
    groupLayout->addWidget(categoryScroll, 1);
    groupLayout->addLayout(btnRow);
    layout->addWidget(logGroup, 1);

    // Measure after parenting and polishing: the inherited canon sheet changes
    // the checkbox font, indicator and scrollbar widths. All three columns
    // must fit while about six rows show at the floor.
    ensurePolished();
    const int rowHeight = categoryPage->findChild<QCheckBox*>()
        ? categoryPage->findChild<QCheckBox*>()->sizeHint().height() + gridLayout->verticalSpacing()
        : 22;
    categoryScroll->setMinimumWidth(categoryPage->minimumSizeHint().width()
                                    + categoryScroll->verticalScrollBar()->sizeHint().width());
    categoryScroll->setMinimumHeight(6 * rowHeight);

    // ── Log file info ─────────────────────────────────────────────────────
    auto* fileRow = new QHBoxLayout;
    // Plain text, so a path is never read as markup, and selectable so it can
    // be copied into a report.
    // A read-only field: the whole path can be selected and copied into a
    // report, and a long one scrolls instead of widening the window.
    const QString logPath = QDir::toNativeSeparators(
        ExternalFilePath::resolve(LogManager::instance().logFilePath()));
    auto* pathCaption = new QLabel(QStringLiteral("Log:"));
    pathCaption->setObjectName(QStringLiteral("supportLogCaption"));
    auto* pathField = new QLineEdit(logPath);
    pathField->setObjectName(QStringLiteral("supportLogPath"));
    pathField->setAccessibleName(QStringLiteral("Log file path"));
    pathField->setReadOnly(true);
    pathField->setToolTip(logPath);
    pathField->setCursorPosition(0);
    pathCaption->setBuddy(pathField);
    m_sizeLabel = new QLabel;
    m_sizeLabel->setObjectName(QStringLiteral("supportLogSize"));
    fileRow->addWidget(pathCaption);
    fileRow->addWidget(pathField, 1);
    fileRow->addWidget(m_sizeLabel);
    layout->addLayout(fileRow);

    // ── Log viewer ────────────────────────────────────────────────────────
    m_logViewer = new QPlainTextEdit;
    m_logViewer->setReadOnly(true);
    m_logViewer->setMaximumBlockCount(2000);
    m_logViewer->setObjectName(QStringLiteral("supportLogViewer"));
    m_logViewer->setAccessibleName(QStringLiteral("Log file tail"));
    m_logViewer->setMinimumHeight(90);
    layout->addWidget(m_logViewer, 1);  // stretch

    // ── Log action buttons ────────────────────────────────────────────────
    auto* actionRow = new QHBoxLayout;
    auto* refreshBtn = new QPushButton("Refresh");
    auto* clearBtn = new QPushButton("Clear Log");
    auto* openBtn = new QPushButton("Open Log Folder");
    connect(refreshBtn, &QPushButton::clicked, this, &SupportDialog::refreshLog);
    connect(clearBtn, &QPushButton::clicked, this, &SupportDialog::clearLog);
    connect(openBtn, &QPushButton::clicked, this, &SupportDialog::openLogFolder);
    actionRow->addWidget(refreshBtn);
    actionRow->addWidget(clearBtn);
    actionRow->addWidget(openBtn);
    actionRow->addStretch();
    layout->addLayout(actionRow);

    // ── Online help ───────────────────────────────────────────────────────
    // Self-help before a report: the troubleshooting symptom index and the
    // Log Analyzer, which checks a log or support bundle in the browser.
    auto* docsRow = new QHBoxLayout;
    auto* docsLabel = new QLabel("Before you report:");
    docsLabel->setObjectName(QStringLiteral("supportDocsLabel"));
    docsRow->addWidget(docsLabel);
    const auto addDocsButton = [this, docsRow](const QString& text,
                                               const QString& objectName,
                                               const QString& accessibleName,
                                               const QString& docsUrl) {
        auto* btn = new QPushButton(text);
        btn->setObjectName(objectName);
        btn->setAccessibleName(accessibleName);
        btn->setToolTip(QStringLiteral("Opens %1 in your browser").arg(docsUrl));
        connect(btn, &QPushButton::clicked, this, [docsUrl] { DocsLinks::open(docsUrl); });
        docsRow->addWidget(btn);
    };
    addDocsButton(
        QStringLiteral("Troubleshooting Guide"),
        QStringLiteral("supportTroubleshootingGuideButton"),
        QStringLiteral("Open the troubleshooting guide in your browser"),
        DocsLinks::troubleshooting());
    addDocsButton(
        QStringLiteral("Log Analyzer"),
        QStringLiteral("supportLogAnalyzerButton"),
        QStringLiteral("Open the Log Analyzer in your browser"),
        DocsLinks::logAnalyzer());
    docsRow->addStretch();
    layout->addLayout(docsRow);

    // ── Instructions ──────────────────────────────────────────────────────
    // "File an Issue" lives on Help and "Reset Settings" on Settings;
    // point the user there rather than duplicating the buttons here.
    auto* instructions = new QLabel(
        "<p>"
        "To report an issue:<br>"
        "1. Enable logging for the relevant module(s) above<br>"
        "2. <b>Restart AetherSDR</b> (logging changes take effect on next launch)<br>"
        "3. Reproduce the problem<br>"
        "4. Choose <b>Help \xE2\x86\x92 File an Issue\xE2\x80\xA6</b> and drag your log file into the GitHub form</p>");
    instructions->setObjectName(QStringLiteral("supportInstructions"));
    instructions->setTextFormat(Qt::RichText);
    layout->addWidget(instructions);

    // 40 px taller than before for the canon header, and never smaller than
    // the layout needs.
    resize(680, 640);
    setMinimumSize(QSize(600, 560).expandedTo(minimumSizeHint()));
}

void SupportDialog::syncCheckboxes()
{
    auto& mgr = LogManager::instance();
    for (auto it = m_checkboxes.begin(); it != m_checkboxes.end(); ++it) {
        QSignalBlocker b(it.value());
        it.value()->setChecked(mgr.isEnabled(it.key()));
    }
}

void SupportDialog::refreshLog()
{
    auto& mgr = LogManager::instance();
    m_sizeLabel->setText(formatFileSize(mgr.logFileSize()));

    QFile f(mgr.logFilePath());
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        m_logViewer->setPlainText("(unable to open log file)");
        return;
    }

    // Read last 200KB max to keep UI responsive
    constexpr qint64 MAX_READ = 200 * 1024;
    if (f.size() > MAX_READ)
        f.seek(f.size() - MAX_READ);

    m_logViewer->setPlainText(QString::fromUtf8(f.readAll()));
    f.close();

    // Scroll to bottom
    auto* sb = m_logViewer->verticalScrollBar();
    sb->setValue(sb->maximum());
}

void SupportDialog::clearLog()
{
    LogManager::instance().clearLog();
    refreshLog();
}

void SupportDialog::openLogFolder()
{
    QFileInfo fi(ExternalFilePath::resolve(LogManager::instance().logFilePath()));
    QDesktopServices::openUrl(QUrl::fromLocalFile(fi.absolutePath()));
}

void SupportDialog::resetSettings(QWidget* parent)
{
    // The settings database + every sidecar and recovery artifact a reset
    // must not resurrect (RFC #4603): WAL/SHM, the frozen pre-SQLite XML
    // snapshot with its .bak/.tmp/.corrupt siblings, rolling backups and
    // quarantined copies. A final pre-reset backup is written first, into a
    // location the purge does NOT touch, so a reset is recoverable.
    const QString dbPath = AppSettings::instance().filePath();
    const QString xmlPath = AppSettings::instance().legacyXmlPath();
    QStringList resetPaths = {
        dbPath,
        dbPath + "-wal",
        dbPath + "-shm",
        xmlPath,
        xmlPath + ".bak",
        xmlPath + ".tmp",
        xmlPath + ".corrupt",
        AudioEngine::wisdomFilePath()
    };
#ifdef Q_OS_MACOS
    resetPaths << (QDir::homePath() + "/Library/Preferences/com.aethersdr.AetherSDR.plist");
#endif

    QStringList promptPaths;
    for (const QString& path : resetPaths) {
        if (QFileInfo::exists(path))
            promptPaths << path;
    }

    if (!ResetSettingsPrompts::confirm(parent, AetherSDR::SettingsPaths::backupsDir(),
                                      promptPaths)) {
        return;
    }

    QCoreApplication::instance()->setProperty("AetherSettingsResetInProgress", true);

    // Backup, checkpoint, and close every database connection BEFORE deleting:
    // Windows holds the files locked while a connection is open, and the WAL
    // must be folded into the main file for the backup to be complete.
    const QString backupPath = AppSettings::instance().shutdownForReset();
    if (backupPath.isEmpty()) {
        qWarning() << "SupportDialog: pre-reset settings backup could not be"
                      " written — continuing with the reset";
    }

    QStringList removed;
    QStringList failed;

    const auto removeIfPresent = [&](const QString& path) {
        if (!QFileInfo::exists(path))
            return;
        if (QFile::remove(path))
            removed << path;
        else
            failed << path;
    };

    for (const QString& path : resetPaths)
        removeIfPresent(path);

    // Rolling backups and quarantined corrupt stores go too — but never the
    // pre-reset backups, which live in the same directory under a distinct
    // suffix and are the recovery path from this very operation.
    {
        QDir backups(AetherSDR::SettingsPaths::backupsDir());
        const QStringList stale = backups.entryList(
            {"*-auto.db", "*-postmigration.db"}, QDir::Files);
        for (const QString& name : stale)
            removeIfPresent(backups.filePath(name));
        QDir quarantine(AetherSDR::SettingsPaths::quarantineDir());
        const QStringList quarantined = quarantine.entryList(QDir::Files);
        for (const QString& name : quarantined)
            removeIfPresent(quarantine.filePath(name));
    }

    AppSettings::instance().reset();

    if (failed.isEmpty()) {
        QApplication::quit();
        return;
    }

    ResetSettingsPrompts::showFailures(parent, removed, failed);
    QApplication::quit();
}

void SupportDialog::fileIssue(QWidget* parent, RadioModel* radioModel)
{
    // Create support bundle
    SupportBundle::RadioInfo radio;
    if (radioModel && radioModel->isConnected()) {
        radio.model = radioModel->model();
        radio.serial = radioModel->serial();
        radio.firmware = radioModel->version();
        radio.protocolVersion = radioModel->protocolVersion();
        radio.callsign = radioModel->callsign();
        radio.ip = radioModel->radioAddress().toString();
        radio.connected = true;
        radio.hl2 = SupportBundle::collectHl2Info(radioModel);
    }
    QString bundlePath = SupportBundle::createBundle(radio);
    if (bundlePath.isEmpty()) {
        FramelessMessageBox::warning(parent, "Error",
            "Failed to create support bundle.");
        return;
    }

    // Collect system info
    QString version = QCoreApplication::applicationVersion();
    QString qt = qVersion();
    QString os;
#if defined(Q_OS_MACOS)
    os = "macOS";
#elif defined(Q_OS_WIN)
    os = "Windows";
#else
    os = "Linux";
#endif
    QString radioInfo = (radioModel && radioModel->isConnected())
        ? QString("%1 fw %2").arg(radioModel->model(), radioModel->version())
        : "not connected";

    // Diagnostic prompt for AI
    static const QString kPromptTemplate =
        "I'm experiencing an issue with AetherSDR, a Linux/macOS/Windows SDR client\n"
        "for FlexRadio transceivers (https://github.com/aethersdr/AetherSDR).\n\n"
        "My system:\n"
        "- AetherSDR version: %1\n"
        "- Qt version: %2\n"
        "- OS: %3\n"
        "- Radio: %4\n\n"
        "Before writing the bug report, please read the AetherSDR project context at\n"
        "https://raw.githubusercontent.com/aethersdr/AetherSDR/main/AGENTS.md\n"
        "for architecture overview, data flow, protocol details, and known issues.\n\n"
        "Based on my description below, write a complete GitHub bug report.\n"
        "Do NOT ask me follow-up questions — just write the best report you can\n"
        "from what I've provided. Output it in one response, ready to paste.\n"
        "Use GitHub-flavored Markdown formatting (headers, code blocks, bullet points).\n\n"
        "Format it as:\n"
        "### Title: (short, descriptive)\n"
        "### What happened?\n"
        "(expand on my description with clear, specific language)\n"
        "### What did you expect?\n"
        "(infer the expected behavior from context)\n"
        "### Steps to reproduce\n"
        "(numbered steps based on my description)\n"
        "### Radio model & firmware\n"
        "(pre-filled above)\n"
        "### OS & version\n"
        "(pre-filled above)\n"
        "### Developer Notes\n"
        "(After reviewing the codebase, suggest which source files and functions\n"
        "are most likely involved, what logging categories to enable in\n"
        "Help → Support to capture diagnostic data, and any potential root causes\n"
        "based on the code architecture. Reference specific file paths and line\n"
        "numbers where possible.)\n\n"
        "Here is my issue:\n\n"
        "[Describe what went wrong — for example: \"The waterfall freezes after\n"
        "about 10 minutes\" or \"Audio cuts out when I switch bands\"]";

    QString prompt = kPromptTemplate.arg(version, qt, os, radioInfo);
    QApplication::clipboard()->setText(prompt);

    FramelessMessageBox dlg(parent);
    dlg.setWindowTitle("AI-Assisted Bug Report");
    dlg.setIcon(QMessageBox::Information);
    dlg.setText(
        "<h3>Get Help Describing Your Issue</h3>"
        "<p>Use any AI assistant to help you write a clear bug report. "
        "A diagnostic prompt with your system info has been copied to your clipboard.</p>"
        "<ol>"
        "<li><b>Choose your AI</b> — click one of the buttons below</li>"
        "<li><b>Paste the prompt</b> — your system info is pre-filled</li>"
        "<li><b>Describe what happened</b> — the AI will help you structure it</li>"
        "<li><b>Copy the AI's output</b> and click <b>Submit Bug Report</b></li>"
        "<li><b>Drag your support bundle</b> into the GitHub issue form</li>"
        "</ol>"
        "<p style='color:#8aa8c0; font-size:11px;'>"
        "Your support bundle (logs + settings) is ready to attach.</p>");

    auto* claudeBtn   = dlg.addButton("Claude", QMessageBox::ActionRole);
    auto* chatgptBtn  = dlg.addButton("ChatGPT", QMessageBox::ActionRole);
    auto* geminiBtn   = dlg.addButton("Gemini", QMessageBox::ActionRole);
    auto* grokBtn     = dlg.addButton("Grok", QMessageBox::ActionRole);
    auto* perplexBtn  = dlg.addButton("Perplexity", QMessageBox::ActionRole);
    auto* issueBtn    = dlg.addButton("Submit Bug Report", QMessageBox::ActionRole);
    dlg.addButton(QMessageBox::Close);

    dlg.exec();

    auto* clicked = dlg.clickedButton();
    bool openedLLM = false;
    if (clicked == claudeBtn) {
        QDesktopServices::openUrl(QUrl("https://claude.ai/new"));
        openedLLM = true;
    } else if (clicked == chatgptBtn) {
        QDesktopServices::openUrl(QUrl("https://chat.openai.com/"));
        openedLLM = true;
    } else if (clicked == geminiBtn) {
        QDesktopServices::openUrl(QUrl("https://gemini.google.com/"));
        openedLLM = true;
    } else if (clicked == grokBtn) {
        QDesktopServices::openUrl(QUrl("https://grok.x.ai/"));
        openedLLM = true;
    } else if (clicked == perplexBtn) {
        QDesktopServices::openUrl(QUrl("https://www.perplexity.ai/"));
        openedLLM = true;
    } else if (clicked == issueBtn) {
        // Pre-fill the issues/new form with the snapshot + a redacted recent-
        // log tail (#3705).  The log tail is already secret-redacted on disk
        // and re-scrubbed at the render boundary in buildIssueReport().
        auto sys = SupportBundle::collectSystemInfo();
        const QString logTail = SupportBundle::recentLogTail(kIssueLogTailLines);
        const QString fullBody = buildIssueReport(sys, radio, logTail);

        const QString title = radio.connected
            ? QString("[bug] %1 v%2 \xE2\x80\x94 ").arg(radio.model, sys.aetherVersion)
            : QString("[bug] AetherSDR v%1 \xE2\x80\x94 ").arg(sys.aetherVersion);

        const auto makeUrl = [&title](const QString& body) {
            QUrl url("https://github.com/aethersdr/AetherSDR/issues/new");
            QUrlQuery query;
            query.addQueryItem("labels", "bug");
            query.addQueryItem("title", title);
            query.addQueryItem("body", body);
            url.setQuery(query);
            return url;
        };

        // First try the full body; if the encoded URL exceeds the ceiling,
        // re-render without the log block and hand the full report to the
        // clipboard so nothing is lost.
        QUrl url = makeUrl(fullBody);
        bool logTrimmed = false;
        if (url.toEncoded().size() > kIssueUrlMaxBytes) {
            url = makeUrl(buildIssueReport(sys, radio, QString()));
            QApplication::clipboard()->setText(fullBody);
            logTrimmed = true;
        }
        QDesktopServices::openUrl(url);

        // Open support bundle folder for drag-and-drop
        QFileInfo fi(ExternalFilePath::resolve(bundlePath));
        QDesktopServices::openUrl(QUrl::fromLocalFile(fi.absolutePath()));

        FramelessMessageBox::information(parent, "Submit Bug Report",
            QString("Your browser and support folder have been opened.\n\n"
                    "The issue form is pre-filled with your system + radio "
                    "snapshot%1.\n"
                    "1. Fill in what happened / expected / steps\n"
                    "2. Drag and drop the support bundle: %2")
                .arg(logTrimmed
                         ? " (the recent log was too long for the link \xE2\x80\x94 "
                           "it's on your clipboard; paste it in or just attach "
                           "the bundle)"
                         : " and a redacted recent-log tail",
                     fi.fileName()));
    }

    if (openedLLM) {
        FramelessMessageBox::information(parent, "Prompt Copied",
            "The diagnostic prompt has been copied to your clipboard.\n\n"
            "Paste it into the AI, describe your issue, then come back\n"
            "and click \"Submit Bug Report\" to file it on GitHub.");
    }
}

void SupportDialog::enableAll()
{
    LogManager::instance().setAllEnabled(true);
    syncCheckboxes();
}

void SupportDialog::disableAll()
{
    LogManager::instance().setAllEnabled(false);
    syncCheckboxes();
}

QString SupportDialog::formatFileSize(qint64 bytes) const
{
    if (bytes < 1024) return QString("%1 B").arg(bytes);
    if (bytes < 1024 * 1024) return QString("%1 KB").arg(bytes / 1024.0, 0, 'f', 1);
    return QString("%1 MB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 1);
}

} // namespace AetherSDR
