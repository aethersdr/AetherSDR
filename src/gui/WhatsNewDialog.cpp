#include "WhatsNewDialog.h"
#include "generated/WhatsNewData.h"
#include "core/VersionNumber.h"
#include "core/AppSettings.h"

#include <QCoreApplication>
#include <QLabel>
#include <QPushButton>
#include <QTextBrowser>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QDesktopServices>

namespace MasterSDR {

WhatsNewDialog::WhatsNewDialog(const QString& lastSeenVersion,
                               const QString& currentVersion,
                               QWidget* parent,
                               bool showUpgrade)
    : QDialog(parent)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
    buildUI(lastSeenVersion, currentVersion, showUpgrade);
}

WhatsNewDialog* WhatsNewDialog::showAll(QWidget* parent)
{
    auto* dlg = new WhatsNewDialog("", QCoreApplication::applicationVersion(), parent);
    dlg->show();
    return dlg;
}

void WhatsNewDialog::buildUI(const QString& lastSeenVersion,
                              const QString& currentVersion,
                              bool showUpgrade)
{
    setWindowTitle("What's New — MasterSDR");
    resize(580, 540);
    setMinimumSize(420, 320);

    auto lastSeen = VersionNumber::parse(lastSeenVersion);
    auto current = VersionNumber::parse(currentVersion);
    bool isWelcome = lastSeen.isNull();

    // Filter entries: show releases where lastSeen < version <= current
    std::vector<ReleaseEntry> visible;
    for (const auto& entry : whatsNewEntries()) {
        auto v = VersionNumber::parse(entry.version);
        if (isWelcome) {
            // First install: only show current version
            if (v == current) visible.push_back(entry);
        } else {
            if (v > lastSeen && v <= current)
                visible.push_back(entry);
        }
    }
    // Cap at 5 most recent to avoid overwhelming the user
    if (visible.size() > 5)
        visible.resize(5);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // Header
    auto* header = new QLabel;
    header->setAlignment(Qt::AlignCenter);
    if (isWelcome) {
        header->setText(QString("<div style='padding: 16px;'>"
            "<span style='color: #00b4d8; font-size: 11px; letter-spacing: 3px;'>"
            "MasterSDR V%1</span><br>"
            "<span style='color: #c8d8e8; font-size: 20px; font-weight: bold;'>"
            "Welcome!</span></div>").arg(currentVersion));
    } else {
        header->setText(QString("<div style='padding: 16px;'>"
            "<span style='color: #00b4d8; font-size: 11px; letter-spacing: 3px;'>"
            "MasterSDR V%1</span><br>"
            "<span style='color: #c8d8e8; font-size: 20px; font-weight: bold;'>"
            "What's New</span></div>").arg(currentVersion));
    }
    header->setStyleSheet("QLabel { background: #0a0a14; }");
    layout->addWidget(header);

    // Lightbulb hint (#485)
    auto* hint = new QLabel(
        "<span style='color: #8090a0; font-size: 11px;'>"
        "Found a bug or have an idea? Click the \xf0\x9f\x92\xa1 button in the title bar.</span>");
    hint->setAlignment(Qt::AlignCenter);
    hint->setContentsMargins(16, 0, 16, 4);
    hint->setStyleSheet("QLabel { background: #0a0a14; }");
    layout->addWidget(hint);

    // Separator
    auto* sep = new QWidget;
    sep->setFixedHeight(1);
    sep->setStyleSheet("background: #203040;");
    layout->addWidget(sep);

    // Content
    auto* browser = new QTextBrowser;
    browser->setOpenExternalLinks(false);
    browser->setReadOnly(true);
    browser->setStyleSheet(
        "QTextBrowser { background: #0f0f1a; color: #c8d8e8; border: none; "
        "padding: 12px; font-size: 12px; }"
        "QScrollBar:vertical { background: #0a0a14; width: 8px; }"
        "QScrollBar::handle:vertical { background: #304050; border-radius: 4px; }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }");
    browser->setHtml(renderHtml(visible, isWelcome));
    layout->addWidget(browser, 1);

    // Footer with button
    auto* footer = new QWidget;
    footer->setStyleSheet("background: #0a0a14;");
    auto* footerLayout = new QVBoxLayout(footer);
    footerLayout->setContentsMargins(16, 12, 16, 16);

    auto* btnRow = new QHBoxLayout;
    btnRow->setSpacing(12);

    auto* gotItBtn = new QPushButton("Got it \xe2\x80\x94 73!");
    gotItBtn->setFixedHeight(36);
    gotItBtn->setCursor(Qt::PointingHandCursor);
    gotItBtn->setStyleSheet(
        "QPushButton { background: #00b4d8; color: #0f0f1a; font-weight: bold; "
        "font-size: 14px; border-radius: 18px; padding: 0 32px; }"
        "QPushButton:hover { background: #00c8f0; }");
    connect(gotItBtn, &QPushButton::clicked, this, &QDialog::close);
    btnRow->addWidget(gotItBtn);

    if (showUpgrade) {
        auto* upgradeBtn = new QPushButton("Upgrade");
        upgradeBtn->setFixedHeight(36);
        upgradeBtn->setCursor(Qt::PointingHandCursor);
        upgradeBtn->setStyleSheet(
            "QPushButton { background: #20a040; color: #ffffff; font-weight: bold; "
            "font-size: 14px; border-radius: 18px; padding: 0 32px; }"
            "QPushButton:hover { background: #28c050; }");
        connect(upgradeBtn, &QPushButton::clicked, this, [this] {
            QDesktopServices::openUrl(QUrl("https://github.com/MasterSDR/MasterSDR/releases/latest"));
            close();
        });
        btnRow->addWidget(upgradeBtn);

        auto* skipBtn = new QPushButton("Skip this version");
        skipBtn->setFixedHeight(36);
        skipBtn->setCursor(Qt::PointingHandCursor);
        skipBtn->setStyleSheet(
            "QPushButton { background: #1a2a3a; color: #8090a0; font-weight: bold; "
            "font-size: 14px; border-radius: 18px; padding: 0 24px; "
            "border: 1px solid #304050; }"
            "QPushButton:hover { background: #203040; color: #c8d8e8; }");
        connect(skipBtn, &QPushButton::clicked, this, [currentVersion, this] {
            auto& s = AppSettings::instance();
            s.setValue("LastSeenVersion", currentVersion);
            s.save();
            close();
        });
        btnRow->addWidget(skipBtn);
    }

    footerLayout->addLayout(btnRow);
    footerLayout->setAlignment(btnRow, Qt::AlignCenter);
    layout->addWidget(footer);

    setStyleSheet("WhatsNewDialog { background: #0f0f1a; }");
}

