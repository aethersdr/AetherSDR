#include "ShortcutDialog.h"
#include "CanonIndicators.h"
#include "CanonMessageBox.h"
#include "ComboStyle.h"
#include "FramelessResizer.h"
#include "KeyboardMapWidget.h"
#include "ModemChrome.h"
#include "ScopedChildWidget.h"
#include "core/AppSettings.h"
#include "core/ShortcutManager.h"
#include "core/ThemeManager.h"

#include <QBoxLayout>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStandardPaths>
#include <QTableWidget>

namespace AetherSDR {

namespace {

// The canon dialog sheet, with what this window adds: popups (the combos'
// lists, the Filter field's right-click menu) get their panel back from the
// sheet's transparent ground, and the table sits on a nested card.
QString shortcutDialogStyleSheet()
{
    return ModemChrome::styleSheet(ModemChrome::Scale::Dialog, ModemChrome::Look::Canon)
           + QStringLiteral(R"(
QTableWidget {
    background: {{color.canon.nested}};
    border: 1px solid {{color.canon.line}};
    border-radius: 6px;
    selection-color: {{color.canon.ink}};
    selection-background-color: {{color.canon.control}};
}
QTableWidget::item:selected {
    color: {{color.canon.cyan}};
    background: {{color.canon.control}};
}
QComboBox QAbstractItemView {
    color: {{color.canon.inkSoft}};
    background: {{color.canon.nested}};
    border: 1px solid {{color.canon.lineHi}};
    selection-background-color: {{color.canon.cyan}};
    selection-color: {{color.canon.onAccent}};
}
QMenu {
    color: {{color.canon.inkSoft}};
    background: {{color.canon.raised}};
    border: 1px solid {{color.canon.lineHi}};
}
QMenu::item {
    padding: 4px 18px;
}
QMenu::item:selected {
    color: {{color.canon.onAccent}};
    background: {{color.canon.cyan}};
}
QMenu::item:disabled {
    color: {{color.control.unavailable}};
}
QMenu::separator {
    height: 1px;
    margin: 4px 8px;
    background: {{color.canon.line}};
}
QScrollBar:horizontal {
    background: transparent;
    height: 12px;
    margin: 2px 8px 2px 8px;
    border-radius: 6px;
}
QScrollBar::handle:horizontal {
    background: {{color.canon.lineHi}};
    border-radius: 5px;
    min-width: 34px;
}
QScrollBar::add-line:horizontal,
QScrollBar::sub-line:horizontal {
    width: 0px;
}
QLabel#shortcutKeyLabel {
    color: {{color.canon.cyan}};
    font-weight: bold;
}
QLabel[shortcutRole="legend"] {
    color: {{color.canon.inkSoft}};
    font-size: 11px;
}
QLabel#shortcutCategoryLabel {
    color: {{color.canon.muted}};
}
QComboBox::drop-down {
    subcontrol-origin: border;
    subcontrol-position: top right;
    width: 22px;
    border: none;
}
)") + comboArrowRule();
}

// A notice with one OK button, over the editor.
void showNotice(QWidget* parent, const QString& title, const QString& text,
                const QString& info = {}, const QString& details = {})
{
    ScopedChildWidget<CanonMessageBox> box(title, text, parent);
    box.get()->setInformativeText(info);
    box.get()->setDetailedText(details);
    box.get()->setDefaultButton(box.get()->addButton(QStringLiteral("OK"),
                                                     CanonMessageBox::Role::Accept));
    box.get()->exec();
}

// A question whose accepting button is `accept`; true only when it was clicked.
bool confirm(QWidget* parent, const QString& title, const QString& text,
             const QString& accept, CanonMessageBox::Role role)
{
    ScopedChildWidget<CanonMessageBox> box(title, text, parent);
    QPushButton* cancel = box.get()->addButton(QStringLiteral("Cancel"),
                                               CanonMessageBox::Role::Reject);
    QPushButton* go = box.get()->addButton(accept, role);
    box.get()->setDefaultButton(role == CanonMessageBox::Role::Destructive ? cancel : go);
    box.get()->exec();
    return box && box.get()->clickedButton() == go;
}

} // namespace

