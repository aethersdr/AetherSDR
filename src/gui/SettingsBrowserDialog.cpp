#include "SettingsBrowserDialog.h"
#include "FramelessMessageBox.h"
#include "core/AppSettings.h"
#include "core/SettingsCredentialPolicy.h"
#include "core/SettingsSanitizer.h"
#include "core/ThemeManager.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QShortcut>
#include <QShowEvent>
#include <QSplitter>
#include <QStyledItemDelegate>
#include <QTableWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <functional>

namespace AetherSDR {

namespace {

// Tree/table item data roles for the scope coordinates a row addresses.
constexpr int kRoleKind = Qt::UserRole;          // int(ScopeKind)
constexpr int kRoleFamily = Qt::UserRole + 1;
constexpr int kRoleRadioId = Qt::UserRole + 2;
constexpr int kRoleKey = Qt::UserRole + 3;       // app/station key, or feature
constexpr int kRoleSecret = Qt::UserRole + 4;    // bool: rendered redacted
// The document row's stored bytes, so the viewer can tell a CORRUPT document
// (present, unparseable) from an ABSENT one — both read back as an empty
// object through the typed API (PR #4631 review, NF0T). Feature rows only;
// never carries a flat value, so no plaintext is stashed for a copy action.
constexpr int kRoleRawValue = Qt::UserRole + 5;

const QString kDialogStyle =
    "QDialog { background: #0f0f1a; color: #c8d8e8; }"
    "QLabel { color: #c8d8e8; }"
    "QLineEdit { background: #0a0a18; border: 1px solid #1e2e3e; border-radius: 3px;"
    "  padding: 4px 6px; color: #c8d8e8; }"
    "QTreeWidget { background: #0a0a18; border: 1px solid #1e2e3e; border-radius: 3px;"
    "  color: #c8d8e8; outline: none; }"
    "QTreeWidget::item { min-height: 22px; padding: 1px 4px; }"
    "QTreeWidget::item:selected { background: #0070c0; color: #ffffff; }"
    "QTreeWidget::item:hover:!selected { background: #1a2a3a; }"
    "QTableWidget { background: #0a0a18; border: 1px solid #1e2e3e; border-radius: 3px;"
    "  color: #c8d8e8; gridline-color: #14202f;"
    "  selection-background-color: #1b3650; selection-color: #e8f4ff; }"
    "QHeaderView::section { color: #8d99ad; background: #101c2a; border: none;"
    "  border-bottom: 1px solid #233246; padding: 4px 8px; font-size: 11px; }"
    "QPlainTextEdit { background: #0a0a18; border: 1px solid #1e2e3e;"
    "  border-radius: 3px; color: #c8d8e8;"
    "  font-family: \"SF Mono\", \"Menlo\", \"Consolas\", monospace; }"
    "QPushButton { background: #1a2a3a; border: 1px solid #203040;"
    "  border-radius: 3px; padding: 4px 12px; color: #c8d8e8; }"
    "QPushButton:hover { background: #2a3a4a; }"
    "QPushButton:disabled { background: #141c26; border: 1px solid #1a2632;"
    "  color: #5a6a78; }"
    "QComboBox { background: #0a0a18; border: 1px solid #1e2e3e; border-radius: 3px;"
    "  padding: 2px 6px; color: #c8d8e8; }";

// Same result-line styling as ProfileManagerDialog / ConnectionPanel so the
// status messages read as the same kind of message app-wide.
const char* kResultInfoStyle =
    "QLabel { color: #9bd1ff; font-size: 11px; background: transparent; border: none; }";
const char* kResultErrorStyle =
    "QLabel { color: #ff8f8f; font-size: 11px; background: transparent; border: none; }";
const char* kBannerStyle =
    "QLabel { color: #ffc978; font-size: 11px; background: #241c10;"
    "  border: 1px solid #4a3a1a; border-radius: 3px; padding: 5px 8px; }";

const char* kAdvancedBannerText =
    "Advanced — edits apply immediately and bypass each feature's own "
    "validation. Prefer the feature's UI when one exists.";

// True when a stored VALUE is a JSON document holding a credential-shaped
// field at any depth. Such a row renders partially redacted, so it must be
// read-only too — otherwise committing an edit writes the "[REDACTED]"
// placeholder over the real secret (PR #4631 review, both reviewers).
bool valueHidesSecret(const QString& value)
{
    const QByteArray raw = value.toUtf8().trimmed();
    if (!raw.startsWith('{') && !raw.startsWith('[')) {
        return false;
    }
    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(raw, &parseError);
    if (parseError.error != QJsonParseError::NoError || doc.isNull()) {
        return false;
    }
    if (doc.isObject()) {
        return SettingsSanitizer::containsSecretField(doc.object());
    }
    // Top-level array: wrap so the recursive object check walks it.
    return SettingsSanitizer::containsSecretField(
        QJsonObject{{QStringLiteral("_"), doc.array()}});
}

// Values that are exactly "True"/"False" (the store's boolean convention) get
// a two-entry combo instead of free text, so a typo can't produce "Ture".
class BoolAwareValueDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem& option,
                          const QModelIndex& index) const override
    {
        const QString value = index.data(Qt::EditRole).toString();
        if (value == QLatin1String("True") || value == QLatin1String("False")) {
            auto* combo = new QComboBox(parent);
            combo->addItems({QStringLiteral("True"), QStringLiteral("False")});
            combo->setCurrentText(value);
            return combo;
        }
        return QStyledItemDelegate::createEditor(parent, option, index);
    }

