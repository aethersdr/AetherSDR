#include "SupportDialog.h"
#include "core/AppSettings.h"
#include "core/AudioEngine.h"
#include "core/LogManager.h"
#include "core/SupportBundle.h"
#include "models/RadioModel.h"
#include "core/RadioConnection.h"

#include <QCheckBox>
#include <QCursor>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QMessageBox>
#include <QFileInfo>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QStringList>
#include <QUrl>
#include <QUrlQuery>
#include <QClipboard>
#include <QApplication>
#include <QVBoxLayout>

namespace MasterSDR {

SupportDialog::SupportDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle("Support & Diagnostics");
    setMinimumSize(600, 520);
    resize(680, 600);
    buildUI();
    syncCheckboxes();
    refreshLog();
}

void SupportDialog::buildUI()
{
    auto* layout = new QVBoxLayout(this);
    layout->setSpacing(8);

    // ── Diagnostic Logging group ──────────────────────────────────────────
    auto* logGroup = new QGroupBox("Diagnostic Logging");
    auto* gridLayout = new QGridLayout(logGroup);
    gridLayout->setSpacing(4);

    auto& mgr = LogManager::instance();
    const auto cats = mgr.categories();
    int col = 0, row = 0;
    constexpr int COLS = 3;

    for (const auto& cat : cats) {
        auto* cb = new QCheckBox(cat.label);
        cb->setToolTip(cat.description);
        cb->setChecked(cat.enabled);
        cb->setStyleSheet("QCheckBox { color: #c8d8e8; }");
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
    enableAllBtn->setFixedHeight(24);
    disableAllBtn->setFixedHeight(24);
    connect(enableAllBtn, &QPushButton::clicked, this, &SupportDialog::enableAll);
    connect(disableAllBtn, &QPushButton::clicked, this, &SupportDialog::disableAll);
    btnRow->addWidget(enableAllBtn);
    btnRow->addWidget(disableAllBtn);
    btnRow->addStretch();

    auto* groupLayout = new QVBoxLayout;
    groupLayout->addLayout(gridLayout);
    groupLayout->addLayout(btnRow);
    logGroup->setLayout(groupLayout);
    layout->addWidget(logGroup);

    // ── Log file info ─────────────────────────────────────────────────────
    auto* fileRow = new QHBoxLayout;
    auto* pathLabel = new QLabel(QString("Log: <code>%1</code>").arg(
        LogManager::instance().logFilePath()));
    pathLabel->setTextFormat(Qt::RichText);
    pathLabel->setStyleSheet("color: #8898a8; font-size: 11px;");
    m_sizeLabel = new QLabel;
    m_sizeLabel->setStyleSheet("color: #8898a8; font-size: 11px;");
    fileRow->addWidget(pathLabel);
    fileRow->addStretch();
    fileRow->addWidget(m_sizeLabel);
    layout->addLayout(fileRow);

    // ── Log viewer ────────────────────────────────────────────────────────
    m_logViewer = new QPlainTextEdit;
    m_logViewer->setReadOnly(true);
    m_logViewer->setMaximumBlockCount(2000);
    m_logViewer->setStyleSheet(
        "QPlainTextEdit {"
        "  background: #0a0a14;"
        "  color: #a0b0c0;"
        "  font-family: monospace;"
        "  font-size: 11px;"
        "  border: 1px solid #203040;"
        "}");
    layout->addWidget(m_logViewer, 1);  // stretch

    // ── Log action buttons ────────────────────────────────────────────────
    auto* actionRow = new QHBoxLayout;
    auto* refreshBtn = new QPushButton("Refresh");
    auto* clearBtn = new QPushButton("Clear Log");
    auto* openBtn = new QPushButton("Open Log Folder");
    auto* resetBtn = new QPushButton("Reset Settings");
    refreshBtn->setFixedHeight(26);
    clearBtn->setFixedHeight(26);
    openBtn->setFixedHeight(26);
    resetBtn->setFixedHeight(26);
    resetBtn->setToolTip("Delete MasterSDR's local settings and NR2 wisdom cache. Radio settings stay on the radio.");
    connect(refreshBtn, &QPushButton::clicked, this, &SupportDialog::refreshLog);
    connect(clearBtn, &QPushButton::clicked, this, &SupportDialog::clearLog);
    connect(openBtn, &QPushButton::clicked, this, &SupportDialog::openLogFolder);
    connect(resetBtn, &QPushButton::clicked, this, &SupportDialog::resetSettings);
    auto* sendBtn = new QPushButton("File an Issue");
    sendBtn->setFixedHeight(26);
    sendBtn->setStyleSheet("QPushButton { background: #00607a; color: #e0f0ff; font-weight: bold; }");
    connect(sendBtn, &QPushButton::clicked, this, [this]() {
        // Create support bundle
        SupportBundle::RadioInfo radio;
        if (m_radioModel && m_radioModel->isConnected()) {
            radio.model = m_radioModel->model();
            radio.serial = m_radioModel->serial();
            radio.firmware = m_radioModel->version();
            radio.callsign = m_radioModel->callsign();
            radio.ip = m_radioModel->radioAddress().toString();
            radio.connected = true;
        }
        QString bundlePath = SupportBundle::createBundle(radio);
        if (bundlePath.isEmpty()) {
            QMessageBox::warning(this, "Error",
                "Failed to create support bundle.");
            return;
        }

        // Collect system info
        QString version = QCoreApplication::applicationVersion();
        QString qt = qVersion();
        QString os;
#if defined(Q_OS_MAC)
        os = "macOS";
#elif defined(Q_OS_WIN)
        os = "Windows";
#else
        os = "Linux";
#endif
        QString radioInfo = (m_radioModel && m_radioModel->isConnected())
            ? QString("%1 fw %2").arg(m_radioModel->model(), m_radioModel->version())
            : "not connected";

        // Diagnostic prompt for AI
        static const QString kPromptTemplate =
            "I'm experiencing an issue with MasterSDR, a Linux/macOS/Windows SDR client\n"
            "for FlexRadio transceivers (https://github.com/MasterSDR/MasterSDR).\n\n"
            "My system:\n"
            "- MasterSDR version: %1\n"
            "- Qt version: %2\n"
            "- OS: %3\n"
            "- Radio: %4\n\n"
            "Before writing the bug report, please read the MasterSDR project context at\n"
            "https://raw.githubusercontent.com/MasterSDR/MasterSDR/main/CLAUDE.md\n"
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

        QMessageBox dlg(this);
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
            QUrl url("https://github.com/MasterSDR/MasterSDR/issues/new");
            QUrlQuery query;
            query.addQueryItem("labels", "bug");
            url.setQuery(query);
            QDesktopServices::openUrl(url);

            // Open support bundle folder for drag-and-drop
            QFileInfo fi(bundlePath);
            QDesktopServices::openUrl(QUrl::fromLocalFile(fi.absolutePath()));

            QMessageBox::information(this, "Submit Bug Report",
                QString("Your browser and support folder have been opened.\n\n"
                        "1. Paste the AI's bug report into the GitHub form\n"
                        "2. Drag and drop the support bundle: %1")
                    .arg(fi.fileName()));
        }

        if (openedLLM) {
            QMessageBox::information(this, "Prompt Copied",
                "The diagnostic prompt has been copied to your clipboard.\n\n"
                "Paste it into the AI, describe your issue, then come back\n"
                "and click \"Submit Bug Report\" to file it on GitHub.");
        }
    });
    actionRow->addWidget(refreshBtn);
    actionRow->addWidget(clearBtn);
    actionRow->addWidget(openBtn);
    actionRow->addWidget(resetBtn);
    actionRow->addStretch();
    actionRow->addWidget(sendBtn);
    layout->addLayout(actionRow);

    // ── Instructions ──────────────────────────────────────────────────────
    auto* instructions = new QLabel(
        "<p style='color:#c8d8e8; font-size: 13px;'>"
        "To report an issue:<br>"
        "1. Enable logging for the relevant module(s) above<br>"
        "2. <b>Restart MasterSDR</b> (logging changes take effect on next launch)<br>"
        "3. Reproduce the problem<br>"
        "4. Click <b>File an Issue</b> and drag your log file into the GitHub form</p>");
    instructions->setTextFormat(Qt::RichText);
    layout->addWidget(instructions);

    // ── Close button ──────────────────────────────────────────────────────
    auto* closeRow = new QHBoxLayout;
    closeRow->addStretch();
    auto* closeBtn = new QPushButton("Close");
    closeBtn->setFixedHeight(28);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);
    closeRow->addWidget(closeBtn);
    layout->addLayout(closeRow);
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
    QFileInfo fi(LogManager::instance().logFilePath());
    QDesktopServices::openUrl(QUrl::fromLocalFile(fi.absolutePath()));
}

