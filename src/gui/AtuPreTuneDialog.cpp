#include "AtuPreTuneDialog.h"
#include "AtuPreTuneCenters.h"
#include "FramelessWindowTitleBar.h"
#include "FramelessResizer.h"
#include "core/AppSettings.h"
#include "models/BandPlanManager.h"
#include "models/MeterModel.h"
#include "models/PanadapterModel.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"
#include "models/TransmitModel.h"

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace AetherSDR {

namespace {

// Canonical HF/6m amateur bands. Each row carries a search window used to pick
// which segments of the active BandPlanManager belong to the band — actual
// low/high are derived from those segments so the sweep follows the region's
// regulatory edges rather than a hard-coded table. Segment sizes follow the
// issue's IARU R1 reference table. (#2624)
struct BandSpec {
    const char* name;
    double  searchLowMhz;
    double  searchHighMhz;
    int     segmentKhz;
};

constexpr BandSpec kBandSpecs[] = {
    {"160m",  1.6,  2.1,    9},
    {"80m",   3.3,  4.1,    9},
    {"60m",   5.2,  5.5,    9},
    {"40m",   6.9,  7.4,   25},
    {"30m",  10.0, 10.2,   25},
    {"20m",  13.9, 14.5,   51},
    {"17m",  18.0, 18.3,   51},
    {"15m",  20.9, 21.6,   75},
    {"12m",  24.8, 25.1,   75},
    {"10m",  27.9, 30.0,   75},
    {"6m",   49.0, 54.5,  101},
};

constexpr const char* kDisclaimerText =
    "<b>Operator Responsibility:</b> You must ensure that your transmissions "
    "do not interfere with other radio traffic. Always verify that the selected "
    "frequency is clear before tuning, and never leave this process unattended "
    "unless you fully understand its behavior, failure modes, and risks. "
    "AetherSDR transmits at tune power on each frequency in the sweep. You are "
    "responsible for compliance with your license and local regulations. Do not "
    "use this function for unattended or automated transmission unless you "
    "fully understand its behavior, failure modes, and risks.";

constexpr int kSecondsPerPointEstimate = 15;
constexpr int kPerPointTimeoutMs = 30 * 1000;
constexpr int kSettleMs = 300;
constexpr int kMaxConsecutiveFailBypass = 3;

// Operator-attention guardrail: warn if the planned sweep exceeds this many
// points across all selected bands. Catches the "select-all on a wide
// regional plan" footgun where 100+ keyings can run for tens of minutes
// unattended. (#2649 #4)
constexpr int kMaxPointsSoftCap = 100;

// Equipment guardrail: warn if tune power exceeds this watts when entering
// Auto mode. Some amps need higher drive, so the gate is a warning rather
// than a refusal — Principle XIII (the operator outranks the agent). (#2649 #5)
constexpr int kAutoModeTunePowerWarnW = 20;

// AppSettings key for the operator's licence class code (e.g. "G" for
// General). Stored as a single nested-JSON-equivalent flat string per
// Principle V's pragmatic note about standalone keys when no related block
// exists yet to nest under. (#2649 #8)
constexpr const char* kLicenseClassKey = "OperatorLicenseClass";

// computeCenters() is now in AtuPreTuneCenters.h (header-only) so the
// unit test can exercise it without dragging in this dialog. (#2648)
int pointsForRange(double lowMhz, double highMhz, int segmentKhz)
{
    return computeCenters(lowMhz, highMhz, segmentKhz).size();
}

// Walk each contiguous region from BandPlanManager (which already merges
// adjacent segments — see #2822) and produce tune centers per region. On
// discrete-channel bands (US 60m: 5 USB channels at 2.8 kHz each separated
// by tens of kHz of gap) this generates one tune-center per legal channel
// instead of stepping across illegal gaps and triggering radio-side
// out-of-band rejections. On continuous bands the merge collapses to a
// single [min, max] region and the walk degenerates to the original
// even-stride walk. (#2647)
QVector<double> computeCentersForBand(
    const BandPlanManager& bandPlan,
    double searchLowMhz, double searchHighMhz, int segmentKhz,
    const QString& allowedLicenseClass = QString())
{
    QVector<double> out;
    if (segmentKhz <= 0) return out;

    // License-class filter (#2649 #8): when set, drop segments whose
    // non-empty license field doesn't include the operator's class. The
    // filter runs before the merge so an allowed segment adjacent to a
    // disallowed one stays as a separate region.
    const auto regions = bandPlan.contiguousRegionsForBand(
        searchLowMhz, searchHighMhz, allowedLicenseClass);
    if (regions.isEmpty()) return out;

    // If a region is narrower than one full tune segment (e.g. US 60m's
    // 2.8 kHz channel vs 9 kHz segment), emit a single midpoint so the
    // ATU still tunes that channel; computeCenters() returns empty for
    // narrow ranges.
    const double segMhz = segmentKhz / 1000.0;
    for (const auto& r : regions) {
        if (r.highMhz - r.lowMhz < segMhz) {
            out.append((r.lowMhz + r.highMhz) / 2.0);
        } else {
            out.append(computeCenters(r.lowMhz, r.highMhz, segmentKhz));
        }
    }
    return out;
}

} // namespace

AtuPreTuneDialog::AtuPreTuneDialog(RadioModel* radio,
                                   BandPlanManager* bandPlan,
                                   QWidget* parent)
    : QDialog(parent),
      m_radio(radio),
      m_bandPlan(bandPlan)
{
    setWindowTitle("ATU Band Pre-Tune Sweep");
    setModal(false);
    resize(560, 640);

    m_settleTimer = new QTimer(this);
    m_settleTimer->setSingleShot(true);
    m_settleTimer->setInterval(kSettleMs);
    connect(m_settleTimer, &QTimer::timeout, this, [this]() {
        if (!m_radio) return;
        m_waitingForAtu = true;
        m_tuneLastSwr = 0.0f;
        m_swrTracking = true;
        m_timeoutTimer->start(kPerPointTimeoutMs);
        m_radio->transmitModel().atuStart();
    });

    m_timeoutTimer = new QTimer(this);
    m_timeoutTimer->setSingleShot(true);
    connect(m_timeoutTimer, &QTimer::timeout,
            this, &AtuPreTuneDialog::onPerPointTimeout);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    auto* titleBar = new FramelessWindowTitleBar("ATU Band Pre-Tune Sweep", this);
    m_titleBar = titleBar;
    outer->addWidget(m_titleBar);

    auto* body = new QWidget(this);
    m_bodyLayout = new QVBoxLayout(body);
    m_bodyLayout->setContentsMargins(12, 10, 12, 10);
    m_bodyLayout->setSpacing(8);

    m_pages = new QStackedWidget(body);
    m_bodyLayout->addWidget(m_pages, 1);
    outer->addWidget(body, 1);

    buildConfigPage();
    buildSweepPage();
    m_pages->setCurrentWidget(m_configPage);

    populateLicenseClassRow();
    populateBands();

    if (m_licenseClassCombo) {
        connect(m_licenseClassCombo,
                QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this](int) {
            const QString code = selectedLicenseClass();
            AppSettings::instance().setValue(kLicenseClassKey, code);
            AppSettings::instance().save();
            populateBands();  // re-filter centers under the new class
        });
    }

    if (m_radio) {
        connect(&m_radio->transmitModel(), &TransmitModel::atuStateChanged,
                this, &AtuPreTuneDialog::onAtuStateChanged);
        // Sample SWR while waiting for ATU terminal state.  The radio
        // resets SWR to exactly 1.0 in the final meter packet of each tune
        // cycle (right before the terminal status arrives) even at full
        // forward power — track the most recent reading > 1.001 so we
        // report the settled post-tune SWR, not the reset artifact. (#2624)
        connect(&m_radio->meterModel(), &MeterModel::txMetersChanged,
                this, [this](float, float swr) {
            if (m_swrTracking && swr > 1.001f)
                m_tuneLastSwr = swr;
        });
    }

    setFramelessMode(AppSettings::instance().value("FramelessWindow", "True").toString() == "True");
    FramelessResizer::install(this);
}

