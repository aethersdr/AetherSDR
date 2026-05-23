#pragma once

#include <QDialog>

class QComboBox;
class QLabel;
class QTableWidget;
class QLineEdit;

namespace MasterSDR {

class ShortcutManager;
class KeyboardMapWidget;

class ShortcutDialog : public QDialog {
    Q_OBJECT
public:
    explicit ShortcutDialog(ShortcutManager* mgr, QWidget* parent = nullptr);

protected:
    void keyPressEvent(QKeyEvent* ev) override;

private:
    void buildUI();
    void populateTable(const QString& filter = {}, const QString& category = {});
    void onKeySelected(Qt::Key key);
    void onTableRowSelected(int row);
    void assignAction(const QString& actionId);
    void clearSelected();
    void resetSelected();
    void updateSelectedKeyInfo();

    ShortcutManager* m_mgr;
    KeyboardMapWidget* m_keyboardMap;

    // Selected key panel
    QLabel* m_selectedKeyLabel;
    QComboBox* m_actionCombo;
    QLabel* m_categoryLabel;

    // Action table
    QLineEdit* m_filterEdit;
    QComboBox* m_categoryFilter;
    QTableWidget* m_table;

    // State
    bool m_capturingKey{false};   // true when waiting for a keypress to assign
};

} // namespace MasterSDR
