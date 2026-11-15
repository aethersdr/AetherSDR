#include "SwrSweepLicenseDialog.h"
#include "core/AppSettings.h"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include "core/ThemeManager.h"

namespace AetherSDR {

namespace {

constexpr const char* kSettingsKey = "SwrSweepLicenseConfirmed";

constexpr const char* kDisclaimerText =
    "<b>Operator Responsibility:</b> The Antenna SWR Sweep transmits a "
    "1&nbsp;W tune carrier at multiple frequencies across the current TX "
    "band.  You must ensure that your transmissions do not interfere with "
    "other radio traffic — always verify that the band is clear before "
    "starting, and never run an unattended sweep unless you fully "
    "understand its behavior, failure modes, and risks.  You are "
    "responsible for compliance with your license class and local "
    "regulations.";

} // namespace

SwrSweepLicenseDialog::SwrSweepLicenseDialog(QWidget* parent)
    // Empty geomKey — modal one-shot, no need to persist geometry.
    : PersistentDialog("Antenna SWR Sweep — License Confirmation",
                       /*geomKey*/ QString(), parent)
{
    theme::setContainer(this, QStringLiteral("dialog/swrSweepLicense"));
    setModal(true);
    setMinimumSize(520, 240);
    AetherSDR::ThemeManager::instance().applyStyleSheet(this, "QDialog { background: {{color.background.0}}; color: {{color.text.primary}}; }");

    auto* root = new QVBoxLayout(bodyWidget());
    root->setSpacing(14);

    auto* disclaimer = new QLabel(kDisclaimerText);
    disclaimer->setWordWrap(true);
    disclaimer->setTextFormat(Qt::RichText);
    AetherSDR::ThemeManager::instance().applyStyleSheet(disclaimer, "QLabel { color: {{color.text.primary}}; font-size: 12px; line-height: 1.4; }");
    root->addWidget(disclaimer);

    m_rememberCheck = new QCheckBox("Remember my answer");
    AetherSDR::ThemeManager::instance().applyStyleSheet(m_rememberCheck, "QCheckBox { color: {{color.text.secondary}}; font-size: 11px; }"
        "QCheckBox::indicator { width: 14px; height: 14px;"
        " border: 1px solid #406080; border-radius: 2px; background: {{color.background.0}}; }"
        "QCheckBox::indicator:checked { background: {{color.accent}}; }");
    root->addWidget(m_rememberCheck);

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

    auto* acceptBtn = new QPushButton("I am licensed to use this feature");
    acceptBtn->setDefault(true);
    AetherSDR::ThemeManager::instance().applyStyleSheet(acceptBtn, "QPushButton { background: {{color.accent}}; color: {{color.background.0}}; font-weight: bold;"
        " border: 1px solid {{color.accent.dim}}; border-radius: 3px;"
        " padding: 6px 16px; font-size: 11px; }"
        "QPushButton:hover { background: {{color.accent.bright}}; }"
        "QPushButton:default { border: 2px solid #00f0ff; }");
    connect(acceptBtn, &QPushButton::clicked, this, &QDialog::accept);
    btnRow->addWidget(acceptBtn);

    root->addStretch();
    root->addLayout(btnRow);
}

bool SwrSweepLicenseDialog::confirm(QWidget* parent)
{
    auto& s = AppSettings::instance();
    if (s.value(kSettingsKey, "False").toString() == "True") {
        return true;
    }

    SwrSweepLicenseDialog dlg(parent);
    if (dlg.exec() != QDialog::Accepted) {
        return false;
    }
    if (dlg.m_rememberCheck && dlg.m_rememberCheck->isChecked()) {
        s.setValue(kSettingsKey, "True");
        s.save();
    }
    return true;
}

} // namespace AetherSDR
