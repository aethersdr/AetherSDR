#include "MemoryBrowsePanel.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include <QAbstractItemView>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTableWidgetItem>
#include <QTableWidget>
#include <QVBoxLayout>

namespace MasterSDR {

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
    setFixedSize(252, 460);
    setStyleSheet("background: rgba(15, 15, 26, 220); border: 1px solid #304050; border-radius: 3px;");

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(3, 3, 3, 3);
    root->setSpacing(4);

    m_emptyLabel = new QLabel("No memories are available yet.", this);
    m_emptyLabel->setAlignment(Qt::AlignCenter);
    m_emptyLabel->setWordWrap(true);
    m_emptyLabel->setStyleSheet(
        "QLabel { color: #607080; font-size: 12px; padding: 12px 8px; }");
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
    m_table->setStyleSheet(
        "QTableWidget { background: #0f1720; border: 1px solid #203040; border-radius: 2px; "
        "color: #d2dbe4; selection-background-color: #2060a0; selection-color: #ffffff; }"
        "QHeaderView::section { background: #101b26; color: #70879b; border: none; "
        "border-bottom: 1px solid #203040; font-size: 11px; font-weight: bold; padding: 4px 3px; }"
        "QScrollBar:vertical { background: #0a0a14; width: 6px; }"
        "QScrollBar::handle:vertical { background: #304050; border-radius: 3px; }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }");
    root->addWidget(m_table, 1);
    m_table->hide();

    // "MEM+" button anchored at the bottom of the panel — outside the
    // scrolling table so it stays visible regardless of scroll position.
    // Style matches the spectrum overlay menu buttons it replaces.
    m_addBtn = new QPushButton(QStringLiteral("Add Memory"), this);
    m_addBtn->setFixedHeight(22);
    m_addBtn->setStyleSheet(
        "QPushButton { background: rgba(20, 30, 45, 240); "
        "border: 1px solid rgba(255, 255, 255, 40); border-radius: 2px; "
        "color: #c8d8e8; font-size: 11px; font-weight: bold; }"
        "QPushButton:hover { background: rgba(0, 112, 192, 180); "
        "border: 1px solid #0090e0; }");
    m_addBtn->setToolTip("Save the current slice on this panadapter as a memory.");
    root->addWidget(m_addBtn, 0);
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

    for (const MemoryEntry& memory : sortedMemories) {
        if (memory.freq <= 0.0)
            continue;

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

    const bool hasRows = m_table->rowCount() > 0;
    m_emptyLabel->setVisible(!hasRows);
    m_table->setVisible(hasRows);
    if (hasRows)
        scrollToHighlightedRow();
}

} // namespace MasterSDR
