#include "TxApplet.h"
#include "AtuPreTuneDialog.h"
#include "GuardedSlider.h"
#include "ComboStyle.h"
#include "HGauge.h"
#include "models/RadioModel.h"
#include "models/TransmitModel.h"
#include "models/TunerModel.h"

#include <QAction>
#include <QPushButton>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QSlider>
#include <QComboBox>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSignalBlocker>
#include <cmath>

namespace AetherSDR {



// ── Styled indicator label (small coloured-dot + text) ──────────────────────

static QLabel* makeIndicator(const QString& text)
{
    auto* lbl = new QLabel(text);
    lbl->setStyleSheet("QLabel { color: #405060; font-size: 9px; font-weight: bold; }");
    lbl->setAlignment(Qt::AlignCenter);
    return lbl;
}

static void setIndicatorActive(QLabel* lbl, bool active, const QColor& color = QColor(0x00, 0xc0, 0x40))
{
    if (active) {
        lbl->setStyleSheet(
            QString("QLabel { color: %1; font-size: 9px; font-weight: bold; }").arg(color.name()));
    } else {
        lbl->setStyleSheet("QLabel { color: #405060; font-size: 9px; font-weight: bold; }");
    }
}

// ── Compact slider row: "Label:  [slider] value" ────────────────────────────

static constexpr const char* kSliderStyle =
    "QSlider::groove:horizontal { height: 4px; background: #203040; border-radius: 2px; }"
    "QSlider::handle:horizontal { width: 10px; height: 10px; margin: -3px 0;"
    "background: #00b4d8; border-radius: 5px; }";

static QString wattsText(int value)
{
    return QStringLiteral("%1 W").arg(value);
}

// ── TxApplet ────────────────────────────────────────────────────────────────

TxApplet::TxApplet(QWidget* parent)
    : QWidget(parent)
{
    hide();
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    buildUI();

    // PEP peak-hold ballistics — 50 ms tick advances decay after the 2 s
    // hold window, matching SmartSDR's peak-hold bar and the RX S-meter
    // peak-hold pattern in SMeterWidget. (#2561)
    m_peakTick.setInterval(50);
    connect(&m_peakTick, &QTimer::timeout, this, [this]() {
        if (!m_peakHoldRunning) {
            m_peakTick.stop();
            return;
        }
        const qint64 elapsedMs = m_peakHoldTimer.elapsed();
        constexpr qint64 kHoldMs = 2000;
        if (elapsedMs <= kHoldMs)
            return;
        // After the hold, decay the peak toward the current smoothed value
        // at a rate scaled to the gauge full-scale so the visual feel
        // (~2.5 s from peak to floor) stays consistent across barefoot
        // (120 W gauge) and Aurora 500 W exciter (600 W gauge).  Set by
        // setPowerScale; defaults to the barefoot 48 W/s.
        const float decaySecs = static_cast<float>(elapsedMs - kHoldMs) / 1000.0f;
        const float decayed = m_peakDecayStart - m_peakDecayWattsPerSec * decaySecs;
        if (decayed <= m_smoothedPower) {
            m_peakPower = m_smoothedPower;
            m_peakHoldRunning = false;
            m_peakTick.stop();
        } else {
            m_peakPower = decayed;
        }
        static_cast<HGauge*>(m_fwdGauge)->setPeakValue(m_peakPower);
    });
}

void TxApplet::buildUI()
{
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    // Body with margins
    auto* body = new QWidget;
    auto* vbox = new QVBoxLayout(body);
    vbox->setContentsMargins(4, 2, 8, 2);
    vbox->setSpacing(2);

    // ── Forward Power gauge (0–120 W, red > 100 W) ─────────────────────────
    m_fwdGauge = new HGauge(0.0f, 120.0f, 100.0f, "RF Pwr", "W",
        {{0, "0"}, {40, "40"}, {80, "80"}, {100, "100"}, {120, "120"}},
        this, 80.0f);
    m_fwdGauge->setAccessibleName("Forward power gauge");
    m_fwdGauge->setAccessibleDescription("RF forward power in watts");
    vbox->addWidget(m_fwdGauge);

    // ── SWR gauge (1.0–3.0, red > 2.5) ─────────────────────────────────────
    m_swrGauge = new HGauge(1.0f, 3.0f, 2.5f, "SWR", "",
        {{1.0f, "1"}, {1.5f, "1.5"}, {2.5f, "2.5"}, {3.0f, "3"}},
        this, 2.0f);
    m_swrGauge->setAccessibleName("SWR gauge");
    m_swrGauge->setAccessibleDescription("Standing wave ratio");
    vbox->addWidget(m_swrGauge);

    // ── RF Power slider ─────────────────────────────────────────────────────
    {
        auto* row = new QHBoxLayout;
        row->setSpacing(4);
        auto* label = new QLabel("RF Power:");
        label->setStyleSheet("QLabel { color: #8aa8c0; font-size: 10px; }");
        label->setFixedWidth(62);
        row->addWidget(label);

        m_rfPowerSlider = new GuardedSlider(Qt::Horizontal);
        m_rfPowerSlider->setRange(0, 100);
        static_cast<GuardedSlider*>(m_rfPowerSlider)->setDragValueFormatter(wattsText);
        m_rfPowerSlider->setStyleSheet(kSliderStyle);
        m_rfPowerSlider->setAccessibleName("RF power");
        m_rfPowerSlider->setAccessibleDescription("Transmit RF power level, 0 to 100 watts");
        row->addWidget(m_rfPowerSlider, 1);

        m_rfPowerLabel = new QLabel("100");
        m_rfPowerLabel->setStyleSheet("QLabel { color: #c8d8e8; font-size: 10px; }");
        m_rfPowerLabel->setFixedWidth(30);
        m_rfPowerLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        row->addWidget(m_rfPowerLabel);
        vbox->addLayout(row);
    }

    // ── Tune Power slider ───────────────────────────────────────────────────
    {
        auto* row = new QHBoxLayout;
        row->setSpacing(4);
        auto* label = new QLabel("Tune Pwr:");
        label->setStyleSheet("QLabel { color: #8aa8c0; font-size: 10px; }");
        label->setFixedWidth(62);
        row->addWidget(label);

        m_tunePowerSlider = new GuardedSlider(Qt::Horizontal);
        m_tunePowerSlider->setRange(0, 100);
        static_cast<GuardedSlider*>(m_tunePowerSlider)->setDragValueFormatter(wattsText);
        m_tunePowerSlider->setStyleSheet(kSliderStyle);
        m_tunePowerSlider->setAccessibleName("Tune power");
        m_tunePowerSlider->setAccessibleDescription("Tune carrier power level, 0 to 100 watts");
        row->addWidget(m_tunePowerSlider, 1);

        m_tunePowerLabel = new QLabel("10");
        m_tunePowerLabel->setStyleSheet("QLabel { color: #c8d8e8; font-size: 10px; }");
        m_tunePowerLabel->setFixedWidth(30);
        m_tunePowerLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        row->addWidget(m_tunePowerLabel);
        vbox->addLayout(row);
    }

    // ── Profile dropdown + Success/Byp/Mem indicators (same row) ────────────
    {
        auto* row = new QHBoxLayout;
        row->setSpacing(4);

        m_profileCombo = new GuardedComboBox;
        AetherSDR::applyComboStyle(m_profileCombo);
        m_profileCombo->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        m_profileCombo->setAccessibleName("TX profile");
        m_profileCombo->setAccessibleDescription("Select transmit profile");
        row->addWidget(m_profileCombo, 1);  // 1 out of 2 = 50%

        m_successInd = makeIndicator("Success");
        m_bypInd     = makeIndicator("Byp");
        m_memInd     = makeIndicator("Mem");
        auto* indRow = new QHBoxLayout;
        indRow->setSpacing(0);
        indRow->addWidget(m_successInd);
        indRow->addWidget(m_bypInd);
        indRow->addWidget(m_memInd);
        row->addLayout(indRow, 1);  // 1 out of 2 = 50%
        vbox->addLayout(row);
    }

    // ── TUNE / MOX / ATU / MEM buttons ──────────────────────────────────────
    {
        auto* row = new QHBoxLayout;
        row->setSpacing(2);

        const char* btnStyle =
            "QPushButton { background: #1a3a5a; border: 1px solid #205070; "
            "border-radius: 3px; color: #c8d8e8; font-size: 10px; font-weight: bold; "
            "padding: 2px; }"
            "QPushButton:hover { background: #204060; }"
            "QPushButton:disabled { background-color: #1a1a2a; color: #556070; "
            "border: 1px solid #2a3040; }";

        m_tuneBtn = new QPushButton("TUNE");
        m_tuneBtn->setStyleSheet(btnStyle);
        m_tuneBtn->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        m_tuneBtn->setFixedHeight(22);
        m_tuneBtn->setAccessibleName("Tune");
        m_tuneBtn->setAccessibleDescription("Start or stop tune carrier");
        // Right-click picks the carrier shape (Mono Tone / Two Tone) for
        // the next tune cycle.  Transient one-shot — no AppSettings write.
        m_tuneBtn->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(m_tuneBtn, &QPushButton::customContextMenuRequested,
                this, &TxApplet::showTuneContextMenu);
        row->addWidget(m_tuneBtn);

        m_moxBtn = new QPushButton("MOX");
        m_moxBtn->setStyleSheet(btnStyle);
        m_moxBtn->setCheckable(true);
        m_moxBtn->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        m_moxBtn->setFixedHeight(22);
        m_moxBtn->setAccessibleName("MOX transmit");
        m_moxBtn->setAccessibleDescription("Toggle manual transmit on or off");
        row->addWidget(m_moxBtn);

        m_atuBtn = new QPushButton("ATU");
        m_atuBtn->setStyleSheet(btnStyle);
        m_atuBtn->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        m_atuBtn->setFixedHeight(22);
        m_atuBtn->setAccessibleName("ATU tune");
        m_atuBtn->setAccessibleDescription("Start automatic antenna tuner");
        // Right-click on the ATU button exposes the pre-tune sweep and
        // Clear ATU Memories actions. Matches SmartSDR Windows's hidden
        // right-click menu on this button. (#2624)
        m_atuBtn->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(m_atuBtn, &QPushButton::customContextMenuRequested,
                this, &TxApplet::showAtuContextMenu);
        row->addWidget(m_atuBtn);

        m_memBtn = new QPushButton("MEM");
        m_memBtn->setStyleSheet(btnStyle);
        m_memBtn->setCheckable(true);
        m_memBtn->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        m_memBtn->setFixedHeight(22);
        m_memBtn->setAccessibleName("ATU memories");
        m_memBtn->setAccessibleDescription("Toggle ATU memory recall");
        row->addWidget(m_memBtn);

        vbox->addLayout(row);
    }

    // ── APD button + Active / Cal / Avail indicators ────────────────────────
    {
        auto* row = new QHBoxLayout;
        row->setSpacing(4);

        m_apdBtn = new QPushButton("APD");
        m_apdBtn->setCheckable(true);
        m_apdBtn->setFixedHeight(22);
        m_apdBtn->setAccessibleName("APD pre-distortion");
        m_apdBtn->setAccessibleDescription("Toggle adaptive pre-distortion");
        m_apdBtn->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        m_apdBtn->setStyleSheet(
            "QPushButton { background: #1a3a5a; border: 1px solid #205070; "
            "border-radius: 3px; color: #c8d8e8; font-size: 10px; font-weight: bold; }"
            "QPushButton:checked { background: #006030; border: 1px solid #008040; color: #fff; }"
            "QPushButton:hover { background: #204060; }");
        row->addWidget(m_apdBtn, 2);  // 40%

        // Inset container for ATU status words (styled like RIT/XIT readout)
        auto* inset = new QWidget;
        inset->setFixedHeight(22);
        inset->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        inset->setObjectName("atuInset");
        inset->setStyleSheet(
            "#atuInset { background: #0a0a18; border: 1px solid #1e2e3e; border-radius: 3px; }"
            "#atuInset QLabel { border: none; background: transparent; }");
        auto* insetLayout = new QHBoxLayout(inset);
        insetLayout->setContentsMargins(4, 0, 4, 0);
        insetLayout->setSpacing(2);

        m_activeInd = makeIndicator("Active");
        m_calInd    = makeIndicator("Cal");
        m_availInd  = makeIndicator("Avail");
        // Larger font for status words inside the inset
        const QString indStyle =
            "QLabel { color: #405060; font-size: 11px; font-weight: bold; background: transparent; }";
        m_activeInd->setStyleSheet(indStyle);
        m_calInd->setStyleSheet(indStyle);
        m_availInd->setStyleSheet(indStyle);

        insetLayout->addWidget(m_activeInd);
        insetLayout->addWidget(m_calInd);
        insetLayout->addWidget(m_availInd);

        row->addWidget(inset, 3);  // 60%

        m_apdRow = new QWidget;
        m_apdRow->setLayout(row);
        vbox->addWidget(m_apdRow);
    }

    outer->addWidget(body);

    // ── Slider → label + command connections ────────────────────────────────
    connect(m_rfPowerSlider, &QSlider::valueChanged, this, [this](int v) {
        m_rfPowerLabel->setText(QString::number(v));
        if (!m_updatingFromModel && m_model)
            m_model->setRfPower(v);
    });
    connect(m_tunePowerSlider, &QSlider::valueChanged, this, [this](int v) {
        m_tunePowerLabel->setText(QString::number(v));
        if (!m_updatingFromModel && m_model)
            m_model->setTunePower(v);
    });

    // Profile dropdown → load command
    connect(m_profileCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int /*idx*/) {
        if (!m_updatingFromModel && m_model) {
            const QString name = m_profileCombo->currentText();
            if (!name.isEmpty())
                m_model->loadProfile(name);
        }
    });

