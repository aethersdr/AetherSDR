#pragma once

#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QVector>

namespace AetherSDR {

class SliceModel;

// Headless engine that calibrates a slice's AGC-T against the noise floor (see
// docs/agc-t-calibration-design.md). AGC slow/med/fast: AGC-T is agc_threshold;
// find the knee where post-AGC audio RMS just begins to drop. AGC off: AGC-T is
// agc_off_level; solve for a target noise level. The 0-100 value has no
// radio-exposed dBm mapping (fw v4.2.18), so calibration is empirical, writing
// only through SliceModel setters. Auto (timer-stepped) and manual (user-moved)
// modes build the same curve for the same detector.
class AgcTCalibrator : public QObject {
    Q_OBJECT

public:
    enum class Strategy {
        Knee,        // AGC on  -> find the elbow in audio-RMS vs threshold
        TargetLevel  // AGC off -> solve for a target audio-noise level
    };

    struct Point {
        int   value;   // AGC-T value 0..100
        float rmsDb;   // post-AGC audio RMS, in dB (20*log10(rms))
    };

    explicit AgcTCalibrator(QObject* parent = nullptr);

    void setSlice(SliceModel* slice);
    SliceModel* slice() const { return m_slice.data(); }

    // Comfortable audio-noise target for the AGC-off solve, in dB (negative).
    void  setTargetLevelDb(float db) { m_targetDb = db; }
    float targetLevelDb() const { return m_targetDb; }

    // Per-step settle time for the auto sweep (radio AGC needs time to react).
    void setSettleMs(int ms) { m_settleMs = ms; }

    bool             isRunning() const { return m_running; }
    bool             isAuto() const { return m_auto; }
    Strategy         strategy() const;
    QVector<Point>   curve() const { return m_curve; }
    int              recommendedValue() const { return m_recommended; }
    bool             hasRecommendation() const { return m_recommended >= 0; }
    int              originalValue() const { return m_originalValue; }

public slots:
    // Live inputs.
    void onAudioLevel(float rms);            // AudioEngine::levelChanged (0..1 linear)
    void onSLevel(int sliceIndex, float dbm);// MeterModel::sLevelChanged (passband S+N)
    void setNoiseFloorDb(float dbm);         // measured RF noise floor (pan)
    void onValueChanged(int value);          // slice AGC-T value changed (manual record)

    // Control.
    void startAutoSweep();
    void stop();                  // abort: restore original value, finished(false)
    void applyRecommendation();   // keep recommended value, finished(true)
    void clear();                 // drop curve + recommendation (not running)

signals:
    void started(AgcTCalibrator::Strategy strategy, int originalValue);
    void progress(int currentValue, int percent);
    void pointAdded(int value, float rmsDb);
    void recommendation(int value, bool isKnee);
    void quietSpotStatus(bool quiet, float marginDb); // S-meter vs noise floor
    void finished(bool applied);

private:
    int   currentValue() const;          // read the active knob from the slice
    void  applyValue(int value);         // set the active knob on the slice
    bool  useOffLevel() const;           // true when AGC mode == "off"
    void  onSweepStep();                 // auto-sweep timer tick
    void  onManualSettle();              // manual record timer tick
    void  recordPoint(int value);        // append (value, currentRms) to curve
    void  finishSweep();                 // compute recommendation after a sweep
    void  recompute();                   // recompute recommendation from curve
    float currentRmsDb() const;
    void  evaluateQuietSpot();

    // QPointer so slice removal / disconnect mid-calibration doesn't dangle.
    // All call sites null-check before use.
    QPointer<SliceModel> m_slice;

    bool   m_running{false};
    bool   m_auto{false};
    int    m_originalValue{-1};
    int    m_recommended{-1};

    QVector<Point> m_curve;

    // Audio RMS smoothing (fed from the audio thread via a queued signal).
    float  m_rmsEma{0.0f};
    bool   m_haveRms{false};
    static constexpr float kRmsAlpha = 0.30f;
    static constexpr float kRmsFloor = 1.0e-6f; // avoid log10(0)

    // Quiet-spot guard.
    float  m_sLevelDbm{-200.0f};
    float  m_noiseFloorDbm{-200.0f};
    bool   m_haveSLevel{false};
    bool   m_haveFloor{false};
    static constexpr float kQuietMarginDb = 6.0f;

    // Auto sweep.
    QTimer m_stepTimer;
    int    m_sweepValue{100};
    int    m_sweepStart{100};
    static constexpr int kSweepStep = 4; // coarse step (100 -> 0)
    int    m_settleMs{280};

    // Manual record (settle after the user moves the slider).
    QTimer m_manualTimer;
    int    m_pendingManualValue{-1};

    // AGC-off comfortable-noise target (dB). Tunable; sensible default.
    float  m_targetDb{-28.0f};
};

} // namespace AetherSDR