void AtuPreTuneDialog::setFramelessMode(bool on)
{
    const bool wasVisible = isVisible();
    const QRect g = geometry();
    if (on) {
        setWindowFlag(Qt::FramelessWindowHint, true);
        if (m_titleBar) m_titleBar->show();
    } else {
        setWindowFlag(Qt::FramelessWindowHint, false);
        if (m_titleBar) m_titleBar->hide();
    }
    if (wasVisible) {
        setGeometry(g);
        show();
    }
}

void AtuPreTuneDialog::buildConfigPage()
{
    m_configPage = new QWidget(m_pages);
    auto* layout = new QVBoxLayout(m_configPage);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    m_planNameLabel = new QLabel(m_configPage);
    m_planNameLabel->setWordWrap(true);
    m_planNameLabel->setStyleSheet("color: #c8d8e8; font-size: 11px;");
    layout->addWidget(m_planNameLabel);

    auto* disclaimer = new QLabel(kDisclaimerText, m_configPage);
    disclaimer->setWordWrap(true);
    disclaimer->setTextFormat(Qt::RichText);
    disclaimer->setStyleSheet(
        "QLabel { background: #2a1a10; border: 1px solid #804020; "
        "color: #ffd0a0; padding: 8px; font-size: 11px; }");
    layout->addWidget(disclaimer);

    {
        auto* row = new QHBoxLayout;
        auto* lbl = new QLabel("Mode:", m_configPage);
        lbl->setStyleSheet("color: #8aa8c0; font-size: 11px;");
        row->addWidget(lbl);
        m_modeCombo = new QComboBox(m_configPage);
        m_modeCombo->addItem("Step (confirm each point)", static_cast<int>(Mode::Step));
        m_modeCombo->addItem("Auto (run unattended)",     static_cast<int>(Mode::Auto));
        row->addWidget(m_modeCombo, 1);
        layout->addLayout(row);
    }

    // Licence-class filter row (#2649 #8). Populated in populateLicenseClassRow();
    // hidden entirely when the active plan declares no license_classes block.
    m_licenseClassRow = new QWidget(m_configPage);
    {
        auto* row = new QHBoxLayout(m_licenseClassRow);
        row->setContentsMargins(0, 0, 0, 0);
        auto* lbl = new QLabel("License class:", m_licenseClassRow);
        lbl->setStyleSheet("color: #8aa8c0; font-size: 11px;");
        row->addWidget(lbl);
        m_licenseClassCombo = new QComboBox(m_licenseClassRow);
        row->addWidget(m_licenseClassCombo, 1);
    }
    layout->addWidget(m_licenseClassRow);
    m_licenseClassRow->setVisible(false);

    auto* scroll = new QScrollArea(m_configPage);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    m_bandsContainer = new QWidget(scroll);
    m_bandsLayout = new QVBoxLayout(m_bandsContainer);
    m_bandsLayout->setContentsMargins(0, 0, 0, 0);
    m_bandsLayout->setSpacing(4);
    scroll->setWidget(m_bandsContainer);
    layout->addWidget(scroll, 1);

    auto* btnRow = new QHBoxLayout;
    btnRow->addStretch(1);
    m_cancelBtn = new QPushButton("Cancel", m_configPage);
    m_startBtn  = new QPushButton("START", m_configPage);
    m_startBtn->setStyleSheet(
        "QPushButton { background: #006030; border: 1px solid #008040; "
        "color: #fff; padding: 4px 12px; font-weight: bold; }"
        "QPushButton:hover { background: #007038; }"
        "QPushButton:disabled { background: #1a2a1a; color: #556070; }");
    btnRow->addWidget(m_cancelBtn);
    btnRow->addWidget(m_startBtn);
    layout->addLayout(btnRow);

    connect(m_cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_startBtn,  &QPushButton::clicked, this, &AtuPreTuneDialog::onStartClicked);

    m_pages->addWidget(m_configPage);
}