    // TUNE button — toggle tune
    connect(m_tuneBtn, &QPushButton::clicked, this, [this]() {
        if (!m_model) return;
        if (m_model->isTuning())
            m_model->stopTune();
        else
            m_model->startTune();
    });

    // MOX button — toggle transmit.  Routes through requestPttOn/Off so
    // the Quindar tone coordinator (#2262) can run intro/outro tones on
    // phone modes when enabled.  Falls through to setMox() when Quindar
    // is disabled or the active TX slice isn't on a phone mode.
    connect(m_moxBtn, &QPushButton::toggled, this, [this](bool on) {
        if (m_updatingFromModel || !m_model) return;
        if (on)
            m_model->requestPttOn(TransmitModel::PttSource::Mox);
        else
            m_model->requestPttOff(TransmitModel::PttSource::Mox);
    });

    // ATU button — toggle between tune and bypass.
    //   First click on a fresh slice → start tune cycle
    //   Second click while tuner holds a Successful/OK match at the SAME
    //     freq we tuned at → switch to bypass
    //   Click after any freq change → start a fresh tune cycle even if the
    //     prior status was Successful/OK
    // Mirrors SmartSDR's per-frequency toggle. (#1993)
    connect(m_atuBtn, &QPushButton::clicked, this, [this]() {
        if (!m_model) return;
        const auto status = m_model->atuStatus();
        const bool tuned = (status == ATUStatus::Successful || status == ATUStatus::OK);
        const double curFreq = m_model->transmitFreq();
        const bool sameFreq = (m_atuTunedFreqMhz > 0.0
                               && std::abs(curFreq - m_atuTunedFreqMhz) < 1e-6);
        if (tuned && sameFreq)
            m_model->atuBypass();
        else
            m_model->atuStart();
    });

