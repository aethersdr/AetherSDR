#include "MemoryDialog.h"
#include "FramelessResizer.h"
#include "FramelessWindowTitleBar.h"
#include "MemoryCommands.h"
#include "core/AppSettings.h"
#include "core/MemoryCsvCompat.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"
#include "models/TransmitModel.h"
#include "core/RadioConnection.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QComboBox>
#include <QLineEdit>
#include <QLabel>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QDebug>
#include <QMessageBox>
#include <QProgressBar>
#include <QProgressDialog>
#include <QPointer>
#include <QShortcut>
#include <QSaveFile>
#include <QSharedPointer>
#include <QTimer>
#include <QCloseEvent>
#include <QKeyEvent>
#include <QtGlobal>

#include <functional>

namespace AetherSDR {

namespace {

class MemoryTableItem : public QTableWidgetItem {
public:
    using QTableWidgetItem::QTableWidgetItem;

    bool operator<(const QTableWidgetItem& other) const override
    {
        const QVariant lhs = data(Qt::UserRole);
        const QVariant rhs = other.data(Qt::UserRole);
        if (lhs.isValid() && rhs.isValid())
            return lhs.toDouble() < rhs.toDouble();
        return QTableWidgetItem::operator<(other);
    }
};

bool buildMemoryFieldUpdate(int col, const QTableWidgetItem* item,
                            QString& commandSuffix, QMap<QString, QString>& kvs)
{
    if (!item)
        return false;

    const QString value = item->text();
    switch (col) {
    case 0: {
        const QString encoded = AetherSDR::encodeMemoryText(value);
        commandSuffix = "group=" + encoded;
        kvs["group"] = encoded;
        return true;
    }
    case 1: {
        const QString encoded = AetherSDR::encodeMemoryText(value);
        commandSuffix = "owner=" + encoded;
        kvs["owner"] = encoded;
        return true;
    }
    case 2:
        commandSuffix = "freq=" + value;
        kvs["freq"] = value;
        return true;
    case 3: {
        const QString encoded = AetherSDR::encodeMemoryText(value);
        commandSuffix = "name=" + encoded;
        kvs["name"] = encoded;
        return true;
    }
    case 4:
        commandSuffix = "mode=" + value;
        kvs["mode"] = value;
        return true;
    case 5:
        commandSuffix = "step=" + value;
        kvs["step"] = value;
        return true;
    case 6:
        commandSuffix = "repeater=" + value;
        kvs["repeater"] = value;
        return true;
    case 7:
        commandSuffix = "repeater_offset=" + value;
        kvs["repeater_offset"] = value;
        return true;
    case 8:
        commandSuffix = "tone_mode=" + value;
        kvs["tone_mode"] = value;
        return true;
    case 9:
        commandSuffix = "tone_value=" + value;
        kvs["tone_value"] = value;
        return true;
    case 10: {
        const QString squelch = item->checkState() == Qt::Checked ? "1" : "0";
        commandSuffix = "squelch=" + squelch;
        kvs["squelch"] = squelch;
        return true;
    }
    case 11:
        commandSuffix = "squelch_level=" + value;
        kvs["squelch_level"] = value;
        return true;
    case 12:
        commandSuffix = "rx_filter_low=" + value;
        kvs["rx_filter_low"] = value;
        return true;
    case 13:
        commandSuffix = "rx_filter_high=" + value;
        kvs["rx_filter_high"] = value;
        return true;
    case 14:
        commandSuffix = "rtty_mark=" + value;
        kvs["rtty_mark"] = value;
        return true;
    case 15:
        commandSuffix = "rtty_shift=" + value;
        kvs["rtty_shift"] = value;
        return true;
    case 16:
        commandSuffix = "digl_offset=" + value;
        kvs["digl_offset"] = value;
        return true;
    case 17:
        commandSuffix = "digu_offset=" + value;
        kvs["digu_offset"] = value;
        return true;
    default:
        return false;
    }
}

QString defaultExportFilePath()
{
    const QString baseName = QString("AetherSDR_Memories_%1_v%2.csv")
        .arg(QDateTime::currentDateTime().toString("MM-dd-yy_hh_mm"))
        .arg(QCoreApplication::applicationVersion());
    return QDir::home().filePath(QString("Documents/%1").arg(baseName));
}

QList<MemoryCsvRecord> currentExportRecords(const QMap<int, MemoryEntry>& memories,
                                            const QString& filterProfile)
{
    QList<MemoryCsvRecord> records;
    for (auto it = memories.cbegin(); it != memories.cend(); ++it) {
        const MemoryEntry& memory = it.value();
        if (!filterProfile.isEmpty() && memory.group != filterProfile)
            continue;
        records << MemoryCsvCompat::fromMemoryEntry(memory);
    }

    std::sort(records.begin(), records.end(),
              [](const MemoryCsvRecord& lhs, const MemoryCsvRecord& rhs) {
        if (!qFuzzyCompare(lhs.memory.freq + 1.0, rhs.memory.freq + 1.0))
            return lhs.memory.freq < rhs.memory.freq;
        return lhs.memory.index < rhs.memory.index;
    });
    return records;
}

QString selectionHintText()
{
#if defined(Q_OS_MACOS)
    return "Tip: Double-click tunes. Shift-click selects a range. Command-click adds or removes rows.";
#else
    return "Tip: Double-click tunes. Shift-click selects a range. Ctrl-click adds or removes rows.";
#endif
}

QString describeMemory(const MemoryEntry& memory)
{
    const QString label = memory.name.trimmed().isEmpty()
        ? QString("Memory %1").arg(memory.index)
        : memory.name.trimmed();
    return QString("%1 (%2 MHz)").arg(label, QString::number(memory.freq, 'f', 6));
}

QString describeImportedMemory(const MemoryEntry& memory)
{
    const QString label = memory.name.trimmed();
    if (!label.isEmpty())
        return QString("%1 (%2 MHz)").arg(label, QString::number(memory.freq, 'f', 6));
    if (memory.freq > 0.0)
        return QString("%1 MHz").arg(QString::number(memory.freq, 'f', 6));
    return "Unnamed memory";
}

QString formatImportIssue(const QString& rowLabel,
                          const QString& message,
                          const QString& detail = QString())
{
    const QString normalizedDetail = detail.simplified();
    return normalizedDetail.isEmpty()
        ? QString("%1: %2").arg(rowLabel, message)
        : QString("%1: %2 (%3)").arg(rowLabel, message, normalizedDetail);
}

} // namespace

static const QStringList COLUMNS = {
    "Group", "Owner", "Frequency", "Name", "Mode", "Step",
    "FM TX Offset Dir", "Repeater Offset", "Tone Mode", "Tone Value",
    "Squelch", "Squelch Level", "RX Filter Low", "RX Filter High",
    "RTTY Mark", "RTTY Shift", "DIGL Offset", "DIGU Offset"
};

MemoryDialog::MemoryDialog(RadioModel* model, QWidget* parent)
    : QDialog(parent), m_model(model)
{
    setWindowTitle("Memory Channels");
    resize(1000, 500);

    // Outer layout hosts the frameless title bar + content area.  The
    // existing dialog body layout (search row, table, buttons) lives
    // inside `content` so flipping frameless on/off only adjusts the
    // title bar's visibility and outer margin.
    m_outerLayout = new QVBoxLayout(this);
    m_outerLayout->setSpacing(0);
    m_outerLayout->setContentsMargins(0, 0, 0, 0);

    m_titleBar = new FramelessWindowTitleBar(QStringLiteral("Memory Channels"), this);
    m_outerLayout->addWidget(m_titleBar);

    auto* content = new QWidget(this);
    m_outerLayout->addWidget(content, 1);

    auto* root = new QVBoxLayout(content);

    // ── Search + profile filter ──────────────────────────────────────────
    auto* filterRow = new QHBoxLayout;
    filterRow->addWidget(new QLabel("Search:"));
    m_searchEdit = new QLineEdit;
    m_searchEdit->setPlaceholderText("Type a memory name and press Enter");
    m_searchEdit->setClearButtonEnabled(true);
    m_searchEdit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_searchEdit->installEventFilter(this);
    filterRow->addWidget(m_searchEdit, 1);
    filterRow->addWidget(new QLabel("Profile:"));
    m_filterCombo = new QComboBox;
    m_filterCombo->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    rebuildFilterCombo();
    filterRow->addWidget(m_filterCombo);
    root->addLayout(filterRow);

    connect(m_searchEdit, &QLineEdit::textChanged,
            this, [this](const QString&) { populateTable(); });
    connect(m_searchEdit, &QLineEdit::returnPressed,
            this, [this]() { activateMemoryRow(m_table ? m_table->currentRow() : -1); });
    connect(m_filterCombo, &QComboBox::currentIndexChanged,
            this, [this](int) { populateTable(); });

    // ── Table ─────────────────────────────────────────────────────────────
    m_table = new QTableWidget(0, COLUMNS.size());
    m_table->setHorizontalHeaderLabels(COLUMNS);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->verticalHeader()->setVisible(false);
    m_table->setAlternatingRowColors(true);
    m_table->setSortingEnabled(false);
    m_table->installEventFilter(this);
    m_table->viewport()->installEventFilter(this);
    m_table->setStyleSheet(
        "QTableWidget { alternate-background-color: #1a1a2e; }"
        "QTableWidget::item:selected { background: #2060a0; }");
    auto* header = m_table->horizontalHeader();
    header->setSectionsClickable(true);
    header->setSortIndicatorShown(false);
    header->resizeSection(2, 110);
    connect(header, &QHeaderView::sectionClicked, this, [this, header](int section) {
        if (!isSortableColumn(section)) return;
        if (m_sortColumn == section)
            m_sortOrder = (m_sortOrder == Qt::AscendingOrder)
                ? Qt::DescendingOrder : Qt::AscendingOrder;
        else {
            m_sortColumn = section;
            m_sortOrder = Qt::AscendingOrder;
        }
        header->setSortIndicatorShown(true);
        header->setSortIndicator(m_sortColumn, m_sortOrder);
        m_table->sortItems(m_sortColumn, m_sortOrder);
    });
    root->addWidget(m_table);
    root->addWidget(new QLabel(selectionHintText()));

    // ── Buttons ───────────────────────────────────────────────────────────
    auto* btnRow = new QHBoxLayout;
    auto* importBtn = new QPushButton("Import...");
    auto* exportBtn = new QPushButton("Export...");
    auto* addBtn = new QPushButton("Add");
    m_selectionLabel = new QLabel("0 selected");
    m_editBtn = new QPushButton("Edit");
    m_selectBtn = new QPushButton("Tune");
    m_selectAllBtn = new QPushButton("Select All");
    m_removeBtn = new QPushButton("Remove");
    btnRow->addWidget(addBtn);
    btnRow->addWidget(m_editBtn);
    btnRow->addWidget(m_selectBtn);
    btnRow->addWidget(m_selectAllBtn);
    btnRow->addWidget(importBtn);
    btnRow->addWidget(exportBtn);
    btnRow->addStretch();
    btnRow->addWidget(m_selectionLabel);
    btnRow->addWidget(m_removeBtn);
    root->addLayout(btnRow);

    for (QPushButton* button : {addBtn, m_editBtn, m_selectBtn, m_selectAllBtn, importBtn, exportBtn, m_removeBtn}) {
        button->setAutoDefault(false);
        button->setDefault(false);
    }

    connect(importBtn, &QPushButton::clicked, this, &MemoryDialog::onImport);
    connect(exportBtn, &QPushButton::clicked, this, &MemoryDialog::onExport);
    connect(addBtn, &QPushButton::clicked, this, &MemoryDialog::onAdd);
    connect(m_editBtn, &QPushButton::clicked, this, &MemoryDialog::onEdit);
    connect(m_selectBtn, &QPushButton::clicked, this, &MemoryDialog::onSelect);
    connect(m_selectAllBtn, &QPushButton::clicked, this, &MemoryDialog::onSelectAll);
    connect(m_removeBtn, &QPushButton::clicked, this, &MemoryDialog::onRemove);
    connect(m_table, &QTableWidget::cellDoubleClicked,
            this, [this](int row, int) { activateMemoryRow(row); });
    connect(m_table->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, &MemoryDialog::updateSelectionActions);
    connect(m_table->selectionModel(), &QItemSelectionModel::currentRowChanged,
            this, [this](const QModelIndex&, const QModelIndex&) {
        updateSelectionActions();
    });
    new QShortcut(QKeySequence::Find, this, [this]() {
        if (!m_searchEdit)
            return;
        m_searchEdit->setFocus(Qt::ShortcutFocusReason);
        m_searchEdit->selectAll();
    });
    new QShortcut(QKeySequence::New, this, [this]() { onAdd(); });
    new QShortcut(QKeySequence(Qt::Key_F2), this, [this]() { onEdit(); });
    new QShortcut(QKeySequence(QStringLiteral("Ctrl+E")), this, [this]() { onEdit(); });
    new QShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+A")), this, [this]() { onSelectAll(); });

    // Rebuild filter combo when profile lists change
    connect(model, &RadioModel::globalProfilesChanged,
            this, &MemoryDialog::rebuildFilterCombo);
    connect(&model->transmitModel(), &TransmitModel::profileListChanged,
            this, &MemoryDialog::rebuildFilterCombo);

    // Listen for live memory updates while dialog is open
    connect(model, &RadioModel::memoryChanged,
            this, [this](int) {
        QTimer::singleShot(50, this, [this]() { populateTable(); });
    });
    connect(model, &RadioModel::memoryRemoved,
            this, [this](int) {
        populateTable();
    });
    connect(model, &RadioModel::memoriesCleared,
            this, [this]() {
        populateTable();
    });

    // Send edits to the radio when any cell changes
    connect(m_table, &QTableWidget::cellChanged, this, [this](int row, int col) {
        submitCellEdit(row, col);
    });
    // Double-click tunes to the selected memory.
    // Editing is explicit via the Edit button to avoid accidental cell mutations.

    // The radio doesn't support "sub memory all" or "memory list".
    // Populate from RadioModel cache (filled from status pushes during connect).
    // If cache is empty, memories may not have been pushed yet. As a fallback,
    // new memories created via Add will populate the table immediately.
    populateTable();

    // 8-axis edge resize for the frameless mode.
    FramelessResizer::install(this);
    setFramelessMode(
        AppSettings::instance().value("FramelessWindow", "True").toString() == "True");
}

void MemoryDialog::setFramelessMode(bool on)
{
    const QRect geom = geometry();
    const bool wasVisible = isVisible();

    Qt::WindowFlags flags = (windowFlags() & ~Qt::WindowType_Mask) | Qt::Dialog;
    flags.setFlag(Qt::FramelessWindowHint, on);
    setWindowFlags(flags);
    setGeometry(geom);

    if (m_titleBar)
        m_titleBar->setVisible(on);
    if (m_outerLayout)
        m_outerLayout->setContentsMargins(0, 0, 0, 0);

    if (wasVisible)
        show();
}

void MemoryDialog::closeEvent(QCloseEvent* event)
{
    QDialog::closeEvent(event);
}

void MemoryDialog::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape) {
        if (m_searchEdit && !m_searchEdit->text().isEmpty()) {
            m_searchEdit->clear();
            m_searchEdit->setFocus(Qt::ShortcutFocusReason);
        } else {
            reject();
        }
        return;
    }

    QDialog::keyPressEvent(event);
}

