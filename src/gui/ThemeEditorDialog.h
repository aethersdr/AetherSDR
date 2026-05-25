#pragma once

#include <QDialog>

class QListWidget;
class QListWidgetItem;
class QLabel;
class QLineEdit;
class QPushButton;

namespace AetherSDR {

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
class ThemeEditorDialog : public QDialog {
    Q_OBJECT
public:
    explicit ThemeEditorDialog(QWidget* parent = nullptr);

private slots:
    void refreshTokenList();         // rebuild rows from ThemeManager
    void onTokenRowClicked(QListWidgetItem* item);
    void onSaveAsClicked();
    void onActiveThemeChanged();     // re-load when user picks a different theme

private:
    void updateTitle();
    void updateRow(QListWidgetItem* item);   // re-paint swatch + hex label

    QLabel*      m_themeLabel{nullptr};   // "Editing: <name>"
    QLineEdit*   m_filterEdit{nullptr};   // type-to-filter token names
    QListWidget* m_tokenList{nullptr};
    QPushButton* m_saveAsBtn{nullptr};

    // Tracks the active-theme NAME we last rendered against, so the
    // themeChanged handler can distinguish "user switched theme" (full
    // rebuild) from "user just edited one token in the active theme"
    // (no rebuild — would otherwise destroy the row the caller is
    // still holding a pointer to and segfault during the post-edit
    // updateRow()).
    QString m_lastRenderedTheme;
};

} // namespace AetherSDR