    // MEM button — toggle ATU memories
    connect(m_memBtn, &QPushButton::toggled, this, [this](bool on) {
        if (!m_updatingFromModel && m_model)
            m_model->setAtuMemories(on);
    });
}

void TxApplet::setTransmitModel(TransmitModel* model)
{
    if (m_model == model) return;
    m_model = model;
    if (!m_model) return;

    // Transmit state changes → update sliders, tune button
    connect(m_model, &TransmitModel::stateChanged, this, &TxApplet::syncFromModel);

    // Tune state → red button
    connect(m_model, &TransmitModel::tuneChanged, this, [this](bool tuning) {
        if (tuning) {
            m_tuneBtn->setStyleSheet(
                "QPushButton { background: #cc2222; border: 1px solid #ff4444; "
                "border-radius: 3px; color: #ffffff; font-size: 10px; font-weight: bold; "
                "padding: 2px; }");
            m_tuneBtn->setText("TUNING...");
        } else {
            m_tuneBtn->setStyleSheet(
                "QPushButton { background: #1a3a5a; border: 1px solid #205070; "
                "border-radius: 3px; color: #c8d8e8; font-size: 10px; font-weight: bold; "
                "padding: 2px; }"
                "QPushButton:hover { background: #204060; }");
            m_tuneBtn->setText("TUNE");
        }
    });

    // MOX / transmit state → red button
    connect(m_model, &TransmitModel::moxChanged, this, [this](bool tx) {
        m_updatingFromModel = true;
        m_moxBtn->setChecked(tx);
        m_moxBtn->setStyleSheet(tx
            ? "QPushButton { background: #cc2222; border: 1px solid #ff4444; "
              "border-radius: 3px; color: #ffffff; font-size: 10px; font-weight: bold; "
              "padding: 2px; }"
            : "QPushButton { background: #1a3a5a; border: 1px solid #205070; "
              "border-radius: 3px; color: #c8d8e8; font-size: 10px; font-weight: bold; "
              "padding: 2px; }"
              "QPushButton:hover { background: #204060; }");
        m_updatingFromModel = false;
    });

    // ATU state changes → indicators
    connect(m_model, &TransmitModel::atuStateChanged, this, &TxApplet::syncAtuIndicators);

    // APD button + indicators
    connect(m_apdBtn, &QPushButton::toggled, this, [this](bool on) {
        if (!m_updatingFromModel && m_model)
            m_model->setApdEnabled(on);
    });
    connect(m_model, &TransmitModel::apdStateChanged, this, [this] {
        m_updatingFromModel = true;
        m_apdBtn->setChecked(m_model->apdEnabled());
        m_updatingFromModel = false;
        syncAtuIndicators();  // also refreshes APD indicators
    });

    // Profile list changes → populate combo
    connect(m_model, &TransmitModel::profileListChanged, this, [this]() {
        m_updatingFromModel = true;
        const QSignalBlocker blocker(m_profileCombo);
        m_profileCombo->clear();
        m_profileCombo->addItems(m_model->profileList());
        // Select current profile if known
        if (!m_model->activeProfile().isEmpty()) {
            int idx = m_profileCombo->findText(m_model->activeProfile());
            if (idx >= 0) m_profileCombo->setCurrentIndex(idx);
        }
        m_updatingFromModel = false;
    });

    syncFromModel();
    syncAtuIndicators();
}

