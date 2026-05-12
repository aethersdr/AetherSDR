#pragma once

#include <QDialog>

class QVBoxLayout;
class QTableWidget;
class QPushButton;
class QLabel;

namespace AetherSDR {

class RadioModel;

class MultiFlexDialog : public QDialog {
    Q_OBJECT
public:
    explicit MultiFlexDialog(RadioModel* model, QWidget* parent = nullptr);
    void setFramelessMode(bool on);

private:
    void refresh();

    RadioModel* m_model;
    QTableWidget* m_table;
    QPushButton* m_enableBtn;
    QLabel* m_pttLabel;
    QPushButton* m_pttBtn;
    QWidget* m_titleBar{nullptr};
    QVBoxLayout* m_bodyLayout{nullptr};
    bool m_refreshing{false};
};

} // namespace AetherSDR
