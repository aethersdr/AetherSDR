#pragma once

#include "PersistentDialog.h"

#include <QPointer>

class QDragEnterEvent;
class QDropEvent;
class QTreeWidget;
class QTreeWidgetItem;
class QLabel;
class QComboBox;
class QLineEdit;
class QPushButton;

namespace AetherSDR {

class ThemeInspector;
class TokenEditorWidget;

// Modeless dialog for live-editing the active theme's color tokens.
//
// Phase 5 PR 1 — minimum viable surface:
//   * List every color token discovered via ThemeManager::allTokenKeys()
//   * Click a row → QColorDialog opens with the current value seeded
//   * Accepting the dialog calls ThemeManager::setColor(), which emits
//     themeChanged() and live-repaints every widget registered through
//     applyStyleSheet().
//   * "Save As…" prompts for a name and writes m_tokens to
//     ~/.config/AetherSDR/themes/<name>.json via saveCurrentThemeAs().
//     The new theme is registered + made active immediately.
//
// Deferred to follow-on PRs:
//   * Inspector mode (click-on-widget to find tokens that paint it)
//   * Gradient editing (waterfall colormap stops, slice.dim block)
//   * Font / sizing token editing
//   * Import (drag-and-drop / file picker for arbitrary theme JSON)
class ThemeEditorDialog : public PersistentDialog {
    Q_OBJECT
public:
    explicit ThemeEditorDialog(QWidget* parent = nullptr);

private slots:
    void refreshTokenList();         // rebuild rows from ThemeManager
    void onTokenRowSelectionChanged();
    void onTokenEditedByEditor(const QString& key);
    void onSaveAsClicked();
    void onSaveAsBeforeCommit();     // fork built-in theme before committing an edit
    void onActiveThemeChanged();     // re-load when user picks a different theme
    void onContainerChanged(int);    // user picked a different scope from the container combo
    // Click a scope-column cell in the columnar token table → focus
    // that scope in the picker AND select the row's token for editing.
    void onTokenCellClicked(QTreeWidgetItem* item, int column);
    // Right-click a scope-column cell → context menu with
    // "Clear override at <scope>" (enabled only when an override
    // actually exists at that level for the row's token).
    void onTokenContextMenu(const QPoint& pos);

    // Inspector-mode handlers.
    void onInspectToggled(bool on);
    void onInspectorPicked(QWidget* target, QPoint localPos);
    void onInspectorActiveChanged(bool active);

    // Theme-file management — Rename / Delete / Export / Import on the
    // "Theme actions" menu next to Save As.
    void onRenameThemeClicked();
    void onDeleteThemeClicked();
    void onExportThemeClicked();
    void onImportThemeClicked();
    // Shared body for both the menu Import and the drag-and-drop path.
    void importThemeFromPath(const QString& filePath);

protected:
    // Drag-and-drop: drop a `.aethertheme` (or `.json` theme) on the
    // dialog to install + activate it.
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    void updateTitle();
    void updateRow(QTreeWidgetItem* item);   // re-paint swatch + hex label
    void populateRow(QTreeWidgetItem* item); // shared by refresh + update
    void rebuildColumns();                   // adjust column count + headers to active scope
    void updateInspectorStatus(const QString& text);
    // Filter the token list down to a specific subset, e.g. tokens
    // returned by tokensForWidget().  An empty list clears the filter.
    void filterTokensTo(const QStringList& subset);

    QLabel*      m_themeLabel{nullptr};   // "Editing: <name>"
    TokenEditorWidget* m_tokenEditor{nullptr};   // inline editor stack
    QComboBox*   m_containerCombo{nullptr};   // scope picker — root + declared paths
    QLineEdit*   m_filterEdit{nullptr};   // type-to-filter token names
    QTreeWidget* m_tokenList{nullptr};   // multi-column: Object | <scope chain…> | Value
    void         refreshContainerCombo();
    QString      m_activeContainerPath;  // empty == root scope
    QPushButton* m_saveAsBtn{nullptr};
    QPushButton* m_inspectBtn{nullptr};   // checkable
    QLabel*      m_inspectStatus{nullptr};
    ThemeInspector* m_inspector{nullptr};
    QStringList     m_activeSubset;       // last inspector-picked token list

    // Tracks the active-theme NAME we last rendered against, so the
    // themeChanged handler can distinguish "user switched theme" (full
    // rebuild) from "user just edited one token in the active theme"
    // (no rebuild — would otherwise destroy the row the caller is
    // still holding a pointer to and segfault during the post-edit
    // updateRow()).
    QString m_lastRenderedTheme;
};

} // namespace AetherSDR
