#pragma once

#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QVector>

namespace AetherSDR {

class SliceModel;

// AgcTCalibrator — headless engine that calibrates a slice's AGC-T value
// against the receiver noise floor.
//
// Background (see docs/agc-t-calibration-design.md):
//   * When AGC mode is slow/med/fast, the AGC-T knob is `agc_threshold` (the
//     AGC "knee"). The goal is to find the knee: lower the threshold until the
//     audio noise *just begins to decrease*. We detect that elbow on a curve of
//     post-AGC audio RMS vs threshold value.
//   * When AGC mode is off, the knob is `agc_off_level` (a fixed gain). There is
//     no knee; instead we solve for the value that places the audio-noise level
//     at a comfortable target.
//
// The 0-100 knob value has no radio-exposed dBm mapping (firmware v4.2.18), so
// calibration is empirical: we vary the knob and observe the post-AGC audio RMS
// fed in via onAudioLevel(). The radio remains authoritative — we only ever set
// the value through SliceModel's setters (acting like a user turning the knob).
//
// Auto and manual calibration share this one engine: in auto mode a timer steps
// the value and samples; in manual mode the user moves the slider and we record
// (value, rms) points after each value settles. Both build the same curve, and
// the same knee/target detector runs over it.
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