    void setModelData(QWidget* editor, QAbstractItemModel* model,
                      const QModelIndex& index) const override
    {
        if (auto* combo = qobject_cast<QComboBox*>(editor)) {
            model->setData(index, combo->currentText(), Qt::EditRole);
            return;
        }
        QStyledItemDelegate::setModelData(editor, model, index);
    }
};

QString scopeLabelForRadioId(const QString& radioId)
{
    return radioId.isEmpty() ? QStringLiteral("(family-wide defaults)") : radioId;
}

// Display name for a radio scope: the nickname from its Identity document
// when one exists (family-agnostic — driven by the data, not the backend).
QString nicknameFor(const QList<AppSettings::RadioFeatureEntry>& rows,
                    const QString& family, const QString& radioId)
{
    for (const auto& row : rows) {
        if (row.family != family || row.radioId != radioId
            || row.feature != QLatin1String("Identity")) {
            continue;
        }
        const QJsonDocument doc = QJsonDocument::fromJson(row.value.toUtf8());
        return doc.object().value(QLatin1String("nickname")).toString();
    }
    return {};
}

} // namespace

SettingsBrowserDialog::SettingsBrowserDialog(QWidget* parent)
    : PersistentDialog("Settings Browser", "SettingsBrowserDialogGeometry", parent)
{
    theme::setContainer(this, QStringLiteral("dialog/settingsBrowser"));
    setMinimumSize(760, 480);
    setStyleSheet(kDialogStyle);

    auto* root = new QVBoxLayout(bodyWidget());
    root->setSpacing(9);

    m_banner = new QLabel(kAdvancedBannerText);
    m_banner->setStyleSheet(kBannerStyle);
    m_banner->setWordWrap(true);
    root->addWidget(m_banner);

    auto* splitter = new QSplitter(Qt::Horizontal);
    splitter->setChildrenCollapsible(false);

    m_tree = new QTreeWidget;
    m_tree->setHeaderHidden(true);
    m_tree->setMinimumWidth(190);
    m_tree->setAccessibleName("Settings scopes");
    m_tree->setAccessibleDescription(
        "Scopes in the settings store: application, station, and each "
        "radio's feature documents");
    connect(m_tree, &QTreeWidget::currentItemChanged, this,
            [this] { populateTable(); });
    splitter->addWidget(m_tree);

    auto* rightHost = new QWidget;
    auto* right = new QVBoxLayout(rightHost);
    right->setContentsMargins(0, 0, 0, 0);
    right->setSpacing(6);

    m_search = new QLineEdit;
    m_search->setPlaceholderText("Filter keys and values…");
    m_search->setClearButtonEnabled(true);
    m_search->setAccessibleName("Settings filter");
    m_search->setAccessibleDescription(
        "Case-insensitive substring match over the keys and values of the "
        "selected scope");
    connect(m_search, &QLineEdit::textChanged, this,
            [this] { applyFilter(); });
    right->addWidget(m_search);

    m_table = new QTableWidget;
    m_table->setColumnCount(2);
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::DoubleClicked
                             | QAbstractItemView::EditKeyPressed);
    m_table->setItemDelegateForColumn(1, new BoolAwareValueDelegate(m_table));
    m_table->setAccessibleName("Settings for selected scope");
    connect(m_table, &QTableWidget::itemChanged, this,
            &SettingsBrowserDialog::onTableItemChanged);
    // ONLY doubleClicked — not activated. Both fire from
    // QAbstractItemView::mouseDoubleClickEvent(), and `activated` also fires
    // on a SINGLE click under styles where ActivateItemOnSingleClick is true
    // (Breeze, some GTK themes), which would pop this modal on a stray click.
    // Keyboard open is bound explicitly below instead (PR #4631 review, NF0T).
    connect(m_table, &QTableWidget::cellDoubleClicked, this,
            [this](int row, int) { onTableActivated(row); });
    for (const int key : {Qt::Key_Return, Qt::Key_Enter}) {   // Enter = numpad
        auto* openDocShortcut = new QShortcut(QKeySequence(key), m_table);
        openDocShortcut->setContext(Qt::WidgetShortcut);
        connect(openDocShortcut, &QShortcut::activated, this,
                [this] { onTableActivated(m_table->currentRow()); });
    }
    connect(m_table, &QTableWidget::itemSelectionChanged, this,
            [this] { updateButtonState(); });
    right->addWidget(m_table, 1);

    splitter->addWidget(rightHost);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    root->addWidget(splitter, 1);

    m_status = new QLabel;
    m_status->setStyleSheet(kResultInfoStyle);
    m_status->setVisible(false);
    m_status->setWordWrap(true);
    root->addWidget(m_status);

    auto* buttons = new QHBoxLayout;
    m_addBtn = new QPushButton("Add Key…");
    m_addBtn->setAccessibleName("Add settings key");
    connect(m_addBtn, &QPushButton::clicked, this,
            &SettingsBrowserDialog::addKey);
    buttons->addWidget(m_addBtn);

    m_deleteBtn = new QPushButton("Delete");
    m_deleteBtn->setAccessibleName("Delete selected setting");
    connect(m_deleteBtn, &QPushButton::clicked, this,
            &SettingsBrowserDialog::deleteSelected);
    buttons->addWidget(m_deleteBtn);

    m_refreshBtn = new QPushButton("Refresh");
    m_refreshBtn->setAccessibleName("Refresh settings view");
    connect(m_refreshBtn, &QPushButton::clicked, this,
            [this] { rebuildTree(); });
    buttons->addWidget(m_refreshBtn);

    buttons->addStretch();

    m_exportBtn = new QPushButton("Export Sanitized…");
    m_exportBtn->setAccessibleName("Export sanitized settings dump");
    m_exportBtn->setAccessibleDescription(
        "Writes the secret-redacted diagnostic dump — not a restorable backup");
    connect(m_exportBtn, &QPushButton::clicked, this,
            &SettingsBrowserDialog::exportSanitized);
    buttons->addWidget(m_exportBtn);

    auto* closeBtn = new QPushButton("Close");
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);
    buttons->addWidget(closeBtn);
    root->addLayout(buttons);

    rebuildTree();
}

