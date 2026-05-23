#include "MultiFlexDialog.h"
#include "models/RadioModel.h"

#include <QBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QTableWidgetItem>

namespace AetherSDR {

static const char* kDialogStyle =
    "QDialog { background: #0f0f1a; }"
    "QLabel { color: #c8d8e8; }"
    "QPushButton { background: #1a2a3a; border: 1px solid #304050; "
    "  border-radius: 3px; color: #c8d8e8; padding: 6px 20px; font-weight: bold; }"
    "QPushButton:hover { background: #203040; border-color: #00b4d8; }"
    "QPushButton#disconnectClientButton { background: #3a1a22; border-color: #7a2836; "
    "  color: #ffd6d6; padding: 4px 12px; }"
    "QPushButton#disconnectClientButton:hover { background: #4a202a; border-color: #d84a5f; }"
    "QPushButton#disconnectClientButton:disabled { background: #24242e; border-color: #404050; color: #8a8a94; }"
    "QTableWidget { background: #0f0f1a; color: #c8d8e8; "
    "  gridline-color: #203040; border: 1px solid #304050; }"
    "QTableWidget::item { padding: 4px 8px; }"
    "QHeaderView::section { background: #1a2a3a; color: #8aa8c0; "
    "  border: 1px solid #203040; padding: 6px; font-weight: bold; }";

constexpr int kDisconnectActionColumnWidth = 144;
constexpr int kDisconnectButtonWidth = 128;

MultiFlexDialog::MultiFlexDialog(RadioModel* model, QWidget* parent)
    : PersistentDialog("multiFLEX Dashboard", "MultiFlexDialogGeometry", parent),
      m_model(model)
{
    setMinimumSize(680, 280);
    resize(760, 320);
    setStyleSheet(kDialogStyle);

    auto* root = new QVBoxLayout(bodyWidget());
    root->setSpacing(10);

    // Title
    auto* title = new QLabel("multiFLEX Stations");
    title->setAlignment(Qt::AlignCenter);
    title->setStyleSheet("QLabel { font-size: 16px; font-weight: bold; color: #c8d8e8; }");
    root->addWidget(title);

    // Enable/disable button
    auto* btnRow = new QHBoxLayout;
    btnRow->addStretch();
    m_enableBtn = new QPushButton;
    connect(m_enableBtn, &QPushButton::clicked, this, [this]() {
        m_model->setMultiFlexEnabled(!m_model->multiFlexEnabled());
        refresh();
    });
    btnRow->addWidget(m_enableBtn);
    btnRow->addStretch();
    root->addLayout(btnRow);

    // Client table
    m_table = new QTableWidget;
    m_table->setColumnCount(5);
    m_table->setHorizontalHeaderLabels({"LOCAL PTT", "STATION", "TX ANT", "TX FREQ (MHz)", ""});
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Fixed);
    m_table->setColumnWidth(4, kDisconnectActionColumnWidth);
    m_table->verticalHeader()->setVisible(false);
    m_table->verticalHeader()->setDefaultSectionSize(36);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    root->addWidget(m_table);

    // Local PTT button row
    auto* pttRow = new QHBoxLayout;
    pttRow->addStretch();
    m_pttLabel = new QLabel;
    m_pttLabel->setStyleSheet("QLabel { color: #8aa8c0; }");
    pttRow->addWidget(m_pttLabel);
    m_pttBtn = new QPushButton("Enable");
    connect(m_pttBtn, &QPushButton::clicked, this, [this]() {
        m_model->requestLocalPtt();
    });
    pttRow->addWidget(m_pttBtn);
    pttRow->addStretch();
    root->addLayout(pttRow);

    // Close button
    auto* closeRow = new QHBoxLayout;
    closeRow->addStretch();
    auto* closeBtn = new QPushButton("Close");
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);
    closeRow->addWidget(closeBtn);
    root->addLayout(closeRow);

    // Refresh on client changes or row selection
    connect(m_model, &RadioModel::otherClientsChanged, this, [this]() { refresh(); });
    connect(m_model, &RadioModel::infoChanged, this, [this]() { refresh(); });
    connect(m_table, &QTableWidget::itemSelectionChanged, this, [this]() { refresh(); });

    refresh();
}