bool MemoryDialog::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::KeyPress) {
        auto* keyEvent = static_cast<QKeyEvent*>(event);
        const int key = keyEvent->key();

        if ((watched == m_searchEdit || watched == m_table) && key == Qt::Key_Escape) {
            if (m_searchEdit && !m_searchEdit->text().isEmpty()) {
                m_searchEdit->clear();
                m_searchEdit->setFocus(Qt::ShortcutFocusReason);
            } else {
                reject();
            }
            return true;
        }

        if (watched == m_searchEdit) {
            if (key == Qt::Key_Up || key == Qt::Key_Down) {
                const int rowCount = m_table ? m_table->rowCount() : 0;
                if (rowCount <= 0)
                    return true;

                const int currentRow = qMax(0, m_table->currentRow());
                const int nextRow = (key == Qt::Key_Down)
                    ? qMin(currentRow + 1, rowCount - 1)
                    : qMax(currentRow - 1, 0);
                m_table->selectRow(nextRow);
                m_table->setCurrentCell(nextRow, 0);
                return true;
            }
            if (key == Qt::Key_Return || key == Qt::Key_Enter) {
                int row = m_table ? m_table->currentRow() : -1;
                if (m_table && row < 0 && m_table->rowCount() > 0) {
                    row = 0;
                    m_table->selectRow(row);
                    m_table->setCurrentCell(row, 0);
                }
                activateMemoryRow(row);
                return true;
            }
        }

        if (m_table && watched == m_table && (key == Qt::Key_Return || key == Qt::Key_Enter)) {
            // Don't intercept Enter while editing a cell — let the delegate handle it.
            // QAbstractItemView::state() is protected, so check for an active editor
            // via indexWidget on the current index instead.
            QModelIndex idx = m_table->currentIndex();
            if (selectedMemoryIndices().size() == 1
                && !m_table->indexWidget(idx)
                && !m_table->isPersistentEditorOpen(m_table->currentItem())) {
                activateMemoryRow(m_table->currentRow());
                return true;
            }
        }

        if (m_table && watched == m_table && (key == Qt::Key_Delete || key == Qt::Key_Backspace)) {
            if (!selectedMemoryIndices().isEmpty()) {
                onRemove();
                return true;
            }
        }

        if (m_table && watched == m_table && key == Qt::Key_A
            && (keyEvent->modifiers() & (Qt::ControlModifier | Qt::MetaModifier))) {
            onSelectAll();
            return true;
        }
    }

    return QDialog::eventFilter(watched, event);
}