void TxApplet::setTunerModel(TunerModel* tuner)
{
    if (!tuner) return;

    auto updateButtons = [this, tuner]() {
        // When TGXL is in Operate, disable only the internal ATU controls.
        // TUNE stays enabled — it sends a carrier through the TGXL for
        // power/SWR checks, matching SmartSDR behavior (#443).
        bool tgxlOperate = tuner->isPresent() && tuner->isOperate() && !tuner->isBypass();
        m_atuBtn->setEnabled(!tgxlOperate);
        m_memBtn->setEnabled(!tgxlOperate);
        QString tip = tgxlOperate ? "Disabled — TGXL is in OPERATE mode" : "";
        m_atuBtn->setToolTip(tip);
        m_memBtn->setToolTip(tip);
    };

    connect(tuner, &TunerModel::stateChanged, this, updateButtons);
    connect(tuner, &TunerModel::presenceChanged, this, updateButtons);
    updateButtons();
}

void TxApplet::syncFromModel()
{
    if (!m_model) return;

    m_updatingFromModel = true;

    if (m_rfPowerSlider->value() != m_model->rfPower())
        m_rfPowerSlider->setValue(m_model->rfPower());
    m_rfPowerLabel->setText(QString::number(m_model->rfPower()));

    if (m_tunePowerSlider->value() != m_model->tunePower())
        m_tunePowerSlider->setValue(m_model->tunePower());
    m_tunePowerLabel->setText(QString::number(m_model->tunePower()));

    // Active profile — update combo selection
    if (!m_model->activeProfile().isEmpty()) {
        int idx = m_profileCombo->findText(m_model->activeProfile());
        if (idx >= 0 && m_profileCombo->currentIndex() != idx) {
            const QSignalBlocker blocker(m_profileCombo);
            m_profileCombo->setCurrentIndex(idx);
        }
    }

    m_updatingFromModel = false;
}

