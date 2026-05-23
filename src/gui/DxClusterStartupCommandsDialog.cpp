#include "DxClusterStartupCommandsDialog.h"
#include "core/AppSettings.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace MasterSDR {

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
    setModal(true);
    setMinimumSize(560, 380);
    setStyleSheet("QDialog { background: #0f0f1a; color: #c8d8e8; }");

    auto* root = new QVBoxLayout(bodyWidget());
    root->setSpacing(10);

    auto* header = new QLabel(kHeaderText);
    header->setWordWrap(true);
    header->setTextFormat(Qt::RichText);
    header->setStyleSheet(
        "QLabel { color: #8aa8c0; font-size: 11px; line-height: 1.4; }");
    root->addWidget(header);

    m_edit = new QPlainTextEdit;
    m_edit->setPlaceholderText(
        "One cluster command per line — e.g. SET/NAME John");
    m_edit->setStyleSheet(
        "QPlainTextEdit {"
        "  background: #0a0a14;"
        "  color: #c8d8e8;"
        "  font-family: monospace;"
        "  font-size: 12px;"
        "  border: 1px solid #203040;"
        "  padding: 4px;"
        "}");
    m_edit->setPlainText(AppSettings::instance().value(m_key).toString());
    root->addWidget(m_edit, 1);

    auto* btnRow = new QHBoxLayout;
    btnRow->setSpacing(8);
    btnRow->addStretch();

    auto* cancelBtn = new QPushButton("Cancel");
    cancelBtn->setStyleSheet(
        "QPushButton { background: #1a2a3a; color: #c8d8e8; "
        "border: 1px solid #304050; border-radius: 3px;"
        " padding: 6px 16px; font-size: 11px; }"
        "QPushButton:hover { background: #203040; border-color: #0090e0; }");
    connect(cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
    btnRow->addWidget(cancelBtn);

    auto* okBtn = new QPushButton("OK");
    okBtn->setDefault(true);
    okBtn->setStyleSheet(
        "QPushButton { background: #00b4d8; color: #0f0f1a; font-weight: bold;"
        " border: 1px solid #008ba8; border-radius: 3px;"
        " padding: 6px 16px; font-size: 11px; }"
        "QPushButton:hover { background: #00c8f0; }"
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

} // namespace MasterSDR
