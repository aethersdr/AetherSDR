#include "MemoryBrowsePanel.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include <QAbstractItemView>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QIcon>
#include <QLineEdit>
#include <QPainter>
#include <QPen>
#include <QPixmap>
#include <QPointF>
#include <QRectF>
#include <QToolButton>
#include <QLabel>
#include <QPushButton>
#include <QTableWidgetItem>
#include <QTableWidget>
#include <QVBoxLayout>
#include "core/ThemeManager.h"

namespace AetherSDR {

namespace {

QString displayMemoryName(const MemoryEntry& memory)
{
    if (!memory.name.trimmed().isEmpty())
        return memory.name.trimmed();
    if (!memory.group.trimmed().isEmpty())
        return memory.group.trimmed();
    return QString("Memory %1").arg(memory.index);
}

} // namespace

MemoryBrowsePanel::MemoryBrowsePanel(QWidget* parent)
    : QWidget(parent)
{
    theme::setContainer(this, QStringLiteral("panel/memoryBrowse"));
    setFixedWidth(252);
    AetherSDR::ThemeManager::instance().applyStyleSheet(this, "background: rgba(15, 15, 26, 220); border: 1px solid {{color.background.2}}; border-radius: 3px;");

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(3, 3, 3, 3);
    root->setSpacing(4);

    m_emptyLabel = new QLabel("No memories are available yet.", this);
    m_emptyLabel->setAlignment(Qt::AlignCenter);
    m_emptyLabel->setWordWrap(true);
    AetherSDR::ThemeManager::instance().applyStyleSheet(m_emptyLabel, "QLabel { color: {{color.text.label}}; font-size: 12px; padding: 12px 8px; }");
    root->addWidget(m_emptyLabel, 1);

    m_table = new QTableWidget(0, 2, this);
    m_table->setHorizontalHeaderLabels({"Frequency", "Name"});
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_table->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_table->setShowGrid(false);
    m_table->setFocusPolicy(Qt::StrongFocus);
    m_table->setWordWrap(false);
    m_table->setTextElideMode(Qt::ElideRight);
    m_table->verticalHeader()->setVisible(false);
    m_table->verticalHeader()->setDefaultSectionSize(26);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Fixed);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_table->setColumnWidth(0, 96);
    AetherSDR::ThemeManager::instance().applyStyleSheet(m_table, "QTableWidget { background: #0f1720; border: 1px solid {{color.background.1}}; border-radius: 2px; "
        "color: #d2dbe4; selection-background-color: #2060a0; selection-color: {{color.text.primary}}; }"
        "QHeaderView::section { background: #101b26; color: #70879b; border: none; "
        "border-bottom: 1px solid {{color.background.1}}; font-size: 11px; font-weight: bold; padding: 4px 3px; }"
        "QScrollBar:vertical { background: {{color.background.0}}; width: 6px; }"
        "QScrollBar::handle:vertical { background: {{color.background.2}}; border-radius: 3px; }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }");
    root->addWidget(m_table, 1);
    m_table->hide();

