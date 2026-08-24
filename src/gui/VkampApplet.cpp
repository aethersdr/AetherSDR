#include "VkampApplet.h"
#include "HGauge.h"
#include "core/ThemeManager.h"
#include "MeterSmoother.h"

#include <QAccessible>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QStyle>
#include <QVBoxLayout>

namespace AetherSDR {

namespace {

// 5 evenly spaced ticks across [0, max] -- same convention as
// AcomApplet/AmpApplet's own gauge tick helper.
QVector<HGauge::Tick> evenTicks(float max)
{
    QVector<HGauge::Tick> ticks;
    for (float frac : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
        const int v = static_cast<int>(max * frac);
        ticks.append({static_cast<float>(v), QString::number(v)});
    }
    return ticks;
}

QLabel* makeValueLabel(QWidget* parent)
{
    auto* lbl = new QLabel(parent);
    // 72px, not ACOM's 46px -- VK3AMP's 2000W variant needs 4-digit
    // readouts (up to "2500" at meter full-scale), same width AmpApplet
    // (PGXL, same power range) already uses for the same reason.
    lbl->setFixedWidth(72);
    lbl->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    AetherSDR::ThemeManager::instance().applyStyleSheet(lbl,
        "QLabel { color: {{color.text.primary}}; font-size: 11px; font-weight: bold; }");
    return lbl;
}

// Builds the QSS for one named [vkState="..."] variant. Combined with
// neutralBtnStyle() into a single template applied ONCE per button (see
// makeStateBtnStyle() below) -- state changes then just toggle the
// vkState dynamic property and repolish (setBtnState()), instead of
// calling applyStyleSheet() again on every transition. Same pattern as
// AppletPanel::setScrollHandleActive()/RxApplet's squelch slider.
QString activeStateStyle(const QString& state, const QString& bg, const QString& border, const QString& fg)
{
    return QStringLiteral(
        "QPushButton[vkState=\"%1\"] { background: %2; border: 2px solid %3; border-radius: 4px; "
        "color: %4; font-size: 10px; font-weight: bold; box-shadow: inset 0 0 0 1px rgba(255,255,255,0.12); } "
        "QPushButton[vkState=\"%1\"]:hover { background: %2; border: 2px solid %3; } "
        "QPushButton[vkState=\"%1\"]:pressed { background: %2; border: 2px solid %3; }")
        .arg(state, bg, border, fg);
}

QString neutralBtnStyle()
{
    return QStringLiteral(
        "QPushButton { background: {{color.background.2}}; border: 1px solid {{color.background.2}}; "
        "border-radius: 3px; color: {{color.text.primary}}; font-size: 10px; font-weight: bold; }"
        "QPushButton:hover { background: {{color.background.1}}; }"
        "QPushButton:disabled { background: {{color.background.0}}; border: 1px solid {{color.border.subtle}}; "
        "color: {{color.text.disabled}}; }");
}

// Combines the always-present neutral rules with however many named
// active-state variants a given button needs (bypass/AMP ON has two;
// everything else has one), so the whole thing can be handed to
// applyStyleSheet() a single time.
QString makeStateBtnStyle(std::initializer_list<QString> stateVariants)
{
    QString sheet = neutralBtnStyle();
    for (const auto& variant : stateVariants) {
        sheet += variant;
    }
    return sheet;
}

// Switches which named QSS state a button paints as, without re-calling
// setStyleSheet() -- the full template (neutral + every active variant)
// is already resolved and set once at construction time.
void setBtnState(QPushButton* btn, const QString& state)
{
    if (btn->property("vkState").toString() == state) return;
    btn->setProperty("vkState", state);
    btn->style()->unpolish(btn);
    btn->style()->polish(btn);
}

QPushButton* makeSmallButton(const QString& text, QWidget* parent)
{
    auto* btn = new QPushButton(text, parent);
    btn->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    return btn;
}

// Reflected-power axis as a fraction of the variant's rated output. 15% puts
// the W2000 unit at the 300W axis this shipped with, and scales the smaller
// units down instead of leaving them with an axis they can never reach.
constexpr float kReflectedScaleFraction = 0.15f;

float reflectedFullScale(Vkamp::Variant v)
{
    return Vkamp::ratedWatts(v) * kReflectedScaleFraction;
}

}  // namespace

VkampApplet::VkampApplet(QWidget* parent)
    : QWidget(parent)
{
    theme::setContainer(this, QStringLiteral("applet/vkamp"));
    auto* vbox = new QVBoxLayout(this);
    vbox->setContentsMargins(4, 2, 4, 2);
    vbox->setSpacing(2);

    // ── Header: status pill ─────────────────────────────────────────────────
    m_statusPill = new QLabel("—", this);
    // A status badge, per docs/a11y.md Section 1 -- without a name an AT
    // announces the bare em-dash placeholder and nothing else.
    m_statusPill->setAccessibleName(tr("Amplifier connection status"));
    AetherSDR::ThemeManager::instance().applyStyleSheet(m_statusPill,
        "QLabel { background: {{color.background.1}}; color: {{color.text.label}}; "
        "border: 1px solid {{color.border.strong}}; border-radius: 3px; font-size: 9px; "
        "font-weight: bold; padding: 2px 6px; }");
    m_statusPill->setAlignment(Qt::AlignCenter);
    auto* headerRow = new QHBoxLayout;
    headerRow->addStretch();
    headerRow->addWidget(m_statusPill);
    vbox->addLayout(headerRow);

    // ── PWR row ───────────────────────────────────────────────────────────
    m_pwrLabel = makeValueLabel(this);
    m_pwrLabel->setText("PWR");
    m_pwrGauge = new HGauge(0.0f, Vkamp::meterFullScaleWatts(m_variant), Vkamp::ratedWatts(m_variant),
        "", "", evenTicks(Vkamp::meterFullScaleWatts(m_variant)), this);
    m_pwrGauge->setBallistics({0.030f, 0.800f});
    m_pwrGauge->setAccessibleName(tr("Forward power"));
    auto* pwrRow = new QHBoxLayout;
    pwrRow->setSpacing(4);
    pwrRow->addWidget(m_pwrLabel);
    pwrRow->addWidget(m_pwrGauge, 1);
    vbox->addLayout(pwrRow);

    // ── REF row ───────────────────────────────────────────────────────────
    m_refLabel = makeValueLabel(this);
    m_refLabel->setText("REF");
    // Scaled off the variant like the forward gauge, not a fixed 300W: on a
    // 600W unit a 300W full-scale reflected axis is most of the rated output
    // and never leaves the first sixth of the bar. kReflectedScaleFraction
    // keeps the W2000 axis at exactly the 300W this shipped with.
    m_refGauge = new HGauge(0.0f, reflectedFullScale(m_variant), reflectedFullScale(m_variant) / 2.0f,
        "", "", evenTicks(reflectedFullScale(m_variant)), this);
    m_refGauge->setAccessibleName(tr("Reflected power"));
    auto* refRow = new QHBoxLayout;
    refRow->setSpacing(4);
    refRow->addWidget(m_refLabel);
    refRow->addWidget(m_refGauge, 1);
    vbox->addLayout(refRow);

    // ── SWR row ───────────────────────────────────────────────────────────
    m_swrLabel = makeValueLabel(this);
    m_swrLabel->setText("SWR");
    m_swrGauge = new HGauge(1.0f, 3.0f, 2.5f, "", "",
        {{1.0f, "1"}, {1.5f, "1.5"}, {2.0f, "2"}, {2.5f, "2.5"}, {3.0f, "3"}},
        this, 2.0f);
    m_swrGauge->setAccessibleName(tr("SWR"));
    auto* swrRow = new QHBoxLayout;
    swrRow->setSpacing(4);
    swrRow->addWidget(m_swrLabel);
    swrRow->addWidget(m_swrGauge, 1);
    vbox->addLayout(swrRow);

    vbox->addSpacing(4);

    // ── Info grid: temp / volts / current, band / antenna / fault ──────────
    const QString kTelStyle = "QLabel { color: {{color.text.primary}}; font-size: 10px; }";
    auto& theme = AetherSDR::ThemeManager::instance();

    m_tempLabel = new QLabel("TEMP  — C", this);
    theme.applyStyleSheet(m_tempLabel, kTelStyle);
    m_voltsLabel = new QLabel("SUPPLY  — V", this);
    theme.applyStyleSheet(m_voltsLabel, kTelStyle);
    m_currentLabel = new QLabel("CURR  — A", this);
    theme.applyStyleSheet(m_currentLabel, kTelStyle);
    m_bandLabel = new QLabel("BAND  —", this);
    theme.applyStyleSheet(m_bandLabel, kTelStyle);
    m_antennaLabel = new QLabel("ANT  —", this);
    theme.applyStyleSheet(m_antennaLabel, kTelStyle);

    auto* infoGrid = new QGridLayout;
    infoGrid->setHorizontalSpacing(12);
    infoGrid->setVerticalSpacing(2);
    infoGrid->addWidget(m_tempLabel,    0, 0);
    infoGrid->addWidget(m_voltsLabel,   0, 1);
    infoGrid->addWidget(m_currentLabel, 0, 2);
    infoGrid->addWidget(m_bandLabel,    1, 0);
    infoGrid->addWidget(m_antennaLabel, 1, 1);
    vbox->addLayout(infoGrid);

    vbox->addSpacing(4);

    // ── Fault banner (own row, full width, only shown when active) ─────────
    // Raw numeric code only, no fault-name lookup -- see design doc Section 1
    // (Constitution Principle IV: the only known code->name mapping for this
    // amp is decompile-derived, and is deliberately not carried into this
    // codebase).
    m_faultLabel = new QLabel(this);
    m_faultLabel->setWordWrap(true);
    theme.applyStyleSheet(m_faultLabel, "QLabel { color: {{color.accent.danger}}; font-size: 10px; font-weight: bold; }");
    m_faultLabel->hide();
    vbox->addWidget(m_faultLabel);

    // ── Bypass / Cooling row ─────────────────────────────────────────────────
    m_bypassBtn = makeSmallButton("BYPASS", this);
    m_bypassBtn->setAccessibleDescription(
        tr("Switches the amplifier between bypass and in-line. The label shows the current state, "
           "not the action."));
    // Both blocks live in one sheet now, and QPushButton[vkState="ampOn"] and
    // QPushButton:disabled are equal CSS2 specificity (0,1,1) with the variant
    // appended last -- so source order hands a disabled button the bright
    // green if vkState is ever "ampOn". Nothing about the sheet prevents that;
    // setBypass()'s m_connected ternary is the guard, and setConnected(false)
    // calls setBypass(false) to clear it.
    AetherSDR::ThemeManager::instance().applyStyleSheet(m_bypassBtn,
        makeStateBtnStyle({
            activeStateStyle("bypass", "{{color.background.1}}", "{{color.background.2}}", "{{color.accent.warning}}"),
            activeStateStyle("ampOn", "{{color.background.0}}", "{{color.accent.success}}", "{{color.accent.success}}"),
        }));
    connect(m_bypassBtn, &QPushButton::clicked, this, [this]() { emit bypassToggled(!m_bypassed); });

    m_coolingBtn = makeSmallButton("COOLING", this);
    m_coolingBtn->setAccessibleName(tr("Cooling override"));
    AetherSDR::ThemeManager::instance().applyStyleSheet(m_coolingBtn,
        makeStateBtnStyle({activeStateStyle("active", "{{color.background.1}}", "{{color.background.2}}", "{{color.text.primary}}")}));
    connect(m_coolingBtn, &QPushButton::clicked, this, [this]() {
        emit coolingToggled(m_coolingBtn->property("vkState").toString() != "active");
    });

    auto* toggleRow = new QHBoxLayout;
    toggleRow->setSpacing(6);
    toggleRow->addWidget(m_bypassBtn);
    toggleRow->addWidget(m_coolingBtn);
    vbox->addLayout(toggleRow);

    // ── Antenna row ───────────────────────────────────────────────────────
    auto* antLabel = new QLabel("ANT", this);
    theme.applyStyleSheet(antLabel, kTelStyle);
    m_ant1Btn = makeSmallButton("1", this);
    m_ant2Btn = makeSmallButton("2", this);
    m_ant3Btn = makeSmallButton("3", this);
    // "1"/"2"/"3" carry no meaning on their own to an AT reading the button
    // out of its row context -- docs/a11y.md Section 1.
    m_ant1Btn->setAccessibleName(tr("Antenna port 1"));
    m_ant2Btn->setAccessibleName(tr("Antenna port 2"));
    m_ant3Btn->setAccessibleName(tr("Antenna port 3"));
    const QString antBtnStyle = makeStateBtnStyle(
        {activeStateStyle("active", "{{color.background.1}}", "{{color.accent}}", "{{color.text.primary}}")});
    for (auto* btn : {m_ant1Btn, m_ant2Btn, m_ant3Btn}) {
        AetherSDR::ThemeManager::instance().applyStyleSheet(btn, antBtnStyle);
    }
    // Read-only display, not an optimistic latch -- see setAntenna()'s own
    // doc comment. Clicking just asks; the label/highlight only moves once
    // the live status confirms it.
    connect(m_ant1Btn, &QPushButton::clicked, this, [this]() { emit antennaSelected(1); });
    connect(m_ant2Btn, &QPushButton::clicked, this, [this]() { emit antennaSelected(2); });
    connect(m_ant3Btn, &QPushButton::clicked, this, [this]() { emit antennaSelected(3); });

    auto* antRow = new QHBoxLayout;
    antRow->setSpacing(6);
    antRow->addWidget(antLabel);
    antRow->addWidget(m_ant1Btn);
    antRow->addWidget(m_ant2Btn);
    antRow->addWidget(m_ant3Btn);
    antRow->addStretch();
    vbox->addLayout(antRow);

    // ── Voltage rail row ──────────────────────────────────────────────────
    // Two fixed setpoints, never a dial -- see design doc Section 5. Both
    // buttons are disabled while bypassed (refreshVoltageButtons()) -- a
    // real-hardware safety finding, not a cosmetic choice.
    auto* railLabel = new QLabel("RAIL", this);
    theme.applyStyleSheet(railLabel, kTelStyle);
    m_voltLowBtn = makeSmallButton("LOW", this);
    m_voltHighBtn = makeSmallButton("HIGH", this);
    m_voltLowBtn->setAccessibleName(tr("Low voltage rail"));
    m_voltHighBtn->setAccessibleName(tr("High voltage rail"));
    m_voltLowBtn->setAccessibleDescription(
        tr("Selects the amplifier's low supply rail. Unavailable while the amplifier is bypassed."));
    m_voltHighBtn->setAccessibleDescription(
        tr("Selects the amplifier's high supply rail. Unavailable while the amplifier is bypassed."));
    AetherSDR::ThemeManager::instance().applyStyleSheet(m_voltLowBtn,
        makeStateBtnStyle({activeStateStyle("active", "{{color.background.0}}", "{{color.accent.danger}}", "{{color.text.primary}}")}));
    AetherSDR::ThemeManager::instance().applyStyleSheet(m_voltHighBtn,
        makeStateBtnStyle({activeStateStyle("active", "{{color.background.1}}", "{{color.accent}}", "{{color.text.primary}}")}));
    connect(m_voltLowBtn, &QPushButton::clicked, this, [this]() { emit voltageSelected(true); });
    connect(m_voltHighBtn, &QPushButton::clicked, this, [this]() { emit voltageSelected(false); });

    auto* railRow = new QHBoxLayout;
    railRow->setSpacing(6);
    railRow->addWidget(railLabel);
    railRow->addWidget(m_voltLowBtn);
    railRow->addWidget(m_voltHighBtn);
    railRow->addStretch();
    vbox->addLayout(railRow);

    // ── Reset row ─────────────────────────────────────────────────────────
    // Hold-to-confirm on the amp's own end (the wire command needs ~9-12s of
    // repeated sends to register -- design doc Section 4); the confirm
    // dialog here is this app's own gate against an accidental click, same
    // pattern as every other destructive-action confirm in this codebase
    // (see e.g. ProfileManagerDialog's delete-profile confirm).
    m_resetBtn = new QPushButton("RESET", this);
    m_resetBtn->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    m_resetBtn->setAccessibleName(tr("Reset amplifier"));
    m_resetBtn->setAccessibleDescription(
        tr("Asks the amplifier to reset. Confirms first, then holds the command for about ten seconds."));
    theme.applyStyleSheet(m_resetBtn,
        "QPushButton { background: {{color.background.tx}}; border: 1px solid {{color.border.tx}}; "
        "border-radius: 3px; color: {{color.accent.warning}}; font-size: 10px; font-weight: bold; padding: 2px 8px; }"
        "QPushButton:hover { background: {{color.background.tx}}; }"
        "QPushButton:disabled { background: {{color.background.0}}; border: 1px solid {{color.border.subtle}}; "
        "color: {{color.text.disabled}}; }");
    connect(m_resetBtn, &QPushButton::clicked, this, [this]() {
        const auto reply = QMessageBox::question(this, "Reset Amplifier",
            "Reset the VK3AMP? The amp needs the reset command held for about "
            "10 seconds to register.",
            QMessageBox::Yes | QMessageBox::No);
        if (reply == QMessageBox::Yes) {
            emit resetRequested();
        }
    });
    auto* resetRow = new QHBoxLayout;
    resetRow->addStretch();
    resetRow->addWidget(m_resetBtn);
    vbox->addLayout(resetRow);

    m_labelTimer.setInterval(kMeterReadoutUpdateMs);
    connect(&m_labelTimer, &QTimer::timeout, this, &VkampApplet::updateValueLabels);
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

void VkampApplet::setVariant(Vkamp::Variant variant)
{
    m_variant = variant;
    const float maxW = Vkamp::meterFullScaleWatts(variant);
    m_pwrGauge->setRange(0.0f, maxW, Vkamp::ratedWatts(variant), evenTicks(maxW));
    const float refMax = reflectedFullScale(variant);
    m_refGauge->setRange(0.0f, refMax, refMax / 2.0f, evenTicks(refMax));
}

void VkampApplet::clearTelemetry()
{
    // Only the TX-gated UDP fields -- see this method's own doc comment.
    m_fwdWatts = 0.0f;
    m_reflectedWatts = 0.0f;
    m_swrVal = 1.0f;
    m_currentAmps = 0.0f;
    m_peakFwd = 0.0f;
    if (m_peakTimer) { m_peakTimer->stop(); }
    m_pwrGauge->setValue(0.0f);
    m_pwrGauge->clearPeak();
    m_refGauge->setValue(0.0f);
    m_swrGauge->setValue(1.0f);
    m_valuesDirty = true;
    updateValueLabels();
}

void VkampApplet::setForwardPower(float watts)
{
    m_fwdWatts = watts;
    m_pwrGauge->setValue(watts);
    if (watts > m_peakFwd) {
        m_peakFwd = watts;
        m_pwrGauge->setPeakValue(watts);
        m_peakTimer->start();
    }
    m_valuesDirty = true;
}

void VkampApplet::setReflectedPower(float watts)
{
    m_reflectedWatts = watts;
    // Reflected power only means anything alongside forward drive -- hold
    // the needle at baseline below 1W, same gate AcomApplet uses.
    m_refGauge->setValue(m_fwdWatts >= 1.0f ? watts : 0.0f);
    m_valuesDirty = true;
}

void VkampApplet::setSwr(float swr)
{
    m_swrVal = swr;
    m_swrGauge->setValue(m_fwdWatts >= 1.0f ? swr : 1.0f);
    m_valuesDirty = true;
}

void VkampApplet::setCurrent(float amps)
{
    m_currentAmps = amps;
    m_valuesDirty = true;
}

void VkampApplet::setTemp(float degC)
{
    m_tempC = degC;
    m_valuesDirty = true;
}

void VkampApplet::setSupplyVoltage(float volts)
{
    m_volts = volts;
    m_valuesDirty = true;
}

void VkampApplet::setBand(const QString& band)
{
    m_pendingBand = band;
    m_bandDirty = true;
}

void VkampApplet::setAntenna(int port)
{
    m_antennaPort = port;
    m_bandDirty = true;  // reuses the same throttle flag as band -- both are low-rate status fields

    setBtnState(m_ant1Btn, port == 1 ? "active" : QString());
    setBtnState(m_ant2Btn, port == 2 ? "active" : QString());
    setBtnState(m_ant3Btn, port == 3 ? "active" : QString());
}

void VkampApplet::setBypass(bool on)
{
    m_bypassed = on;
    m_bypassBtn->setText(on ? "BYPASS" : "AMP ON");
    // The label is a STATE, not an action ("BYPASS" means it is bypassed), so
    // spell that out for an AT rather than leaving it to infer a verb --
    // docs/a11y.md Section 1's dynamic-name case.
    m_bypassBtn->setAccessibleName(on ? tr("Amplifier bypassed") : tr("Amplifier in line"));
    // Text-only value change -- see docs/a11y.md; same pattern as
    // AcomApplet::updateTempLabel()'s own dynamic-text button.
    if (QAccessible::isActive()) {
        QAccessibleEvent event(m_bypassBtn, QAccessible::NameChanged);
        QAccessible::updateAccessibility(&event);
    }
    // THIS ternary is the only thing keeping the green off a disconnected
    // button: the stylesheet cascade would happily paint an undimmed "AMP ON"
    // on a disabled button (the variant block wins on source order over
    // :disabled at equal specificity), which reads as a live, in-line
    // amplifier that isn't even connected. Green matches the "CONNECTED"
    // status pill's own palette -- AMP ON is the normal/good operating state.
    setBtnState(m_bypassBtn, on ? "bypass" : (m_connected ? "ampOn" : QString()));
    refreshVoltageButtons();
}

void VkampApplet::setCoolingOverride(bool on)
{
    setBtnState(m_coolingBtn, on ? "active" : QString());
}

void VkampApplet::setVoltageLow(bool low)
{
    m_voltageLow = low;
    refreshVoltageButtons();
}

void VkampApplet::refreshVoltageButtons()
{
    // Neither button shows active while bypassed, and both are disabled (not
    // just visually muted) for the entire time bypass is on. This is design
    // doc Section 5's real-hardware finding, and that finding is the whole
    // authority for it: bypass parks the supply at a standby reading (~6.3V)
    // that is neither rail target, so "volts below the midpoint" would paint
    // LOW as active when nobody selected it -- and commanding a rail change
    // from that state was observed live pulling the supply toward ~0V.
    // Gated on m_connected for the same reason the bypass button is: a
    // disabled button carrying vkState="active" still paints in the full
    // active colours, so without this a disconnected amp shows a bright,
    // confident RAIL LOW next to a greyed-out BYPASS and a "—" status pill.
    // m_voltageLow itself is untouched, so the selection repaints correctly
    // the moment the amp reconnects and reports a rail.
    const bool lowActive = m_voltageLow && !m_bypassed && m_connected;
    const bool highActive = !m_voltageLow && !m_bypassed && m_connected;
    setBtnState(m_voltLowBtn, lowActive ? "active" : QString());
    setBtnState(m_voltHighBtn, highActive ? "active" : QString());
    const bool enabled = m_connected && !m_bypassed && !m_resetting;
    m_voltLowBtn->setEnabled(enabled);
    m_voltHighBtn->setEnabled(enabled);
}

void VkampApplet::setFaultCode(int code)
{
    m_faultCode = code;
    if (code == 0) {
        m_faultLabel->hide();
        m_faultLabel->clear();
        return;
    }
    m_faultLabel->setText(QStringLiteral("Error %1").arg(code));
    m_faultLabel->show();
}

void VkampApplet::setResetProgress(double remainingSeconds, bool active)
{
    m_resetting = active;
    if (active) {
        m_resetBtn->setEnabled(false);
        m_resetBtn->setText(QStringLiteral("RESETTING… %1s").arg(static_cast<int>(remainingSeconds + 0.5)));
    } else {
        m_resetBtn->setEnabled(m_connected);
        m_resetBtn->setText("RESET");
    }
    // The hold owns the wire for its whole duration -- VkampConnection drops
    // any other command while it runs, so these have to look unavailable
    // rather than accept a click that will be thrown away.
    const bool controlsLive = m_connected && !active;
    m_bypassBtn->setEnabled(controlsLive);
    m_coolingBtn->setEnabled(controlsLive);
    m_ant1Btn->setEnabled(controlsLive);
    m_ant2Btn->setEnabled(controlsLive);
    m_ant3Btn->setEnabled(controlsLive);
    refreshVoltageButtons();
    // Text-only value change -- see docs/a11y.md. Fires at most once per
    // second (VkampConnection's own kResetIntervalSeconds), not high-rate.
    if (QAccessible::isActive()) {
        QAccessibleEvent event(m_resetBtn, QAccessible::NameChanged);
        QAccessible::updateAccessibility(&event);
    }
}

void VkampApplet::setConnected(bool connected)
{
    m_connected = connected;
    if (!connected) { m_resetting = false; }
    m_bypassBtn->setEnabled(connected);
    m_coolingBtn->setEnabled(connected);
    m_ant1Btn->setEnabled(connected);
    m_ant2Btn->setEnabled(connected);
    m_ant3Btn->setEnabled(connected);
    m_resetBtn->setEnabled(connected);
    refreshVoltageButtons();

    m_statusPill->setText(connected ? QStringLiteral("CONNECTED") : QStringLiteral("—"));
    AetherSDR::ThemeManager::instance().applyStyleSheet(m_statusPill, connected
        ? "QLabel { background: {{color.background.0}}; color: {{color.accent.success}}; "
          "border: 1px solid {{color.accent.success}}; border-radius: 3px; font-size: 9px; "
          "font-weight: bold; padding: 2px 6px; }"
        : "QLabel { background: {{color.background.1}}; color: {{color.text.label}}; "
          "border: 1px solid {{color.border.strong}}; border-radius: 3px; font-size: 9px; "
          "font-weight: bold; padding: 2px 6px; }");

    if (!connected) {
        setFaultCode(0);
        setBypass(false);
        setCoolingOverride(false);
        m_antennaPort = 0;
        m_pendingBand.clear();
        m_bandLabel->setText("BAND  —");
        m_antennaLabel->setText("ANT  —");
        for (auto* btn : {m_ant1Btn, m_ant2Btn, m_ant3Btn}) {
            setBtnState(btn, QString());
        }
        m_tempC = 0.0f;
        m_volts = 0.0f;
        m_currentAmps = 0.0f;
        m_tempLabel->setText("TEMP  — C");
        m_voltsLabel->setText("SUPPLY  — V");
        m_currentLabel->setText("CURR  — A");
        // Consume any pending status update now, before updateValueLabels()
        // below -- otherwise a status frame that landed in the last
        // <=100ms leaves this true, and updateValueLabels()'s own
        // if (m_valuesDirty) block immediately overwrites the placeholders
        // above with "0C"/"0.0V"/"0.0A", which reads as a real reading
        // rather than "no data" for the rest of the disconnected session.
        m_valuesDirty = false;
        m_fwdWatts = 0.0f;
        m_reflectedWatts = 0.0f;
        m_swrVal = 1.0f;
        m_peakFwd = 0.0f;
        if (m_peakTimer) { m_peakTimer->stop(); }
        m_pwrGauge->setValueImmediate(0.0f);
        m_pwrGauge->clearPeak();
        m_refGauge->setValueImmediate(0.0f);
        m_swrGauge->setValueImmediate(1.0f);
        setResetProgress(0.0, false);
        updateValueLabels();
    }
}

void VkampApplet::updateValueLabels()
{
    m_pwrLabel->setText(m_fwdWatts >= 1.0f
        ? QStringLiteral("PWR  %1").arg(static_cast<int>(m_fwdWatts))
        : QStringLiteral("PWR"));
    m_refLabel->setText(m_fwdWatts >= 1.0f
        ? QStringLiteral("REF  %1").arg(static_cast<int>(m_reflectedWatts))
        : QStringLiteral("REF"));
    m_swrLabel->setText(m_fwdWatts >= 1.0f
        ? QStringLiteral("SWR  %1:1").arg(m_swrVal, 0, 'f', 1)
        : QStringLiteral("SWR"));

    if (m_valuesDirty) {
        m_valuesDirty = false;
        m_tempLabel->setText(QStringLiteral("TEMP  %1C").arg(static_cast<int>(m_tempC)));
        m_voltsLabel->setText(QStringLiteral("SUPPLY  %1V").arg(m_volts, 0, 'f', 1));
        m_currentLabel->setText(QStringLiteral("CURR  %1A").arg(m_currentAmps, 0, 'f', 1));
    }
    if (m_bandDirty) {
        m_bandDirty = false;
        m_bandLabel->setText(QStringLiteral("BAND  %1").arg(m_pendingBand.isEmpty() ? QStringLiteral("—") : m_pendingBand));
        m_antennaLabel->setText(QStringLiteral("ANT  %1").arg(m_antennaPort > 0 ? QString::number(m_antennaPort) : QStringLiteral("—")));
    }
}

}  // namespace AetherSDR