void SettingsBrowserDialog::showEvent(QShowEvent* event)
{
    PersistentDialog::showEvent(event);
    // The store changes underneath us (features write documents debounced) —
    // re-read on every show so a re-raised dialog isn't showing stale rows.
    rebuildTree();
}

void SettingsBrowserDialog::rebuildTree()
{
    const Scope remembered = currentScope();
    auto& s = AppSettings::instance();

    m_storeWritable = s.storeWritable();
    if (m_storeWritable) {
        m_banner->setText(kAdvancedBannerText);
    } else {
        const QString notice = s.loadNotice();
        m_banner->setText(
            "This settings store is read-only this session"
            + (notice.isEmpty() ? QString(".") : QString(": ") + notice));
    }

    m_populating = true;
    m_tree->clear();

    auto* appItem = new QTreeWidgetItem(
        m_tree, {QStringLiteral("App Settings (%1)")
                     .arg(s.allKeysForDiagnostics().size())});
    appItem->setData(0, kRoleKind, int(ScopeKind::App));

    auto* stationItem = new QTreeWidgetItem(
        m_tree, {QStringLiteral("Station: %1 (%2)")
                     .arg(s.stationName())
                     .arg(s.stationKeysForDiagnostics().size())});
    stationItem->setData(0, kRoleKind, int(ScopeKind::Station));

    const QList<AppSettings::RadioFeatureEntry> rows =
        s.radioFeatureEntriesForDiagnostics();
    if (!rows.isEmpty()) {
        auto* radiosItem = new QTreeWidgetItem(m_tree, {QStringLiteral("Radios")});
        radiosItem->setFlags(radiosItem->flags() & ~Qt::ItemIsSelectable);

        // family -> ordered unique radioIds ('' first when present)
        QMap<QString, QStringList> radiosByFamily;
        for (const auto& row : rows) {
            QStringList& ids = radiosByFamily[row.family];
            if (!ids.contains(row.radioId)) {
                ids.append(row.radioId);
            }
        }
        for (auto it = radiosByFamily.cbegin(); it != radiosByFamily.cend();
             ++it) {
            auto* familyItem = new QTreeWidgetItem(radiosItem, {it.key()});
            familyItem->setFlags(familyItem->flags() & ~Qt::ItemIsSelectable);
            QStringList ids = it.value();
            ids.sort();
            for (const QString& radioId : ids) {
                const QString nickname = nicknameFor(rows, it.key(), radioId);
                const QString label =
                    nickname.isEmpty()
                        ? scopeLabelForRadioId(radioId)
                        : QStringLiteral("%1 (%2)").arg(nickname, radioId);
                auto* radioItem = new QTreeWidgetItem(familyItem, {label});
                radioItem->setData(0, kRoleKind, int(ScopeKind::RadioScope));
                radioItem->setData(0, kRoleFamily, it.key());
                radioItem->setData(0, kRoleRadioId, radioId);
            }
        }
        radiosItem->setExpanded(true);
        for (int i = 0; i < radiosItem->childCount(); ++i) {
            radiosItem->child(i)->setExpanded(true);
        }
    }

    // Select while m_populating still holds so currentItemChanged's populate
    // no-ops — one explicit populate below, not two (PR #4631 review).
    selectScopeItem(remembered.kind == ScopeKind::None
                        ? Scope{ScopeKind::App, {}, {}}
                        : remembered);
    m_populating = false;
    populateTable();
}