    // Keep the list action above the scrolling rows as the drawer shrinks
    // to fit the panadapter.
    m_addBtn = new QPushButton(QStringLiteral("Add Memory"), this);
    m_addBtn->setObjectName(QStringLiteral("memoryAddButton"));
    m_addBtn->setAccessibleName(QStringLiteral("Add Memory"));
    m_addBtn->setAccessibleDescription(
        QStringLiteral("Save the current slice on this panadapter as a memory."));
    m_addBtn->setFixedHeight(22);
    AetherSDR::ThemeManager::instance().applyStyleSheet(m_addBtn, "QPushButton { background: rgba(20, 30, 45, 240); "
        "border: 1px solid rgba(255, 255, 255, 40); border-radius: 2px; "
        "color: {{color.text.primary}}; font-size: 11px; font-weight: bold; }"
        "QPushButton:hover { background: rgba(0, 112, 192, 180); "
        "border: 1px solid {{color.accent.dim}}; }");
    m_addBtn->setToolTip("Save the current slice on this panadapter as a memory.");
    auto* actions = new QHBoxLayout;
    actions->setSpacing(4);
    actions->addWidget(m_addBtn, 1);
    auto* searchButton = new QToolButton(this);
    searchButton->setObjectName(QStringLiteral("memorySearchButton"));
    searchButton->setAccessibleName(QStringLiteral("Search memories"));
    searchButton->setAccessibleDescription(
        QStringLiteral("Show or hide the memory name filter. Hiding it clears the filter."));
    searchButton->setToolTip(QStringLiteral("Search memories"));
    searchButton->setCheckable(true);
    searchButton->setFixedSize(22, 22);
    ThemeManager::instance().applyStyleSheet(searchButton,
        "QToolButton { background: {{color.canon.control}}; border: 1px solid {{color.canon.lineHi}}; border-radius: 2px; }"
        "QToolButton:hover { background: {{color.canon.nested}}; }"
        "QToolButton:checked, QToolButton:focus { border-color: {{color.canon.cyan}}; }");
    const auto updateSearchIcon = [searchButton] {
        const qreal dpr = searchButton->devicePixelRatioF();
        QPixmap icon(qRound(16 * dpr), qRound(16 * dpr));
        icon.setDevicePixelRatio(dpr);
        icon.fill(Qt::transparent);
        QPainter painter(&icon);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(ThemeManager::instance().color(searchButton,
                              QStringLiteral("color.canon.cyan")), 1.5));
        painter.drawEllipse(QRectF(2, 2, 8, 8));
        painter.drawLine(QPointF(9, 9), QPointF(14, 14));
        painter.end();
        searchButton->setIcon(QIcon(icon));
    };
    updateSearchIcon();
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
            searchButton, updateSearchIcon);
    actions->addWidget(searchButton);
    root->insertLayout(0, actions);

    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setObjectName(QStringLiteral("memorySearchEdit"));
    m_searchEdit->setAccessibleName(QStringLiteral("Memory name filter"));
    m_searchEdit->setAccessibleDescription(
        QStringLiteral("Filter memory names as you type. Typing does not recall a memory."));
    m_searchEdit->setPlaceholderText(QStringLiteral("Search memory names"));
    m_searchEdit->setClearButtonEnabled(true);
    m_searchEdit->setFixedHeight(22);
    ThemeManager::instance().applyStyleSheet(m_searchEdit,
        "QLineEdit { background: {{color.canon.control}}; color: {{color.canon.ink}};"
        " border: 1px solid {{color.canon.lineHi}}; border-radius: 2px; padding: 0 4px; }"
        "QLineEdit:focus { border-color: {{color.canon.cyan}}; }");
    root->insertWidget(1, m_searchEdit);
    m_searchEdit->hide();
    connect(m_searchEdit, &QLineEdit::textChanged, this, &MemoryBrowsePanel::populateTable);
    connect(searchButton, &QToolButton::toggled, this, [this, searchButton](bool shown) {
        m_searchEdit->setVisible(shown);
        if (shown) {
            m_searchEdit->setFocus(Qt::OtherFocusReason);
        } else {
            m_searchEdit->clear();
            searchButton->setFocus(Qt::OtherFocusReason);
        }
    });
    connect(m_addBtn, &QPushButton::clicked, this,
            &MemoryBrowsePanel::quickAddRequested);

    connect(m_table, &QTableWidget::cellClicked, this, [this](int row, int) {
        auto* indexItem = m_table->item(row, 0);
        if (!indexItem)
            return;
        emit memoryActivated(indexItem->data(Qt::UserRole).toInt());
    });

    connect(m_table, &QTableWidget::cellActivated, this, [this](int row, int) {
        auto* indexItem = m_table->item(row, 0);
        if (!indexItem)
            return;
        emit memoryActivated(indexItem->data(Qt::UserRole).toInt());
    });
}

void MemoryBrowsePanel::setMemories(const QMap<int, MemoryEntry>& memories)
{
    m_memories = memories;
    populateTable();
}

void MemoryBrowsePanel::setWritable(bool writable)
{
    m_addBtn->setEnabled(writable);
    m_addBtn->setToolTip(writable
        ? QStringLiteral("Save the current slice on this panadapter as a memory.")
        : QStringLiteral("Radio memories are read-only in this version."));
}

