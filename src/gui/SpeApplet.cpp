#include "SpeApplet.h"
#include "AmpAppletStyles.h"
#include "HGauge.h"
#include "core/ThemeManager.h"
#include "core/TxKeyingMarker.h"
#include "MeterSmoother.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QLabel>

namespace AetherSDR {

namespace {

// 5 evenly spaced ticks across [0, max] — same convention as AcomApplet's
// model-scaled power gauge.
QVector<HGauge::Tick> evenTicks(float max)
{
    QVector<HGauge::Tick> ticks;
    for (float frac : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
        const int v = static_cast<int>(max * frac);
        ticks.append({static_cast<float>(v), QString::number(v)});
    }
    return ticks;
}

// Left-side label+value, fixed width so all gauge rows line up — matches
// AcomApplet/AmpApplet's makeValueLabel convention. Slightly wider than
// ACOM's 46px: the ATU row label ("ATU  1.2:1") is the longest in the family.
// Themed rather than raw-styled so the colour re-resolves on theme change
// (and stays off the hardcoded-colour ratchet).
QLabel* makeValueLabel(QWidget* parent)
{
    auto* lbl = new QLabel(parent);
    lbl->setFixedWidth(52);
    lbl->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    AetherSDR::ThemeManager::instance().applyStyleSheet(lbl,
        "QLabel { color: {{color.text.primary}}; font-size: 11px; font-weight: bold; }");
    return lbl;
}

QString pillText(AmpPillState state)
{
    switch (state) {
        case AmpPillState::OperateTx: return QStringLiteral("OPR · TX");
        case AmpPillState::OperateRx: return QStringLiteral("OPR · RX");
        case AmpPillState::Standby:   return QStringLiteral("STANDBY");
        default:                      return QStringLiteral("—");
    }
}

// The family's shared neutral button, plus SPE's disabled tint — most of
// this applet's keys disable while the amp is silent (updateCommandsEnabled),
// which the push-driven ACOM never needs.
QString neutralBtnStyle()
{
    return ampNeutralBtnStyle()
        + QStringLiteral("QPushButton:disabled { color: {{color.text.disabled}}; }");
}

}  // namespace

SpeApplet::SpeApplet(QWidget* parent)
    : QWidget(parent)
{
    theme::setContainer(this, QStringLiteral("applet/spe"));
    auto* vbox = new QVBoxLayout(this);
    vbox->setContentsMargins(4, 2, 4, 2);
    vbox->setSpacing(2);

    // ── Header: source (connection status) + model + status pill (mode) ───
    auto& theme = AetherSDR::ThemeManager::instance();
    m_sourceLabel = new QLabel("● —", this);
    theme.applyStyleSheet(m_sourceLabel,
        "QLabel { color: {{color.accent}}; font-size: 9px; }");
    m_modelLabel = new QLabel(this);
    theme.applyStyleSheet(m_modelLabel,
        "QLabel { color: {{color.text.secondary}}; font-size: 9px; font-weight: bold; }");
    m_statusPill = new QLabel(pillText(AmpPillState::Neutral), this);
    theme.applyStyleSheet(m_statusPill, ampPillStyle(AmpPillState::Neutral));
    m_statusPill->setAlignment(Qt::AlignCenter);
    auto* headerRow = new QHBoxLayout;
    headerRow->addWidget(m_sourceLabel);
    headerRow->addSpacing(8);
    headerRow->addWidget(m_modelLabel);
    headerRow->addStretch();
    headerRow->addWidget(m_statusPill);
    vbox->addLayout(headerRow);

    // ── PWR row ───────────────────────────────────────────────────────────
    m_pwrLabel = makeValueLabel(this);
    m_pwrLabel->setText("PWR");
    m_pwrGauge = new HGauge(0.0f, 1600.0f, 1500.0f, "", "",
        evenTicks(1600.0f), this, 1450.0f);
    m_pwrGauge->setBallistics({0.030f, 0.800f});
    m_pwrGauge->setAccessibleName(tr("Output power"));
    auto* pwrRow = new QHBoxLayout;
    pwrRow->setSpacing(4);
    pwrRow->addWidget(m_pwrLabel);
    pwrRow->addWidget(m_pwrGauge, 1);
    vbox->addLayout(pwrRow);

    // ── SWR (antenna) row ─────────────────────────────────────────────────
    m_swrAntLabel = makeValueLabel(this);
    m_swrAntLabel->setText("SWR");
    m_swrAntGauge = new HGauge(1.0f, 3.0f, 2.5f, "", "",
        {{1.0f, "1"}, {1.5f, "1.5"}, {2.0f, "2"}, {2.5f, "2.5"}, {3.0f, "3"}},
        this, 2.0f);
    m_swrAntGauge->setAccessibleName(tr("Antenna SWR"));
    auto* swrAntRow = new QHBoxLayout;
    swrAntRow->setSpacing(4);
    swrAntRow->addWidget(m_swrAntLabel);
    swrAntRow->addWidget(m_swrAntGauge, 1);
    vbox->addLayout(swrAntRow);

    // ── SWR (ATU input) row ───────────────────────────────────────────────
    m_swrAtuLabel = makeValueLabel(this);
    m_swrAtuLabel->setText("ATU");
    m_swrAtuGauge = new HGauge(1.0f, 3.0f, 2.5f, "", "",
        {{1.0f, "1"}, {1.5f, "1.5"}, {2.0f, "2"}, {2.5f, "2.5"}, {3.0f, "3"}},
        this, 2.0f);
    m_swrAtuGauge->setAccessibleName(tr("SWR seen before the ATU"));
    auto* swrAtuRow = new QHBoxLayout;
    swrAtuRow->setSpacing(4);
    swrAtuRow->addWidget(m_swrAtuLabel);
    swrAtuRow->addWidget(m_swrAtuGauge, 1);
    vbox->addLayout(swrAtuRow);

    vbox->addSpacing(4);

    // ── Info grid: temp / V / I, then band / antenna / input·level ─────────
    static const char* kTelStyle = "QLabel { color: {{color.text.primary}}; font-size: 10px; }";

    m_tempLabel = new QLabel("TEMP  —", this);
    theme.applyStyleSheet(m_tempLabel, kTelStyle);
    // The amplifier reports temperature in whichever unit its own display is
    // configured for, without indicating which on the wire (spec §5) — so
    // the readout carries a bare degree sign and no C/F toggle.
    m_tempLabel->setToolTip(tr("Heatsink temperature, in the unit the amplifier's"
                               " own display is configured for"));
    m_voltLabel = new QLabel("V  — V", this);
    theme.applyStyleSheet(m_voltLabel, kTelStyle);
    m_currLabel = new QLabel("I  — A", this);
    theme.applyStyleSheet(m_currLabel, kTelStyle);
    m_bandLabel = new QLabel(this);
    theme.applyStyleSheet(m_bandLabel, kTelStyle);
    m_bandLabel->hide();
    m_antLabel = new QLabel(this);
    theme.applyStyleSheet(m_antLabel, kTelStyle);
    m_antLabel->hide();
    m_inputLabel = new QLabel(this);
    theme.applyStyleSheet(m_inputLabel, kTelStyle);
    m_inputLabel->hide();

    auto* infoGrid = new QGridLayout;
    infoGrid->setHorizontalSpacing(12);
    infoGrid->setVerticalSpacing(2);
    infoGrid->addWidget(m_tempLabel,  0, 0);
    infoGrid->addWidget(m_voltLabel,  0, 1);
    infoGrid->addWidget(m_currLabel,  0, 2);
    infoGrid->addWidget(m_bandLabel,  1, 0);
    infoGrid->addWidget(m_antLabel,   1, 1);
    infoGrid->addWidget(m_inputLabel, 1, 2);
    vbox->addLayout(infoGrid);

    vbox->addSpacing(4);

    // ── Fault banner (own row, full width, only shown when active) ─────────
    m_faultLabel = new QLabel(this);
    m_faultLabel->setWordWrap(true);
    theme.applyStyleSheet(m_faultLabel,
        "QLabel { color: {{color.accent.danger}}; font-size: 10px; font-weight: bold; }");
    m_faultLabel->hide();
    vbox->addWidget(m_faultLabel);

    // ── Button rows: OPER/STBY · power level · TUNE · OFF, then INPUT/ANT
    //    and the drive-power arrows. Every button is a literal front-panel
    //    keystroke.
    auto makeKeyBtn = [this, &theme](const QString& text) {
        auto* btn = new QPushButton(text, this);
        btn->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
        theme.applyStyleSheet(btn, neutralBtnStyle());
        return btn;
    };

    auto* btnRow1 = new QHBoxLayout;
    btnRow1->setSpacing(6);
    // ON is a hardware pulse, not a keystroke — deliberately styled apart
    // and kept enabled while the amp is silent (that is its whole purpose;
    // see updateCommandsEnabled).
    m_onBtn = new QPushButton("ON", this);
    m_onBtn->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    // The family's operate-green (same pair as ACOM's engaged OPERATE) —
    // an "energize" affordance, not a new colour.
    theme.applyStyleSheet(m_onBtn, ampOperateActiveBtnStyle());
    m_onBtn->setToolTip(tr("Power the amplifier ON — pulses the serial control"
                           " lines (over the network this needs an"
                           " rfc2217-enabled ser2net port; see the Radio Setup"
                           " row's tooltip)."));
    connect(m_onBtn, &QPushButton::clicked, this, &SpeApplet::powerOnClicked);
    // "OPER"/"STBY" rather than the full words — the row has 5 buttons and
    // the long labels clip at the applet's default width (hardware-tested).
    m_operateBtn = makeKeyBtn("OPER");
    m_operateBtn->setToolTip(tr("Toggle STANDBY / OPERATE (front-panel OPERATE key)"));
    connect(m_operateBtn, &QPushButton::clicked, this, &SpeApplet::operateClicked);
    // Label shows the CURRENT level (LOW/MID/HIGH) once known — clicking
    // cycles to the next one, mirroring the amplifier's own POWER key.
    m_pwrLevelBtn = makeKeyBtn("PWR");
    m_pwrLevelBtn->setToolTip(tr("Output power level — click to cycle LOW / MID / HIGH"));
    connect(m_pwrLevelBtn, &QPushButton::clicked, this, &SpeApplet::powerLevelClicked);
    m_tuneBtn = makeKeyBtn("TUNE");
    markTxKeying(m_tuneBtn);  // ATU tune runs with RF drive — keys TX (#3646)
    m_tuneBtn->setToolTip(tr("Start ATU tuning (front-panel TUNE key)"));
    connect(m_tuneBtn, &QPushButton::clicked, this, &SpeApplet::tuneClicked);
    m_offBtn = makeKeyBtn("OFF");
    m_offBtn->setToolTip(tr("Switch the amplifier off. Use ON to power it back"
                            " up (over the network this needs an"
                            " rfc2217-enabled ser2net port)."));
    connect(m_offBtn, &QPushButton::clicked, this, &SpeApplet::offClicked);
    btnRow1->addStretch();
    btnRow1->addWidget(m_onBtn);
    btnRow1->addWidget(m_operateBtn);
    btnRow1->addWidget(m_pwrLevelBtn);
    btnRow1->addWidget(m_tuneBtn);
    btnRow1->addWidget(m_offBtn);
    vbox->addLayout(btnRow1);

    auto* btnRow2 = new QHBoxLayout;
    btnRow2->setSpacing(6);
    m_inputBtn = makeKeyBtn("INPUT");
    m_inputBtn->setToolTip(tr("Toggle input port 1 / 2"));
    connect(m_inputBtn, &QPushButton::clicked, this, &SpeApplet::inputClicked);
    m_antBtn = makeKeyBtn("ANT");
    m_antBtn->setToolTip(tr("Cycle the TX antenna for the current band"));
    connect(m_antBtn, &QPushButton::clicked, this, &SpeApplet::antennaClicked);
    // The Expert's arrow keys adjust the drive power the amplifier requests
    // from the radio over CAT — no band keys here; the amp follows the
    // radio's band on its own.
    m_driveDownBtn = makeKeyBtn("▼");
    // The glyph is the whole label, so without an explicit name a screen
    // reader announces the arrow character itself. Tooltips reach AT only as
    // help text on request (docs/a11y.md), so they don't stand in for a name.
    m_driveDownBtn->setAccessibleName(tr("Lower requested drive power"));
    m_driveDownBtn->setToolTip(tr("Lower the drive power the amplifier requests"
                                  " from the radio (front-panel arrow key)"));
    connect(m_driveDownBtn, &QPushButton::clicked, this, &SpeApplet::driveDownClicked);
    m_driveUpBtn = makeKeyBtn("▲");
    m_driveUpBtn->setAccessibleName(tr("Raise requested drive power"));
    m_driveUpBtn->setToolTip(tr("Raise the drive power the amplifier requests"
                                " from the radio (front-panel arrow key)"));
    connect(m_driveUpBtn, &QPushButton::clicked, this, &SpeApplet::driveUpClicked);
    btnRow2->addStretch();
    btnRow2->addWidget(m_inputBtn);
    btnRow2->addWidget(m_antBtn);
    btnRow2->addWidget(m_driveDownBtn);
    btnRow2->addWidget(m_driveUpBtn);
    vbox->addLayout(btnRow2);

    // Label text throttle — matches AmpApplet/AcomApplet's 10 Hz readout
    // convention.
    m_labelTimer.setInterval(kMeterReadoutUpdateMs);
    connect(&m_labelTimer, &QTimer::timeout, this, &SpeApplet::updateValueLabels);
    m_labelTimer.start();

    m_peakTimer = new QTimer(this);
    m_peakTimer->setSingleShot(true);
    m_peakTimer->setInterval(2500);
    connect(m_peakTimer, &QTimer::timeout, this, [this]() {
        m_peakFwd = 0.0f;
        m_pwrGauge->clearPeak();
    });

    setConnected(false);
}

void SpeApplet::setPowerRange(float nominalW, float warnW, float maxW)
{
    // Called on every status frame with the level-derived scale — no-op on
    // repeats so only an actual LOW/MID/HIGH (or model) change repaints.
    // All three thresholds are compared: warnW is derived from nominalW
    // today, but a guard that silently ignores one of its inputs is a trap
    // for whoever changes that derivation.
    if (nominalW == m_rangeNominal && warnW == m_rangeWarn && maxW == m_rangeMax)
        return;
    m_rangeNominal = nominalW;
    m_rangeWarn = warnW;
    m_rangeMax = maxW;
    m_pwrGauge->setRange(0.0f, maxW, nominalW, evenTicks(maxW), warnW);
}

void SpeApplet::setModelName(const QString& displayName)
{
    m_modelLabel->setText(displayName);
}

void SpeApplet::setForwardPower(float watts)
{
    m_fwdWatts = watts;
    m_pwrGauge->setValue(watts);
    if (watts > m_peakFwd) {
        m_peakFwd = watts;
        m_pwrGauge->setPeakValue(watts);
        m_peakTimer->start();
    }
}

void SpeApplet::setSwrAnt(float swr)
{
    m_swrAntVal = swr;
    // Without forward drive SWR is undefined (the amp reports 0.00 in RX,
    // which is below the gauge's 1.0 floor anyway) — hold the needle at 1.0,
    // same gate as the readout label.
    m_swrAntGauge->setValue(m_fwdWatts >= 1.0f ? swr : 1.0f);
}

void SpeApplet::setSwrAtu(float swr)
{
    m_swrAtuVal = swr;
    m_swrAtuGauge->setValue(m_fwdWatts >= 1.0f ? swr : 1.0f);
}

void SpeApplet::setSupplyVoltage(float volts)
{
    m_supplyVolts = volts;
    m_diagDirty = true;
}

void SpeApplet::setSupplyCurrent(float amps)
{
    m_supplyAmps = amps;
    m_diagDirty = true;
}

void SpeApplet::setTemps(int upper, int lower, int combiner, bool hasCombiner)
{
    m_pendingTempText = hasCombiner
        ? QStringLiteral("TEMP  %1° %2° %3°").arg(upper).arg(lower).arg(combiner)
        : QStringLiteral("TEMP  %1°").arg(upper);
    m_tempDirty = true;
}

void SpeApplet::setBand(const QString& band)
{
    m_pendingBand = band;
    m_bandDirty = true;
}

void SpeApplet::setAntenna(int antenna, QChar atuState)
{
    QString text = QStringLiteral("ANT  %1").arg(antenna);
    if (atuState == u'a')
        text += QStringLiteral(" · ATU");
    else if (atuState == u'b')
        text += QStringLiteral(" · BYP");
    else if (atuState == u't')
        text += QStringLiteral(" · TUN");
    m_pendingAntText = text;
    m_antDirty = true;
}

void SpeApplet::setInputPort(int input)
{
    // Composed with the power level into one grid cell by updateValueLabels().
    m_inputPort = input;
    m_inputDirty = true;
}

void SpeApplet::setPowerLevel(const QString& levelName)
{
    m_levelName = levelName;
    m_inputDirty = true;
    // The button doubles as the level indicator (reference-app behavior).
    m_pwrLevelBtn->setText(levelName.isEmpty() ? QStringLiteral("PWR") : levelName);
}

void SpeApplet::setMode(bool operate, bool transmitting)
{
    m_operate = operate;
    m_transmitting = transmitting;
    applyModePill();
}

void SpeApplet::applyModePill()
{
    AmpPillState state = AmpPillState::Neutral;
    if (m_connected && m_responding)
        state = !m_operate ? AmpPillState::Standby
              : (m_transmitting ? AmpPillState::OperateTx : AmpPillState::OperateRx);
    // This runs on every status frame, i.e. at the 10 Hz poll rate, and both
    // setStyleSheet() and ThemeManager::applyStyleSheet() re-resolve and
    // re-polish unconditionally — neither has a no-op guard. pillText() is
    // 1:1 with the state, so the pill's own text is a sufficient one.
    const QString pill = pillText(state);
    if (m_statusPill->text() == pill)
        return;
    m_statusPill->setText(pill);

    auto& theme = AetherSDR::ThemeManager::instance();
    theme.applyStyleSheet(m_statusPill, ampPillStyle(state));
    const bool operateActive = (state == AmpPillState::OperateRx || state == AmpPillState::OperateTx);
    // Short labels — OPERATE/STANDBY clip at default applet width.
    m_operateBtn->setText(operateActive ? QStringLiteral("STBY") : QStringLiteral("OPER"));
    theme.applyStyleSheet(m_operateBtn,
        operateActive ? ampOperateActiveBtnStyle() : neutralBtnStyle());
}

void SpeApplet::setFaultText(const QString& text)
{
    if (text.isEmpty()) {
        m_faultLabel->hide();
        m_faultLabel->clear();
        return;
    }
    m_faultLabel->setText(text);
    m_faultLabel->show();
}

void SpeApplet::setSource(const QString& text)
{
    m_sourceLabel->setText(QStringLiteral("● %1").arg(text));
}

void SpeApplet::setResponding(bool responding)
{
    m_responding = responding;
    // Blank the readings, not just the buttons. Over ser2net — the topology
    // this integration is built around — the TCP link outlives the amplifier
    // being switched off, so this is the ONLY signal that arrives: without
    // the reset the panel keeps rendering the last poll's supply voltage,
    // heatsink temperature, band/antenna/level and alarm banner as though
    // they were live, which is worse than showing nothing. The pill going
    // neutral and the keys greying out are too quiet to carry that alone.
    if (!responding)
        clearTelemetry();
    updateCommandsEnabled();
    applyModePill();
}

void SpeApplet::updateCommandsEnabled()
{
    // Commands only make sense while the amplifier is actually answering —
    // a live ser2net socket with the amp switched off would otherwise offer
    // buttons that silently do nothing.
    const bool enabled = m_connected && m_responding;
    for (auto* btn : {m_operateBtn, m_pwrLevelBtn, m_tuneBtn, m_offBtn,
                      m_inputBtn, m_antBtn, m_driveDownBtn, m_driveUpBtn})
        btn->setEnabled(enabled);
    // ON stays available whenever the transport is up — a silent amp is
    // exactly when it's needed.
    m_onBtn->setEnabled(m_connected);
}

void SpeApplet::clearTelemetry()
{
    setFaultText(QString());
    m_bandLabel->hide();
    m_antLabel->hide();
    m_inputLabel->hide();
    m_bandDirty = m_antDirty = m_inputDirty = false;
    m_inputPort = 0;
    m_levelName.clear();
    m_pwrLevelBtn->setText(QStringLiteral("PWR"));
    m_supplyVolts = 0.0f;
    m_supplyAmps = 0.0f;
    m_diagDirty = false;
    m_voltLabel->setText("V  — V");
    m_currLabel->setText("I  — A");
    m_tempDirty = false;
    m_tempLabel->setText("TEMP  —");
    m_operate = false;
    m_transmitting = false;
    m_fwdWatts = 0.0f;
    m_swrAntVal = 1.0f;
    m_swrAtuVal = 1.0f;
    // Clear the forward-power peak hold too — a stale peak would
    // otherwise survive into the next session (same fix AcomApplet
    // carries in its setConnected).
    m_peakFwd = 0.0f;
    if (m_peakTimer) m_peakTimer->stop();
    m_pwrGauge->setValueImmediate(0.0f);
    m_pwrGauge->clearPeak();
    m_swrAntGauge->setValueImmediate(1.0f);
    m_swrAtuGauge->setValueImmediate(1.0f);
    updateValueLabels();
}

void SpeApplet::setConnected(bool connected)
{
    m_connected = connected;
    if (!connected) {
        m_responding = false;
        // Transport-level identity goes too, which the responding path keeps:
        // a silent amplifier is still reached over a known link, and is still
        // the model it identified as.
        m_sourceLabel->setText(QStringLiteral("● —"));
        m_modelLabel->clear();
        clearTelemetry();
    }
    updateCommandsEnabled();
    applyModePill();
}

void SpeApplet::updateValueLabels()
{
    m_pwrLabel->setText(m_fwdWatts >= 1.0f
        ? QStringLiteral("PWR  %1").arg(static_cast<int>(m_fwdWatts))
        : QStringLiteral("PWR"));
    m_swrAntLabel->setText(m_fwdWatts >= 1.0f
        ? QStringLiteral("SWR  %1:1").arg(m_swrAntVal, 0, 'f', 1)
        : QStringLiteral("SWR"));
    m_swrAtuLabel->setText(m_fwdWatts >= 1.0f
        ? QStringLiteral("ATU  %1:1").arg(m_swrAtuVal, 0, 'f', 1)
        : QStringLiteral("ATU"));

    if (m_tempDirty) {
        m_tempDirty = false;
        m_tempLabel->setText(m_pendingTempText);
    }
    if (m_diagDirty) {
        m_diagDirty = false;
        // Text readouts, not gauges — the protocol defines no nominal/max
        // scale for supply voltage/current to size an axis against.
        m_voltLabel->setText(QStringLiteral("V  %1V").arg(m_supplyVolts, 0, 'f', 1));
        m_currLabel->setText(QStringLiteral("I  %1A").arg(m_supplyAmps, 0, 'f', 1));
    }
    if (m_bandDirty) {
        m_bandDirty = false;
        m_bandLabel->setText(QStringLiteral("BAND  %1").arg(m_pendingBand));
        m_bandLabel->show();
    }
    if (m_antDirty) {
        m_antDirty = false;
        m_antLabel->setText(m_pendingAntText);
        m_antLabel->show();
    }
    if (m_inputDirty) {
        m_inputDirty = false;
        QString text = QStringLiteral("IN  %1").arg(m_inputPort > 0
            ? QString::number(m_inputPort) : QStringLiteral("—"));
        if (!m_levelName.isEmpty())
            text += QStringLiteral(" · %1").arg(m_levelName);
        m_inputLabel->setText(text);
        m_inputLabel->show();
    }
}

}  // namespace AetherSDR