SettingsBrowserDialog::Scope SettingsBrowserDialog::currentScope() const
{
    Scope scope;
    const QTreeWidgetItem* item = m_tree->currentItem();
    if (item == nullptr || item->data(0, kRoleKind).isNull()) {
        return scope;
    }
    scope.kind = ScopeKind(item->data(0, kRoleKind).toInt());
    scope.family = item->data(0, kRoleFamily).toString();
    scope.radioId = item->data(0, kRoleRadioId).toString();
    return scope;
}

void SettingsBrowserDialog::selectScopeItem(const Scope& scope)
{
    std::function<QTreeWidgetItem*(QTreeWidgetItem*)> find =
        [&](QTreeWidgetItem* item) -> QTreeWidgetItem* {
        if (item != nullptr && !item->data(0, kRoleKind).isNull()
            && ScopeKind(item->data(0, kRoleKind).toInt()) == scope.kind
            && item->data(0, kRoleFamily).toString() == scope.family
            && item->data(0, kRoleRadioId).toString() == scope.radioId) {
            return item;
        }
        for (int i = 0; item != nullptr && i < item->childCount(); ++i) {
            if (QTreeWidgetItem* hit = find(item->child(i))) {
                return hit;
            }
        }
        return nullptr;
    };
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        if (QTreeWidgetItem* hit = find(m_tree->topLevelItem(i))) {
            m_tree->setCurrentItem(hit);
            return;
        }
    }
    if (m_tree->topLevelItemCount() > 0) {
        m_tree->setCurrentItem(m_tree->topLevelItem(0));
    }
}

void SettingsBrowserDialog::populateTable()
{
    if (m_populating) {
        return;
    }
    const Scope scope = currentScope();
    auto& s = AppSettings::instance();

    m_populating = true;
    m_table->clearContents();
    m_table->setRowCount(0);

    const bool docScope = scope.kind == ScopeKind::RadioScope;
    m_table->setColumnCount(docScope ? 3 : 2);
    m_table->setHorizontalHeaderLabels(
        docScope ? QStringList{"Feature", "Schema", "Document"}
                 : QStringList{"Key", "Value"});

    auto addRow = [this](const QString& key, const QString& value,
                         bool editable) {
        const int row = m_table->rowCount();
        m_table->insertRow(row);
        // Masked = the display differs from the stored truth, whether the
        // whole row is credential-shaped or a JSON value hides a secret
        // field. Either way editing the displayed text would commit
        // "[REDACTED]" over the real value, so masked rows are read-only.
        const bool secret =
            SettingsSanitizer::isSecretKey(key) || valueHidesSecret(value);

        auto* keyItem = new QTableWidgetItem(key);
        keyItem->setFlags(keyItem->flags() & ~Qt::ItemIsEditable);
        keyItem->setData(kRoleKey, key);
        m_table->setItem(row, 0, keyItem);

        // Only masked rows go through the sanitizer. redactedValue() also
        // re-serialises clean JSON (QJsonObject sorts keys, numbers
        // normalise), so routing every row through it would leave an
        // EDITABLE row displaying something other than the stored bytes,
        // and committing would persist the re-serialisation. Masked rows
        // are read-only, so there the divergence is the point; everywhere
        // else display now equals stored truth (PR #4631, NF0T).
        auto* valueItem = new QTableWidgetItem(
            secret ? SettingsSanitizer::redactedValue(key, value) : value);
        valueItem->setData(kRoleSecret, secret);
        if (secret || !editable || !m_storeWritable) {
            valueItem->setFlags(valueItem->flags() & ~Qt::ItemIsEditable);
        }
        if (secret) {
            valueItem->setToolTip(
                "Credential-shaped values are masked and read-only here — "
                "credentials belong in the OS keychain, not this store");
        }
        m_table->setItem(row, 1, valueItem);
    };

    switch (scope.kind) {
    case ScopeKind::App: {
        const QStringList keys = s.allKeysForDiagnostics();
        for (const QString& key : keys) {
            addRow(key, s.value(key).toString(), true);
        }
        break;
    }
    case ScopeKind::Station: {
        const QStringList keys = s.stationKeysForDiagnostics();
        for (const QString& key : keys) {
            addRow(key, s.stationValue(key).toString(), true);
        }
        break;
    }
    case ScopeKind::RadioScope: {
        const QList<AppSettings::RadioFeatureEntry> rows =
            s.radioFeatureEntriesForDiagnostics();
        for (const auto& entry : rows) {
            if (entry.family != scope.family
                || entry.radioId != scope.radioId) {
                continue;
            }
            const int row = m_table->rowCount();
            m_table->insertRow(row);

            auto* featureItem = new QTableWidgetItem(entry.feature);
            featureItem->setFlags(featureItem->flags() & ~Qt::ItemIsEditable);
            featureItem->setData(kRoleKey, entry.feature);
            featureItem->setData(kRoleRawValue, entry.value);
            m_table->setItem(row, 0, featureItem);

            auto* versionItem = new QTableWidgetItem(
                QStringLiteral("v%1").arg(entry.schemaVersion));
            versionItem->setFlags(versionItem->flags() & ~Qt::ItemIsEditable);
            m_table->setItem(row, 1, versionItem);

            // Redacted like every other rendering path — the preview column
            // is a display of the document, not an exception to the policy
            // (found while fixing the viewer leak from the #4631 review).
            auto* docItem = new QTableWidgetItem(
                SettingsSanitizer::redactedValue(entry.feature, entry.value));
            docItem->setFlags(docItem->flags() & ~Qt::ItemIsEditable);
            docItem->setToolTip("Double-click to view the document");
            m_table->setItem(row, 2, docItem);
        }
        break;
    }
    case ScopeKind::None:
        break;
    }

    m_table->resizeColumnToContents(0);
    if (docScope) {
        m_table->resizeColumnToContents(1);
    }
    m_populating = false;
    applyFilter();
    updateButtonState();
}

