#pragma once

#include "PersistentDialog.h"
#include "core/MqttSettings.h"

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTableWidget;

namespace AetherSDR {

class MqttSettingsDialog : public PersistentDialog {
    Q_OBJECT

public:
    explicit MqttSettingsDialog(QWidget* parent = nullptr);

signals:
    void settingsSaved(const QString& password);

private:
    void buildUi();
    void loadSettings();
    void saveSettings();
    void loadPasswordFromKeychain();
    void savePasswordToKeychain(const QString& password);

    void addTopicRow(const MqttTopicDef& def = {});
    void removeSelectedTopicRows();
    QVector<MqttTopicDef> topicRows() const;

    void addButtonRow(const MqttButtonDef& def = {});
    void removeSelectedButtonRows();
    QVector<MqttButtonDef> buttonRows() const;
    void updateButtonControls();

    QLineEdit* m_hostEdit{nullptr};
    QSpinBox*  m_portSpin{nullptr};
    QLineEdit* m_userEdit{nullptr};
    QLineEdit* m_passEdit{nullptr};
    QCheckBox* m_tlsCheck{nullptr};
    QLineEdit* m_caFileEdit{nullptr};
    QTableWidget* m_topicsTable{nullptr};
    QTableWidget* m_buttonsTable{nullptr};
    QPushButton* m_addButtonRowBtn{nullptr};
};

} // namespace AetherSDR