ShortcutDialog::ShortcutDialog(ShortcutManager* mgr, QWidget* parent)
    : CanonWindow(QStringLiteral("Keyboard Shortcuts"), parent), m_mgr(mgr)
{
    theme::setContainer(this, QStringLiteral("canon/shortcuts"));
    // 700 px tall fits a 768 px screen's usable height.
    setMinimumSize(950, 700);
    resize(1100, 840);
    // A page of controls, not a form: Return in the filter field must not
    // press Clear or Reset to Default.
    setReturnClicksDefault(false);
    FramelessResizer::install(this);
    applyCanonSheet(bodyWidget(), [] { return shortcutDialogStyleSheet(); });
    buildUI();
    populateTable();
}

void ShortcutDialog::buildUI()
{
    auto* root = new QVBoxLayout(bodyWidget());
    root->setContentsMargins(14, 12, 14, 14);
    root->setSpacing(10);
    root->addWidget(makeCanonHeader(QStringLiteral("Keyboard Shortcuts")));

    // ── Keyboard map ────────────────────────────────────────────────────
    m_keyboardMap = new KeyboardMapWidget(m_mgr, this);
    root->addWidget(m_keyboardMap);
    connect(m_keyboardMap, &KeyboardMapWidget::keySelected,
            this, [this](Qt::Key key) { onKeySelected(key); });

    // ── Legend ──────────────────────────────────────────────────────────
    auto* legendRow = new QHBoxLayout;
    legendRow->setSpacing(12);
    for (const auto& cat : ShortcutManager::categories()) {
        auto* swatch = new QLabel;
        swatch->setFixedSize(12, 12);
        QColor c = m_keyboardMap->categoryColor(cat);
        swatch->setStyleSheet(QString("background: %1; border: 1px solid %2; border-radius: 2px;")
            .arg(c.name(), c.lighter(140).name()));
        legendRow->addWidget(swatch);
        auto* lbl = new QLabel(cat);
        lbl->setProperty("shortcutRole", QStringLiteral("legend"));
        legendRow->addWidget(lbl);
    }
    legendRow->addStretch();
    root->addLayout(legendRow);
    // The editing rows stand a little apart from the map and its legend.
    root->addSpacing(6);

    // ── Selected key info ───────────────────────────────────────────────
    auto* selGroup = new QHBoxLayout;
    selGroup->setSpacing(10);

    selGroup->addWidget(new QLabel("Key:"));
    m_selectedKeyLabel = new QLabel("(none)");
    m_selectedKeyLabel->setObjectName(QStringLiteral("shortcutKeyLabel"));
    m_selectedKeyLabel->setFixedWidth(80);
    selGroup->addWidget(m_selectedKeyLabel);
    selGroup->addSpacing(8);

    selGroup->addWidget(new QLabel("Action:"));
    m_actionCombo = new QComboBox;
    m_actionCombo->setMinimumWidth(250);
    m_actionCombo->addItem("(none)", "");
    for (const auto& a : m_mgr->actions())
        m_actionCombo->addItem(QString("[%1] %2").arg(a.category, a.displayName), a.id);
    connect(m_actionCombo, &QComboBox::activated, this, [this](int idx) {
        QString actionId = m_actionCombo->itemData(idx).toString();
        assignAction(actionId);
    });
    selGroup->addWidget(m_actionCombo);

    m_categoryLabel = new QLabel;
    m_categoryLabel->setObjectName(QStringLiteral("shortcutCategoryLabel"));
    selGroup->addWidget(m_categoryLabel);
    selGroup->addSpacing(8);

    auto* clearBtn = new QPushButton("Clear");
    connect(clearBtn, &QPushButton::clicked, this, &ShortcutDialog::clearSelected);
    selGroup->addWidget(clearBtn);

    auto* resetBtn = new QPushButton("Reset to Default");
    connect(resetBtn, &QPushButton::clicked, this, &ShortcutDialog::resetSelected);
    selGroup->addWidget(resetBtn);

    selGroup->addStretch();
    root->addLayout(selGroup);

    // ── Action table ────────────────────────────────────────────────────
    auto* filterRow = new QHBoxLayout;
    filterRow->setSpacing(10);
    filterRow->addWidget(new QLabel("Filter:"));
    m_filterEdit = new QLineEdit;
    m_filterEdit->setPlaceholderText("Search actions...");
    m_filterEdit->setFixedWidth(200);
    connect(m_filterEdit, &QLineEdit::textChanged, this, [this](const QString& text) {
        populateTable(text, m_categoryFilter->currentText());
    });
    filterRow->addWidget(m_filterEdit);
    filterRow->addSpacing(8);

    filterRow->addWidget(new QLabel("Category:"));
    m_categoryFilter = new QComboBox;
    m_categoryFilter->addItem("All");
    for (const auto& cat : ShortcutManager::categories())
        m_categoryFilter->addItem(cat);
    connect(m_categoryFilter, &QComboBox::currentTextChanged, this, [this](const QString& cat) {
        populateTable(m_filterEdit->text(), cat);
    });
    filterRow->addWidget(m_categoryFilter);
    filterRow->addStretch();
    root->addLayout(filterRow);

    m_table = new QTableWidget;
    m_table->setColumnCount(4);
    m_table->setHorizontalHeaderLabels({"Action", "Category", "Current Key", "Default Key"});
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->verticalHeader()->setVisible(false);
    connect(m_table, &QTableWidget::currentCellChanged, this, [this](int row) {
        onTableRowSelected(row);
    });
    root->addWidget(m_table, 1);

    // ── Bottom buttons ──────────────────────────────────────────────────
    auto* bottomRow = new QHBoxLayout;

    auto* importBtn = new QPushButton("Import...");
    importBtn->setAutoDefault(false);
    importBtn->setObjectName(QStringLiteral("shortcutImportButton"));
    importBtn->setAccessibleName(QStringLiteral("Import keyboard shortcuts"));
    importBtn->setAccessibleDescription(
        QStringLiteral("Import an AetherSDR keyboard shortcut CSV backup"));
    connect(importBtn, &QPushButton::clicked, this, &ShortcutDialog::importShortcuts);
    bottomRow->addWidget(importBtn);

    auto* exportBtn = new QPushButton("Export...");
    exportBtn->setAutoDefault(false);
    exportBtn->setObjectName(QStringLiteral("shortcutExportButton"));
    exportBtn->setAccessibleName(QStringLiteral("Export keyboard shortcuts"));
    exportBtn->setAccessibleDescription(
        QStringLiteral("Export keyboard shortcuts to a portable CSV backup"));
    connect(exportBtn, &QPushButton::clicked, this, &ShortcutDialog::exportShortcuts);
    bottomRow->addWidget(exportBtn);

    auto* resetAllBtn = new QPushButton("Reset All to Defaults");
    resetAllBtn->setObjectName(QStringLiteral("shortcutResetAllButton"));
    resetAllBtn->setAccessibleName(QStringLiteral("Reset all shortcuts to defaults"));
    connect(resetAllBtn, &QPushButton::clicked, this, [this]() {
        if (confirm(this, QStringLiteral("Reset Shortcuts"),
                    QStringLiteral("Reset all keyboard shortcuts to their defaults?"),
                    QStringLiteral("Reset All"), CanonMessageBox::Role::Destructive)) {
            m_mgr->resetToDefaults();
            populateTable(m_filterEdit->text(), m_categoryFilter->currentText());
            updateSelectedKeyInfo();
        }
    });
    bottomRow->addWidget(resetAllBtn);

    bottomRow->addStretch();

    auto* closeBtn = new QPushButton("Close");
    closeBtn->setObjectName(QStringLiteral("shortcutCloseButton"));
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);
    bottomRow->addWidget(closeBtn);

    root->addLayout(bottomRow);
}