void SettingsBrowserDialog::applyFilter()
{
    const QString needle = m_search->text().trimmed();
    for (int row = 0; row < m_table->rowCount(); ++row) {
        bool match = needle.isEmpty();
        for (int col = 0; !match && col < m_table->columnCount(); ++col) {
            const QTableWidgetItem* item = m_table->item(row, col);
            match = item != nullptr
                    && item->text().contains(needle, Qt::CaseInsensitive);
        }
        m_table->setRowHidden(row, !match);
    }
}

void SettingsBrowserDialog::onTableItemChanged(QTableWidgetItem* item)
{
    if (m_populating || item == nullptr || item->column() != 1) {
        return;
    }
    const Scope scope = currentScope();
    if (scope.kind != ScopeKind::App && scope.kind != ScopeKind::Station) {
        return;
    }
    const QTableWidgetItem* keyItem = m_table->item(item->row(), 0);
    if (keyItem == nullptr) {
        return;
    }
    const QString key = keyItem->data(kRoleKey).toString();
    const QString newValue = item->text();

    auto& s = AppSettings::instance();
    if (scope.kind == ScopeKind::App) {
        s.setValue(key, newValue);
    } else {
        s.setStationValue(key, newValue);
    }
    s.save();

    // Verify from the DATABASE FILE, not the cache: a failed save() keeps the
    // change in memory for retry, so a cache re-read would report "Saved" for
    // a write that never reached disk (PR #4631 review, NF0T). This is what
    // `--config set` does, and what AGENTS.md means by checking the write
    // result — the credential-divert case is caught either way, but a genuine
    // commit failure is only visible on disk.
    QString stored;
    const bool onDisk = scope.kind == ScopeKind::App
                            ? s.readAppRowFromDisk(key, stored)
                            : s.readStationRowFromDisk(key, stored);
    if (onDisk && stored == newValue) {
        setStatus(QStringLiteral("Saved %1").arg(key), false);
    } else {
        setStatus(QStringLiteral("%1 was NOT written to the store — value "
                                 "reverted")
                      .arg(key),
                  true);
        // Revert IN PLACE rather than repopulating: we are inside this
        // item's own itemChanged emission (rebuilding would delete the
        // emitting item mid-signal), and an in-place revert also keeps the
        // filter, selection, and scroll position (PR #4631 review).
        const QString truth = scope.kind == ScopeKind::App
                                  ? s.value(key).toString()
                                  : s.stationValue(key).toString();
        const bool wasPopulating = m_populating;
        m_populating = true;   // the revert must not re-enter this handler
        // Same display rule as addRow(): stored bytes verbatim unless the
        // row is masked (an editable row's text must equal stored truth).
        item->setText(item->data(kRoleSecret).toBool()
                          ? SettingsSanitizer::redactedValue(key, truth)
                          : truth);
        m_populating = wasPopulating;
    }
}

void SettingsBrowserDialog::onTableActivated(int row)
{
    const Scope scope = currentScope();
    if (scope.kind != ScopeKind::RadioScope) {
        return;   // app/station rows edit inline via the delegate
    }
    const QTableWidgetItem* featureItem = m_table->item(row, 0);
    if (featureItem == nullptr) {
        return;
    }
    openDocumentViewer(scope.family, scope.radioId,
                       featureItem->data(kRoleKey).toString(),
                       featureItem->data(kRoleRawValue).toString());
}

