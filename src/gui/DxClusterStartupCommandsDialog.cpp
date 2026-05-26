#include "DxClusterStartupCommandsDialog.h"
#include "core/AppSettings.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include "core/ThemeManager.h"

namespace AetherSDR {

namespace {

constexpr const char* kHeaderText =
    "<b>One command per line</b> — sent to the cluster immediately "
    "after login, every connect (including auto-reconnect).<br>"
    "Examples: <code>SET/NAME John</code>, <code>SET/QTH London</code>, "
    "<code>ACCEPT/SPOT 0 ON HF</code>, <code>SET/SKIMMER CW</code>.<br>"
    "Blank lines are skipped.";

} // namespace

DxClusterStartupCommandsDialog::DxClusterStartupCommandsDialog(
    const QString& title, const QString& appSettingsKey, QWidget* parent)
    // Empty geomKey — modal one-shot, geometry persistence not needed.
    : PersistentDialog(title, /*geomKey*/ QString(), parent)
    , m_key(appSettingsKey)
{
    theme::setContainer(this, QStringLiteral("dialog/dxClusterStartup"));
    setModal(true);
    setMinimumSize(560, 380);
    AetherSDR::ThemeManager::instance().applyStyleSheet(this, "QDialog { background: {{color.background.0}}; color: {{color.text.primary}}; }");

    auto* root = new QVBoxLayout(bodyWidget());
    root->setSpacing(10);

    auto* header = new QLabel(kHeaderText);
    header->setWordWrap(true);
    header->setTextFormat(Qt::RichText);
    AetherSDR::ThemeManager::instance().applyStyleSheet(header, "QLabel { color: {{color.text.secondary}}; font-size: 11px; line-height: 1.4; }");
    root->addWidget(header);

    m_edit = new QPlainTextEdit;
    m_edit->setPlaceholderText(
        "One cluster command per line — e.g. SET/NAME John");
    AetherSDR::ThemeManager::instance().applyStyleSheet(m_edit, "QPlainTextEdit {"
        "  background: {{color.background.0}};"
        "  color: {{color.text.primary}};"
        "  font-family: monospace;"
        "  font-size: 12px;"
        "  border: 1px solid {{color.background.1}};"
        "  padding: 4px;"
        "}");
    m_edit->setPlainText(AppSettings::instance().value(m_key).toString());
    root->addWidget(m_edit, 1);

    auto* btnRow = new QHBoxLayout;
    btnRow->setSpacing(8);
    btnRow->addStretch();

    auto* cancelBtn = new QPushButton("Cancel");
    AetherSDR::ThemeManager::instance().applyStyleSheet(cancelBtn, "QPushButton { background: {{color.background.1}}; color: {{color.text.primary}}; "
        "border: 1px solid {{color.background.2}}; border-radius: 3px;"
        " padding: 6px 16px; font-size: 11px; }"
        "QPushButton:hover { background: {{color.background.1}}; border-color: {{color.accent.dim}}; }");
    connect(cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
    btnRow->addWidget(cancelBtn);

    auto* okBtn = new QPushButton("OK");
    okBtn->setDefault(true);
    AetherSDR::ThemeManager::instance().applyStyleSheet(okBtn, "QPushButton { background: {{color.accent}}; color: {{color.background.0}}; font-weight: bold;"
        " border: 1px solid {{color.accent.dim}}; border-radius: 3px;"
        " padding: 6px 16px; font-size: 11px; }"
        "QPushButton:hover { background: {{color.accent.bright}}; }"
        "QPushButton:default { border: 2px solid #00f0ff; }");
    connect(okBtn, &QPushButton::clicked, this, &QDialog::accept);
    btnRow->addWidget(okBtn);

    root->addLayout(btnRow);
}

void DxClusterStartupCommandsDialog::edit(
    const QString& title, const QString& appSettingsKey, QWidget* parent)
{
    DxClusterStartupCommandsDialog dlg(title, appSettingsKey, parent);
    if (dlg.exec() != QDialog::Accepted) return;
    auto& s = AppSettings::instance();
    s.setValue(appSettingsKey, dlg.m_edit->toPlainText());
    s.save();
}

} // namespace AetherSDR
