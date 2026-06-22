#include "gui/AgcCalibrationDialog.h"

#include "core/AudioEngine.h"
#include "models/MeterModel.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace AetherSDR {

// ===========================================================================
// AgcCurveWidget
// ===========================================================================

AgcCurveWidget::AgcCurveWidget(QWidget* parent)
    : QWidget(parent)
{
    setMinimumSize(360, 200);
    setAttribute(Qt::WA_StyledBackground, true);
}

void AgcCurveWidget::setCurve(const QVector<AgcTCalibrator::Point>& pts)
{
    m_pts = pts;
    update();
}

void AgcCurveWidget::setCurrentValue(int value)
{
    if (m_current != value) {
        m_current = value;
        update();
    }
}

void AgcCurveWidget::setRecommended(int value, bool isKnee)
{
    m_recommended = value;
    m_recIsKnee = isKnee;
    update();
}

void AgcCurveWidget::setStrategy(AgcTCalibrator::Strategy s)
{
    if (m_strategy != s) {
        m_strategy = s;
        update();
    }
}

void AgcCurveWidget::setTargetDb(float db)
{
    m_targetDb = db;
    update();
}

void AgcCurveWidget::clear()
{
    m_pts.clear();
    m_current = -1;
    m_recommended = -1;
    update();
}

void AgcCurveWidget::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QRectF full = rect();
    const qreal padL = 8.0, padR = 8.0, padT = 8.0, padB = 18.0;
    const QRectF plot(full.left() + padL, full.top() + padT,
                      full.width() - padL - padR, full.height() - padT - padB);

    p.fillRect(full, QColor(18, 20, 24));
    p.setPen(QPen(QColor(70, 76, 86), 1.0));
    p.drawRect(plot);

    auto xForValue = [&](int v) {
        return plot.left() + (v / 100.0) * plot.width();
    };

    // Y range from data (audio-noise dB), with padding; fall back to a default.
    float yMin = -90.0f, yMax = 0.0f;
    if (!m_pts.isEmpty()) {
        yMin = yMax = m_pts.first().rmsDb;
        for (const auto& pt : m_pts) {
            yMin = std::min(yMin, pt.rmsDb);
            yMax = std::max(yMax, pt.rmsDb);
        }
        const float span = std::max(6.0f, yMax - yMin);
        yMin -= span * 0.12f;
        yMax += span * 0.12f;
    }
    auto yForDb = [&](float db) {
        const float t = (db - yMin) / std::max(1e-3f, (yMax - yMin));
        return plot.bottom() - t * plot.height();
    };

    // AGC-off target line.
    if (m_strategy == AgcTCalibrator::Strategy::TargetLevel
        && m_targetDb >= yMin && m_targetDb <= yMax) {
        p.setPen(QPen(QColor(90, 150, 220), 1.0, Qt::DashLine));
        const qreal y = yForDb(m_targetDb);
        p.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
    }

    // Curve.
    if (m_pts.size() >= 2) {
        QPainterPath path;
        path.moveTo(xForValue(m_pts.first().value), yForDb(m_pts.first().rmsDb));
        for (int i = 1; i < m_pts.size(); ++i) {
            path.lineTo(xForValue(m_pts[i].value), yForDb(m_pts[i].rmsDb));
        }
        p.setPen(QPen(QColor(120, 210, 140), 2.0));
        p.drawPath(path);
    }
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(150, 230, 170));
    for (const auto& pt : m_pts) {
        p.drawEllipse(QPointF(xForValue(pt.value), yForDb(pt.rmsDb)), 2.0, 2.0);
    }

    // Recommended / knee marker.
    if (m_recommended >= 0) {
        const qreal x = xForValue(m_recommended);
        p.setPen(QPen(QColor(245, 200, 90), 2.0));
        p.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
        p.setPen(QColor(245, 200, 90));
        p.drawText(QPointF(std::min(x + 4, plot.right() - 70), plot.top() + 12),
                   m_recIsKnee ? QStringLiteral("knee %1").arg(m_recommended)
                               : QStringLiteral("target %1").arg(m_recommended));
    }

    // Current value marker.
    if (m_current >= 0) {
        const qreal x = xForValue(m_current);
        p.setPen(QPen(QColor(220, 220, 230), 1.0, Qt::DotLine));
        p.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
    }

    // X axis labels.
    p.setPen(QColor(150, 156, 166));
    p.drawText(QPointF(plot.left(), plot.bottom() + 14), QStringLiteral("0"));
    p.drawText(QPointF(plot.right() - 24, plot.bottom() + 14), QStringLiteral("100"));
    p.drawText(QPointF(plot.center().x() - 30, plot.bottom() + 14),
               QStringLiteral("AGC-T"));
}