void MemoryDialog::showEvent(QShowEvent* event)
{
    QDialog::showEvent(event);
    if (m_searchEdit)
        m_searchEdit->setFocus(Qt::OtherFocusReason);
}

void MemoryDialog::activateMemoryRow(int row)
{
    if (row < 0)
        return;

    auto* indexItem = m_table->item(row, 0);
    if (!indexItem)
        return;

    const int idx = indexItem->data(Qt::UserRole).toInt();
    if (m_model->memories().constFind(idx) == m_model->memories().constEnd())
        return;

    setInlineEditMode(false);
    m_table->setCurrentCell(row, 0);
    focusTableOnCurrentRow();
    emit memoryActivated(idx);
}

void MemoryDialog::beginEditingMemoryName(int memoryIndex)
{
    if (!m_table)
        return;

    for (int row = 0; row < m_table->rowCount(); ++row) {
        auto* indexItem = m_table->item(row, 0);
        auto* nameItem = m_table->item(row, 3);
        if (!indexItem || !nameItem)
            continue;
        if (indexItem->data(Qt::UserRole).toInt() != memoryIndex)
            continue;

        if (auto* selectionModel = m_table->selectionModel()) {
            selectionModel->select(
                m_table->model()->index(row, 0),
                QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        }
        setInlineEditMode(true);
        m_table->setCurrentCell(row, 3, QItemSelectionModel::NoUpdate);
        m_table->scrollToItem(nameItem, QAbstractItemView::PositionAtCenter);
        m_table->setFocus(Qt::OtherFocusReason);
        updateSelectionActions();
        m_table->editItem(nameItem);
        return;
    }
}

void MemoryDialog::focusTableOnCurrentRow()
{
    if (!m_table)
        return;

    if (m_table->currentRow() < 0 && m_table->rowCount() > 0)
        m_table->setCurrentCell(0, 0, QItemSelectionModel::NoUpdate);
    m_table->setFocus(Qt::OtherFocusReason);
}

void MemoryDialog::setInlineEditMode(bool enabled)
{
    m_inlineEditMode = enabled;
    if (!m_table)
        return;

    m_table->setEditTriggers(enabled
        ? (QAbstractItemView::SelectedClicked | QAbstractItemView::EditKeyPressed)
        : QAbstractItemView::NoEditTriggers);
}

void MemoryDialog::populateTable()
{
    const QSignalBlocker blocker(m_table);
    const QSet<int> previousSelection = selectedMemoryIndices();
    const int currentMemoryIndex = (m_table->currentRow() >= 0 && m_table->item(m_table->currentRow(), 0))
        ? m_table->item(m_table->currentRow(), 0)->data(Qt::UserRole).toInt()
        : -1;
    m_table->setSortingEnabled(false);
    m_table->setRowCount(0);
    const auto& memories = m_model->memories();
    const QString filterProfile = m_filterCombo->currentData().toString();
    const QString nameFilter = m_searchEdit ? m_searchEdit->text().trimmed() : QString();
    bool hasRows = false;

    for (auto it = memories.begin(); it != memories.end(); ++it) {
        const auto& m = it.value();

        // Apply profile filter: skip memories whose group doesn't match
        if (!filterProfile.isEmpty() && m.group != filterProfile) {
            continue;
        }
        if (!nameFilter.isEmpty() && !m.name.contains(nameFilter, Qt::CaseInsensitive)) {
            continue;
        }
        int row = m_table->rowCount();
        m_table->insertRow(row);
        hasRows = true;

        int col = 0;
        m_table->setItem(row, col++, new QTableWidgetItem(m.group));
        m_table->setItem(row, col++, new QTableWidgetItem(m.owner));
        // Show the full MHz value so the last 3 digits (Hz) are not lost.
        auto* freqItem = new MemoryTableItem(QString::number(m.freq, 'f', 6));
        freqItem->setData(Qt::UserRole, m.freq);
        m_table->setItem(row, col++, freqItem);
        m_table->setItem(row, col++, new QTableWidgetItem(m.name));
        m_table->setItem(row, col++, new QTableWidgetItem(m.mode));
        m_table->setItem(row, col++, new QTableWidgetItem(
            QString::number(m.step)));
        m_table->setItem(row, col++, new QTableWidgetItem(m.offsetDir));
        m_table->setItem(row, col++, new QTableWidgetItem(
            QString::number(m.repeaterOffset, 'f', 1)));
        m_table->setItem(row, col++, new QTableWidgetItem(m.toneMode));
        m_table->setItem(row, col++, new QTableWidgetItem(
            QString::number(m.toneValue, 'f', 1)));

        // Squelch checkbox column
        auto* sqItem = new QTableWidgetItem();
        sqItem->setCheckState(m.squelch ? Qt::Checked : Qt::Unchecked);
        m_table->setItem(row, col++, sqItem);

        m_table->setItem(row, col++, new QTableWidgetItem(
            QString::number(m.squelchLevel)));
        m_table->setItem(row, col++, new QTableWidgetItem(
            QString::number(m.rxFilterLow)));
        m_table->setItem(row, col++, new QTableWidgetItem(
            QString::number(m.rxFilterHigh)));
        m_table->setItem(row, col++, new QTableWidgetItem(
            QString::number(m.rttyMark)));
        m_table->setItem(row, col++, new QTableWidgetItem(
            QString::number(m.rttyShift)));
        m_table->setItem(row, col++, new QTableWidgetItem(
            QString::number(m.diglOffset)));
        m_table->setItem(row, col++, new QTableWidgetItem(
            QString::number(m.diguOffset)));

        // Store memory index in first column's data for retrieval
        m_table->item(row, 0)->setData(Qt::UserRole, m.index);

        // All columns are editable (double-click to edit).
        // Squelch column (10) uses checkbox — keep it user-checkable.
        for (int c = 0; c < m_table->columnCount(); ++c) {
            auto* item = m_table->item(row, c);
            if (item && c != 10)
                item->setFlags(item->flags() | Qt::ItemIsEditable);
        }
    }

    m_table->resizeColumnsToContents();
    m_table->setColumnWidth(2, std::max(m_table->columnWidth(2), 105));
    const int minNameWidth = fontMetrics().horizontalAdvance(QString(20, QChar('M')));
    m_table->setColumnWidth(3, std::max(m_table->columnWidth(3), minNameWidth));
    if (isSortableColumn(m_sortColumn)) {
        auto* header = m_table->horizontalHeader();
        header->setSortIndicatorShown(true);
        header->setSortIndicator(m_sortColumn, m_sortOrder);
        m_table->sortItems(m_sortColumn, m_sortOrder);
    }
    m_table->setSortingEnabled(true);

    if (hasRows) {
        bool restoredSelection = false;
        int currentRow = -1;
        int firstSelectedRow = -1;
        for (int row = 0; row < m_table->rowCount(); ++row) {
            auto* item = m_table->item(row, 0);
            if (!item)
                continue;

            const int memoryIndex = item->data(Qt::UserRole).toInt();
            if (previousSelection.contains(memoryIndex)) {
                m_table->selectionModel()->select(
                    m_table->model()->index(row, 0),
                    QItemSelectionModel::Select | QItemSelectionModel::Rows);
                restoredSelection = true;
                if (firstSelectedRow < 0)
                    firstSelectedRow = row;
            }
            if (memoryIndex == currentMemoryIndex)
                currentRow = row;
        }

        if (!restoredSelection) {
            currentRow = currentRow >= 0 ? currentRow : 0;
            m_table->selectRow(currentRow);
        } else if (currentRow < 0) {
            currentRow = firstSelectedRow;
        }

        if (currentRow >= 0)
            m_table->setCurrentCell(currentRow, 0, QItemSelectionModel::NoUpdate);
    }

    updateSelectionActions();

    if (m_pendingEditMemoryIndex >= 0 && m_pendingEditRetries > 0) {
        const int pendingMemoryIndex = m_pendingEditMemoryIndex;
        --m_pendingEditRetries;
        QTimer::singleShot(0, this, [this, pendingMemoryIndex]() {
            beginEditingMemoryName(pendingMemoryIndex);
        });
        if (m_pendingEditRetries == 0)
            m_pendingEditMemoryIndex = -1;
    }
}

bool MemoryDialog::isSortableColumn(int column) const
{
    return column == 2 || column == 3 || column == 4;
}

void MemoryDialog::submitCellEdit(int row, int col)
{
    auto* indexItem = m_table->item(row, 0);
    auto* item = m_table->item(row, col);
    if (!indexItem || !item)
        return;

    QString commandSuffix;
    QMap<QString, QString> kvs;
    if (!buildMemoryFieldUpdate(col, item, commandSuffix, kvs))
        return;

    const int memIdx = indexItem->data(Qt::UserRole).toInt();
    if (col == 3 && memIdx == m_pendingEditMemoryIndex) {
        m_pendingEditMemoryIndex = -1;
        m_pendingEditRetries = 0;
    }
    RadioModel* const model = m_model;
    const QPointer<MemoryDialog> dialogGuard(this);
    model->sendCmdPublic(QString("memory set %1 %2").arg(memIdx).arg(commandSuffix),
        [model, dialogGuard, memIdx, kvs](int code, const QString&) {
        if (code == 0) {
            model->handleMemoryStatus(memIdx, kvs);
            return;
        }
        if (dialogGuard)
            dialogGuard->populateTable();
    });
}

void MemoryDialog::onAdd()
{
    const auto slices = m_model->slices();
    if (slices.isEmpty())
        return;

    SliceModel* active = nullptr;
    for (auto* candidate : slices) {
        if (candidate && candidate->isActive()) {
            active = candidate;
            break;
        }
    }
    if (!active)
        active = slices.first();
    if (!active)
        return;

    createMemoryFromSlice(m_model, active, QString(), this,
        [this](int code, const QString&, int memoryIndex) {
        if (code != 0 || memoryIndex < 0)
            return;

        m_pendingEditMemoryIndex = memoryIndex;
        m_pendingEditRetries = 3;
        populateTable();
    });
}

void MemoryDialog::onExport()
{
    const QString filterProfile = m_filterCombo->currentData().toString();
    const QList<MemoryCsvRecord> records =
        currentExportRecords(m_model->memories(), filterProfile);

    if (records.isEmpty()) {
        QMessageBox::information(this, "Export Memories",
                                 filterProfile.isEmpty()
                                     ? "There are no memories to export."
                                     : "There are no memories in the current filter to export.");
        return;
    }

    const QString path = QFileDialog::getSaveFileName(
        this,
        "Export Memories",
        defaultExportFilePath(),
        "CSV Files (*.csv)");
    if (path.isEmpty())
        return;

    const QByteArray csv = MemoryCsvCompat::serialize(records);
    const MemoryCsvParseResult validation = MemoryCsvCompat::parse(csv);
    if (!validation.ok()) {
        QMessageBox::warning(this, "Export Memories",
                             QString("The generated SmartSDR CSV failed validation:\n%1")
                                 .arg(validation.errors.join('\n')));
        return;
    }

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        QMessageBox::warning(this, "Export Memories",
                             QString("Couldn't open %1 for writing.")
                                 .arg(QDir::toNativeSeparators(path)));
        return;
    }

    if (file.write(csv) != csv.size()) {
        QMessageBox::warning(this, "Export Memories",
                             QString("Couldn't write the SmartSDR CSV to %1.")
                                 .arg(QDir::toNativeSeparators(path)));
        return;
    }

    if (!file.commit()) {
        QMessageBox::warning(this, "Export Memories",
                             QString("Couldn't save %1.")
                                 .arg(QDir::toNativeSeparators(path)));
        return;
    }

    QMessageBox::information(this, "Export Memories",
                             QString("Exported %1 memories to %2.")
                                 .arg(records.size())
                                 .arg(QDir::toNativeSeparators(QFileInfo(path).fileName())));
}

