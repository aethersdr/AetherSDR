#include "gui/WfmDeviceDialog.h"
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QLabel>
#include <QListWidget>
#include <QMediaDevices>
#include <QPushButton>
#include <QVBoxLayout>

namespace AetherSDR {

WfmDeviceDialog::WfmDeviceDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Select WFM Audio Output Device"));
    setMinimumWidth(420);

    auto* layout = new QVBoxLayout(this);

    auto* label = new QLabel(
        tr("Choose the audio output device for the WFM demodulator.\n"
           "Select a Virtual Audio Cable (e.g. Hi-Fi Cable Input, BlackHole,\n"
           "PipeWire null-sink) to feed another application."), this);
    label->setWordWrap(true);
    layout->addWidget(label);

    m_list = new QListWidget(this);
    m_list->setAlternatingRowColors(true);
    layout->addWidget(m_list);

    m_rememberCheck = new QCheckBox(tr("Remember this choice"), this);
    m_rememberCheck->setChecked(true);
    layout->addWidget(m_rememberCheck);

    m_buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    m_buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
    layout->addWidget(m_buttons);

    connect(m_list, &QListWidget::itemSelectionChanged, this, [this]() {
        m_buttons->button(QDialogButtonBox::Ok)
            ->setEnabled(!m_list->selectedItems().isEmpty());
    });
    connect(m_list, &QListWidget::itemDoubleClicked,
            this, &QDialog::accept);
    connect(m_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    populate();
}

void WfmDeviceDialog::populate()
{
    m_list->clear();
    m_devices = QMediaDevices::audioOutputs();
    for (const QAudioDevice& dev : m_devices) {
        auto* item = new QListWidgetItem(dev.description(), m_list);
        item->setData(Qt::UserRole, QString::fromUtf8(dev.id()));
    }
    if (m_list->count() > 0)
        m_list->setCurrentRow(0);
}

QString WfmDeviceDialog::selectedDeviceId() const
{
    const auto items = m_list->selectedItems();
    if (items.isEmpty())
        return {};
    return items.first()->data(Qt::UserRole).toString();
}

QString WfmDeviceDialog::selectedDeviceName() const
{
    const auto items = m_list->selectedItems();
    if (items.isEmpty())
        return {};
    return items.first()->text();
}

bool WfmDeviceDialog::rememberChoice() const
{
    return m_rememberCheck->isChecked();
}

} // namespace AetherSDR
