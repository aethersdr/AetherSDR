#include "core/AgcTCalibrator.h"

#include "models/SliceModel.h"

#include <QLoggingCategory>

#include <algorithm>
#include <cmath>

Q_LOGGING_CATEGORY(lcAgcCal, "aether.agccal", QtWarningMsg)

namespace AetherSDR {

AgcTCalibrator::AgcTCalibrator(QObject* parent)
    : QObject(parent)
{
    m_stepTimer.setSingleShot(true);
    connect(&m_stepTimer, &QTimer::timeout, this, &AgcTCalibrator::onSweepStep);

    m_manualTimer.setSingleShot(true);
    connect(&m_manualTimer, &QTimer::timeout, this, &AgcTCalibrator::onManualSettle);
}

void AgcTCalibrator::setSlice(SliceModel* slice)
{
    if (m_slice == slice) {
        return;
    }
    // Changing slices invalidates any in-flight calibration.
    if (m_running) {
        stop();
    }
    m_slice = slice;
    clear();
}

AgcTCalibrator::Strategy AgcTCalibrator::strategy() const
{
    return useOffLevel() ? Strategy::TargetLevel : Strategy::Knee;
}

bool AgcTCalibrator::useOffLevel() const
{
    return m_slice && m_slice->agcMode() == QStringLiteral("off");
}

int AgcTCalibrator::currentValue() const
{
    if (!m_slice) {
        return 0;
    }
    return useOffLevel() ? m_slice->agcOffLevel() : m_slice->agcThreshold();
}

void AgcTCalibrator::applyValue(int value)
{
    if (!m_slice) {
        return;
    }
    value = std::clamp(value, 0, 100);
    if (useOffLevel()) {
        m_slice->setAgcOffLevel(value);
    } else {
        m_slice->setAgcThreshold(value);
    }
}

float AgcTCalibrator::currentRmsDb() const
{
    const float rms = std::max(m_rmsEma, kRmsFloor);
    return 20.0f * std::log10(rms);
}

// ---- live inputs ----------------------------------------------------------

void AgcTCalibrator::onAudioLevel(float rms)
{
    if (!std::isfinite(rms)) {
        return;
    }
    rms = std::max(0.0f, rms);
    if (!m_haveRms) {
        m_rmsEma = rms;
        m_haveRms = true;
    } else {
        m_rmsEma = (1.0f - kRmsAlpha) * m_rmsEma + kRmsAlpha * rms;
    }
}

void AgcTCalibrator::onSLevel(int sliceIndex, float dbm)
{
    if (m_slice && sliceIndex != m_slice->sliceId()) {
        return;
    }
    if (!std::isfinite(dbm)) {
        return;
    }
    m_sLevelDbm = dbm;
    m_haveSLevel = true;
    evaluateQuietSpot();
}

void AgcTCalibrator::setNoiseFloorDb(float dbm)
{
    if (!std::isfinite(dbm)) {
        return;
    }
    m_noiseFloorDbm = dbm;
    m_haveFloor = true;
    evaluateQuietSpot();
}

void AgcTCalibrator::evaluateQuietSpot()
{
    if (!m_haveSLevel || !m_haveFloor) {
        return;
    }
    const float margin = m_sLevelDbm - m_noiseFloorDbm;
    emit quietSpotStatus(margin <= kQuietMarginDb, margin);
}

// ---- manual recording -----------------------------------------------------

void AgcTCalibrator::onValueChanged(int value)
{
    if (m_auto) {
        return; // the sweep drives the value itself
    }
    // Debounce: record once the value has settled.
    m_pendingManualValue = std::clamp(value, 0, 100);
    m_manualTimer.start(std::max(120, m_settleMs));
}

void AgcTCalibrator::onManualSettle()
{
    if (m_auto || m_pendingManualValue < 0) {
        return;
    }
    recordPoint(m_pendingManualValue);
    m_pendingManualValue = -1;
    recompute();
}

// ---- auto sweep -----------------------------------------------------------

void AgcTCalibrator::startAutoSweep()
{
    if (!m_slice) {
        qCWarning(lcAgcCal) << "startAutoSweep with no slice";
        return;
    }
    if (m_slice->externalReceiveReplacementActive()) {
        qCWarning(lcAgcCal)
            << "startAutoSweep skipped for external receive replacement";
        return;
    }
    if (m_running) {
        return;
    }
    m_originalValue = currentValue();
    m_recommended = -1;
    m_curve.clear();
    m_running = true;
    m_auto = true;
    m_sweepStart = 100;
    m_sweepValue = m_sweepStart;

    applyValue(m_sweepValue);
    emit started(strategy(), m_originalValue);
    emit progress(m_sweepValue, 0);
    // Wait for the AGC to settle before sampling this first point.
    m_stepTimer.start(m_settleMs);
}

void AgcTCalibrator::onSweepStep()
{
    if (!m_running || !m_auto) {
        return;
    }
    // Sample the value currently applied (already settled).
    recordPoint(m_sweepValue);

    const int span = std::max(1, m_sweepStart);
    const int done = m_sweepStart - m_sweepValue;
    emit progress(m_sweepValue, static_cast<int>(100.0 * done / span));

    if (m_sweepValue <= 0) {
        finishSweep();
        return;
    }
    m_sweepValue = std::max(0, m_sweepValue - kSweepStep);
    applyValue(m_sweepValue);
    m_stepTimer.start(m_settleMs);
}

void AgcTCalibrator::finishSweep()
{
    m_stepTimer.stop();
    m_running = false;
    m_auto = false;
    recompute();
    // Apply the recommendation so the operator immediately hears the result.
    // They still confirm with Apply (keep) or Stop (restore original).
    if (m_recommended >= 0) {
        applyValue(m_recommended);
    }
    emit finished(false); // not yet *confirmed*; UI decides keep vs restore
}

// ---- recording + detection ------------------------------------------------

void AgcTCalibrator::recordPoint(int value)
{
    const float db = currentRmsDb();
    // Replace an existing sample at the same value (manual re-visits).
    for (Point& p : m_curve) {
        if (p.value == value) {
            p.rmsDb = db;
            emit pointAdded(value, db);
            return;
        }
    }
    m_curve.push_back({value, db});
    std::sort(m_curve.begin(), m_curve.end(),
              [](const Point& a, const Point& b) { return a.value < b.value; });
    emit pointAdded(value, db);
}

void AgcTCalibrator::recompute()
{
    if (m_curve.size() < 3) {
        return;
    }

    QVector<Point> pts = m_curve; // already sorted by value ascending

    if (strategy() == Strategy::TargetLevel) {
        // AGC off: monotonic rms vs value. Solve for the value whose audio-noise
        // level crosses the comfortable target, via linear interpolation.
        int best = pts.first().value;
        float bestErr = std::abs(pts.first().rmsDb - m_targetDb);
        for (int i = 1; i < pts.size(); ++i) {
            const Point& a = pts[i - 1];
            const Point& b = pts[i];
            // crossing between a and b?
            const bool brackets = (a.rmsDb - m_targetDb) * (b.rmsDb - m_targetDb) <= 0.0f;
            if (brackets && std::abs(b.rmsDb - a.rmsDb) > 1e-3f) {
                const float t = (m_targetDb - a.rmsDb) / (b.rmsDb - a.rmsDb);
                best = std::clamp(static_cast<int>(std::lround(a.value + t * (b.value - a.value))), 0, 100);
                m_recommended = best;
                emit recommendation(best, /*isKnee=*/false);
                return;
            }
            const float err = std::abs(b.rmsDb - m_targetDb);
            if (err < bestErr) {
                bestErr = err;
                best = b.value;
            }
        }
        m_recommended = best;
        emit recommendation(best, /*isKnee=*/false);
        return;
    }

    // AGC on: find the knee (elbow) via maximum perpendicular distance from the
    // chord joining the first and last points. On a curve of audio-RMS vs
    // threshold, the noise stays high/flat at high thresholds then bends down as
    // the threshold drops below the noise level — the bend is the knee.
    const Point& first = pts.first();
    const Point& last = pts.last();
    const float dx = static_cast<float>(last.value - first.value);
    const float dy = last.rmsDb - first.rmsDb;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len < 1e-6f) {
        return;
    }