void MemoryDialog::onImport()
{
    const QString path = QFileDialog::getOpenFileName(
        this,
        "Import Memories",
        QDir::home().filePath("Documents"),
        "CSV Files (*.csv)");
    if (path.isEmpty())
        return;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, "Import Memories",
                             QString("Couldn't open %1 for reading.")
                                 .arg(QDir::toNativeSeparators(path)));
        return;
    }

    const MemoryCsvParseResult parsed = MemoryCsvCompat::parse(file.readAll());

    struct ImportState {
        QList<MemoryCsvRecord> records;
        QStringList issues;
        QString fileName;
        int nextRecord{0};
        int processedCount{0};
        int importedCount{0};
    };

    const auto state = QSharedPointer<ImportState>::create();
    state->records = parsed.records;
    state->issues = parsed.errors;
    state->fileName = QFileInfo(path).fileName();

    const QPointer<MemoryDialog> dialogGuard(this);
    auto showSummary = [dialogGuard, state]() {
        if (!dialogGuard)
            return;

        dialogGuard->populateTable();

        QMessageBox summary(dialogGuard);
        summary.setWindowTitle("Import Memories");
        summary.setIcon(state->issues.isEmpty() ? QMessageBox::Information : QMessageBox::Warning);
        summary.setText(QString("Imported %1 %2 from %3.")
                            .arg(state->importedCount)
                            .arg(state->importedCount == 1 ? "record" : "records")
                            .arg(state->fileName));
        if (!state->issues.isEmpty()) {
            summary.setInformativeText(
                QString("%1 %2 skipped or couldn't be imported. See Details for the row names and messages.")
                    .arg(state->issues.size())
                    .arg(state->issues.size() == 1 ? "row was" : "rows were"));
            summary.setDetailedText(state->issues.join('\n'));
        }
        summary.exec();
        if (dialogGuard)
            dialogGuard->focusTableOnCurrentRow();
    };

    if (state->records.isEmpty()) {
        showSummary();
        return;
    }

    auto* progressDialog = new QProgressDialog(
        QString("Importing 0 of %1 memories...").arg(state->records.size()),
        QString(), 0, state->records.size(), this);
    auto* progressBar = new QProgressBar(progressDialog);
    progressBar->setTextVisible(false);
    progressDialog->setBar(progressBar);
    progressDialog->setWindowTitle("Import Memories");
    progressDialog->setWindowModality(Qt::WindowModal);
    progressDialog->setMinimumDuration(0);
    progressDialog->setAutoClose(false);
    progressDialog->setAutoReset(false);
    progressDialog->setCancelButton(nullptr);
    progressDialog->setWindowFlag(Qt::WindowCloseButtonHint, false);
    progressDialog->setMinimumWidth(420);
    progressDialog->setValue(0);
    progressDialog->show();

    RadioModel* const model = m_model;
    const QPointer<QProgressDialog> progressGuard(progressDialog);
    auto advanceProgress = [state, progressGuard]() {
        ++state->processedCount;
        if (progressGuard)
            progressGuard->setValue(state->processedCount);
    };
    const auto runNext = QSharedPointer<std::function<void()>>::create();
    *runNext = [model, dialogGuard, state, runNext, showSummary, progressGuard, advanceProgress]() {
        if (!dialogGuard)
            return;

        if (state->nextRecord >= state->records.size()) {
            if (progressGuard) {
                progressGuard->setValue(progressGuard->maximum());
                progressGuard->close();
            }
            showSummary();
            return;
        }

        const MemoryCsvRecord record = state->records.at(state->nextRecord++);
        const QString rowLabel = describeImportedMemory(record.memory);
        const MemoryUpdateData update = buildMemoryUpdateData(record.memory);
        if (progressGuard) {
            const int currentRecord = state->processedCount + 1;
            const int percent = state->records.isEmpty()
                ? 0
                : qRound((double(currentRecord) * 100.0) / double(state->records.size()));
            const QString percentText = QString::number(percent) + QLatin1Char('%');
            progressGuard->setLabelText(
                QString("Importing %1 of %2 memories (%3)...\n\n%4")
                    .arg(currentRecord)
                    .arg(state->records.size())
                    .arg(percentText)
                    .arg(rowLabel));
        }

        model->sendCmdPublic("memory create",
            [model, dialogGuard, state, runNext, rowLabel, update, advanceProgress](int code, const QString& body) {
            if (!dialogGuard)
                return;

            if (code != 0) {
                state->issues << formatImportIssue(
                    rowLabel,
                    "couldn't create a new memory on the radio",
                    body);
                advanceProgress();
                (*runNext)();
                return;
            }

            bool ok = false;
            const int idx = body.trimmed().toInt(&ok);
            if (!ok) {
                state->issues << formatImportIssue(
                    rowLabel,
                    "the radio returned an unrecognized memory id",
                    body);
                advanceProgress();
                (*runNext)();
                return;
            }

            model->sendCmdPublic(
                QString("memory set %1 %2").arg(idx).arg(update.commandSuffix),
                [model, dialogGuard, state, runNext, rowLabel, update, idx, advanceProgress](int setCode, const QString& setBody) {
                if (!dialogGuard)
                    return;

                if (setCode == 0) {
                    model->handleMemoryStatus(idx, update.kvs);
                    ++state->importedCount;
                    advanceProgress();
                    (*runNext)();
                    return;
                }

                state->issues << formatImportIssue(
                    rowLabel,
                    "the radio rejected one or more imported fields",
                    setBody);

                model->sendCmdPublic(QString("memory remove %1").arg(idx),
                    [model, dialogGuard, state, runNext, rowLabel, idx, advanceProgress](int removeCode, const QString& removeBody) {
                    if (!dialogGuard)
                        return;

                    if (removeCode == 0) {
                        QMap<QString, QString> kvs;
                        kvs["removed"] = QString{};
                        model->handleMemoryStatus(idx, kvs);
                    } else {
                        state->issues << formatImportIssue(
                            rowLabel,
                            "the partially created memory couldn't be cleaned up automatically",
                            removeBody);
                    }

                    advanceProgress();
                    (*runNext)();
                });
            });
        });
    };

    (*runNext)();
}