void SettingsBrowserDialog::openDocumentViewer(const QString& family,
                                               const QString& radioId,
                                               const QString& feature,
                                               const QString& rawValue)
{
    // A double-click delivers cellActivated AND cellDoubleClicked on some
    // platforms — one viewer per gesture (PR #4631 review).
    if (m_docViewerOpen) {
        return;
    }
    m_docViewerOpen = true;

    auto& s = AppSettings::instance();
    int schemaVersion = 0;
    const QJsonObject doc =
        s.radioFeatureExact(family, radioId, feature, &schemaVersion);

    // A row that is PRESENT but unparseable reads back as an empty object
    // through the typed API, indistinguishable from an absent row — and
    // saving over it would write {} at v0 across someone's damaged-but-
    // recoverable document. Detect it from the stored bytes and refuse to
    // edit (PR #4631 review, NF0T).
    QJsonParseError storedError{};
    const QJsonDocument storedDoc =
        QJsonDocument::fromJson(rawValue.toUtf8(), &storedError);
    // Mirror radioFeatureExact()'s OWN condition, including !isObject(): a
    // stored JSON *array* parses cleanly, so a parse-only check calls it
    // healthy while the API returns {} at version 0 — Edit enabled, Save
    // writing {} and downgrading the row's schema_version to 0, with no
    // guard downstream (setRadioFeature has no version check; the upsert
    // takes excluded.schema_version). Same threat model as the redaction
    // work: foreign, hand-edited, or older-build stores (PR #4631, NF0T).
    const bool corrupt = !rawValue.trimmed().isEmpty()
                         && (storedDoc.isNull()
                             || storedError.error != QJsonParseError::NoError
                             || !storedDoc.isObject());

    // Same recursive masking as the table and the sanitized dump — the
    // viewer must not be the one path that renders a credential-shaped
    // field in cleartext (PR #4631 review, nigelfenton's bench probe).
    const bool hasSecret = SettingsSanitizer::containsSecretField(doc);
    QJsonObject displayDoc = doc;
    if (hasSecret) {
        const QString redacted = SettingsSanitizer::redactedValue(
            feature, QString::fromUtf8(
                         QJsonDocument(doc).toJson(QJsonDocument::Compact)));
        displayDoc = QJsonDocument::fromJson(redacted.toUtf8()).object();
    }

    QDialog viewer(this);
    viewer.setWindowTitle(QStringLiteral("%1 — %2 / %3").arg(
        feature, family, scopeLabelForRadioId(radioId)));
    viewer.setMinimumSize(520, 420);
    viewer.setStyleSheet(kDialogStyle);
    auto* layout = new QVBoxLayout(&viewer);

    auto* header = new QLabel(
        QStringLiteral("%1 / %2 / %3 — schema v%4%5")
            .arg(family, scopeLabelForRadioId(radioId), feature)
            .arg(schemaVersion)
            .arg(corrupt
                     ? QStringLiteral("  ·  DOES NOT PARSE — read-only")
                     : hasSecret
                           ? QStringLiteral(
                                 "  ·  credential fields masked, read-only")
                           : QString()));
    layout->addWidget(header);

    auto* text = new QPlainTextEdit;
    text->setPlainText(
        corrupt ? QStringLiteral(
                      "This document is stored but does not parse as JSON "
                      "(%1 at offset %2), so it cannot be edited here without "
                      "overwriting it.\n\nStored bytes:\n\n%3")
                      .arg(storedError.errorString())
                      .arg(storedError.offset)
                      .arg(SettingsSanitizer::redactedValue(feature, rawValue))
                : QString::fromUtf8(
                      QJsonDocument(displayDoc).toJson(QJsonDocument::Indented)));
    text->setReadOnly(true);
    text->setAccessibleName(
        QStringLiteral("%1 document contents").arg(feature));
    layout->addWidget(text, 1);

    auto* buttons = new QHBoxLayout;
    auto* editBtn = new QPushButton("Edit Raw JSON…");
    // Editing a redacted buffer would save "[REDACTED]" over the live
    // credential — documents holding one are read-only here, matching the
    // masked flat rows. (Delete stays available: a whole-document delete is
    // the sanctioned remediation for a legacy store that still carries a
    // credential-bearing document, and destroys rather than corrupts.)
    editBtn->setEnabled(m_storeWritable && !hasSecret && !corrupt);
    if (corrupt) {
        editBtn->setToolTip(
            "This document does not parse — editing here would overwrite "
            "recoverable bytes. Repair it with --config, or delete it to let "
            "the owning feature start fresh.");
    } else if (hasSecret) {
        editBtn->setToolTip(
            "This document contains a credential-shaped field — edit it "
            "through the owning feature's UI, which keeps the secret in the "
            "OS keychain");
    }
    auto* saveBtn = new QPushButton("Save");
    saveBtn->setEnabled(false);
    auto* closeBtn = new QPushButton("Close");
    buttons->addWidget(editBtn);
    buttons->addStretch();
    buttons->addWidget(saveBtn);
    buttons->addWidget(closeBtn);
    layout->addLayout(buttons);

    connect(editBtn, &QPushButton::clicked, &viewer, [text, saveBtn, editBtn] {
        text->setReadOnly(false);
        saveBtn->setEnabled(true);
        editBtn->setEnabled(false);
        text->setFocus();
    });
    connect(closeBtn, &QPushButton::clicked, &viewer, &QDialog::reject);
    connect(saveBtn, &QPushButton::clicked, &viewer,
            [this, &viewer, text, family, radioId, feature, schemaVersion,
             openTimeDoc = doc] {
        QJsonParseError parseError{};
        const QJsonDocument parsed = QJsonDocument::fromJson(
            text->toPlainText().toUtf8(), &parseError);
        if (parsed.isNull() || !parsed.isObject()) {
            FramelessMessageBox::warning(
                &viewer, "Invalid JSON",
                parseError.error != QJsonParseError::NoError
                    ? QStringLiteral("Not saved — %1 at offset %2.")
                          .arg(parseError.errorString())
                          .arg(parseError.offset)
                    : QStringLiteral(
                          "Not saved — the document must be a JSON object."));
            return;
        }
        // The store changes underneath an open viewer (features write
        // debounced). Compare CONTENT, not just the schema version: a
        // feature rewriting its document at the same version is the common
        // case by a wide margin, and a version-only check would pass it
        // through and commit the open-time buffer over the newer content —
        // a guard that reads right and checks the wrong quantity (PR #4631,
        // NF0T, same class as the cache-vs-disk finding).
        int nowVersion = 0;
        const QJsonObject nowDoc = AppSettings::instance().radioFeatureExact(
            family, radioId, feature, &nowVersion);
        if (nowVersion != schemaVersion || nowDoc != openTimeDoc) {
            FramelessMessageBox::warning(
                &viewer, "Document Changed",
                "This document was rewritten by its owning feature while the "
                "viewer was open. Nothing saved — close and re-open to edit "
                "the current version.");
            return;
        }
        if (!AppSettings::instance().setRadioFeature(
                family, radioId, feature, schemaVersion, parsed.object())) {
            FramelessMessageBox::warning(
                &viewer, "Write Refused",
                "The store refused this write (read-only session, or the "
                "stored document is from a newer version). Nothing changed.");
            return;
        }
        setStatus(QStringLiteral("Saved document %1").arg(feature), false);
        viewer.accept();
    });

    viewer.exec();
    m_docViewerOpen = false;
    populateTable();
}

