#pragma once

#include <QDialog>
#include <QVector>

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QStackedWidget;
class QTimer;
class QVBoxLayout;

namespace AetherSDR {

class RadioModel;
class BandPlanManager;

// ATU pre-tune sweep dialog (#2624).
//
// Steps the active TX slice through a calculated set of center frequencies
// across the selected HF/6m bands, triggering "atu start" at each point
// and waiting for atuStateChanged before moving to the next. Band edges
// come from the active BandPlanManager so the sweep stays within the
// region's plan — band edges from the plan, not from kBands[].
//
// Two modes: Step (per-point confirmation) and Auto (unattended).
// Safety: MEM must be enabled (gating is enforced before opening), an
// always-visible Abort, a 30 s per-point timeout, and a hard stop after
// 3 consecutive TUNE_FAIL_BYPASS results.  After the sweep the slice is
// restored to its pre-sweep frequency.
class AtuPreTuneDialog : public QDialog {
    Q_OBJECT

public:
    AtuPreTuneDialog(RadioModel* radio,
                     BandPlanManager* bandPlan,
                     QWidget* parent = nullptr);

    void setFramelessMode(bool on);

protected:
    void closeEvent(QCloseEvent* ev) override;

private:
    // One band row in the band picklist.
    struct BandRow {
        QString name;               // "160m", "80m", …
        double  lowMhz{0.0};        // derived from BandPlanManager segments
        double  highMhz{0.0};
        int     segmentKhz{0};      // tuning segment size in kHz
        int     points{0};          // count of pre-tune frequencies
        // Precomputed sweep centers — walked per contiguous-segment region so
        // discrete-channel bands (e.g. US 60m) tune at legal channel midpoints
        // instead of evenly across the [low, high] envelope. (#2647)
        QVector<double> centers;
        QCheckBox* check{nullptr};
        QLabel*    info{nullptr};
    };

    enum class Mode { Step, Auto };

    void buildConfigPage();
    void buildSweepPage();
    void populateBands();
    void populateLicenseClassRow();
    QString selectedLicenseClass() const;
    QVector<double> centersForBand(const BandRow& row) const;
    void onStartClicked();
    void onTuneClicked();
    void onSkipClicked();
    void onAbortClicked();
    void onContinueClicked();
    void onAtuStateChanged();
    void onPerPointTimeout();
    void beginNextPoint();
    void requestTuneNow();
    void finishSweep(const QString& summaryExtra = {});
    void restoreOriginalFrequency();
    void setStepControlsEnabled(bool enabled);
    void showFailControls(bool failBypass);
    void setAbortButtonAbortMode();
    void setAbortButtonCloseMode();

    RadioModel*       m_radio{nullptr};
    BandPlanManager*  m_bandPlan{nullptr};

    QWidget*     m_titleBar{nullptr};
    QVBoxLayout* m_bodyLayout{nullptr};
    QStackedWidget* m_pages{nullptr};

    QWidget* m_configPage{nullptr};
    QLabel*  m_planNameLabel{nullptr};
    QComboBox* m_modeCombo{nullptr};
    QWidget*   m_licenseClassRow{nullptr};
    QComboBox* m_licenseClassCombo{nullptr};
    QWidget* m_bandsContainer{nullptr};
    QVBoxLayout* m_bandsLayout{nullptr};
    QVector<BandRow> m_bands;
    QPushButton* m_startBtn{nullptr};
    QPushButton* m_cancelBtn{nullptr};

    QWidget* m_sweepPage{nullptr};
    QLabel*  m_sweepStatus{nullptr};
    QLabel*  m_sweepProgress{nullptr};
    QLabel*  m_sweepResult{nullptr};
    QPushButton* m_tuneBtn{nullptr};
    QPushButton* m_skipBtn{nullptr};
    QPushButton* m_abortBtn{nullptr};
    QPushButton* m_continueAfterFailBtn{nullptr};

    struct Point {
        QString bandName;
        double  freqMhz{0.0};
        int     indexInBand{0};
        int     totalInBand{0};
        double  bandLowMhz{0.0};
        double  bandHighMhz{0.0};
    };
    QVector<Point> m_points;
    int m_currentIndex{-1};
    Mode m_mode{Mode::Step};

    int m_txSliceId{-1};
    double m_originalSliceFreqMhz{0.0};
    QString m_originalPanId;
    double  m_originalPanCenterMhz{0.0};
    double  m_originalPanBandwidthMhz{0.0};

    QTimer* m_settleTimer{nullptr};
    QTimer* m_timeoutTimer{nullptr};
    bool m_waitingForAtu{false};
    bool m_sweepActive{false};

    int m_successCount{0};
    int m_skipCount{0};
    int m_failCount{0};
    int m_consecutiveFailBypass{0};

    // Most recent non-reset SWR reading observed while waitingForAtu is
    // true.  The radio's final meter packet of each tune cycle resets SWR
    // to exactly 1.0 at full forward power — we track readings > 1.001 so
    // the report reflects the settled post-tune SWR. (#2624)
    float m_tuneLastSwr{0.0f};
    bool  m_swrTracking{false};
};

} // namespace AetherSDR