void MemoryDialog::onSelect()
{
    if (selectedMemoryIndices().size() != 1)
        return;

    activateMemoryRow(m_table->currentRow());
}

void MemoryDialog::onEdit()
{
    if (!m_table || selectedMemoryIndices().size() != 1)
        return;

    auto* indexItem = m_table->item(m_table->currentRow(), 0);
    if (!indexItem)
        return;

    beginEditingMemoryName(indexItem->data(Qt::UserRole).toInt());
}

void MemoryDialog::onSelectAll()
{
    if (!m_table || m_table->rowCount() <= 0)
        return;

    setInlineEditMode(false);
    m_table->selectAll();
    if (m_table->currentRow() < 0)
        m_table->setCurrentCell(0, 0, QItemSelectionModel::NoUpdate);
    focusTableOnCurrentRow();
    updateSelectionActions();
}

void MemoryDialog::onRemove()
{
    const QSet<int> selectedIndices = selectedMemoryIndices();
    if (selectedIndices.isEmpty())
        return;

    setInlineEditMode(false);
    QList<int> indices = selectedIndices.values();
    std::sort(indices.begin(), indices.end());

    QStringList memoryDescriptions;
    memoryDescriptions.reserve(indices.size());
    for (int idx : indices) {
        const auto it = m_model->memories().constFind(idx);
        memoryDescriptions << (it != m_model->memories().constEnd()
            ? describeMemory(it.value())
            : QString("Memory %1").arg(idx));
    }

    QMessageBox confirm(this);
    confirm.setIcon(QMessageBox::Warning);
    confirm.setWindowTitle(indices.size() == 1 ? "Delete Memory" : "Delete Memories");
    confirm.setText(indices.size() == 1
        ? QString("Delete %1?").arg(memoryDescriptions.value(0, "the selected memory"))
        : QString("Delete %1 selected memories?").arg(indices.size()));
    confirm.setInformativeText("This can't be undone.");
    if (memoryDescriptions.size() > 1)
        confirm.setDetailedText(memoryDescriptions.join('\n'));
    confirm.setStandardButtons(QMessageBox::Yes | QMessageBox::Cancel);
    confirm.setDefaultButton(QMessageBox::Cancel);
    if (confirm.exec() != QMessageBox::Yes) {
        focusTableOnCurrentRow();
        return;
    }

    RadioModel* const model = m_model;
    const QPointer<MemoryDialog> dialogGuard(this);
    struct RemovalState {
        QList<int> indices;
        QStringList descriptions;
        int nextIndex{0};
        int processedCount{0};
        int failed{0};
        QStringList failedDescriptions;
    };
    const auto state = QSharedPointer<RemovalState>::create();
    state->indices = indices;
    state->descriptions = memoryDescriptions;

    auto* progressDialog = new QProgressDialog(
        QString("Deleting 0 of %1 memories...").arg(indices.size()),
        QString(), 0, indices.size(), this);
    auto* progressBar = new QProgressBar(progressDialog);
    progressBar->setTextVisible(false);
    progressDialog->setBar(progressBar);
    progressDialog->setWindowTitle(indices.size() == 1 ? "Delete Memory" : "Delete Memories");
    progressDialog->setWindowModality(Qt::WindowModal);
    progressDialog->setMinimumDuration(0);
    progressDialog->setAutoClose(false);
    progressDialog->setAutoReset(false);
    progressDialog->setCancelButton(nullptr);
    progressDialog->setWindowFlag(Qt::WindowCloseButtonHint, false);
    progressDialog->setMinimumWidth(420);
    progressDialog->setValue(0);
    progressDialog->show();

    const QPointer<QProgressDialog> progressGuard(progressDialog);
    auto advanceProgress = [state, progressGuard]() {
        ++state->processedCount;
        if (progressGuard)
            progressGuard->setValue(state->processedCount);
    };

    const auto runNext = QSharedPointer<std::function<void()>>::create();
    *runNext = [model, dialogGuard, state, runNext, progressGuard, advanceProgress]() {
        if (!dialogGuard)
            return;

        if (state->nextIndex >= state->indices.size()) {
            if (progressGuard) {
                progressGuard->setValue(progressGuard->maximum());
                progressGuard->close();
            }
            dialogGuard->populateTable();
            if (state->failed > 0) {
                QMessageBox failure(dialogGuard);
                failure.setIcon(QMessageBox::Warning);
                failure.setWindowTitle(state->failed == 1 ? "Delete Memory" : "Delete Memories");
                failure.setText(state->failed == 1
                    ? QString("Couldn't delete %1.").arg(state->failedDescriptions.value(0))
                    : QString("Couldn't delete %1 memories.").arg(state->failed));
                if (state->failedDescriptions.size() > 1)
                    failure.setDetailedText(state->failedDescriptions.join('\n'));
                failure.exec();
            }
            dialogGuard->focusTableOnCurrentRow();
            return;
        }

        const int idx = state->indices.at(state->nextIndex);
        const QString description = state->descriptions.value(
            state->nextIndex, QString("Memory %1").arg(idx));
        ++state->nextIndex;

        if (progressGuard) {
            const int currentRecord = state->processedCount + 1;
            const int percent = state->indices.isEmpty()
                ? 0
                : qRound((double(currentRecord) * 100.0) / double(state->indices.size()));
            const QString percentText = QString::number(percent) + QLatin1Char('%');
            progressGuard->setLabelText(
                QString("Deleting %1 of %2 memories (%3)...\n\n%4")
                    .arg(currentRecord)
                    .arg(state->indices.size())
                    .arg(percentText)
                    .arg(description));
        }

        model->sendCmdPublic(QString("memory remove %1").arg(idx),
            [model, dialogGuard, idx, description, state, runNext, advanceProgress](int code, const QString&) {
            if (code == 0) {
                QMap<QString, QString> kvs;
                kvs["removed"] = QString{};
                model->handleMemoryStatus(idx, kvs);
            } else {
                ++state->failed;
                state->failedDescriptions << description;
            }
            advanceProgress();
            if (dialogGuard)
                (*runNext)();
        });
    };

    (*runNext)();
}