// ===========================================================================
// AgcCalibrationDialog
// ===========================================================================

AgcCalibrationDialog::AgcCalibrationDialog(RadioModel* radio,
                                           AudioEngine* audio,
                                           NoiseFloorFn noiseFloorFn,
                                           QWidget* parent)
    : PersistentDialog(QStringLiteral("AGC-T Calibration"),
                       QStringLiteral("AgcCalibrationDialogGeometry"), parent),
      m_radio(radio), m_audio(audio), m_noiseFloorFn(std::move(noiseFloorFn))
{
    setMinimumSize(420, 360);
    resize(460, 420);

    auto* body = new QVBoxLayout(bodyWidget());
    body->setContentsMargins(10, 10, 10, 10);
    body->setSpacing(8);

    m_modeLabel = new QLabel(this);
    m_quietLabel = new QLabel(this);
    body->addWidget(m_modeLabel);
    body->addWidget(m_quietLabel);

    m_curve = new AgcCurveWidget(this);
    body->addWidget(m_curve, 1);

    m_recLabel = new QLabel(this);
    body->addWidget(m_recLabel);

    // AGC-off advanced: comfortable-noise target.
    m_targetRow = new QWidget(this);
    auto* targetLayout = new QHBoxLayout(m_targetRow);
    targetLayout->setContentsMargins(0, 0, 0, 0);
    targetLayout->addWidget(new QLabel(QStringLiteral("Target audio noise (dB):"), m_targetRow));
    m_targetSpin = new QDoubleSpinBox(m_targetRow);
    m_targetSpin->setRange(-80.0, 0.0);
    m_targetSpin->setSingleStep(1.0);
    m_targetSpin->setValue(m_engine.targetLevelDb());
    targetLayout->addWidget(m_targetSpin);
    targetLayout->addStretch(1);
    body->addWidget(m_targetRow);
    connect(m_targetSpin, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        m_engine.setTargetLevelDb(static_cast<float>(v));
        m_curve->setTargetDb(static_cast<float>(v));
    });

    auto* buttons = new QHBoxLayout;
    m_startStopBtn = new QPushButton(QStringLiteral("Auto Sweep"), this);
    m_applyBtn = new QPushButton(QStringLiteral("Apply"), this);
    m_applyBtn->setEnabled(false);
    buttons->addWidget(m_startStopBtn);
    buttons->addStretch(1);
    buttons->addWidget(m_applyBtn);
    body->addLayout(buttons);

    m_hintLabel = new QLabel(
        QStringLiteral("Tune to a clear spot, then Auto Sweep — or move the AGC-T "
                       "slider and watch where the noise bends."), this);
    m_hintLabel->setWordWrap(true);
    body->addWidget(m_hintLabel);

    connect(m_startStopBtn, &QPushButton::clicked, this, &AgcCalibrationDialog::onStartStop);
    connect(m_applyBtn, &QPushButton::clicked, this, &AgcCalibrationDialog::onApply);

    // Engine wiring.
    connect(&m_engine, &AgcTCalibrator::started, this, &AgcCalibrationDialog::onStarted);
    connect(&m_engine, &AgcTCalibrator::progress, this, &AgcCalibrationDialog::onProgress);
    connect(&m_engine, &AgcTCalibrator::pointAdded, this, &AgcCalibrationDialog::onPointAdded);
    connect(&m_engine, &AgcTCalibrator::recommendation, this, &AgcCalibrationDialog::onRecommendation);
    connect(&m_engine, &AgcTCalibrator::quietSpotStatus, this, &AgcCalibrationDialog::onQuietSpot);
    connect(&m_engine, &AgcTCalibrator::finished, this, &AgcCalibrationDialog::onFinished);

    // Live audio level (cross-thread; auto-queued) and S-meter feed.
    if (m_audio) {
        connect(m_audio, &AudioEngine::levelChanged,
                &m_engine, &AgcTCalibrator::onAudioLevel);
    }
    if (m_radio) {
        connect(&m_radio->meterModel(), &MeterModel::sLevelChanged,
                &m_engine, &AgcTCalibrator::onSLevel);
    }

    // Periodically feed the measured RF noise floor for the quiet-spot guard.
    m_floorTimer = new QTimer(this);
    m_floorTimer->setInterval(500);
    connect(m_floorTimer, &QTimer::timeout, this, &AgcCalibrationDialog::pollNoiseFloor);
    m_floorTimer->start();

    updateModeUi();
}

