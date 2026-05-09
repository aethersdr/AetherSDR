#pragma once

#include <QDialog>
#include <QCloseEvent>
#include <QEvent>
#include <QKeyEvent>
#include <QShowEvent>
#include <QSet>
#include <QTableWidget>
#include <QMap>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QVBoxLayout;

namespace AetherSDR {

class RadioModel;

class MemoryDialog : public QDialog {
    Q_OBJECT

public:
    explicit MemoryDialog(RadioModel* model, QWidget* parent = nullptr);

    // Toggle frameless chrome at runtime — mirrors the SpotHub /
    // RadioSetup pattern.  Snapshots geometry + visibility, flips
    // Qt::FramelessWindowHint, restores.  Called by MainWindow when
    // the global View → Frameless Window setting changes.
    void setFramelessMode(bool on);

Q_SIGNALS:
    void memoryActivated(int memoryIndex);

protected:
    void closeEvent(QCloseEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    void activateMemoryRow(int row);
    void beginEditingMemoryName(int memoryIndex);
    void focusTableOnCurrentRow();
    void populateTable();
    void setInlineEditMode(bool enabled);
    void submitCellEdit(int row, int col);
    void onAdd();
    void onImport();
    void onExport();
    void onEdit();
    void onSelect();
    void onSelectAll();
    void onRemove();
    bool isSortableColumn(int column) const;
    void rebuildFilterCombo();
    QSet<int> selectedMemoryIndices() const;
    void updateSelectionActions();

    RadioModel* m_model;
    QTableWidget* m_table;
    QLineEdit* m_searchEdit;
    QComboBox* m_filterCombo;
    QLabel* m_selectionLabel{nullptr};
    QPushButton* m_editBtn{nullptr};
    QPushButton* m_selectBtn{nullptr};
    QPushButton* m_selectAllBtn{nullptr};
    QPushButton* m_removeBtn{nullptr};
    int m_pendingEditMemoryIndex{-1};
    int m_pendingEditRetries{0};
    bool m_inlineEditMode{false};
    int m_sortColumn{2};
    Qt::SortOrder m_sortOrder{Qt::AscendingOrder};

    // Frameless chrome (mirrors SpotHub / RadioSetup): custom title
    // bar at the top, FramelessResizer on the body for 8-axis edge
    // resize.
    QWidget*     m_titleBar{nullptr};
    QVBoxLayout* m_outerLayout{nullptr};
};

} // namespace AetherSDR