void SettingsBrowserDialog::addKey()
{
    const Scope scope = currentScope();
    if (scope.kind != ScopeKind::App && scope.kind != ScopeKind::Station) {
        return;
    }
    const QString scopeName = scope.kind == ScopeKind::App
                                  ? QStringLiteral("App Settings")
                                  : QStringLiteral("Station: %1").arg(
                                        AppSettings::instance().stationName());

    QDialog dlg(this);
    dlg.setWindowTitle("Add Key");
    dlg.setStyleSheet(kDialogStyle);
    auto* form = new QFormLayout(&dlg);
    auto* keyEdit = new QLineEdit;
    keyEdit->setAccessibleName("New key name");
    auto* valueEdit = new QLineEdit;
    valueEdit->setAccessibleName("New key value");
    form->addRow(new QLabel(QStringLiteral("Scope: %1").arg(scopeName)));
    form->addRow("Key:", keyEdit);
    form->addRow("Value:", valueEdit);
    auto* box = new QDialogButtonBox(QDialogButtonBox::Ok
                                     | QDialogButtonBox::Cancel);
    connect(box, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    form->addRow(box);
    if (dlg.exec() != QDialog::Accepted) {
        return;
    }

    const QString key = keyEdit->text().trimmed();
    if (key.isEmpty()) {
        return;
    }
    auto& s = AppSettings::instance();
    if (SettingsCredentialPolicy::isFlatCredentialKey(key)
        || SettingsSanitizer::isSecretKey(key)) {
        FramelessMessageBox::warning(
            this, "Credential Key Refused",
            "Credentials are never stored in the settings database — they "
            "belong in the OS keychain, via the owning feature's own UI.");
        return;
    }
    const bool exists = scope.kind == ScopeKind::App
                            ? s.contains(key)
                            : s.stationKeysForDiagnostics().contains(key);
    if (!exists
        && FramelessMessageBox::question(
               this, "Create Raw Key?",
               QStringLiteral("Create %1 in %2?\n\nThis writes a raw key the "
                              "app may never read. Continue?")
                   .arg(key, scopeName))
               != FramelessMessageBox::Yes) {
        return;
    }
    if (scope.kind == ScopeKind::App) {
        s.setValue(key, valueEdit->text());
    } else {
        s.setStationValue(key, valueEdit->text());
    }
    s.save();
    // Same on-disk verification as the inline-edit path — "Added" must mean
    // the row reached the database, not just the cache (PR #4631 review).
    QString stored;
    const bool onDisk = scope.kind == ScopeKind::App
                            ? s.readAppRowFromDisk(key, stored)
                            : s.readStationRowFromDisk(key, stored);
    if (onDisk && stored == valueEdit->text()) {
        setStatus(QStringLiteral("Added %1").arg(key), false);
    } else {
        setStatus(QStringLiteral("%1 was NOT written to the store").arg(key),
                  true);
    }
    populateTable();
}

void SettingsBrowserDialog::deleteSelected()
{
    const Scope scope = currentScope();
    const int row = m_table->currentRow();
    if (row < 0) {
        return;
    }
    const QTableWidgetItem* keyItem = m_table->item(row, 0);
    if (keyItem == nullptr) {
        return;
    }
    const QString key = keyItem->data(kRoleKey).toString();
    auto& s = AppSettings::instance();

    QString prompt;
    switch (scope.kind) {
    case ScopeKind::App:
        prompt = QStringLiteral("Delete app setting %1?").arg(key);
        break;
    case ScopeKind::Station:
        prompt = QStringLiteral("Delete station setting %1?").arg(key);
        break;
    case ScopeKind::RadioScope:
        prompt = QStringLiteral(
                     "Delete the %1 document for %2 / %3?\n\nThe owning "
                     "feature will start from scratch — for a radio-state "
                     "document that means the radio's remembered operating "
                     "state is gone.")
                     .arg(key, scope.family, scopeLabelForRadioId(scope.radioId));
        break;
    case ScopeKind::None:
        return;
    }
    if (FramelessMessageBox::question(this, "Delete Setting?", prompt)
        != FramelessMessageBox::Yes) {
        return;
    }

    switch (scope.kind) {
    case ScopeKind::App:
        s.remove(key);
        s.save();
        break;
    case ScopeKind::Station:
        s.removeStationValue(key);
        s.save();
        break;
    case ScopeKind::RadioScope:
        if (!s.removeRadioFeature(scope.family, scope.radioId, key)) {
            setStatus(QStringLiteral("Delete of %1 was refused").arg(key),
                      true);
            return;
        }
        break;
    case ScopeKind::None:
        return;
    }
    setStatus(QStringLiteral("Deleted %1").arg(key), false);
    rebuildTree();
}

void SettingsBrowserDialog::exportSanitized()
{
    const QString path = QFileDialog::getSaveFileName(
        this, "Export Sanitized Settings",
        QDir::home().filePath(QStringLiteral("AetherSDR-settings.txt")),
        "Text files (*.txt);;All files (*)");
    if (path.isEmpty()) {
        return;
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        setStatus(QStringLiteral("Could not write %1: %2")
                      .arg(path, file.errorString()),
                  true);
        return;
    }
    const QByteArray payload = SettingsSanitizer::dump().toUtf8();
    const bool wrote = file.write(payload) == payload.size() && file.flush();
    file.close();
    if (!wrote || file.error() != QFileDevice::NoError) {
        setStatus(QStringLiteral("Export to %1 failed: %2 — file may be "
                                 "truncated")
                      .arg(path, file.errorString()),
                  true);
        return;
    }
    setStatus(QStringLiteral("Exported sanitized dump to %1").arg(path), false);
}

void SettingsBrowserDialog::setStatus(const QString& text, bool error)
{
    m_status->setStyleSheet(error ? kResultErrorStyle : kResultInfoStyle);
    m_status->setText(text);
    m_status->setVisible(!text.isEmpty());
}

void SettingsBrowserDialog::updateButtonState()
{
    const Scope scope = currentScope();
    const bool flatScope = scope.kind == ScopeKind::App
                           || scope.kind == ScopeKind::Station;
    m_addBtn->setEnabled(m_storeWritable && flatScope);

    bool deletable = false;
    const int row = m_table->currentRow();
    if (m_storeWritable && row >= 0 && scope.kind != ScopeKind::None) {
        const QTableWidgetItem* valueItem = m_table->item(row, 1);
        // A masked (credential-shaped) row is fully read-only here — deleting
        // blind is as unsafe as editing blind.
        deletable = scope.kind == ScopeKind::RadioScope
                    || valueItem == nullptr
                    || !valueItem->data(kRoleSecret).toBool();
    }
    m_deleteBtn->setEnabled(deletable);
}

} // namespace AetherSDR