QString WhatsNewDialog::renderHtml(const std::vector<ReleaseEntry>& entries,
                                    bool isWelcome) const
{
    // Category dot colors
    auto dotColor = [](ChangeCategory cat) -> const char* {
        switch (cat) {
        case ChangeCategory::Feature:        return "#00b4d8";
        case ChangeCategory::BugFix:         return "#e06040";
        case ChangeCategory::Improvement:    return "#40c060";
        case ChangeCategory::Infrastructure: return "#8898a8";
        }
        return "#c8d8e8";
    };

    QString html;
    html += "<style>"
            "body { font-family: sans-serif; color: #c8d8e8; }"
            ".release { margin-bottom: 16px; }"
            ".release-header { color: #00b4d8; font-size: 13px; font-weight: bold; "
            "  margin-bottom: 2px; }"
            ".headline { color: #8898a8; font-style: italic; font-size: 12px; "
            "  margin-bottom: 10px; }"
            ".item { margin-bottom: 10px; padding-left: 4px; }"
            ".item-title { font-weight: bold; font-size: 12px; color: #c8d8e8; }"
            ".item-desc { color: #8898a8; font-size: 11px; margin-top: 2px; }"
            ".release-date { color:#8aa8c0; font-size:11px; margin-top:2px; margin-bottom:4px; }"
            ".dot { font-size: 14px; }"
            "</style>";

    for (const auto& entry : entries) {
        html += "<div class='release'>";

        // Release header
        if (!isWelcome || entries.size() > 1) {
            html += QString("<div class='release-header'>v%1</div>").arg(entry.version);
            if (!entry.date.isEmpty())
                html += QString("<div class='release-date'>%1</div>")
                    .arg(entry.date);
        }

        if (!entry.headline.isEmpty())
            html += QString("<div class='headline'>%1</div>").arg(entry.headline.toHtmlEscaped());

        // Items
        for (const auto& item : entry.items) {
            html += "<div class='item'>";
            html += QString("<span class='dot' style='color:%1;'>&#x25CF;</span> ")
                .arg(dotColor(item.category));
            html += QString("<span class='item-title'>%1</span>").arg(item.title.toHtmlEscaped());
            if (!item.description.isEmpty())
                html += QString("<div class='item-desc'>%1</div>").arg(item.description.toHtmlEscaped());
            html += "</div>";
        }

        html += "</div>";
    }

    if (entries.empty()) {
        html += "<div style='text-align:center; color:#8aa8c0; padding:40px;'>"
                "No new changes to report.</div>";
    }

    return html;
}

} // namespace MasterSDR
