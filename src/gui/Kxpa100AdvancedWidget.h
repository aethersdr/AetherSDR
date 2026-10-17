#pragma once
#include "models/Kxpa100Types.h"
#include <QPointer>
#include <QWidget>
#include <optional>
class QLabel;
class QPushButton;
class QCheckBox;
class QComboBox;
class QTableWidget;
class QProgressBar;
class QFileDialog;
class QSpinBox;
namespace AetherSDR
{
class Kxpa100Model;
class Kxpa100AdvancedWidget : public QWidget
{
    Q_OBJECT
  public:
    explicit Kxpa100AdvancedWidget(Kxpa100Model* model, QWidget* parent = nullptr);
    ~Kxpa100AdvancedWidget() override;

  private:
    friend class Kxpa100AdvancedWidgetTest;
    enum class FileAction { Backup, Choose, Faults };
    void refreshAvailability();
    void renderConfiguration();
    void configurationRead(const Kxpa100::Configuration& values);
    void stageField(int row, int column);
    void clearDeviceData();
    void showStatus(const QString& message);
    void openFileDialog(FileAction action);
    bool loadBackup(const QString& path);
    void readFaults(const QList<Kxpa100::FaultRecord>& records);
    void readMemories(const Kxpa100::MemoryBin& bin);
    QString schemaReason(bool writing) const;
    QString restoreReason() const;
    QPointer<Kxpa100Model> m_model;
    QPointer<QFileDialog> m_fileDialog;
    std::optional<Kxpa100::Configuration> m_confirmed, m_staged;
    std::optional<Kxpa100::Backup> m_backup;
    QString m_savePath;
    QList<Kxpa100::FaultRecord> m_faultRecords;
    bool m_ownOperation = false, m_applying = false, m_faultsLoaded = false;
    QLabel* m_identity;
    QLabel* m_availability;
    QLabel* m_status;
    QLabel* m_preview;
    QLabel* m_memorySummary;
    QComboBox* m_eraseBand;
    QComboBox* m_eraseAntenna;
    QTableWidget* m_configuration;
    QTableWidget* m_faults;
    QTableWidget* m_memories;
    QCheckBox* m_restoreConfirm;
    QCheckBox* m_eraseConfirm;
    QCheckBox* m_resetConfirm;
    QSpinBox* m_memoryFrequency;
    QProgressBar* m_progress;
};
} // namespace AetherSDR
