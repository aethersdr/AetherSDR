#pragma once

#include <QWidget>
#include <QList>
#include <QString>

class QComboBox;
class QPushButton;

namespace AetherSDR {

class AppletPicker : public QWidget {
    Q_OBJECT

public:
    struct Entry {
        QString id;
        QString title;
        QString category;
        bool available{true};
        bool open{false};
    };

    explicit AppletPicker(QWidget* parent = nullptr);
    void setEntries(const QList<Entry>& entries);

signals:
    void addRequested(const QString& id);

private:
    void updateAddButton();
    QComboBox* m_combo{nullptr};
    QPushButton* m_addButton{nullptr};
};

}