void AtuPreTuneDialog::buildSweepPage()
{
    m_sweepPage = new QWidget(m_pages);
    auto* layout = new QVBoxLayout(m_sweepPage);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    m_sweepStatus = new QLabel("", m_sweepPage);
    m_sweepStatus->setWordWrap(true);
    m_sweepStatus->setStyleSheet(
        "QLabel { color: #c8d8e8; font-size: 13px; font-weight: bold; }");
    layout->addWidget(m_sweepStatus);

    m_sweepProgress = new QLabel("", m_sweepPage);
    m_sweepProgress->setStyleSheet("QLabel { color: #8aa8c0; font-size: 11px; }");
    layout->addWidget(m_sweepProgress);

    m_sweepResult = new QLabel("", m_sweepPage);
    m_sweepResult->setWordWrap(true);
    m_sweepResult->setStyleSheet("QLabel { color: #c8d8e8; font-size: 11px; }");
    layout->addWidget(m_sweepResult, 1);

    auto* row = new QHBoxLayout;
    m_tuneBtn  = new QPushButton("Tune this frequency", m_sweepPage);
    m_tuneBtn->setStyleSheet(
        "QPushButton { background: #006030; border: 1px solid #008040; "
        "color: #fff; padding: 4px 12px; font-weight: bold; }"
        "QPushButton:hover { background: #007038; }"
        "QPushButton:disabled { background: #1a2a1a; color: #556070; }");
    m_skipBtn  = new QPushButton("Skip", m_sweepPage);
    m_continueAfterFailBtn = new QPushButton("Continue", m_sweepPage);
    m_continueAfterFailBtn->setVisible(false);
    m_abortBtn = new QPushButton("ABORT", m_sweepPage);
    setAbortButtonAbortMode();
    row->addWidget(m_tuneBtn);
    row->addWidget(m_skipBtn);
    row->addWidget(m_continueAfterFailBtn);
    row->addStretch(1);
    row->addWidget(m_abortBtn);
    layout->addLayout(row);

    connect(m_tuneBtn,  &QPushButton::clicked, this, &AtuPreTuneDialog::onTuneClicked);
    connect(m_skipBtn,  &QPushButton::clicked, this, &AtuPreTuneDialog::onSkipClicked);
    connect(m_abortBtn, &QPushButton::clicked, this, &AtuPreTuneDialog::onAbortClicked);
    connect(m_continueAfterFailBtn, &QPushButton::clicked,
            this, &AtuPreTuneDialog::onContinueClicked);

    m_pages->addWidget(m_sweepPage);
}