void AgcCalibrationDialog::setSlice(SliceModel* slice)
{
    if (m_slice == slice) {
        return;
    }
    if (m_slice) {
        disconnectSlice(m_slice);
    }
    m_slice = slice;
    m_engine.setSlice(slice);
    if (m_slice) {
        connectSlice(m_slice);
    }
    m_curve->clear();
    m_applyBtn->setEnabled(false);
    updateModeUi();
    refreshFromSlice();
}

void AgcCalibrationDialog::connectSlice(SliceModel* slice)
{
    connect(slice, &SliceModel::agcModeChanged, this, &AgcCalibrationDialog::updateModeUi);
    connect(slice, &SliceModel::externalReceiveAgcModeChanged,
            this, &AgcCalibrationDialog::updateModeUi);
    connect(slice, &SliceModel::agcThresholdChanged, &m_engine, &AgcTCalibrator::onValueChanged);
    connect(slice, &SliceModel::agcOffLevelChanged, &m_engine, &AgcTCalibrator::onValueChanged);
    connect(slice, &SliceModel::agcThresholdChanged, this, &AgcCalibrationDialog::refreshFromSlice);
    connect(slice, &SliceModel::agcOffLevelChanged, this, &AgcCalibrationDialog::refreshFromSlice);
}

void AgcCalibrationDialog::disconnectSlice(SliceModel* slice)
{
    disconnect(slice, nullptr, &m_engine, nullptr);
    disconnect(slice, nullptr, this, nullptr);
}

int AgcCalibrationDialog::liveValue() const
{
    if (!m_slice) {
        return -1;
    }
    if (m_slice->externalReceiveReplacementActive()) {
        return m_slice->receiveAgcMode() == QStringLiteral("off")
                   ? m_slice->receiveAgcOffLevel()
                   : m_slice->receiveAgcThreshold();
    }
    return m_slice->agcMode() == QStringLiteral("off")
               ? m_slice->agcOffLevel()
               : m_slice->agcThreshold();
}

bool AgcCalibrationDialog::nrSuppressesCalibration() const
{
    if (m_audio && (m_audio->nr2Enabled() || m_audio->rn2Enabled()
                  || m_audio->nr4Enabled() || m_audio->dfnrEnabled()
                  || m_audio->mnrEnabled() || m_audio->bnrEnabled())) {
        return true;
    }
    if (m_slice && m_slice->nrOn()) {
        return true;
    }
    return false;
}

void AgcCalibrationDialog::updateModeUi()
{
    const bool externalReceive =
        m_slice && m_slice->externalReceiveReplacementActive();
    const QString mode = m_slice
        ? (externalReceive ? m_slice->receiveAgcMode() : m_slice->agcMode())
        : QString();
    const bool off = mode == QStringLiteral("off");
    const AgcTCalibrator::Strategy s =
        off ? AgcTCalibrator::Strategy::TargetLevel : AgcTCalibrator::Strategy::Knee;
    m_curve->setStrategy(s);
    m_targetRow->setVisible(off);

    // If NR is active (client or radio), the levelChanged tap reads post-NR
    // audio and the AGC knee is masked. Warn in the hint label; the auto-sweep
    // start path also refuses to begin until NR is off.
    if (nrSuppressesCalibration() && m_hintLabel) {
        m_hintLabel->setText(QStringLiteral(
            "⚠ Disable Noise Reduction (NR/NR2/RN2/NR4/DFNR/MNR/BNR) for accurate "
            "calibration — NR crushes the noise floor the knee detector is "
            "looking for."));
    } else if (m_hintLabel) {
        m_hintLabel->setText(QStringLiteral(
            "Tune to a clear spot, then Auto Sweep — or move the AGC-T "
            "slider and watch where the noise bends."));
    }

    if (!m_slice) {
        m_modeLabel->setText(QStringLiteral("No active slice."));
        m_startStopBtn->setEnabled(false);
        return;
    }
    if (externalReceive) {
        m_modeLabel->setText(QStringLiteral(
            "AGC-T calibration is available for Flex receive only."));
        m_startStopBtn->setEnabled(false);
        return;
    }
    m_startStopBtn->setEnabled(true);
    if (off) {
        m_modeLabel->setText(QStringLiteral(
            "AGC: OFF — calibrating fixed gain (agc_off_level) to a comfortable "
            "noise level. No knee in this mode."));
    } else {
        m_modeLabel->setText(QStringLiteral(
            "AGC: %1 — finding the knee (agc_threshold) just above the noise floor.")
            .arg(m_slice->agcMode().toUpper()));
    }
}