namespace {

// Remember the last directory the user picked so import/export share it and
// don't reset to Documents on every invocation — matches the profile
// import/export dialog's pattern.
QString shortcutTransferDirectory()
{
    const QString saved = AppSettings::instance()
                              .value(QStringLiteral("ShortcutImportExportPath"), QString())
                              .toString();
    if (!saved.isEmpty() && QDir(saved).exists()) {
        return saved;
    }
    const QString docs = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (!docs.isEmpty()) {
        return docs;
    }
    return QDir::homePath();
}

void rememberShortcutTransferDirectory(const QString& path)
{
    const QFileInfo info(path);
    if (!info.absolutePath().isEmpty()) {
        AppSettings::instance().setValue(QStringLiteral("ShortcutImportExportPath"),
                                         info.absolutePath());
    }
}

} // namespace

void ShortcutDialog::importShortcuts()
{
    QFileDialog dialog(this, QStringLiteral("Import Keyboard Shortcuts"),
                       shortcutTransferDirectory(),
                       QStringLiteral("CSV Files (*.csv)"));
    dialog.setAcceptMode(QFileDialog::AcceptOpen);
    dialog.setFileMode(QFileDialog::ExistingFile);
    dialog.setDefaultSuffix(QStringLiteral("csv"));
    if (dialog.exec() != QDialog::Accepted || dialog.selectedFiles().isEmpty()) {
        return;
    }
    const QString path = dialog.selectedFiles().first();
    rememberShortcutTransferDirectory(path);

    const ShortcutImportResult result = m_mgr->importFromFile(path);
    if (!result.ok()) {
        showNotice(this, QStringLiteral("Import Keyboard Shortcuts"),
                   QStringLiteral("No shortcuts were imported from %1.")
                       .arg(QFileInfo(path).fileName()),
                   {}, result.errors.join(QLatin1Char('\n')));
        return;
    }

    populateTable(m_filterEdit->text(), m_categoryFilter->currentText());
    updateSelectedKeyInfo();

    QStringList informativeLines;
    QStringList detailLines;
    if (!result.unknownActions.isEmpty()) {
        informativeLines
            << QStringLiteral("%1 actions are not available in this AetherSDR release and were skipped.")
                   .arg(result.unknownActions.size());
        detailLines << QStringLiteral("Skipped:");
        detailLines << result.unknownActions;
    }
    if (!result.displacedActions.isEmpty()) {
        informativeLines
            << QStringLiteral("%1 local binding(s) were cleared because an imported customized shortcut takes their key.")
                   .arg(result.displacedActions.size());
        if (!detailLines.isEmpty()) detailLines << QString();
        detailLines << QStringLiteral("Displaced local bindings:");
        detailLines << result.displacedActions;
    }
    // A canon box has no warning icon, so the text says it.
    showNotice(this, QStringLiteral("Import Keyboard Shortcuts"),
               (informativeLines.isEmpty()
                    ? QStringLiteral("Imported %1 shortcut actions from %2.")
                    : QStringLiteral("Imported %1 shortcut actions from %2, with warnings."))
                   .arg(result.importedCount)
                   .arg(QFileInfo(path).fileName()),
               informativeLines.join(QLatin1Char('\n')),
               detailLines.join(QLatin1Char('\n')));
}

