#include "SwrSweepLicenseDialog.h"
#include "core/AppSettings.h"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

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
    setModal(true);
    setMinimumSize(520, 240);
    setStyleSheet("QDialog { background: #0f0f1a; color: #c8d8e8; }");

    auto* root = new QVBoxLayout(bodyWidget());
    root->setSpacing(14);

    auto* disclaimer = new QLabel(kDisclaimerText);
    disclaimer->setWordWrap(true);
    disclaimer->setTextFormat(Qt::RichText);
    disclaimer->setStyleSheet(
        "QLabel { color: #c8d8e8; font-size: 12px; line-height: 1.4; }");
    root->addWidget(disclaimer);

    m_rememberCheck = new QCheckBox("Remember my answer");
    m_rememberCheck->setStyleSheet(
        "QCheckBox { color: #8aa8c0; font-size: 11px; }"
        "QCheckBox::indicator { width: 14px; height: 14px;"
        " border: 1px solid #406080; border-radius: 2px; background: #0a0a18; }"
        "QCheckBox::indicator:checked { background: #00b4d8; }");
    root->addWidget(m_rememberCheck);

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

    auto* acceptBtn = new QPushButton("I am licensed to use this feature");
    acceptBtn->setDefault(true);
    acceptBtn->setStyleSheet(
        "QPushButton { background: #00b4d8; color: #0f0f1a; font-weight: bold;"
        " border: 1px solid #008ba8; border-radius: 3px;"
        " padding: 6px 16px; font-size: 11px; }"
        "QPushButton:hover { background: #00c8f0; }"
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
