#include "WaveformsDialog.h"
#include "core/ThemeManager.h"
#include "core/WaveformInstaller.h"
#include "models/FlexWaveformModel.h"
#include "models/RadioModel.h"

#include <QFileDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

namespace AetherSDR {

WaveformsDialog::WaveformsDialog(RadioModel* model, QWidget* parent)
    : PersistentDialog(tr("Waveforms"), QStringLiteral("WaveformsDialogGeometry"), parent)
    , m_radioModel(model)
{
    theme::setContainer(this, QStringLiteral("dialog/waveforms"));
    setMinimumSize(440, 200);

    auto* root = new QVBoxLayout(bodyWidget());
    root->setSpacing(8);
    root->setContentsMargins(10, 8, 10, 10);

    // ── WFP status bar ────────────────────────────────────────────────────────
    auto* statusFrame = new QFrame;
    statusFrame->setFrameShape(QFrame::StyledPanel);
    auto* statusRow = new QHBoxLayout(statusFrame);
    statusRow->setContentsMargins(8, 4, 8, 4);

    m_statusLabel = new QLabel;
    m_statusLabel->setTextFormat(Qt::RichText);
    statusRow->addWidget(m_statusLabel);
    statusRow->addStretch();

    m_installBtn = new QPushButton(tr("Install…"));
    m_installBtn->setAccessibleName(tr("Install Docker Waveform"));
    m_installBtn->setFixedWidth(80);
    m_installBtn->setEnabled(false);  // enabled once WFP is powered and ready
    connect(m_installBtn, &QPushButton::clicked, this, &WaveformsDialog::onInstallClicked);
    statusRow->addWidget(m_installBtn);

    root->addWidget(statusFrame);

    // ── Waveform list (scrollable) ────────────────────────────────────────────
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    m_listContainer = new QWidget;
    m_listLayout = new QVBoxLayout(m_listContainer);
    m_listLayout->setSpacing(4);
    m_listLayout->setContentsMargins(0, 0, 0, 0);
    m_listLayout->addStretch();

    scroll->setWidget(m_listContainer);
    root->addWidget(scroll, 1);

    // ── Wire model + theme signals ────────────────────────────────────────────
    FlexWaveformModel& wfModel = m_radioModel->flexWaveformModel();
    connect(&wfModel, &FlexWaveformModel::wfpStatusChanged,
            this, &WaveformsDialog::refreshStatus);
    connect(&wfModel, &FlexWaveformModel::wfpStatusChanged,
            this, &WaveformsDialog::updateInstallButtonState);
    connect(&wfModel, &FlexWaveformModel::waveformsChanged,
            this, &WaveformsDialog::refreshWaveformList);
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
            this, &WaveformsDialog::refreshStatus);
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
            this, &WaveformsDialog::refreshWaveformList);

    refreshStatus();
    updateInstallButtonState();
    refreshWaveformList();
}

void WaveformsDialog::updateInstallButtonState()
{
    const FlexWaveformModel& wfModel = m_radioModel->flexWaveformModel();
    const bool wfpUp = wfModel.wfpPowered() && wfModel.wfpReady();
    const bool busy  = m_installer && m_installer->isInstalling();
    m_installBtn->setEnabled(wfpUp && !busy);
}

void WaveformsDialog::onInstallClicked()
{
    const QString path = QFileDialog::getOpenFileName(
        this,
        tr("Select Waveform Image"),
        {},
        tr("Waveform Images (*.tar.gz *.tgz);;All Files (*)"));

    if (path.isEmpty())
        return;

    if (!m_installer)
        m_installer = new WaveformInstaller(m_radioModel, this);

    if (m_installer->isInstalling())
        return;

    m_installBtn->setEnabled(false);

    auto* progress = new QProgressDialog(
        tr("Installing waveform…"), tr("Cancel"), 0, 100, this);
    progress->setWindowModality(Qt::WindowModal);
    progress->setMinimumDuration(0);
    progress->setValue(0);
    ThemeManager::instance().applyStyleSheet(progress,
        QStringLiteral("QProgressBar { text-align: center; "
                       "background: {{color.background.0}}; "
                       "color: {{color.text.primary}}; }"));

    connect(m_installer, &WaveformInstaller::progressChanged,
            progress, [progress](int pct, const QString& msg) {
                progress->setValue(pct);
                progress->setLabelText(msg);
            });

    connect(progress, &QProgressDialog::canceled,
            m_installer, &WaveformInstaller::cancel);

    connect(m_installer, &WaveformInstaller::finished,
            this, [this, progress](bool ok, const QString& msg) {
                progress->close();
                progress->deleteLater();
                updateInstallButtonState();
                if (ok)
                    QMessageBox::information(this, tr("Install Complete"), msg);
                else
                    QMessageBox::warning(this, tr("Install Failed"), msg);
            }, Qt::SingleShotConnection);

    m_installer->install(path);
}