void ShortcutDialog::exportShortcuts()
{
    // yyyyMMdd_HHmmss so two exports the same day don't suggest the identical
    // filename (one Enter would silently overwrite the earlier backup).
    const QString fileName = QStringLiteral("AetherSDR_Shortcuts_%1_v%2.csv")
                                 .arg(QDateTime::currentDateTime().toString(
                                          QStringLiteral("yyyyMMdd_HHmmss")),
                                      QCoreApplication::applicationVersion());
    QFileDialog dialog(this, QStringLiteral("Export Keyboard Shortcuts"),
                       QDir(shortcutTransferDirectory()).filePath(fileName),
                       QStringLiteral("CSV Files (*.csv)"));
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setDefaultSuffix(QStringLiteral("csv"));
    if (dialog.exec() != QDialog::Accepted || dialog.selectedFiles().isEmpty()) {
        return;
    }
    const QString path = dialog.selectedFiles().first();
    rememberShortcutTransferDirectory(path);

    const ShortcutExportResult result = m_mgr->exportToFile(path);
    if (!result.ok()) {
        showNotice(this, QStringLiteral("Export Keyboard Shortcuts"), result.error);
        return;
    }
    showNotice(this, QStringLiteral("Export Keyboard Shortcuts"),
               QStringLiteral("Exported %1 shortcut actions to %2.")
                   .arg(result.exportedCount)
                   .arg(QFileInfo(path).fileName()));
}