void AtuPreTuneDialog::populateLicenseClassRow()
{
    if (!m_licenseClassRow || !m_licenseClassCombo) return;
    m_licenseClassCombo->blockSignals(true);
    m_licenseClassCombo->clear();

    if (!m_bandPlan || m_bandPlan->licenseClasses().isEmpty()) {
        // Plan has no class structure — keep the row hidden so the feature
        // is inert and the operator's stored class is preserved untouched
        // for when they switch back to a class-aware plan. (#2649 #8)
        m_licenseClassRow->setVisible(false);
        m_licenseClassCombo->blockSignals(false);
        return;
    }

    // Always-present "no filter" entry — safety floor: if we can't prove
    // the operator's class authorizes a segment, leave them unfiltered and
    // let the disclaimer + TX interlock backstop. (#2649 #8)
    m_licenseClassCombo->addItem("All classes (no filter)", QString());

    const auto classes = m_bandPlan->licenseClasses();
    for (auto it = classes.begin(); it != classes.end(); ++it) {
        m_licenseClassCombo->addItem(
            QString("%1 (%2)").arg(it.value(), it.key()), it.key());
    }

    // Default to the operator's stored class when it exists in this plan;
    // otherwise default to "no filter" rather than auto-picking — keeps
    // the safety floor honest across plan switches.
    const QString stored =
        AppSettings::instance().value(kLicenseClassKey).toString();
    int idx = stored.isEmpty() ? 0 : m_licenseClassCombo->findData(stored);
    if (idx < 0) idx = 0;
    m_licenseClassCombo->setCurrentIndex(idx);

    m_licenseClassRow->setVisible(true);
    m_licenseClassCombo->blockSignals(false);
}

QString AtuPreTuneDialog::selectedLicenseClass() const
{
    if (!m_licenseClassRow || !m_licenseClassRow->isVisible()) return QString();
    if (!m_licenseClassCombo) return QString();
    return m_licenseClassCombo->currentData().toString();
}

