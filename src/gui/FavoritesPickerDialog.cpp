#include "FavoritesPickerDialog.h"
#include "core/ThemeManager.h"

#include <QAbstractItemModel>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QStyledItemDelegate>
#include <QVBoxLayout>

namespace AetherSDR {

namespace {

constexpr const char* kListStyleTemplate =
    "QListWidget { background: {{color.background.0}}; "
    "border: 1px solid {{color.border.strong}}; border-radius: 3px; "
    "color: {{color.text.primary}}; }"
    "QListWidget::item { padding: 4px 6px; }"
    "QListWidget::item:selected { background: {{color.accent}}; color: #000; }"
    "QListWidget::item:hover { background: {{color.background.1}}; }"
    // Canonical dark scrollbar — matches WhatsNewDialog / PanadapterApplet etc.
    "QScrollBar:vertical { background: {{color.background.0}}; width: 10px; margin: 0; }"
    "QScrollBar::handle:vertical { background: {{color.background.2}}; "
    "border-radius: 5px; min-height: 20px; }"
    "QScrollBar::handle:vertical:hover { background: {{color.background.3}}; }"
    "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }"
    "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }"
    "QScrollBar:horizontal { background: {{color.background.0}}; height: 10px; margin: 0; }"
    "QScrollBar::handle:horizontal { background: {{color.background.2}}; "
    "border-radius: 5px; min-width: 20px; }"
    "QScrollBar::handle:horizontal:hover { background: {{color.background.3}}; }"
    "QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; }"
    "QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal { background: transparent; }";

constexpr const char* kArrowButtonTemplate =
    "QPushButton { background: {{color.background.1}}; "
    "border: 1px solid {{color.border.strong}}; border-radius: 3px; "
    "color: {{color.text.primary}}; padding: 2px 6px; min-width: 28px; }"
    "QPushButton:hover { background: {{color.background.2}}; }"
    "QPushButton:disabled { color: {{color.text.disabled}}; "
    "border-color: {{color.background.1}}; }";

// Delegate that renders a "--- Favorites ---" divider below the last
// favourite row.  The divider sits inside the cell of row `splitAfterRow`
// (its sizeHint grows by `kDividerHeight`), keeping the QListWidget's
// item count unchanged — drag-drop semantics stay clean.
class FavoritesDividerDelegate : public QStyledItemDelegate {
public:
    FavoritesDividerDelegate(int splitAfterRow, QObject* parent = nullptr)
        : QStyledItemDelegate(parent), m_splitAfterRow(splitAfterRow)
    {
        const auto& tm = ThemeManager::instance();
        m_lineColor = QColor(tm.resolve("{{color.accent.dim}}").trimmed());
        m_textColor = QColor(tm.resolve("{{color.text.secondary}}").trimmed());
    }

    void paint(QPainter* p, const QStyleOptionViewItem& opt,
               const QModelIndex& idx) const override
    {
        QStyleOptionViewItem o = opt;
        if (idx.row() == m_splitAfterRow) {
            // Reserve the bottom strip for the divider; paint the item
            // text only in the upper portion.
            o.rect.setHeight(o.rect.height() - kDividerHeight);
        }
        QStyledItemDelegate::paint(p, o, idx);

        if (idx.row() == m_splitAfterRow) {
            QRect divRect = opt.rect;
            divRect.setTop(opt.rect.bottom() - kDividerHeight + 1);
            paintDivider(p, divRect);
        }
    }

    QSize sizeHint(const QStyleOptionViewItem& opt,
                   const QModelIndex& idx) const override
    {
        QSize s = QStyledItemDelegate::sizeHint(opt, idx);
        if (idx.row() == m_splitAfterRow) s.setHeight(s.height() + kDividerHeight);
        return s;
    }

private:
    static constexpr int kDividerHeight = 22;

    void paintDivider(QPainter* p, const QRect& rect) const
    {
        p->save();
        const QString label = QStringLiteral("Favorites");
        QFont f = p->font();
        f.setItalic(true);
        f.setPointSizeF(f.pointSizeF() - 1.0);
        p->setFont(f);
        const QFontMetrics fm(f);
        const int textW = fm.horizontalAdvance(label);
        const int cy = rect.center().y();
        const int gap = 8;
        const int textX = rect.left() + (rect.width() - textW) / 2;
        // Left rule
        QPen pen(m_lineColor);
        pen.setWidth(1);
        p->setPen(pen);
        p->drawLine(rect.left() + 4, cy, textX - gap, cy);
        // Right rule
        p->drawLine(textX + textW + gap, cy, rect.right() - 4, cy);
        // Label
        p->setPen(m_textColor);
        p->drawText(textX, cy + fm.ascent() / 2 - 1, label);
        p->restore();
    }