void MultiFlexDialog::refresh()
{
    if (m_refreshing) return;
    m_refreshing = true;

    // Enable button style
    bool enabled = m_model->multiFlexEnabled();
    m_enableBtn->setText(enabled ? "Enabled" : "Disabled");
    m_enableBtn->setStyleSheet(enabled
        ? "QPushButton { background: #1a6030; color: #40ff80; border: 1px solid #20a040; "
          "border-radius: 3px; padding: 6px 20px; font-weight: bold; font-size: 13px; }"
          "QPushButton:hover { background: #207040; }"
        : "QPushButton { background: #601a1a; color: #ff6060; border: 1px solid #a02020; "
          "border-radius: 3px; padding: 6px 20px; font-weight: bold; font-size: 13px; }"
          "QPushButton:hover { background: #702020; }");

    const auto& infoMap = m_model->clientInfoMap();
    const quint32 ourHandle = m_model->ourClientHandle();

    // Drop pending-disconnect entries whose handle has actually left the
    // client list — the radio has confirmed the eviction, so a fresh
    // future appearance (impossible today but defensive) gets a normal
    // button again.
    for (auto it = m_pendingDisconnects.begin(); it != m_pendingDisconnects.end(); ) {
        if (!infoMap.contains(*it))
            it = m_pendingDisconnects.erase(it);
        else
            ++it;
    }

    // Build local TX info override from our own slices (ClientInfo may not
    // have TX data if slice status arrived before client connected status)
    QString ourTxAnt;
    double ourTxFreq = 0;
    for (auto* s : m_model->slices()) {
        if (s->isTxSlice()) {
            ourTxAnt = s->txAntenna();
            ourTxFreq = s->frequency();
            break;
        }
    }

    // Remember selected handle across repopulation
    quint32 selectedHandle = 0;
    {
        const auto sel = m_table->selectedItems();
        if (!sel.isEmpty())
            selectedHandle = sel.first()->data(Qt::UserRole).toUInt();
    }

    // Populate table
    m_table->clearContents();
    m_table->setRowCount(infoMap.size());

    bool weHavePtt = false;
    QString ourStation;
    int restoreRow = -1;
    int row = 0;

    for (auto it = infoMap.cbegin(); it != infoMap.cend(); ++it) {
        quint32 handle = it.key();
        const auto& info = it.value();
        bool isUs = (handle == ourHandle);
        bool hasPtt = info.localPtt || (isUs && infoMap.size() == 1);

        if (isUs) {
            weHavePtt = hasPtt;
            ourStation = info.station;
        }
        if (handle == selectedHandle)
            restoreRow = row;

        // LOCAL PTT
        auto* pttItem = new QTableWidgetItem(hasPtt ? "\xE2\x9C\x94" : "");
        pttItem->setTextAlignment(Qt::AlignCenter);
        if (hasPtt)
            pttItem->setForeground(QColor(0x40, 0xff, 0x40));
        pttItem->setData(Qt::UserRole, handle);
        m_table->setItem(row, 0, pttItem);

        // STATION — program: station
        const QString program = info.program.trimmed();
        const QString station = info.station.trimmed();
        QString displayName = program;
        if (!program.isEmpty() && !station.isEmpty() && station != program)
            displayName = program + ": " + station;
        else if (!station.isEmpty())
            displayName = station;
        if (displayName.trimmed().isEmpty())
            displayName = QString("client 0x%1").arg(handle, 8, 16, QChar('0')).toUpper();
        auto* stationItem = new QTableWidgetItem(displayName);
        stationItem->setTextAlignment(Qt::AlignCenter);
        if (isUs)
            stationItem->setForeground(QColor(0x00, 0xb4, 0xd8));
        stationItem->setData(Qt::UserRole, handle);
        m_table->setItem(row, 1, stationItem);

        // TX ANT and TX FREQ — from slice status, with fallback to our own slices
        QString ant = info.txAntenna.isEmpty()
            ? (isUs && !ourTxAnt.isEmpty() ? ourTxAnt : "----")
            : info.txAntenna;
        QString freq = (info.txFreqMhz > 0)
            ? QString::number(info.txFreqMhz, 'f', 3)
            : (isUs && ourTxFreq > 0 ? QString::number(ourTxFreq, 'f', 3) : "----");

        auto* antItem = new QTableWidgetItem(ant);
        antItem->setTextAlignment(Qt::AlignCenter);
        m_table->setItem(row, 2, antItem);

        auto* freqItem = new QTableWidgetItem(freq);
        freqItem->setTextAlignment(Qt::AlignCenter);
        m_table->setItem(row, 3, freqItem);

        auto* actionItem = new QTableWidgetItem;
        actionItem->setData(Qt::UserRole, handle);
        m_table->setItem(row, 4, actionItem);
        if (!isUs) {
            const bool pending = m_pendingDisconnects.contains(handle);
            auto* disconnectBtn = new QPushButton(
                pending ? "Disconnecting" : "Disconnect", m_table);
            disconnectBtn->setObjectName("disconnectClientButton");
            disconnectBtn->setCursor(Qt::PointingHandCursor);
            disconnectBtn->setFixedWidth(kDisconnectButtonWidth);
            disconnectBtn->setEnabled(!pending);
            connect(disconnectBtn, &QPushButton::clicked,
                    this, [this, disconnectBtn, handle, displayName]() {
                disconnectBtn->setEnabled(false);
                disconnectBtn->setText("Disconnecting");
                m_pendingDisconnects.insert(handle);
                emit disconnectClientRequested(handle, displayName);
            });
            m_table->setCellWidget(row, 4, disconnectBtn);
        }

        ++row;
    }

    // Restore row selection after repopulation (blocked to avoid re-entrancy)
    if (restoreRow >= 0) {
        QSignalBlocker sb(m_table);
        m_table->selectRow(restoreRow);
    }

    m_refreshing = false;

    // Local PTT section — determine what the selected row is asking for
    if (infoMap.size() <= 1) {
        m_pttLabel->hide();
        m_pttBtn->hide();
        return;
    }

    if (weHavePtt) {
        // We already hold PTT; another station cannot be granted it from here —
        // they must request it from their own client.
        const auto sel = m_table->selectedItems();
        quint32 selHandle = sel.isEmpty() ? 0 : sel.first()->data(Qt::UserRole).toUInt();
        bool selIsUs = (selHandle == ourHandle || selHandle == 0);
        if (!selIsUs && infoMap.contains(selHandle)) {
            QString selStation = infoMap.value(selHandle).station;
            m_pttLabel->setText(
                QString("%1 must claim PTT from their station.").arg(selStation));
            m_pttLabel->show();
        } else {
            m_pttLabel->hide();
        }
        m_pttBtn->hide();
    } else {
        // We don't have PTT — offer to claim it for our station
        m_pttLabel->setText(
            QString("Enable Local PTT for this station (%1):").arg(ourStation));
        m_pttLabel->show();
        m_pttBtn->setText("Enable");
        m_pttBtn->show();
    }
}

} // namespace AetherSDR