void AtuPreTuneDialog::populateBands()
{
    for (auto& row : m_bands) {
        delete row.check;
        delete row.info;
    }
    m_bands.clear();

    QString planName = m_bandPlan ? m_bandPlan->activePlanName() : QString();
    m_planNameLabel->setText(
        QString("Using band plan: <b>%1</b> &mdash; change in View &gt; Band Plan")
        .arg(planName.isEmpty() ? "(none)" : planName));

    if (!m_bandPlan) return;
    const auto& segments = m_bandPlan->segments();
    const QString allowedClass = selectedLicenseClass();

    for (const auto& spec : kBandSpecs) {
        BandRow row;
        row.name = spec.name;
        row.segmentKhz = spec.segmentKhz;

        // Walk segments per contiguous region — discrete-channel bands
        // (US 60m) get one center per legal channel instead of stepping
        // across illegal gaps. (#2647)  Display low/high still show the
        // envelope so the operator sees the band's overall coverage.
        row.centers = computeCentersForBand(*m_bandPlan,
                                            spec.searchLowMhz,
                                            spec.searchHighMhz,
                                            row.segmentKhz,
                                            allowedClass);
        if (row.centers.isEmpty()) continue;  // no coverage in active plan
        double lo = 1e9;
        double hi = -1.0;
        for (const auto& seg : segments) {
            const double mid = (seg.lowMhz + seg.highMhz) / 2.0;
            if (mid >= spec.searchLowMhz && mid <= spec.searchHighMhz) {
                lo = std::min(lo, seg.lowMhz);
                hi = std::max(hi, seg.highMhz);
            }
        }
        row.lowMhz = lo;
        row.highMhz = hi;
        row.points = row.centers.size();

        auto* lineWidget = new QWidget(m_bandsContainer);
        auto* lineLayout = new QHBoxLayout(lineWidget);
        lineLayout->setContentsMargins(0, 0, 0, 0);
        lineLayout->setSpacing(8);
        row.check = new QCheckBox(row.name, lineWidget);
        row.check->setChecked(true);
        row.check->setStyleSheet("color: #c8d8e8; font-size: 11px; font-weight: bold;");
        row.check->setMinimumWidth(54);
        lineLayout->addWidget(row.check);

        const int estSecs = row.points * kSecondsPerPointEstimate;
        const QString infoText =
            QString("%1 - %2 MHz  &middot;  %3 kHz seg  &middot;  %4 pt  &middot;  ~%5:%6")
            .arg(row.lowMhz, 0, 'f', 3)
            .arg(row.highMhz, 0, 'f', 3)
            .arg(row.segmentKhz)
            .arg(row.points)
            .arg(estSecs / 60)
            .arg(estSecs % 60, 2, 10, QChar('0'));
        row.info = new QLabel(infoText, lineWidget);
        row.info->setTextFormat(Qt::RichText);
        row.info->setStyleSheet("color: #8aa8c0; font-size: 10px;");
        lineLayout->addWidget(row.info, 1);
        m_bandsLayout->addWidget(lineWidget);

        m_bands.append(row);
    }
    m_bandsLayout->addStretch(1);
}

QVector<double> AtuPreTuneDialog::centersForBand(const BandRow& row) const
{
    // Precomputed in populateBands() so discrete-channel walks match the
    // count shown in the UI exactly. (#2647)
    return row.centers;
}

