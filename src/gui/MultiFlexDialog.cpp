#include "MultiFlexDialog.h"
#include "CanonTableStyle.h"
#include "FramelessResizer.h"
#include "core/ThemeManager.h"
#include "models/RadioModel.h"

#include <QBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTableWidget>
#include <QTableWidgetItem>

namespace AetherSDR {

// Every color is a canon token: the window sits under canon/multiFlex. The
// Enabled / Disabled button keeps its on/off green and red, through the
// success and danger tokens.
static const char* kDialogStyle = R"(
QLabel {
    background: transparent;
    color: {{color.canon.inkSoft}};
}
QPushButton {
    color: {{color.canon.cyan}};
    background: {{color.canon.control}};
    border: 1px solid {{color.canon.lineHi}};
    border-radius: 6px;
    padding: 6px 20px;
    font-weight: bold;
}
QPushButton:hover {
    border-color: {{color.canon.cyan}};
}
QPushButton#multiFlexEnableButton {
    font-size: 13px;
}
QPushButton#multiFlexEnableButton[multiFlexOn="true"] {
    color: {{color.toggle.success.foreground.checked}};
    background: {{color.toggle.success.background.checked}};
    border-color: {{color.toggle.success.border.checked}};
}
QPushButton#multiFlexEnableButton[multiFlexOn="false"] {
    color: {{color.accent.danger}};
    border-color: {{color.accent.danger}};
}
QPushButton#multiFlexEnableButton[multiFlexOn="true"]:hover,
QPushButton#multiFlexEnableButton[multiFlexOn="false"]:hover {
    border-color: {{color.canon.aqua}};
}
QPushButton#disconnectClientButton {
    color: {{color.accent.danger}};
    border-color: {{color.accent.danger}};
    padding: 4px 12px;
}
QPushButton#disconnectClientButton:hover {
    background: {{color.canon.nested}};
}
QPushButton#disconnectClientButton:disabled {
    color: {{color.button.danger.foreground.disabled}};
    background: {{color.button.danger.background.disabled}};
    border-color: {{color.button.danger.border.disabled}};
}
)";

// The dashboard's paddings, after the shared canon table rules.
static const char* kDialogTableStyle = R"(
QTableWidget::item {
    padding: 4px 8px;
}
QHeaderView::section {
    padding: 6px;
}
)";

namespace {

// Keeps an item's own color on a selected row: the PTT tick stays green and
// our station stays cyan on the selection band, where Qt would draw them in
// the highlighted-text color. Items with no color of their own take the
// table's text color there, as on an unselected row.
class KeepItemColorDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

protected:
    void initStyleOption(QStyleOptionViewItem* option, const QModelIndex& index) const override
    {
        QStyledItemDelegate::initStyleOption(option, index);
        const QVariant foreground = index.data(Qt::ForegroundRole);
        option->palette.setBrush(QPalette::HighlightedText,
                                 foreground.canConvert<QBrush>()
                                     ? foreground.value<QBrush>()
                                     : option->palette.brush(QPalette::Text));
    }
};

} // namespace

constexpr int kDisconnectActionColumnWidth = 144;
constexpr int kDisconnectButtonWidth = 128;

MultiFlexDialog::MultiFlexDialog(RadioModel* model, QWidget* parent)
    : CanonWindow(QStringLiteral("multiFLEX Dashboard"), parent),
      m_model(model)
{
    setObjectName(QStringLiteral("MultiFlexDialog"));
    theme::setContainer(this, QStringLiteral("canon/multiFlex"));
    // Size and position persist under the key it used as a PersistentDialog,
    // so saved geometry carries over; it resizes from every edge.
    setGeometryKey(QStringLiteral("MultiFlexDialogGeometry"));
    // Return must not click the default button: that is the Enabled /
    // Disabled toggle, which turns multiFLEX off on the radio.
    setReturnClicksDefault(false);
    setMinimumSize(680, 280);
    resize(760, 320);
    FramelessResizer::install(this);
    ThemeManager::instance().applyStyleSheet(
        bodyWidget(), QString::fromLatin1(kDialogStyle) + canonTableRules()
                          + QString::fromLatin1(kDialogTableStyle));

    auto* root = new QVBoxLayout(bodyWidget());
    root->setContentsMargins(14, 12, 14, 14);
    root->setSpacing(10);
    root->addWidget(makeCanonHeader(QStringLiteral("multiFLEX Dashboard")));

    // Enable/disable button
    auto* btnRow = new QHBoxLayout;
    btnRow->addStretch();
    m_enableBtn = new QPushButton;
    m_enableBtn->setObjectName(QStringLiteral("multiFlexEnableButton"));
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
    m_table->setItemDelegate(new KeepItemColorDelegate(m_table));
    root->addWidget(m_table);

    // Local PTT button row
    auto* pttRow = new QHBoxLayout;
    pttRow->addStretch();
    m_pttLabel = new QLabel;
    pttRow->addWidget(m_pttLabel);
    m_pttBtn = new QPushButton("Enable");
    connect(m_pttBtn, &QPushButton::clicked, this, [this]() {
        m_model->requestLocalPtt();
    });
    pttRow->addWidget(m_pttBtn);
    pttRow->addStretch();
    root->addLayout(pttRow);

    // Refresh on client changes or row selection
    connect(m_model, &RadioModel::otherClientsChanged, this, [this]() { refresh(); });
    connect(m_model, &RadioModel::infoChanged, this, [this]() { refresh(); });
    connect(m_table, &QTableWidget::itemSelectionChanged, this, [this]() { refresh(); });
    // The PTT tick and our own station's name carry token colors per item;
    // a theme change recolors them in place.
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
            this, &MultiFlexDialog::applyItemColors);
    // The dashboard belongs to one radio session: losing the radio closes it,
    // so its controls never act on a radio that is gone and no pending
    // disconnect outlives the session.
    connect(m_model, &RadioModel::connectionStateChanged, this, [this](bool connected) {
        if (!connected)
            close();
    });

    refresh();
}

void MultiFlexDialog::refresh()
{
    if (m_refreshing) return;
    m_refreshing = true;

    // Enable button: green when on, red when off (the sheet's multiFlexOn rules)
    bool enabled = m_model->multiFlexEnabled();
    m_enableBtn->setText(enabled ? "Enabled" : "Disabled");
    if (m_enableBtn->property("multiFlexOn") != QVariant(enabled)) {
        m_enableBtn->setProperty("multiFlexOn", enabled);
        m_enableBtn->style()->unpolish(m_enableBtn);
        m_enableBtn->style()->polish(m_enableBtn);
    }

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
        auto* pttItem = new QTableWidgetItem(hasPtt ? QStringLiteral("\u2713") : QString());
        pttItem->setTextAlignment(Qt::AlignCenter);
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

    applyItemColors();

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

void MultiFlexDialog::applyItemColors()
{
    auto& tm = ThemeManager::instance();
    const QColor pttColor = tm.color(m_table, QStringLiteral("color.accent.success"));
    const QColor ourColor = tm.color(m_table, QStringLiteral("color.canon.cyan"));
    const quint32 ourHandle = m_model->ourClientHandle();
    for (int row = 0; row < m_table->rowCount(); ++row) {
        if (auto* ptt = m_table->item(row, 0); ptt && !ptt->text().isEmpty())
            ptt->setForeground(pttColor);
        if (auto* station = m_table->item(row, 1);
            station && station->data(Qt::UserRole).toUInt() == ourHandle)
            station->setForeground(ourColor);
    }
}

} // namespace AetherSDR
