#include "MultiFlexDialog.h"
#include "FramelessResizer.h"
#include "FramelessWindowTitleBar.h"
#include "core/AppSettings.h"
#include "models/RadioModel.h"

#include <QBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QRect>
#include <QTableWidget>

namespace AetherSDR {

static const char* kDialogStyle =
    "QDialog { background: #0f0f1a; }"
    "QLabel { color: #c8d8e8; }"
    "QPushButton { background: #1a2a3a; border: 1px solid #304050; "
    "  border-radius: 3px; color: #c8d8e8; padding: 6px 20px; font-weight: bold; }"
    "QPushButton:hover { background: #203040; border-color: #00b4d8; }"
    "QTableWidget { background: #0f0f1a; color: #c8d8e8; "
    "  gridline-color: #203040; border: 1px solid #304050; }"
    "QTableWidget::item { padding: 4px 8px; }"
    "QHeaderView::section { background: #1a2a3a; color: #8aa8c0; "
    "  border: 1px solid #203040; padding: 6px; font-weight: bold; }";

MultiFlexDialog::MultiFlexDialog(RadioModel* model, QWidget* parent)
    : QDialog(parent), m_model(model)
{
    setWindowTitle("multiFLEX Dashboard");
    setMinimumSize(550, 280);
    resize(600, 320);
    setStyleSheet(kDialogStyle);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    m_titleBar = new FramelessWindowTitleBar(QStringLiteral("multiFLEX Dashboard"), this);
    outer->addWidget(m_titleBar);

    auto* bodyWidget = new QWidget(this);
    auto* root = new QVBoxLayout(bodyWidget);
    root->setContentsMargins(9, 9, 9, 9);
    root->setSpacing(10);
    m_bodyLayout = root;
    outer->addWidget(bodyWidget, 1);

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
    m_table->setColumnCount(4);
    m_table->setHorizontalHeaderLabels({"LOCAL PTT", "STATION", "TX ANT", "TX FREQ (MHz)"});
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_table->verticalHeader()->setVisible(false);
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

    FramelessResizer::install(this);
    setFramelessMode(
        AppSettings::instance().value("FramelessWindow", "True").toString() == "True");
}

void MultiFlexDialog::setFramelessMode(bool on)
{
    const QRect geom = geometry();
    const bool wasVisible = isVisible();

    Qt::WindowFlags flags = (windowFlags() & ~Qt::WindowType_Mask) | Qt::Dialog;
    flags.setFlag(Qt::FramelessWindowHint, on);
    setWindowFlags(flags);
    if (wasVisible) {
        setGeometry(geom);
    }

    if (m_titleBar) {
        m_titleBar->setVisible(on);
    }
    if (m_bodyLayout) {
        m_bodyLayout->setContentsMargins(9, on ? 7 : 9, 9, 9);
    }
    if (wasVisible) {
        show();
    }
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
        QString displayName = info.program;
        if (!info.station.isEmpty() && info.station != info.program)
            displayName = info.program + ": " + info.station;
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