void AtuPreTuneDialog::onStartClicked()
{
    if (!m_radio) {
        reject();
        return;
    }
    m_txSliceId = m_radio->activeTxSliceNum();
    if (m_txSliceId < 0) {
        m_sweepStatus->setText("No active TX slice — cannot start sweep.");
        m_sweepProgress->clear();
        m_sweepResult->clear();
        m_tuneBtn->setVisible(false);
        m_skipBtn->setVisible(false);
        m_continueAfterFailBtn->setVisible(false);
        setAbortButtonCloseMode();
        m_pages->setCurrentWidget(m_sweepPage);
        return;
    }
    SliceModel* txSlice = m_radio->slice(m_txSliceId);
    m_originalSliceFreqMhz = txSlice ? txSlice->frequency() : 0.0;

    // Capture the TX slice's panadapter view so it can be restored when
    // the sweep ends — pan is zoomed out to full-band view per band. (#2624)
    m_originalPanId.clear();
    m_originalPanCenterMhz = 0.0;
    m_originalPanBandwidthMhz = 0.0;
    if (txSlice) {
        m_originalPanId = txSlice->panId();
        if (auto* pan = m_radio->panadapter(m_originalPanId)) {
            m_originalPanCenterMhz = pan->centerMhz();
            m_originalPanBandwidthMhz = pan->bandwidthMhz();
        }
    }

    m_mode = static_cast<Mode>(m_modeCombo->currentData().toInt());

    m_points.clear();
    for (const auto& row : m_bands) {
        if (!row.check || !row.check->isChecked()) continue;
        const auto centers = centersForBand(row);
        const int total = centers.size();
        int i = 0;
        for (double f : centers) {
            ++i;
            Point p;
            p.bandName = row.name;
            p.freqMhz  = f;
            p.indexInBand = i;
            p.totalInBand = total;
            p.bandLowMhz  = row.lowMhz;
            p.bandHighMhz = row.highMhz;
            m_points.append(p);
        }
    }

    if (m_points.isEmpty()) {
        m_sweepStatus->setText("No bands selected — nothing to do.");
        m_pages->setCurrentWidget(m_sweepPage);
        m_tuneBtn->setVisible(false);
        m_skipBtn->setVisible(false);
        m_continueAfterFailBtn->setVisible(false);
        setAbortButtonCloseMode();
        return;
    }

    // Operator-attention guardrail (#2649 #4): warn before kicking off a
    // sweep large enough to keep the radio transmitting for tens of minutes
    // unattended. Catches the "select all bands on a wide regional plan"
    // footgun (e.g. ~129 points × 15 s ≈ 32 min of TX on R1 with everything
    // checked).
    if (m_points.size() > kMaxPointsSoftCap) {
        const int estSecs = m_points.size() * kSecondsPerPointEstimate;
        const auto reply = QMessageBox::warning(this,
            "Large Pre-Tune Sweep",
            QString("Selected bands total %1 points "
                    "(estimated %2 min %3 s of intermittent TX).\n\n"
                    "Continue?")
                .arg(m_points.size())
                .arg(estSecs / 60)
                .arg(estSecs % 60, 2, 10, QChar('0')),
            QMessageBox::Ok | QMessageBox::Cancel, QMessageBox::Cancel);
        if (reply != QMessageBox::Ok) return;
    }

    // Equipment guardrail (#2649 #5): warn — never refuse — if tune power
    // is high when entering Auto mode. Some amps need higher drive (per
    // Principle XIII the operator outranks the agent); the gate exists to
    // force a deliberate choice before unattended TX.
    if (m_mode == Mode::Auto && m_radio) {
        const int tunePower = m_radio->transmitModel().tunePower();
        if (tunePower > kAutoModeTunePowerWarnW) {
            const auto reply = QMessageBox::warning(this,
                "High Tune Power",
                QString("Tune power is %1 W — higher than the recommended "
                        "%2 W ceiling for unattended Auto mode.\n\n"
                        "Continue?")
                    .arg(tunePower).arg(kAutoModeTunePowerWarnW),
                QMessageBox::Ok | QMessageBox::Cancel, QMessageBox::Cancel);
            if (reply != QMessageBox::Ok) return;
        }
    }

    m_currentIndex = -1;
    m_successCount = 0;
    m_skipCount = 0;
    m_failCount = 0;
    m_consecutiveFailBypass = 0;
    m_sweepActive = true;

    m_pages->setCurrentWidget(m_sweepPage);
    setAbortButtonAbortMode();
    beginNextPoint();
}

void AtuPreTuneDialog::closeEvent(QCloseEvent* ev)
{
    // If the dialog is closed via the window manager mid-sweep, clean up
    // the same way Abort does so the radio isn't left transmitting and the
    // slice gets restored. (#2624)
    if (m_sweepActive) {
        m_settleTimer->stop();
        m_timeoutTimer->stop();
        m_waitingForAtu = false;
        if (m_radio)
            m_radio->transmitModel().atuBypass();
        restoreOriginalFrequency();
        m_sweepActive = false;
    }
    QDialog::closeEvent(ev);
}