void MemoryDialog::rebuildFilterCombo()
{
    const QSignalBlocker blocker(m_filterCombo);
    const QString previous = m_filterCombo->currentData().toString();
    m_filterCombo->clear();

    // "All" shows every memory regardless of group
    m_filterCombo->addItem("All Memories", QString());

    // Collect unique profile names from global and transmit profiles
    QStringList profileNames;
    for (const QString& p : m_model->globalProfiles()) {
        if (!profileNames.contains(p)) {
            profileNames.append(p);
        }
    }
    for (const QString& p : m_model->transmitModel().profileList()) {
        if (!profileNames.contains(p)) {
            profileNames.append(p);
        }
    }
    profileNames.sort(Qt::CaseInsensitive);

    for (const QString& name : profileNames) {
        m_filterCombo->addItem(name, name);
    }

    // Restore previous selection if still present
    int idx = m_filterCombo->findData(previous);
    if (idx >= 0) {
        m_filterCombo->setCurrentIndex(idx);
    }
}

QSet<int> MemoryDialog::selectedMemoryIndices() const
{
    QSet<int> indices;
    if (!m_table || !m_table->selectionModel())
        return indices;

    const QModelIndexList rows = m_table->selectionModel()->selectedRows(0);
    for (const QModelIndex& row : rows)
        indices.insert(row.data(Qt::UserRole).toInt());
    return indices;
}

