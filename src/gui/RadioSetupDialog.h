#pragma once

#include "PersistentDialog.h"

#include <QHash>
#include <QVector>
#include <functional>

class QTabWidget;
class QLabel;
class QLineEdit;
class QGroupBox;
class QProgressBar;
class QPushButton;
class QComboBox;
class QCheckBox;
class QVBoxLayout;
class QTableWidget;

namespace AetherSDR {

class RadioModel;
class AudioEngine;
class FirmwareUploader;
class FirmwareStager;
class TgxlConnection;
class PgxlConnection;
class AntennaGeniusModel;

// Radio Setup dialog — tabbed configuration window matching SmartSDR's
// Settings → Radio Setup. Shows radio info, GPS, TX, RX, filters, etc.
class RadioSetupDialog : public PersistentDialog {
    Q_OBJECT

public:
    explicit RadioSetupDialog(RadioModel* model, AudioEngine* audio = nullptr,
                              TgxlConnection* tgxl = nullptr,
                              PgxlConnection* pgxl = nullptr,
                              AntennaGeniusModel* ag = nullptr,
                              QWidget* parent = nullptr);
    void selectTab(const QString& tabName);
    void refreshFlexControlButtonActions();
    void setFlexControlConnectionStatus(bool connected, const QString& port = {});

signals:
    void txBandSettingsRequested();
    void serialSettingsChanged();
    // Fired when the user toggles SliceLetterDisplay mode in the Themes
    // tab so MainWindow can push a refresh through all slice-letter
    // widgets (the AppSettings value is what's actually consulted at
    // paint time — this signal is just the redraw trigger).
    void sliceLetterDisplayModeChanged();

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    QWidget* buildRadioTab();
    QWidget* buildNetworkTab();
    QGroupBox* buildIpConfigGroup();
    QWidget* buildGpsTab();
    QWidget* buildTxTab();
    QWidget* buildPhoneCwTab();
    QWidget* buildRxTab();
    QWidget* buildAudioTab();
    QWidget* buildFiltersTab();
    QWidget* buildXvtrTab();
    QWidget* buildAntennaNamesTab();
    QWidget* buildApdTab();
    void     refreshApdSamplerCombo(const QString& txAnt);
    QWidget* buildUsbCablesTab();
    QWidget* buildPeripheralsTab();
    QWidget* buildUiEnhancementsTab();
    // Phase 2 of GHSA-wfx7-w6p8-4jr2 (#2951) — Pinned Certificates list
    // (host, sha256 fingerprint, pinned date) with per-row Forget and a
    // Forget All button. Backed by WanCertCache in WanConnection.cpp.
    QWidget* buildSmartLinkTab();

public:
    // Public so MainWindow can refresh the table from outside this
    // dialog when an accept-after-mismatch flow rewrites the pin
    // cache. No-op if the SmartLink tab hasn't been built yet
    // (m_pinnedCertsTable == nullptr).
    void     refreshPinnedCertsTable();

private:
#ifdef HAVE_SERIALPORT
    QWidget* buildSerialTab();
#endif

    // SmartLink Pinned Certs UI handle (#2951). Forward-declared at
    // file scope above; full type comes from <QTableWidget> in the cpp.
    QTableWidget* m_pinnedCertsTable{nullptr};

    RadioModel*  m_model;
    AudioEngine* m_audio{nullptr};
    TgxlConnection*    m_tgxl{nullptr};
    PgxlConnection*    m_pgxl{nullptr};
    AntennaGeniusModel* m_ag{nullptr};
    QTabWidget*  m_tabs{nullptr};
    QHash<QString, QComboBox*> m_flexControlActionCombos;
    QHash<QString, QString> m_flexControlActionDefaults;
    QLabel* m_flexControlStatusLabel{nullptr};
    QPushButton* m_flexControlDetectButton{nullptr};
    QPushButton* m_flexControlCloseButton{nullptr};
    QCheckBox* m_flexControlInvertCheck{nullptr};

    // Radio tab fields
    QLabel* m_serialLabel{nullptr};
    QLabel* m_hwVersionLabel{nullptr};
    QLabel* m_regionLabel{nullptr};
    QLabel* m_optionsLabel{nullptr};
    QLabel* m_remoteOnLabel{nullptr};
    QLabel* m_modelLabel{nullptr};
    QLineEdit* m_nicknameEdit{nullptr};
    QLineEdit* m_callsignEdit{nullptr};
    QPushButton* m_remoteOnBtn{nullptr};

    // License Info
    QLabel* m_licSubscriptionLabel{nullptr};
    QLabel* m_licExpirationLabel{nullptr};
    QLabel* m_licRadioIdLabel{nullptr};
    QLabel* m_licMaxVersionLabel{nullptr};

    // Firmware update
    QLabel*       m_fwStatusLabel{nullptr};
    QProgressBar* m_fwProgress{nullptr};
    QPushButton*  m_fwUploadBtn{nullptr};
    QString       m_fwFilePath;
    FirmwareUploader* m_uploader{nullptr};
    FirmwareStager*   m_stager{nullptr};

    // Lazy tab construction — deferred builders keyed by tab index (#1776)
    QHash<int, std::function<QWidget*()>> m_deferredBuilders;
    void buildDeferredTab(int index);

    // External APD tab (visible only when the radio reports apd configurable=1)
    int                       m_apdTabIndex{-1};
    QHash<QString, QComboBox*> m_apdSamplerCombos;

    // Peripherals tab — savers run on dialog close to persist field edits
    // that the user did not commit via the row's Connect/Disconnect button.
    // Currently only used to honour "user cleared IP and closed dialog"
    // → wipe the saved manual IP/port. New-IP edits still require an
    // explicit Connect click so an unfinished value cannot leak in.
    QVector<std::function<void()>> m_peripheralRowSavers;
};

} // namespace AetherSDR