void AgcCalibrationDialog::refreshFromSlice()
{
    m_curve->setCurrentValue(liveValue());
}

void AgcCalibrationDialog::pollNoiseFloor()
{
    if (!m_slice || !m_noiseFloorFn) {
        return;
    }
    const float floorDb = m_noiseFloorFn(m_slice);
    if (std::isfinite(floorDb)) {
        m_engine.setNoiseFloorDb(floorDb);
    }
}

void AgcCalibrationDialog::onStartStop()
{
    if (m_engine.isRunning()) {
        m_engine.stop(); // restores original value
        return;
    }
    if (m_slice && m_slice->externalReceiveReplacementActive()) {
        updateModeUi();
        return;
    }
    // Refuse to start while any NR stage is suppressing the calibration tap.
    // The user can disable NR and try again. Mirrors the quiet-spot guard
    // pattern in the engine and matches the in-hint warning text.
    if (nrSuppressesCalibration()) {
        QMessageBox::warning(
            this,
            QStringLiteral("Disable NR for AGC-T calibration"),
            QStringLiteral(
                "Noise Reduction is active and is crushing the audio noise "
                "floor that the AGC knee detector measures. Turn off any of "
                "NR / NR2 / RN2 / NR4 / DFNR / MNR / BNR, then try Auto Sweep "
                "again."),
            QMessageBox::Ok);
        return;
    }
    m_curve->clear();
    m_applyBtn->setEnabled(false);
    m_engine.startAutoSweep();
}

void AgcCalibrationDialog::onApply()
{
    m_engine.applyRecommendation();
}

void AgcCalibrationDialog::onStarted(AgcTCalibrator::Strategy strategy, int originalValue)
{
    Q_UNUSED(originalValue);
    m_curve->setStrategy(strategy);
    m_startStopBtn->setText(QStringLiteral("Stop"));
    m_applyBtn->setEnabled(false);
    m_recLabel->setText(QStringLiteral("Sweeping…"));
}

void AgcCalibrationDialog::onProgress(int currentValue, int percent)
{
    m_curve->setCurrentValue(currentValue);
    m_recLabel->setText(QStringLiteral("Sweeping… %1%").arg(percent));
}

void AgcCalibrationDialog::onPointAdded(int value, float rmsDb)
{
    Q_UNUSED(value);
    Q_UNUSED(rmsDb);
    m_curve->setCurve(m_engine.curve());
}

void AgcCalibrationDialog::onRecommendation(int value, bool isKnee)
{
    m_curve->setRecommended(value, isKnee);
    m_recLabel->setText(isKnee
        ? QStringLiteral("Recommended AGC-T (knee): %1").arg(value)
        : QStringLiteral("Recommended AGC-T (target): %1").arg(value));
    if (!m_engine.isRunning()) {
        m_applyBtn->setEnabled(true);
    }
}

void AgcCalibrationDialog::onQuietSpot(bool quiet, float marginDb)
{
    if (quiet) {
        m_quietLabel->setText(QStringLiteral("✓ Quiet spot (signal %1 dB over floor).")
                                  .arg(marginDb, 0, 'f', 1));
    } else {
        m_quietLabel->setText(QStringLiteral("⚠ Signal in passband (%1 dB over floor) "
                                             "— tune to a clear spot for best results.")
                                  .arg(marginDb, 0, 'f', 1));
    }
}

void AgcCalibrationDialog::onFinished(bool applied)
{
    m_startStopBtn->setText(QStringLiteral("Auto Sweep"));
    if (applied) {
        m_applyBtn->setEnabled(false);
        m_recLabel->setText(m_engine.hasRecommendation()
            ? QStringLiteral("Applied AGC-T = %1.").arg(m_engine.recommendedValue())
            : QStringLiteral("Applied."));
    } else if (m_engine.hasRecommendation()) {
        // Sweep done; recommendation applied to the radio but not yet confirmed.
        m_applyBtn->setEnabled(true);
    }
    refreshFromSlice();
}

} // namespace AetherSDR