void WaveformsDialog::refreshStatus()
{
    const FlexWaveformModel& wfModel = m_radioModel->flexWaveformModel();
    auto& tm = ThemeManager::instance();

    const QString onColor  = tm.color(this, QStringLiteral("color.accent")).name();
    const QString offColor = tm.color(this, QStringLiteral("color.text.secondary")).name();

    const QString powerColor = wfModel.wfpPowered() ? onColor : offColor;
    const QString readyColor = wfModel.wfpReady()   ? onColor : offColor;
    const QString powerText  = wfModel.wfpPowered() ? tr("ON")    : tr("OFF");
    const QString readyText  = wfModel.wfpReady()   ? tr("READY") : tr("NOT READY");

    QString ip = wfModel.wfpIpAddress();
    if (ip.isEmpty())
        ip = QStringLiteral("--");

    m_statusLabel->setText(
        QStringLiteral("WFP:&nbsp;&nbsp;"
                       "<font color='%1'>&#9679;</font> %2"
                       "&nbsp;&nbsp;&nbsp;"
                       "<font color='%3'>&#9679;</font> %4"
                       "&nbsp;&nbsp;&nbsp;"
                       "IP: %5")
            .arg(powerColor, powerText, readyColor, readyText, ip));
}

void WaveformsDialog::refreshWaveformList()
{
    // Remove all items except the trailing stretch
    while (m_listLayout->count() > 1) {
        QLayoutItem* item = m_listLayout->takeAt(0);
        if (QWidget* w = item->widget())
            w->deleteLater();
        delete item;
    }

    const FlexWaveformModel& wfModel = m_radioModel->flexWaveformModel();
    const QList<FlexWaveformEntry>& waveforms = wfModel.waveforms();

    if (waveforms.isEmpty()) {
        auto* placeholder = new QLabel(tr("No waveforms installed"));
        placeholder->setAlignment(Qt::AlignCenter);
        ThemeManager::instance().applyStyleSheet(placeholder,
            QStringLiteral("QLabel { color: {{color.text.secondary}}; }"));
        m_listLayout->insertWidget(0, placeholder);
        return;
    }

    for (const FlexWaveformEntry& entry : waveforms) {
        const QString name        = entry.name;
        const bool    isContainer = entry.isContainer;

        auto* row = new QFrame;
        row->setFrameShape(QFrame::StyledPanel);
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(8, 4, 8, 4);
        rowLayout->setSpacing(8);

        // Name + version
        auto* nameLabel = new QLabel(
            QStringLiteral("<b>%1</b> %2").arg(name, entry.version));
        nameLabel->setTextFormat(Qt::RichText);
        rowLayout->addWidget(nameLabel, 1);

        // Type badge
        const QString badgeText = isContainer ? tr("Container") : tr("Waveform");
        auto* typeLabel = new QLabel(QStringLiteral("[%1]").arg(badgeText));
        ThemeManager::instance().applyStyleSheet(typeLabel,
            QStringLiteral("QLabel { color: {{color.text.secondary}}; }"));
        rowLayout->addWidget(typeLabel);

        // Restart button
        auto* restartBtn = new QPushButton(tr("Restart"));
        restartBtn->setFixedWidth(70);
        connect(restartBtn, &QPushButton::clicked, this, [this, name]() {
            m_radioModel->flexWaveformModel().requestRestart(name);
        });
        rowLayout->addWidget(restartBtn);

        // Remove / Uninstall button
        const QString removeLabel = isContainer ? tr("Remove") : tr("Uninstall");
        auto* removeBtn = new QPushButton(removeLabel);
        removeBtn->setFixedWidth(70);
        connect(removeBtn, &QPushButton::clicked, this, [this, name, isContainer]() {
            const QString question = isContainer
                ? tr("Remove the Docker container \"%1\" from the radio?").arg(name)
                : tr("Uninstall the waveform \"%1\" from the radio?").arg(name);
            if (QMessageBox::question(this, tr("Confirm"), question) != QMessageBox::Yes)
                return;
            if (isContainer)
                m_radioModel->flexWaveformModel().requestRemoveContainer(name);
            else
                m_radioModel->flexWaveformModel().requestUninstall(name);
        });
        rowLayout->addWidget(removeBtn);

        m_listLayout->insertWidget(m_listLayout->count() - 1, row);
    }
}

} // namespace AetherSDR