void MemoryBrowsePanel::focusClosestToFrequency(double frequencyMhz)
{
    int closestMemoryIndex = -1;

    if (frequencyMhz > 0.0) {
        double bestDelta = std::numeric_limits<double>::max();
        for (auto it = m_memories.cbegin(); it != m_memories.cend(); ++it) {
            const MemoryEntry& memory = it.value();
            if (memory.freq <= 0.0)
                continue;

            const double delta = std::abs(memory.freq - frequencyMhz);
            if (delta < bestDelta
                || (qFuzzyCompare(delta + 1.0, bestDelta + 1.0)
                    && memory.index < closestMemoryIndex)) {
                bestDelta = delta;
                closestMemoryIndex = memory.index;
            }
        }
    }

    m_highlightedMemoryIndex = closestMemoryIndex;
    populateTable();
}

QColor MemoryBrowsePanel::rowBackground(const MemoryEntry& memory, bool highlight) const
{
    if (highlight)
        return QColor(0x24, 0x3b, 0x4d);
    return (memory.index % 2 == 0)
        ? QColor(0x17, 0x25, 0x34)
        : QColor(0x11, 0x19, 0x23);
}

void MemoryBrowsePanel::scrollToHighlightedRow()
{
    if (m_highlightedMemoryIndex < 0 || !m_table)
        return;

    for (int row = 0; row < m_table->rowCount(); ++row) {
        auto* freqItem = m_table->item(row, 0);
        if (!freqItem)
            continue;
        if (freqItem->data(Qt::UserRole).toInt() != m_highlightedMemoryIndex)
            continue;

        m_table->scrollToItem(freqItem, QAbstractItemView::PositionAtCenter);
        return;
    }
}

void MemoryBrowsePanel::populateTable()
{
    m_table->setSortingEnabled(false);
    m_table->setRowCount(0);
    m_table->clearSelection();

    QList<MemoryEntry> sortedMemories;
    sortedMemories.reserve(m_memories.size());
    for (auto it = m_memories.cbegin(); it != m_memories.cend(); ++it) {
        if (it.value().freq > 0.0)
            sortedMemories.append(it.value());
    }

    std::sort(sortedMemories.begin(), sortedMemories.end(),
              [](const MemoryEntry& lhs, const MemoryEntry& rhs) {
        if (!qFuzzyCompare(lhs.freq + 1.0, rhs.freq + 1.0))
            return lhs.freq < rhs.freq;
        return lhs.index < rhs.index;
    });

    const QString query = m_searchEdit->text().trimmed();
    for (const MemoryEntry& memory : sortedMemories) {
        if (!displayMemoryName(memory).contains(query, Qt::CaseInsensitive)) {
            continue;
        }

        const int row = m_table->rowCount();
        m_table->insertRow(row);

        const bool highlight = (memory.index == m_highlightedMemoryIndex);
        const QColor bg = rowBackground(memory, highlight);
        const QColor fg = QColor(0xd2, 0xdb, 0xe4);

        auto* freqItem = new QTableWidgetItem(QString::number(memory.freq, 'f', 6));
        freqItem->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        freqItem->setBackground(bg);
        freqItem->setForeground(fg);
        freqItem->setData(Qt::UserRole, memory.index);
        freqItem->setToolTip(QString("%1 MHz").arg(QString::number(memory.freq, 'f', 6)));
        m_table->setItem(row, 0, freqItem);

        auto* nameItem = new QTableWidgetItem(displayMemoryName(memory));
        nameItem->setBackground(bg);
        nameItem->setForeground(fg);
        nameItem->setData(Qt::UserRole, memory.index);
        nameItem->setToolTip(nameItem->text());
        m_table->setItem(row, 1, nameItem);
    }

    m_emptyLabel->setText(sortedMemories.isEmpty()
        ? QStringLiteral("No memories are available yet.")
        : QStringLiteral("No matching memories."));
    const bool hasRows = m_table->rowCount() > 0;
    m_emptyLabel->setVisible(!hasRows);
    m_table->setVisible(hasRows);
    if (hasRows)
        scrollToHighlightedRow();
}

} // namespace AetherSDR