void AtuPreTuneDialog::beginNextPoint()
{
    m_continueAfterFailBtn->setVisible(false);
    m_currentIndex++;
    if (m_currentIndex >= m_points.size()) {
        finishSweep();
        return;
    }
    const Point& p = m_points[m_currentIndex];

    // On first point of each band, zoom the panadapter out to the full-band
    // view so the operator sees the whole band being swept rather than the
    // slice-local zoom from before the sweep started. (#2624)
    //
    // Mirror MainWindow::applyPanRangeRequest's optimistic-update pattern:
    // push center+bandwidth together onto the PanadapterModel BEFORE sending
    // the radio command so SpectrumWidget reprojects both FFT and waterfall
    // in one shot.  Skipping the optimistic update produced the same
    // FFT-changes-but-waterfall-doesn't bug the canonical path was written
    // to avoid.
    const bool firstOfBand = (m_currentIndex == 0)
        || (m_points[m_currentIndex - 1].bandName != p.bandName);
    if (firstOfBand && !m_originalPanId.isEmpty() && p.bandHighMhz > p.bandLowMhz) {
        const double center = (p.bandLowMhz + p.bandHighMhz) / 2.0;
        const double width  = (p.bandHighMhz - p.bandLowMhz) * 1.10;
        const QString centerStr = QString::number(center, 'f', 6);
        const QString widthStr  = QString::number(width,  'f', 6);
        if (auto* pan = m_radio->panadapter(m_originalPanId)) {
            pan->applyPanStatus({{"center", centerStr},
                                 {"bandwidth", widthStr}});
        }
        m_radio->sendCommand(
            QString("display pan set %1 center=%2 bandwidth=%3")
                .arg(m_originalPanId, centerStr, widthStr));
    }

    // Move slice to target. SliceModel::setFrequency uses autopan=0 — no recenter.
    if (SliceModel* s = m_radio->slice(m_txSliceId))
        s->setFrequency(p.freqMhz);

    const double freqKhz = p.freqMhz * 1000.0;
    m_sweepStatus->setText(
        QString("Ready to tune: %1 kHz (%2, point %3/%4)")
            .arg(freqKhz, 0, 'f', 1)
            .arg(p.bandName)
            .arg(p.indexInBand)
            .arg(p.totalInBand));
    m_sweepProgress->setText(
        QString("Overall: %1 / %2  (%3 ok, %4 skipped, %5 failed)")
            .arg(m_currentIndex + 1).arg(m_points.size())
            .arg(m_successCount).arg(m_skipCount).arg(m_failCount));
    m_sweepResult->clear();

    if (m_mode == Mode::Step) {
        m_tuneBtn->setVisible(true);
        m_skipBtn->setVisible(true);
        setStepControlsEnabled(true);
    } else {
        m_tuneBtn->setVisible(false);
        m_skipBtn->setVisible(false);
        requestTuneNow();
    }
}

void AtuPreTuneDialog::onTuneClicked()
{
    if (m_currentIndex < 0 || m_currentIndex >= m_points.size()) return;
    setStepControlsEnabled(false);
    requestTuneNow();
}

void AtuPreTuneDialog::requestTuneNow()
{
    if (!m_radio) return;
    m_sweepResult->setText("Tuning...");
    // Slice is already on target (set in beginNextPoint). Wait 300 ms for
    // the slice to settle before issuing atu start. (#2624)
    m_settleTimer->start();
}

void AtuPreTuneDialog::onSkipClicked()
{
    if (m_currentIndex < 0) return;
    m_skipCount++;
    beginNextPoint();
}

void AtuPreTuneDialog::onAbortClicked()
{
    m_settleTimer->stop();
    m_timeoutTimer->stop();
    m_waitingForAtu = false;

    if (m_sweepActive && m_radio)
        m_radio->transmitModel().atuBypass();
    if (m_sweepActive)
        restoreOriginalFrequency();
    m_sweepActive = false;
    accept();
}

void AtuPreTuneDialog::onContinueClicked()
{
    m_continueAfterFailBtn->setVisible(false);
    beginNextPoint();
}

void AtuPreTuneDialog::onPerPointTimeout()
{
    if (!m_waitingForAtu) return;
    m_waitingForAtu = false;
    m_swrTracking = false;
    m_failCount++;
    m_consecutiveFailBypass = 0;
    m_sweepResult->setText(
        QString("No ATU status within %1 s — ATU may be stuck. Aborting.")
            .arg(kPerPointTimeoutMs / 1000));
    if (m_radio) m_radio->transmitModel().atuBypass();
    QApplication::beep();  // terminal-fail cue for operators not watching (#2649 #7)
    finishSweep(" Aborted: per-point timeout.");
}

void AtuPreTuneDialog::onAtuStateChanged()
{
    if (!m_waitingForAtu || !m_radio) return;
    const ATUStatus s = m_radio->transmitModel().atuStatus();

    const bool success     = (s == ATUStatus::Successful || s == ATUStatus::OK);
    const bool bypass      = (s == ATUStatus::Bypass);
    const bool failBypass  = (s == ATUStatus::FailBypass);
    const bool fail        = (s == ATUStatus::Fail);
    const bool aborted     = (s == ATUStatus::Aborted);
    if (!(success || bypass || failBypass || fail || aborted)) return;

    m_waitingForAtu = false;
    m_swrTracking = false;
    m_timeoutTimer->stop();

    const QString swrTag = (m_tuneLastSwr > 0.0f)
        ? QString("  SWR %1:1").arg(m_tuneLastSwr, 0, 'f', 2)
        : QString();

    if (success || bypass) {
        // TUNE_BYPASS after IN_PROGRESS means the ATU completed its cycle
        // and decided no inductors were needed — with MEM on the radio
        // still writes a memory entry, so it counts as a successful pre-tune.
        m_successCount++;
        m_consecutiveFailBypass = 0;
        m_sweepResult->setText(
            (bypass ? "Tune OK (bypass)." : "Tune OK.") + swrTag);
        beginNextPoint();
        return;
    }

    if (failBypass) {
        m_failCount++;
        m_consecutiveFailBypass++;
        if (m_consecutiveFailBypass >= kMaxConsecutiveFailBypass) {
            m_sweepResult->setText(
                QString("Tune failed (fail-bypass) %1 times in a row — aborting.")
                    .arg(m_consecutiveFailBypass));
            QApplication::beep();  // terminal-fail cue (#2649 #7)
            finishSweep(QString(" Aborted: %1 consecutive fail-bypass.")
                            .arg(m_consecutiveFailBypass));
            return;
        }
        showFailControls(/*failBypass=*/true);
        return;
    }

    // fail / aborted
    m_failCount++;
    m_consecutiveFailBypass = 0;
    showFailControls(/*failBypass=*/false);
}

