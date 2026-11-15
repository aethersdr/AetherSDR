#pragma once
#include "models/Kat500Types.h"
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
class Kat500Model;
class Kat500AdvancedWidget : public QWidget
{
    Q_OBJECT
  public:
    explicit Kat500AdvancedWidget(Kat500Model* model, QWidget* parent = nullptr);
    ~Kat500AdvancedWidget() override;

  private:
    friend class Kat500AdvancedWidgetTest;
    enum class FileAction { Backup, Choose, Faults };
    void refreshAvailability();
    void renderConfiguration();
    void configurationRead(const Kat500::Configuration& values);
    void stageField(int row);
    void clearDeviceData();
    void showStatus(const QString& message);
    void openFileDialog(FileAction action);
    bool loadBackup(const QString& path);
    void readFaults(const QList<Kat500::FaultRecord>& records);
    void readMemories(const Kat500::MemoryBin& bin);
    QString schemaReason(bool writing) const;
    QString restoreReason() const;
    QPointer<Kat500Model> m_model;
    QPointer<QFileDialog> m_fileDialog;
    Kat500::Configuration m_confirmed, m_staged;
    std::optional<Kat500::Backup> m_backup;
    QString m_savePath;
    QList<Kat500::FaultRecord> m_faultRecords;
    bool m_ownOperation = false, m_applying = false, m_faultsLoaded = false;
    QLabel* m_identity;
    QLabel* m_availability;
    QLabel* m_status;
    QLabel* m_preview;
    QLabel* m_memorySummary;
    QComboBox* m_band;
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