    int m_splitAfterRow;
    QColor m_lineColor;
    QColor m_textColor;
};

} // namespace

FavoritesPickerDialog::FavoritesPickerDialog(const QList<Entry>& allEntries,
                                             const QStringList& activeOrder,
                                             const QStringList& hiddenIds,
                                             int favoriteSplit,
                                             QWidget* parent)
    : PersistentDialog("Customize Button Bar",
                       "FavoritesPickerDialogGeometry",
                       parent)
    , m_favoriteSplit(favoriteSplit)
    , m_entries(allEntries)
{
    theme::setContainer(this, QStringLiteral("dialog/favoritesPicker"));
    // Default height sized so the Active list shows the full default set
    // (~19 items at ~24 px + chrome ≈ 620 px) without a scrollbar on first
    // open.  Minimum stays smaller so the user can shrink it if they want.
    setMinimumSize(440, 360);
    resize(440, 620);

    auto* root = new QVBoxLayout(bodyWidget());
    root->setSpacing(8);

    auto* helper = new QLabel(
        QStringLiteral("Active buttons appear in the bar — the top %1 fill "
                       "the row, the rest live in the drawer.  Hidden "
                       "buttons disappear from the bar and their applets "
                       "are disabled.  Drag or use the arrows to reorder.")
            .arg(favoriteSplit),
        this);
    helper->setWordWrap(true);
    helper->setStyleSheet(ThemeManager::instance().resolve(
        "QLabel { color: {{color.text.secondary}}; font-size: 11px; }"));
    root->addWidget(helper);

    auto* listsRow = new QHBoxLayout;
    listsRow->setSpacing(6);
    root->addLayout(listsRow, 1);

    // ── Active column ────────────────────────────────────────────────────────
    auto* activeCol = new QVBoxLayout;
    activeCol->setSpacing(2);
    activeCol->addWidget(new QLabel("Active", this));
    m_activeList = new QListWidget(this);
    m_activeList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_activeList->setDragDropMode(QAbstractItemView::DragDrop);
    m_activeList->setDefaultDropAction(Qt::MoveAction);
    m_activeList->setDragDropOverwriteMode(false);
    m_activeList->setItemDelegate(
        new FavoritesDividerDelegate(favoriteSplit - 1, m_activeList));
    ThemeManager::instance().applyStyleSheet(m_activeList, kListStyleTemplate);
    activeCol->addWidget(m_activeList, 1);
    listsRow->addLayout(activeCol, 1);

    // ── Move/reorder buttons column ──────────────────────────────────────────
    auto* btnCol = new QVBoxLayout;
    btnCol->setSpacing(4);
    btnCol->addStretch(1);
    m_upBtn     = new QPushButton(QString::fromUtf8("\xe2\x86\x91"), this);   // ↑
    m_downBtn   = new QPushButton(QString::fromUtf8("\xe2\x86\x93"), this);   // ↓
    m_removeBtn = new QPushButton(QString::fromUtf8("\xe2\x86\x92"), this);   // →
    m_addBtn    = new QPushButton(QString::fromUtf8("\xe2\x86\x90"), this);   // ←
    m_upBtn->setToolTip("Move selected button up");
    m_downBtn->setToolTip("Move selected button down");
    m_removeBtn->setToolTip("Hide selected button (disables its applet)");
    m_addBtn->setToolTip("Show selected button in the bar");
    for (auto* b : {m_upBtn, m_downBtn, m_removeBtn, m_addBtn}) {
        ThemeManager::instance().applyStyleSheet(b, kArrowButtonTemplate);
        b->setFixedWidth(34);
        btnCol->addWidget(b);
    }
    btnCol->addStretch(1);
    listsRow->addLayout(btnCol);

    // ── Hidden column ────────────────────────────────────────────────────────
    auto* hiddenCol = new QVBoxLayout;
    hiddenCol->setSpacing(2);
    hiddenCol->addWidget(new QLabel("Hidden", this));
    m_hiddenList = new QListWidget(this);
    m_hiddenList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_hiddenList->setDragDropMode(QAbstractItemView::DragDrop);
    m_hiddenList->setDefaultDropAction(Qt::MoveAction);
    m_hiddenList->setDragDropOverwriteMode(false);
    ThemeManager::instance().applyStyleSheet(m_hiddenList, kListStyleTemplate);
    hiddenCol->addWidget(m_hiddenList, 1);
    listsRow->addLayout(hiddenCol, 1);

    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    m_okBtn = bb->button(QDialogButtonBox::Ok);
    connect(bb, &QDialogButtonBox::accepted, this, &FavoritesPickerDialog::onAccept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::close);
    root->addWidget(bb);

    // ── Populate lists ───────────────────────────────────────────────────────
    for (const auto& id : activeOrder) {
        for (const auto& e : m_entries) {
            if (e.id == id) {
                m_activeList->addItem(makeItem(e));
                break;
            }
        }
    }
    for (const auto& id : hiddenIds) {
        for (const auto& e : m_entries) {
            if (e.id == id) {
                m_hiddenList->addItem(makeItem(e));
                break;
            }
        }
    }
    // Any registered entries not in either list go to Active (defensive).
    auto allListed = [this](const QString& id) {
        auto matches = [&id](QListWidget* w) {
            for (int i = 0; i < w->count(); ++i)
                if (w->item(i)->data(Qt::UserRole).toString() == id) return true;
            return false;
        };
        return matches(m_activeList) || matches(m_hiddenList);
    };
    for (const auto& e : m_entries) {
        if (!allListed(e.id)) m_activeList->addItem(makeItem(e));
    }

    // ── Wiring ───────────────────────────────────────────────────────────────
    connect(m_addBtn,    &QPushButton::clicked, this, &FavoritesPickerDialog::moveSelectedToActive);
    connect(m_removeBtn, &QPushButton::clicked, this, &FavoritesPickerDialog::moveSelectedToHidden);
    connect(m_upBtn,     &QPushButton::clicked, this, &FavoritesPickerDialog::moveActiveUp);
    connect(m_downBtn,   &QPushButton::clicked, this, &FavoritesPickerDialog::moveActiveDown);
    connect(m_activeList, &QListWidget::itemSelectionChanged, this, &FavoritesPickerDialog::refreshState);
    connect(m_hiddenList, &QListWidget::itemSelectionChanged, this, &FavoritesPickerDialog::refreshState);
    connect(m_activeList->model(), &QAbstractItemModel::rowsInserted, this, &FavoritesPickerDialog::refreshState);
    connect(m_activeList->model(), &QAbstractItemModel::rowsRemoved,  this, &FavoritesPickerDialog::refreshState);
    connect(m_hiddenList->model(), &QAbstractItemModel::rowsInserted, this, &FavoritesPickerDialog::refreshState);
    connect(m_hiddenList->model(), &QAbstractItemModel::rowsRemoved,  this, &FavoritesPickerDialog::refreshState);
    connect(m_activeList, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem*) { moveSelectedToHidden(); });
    connect(m_hiddenList, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem*) { moveSelectedToActive(); });

    refreshState();
}