void AtuPreTuneDialog::showFailControls(bool failBypass)
{
    const QString swrTag = (m_tuneLastSwr > 0.0f)
        ? QString("  SWR %1:1").arg(m_tuneLastSwr, 0, 'f', 2)
        : QString();
    m_sweepResult->setText((failBypass
        ? "Tune failed and ATU bypassed. Continue or Abort."
        : "Tune failed. Continue or Abort.") + swrTag);
    m_tuneBtn->setVisible(false);
    m_skipBtn->setVisible(false);
    m_continueAfterFailBtn->setVisible(true);
    m_continueAfterFailBtn->setEnabled(true);
}

void AtuPreTuneDialog::setStepControlsEnabled(bool enabled)
{
    if (m_tuneBtn) m_tuneBtn->setEnabled(enabled);
    if (m_skipBtn) m_skipBtn->setEnabled(enabled);
}

void AtuPreTuneDialog::setAbortButtonAbortMode()
{
    if (!m_abortBtn) return;
    m_abortBtn->setText("ABORT");
    m_abortBtn->setStyleSheet(
        "QPushButton { background: #802020; border: 1px solid #c03030; "
        "color: #fff; padding: 4px 12px; font-weight: bold; }"
        "QPushButton:hover { background: #903030; }");
}

void AtuPreTuneDialog::setAbortButtonCloseMode()
{
    if (!m_abortBtn) return;
    m_abortBtn->setText("Close");
    m_abortBtn->setStyleSheet(QString());
}

void AtuPreTuneDialog::finishSweep(const QString& summaryExtra)
{
    m_settleTimer->stop();
    m_timeoutTimer->stop();
    m_waitingForAtu = false;
    m_sweepActive = false;

    restoreOriginalFrequency();

    m_tuneBtn->setVisible(false);
    m_skipBtn->setVisible(false);
    m_continueAfterFailBtn->setVisible(false);
    setAbortButtonCloseMode();

    m_sweepStatus->setText("Sweep complete.");
    m_sweepResult->setText(
        QString("%1 points tuned successfully, %2 skipped, %3 failed.%4")
            .arg(m_successCount).arg(m_skipCount).arg(m_failCount)
            .arg(summaryExtra));
    m_sweepProgress->clear();
}

void AtuPreTuneDialog::restoreOriginalFrequency()
{
    if (!m_radio || m_txSliceId < 0) return;
    if (m_originalSliceFreqMhz <= 0.0) return;
    if (SliceModel* s = m_radio->slice(m_txSliceId))
        s->setFrequency(m_originalSliceFreqMhz);

    // Restore the panadapter zoom captured at sweep start — same
    // optimistic-update pattern used for band transitions.
    if (!m_originalPanId.isEmpty() && m_originalPanBandwidthMhz > 0.0) {
        const QString centerStr = QString::number(m_originalPanCenterMhz, 'f', 6);
        const QString widthStr  = QString::number(m_originalPanBandwidthMhz, 'f', 6);
        if (auto* pan = m_radio->panadapter(m_originalPanId)) {
            pan->applyPanStatus({{"center", centerStr},
                                 {"bandwidth", widthStr}});
        }
        m_radio->sendCommand(
            QString("display pan set %1 center=%2 bandwidth=%3")
                .arg(m_originalPanId, centerStr, widthStr));
    }
}

} // namespace AetherSDR