void TxApplet::syncAtuIndicators()
{
    if (!m_model) return;

    const auto status = m_model->atuStatus();

    // Capture the freq the ATU just tuned at — gates the "second click ⇒ bypass"
    // path so only same-frequency follow-up clicks bypass. (#1993)
    if (status == ATUStatus::Successful || status == ATUStatus::OK)
        m_atuTunedFreqMhz = m_model->transmitFreq();
    else if (status == ATUStatus::Bypass || status == ATUStatus::ManualBypass) {
        // Bypass clears the tuned-freq pin so the next click starts a fresh tune.
        m_atuTunedFreqMhz = -1.0;
    }

    // Success — green when tune was successful
    setIndicatorActive(m_successInd,
        status == ATUStatus::Successful || status == ATUStatus::OK);

    // Byp — orange when in bypass
    setIndicatorActive(m_bypInd,
        status == ATUStatus::Bypass || status == ATUStatus::ManualBypass,
        QColor(0xd0, 0x90, 0x00));

    // Mem — green when using memory
    setIndicatorActive(m_memInd, m_model->usingMemory());

    // APD indicators — mutually exclusive states, all off when APD disabled
    // Progression: Cal (calibrating) → Avail (calibration ready) → Active (applied)
    const bool apdOn  = m_model->apdEnabled();
    const bool eqActv = m_model->apdEqualizerActive();
    const bool config = m_model->apdConfigurable();
    setIndicatorActive(m_activeInd, apdOn && eqActv);
    setIndicatorActive(m_availInd,  apdOn && !eqActv && config);
    setIndicatorActive(m_calInd,    apdOn && !eqActv && !config);

    // MEM button — sync checked state
    {
        m_updatingFromModel = true;
        const QSignalBlocker blocker(m_memBtn);
        m_memBtn->setChecked(m_model->memoriesEnabled());
        m_updatingFromModel = false;
    }
}

