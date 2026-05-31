#include "AmpApplet.h"
#include "HGauge.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include "core/ThemeManager.h"
#include "MeterSmoother.h"

namespace AetherSDR {

namespace {

// Left-side label that shows the field name + live value ("PWR 1148").
// Fixed width so all three gauge rows line up.
QLabel* makeValueLabel(QWidget* parent)
{
    auto* lbl = new QLabel(parent);
    lbl->setFixedWidth(72);
    lbl->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    lbl->setStyleSheet("QLabel { color: #c8d8e8; font-size: 11px; font-weight: bold; }");
    return lbl;
}

} // namespace

AmpApplet::AmpApplet(QWidget* parent)
    : QWidget(parent)
{
    theme::setContainer(this, QStringLiteral("applet/amp"));
    auto* vbox = new QVBoxLayout(this);
    vbox->setContentsMargins(4, 2, 4, 2);
    vbox->setSpacing(2);

    // ── PWR row ──────────────────────────────────────────────────────────────
    m_pwrLabel = makeValueLabel(this);
    m_pwrLabel->setText("PWR");
    m_fwdGauge = new HGauge(0.0f, 2000.0f, 1500.0f, "", "",
        {{0, "0"}, {500, "500"}, {1000, "1K"}, {1500, "1.5K"}, {2000, "2K"}},
        this, 1000.0f);
    // Slow release: bar rises quickly on RF bursts but decays over ~800 ms
    // so brief transmissions remain visible — matches S-meter peak-hold feel.
    m_fwdGauge->setBallistics({0.030f, 0.800f});
    auto* pwrRow = new QHBoxLayout;
    pwrRow->setSpacing(4);
    pwrRow->addWidget(m_pwrLabel);
    pwrRow->addWidget(m_fwdGauge, 1);
    vbox->addLayout(pwrRow);

    // ── SWR row ──────────────────────────────────────────────────────────────
    m_swrLabel = makeValueLabel(this);
    m_swrLabel->setText("SWR");
    m_swrGauge = new HGauge(1.0f, 3.0f, 2.5f, "", "",
        {{1.0f, "1"}, {1.5f, "1.5"}, {2.0f, "2"}, {2.5f, "2.5"}, {3.0f, "3"}},
        this, 2.0f);
    auto* swrRow = new QHBoxLayout;
    swrRow->setSpacing(4);
    swrRow->addWidget(m_swrLabel);
    swrRow->addWidget(m_swrGauge, 1);
    vbox->addLayout(swrRow);

    // ── Id row ───────────────────────────────────────────────────────────────
    m_idLabel = makeValueLabel(this);
    m_idLabel->setText("Id");
    m_idGauge = new HGauge(0.0f, 70.0f, 60.0f, "", "",
        {{0, "0"}, {10, "10"}, {20, "20"}, {30, "30"}, {40, "40"}, {50, "50"}, {60, "60"}, {70, "70"}},
        this, 50.0f);
    auto* idRow = new QHBoxLayout;
    idRow->setSpacing(4);
    idRow->addWidget(m_idLabel);
    idRow->addWidget(m_idGauge, 1);
    vbox->addLayout(idRow);

    vbox->addSpacing(4);

    // ── Bottom row: [temp / Vdd / Vac stacked] [OPERATE button] ─────────────
    //   Temp, drain voltage, and mains voltage sit in the empty space to the
    //   left of the OPERATE/STANDBY button.
    static const char* kTelStyle = "QLabel { color: #c8d8e8; font-size: 10px; }";

    m_tempLabel = new QLabel("— C", this);
    m_tempLabel->setStyleSheet(kTelStyle);

    m_vddLabel = new QLabel("Vdd  — V", this);
    m_vddLabel->setStyleSheet(kTelStyle);

    m_vacLabel = new QLabel("Vac  — V", this);
    m_vacLabel->setStyleSheet(kTelStyle);

    m_sourceLabel = new QLabel("● RADIO", this);
    m_sourceLabel->setStyleSheet("QLabel { color: #888888; font-size: 9px; }");

    auto* infoStack = new QVBoxLayout;
    infoStack->setSpacing(0);
    infoStack->setContentsMargins(0, 0, 0, 0);
    infoStack->addWidget(m_tempLabel);
    infoStack->addWidget(m_vddLabel);
    infoStack->addWidget(m_vacLabel);
    infoStack->addWidget(m_sourceLabel);

    auto* btnRow = new QHBoxLayout;
    btnRow->setSpacing(6);
    btnRow->addLayout(infoStack);
    btnRow->addStretch();
    m_operateBtn = new QPushButton("OPERATE");
    m_operateBtn->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    AetherSDR::ThemeManager::instance().applyStyleSheet(m_operateBtn,
        "QPushButton { background: {{color.background.2}}; border: 1px solid {{color.background.2}}; "
        "border-radius: 3px; color: {{color.text.primary}}; font-size: 10px; font-weight: bold; }"
        "QPushButton:hover { background: {{color.background.1}}; }");
    m_operateBtn->hide();
    connect(m_operateBtn, &QPushButton::clicked, this, [this]() {
        bool isOp = (m_operateBtn->text() == "OPERATE");
        emit operateToggled(!isOp);
    });
    btnRow->addWidget(m_operateBtn);
    vbox->addLayout(btnRow);

    // Label text throttle — update PWR/SWR/Id text at 10 Hz so the digits
    // don't flicker on every incoming meter packet.  Gauge fill still
    // animates at full rate via HGauge's own timer.
    m_labelTimer.setInterval(kMeterReadoutUpdateMs);
    connect(&m_labelTimer, &QTimer::timeout, this, &AmpApplet::updateValueLabels);
    m_labelTimer.start();

    // Peak hold: clear the white tick 2.5 s after the last new peak.
    m_peakTimer = new QTimer(this);
    m_peakTimer->setSingleShot(true);
    m_peakTimer->setInterval(2500);
    connect(m_peakTimer, &QTimer::timeout, this, [this]() {
        m_peakFwd = 0.0f;
        m_fwdGauge->clearPeak();
    });
}

void AmpApplet::setFwdPower(float watts)
{
    m_fwdWatts = watts;
    m_fwdGauge->setValue(watts);
    if (watts > m_peakFwd) {
        m_peakFwd = watts;
        m_fwdGauge->setPeakValue(watts);
        m_peakTimer->start();  // restart hold window on each new peak
    }
    // Label text is updated by the 100 ms timer (updateValueLabels).
}

void AmpApplet::setSwr(float swr)
{
    m_swrVal = swr;
    m_swrGauge->setValue(swr);
    // Label text is updated by the 100 ms timer (updateValueLabels).
}

void AmpApplet::setTemp(float degC)
{
    m_tempA = degC;
    updateTempLabel();
}

void AmpApplet::setTempB(float degC)
{
    m_tempB = degC;
    m_hasTempB = true;
    updateTempLabel();
}

void AmpApplet::updateTempLabel()
{
    if (m_hasTempB)
        m_tempLabel->setText(
            QStringLiteral("%1/%2 C")
                .arg(m_tempA, 0, 'f', 1)
                .arg(m_tempB, 0, 'f', 1));
    else
        m_tempLabel->setText(
            QStringLiteral("%1 C").arg(m_tempA, 0, 'f', 1));
}

void AmpApplet::setDrainCurrent(float amps)
{
    m_drainAmps = amps;
    m_idGauge->setValue(amps);
    // Label text is updated by the 100 ms timer (updateValueLabels).
}

void AmpApplet::updateValueLabels()
{
    // PWR: only show value when there is meaningful power
    if (m_fwdWatts >= 5.0f)
        m_pwrLabel->setText(QStringLiteral("PWR  %1").arg(static_cast<int>(m_fwdWatts)));
    else
        m_pwrLabel->setText("PWR");

    // SWR: only show value when power is present (SWR is unmeasurable at idle)
    if (m_fwdWatts >= 5.0f)
        m_swrLabel->setText(QStringLiteral("SWR  %1:1").arg(m_swrVal, 0, 'f', 1));
    else
        m_swrLabel->setText("SWR");

    // Id: only show value when current is flowing
    if (m_drainAmps >= 0.5f)
        m_idLabel->setText(QStringLiteral("Id    %1").arg(static_cast<int>(m_drainAmps)));
    else
        m_idLabel->setText("Id");
}

void AmpApplet::setDrainVoltage(float volts)
{
    if (!m_directConnected) return;
    m_vddLabel->setText(QStringLiteral("Vdd  %1 V").arg(volts, 0, 'f', 1));
}

void AmpApplet::setMainsVoltage(int volts)
{
    if (!m_directConnected) return;
    m_mainsVolts = volts;
    m_vacLabel->setText(QStringLiteral("Vac  %1 V").arg(volts));
}

void AmpApplet::setState(const QString& state)
{
    // PGXL states: IDLE/OPERATE/TRANSMIT_* = operating (green), else standby (grey)
    bool operating = (state == "IDLE" || state == "OPERATE"
                      || state.startsWith("TRANSMIT"));
    if (operating) {
        m_operateBtn->setText("OPERATE");
        AetherSDR::ThemeManager::instance().applyStyleSheet(m_operateBtn,
            "QPushButton { background: #006030; border: 1px solid #008040; "
            "border-radius: 3px; color: {{color.text.primary}}; font-size: 10px; font-weight: bold; }"
            "QPushButton:hover { background: #007040; }");
    } else {
        m_operateBtn->setText("STANDBY");
        AetherSDR::ThemeManager::instance().applyStyleSheet(m_operateBtn,
            "QPushButton { background: {{color.background.2}}; border: 1px solid {{color.background.2}}; "
            "border-radius: 3px; color: {{color.text.primary}}; font-size: 10px; font-weight: bold; }"
            "QPushButton:hover { background: {{color.background.1}}; }");
    }
    m_operateBtn->show();
}

void AmpApplet::setDirectConnected(bool direct)
{
    m_directConnected = direct;
    if (direct) {
        m_sourceLabel->setText("● DIRECT");
        m_sourceLabel->setStyleSheet("QLabel { color: #00b4d8; font-size: 9px; }");
        m_vddLabel->setStyleSheet("QLabel { color: #c8d8e8; font-size: 10px; }");
        m_vacLabel->setStyleSheet("QLabel { color: #c8d8e8; font-size: 10px; }");
    } else {
        m_sourceLabel->setText("● RADIO");
        m_sourceLabel->setStyleSheet("QLabel { color: #888888; font-size: 9px; }");
        // Vdd and Vac are not proxied by the radio — gray out and clear stale values.
        m_vddLabel->setText("Vdd  — V");
        m_vddLabel->setStyleSheet("QLabel { color: #505050; font-size: 10px; }");
        m_vacLabel->setText("Vac  — V");
        m_vacLabel->setStyleSheet("QLabel { color: #505050; font-size: 10px; }");
    }
}

void AmpApplet::setMeff(const QString& /*meff*/)
{
    // MEffA not displayed in the main applet view (matches SSDR layout).
}

} // namespace AetherSDR
