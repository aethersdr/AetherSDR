#pragma once

#include "core/Kpa500Maintenance.h"
#include <QPointer>
#include <QWidget>
#include <optional>

class QCheckBox;
class QFileDialog;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QTableWidget;

namespace AetherSDR {
class Kpa500Connection;

// On-demand peripheral maintenance. File selection never writes to the amp.
class Kpa500AdvancedWidget : public QWidget {
    Q_OBJECT
public:
    explicit Kpa500AdvancedWidget(Kpa500Connection* connection, QWidget* parent = nullptr);
    ~Kpa500AdvancedWidget() override;

private:
    friend class Kpa500AdvancedWidgetTest;
    enum class Operation { None, Backup, Restore, BannerRead, BannerWrite, FaultRead };
    enum class FileAction { SaveBackup, ChooseBackup, ExportFaults };
    void refreshAvailability();
    void clearDeviceData();
    void showStatus(const QString& text, bool error = false);
    void beginOperation(Operation operation);
    void openFileDialog(FileAction action);
    void chooseBackup();
    bool loadBackup(const QString& path);
    void saveBackup();
    void saveBackupToFile(const QString& path);
    void exportFaults();
    void exportFaultsToFile(const QString& path);
    void configurationRead(const QByteArray& data);
    void faultHistoryRead(const QByteArray& data);
    void operationFinished(const QString& message);
    QString restoreUnavailableReason() const;

    QPointer<Kpa500Connection> m_connection;
    QPointer<QFileDialog> m_fileDialog;
    Operation m_operation{Operation::None};
    std::optional<Kpa500::ConfigurationBackup> m_backup;
    QString m_backupPath;
    QString m_savePath;
    QString m_operationSerial;
    QString m_operationFirmware;
    QString m_confirmedBanner;
    QString m_resultMessage;
    bool m_resultError{false};
    bool m_bannerLoaded{false};
    bool m_faultsLoaded{false};
    QLabel* m_identity;
    QLabel* m_availability;
    QLabel* m_status;
    QLabel* m_backupPreview;
    QLabel* m_bannerReadback;
    QLabel* m_faultSummary;
    QPushButton* m_saveBackup;
    QPushButton* m_chooseBackup;
    QPushButton* m_restore;
    QPushButton* m_readBanner;
    QPushButton* m_writeBanner;
    QPushButton* m_readFaults;
    QPushButton* m_exportFaults;
    QPushButton* m_cancel;
    QCheckBox* m_confirmRestore;
    QLineEdit* m_banner;
    QProgressBar* m_progress;
    QTableWidget* m_faults;
};
} // namespace AetherSDR