void ShortcutDialog::populateTable(const QString& filter, const QString& category)
{
    m_table->setRowCount(0);
    for (const auto& a : m_mgr->actions()) {
        if (!category.isEmpty() && category != "All" && a.category != category)
            continue;
        if (!filter.isEmpty() &&
            !a.displayName.contains(filter, Qt::CaseInsensitive) &&
            !a.category.contains(filter, Qt::CaseInsensitive))
            continue;

        int row = m_table->rowCount();
        m_table->insertRow(row);
        auto* nameItem = new QTableWidgetItem(a.displayName);
        nameItem->setData(Qt::UserRole, a.id);
        m_table->setItem(row, 0, nameItem);
        m_table->setItem(row, 1, new QTableWidgetItem(a.category));
        m_table->setItem(row, 2, new QTableWidgetItem(a.currentKey.toString()));
        m_table->setItem(row, 3, new QTableWidgetItem(a.defaultKey.toString()));
    }
}

void ShortcutDialog::onKeySelected(Qt::Key key)
{
    Q_UNUSED(key);
    updateSelectedKeyInfo();
}

void ShortcutDialog::onTableRowSelected(int row)
{
    if (row < 0 || row >= m_table->rowCount()) return;
    auto* item = m_table->item(row, 0);
    if (!item) return;
    QString actionId = item->data(Qt::UserRole).toString();
    auto* act = m_mgr->action(actionId);
    if (!act || act->currentKey.isEmpty()) return;

    // Highlight the corresponding key on the keyboard
    // Find the key index that matches this action's key
    m_keyboardMap->update();
}

void ShortcutDialog::assignAction(const QString& actionId)
{
    Qt::Key key = m_keyboardMap->selectedKey();
    if (key == Qt::Key_unknown) return;

    QKeySequence seq(key);

    if (actionId.isEmpty()) {
        // Clearing: find what action is on this key and clear it
        const auto* act = m_mgr->actionForKey(seq);
        if (act)
            m_mgr->clearBinding(act->id);
    } else {
        // Check for conflict
        QString conflict = m_mgr->conflictCheck(seq, actionId);
        if (!conflict.isEmpty()) {
            if (!confirm(this, QStringLiteral("Key Conflict"),
                         QString("Key [%1] is currently bound to '%2'.\nReassign it?")
                             .arg(seq.toString(), conflict),
                         QStringLiteral("Reassign"), CanonMessageBox::Role::Accept)) {
                updateSelectedKeyInfo();   // the combo goes back to the key's own action
                return;
            }
        }
        m_mgr->setBinding(actionId, seq);
    }

    populateTable(m_filterEdit->text(), m_categoryFilter->currentText());
    updateSelectedKeyInfo();
}

void ShortcutDialog::clearSelected()
{
    Qt::Key key = m_keyboardMap->selectedKey();
    if (key == Qt::Key_unknown) return;
    const auto* act = m_mgr->actionForKey(QKeySequence(key));
    if (act) {
        m_mgr->clearBinding(act->id);
        populateTable(m_filterEdit->text(), m_categoryFilter->currentText());
        updateSelectedKeyInfo();
    }
}

void ShortcutDialog::resetSelected()
{
    Qt::Key key = m_keyboardMap->selectedKey();
    if (key == Qt::Key_unknown) return;
    const auto* act = m_mgr->actionForKey(QKeySequence(key));
    if (act) {
        m_mgr->setBinding(act->id, act->defaultKey);
        populateTable(m_filterEdit->text(), m_categoryFilter->currentText());
        updateSelectedKeyInfo();
    }
}

void ShortcutDialog::updateSelectedKeyInfo()
{
    Qt::Key key = m_keyboardMap->selectedKey();
    if (key == Qt::Key_unknown) {
        m_selectedKeyLabel->setText("(none)");
        m_actionCombo->setCurrentIndex(0);
        m_categoryLabel->clear();
        return;
    }

    QKeySequence seq(key);
    m_selectedKeyLabel->setText(seq.toString());

    const auto* act = m_mgr->actionForKey(seq);
    if (act) {
        // Find the combo index for this action
        int idx = m_actionCombo->findData(act->id);
        if (idx >= 0) m_actionCombo->setCurrentIndex(idx);
        m_categoryLabel->setText(act->category);
    } else {
        m_actionCombo->setCurrentIndex(0);
        m_categoryLabel->clear();
    }
}

} // namespace AetherSDR