void SupportDialog::resetSettings()
{
    QStringList resetPaths = {
        AppSettings::instance().filePath(),
        AudioEngine::wisdomFilePath()
    };
#ifdef Q_OS_MAC
    resetPaths << (QDir::homePath() + "/Library/Preferences/com.mastersdr.MasterSDR.plist");
#endif

    const QString prompt = QString(
        "This will remove MasterSDR's app-specific settings only.\n"
        "It will not change settings stored on the radio.\n"
        "MasterSDR will close immediately after reset so these files are not recreated.\n\n"
        "Files to remove:\n"
        "%1\n\n"
        "Continue?")
        .arg(QString("- %1").arg(resetPaths.join("\n- ")));

    if (QMessageBox::question(this, "Reset Settings", prompt,
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) {
        return;
    }

    QCoreApplication::instance()->setProperty("MasterSettingsResetInProgress", true);

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

    AppSettings::instance().reset();

    if (failed.isEmpty()) {
        QApplication::quit();
        return;
    }

    QMessageBox::warning(
        this, "Reset Settings",
        QString(
            "Some files could not be removed.\n\n"
            "Removed: %1\n"
            "Failed: %2\n\n"
            "MasterSDR will now close to avoid rewriting settings.")
            .arg(removed.isEmpty() ? "none" : removed.join("\n"),
                 failed.join("\n")));
    QApplication::quit();
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

void SupportDialog::sendToSupport()
{
    setCursor(Qt::WaitCursor);

    auto sys = SupportBundle::collectSystemInfo();
    auto radio = SupportBundle::collectRadioInfo(m_radioModel);
    QString bundlePath = SupportBundle::createBundle(radio);

    setCursor(Qt::ArrowCursor);

    if (bundlePath.isEmpty()) {
        QMessageBox::warning(this, "Support Bundle",
            "Failed to create support bundle.\n"
            "Check that the log directory is writable.");
        return;
    }

    QMessageBox::information(this, "Support Bundle Created",
        QString("Support bundle saved to:\n%1\n\n"
                "Your email client will now open.\n"
                "Please attach the bundle file and describe the issue.")
            .arg(bundlePath));

    SupportBundle::openEmailClient(bundlePath, sys, radio);
}

QString SupportDialog::formatFileSize(qint64 bytes) const
{
    if (bytes < 1024) return QString("%1 B").arg(bytes);
    if (bytes < 1024 * 1024) return QString("%1 KB").arg(bytes / 1024.0, 0, 'f', 1);
    return QString("%1 MB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 1);
}

} // namespace MasterSDR