void TxApplet::updateMeters(float fwdPower, float swr)
{
    m_smoothedPower = fwdPower;
    static_cast<HGauge*>(m_fwdGauge)->setValue(fwdPower);
    static_cast<HGauge*>(m_swrGauge)->setValue(swr);
}

void TxApplet::updatePeakPower(float fwdPowerInstant)
{
    if (fwdPowerInstant > m_peakPower) {
        m_peakPower = fwdPowerInstant;
        m_peakDecayStart = fwdPowerInstant;
        m_peakHoldTimer.restart();
        m_peakHoldRunning = true;
        if (!m_peakTick.isActive())
            m_peakTick.start();
        static_cast<HGauge*>(m_fwdGauge)->setPeakValue(m_peakPower);
    }
}

void TxApplet::setTransmitting(bool tx)
{
    if (!tx) {
        // Drop the peak-hold tick to zero immediately on un-key so a held
        // PEP reading does not linger across overs. (#2561)
        m_peakPower = 0.0f;
        m_peakDecayStart = 0.0f;
        m_peakHoldRunning = false;
        m_peakTick.stop();
        static_cast<HGauge*>(m_fwdGauge)->setPeakValue(0.0f);
    }
}

void TxApplet::setRadioModel(RadioModel* radio)
{
    m_radioModel = radio;
}

void TxApplet::setBandPlanManager(BandPlanManager* bandPlan)
{
    m_bandPlanMgr = bandPlan;
}

void TxApplet::showAtuContextMenu(const QPoint& pos)
{
    QMenu menu(m_atuBtn);

    auto* preTune = menu.addAction(QString::fromUtf8("Pre-tune bands\xE2\x80\xA6"));
    const bool memOn = m_model && m_model->memoriesEnabled();
    preTune->setEnabled(memOn);
    if (!memOn)
        preTune->setToolTip("Enable MEM before running the pre-tune sweep.");
    connect(preTune, &QAction::triggered, this, &TxApplet::openPreTuneDialog);

    auto* clearMem = menu.addAction(QString::fromUtf8("Clear ATU memories\xE2\x80\xA6"));
    connect(clearMem, &QAction::triggered,
            this, &TxApplet::confirmAndClearAtuMemories);

    menu.exec(m_atuBtn->mapToGlobal(pos));
}

