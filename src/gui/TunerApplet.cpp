#include "TunerApplet.h"
#include "HGauge.h"
#include "models/TunerModel.h"
#include "models/MeterModel.h"

#include <QPushButton>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSignalBlocker>
#include <QTimer>

namespace MasterSDR {



// ── TunerApplet ─────────────────────────────────────────────────────────────

TunerApplet::TunerApplet(QWidget* parent)
    : QWidget(parent)
{
    hide();   // hidden by default until toggled on
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    // Post-tune capture timer: after tuning=0 arrives, keep capturing SWR
    // for 400ms so the final settled value from the TGXL has time to arrive.
    m_postTuneTimer = new QTimer(this);
    m_postTuneTimer->setSingleShot(true);
    m_postTuneTimer->setInterval(400);
    connect(m_postTuneTimer, &QTimer::timeout, this, [this]() {
        m_postTuneCapture = false;
        float result = (m_tuneSwr < 900.0f) ? m_tuneSwr : m_swr;
        m_tuneBtn->setText(QString("SWR %1").arg(result, 0, 'f', 2));
        QTimer::singleShot(2500, this, [this]() {
            m_tuneBtn->setText("TUNE");
        });
    });

    buildUI();
}

void TunerApplet::setAmplifierMode(bool hasAmp)
{
    setPowerScale(100, hasAmp);
}

void TunerApplet::setPowerScale(int maxWatts, bool hasAmplifier)
{
    auto* gauge = static_cast<HGauge*>(m_fwdGauge);
    if (hasAmplifier) {
        // PGXL: 0–2000 W, red > 1500 W
        gauge->setRange(0.0f, 2000.0f, 1500.0f,
            {{0, "0"}, {500, "500"}, {1500, "1.5k"}, {2000, "2k"}});
    } else if (maxWatts > 100) {
        // Aurora (500 W): 0–600 W, red > 500 W
        gauge->setRange(0.0f, 600.0f, 500.0f,
            {{0, "0"}, {100, "100"}, {200, "200"}, {300, "300"},
             {400, "400"}, {500, "500"}, {600, "600"}});
    } else {
        // Barefoot radio: 0–200 W, red > 125 W
        gauge->setRange(0.0f, 200.0f, 125.0f,
            {{0, "0"}, {50, "50"}, {100, "100"}, {150, "150"}, {200, "200"}});
    }
}

void TunerApplet::buildUI()
{
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    // Body with margins
    auto* body = new QWidget;
    auto* vbox = new QVBoxLayout(body);
    vbox->setContentsMargins(4, 2, 4, 2);
    vbox->setSpacing(2);

    // Forward Power gauge — default barefoot (0–200 W); switches to
    // 0–2000 W if a PGXL amplifier is detected via setAmplifierMode().
    m_fwdGauge = new HGauge(0.0f, 200.0f, 125.0f, "Fwd Pwr", "W",
        {{0, "0"}, {50, "50"}, {100, "100"}, {150, "150"}, {200, "200"}},
        this, 80.0f);
    vbox->addWidget(m_fwdGauge);

    // SWR gauge
    m_swrGauge = new HGauge(1.0f, 3.0f, 2.5f, "SWR", "",
        {{1.0f, "1"}, {1.5f, "1.5"}, {2.5f, "2.5"}, {3.0f, "3"}},
        this, 2.0f);
    vbox->addWidget(m_swrGauge);

    // Bottom section: relay bars (75% left) + buttons (25% right)
    auto* bottomRow = new QHBoxLayout;
    bottomRow->setSpacing(4);

    // Left column: relay bars
    auto* relayCol = new QVBoxLayout;
    relayCol->setSpacing(2);
    m_c1Bar = new RelayBar("C1");
    m_lBar  = new RelayBar("L");
    m_c2Bar = new RelayBar("C2");
    relayCol->addWidget(m_c1Bar);
    relayCol->addWidget(m_lBar);
    relayCol->addWidget(m_c2Bar);
    bottomRow->addLayout(relayCol, 7);  // stretch 7 (70%)

    // Right column: buttons
    auto* btnCol = new QVBoxLayout;
    btnCol->setSpacing(2);

    m_tuneBtn = new QPushButton("TUNE");
    m_tuneBtn->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    m_tuneBtn->setStyleSheet(
        "QPushButton { background: #1a3a5a; border: 1px solid #205070; "
        "border-radius: 3px; color: #c8d8e8; font-size: 10px; font-weight: bold; }"
        "QPushButton:hover { background: #204060; }");
    btnCol->addWidget(m_tuneBtn);

    m_operateBtn = new QPushButton("OPERATE");
    m_operateBtn->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    m_operateBtn->setStyleSheet(
        "QPushButton { background: #1a3a5a; border: 1px solid #205070; "
        "border-radius: 3px; color: #c8d8e8; font-size: 10px; font-weight: bold; }"
        "QPushButton:hover { background: #204060; }");
    btnCol->addWidget(m_operateBtn);

    bottomRow->addLayout(btnCol, 3);  // stretch 3 (30%)

    vbox->addLayout(bottomRow);

    // Antenna switch row (TGXL 3x1) — hidden until direct connection active
    {
        m_antContainer = new QWidget;
        m_antContainer->setVisible(false);
        auto* antRow = new QHBoxLayout(m_antContainer);
        antRow->setContentsMargins(0, 0, 0, 0);
        antRow->setSpacing(2);

        auto makeAntBtn = [](const QString& text) {
            auto* btn = new QPushButton(text);
            btn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            btn->setFixedHeight(22);
            btn->setStyleSheet(
                "QPushButton { background: #1a2a3a; border: 1px solid #205070; "
                "border-radius: 3px; color: #c8d8e8; font-size: 10px; font-weight: bold; }"
                "QPushButton:hover { background: #204060; }");
            return btn;
        };

        m_ant1Btn = makeAntBtn("ANT 1");
        m_ant2Btn = makeAntBtn("ANT 2");
        m_ant3Btn = makeAntBtn("ANT 3");
        antRow->addWidget(m_ant1Btn);
        antRow->addWidget(m_ant2Btn);
        antRow->addWidget(m_ant3Btn);

        connect(m_ant1Btn, &QPushButton::clicked, this, [this]() {
            if (m_model) m_model->setAntennaA(1);
        });
        connect(m_ant2Btn, &QPushButton::clicked, this, [this]() {
            if (m_model) m_model->setAntennaA(2);
        });
        connect(m_ant3Btn, &QPushButton::clicked, this, [this]() {
            if (m_model) m_model->setAntennaA(3);
        });

        vbox->addWidget(m_antContainer);
    }

    outer->addWidget(body);

    // TUNE button: send autotune command
    connect(m_tuneBtn, &QPushButton::clicked, this, [this]() {
        if (m_model) m_model->autoTune();
    });

    // Manual relay adjustment via mousewheel scroll (#469)
    connect(static_cast<RelayBar*>(m_c1Bar), &RelayBar::relayAdjusted, this,
            [this](int dir) { if (m_model) m_model->adjustRelay(0, dir); });
    connect(static_cast<RelayBar*>(m_lBar), &RelayBar::relayAdjusted, this,
            [this](int dir) { if (m_model) m_model->adjustRelay(1, dir); });
    connect(static_cast<RelayBar*>(m_c2Bar), &RelayBar::relayAdjusted, this,
            [this](int dir) { if (m_model) m_model->adjustRelay(2, dir); });

    // OPERATE button: cycle through OPERATE → BYPASS → STANDBY → OPERATE
    connect(m_operateBtn, &QPushButton::clicked, this,
            &TunerApplet::cycleOperateState);
}

void TunerApplet::setTunerModel(TunerModel* model)
{
    if (m_model == model) return;
    m_model = model;
    if (!m_model) return;

    // State changes → refresh UI
    connect(m_model, &TunerModel::stateChanged, this, &TunerApplet::syncFromModel);

    // Forward power and SWR from direct TGXL connection (#625)
    connect(m_model, &TunerModel::metersChanged,
            this, &TunerApplet::updateMeters);

    // Enable relay bar scrolling when direct TGXL connection is active (#469)
    auto updateScrollEnabled = [this]() {
        bool on = m_model && m_model->hasDirectConnection();
        static_cast<RelayBar*>(m_c1Bar)->setScrollEnabled(on);
        static_cast<RelayBar*>(m_lBar)->setScrollEnabled(on);
        static_cast<RelayBar*>(m_c2Bar)->setScrollEnabled(on);
    };
    connect(m_model, &TunerModel::directConnectionChanged, this, updateScrollEnabled);
    updateScrollEnabled();

    // Antenna switch: show buttons only when direct connection is active AND
    // the TGXL reports antA (models without a switch never send antA).
    auto updateAntVisible = [this]() {
        m_antContainer->setVisible(m_model->hasDirectConnection()
                                   && m_model->hasAntennaSwitch());
    };
    connect(m_model, &TunerModel::directConnectionChanged, this, updateAntVisible);
    connect(m_model, &TunerModel::antennaAChanged, this, [this, updateAntVisible](int antA) {
        updateAntVisible();
        updateAntennaButtons(antA);
    });
    updateAntVisible();
    updateAntennaButtons(m_model->antennaA());

    // Tuning state changes → red button + SWR result flash
    connect(m_model, &TunerModel::tuningChanged, this, [this](bool tuning) {
        if (tuning) {
            m_wasTuning = true;
            m_postTuneCapture = false;
            m_postTuneTimer->stop();
            m_tuneSwr = 999.0f;  // reset high so capture tracking works
            m_tuneBtn->setStyleSheet(
                "QPushButton { background: #cc2222; border: 1px solid #ff4444; "
                "border-radius: 3px; color: #ffffff; font-size: 10px; font-weight: bold; }");
            m_tuneBtn->setText("TUNING...");
        } else {
            // Restore normal style
            m_tuneBtn->setStyleSheet(
                "QPushButton { background: #1a3a5a; border: 1px solid #205070; "
                "border-radius: 3px; color: #c8d8e8; font-size: 10px; font-weight: bold; }"
                "QPushButton:hover { background: #204060; }");

            // Don't display result immediately — the final settled SWR from
            // the TGXL often arrives after tuning=0 via TCP.  Start a short
            // capture window so updateMeters() can grab the settled value.
            if (m_wasTuning) {
                m_wasTuning = false;
                m_postTuneCapture = true;
                m_tuneSwr = 999.0f;  // reset — we want the post-tune value, not the sweep
                m_tuneBtn->setText("TUNING...");
                m_postTuneTimer->start();
            } else {
                m_tuneBtn->setText("TUNE");
            }
        }
    });

    syncFromModel();
}

void TunerApplet::syncFromModel()
{
    if (!m_model) return;

    // Relay bars
    m_relayC1 = m_model->relayC1();
    m_relayL  = m_model->relayL();
    m_relayC2 = m_model->relayC2();
    static_cast<RelayBar*>(m_c1Bar)->setValue(m_relayC1);
    static_cast<RelayBar*>(m_lBar)->setValue(m_relayL);
    static_cast<RelayBar*>(m_c2Bar)->setValue(m_relayC2);

    // Operate/Bypass/Standby button — 3-state display
    // operate=1, bypass=0 → OPERATE (green)
    // operate=1, bypass=1 → BYPASS  (orange)
    // operate=0            → STANDBY (default)
    if (m_model->isOperate() && !m_model->isBypass()) {
        m_operateBtn->setText("OPERATE");
        m_operateBtn->setStyleSheet(
            "QPushButton { background: #006030; border: 1px solid #008040; "
            "border-radius: 3px; color: #ffffff; font-size: 10px; font-weight: bold; }"
            "QPushButton:hover { background: #007040; }");
    } else if (m_model->isOperate() && m_model->isBypass()) {
        m_operateBtn->setText("BYPASS");
        m_operateBtn->setStyleSheet(
            "QPushButton { background: #8a6000; border: 1px solid #a07000; "
            "border-radius: 3px; color: #ffffff; font-size: 10px; font-weight: bold; }"
            "QPushButton:hover { background: #9a7000; }");
    } else {
        m_operateBtn->setText("STANDBY");
        m_operateBtn->setStyleSheet(
            "QPushButton { background: #1a3a5a; border: 1px solid #205070; "
            "border-radius: 3px; color: #c8d8e8; font-size: 10px; font-weight: bold; }"
            "QPushButton:hover { background: #204060; }");
    }
}

void TunerApplet::cycleOperateState()
{
    if (!m_model) return;

    // Cycle: OPERATE → BYPASS → STANDBY → OPERATE
    if (m_model->isOperate() && !m_model->isBypass()) {
        // Currently OPERATE → go to BYPASS
        m_model->setBypass(true);
    } else if (m_model->isOperate() && m_model->isBypass()) {
        // Currently BYPASS → go to STANDBY
        m_model->setOperate(false);
    } else {
        // Currently STANDBY → go to OPERATE
        m_model->setBypass(false);
        m_model->setOperate(true);
    }
}

void TunerApplet::updateMeters(float fwdPower, float swr)
{
    m_fwdPower = fwdPower;
    m_swr = swr;
    static_cast<HGauge*>(m_fwdGauge)->setValue(fwdPower);
    static_cast<HGauge*>(m_swrGauge)->setValue(swr);

    // During the post-tune capture window, record the last non-idle SWR.
    // The TGXL reports the settled SWR shortly after tuning=0 arrives;
    // we take the last value > 1.01 (idle/no-RF reads ~1.00).
    if (m_postTuneCapture && swr > 1.01f) {
        m_tuneSwr = swr;
        m_tuneBtn->setText(QString("SWR %1").arg(swr, 0, 'f', 2));
    }
}

void TunerApplet::updateAntennaButtons(int antA)
{
    // antA is 0-indexed: 0=ANT1, 1=ANT2, 2=ANT3
    static constexpr const char* kDefault =
        "QPushButton { background: #1a2a3a; border: 1px solid #205070; "
        "border-radius: 3px; color: #c8d8e8; font-size: 10px; font-weight: bold; }"
        "QPushButton:hover { background: #204060; }";
    static constexpr const char* kActive =
        "QPushButton { background: #006030; border: 1px solid #008040; "
        "border-radius: 3px; color: #ffffff; font-size: 10px; font-weight: bold; }";

    m_ant1Btn->setStyleSheet(antA == 0 ? kActive : kDefault);
    m_ant2Btn->setStyleSheet(antA == 1 ? kActive : kDefault);
    m_ant3Btn->setStyleSheet(antA == 2 ? kActive : kDefault);
}

} // namespace MasterSDR

// RelayBar has Q_OBJECT in a header-only class — include MOC output here
#include "moc_HGauge.cpp"
