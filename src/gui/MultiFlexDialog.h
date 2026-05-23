#pragma once

#include "PersistentDialog.h"

class QTableWidget;
class QPushButton;
class QLabel;

namespace MasterSDR {

class RadioModel;

class MultiFlexDialog : public PersistentDialog {
    Q_OBJECT
public:
    explicit MultiFlexDialog(RadioModel* model, QWidget* parent = nullptr);

private:
    void refresh();

    RadioModel* m_model;
    QTableWidget* m_table;
    QPushButton* m_enableBtn;
    QLabel* m_pttLabel;
    QPushButton* m_pttBtn;
    bool m_refreshing{false};
};

} // namespace MasterSDR