    int kneeValue = first.value;
    float maxDist = -1.0f;
    for (const Point& p : pts) {
        // Perpendicular distance from point p to the chord.
        const float num = std::abs(dy * (p.value - first.value) - dx * (p.rmsDb - first.rmsDb));
        const float dist = num / len;
        if (dist > maxDist) {
            maxDist = dist;
            kneeValue = p.value;
        }
    }

    m_recommended = std::clamp(kneeValue, 0, 100);
    emit recommendation(m_recommended, /*isKnee=*/true);
}

// ---- control ---------------------------------------------------------------

void AgcTCalibrator::stop()
{
    const bool wasRunning = m_running || m_auto;
    m_stepTimer.stop();
    m_manualTimer.stop();
    m_running = false;
    m_auto = false;
    if (m_originalValue >= 0) {
        applyValue(m_originalValue); // restore what we found
    }
    if (wasRunning) {
        emit finished(false);
    }
}

void AgcTCalibrator::applyRecommendation()
{
    m_stepTimer.stop();
    m_manualTimer.stop();
    m_running = false;
    m_auto = false;
    if (m_recommended >= 0) {
        applyValue(m_recommended);
    }
    emit finished(true);
}

void AgcTCalibrator::clear()
{
    if (m_running) {
        return;
    }
    m_curve.clear();
    m_recommended = -1;
    m_pendingManualValue = -1;
    m_manualTimer.stop();
}

} // namespace AetherSDR