void MemoryDialog::updateSelectionActions()
{
    const int selectedCount = selectedMemoryIndices().size();
    const int visibleCount = m_table ? m_table->rowCount() : 0;
    if (selectedCount != 1 && m_inlineEditMode)
        setInlineEditMode(false);
    if (m_selectionLabel) {
        m_selectionLabel->setText(QString("%1 of %2 selected").arg(selectedCount).arg(visibleCount));
    }
    if (m_editBtn) {
        m_editBtn->setEnabled(selectedCount == 1);
        m_editBtn->setToolTip(selectedCount == 1
            ? "Edit the highlighted memory fields."
            : "Edit is available when exactly one memory is highlighted.");
    }
    if (m_selectBtn) {
        m_selectBtn->setEnabled(selectedCount == 1);
        m_selectBtn->setToolTip(selectedCount == 1
            ? QString()
            : "Tune is available when exactly one memory is highlighted.");
    }
    if (m_selectAllBtn) {
        m_selectAllBtn->setEnabled(visibleCount > 0 && selectedCount < visibleCount);
        m_selectAllBtn->setToolTip(visibleCount > 0
            ? "Select every memory in the current search/filter result."
            : QString());
    }
    if (m_removeBtn) {
        m_removeBtn->setEnabled(selectedCount > 0);
        m_removeBtn->setText(selectedCount > 1 ? "Remove Selected" : "Remove");
    }
}

} // namespace AetherSDR
