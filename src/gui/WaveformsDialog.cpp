#include "WaveformsDialog.h"
#include "models/FlexWaveformModel.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

namespace AetherSDR {

WaveformsDialog::WaveformsDialog(FlexWaveformModel* model, QWidget* parent)
    : PersistentDialog(tr("Waveforms"), QStringLiteral("WaveformsDialogGeometry"), parent)
    , m_model(model)
{
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

    // ── Wire model signals ────────────────────────────────────────────────────
    connect(m_model, &FlexWaveformModel::wfpStatusChanged,
            this, &WaveformsDialog::refreshStatus);
    connect(m_model, &FlexWaveformModel::waveformsChanged,
            this, &WaveformsDialog::refreshWaveformList);

    refreshStatus();
    refreshWaveformList();
}

void WaveformsDialog::refreshStatus()
{
    const QString powerColor = m_model->wfpPowered() ? QStringLiteral("#00ff88")
                                                      : QStringLiteral("#505050");
    const QString readyColor = m_model->wfpReady()   ? QStringLiteral("#00ff88")
                                                      : QStringLiteral("#505050");
    const QString powerText  = m_model->wfpPowered() ? tr("ON")  : tr("OFF");
    const QString readyText  = m_model->wfpReady()   ? tr("READY") : tr("NOT READY");

    QString ip = m_model->wfpIpAddress();
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

    const QList<FlexWaveformEntry>& waveforms = m_model->waveforms();

    if (waveforms.isEmpty()) {
        auto* placeholder = new QLabel(tr("No waveforms installed"));
        placeholder->setAlignment(Qt::AlignCenter);
        placeholder->setStyleSheet(QStringLiteral("color: #8090a0;"));
        m_listLayout->insertWidget(0, placeholder);
        return;
    }

    for (const FlexWaveformEntry& entry : waveforms) {
        const QString name    = entry.name;
        const bool isContainer = entry.isContainer;

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
        const QString badgeText  = isContainer ? tr("Container") : tr("Waveform");
        auto* typeLabel = new QLabel(
            QStringLiteral("<font color='#8090a0'>[%1]</font>").arg(badgeText));
        typeLabel->setTextFormat(Qt::RichText);
        rowLayout->addWidget(typeLabel);

        // Restart button
        auto* restartBtn = new QPushButton(tr("Restart"));
        restartBtn->setFixedWidth(70);
        connect(restartBtn, &QPushButton::clicked, this, [this, name]() {
            m_model->requestRestart(name);
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
                m_model->requestRemoveContainer(name);
            else
                m_model->requestUninstall(name);
        });
        rowLayout->addWidget(removeBtn);

        m_listLayout->insertWidget(m_listLayout->count() - 1, row);
    }
}

} // namespace AetherSDR
