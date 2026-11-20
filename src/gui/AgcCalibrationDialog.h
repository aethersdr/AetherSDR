#pragma once

#include "gui/CanonWindow.h"
#include "core/AgcTCalibrator.h"

#include <QMetaObject>
#include <QPointer>
#include <QVector>
#include <QWidget>

#include <functional>
#include <limits>

class QComboBox;
class QLabel;
class QPushButton;
class QDoubleSpinBox;
class QTimer;

namespace AetherSDR {

class RadioModel;
class SliceModel;
class AudioEngine;
class TciServer;

// AgcCurveWidget — lightweight 2D plot of post-AGC audio noise (y, dB) versus
// the AGC-T value (x, 0..100). Markers: current value, detected knee /
// recommended value, and (AGC-off) the comfortable-noise target line.
// Painted in the canon palette (color.canon.*), so it follows the theme.
class AgcCurveWidget : public QWidget {
    Q_OBJECT
public:
    explicit AgcCurveWidget(QWidget* parent = nullptr);

    void setCurve(const QVector<AgcTCalibrator::Point>& pts);
    void setCurrentValue(int value);
    void setRecommended(int value, bool isKnee);
    void setStrategy(AgcTCalibrator::Strategy s);
    void setTargetDb(float db);
    void clear();

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QVector<AgcTCalibrator::Point> m_pts;
    int   m_current{-1};
    int   m_recommended{-1};
    bool  m_recIsKnee{true};
    float m_targetDb{-28.0f};
    AgcTCalibrator::Strategy m_strategy{AgcTCalibrator::Strategy::Knee};
};

// AgcCalibrationDialog — the calibration surface, a canon dialog. Auto-sweep
// first (one click), with the live curve always visible so the operator can
// fine-tune by hand. Operates on the active slice; the radio stays
// authoritative (we only set the value through SliceModel).
class AgcCalibrationDialog : public CanonWindow {
    Q_OBJECT
public:
    using NoiseFloorFn = std::function<float(SliceModel*)>;

    AgcCalibrationDialog(RadioModel* radio,
                         AudioEngine* audio,
                         TciServer* tci,
                         NoiseFloorFn noiseFloorFn,
                         QWidget* parent = nullptr);
    ~AgcCalibrationDialog() override;

    // Every close path (the close button, Escape, Ctrl+W) ends here: a running
    // sweep stops and the AGC-T it found goes back, as Stop does.
    void done(int result) override;

    // Point the panel at a slice (called on show and on active-slice change).
    void setSlice(SliceModel* slice);

private slots:
    void onStartStop();
    void onApply();
    void onStarted(AgcTCalibrator::Strategy strategy, int originalValue);
    void onProgress(int currentValue, int percent);
    void onPointAdded(int value, float rmsDb);
    void onRecommendation(int value, bool isKnee);
    void onQuietSpot(bool quiet, float marginDb);
    void onFinished(bool applied);
    void pollNoiseFloor();
    void refreshFromSlice();
    void refreshSliceCombo();

private:
    void connectSlice(SliceModel* slice);
    void disconnectSlice(SliceModel* slice);
    void updateModeUi();
    int  liveValue() const;
    // Returns true when the current slice mode and DAX channel select TCI as
    // the calibration audio source (DIGU/DIGL with daxChannel >= 1). Requires
    // daxChannel >= 1 so that host-demodulating backends (HL2, etc.) and Flex
    // slices without a DAX channel assigned fall back to PC Audio rather than
    // producing a channel-match that can never fire.
    bool isTciMode() const;
    // Connect (or reconnect) the appropriate audio source to the calibration
    // engine based on the current slice mode. Keeps audio live at all times —
    // not just during auto sweeps — so the manual slider path always has fresh
    // RMS data. Called from the constructor, updateModeUi(), onStartStop()
    // (before startAutoSweep()), and onFinished().
    void rewireAudioSource();
    // Any noise reduction stage that mutates the audio before the calibration
    // tap is emitted. For the PC-Audio path: client-side NR2/RN2/NR4/DFNR/MNR/
    // BNR/NNR via AudioEngine, plus radio-side NR. For the TCI path: only
    // radio-side NR — client NR runs in AudioEngine which TCI never touches.
    bool nrSuppressesCalibration() const;

    RadioModel*  m_radio{nullptr};
    AudioEngine* m_audio{nullptr};
    TciServer*   m_tci{nullptr};
    NoiseFloorFn m_noiseFloorFn;
    // Connection to whichever audio source (PC Audio or TCI) is active.
    // Live while the dialog is open; rewired on mode/DAX change and at sweep
    // start (which latches the source for the sweep's duration).
    QMetaObject::Connection m_audioConn;
    // QPointer so slice removal mid-calibration doesn't dangle through the
    // polling timer or noise-floor lookup callback.
    QPointer<SliceModel> m_slice;

    AgcTCalibrator m_engine;
    QTimer*        m_floorTimer{nullptr};

    AgcCurveWidget* m_curve{nullptr};
    QLabel*         m_modeLabel{nullptr};
    QLabel*         m_sourceLabel{nullptr};
    QLabel*         m_quietLabel{nullptr};
    QLabel*         m_recLabel{nullptr};
    QLabel*         m_hintLabel{nullptr};
    QComboBox*      m_sliceCombo{nullptr};
    QPushButton*    m_startStopBtn{nullptr};
    QPushButton*    m_applyBtn{nullptr};
    QDoubleSpinBox* m_targetSpin{nullptr};
    QWidget*        m_targetRow{nullptr};
};

} // namespace AetherSDR