QListWidgetItem* FavoritesPickerDialog::makeItem(const Entry& e) const
{
    auto* item = new QListWidgetItem(e.label);
    item->setData(Qt::UserRole, e.id);
    if (!e.tooltip.isEmpty())
        item->setToolTip(QStringLiteral("%1 — %2").arg(e.id, e.tooltip));
    else
        item->setToolTip(e.id);
    return item;
}

QStringList FavoritesPickerDialog::activeOrder() const
{
    QStringList out;
    for (int i = 0; i < m_activeList->count(); ++i)
        out.append(m_activeList->item(i)->data(Qt::UserRole).toString());
    return out;
}

QStringList FavoritesPickerDialog::hiddenOrder() const
{
    QStringList out;
    for (int i = 0; i < m_hiddenList->count(); ++i)
        out.append(m_hiddenList->item(i)->data(Qt::UserRole).toString());
    return out;
}

void FavoritesPickerDialog::moveSelectedToActive()
{
    auto* item = m_hiddenList->currentItem();
    if (!item) return;
    int row = m_hiddenList->row(item);
    auto* taken = m_hiddenList->takeItem(row);
    m_activeList->addItem(taken);
    m_activeList->setCurrentItem(taken);
}

void FavoritesPickerDialog::moveSelectedToHidden()
{
    auto* item = m_activeList->currentItem();
    if (!item) return;
    int row = m_activeList->row(item);
    auto* taken = m_activeList->takeItem(row);
    m_hiddenList->addItem(taken);
    m_hiddenList->setCurrentItem(taken);
}

void FavoritesPickerDialog::moveActiveUp()
{
    int row = m_activeList->currentRow();
    if (row <= 0) return;
    auto* item = m_activeList->takeItem(row);
    m_activeList->insertItem(row - 1, item);
    m_activeList->setCurrentRow(row - 1);
}

void FavoritesPickerDialog::moveActiveDown()
{
    int row = m_activeList->currentRow();
    if (row < 0 || row >= m_activeList->count() - 1) return;
    auto* item = m_activeList->takeItem(row);
    m_activeList->insertItem(row + 1, item);
    m_activeList->setCurrentRow(row + 1);
}

void FavoritesPickerDialog::refreshState()
{
    const int activeRow = m_activeList->currentRow();
    m_upBtn->setEnabled(activeRow > 0);
    m_downBtn->setEnabled(activeRow >= 0 && activeRow < m_activeList->count() - 1);
    m_removeBtn->setEnabled(m_activeList->currentItem() != nullptr);
    m_addBtn->setEnabled(m_hiddenList->currentItem() != nullptr);
}

void FavoritesPickerDialog::onAccept()
{
    emit layoutAccepted(activeOrder(), hiddenOrder());
    close();
}

} // namespace AetherSDR