void TxApplet::showTuneContextMenu(const QPoint& pos)
{
    if (!m_model) return;

    QMenu menu(m_tuneBtn);

    // Reflect the radio's current tune_mode in the check marks so the user
    // can see what the next Tune press will do.  Selecting either entry is
    // a one-shot — the radio's tune_mode lives in volatile state and reverts
    // to single_tone on its own across power cycles; AetherSDR does not
    // persist the choice in AppSettings.
    const QString current = m_model->tuneMode();

    auto* mono = menu.addAction("Mono Tone");
    mono->setCheckable(true);
    mono->setChecked(current != QStringLiteral("two_tone"));
    mono->setToolTip("Next Tune press transmits a single carrier (normal Tune).");
    connect(mono, &QAction::triggered, this, [this]() {
        if (m_model)
            m_model->setTuneMode(QStringLiteral("single_tone"));
    });

    auto* two = menu.addAction("Two Tone");
    two->setCheckable(true);
    two->setChecked(current == QStringLiteral("two_tone"));
    two->setToolTip("Next Tune press transmits a two-tone test signal\n"
                    "(for IMD / linearity measurement).");
    connect(two, &QAction::triggered, this, [this]() {
        if (m_model)
            m_model->setTuneMode(QStringLiteral("two_tone"));
    });

    menu.exec(m_tuneBtn->mapToGlobal(pos));
}

void TxApplet::openPreTuneDialog()
{
    if (!m_radioModel || !m_bandPlanMgr || !m_model) return;
    if (!m_model->memoriesEnabled()) return;

    if (!m_preTuneDialog) {
        m_preTuneDialog = new AtuPreTuneDialog(m_radioModel, m_bandPlanMgr,
                                               this->window());
        connect(m_preTuneDialog, &QObject::destroyed,
                this, [this]() { m_preTuneDialog = nullptr; });
        m_preTuneDialog->setAttribute(Qt::WA_DeleteOnClose);
    }
    m_preTuneDialog->show();
    m_preTuneDialog->raise();
    m_preTuneDialog->activateWindow();
}

void TxApplet::confirmAndClearAtuMemories()
{
    if (!m_model) return;
    QMessageBox box(this->window());
    box.setWindowTitle("Clear ATU memories");
    box.setIcon(QMessageBox::Warning);
    box.setText("Clear the radio's entire ATU memory database?");
    box.setInformativeText(
        "This removes every stored ATU tune for every band on every antenna. "
        "The next transmission on any untuned frequency will require a live "
        "tune cycle.\n\n"
        "FlexLib has no per-band clear; this is an all-or-nothing operation.");
    auto* clearBtn = box.addButton("Clear all bands", QMessageBox::DestructiveRole);
    box.addButton("Cancel", QMessageBox::RejectRole);
    box.exec();
    if (box.clickedButton() == clearBtn)
        m_model->atuClearMemories();
}

void TxApplet::setPowerScale(int maxWatts, bool hasAmplifier)
{
    Q_UNUSED(hasAmplifier);
    // TX applet always shows exciter (barefoot) power.
    // Amplified output power is shown in the AMP applet.
    auto* gauge = static_cast<HGauge*>(m_fwdGauge);
    float gaugeFullScaleW = 0.0f;
    if (maxWatts > 100) {
        // Aurora (500 W): 0–600 W, red > 500 W
        gauge->setRange(0.0f, 600.0f, 500.0f,
            {{0, "0"}, {100, "100"}, {200, "200"}, {300, "300"},
             {400, "400"}, {500, "500"}, {600, "600"}});
        gaugeFullScaleW = 600.0f;
    } else {
        // Barefoot: 0–120 W, red > 100 W
        gauge->setRange(0.0f, 120.0f, 100.0f,
            {{0, "0"}, {40, "40"}, {80, "80"}, {100, "100"}, {120, "120"}});
        gaugeFullScaleW = 120.0f;
    }
    // Scale peak-hold decay to the gauge full-scale (~2.5 s from full to
    // zero) so the visual feel is the same whether the rig is barefoot
    // or an Aurora 500 W exciter. (#2561)
    m_peakDecayWattsPerSec = gaugeFullScaleW / 2.5f;
}

} // namespace AetherSDR
