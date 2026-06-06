#pragma once

#include "gui/PersistentDialog.h"
#include "core/AgcTCalibrator.h"

#include <QPointer>
#include <QVector>
#include <QWidget>

#include <functional>

class QLabel;
class QPushButton;
class QDoubleSpinBox;
class QTimer;

namespace AetherSDR {

class RadioModel;
class SliceModel;
class AudioEngine;

// AgcCurveWidget — lightweight 2D plot of post-AGC audio noise (y, dB) versus
// the AGC-T value (x, 0..100). Markers: current value, detected knee /
// recommended value, and (AGC-off) the comfortable-noise target line.
//
// Visual styling here is deliberately plain — final visual design is the
// maintainer's call (CLAUDE.md). This draws the data correctly; colors/fonts
// are placeholders.
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

// AgcCalibrationDialog — the calibration surface. Auto-sweep first (one click),
// with the live curve always visible so the operator can fine-tune by hand.
// Operates on the active slice; the radio stays authoritative (we only set the
// value through SliceModel).
class AgcCalibrationDialog : public PersistentDialog {
    Q_OBJECT
public:
    using NoiseFloorFn = std::function<float(SliceModel*)>;

    AgcCalibrationDialog(RadioModel* radio,
                         AudioEngine* audio,
                         NoiseFloorFn noiseFloorFn,
                         QWidget* parent = nullptr);

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

private:
    void connectSlice(SliceModel* slice);
    void disconnectSlice(SliceModel* slice);
    void updateModeUi();
    int  liveValue() const;
    // Any noise reduction stage that mutates the audio before levelChanged is
    // emitted: client-side NR2/RN2/NR4/DFNR/MNR/BNR via AudioEngine, plus the
    // radio-side NR (slice nr flag). The calibration tap is AudioEngine's
    // post-NR levelChanged, so any of these masks the knee we are trying to
    // find. See aethersdr-agent review on PR #3350.
    bool nrSuppressesCalibration() const;

    RadioModel*  m_radio{nullptr};
    AudioEngine* m_audio{nullptr};
    NoiseFloorFn m_noiseFloorFn;
    // QPointer so slice removal mid-calibration doesn't dangle through the
    // polling timer or noise-floor lookup callback.
    QPointer<SliceModel> m_slice;

    AgcTCalibrator m_engine;
    QTimer*        m_floorTimer{nullptr};

    AgcCurveWidget* m_curve{nullptr};
    QLabel*         m_modeLabel{nullptr};
    QLabel*         m_quietLabel{nullptr};
    QLabel*         m_recLabel{nullptr};
    QLabel*         m_hintLabel{nullptr};
    QPushButton*    m_startStopBtn{nullptr};
    QPushButton*    m_applyBtn{nullptr};
    QDoubleSpinBox* m_targetSpin{nullptr};
    QWidget*        m_targetRow{nullptr};
};

} // namespace AetherSDR
