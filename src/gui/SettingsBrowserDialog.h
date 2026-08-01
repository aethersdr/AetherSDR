#pragma once

#include "PersistentDialog.h"

#include <QString>

class QLabel;
class QLineEdit;
class QPushButton;
class QShowEvent;
class QTableWidget;
class QTableWidgetItem;
class QTreeWidget;
class QTreeWidgetItem;

namespace AetherSDR {

// Settings Browser (RFC #4603 proposal D, PR 5) — a scope-grouped view over
// the whole settings store: app keys, the station section, and every
// radio-scoped feature document, mirrored as a tree of the scopes the store
// actually contains. All edits go through the AppSettings API — never raw
// SQL — so the running app's cache and the database stay coherent, and the
// credential seam / newer-schema refusals apply to browser edits exactly as
// they do to feature code.
//
// Guardrails (RFC-ratified):
//   * meta is not shown (AppSettings does not expose it).
//   * Credential-shaped rows render redacted and are read-only here.
//   * Feature documents open pretty-printed read-only; editing raw JSON is
//     an explicit second step and the document must parse as a JSON object.
//   * A newer-schema (read-only) store disables every edit affordance.
//   * Export reuses the sanitizer dump — diagnostic text, not a backup.
class SettingsBrowserDialog : public PersistentDialog {
    Q_OBJECT

public:
    explicit SettingsBrowserDialog(QWidget* parent = nullptr);

protected:
    void showEvent(QShowEvent* event) override;

private:
    // What a tree row addresses. App/Station are single scopes; RadioScope
    // carries (family, radioId) with radioId "" = the family-wide default row.
    enum class ScopeKind { None, App, Station, RadioScope };

    struct Scope {
        ScopeKind kind{ScopeKind::None};
        QString family;
        QString radioId;
    };

    void rebuildTree();
    void populateTable();
    void applyFilter();
    Scope currentScope() const;
    void selectScopeItem(const Scope& scope);

    void onTableItemChanged(QTableWidgetItem* item);
    void onTableActivated(int row);
    void addKey();
    void deleteSelected();
    void exportSanitized();
    void openDocumentViewer(const QString& family, const QString& radioId,
                            const QString& feature, const QString& rawValue);

    void setStatus(const QString& text, bool error);
    void updateButtonState();

    QTreeWidget*  m_tree{nullptr};
    QTableWidget* m_table{nullptr};
    QLineEdit*    m_search{nullptr};
    QLabel*       m_banner{nullptr};
    QLabel*       m_status{nullptr};
    QPushButton*  m_addBtn{nullptr};
    QPushButton*  m_deleteBtn{nullptr};
    QPushButton*  m_refreshBtn{nullptr};
    QPushButton*  m_exportBtn{nullptr};

    bool m_populating{false};
    bool m_storeWritable{false};
    bool m_docViewerOpen{false};   // double-click emits activated too — one
                                   // viewer per gesture, not two
};

} // namespace AetherSDR
