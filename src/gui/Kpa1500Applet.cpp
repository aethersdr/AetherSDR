#include "Kpa1500Applet.h"
#include "HGauge.h"
#include "MeterSmoother.h"
#include "core/ThemeManager.h"

#include <QAccessible>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QStyle>
#include <QVBoxLayout>

namespace AetherSDR {

namespace {

// Rated output of a KPA1500, and the meter full scale that gives a carrier
// briefly over the nameplate rating somewhere to go instead of pinning —
// same rated + ~25% headroom convention VkampApplet uses.
constexpr float kRatedWatts = 1500.0f;
constexpr float kMeterFullScaleWatts = 1875.0f;
// Reflected axis as a fraction of rated output, same 15% as VkampApplet:
// a full-scale reflected axis the amp can never reach is a wasted bar.
constexpr float kReflectedFullScaleWatts = kRatedWatts * 0.15f;

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
    // 72px: a 1500W amp needs 4-digit readouts, the same width AmpApplet
    // and VkampApplet already use for the same reason.
    lbl->setFixedWidth(72);
    lbl->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    AetherSDR::ThemeManager::instance().applyStyleSheet(lbl,
        "QLabel { color: {{color.text.primary}}; font-size: 11px; font-weight: bold; }");
    return lbl;
}

// One named [kpaState="..."] QSS variant, combined with neutralBtnStyle()
// into a single template applied ONCE per button — state changes then just
// toggle the dynamic property and repolish, instead of re-running
// applyStyleSheet() on every transition. Same pattern as VkampApplet.
QString activeStateStyle(const QString& state, const QString& bg,
                         const QString& border, const QString& fg)
{
    return QStringLiteral(
        "QPushButton[kpaState=\"%1\"] { background: %2; border: 2px solid %3; border-radius: 4px; "
        "color: %4; font-size: 10px; font-weight: bold; } "
        "QPushButton[kpaState=\"%1\"]:hover { background: %2; border: 2px solid %3; } "
        "QPushButton[kpaState=\"%1\"]:pressed { background: %2; border: 2px solid %3; }")
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

QString makeStateBtnStyle(std::initializer_list<QString> stateVariants)
{
    QString sheet = neutralBtnStyle();
    for (const auto& variant : stateVariants) {
        sheet += variant;
    }
    return sheet;
}

void setBtnState(QPushButton* btn, const QString& state)
{
    if (btn->property("kpaState").toString() == state) { return; }
    btn->setProperty("kpaState", state);
    btn->style()->unpolish(btn);
    btn->style()->polish(btn);
}

QPushButton* makeSmallButton(const QString& text, QWidget* parent)
{
    auto* btn = new QPushButton(text, parent);
    btn->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    return btn;
}

const QString& telemetryLabelStyle()
{
    static const QString kStyle =
        QStringLiteral("QLabel { color: {{color.text.primary}}; font-size: 10px; }");
    return kStyle;
}

}  // namespace

Kpa1500Applet::Kpa1500Applet(QWidget* parent)
    : QWidget(parent)
{
    theme::setContainer(this, QStringLiteral("applet/kpa1500"));
    auto* vbox = new QVBoxLayout(this);
    vbox->setContentsMargins(4, 2, 4, 2);
    vbox->setSpacing(2);

    auto& theme = AetherSDR::ThemeManager::instance();

    // ── Header: status pill ──────────────────────────────────────────────
    m_statusPill = new QLabel(QStringLiteral("—"), this);
    // A status badge — without a name an AT announces the bare em-dash
    // placeholder and nothing else (docs/a11y.md §1).
    m_statusPill->setAccessibleName(tr("Amplifier connection status"));
    theme.applyStyleSheet(m_statusPill,
        "QLabel { background: {{color.background.1}}; color: {{color.text.label}}; "
        "border: 1px solid {{color.border.strong}}; border-radius: 3px; font-size: 9px; "
        "font-weight: bold; padding: 2px 6px; }");
    m_statusPill->setAlignment(Qt::AlignCenter);
    auto* headerRow = new QHBoxLayout;
    headerRow->addStretch();
    headerRow->addWidget(m_statusPill);
    vbox->addLayout(headerRow);

    // ── PWR / REF / SWR gauges ───────────────────────────────────────────
    m_pwrLabel = makeValueLabel(this);
    m_pwrLabel->setText(QStringLiteral("PWR"));
    m_pwrGauge = new HGauge(0.0f, kMeterFullScaleWatts, kRatedWatts, QString(), QString(),
                            evenTicks(kMeterFullScaleWatts), this);
    m_pwrGauge->setWindowPeakEnabled(true);
    m_pwrGauge->setAccessibleName(tr("Forward power"));
    auto* pwrRow = new QHBoxLayout;
    pwrRow->setSpacing(4);
    pwrRow->addWidget(m_pwrLabel);
    pwrRow->addWidget(m_pwrGauge, 1);
    vbox->addLayout(pwrRow);

    m_refLabel = makeValueLabel(this);
    m_refLabel->setText(QStringLiteral("REF"));
    m_refGauge = new HGauge(0.0f, kReflectedFullScaleWatts, kReflectedFullScaleWatts / 2.0f,
                            QString(), QString(), evenTicks(kReflectedFullScaleWatts), this);
    m_refGauge->setAccessibleName(tr("Reflected power"));
    auto* refRow = new QHBoxLayout;
    refRow->setSpacing(4);
    refRow->addWidget(m_refLabel);
    refRow->addWidget(m_refGauge, 1);
    vbox->addLayout(refRow);

    m_swrLabel = makeValueLabel(this);
    m_swrLabel->setText(QStringLiteral("SWR"));
    m_swrGauge = new HGauge(1.0f, 3.0f, 2.5f, QString(), QString(),
                            {{1.0f, QStringLiteral("1")},   {1.5f, QStringLiteral("1.5")},
                             {2.0f, QStringLiteral("2")},   {2.5f, QStringLiteral("2.5")},
                             {3.0f, QStringLiteral("3")}},
                            this, 2.0f);
    m_swrGauge->setAccessibleName(tr("SWR"));
    auto* swrRow = new QHBoxLayout;
    swrRow->setSpacing(4);
    swrRow->addWidget(m_swrLabel);
    swrRow->addWidget(m_swrGauge, 1);
    vbox->addLayout(swrRow);

    vbox->addSpacing(4);

    // ── Info grid: temp / band / ATU ─────────────────────────────────────
    m_tempLabel = new QLabel(QStringLiteral("TEMP  — C"), this);
    theme.applyStyleSheet(m_tempLabel, telemetryLabelStyle());
    m_bandLabel = new QLabel(QStringLiteral("BAND  —"), this);
    theme.applyStyleSheet(m_bandLabel, telemetryLabelStyle());
    m_atuLabel = new QLabel(QStringLiteral("ATU  —"), this);
    theme.applyStyleSheet(m_atuLabel, telemetryLabelStyle());

    auto* infoGrid = new QGridLayout;
    infoGrid->setHorizontalSpacing(12);
    infoGrid->setVerticalSpacing(2);
    infoGrid->addWidget(m_tempLabel, 0, 0);
    infoGrid->addWidget(m_bandLabel, 0, 1);
    infoGrid->addWidget(m_atuLabel,  0, 2);
    vbox->addLayout(infoGrid);

    vbox->addSpacing(4);

    // ── Fault banner (own row, only shown when a fault stands) ───────────
    m_faultLabel = new QLabel(this);
    m_faultLabel->setWordWrap(true);
    theme.applyStyleSheet(m_faultLabel,
        "QLabel { color: {{color.accent.danger}}; font-size: 10px; font-weight: bold; }");
    m_faultLabel->hide();
    vbox->addWidget(m_faultLabel);

    // ── OPERATE / TUNE / ATU IN row ──────────────────────────────────────
    m_operateBtn = makeSmallButton(QStringLiteral("STANDBY"), this);
    m_operateBtn->setAccessibleDescription(
        tr("Switches the amplifier between operate and standby. The label shows the current "
           "state, not the action."));
    theme.applyStyleSheet(m_operateBtn, makeStateBtnStyle({
        activeStateStyle(QStringLiteral("operate"), QStringLiteral("{{color.background.0}}"),
                         QStringLiteral("{{color.accent.success}}"),
                         QStringLiteral("{{color.accent.success}}")),
    }));
    connect(m_operateBtn, &QPushButton::clicked, this, [this]() {
        // Asks; the label only moves when the amp's own `^OS` readback
        // confirms it (Principle II — no optimistic latch).
        emit operateToggled(!m_status.operate.value_or(false));
    });

    m_tuneBtn = makeSmallButton(QStringLiteral("TUNE"), this);
    m_tuneBtn->setAccessibleName(tr("Start antenna tuner"));
    m_tuneBtn->setAccessibleDescription(
        tr("Starts a tuning cycle on the amplifier's internal tuner. The amplifier supplies its "
           "own tuning carrier; this does not key the radio."));
    theme.applyStyleSheet(m_tuneBtn, makeStateBtnStyle({
        activeStateStyle(QStringLiteral("active"), QStringLiteral("{{color.background.1}}"),
                         QStringLiteral("{{color.accent.warning}}"),
                         QStringLiteral("{{color.accent.warning}}")),
    }));
    connect(m_tuneBtn, &QPushButton::clicked, this, [this]() { emit tuneRequested(); });

    m_atuInlineBtn = makeSmallButton(QStringLiteral("ATU IN"), this);
    m_atuInlineBtn->setAccessibleDescription(
        tr("Switches the internal tuner in line or bypassed. The label shows the current state, "
           "not the action."));
    theme.applyStyleSheet(m_atuInlineBtn, makeStateBtnStyle({
        activeStateStyle(QStringLiteral("active"), QStringLiteral("{{color.background.1}}"),
                         QStringLiteral("{{color.accent}}"),
                         QStringLiteral("{{color.text.primary}}")),
    }));
    connect(m_atuInlineBtn, &QPushButton::clicked, this, [this]() {
        emit atuInlineToggled(!m_status.atuInline.value_or(false));
    });

    auto* ctrlRow = new QHBoxLayout;
    ctrlRow->setSpacing(6);
    ctrlRow->addWidget(m_operateBtn);
    ctrlRow->addWidget(m_tuneBtn);
    ctrlRow->addWidget(m_atuInlineBtn);
    ctrlRow->addStretch();
    vbox->addLayout(ctrlRow);

    // ── Antenna row ──────────────────────────────────────────────────────
    auto* antLabel = new QLabel(QStringLiteral("ANT"), this);
    theme.applyStyleSheet(antLabel, telemetryLabelStyle());
    m_ant1Btn = makeSmallButton(QStringLiteral("1"), this);
    m_ant2Btn = makeSmallButton(QStringLiteral("2"), this);
    m_ant3Btn = makeSmallButton(QStringLiteral("3"), this);
    // Bare digits carry no meaning to an AT reading a button out of its row
    // context (docs/a11y.md §1).
    m_ant1Btn->setAccessibleName(tr("Antenna port 1"));
    m_ant2Btn->setAccessibleName(tr("Antenna port 2"));
    m_ant3Btn->setAccessibleName(tr("Antenna port 3"));
    const QString antBtnStyle = makeStateBtnStyle({
        activeStateStyle(QStringLiteral("active"), QStringLiteral("{{color.background.1}}"),
                         QStringLiteral("{{color.accent}}"),
                         QStringLiteral("{{color.text.primary}}")),
    });
    for (auto* btn : {m_ant1Btn, m_ant2Btn, m_ant3Btn}) {
        theme.applyStyleSheet(btn, antBtnStyle);
    }
    connect(m_ant1Btn, &QPushButton::clicked, this, [this]() { emit antennaSelected(1); });
    connect(m_ant2Btn, &QPushButton::clicked, this, [this]() { emit antennaSelected(2); });
    connect(m_ant3Btn, &QPushButton::clicked, this, [this]() { emit antennaSelected(3); });

    m_clearFaultBtn = makeSmallButton(QStringLiteral("CLR FAULT"), this);
    m_clearFaultBtn->setAccessibleName(tr("Clear amplifier fault"));
    theme.applyStyleSheet(m_clearFaultBtn, neutralBtnStyle());
    m_clearFaultBtn->hide();
    connect(m_clearFaultBtn, &QPushButton::clicked, this, [this]() { emit faultClearRequested(); });

    auto* antRow = new QHBoxLayout;
    antRow->setSpacing(6);
    antRow->addWidget(antLabel);
    antRow->addWidget(m_ant1Btn);
    antRow->addWidget(m_ant2Btn);
    antRow->addWidget(m_ant3Btn);
    antRow->addStretch();
    antRow->addWidget(m_clearFaultBtn);
    vbox->addLayout(antRow);

    m_labelTimer.setInterval(kMeterReadoutUpdateMs);
    connect(&m_labelTimer, &QTimer::timeout, this, &Kpa1500Applet::updateValueLabels);
    m_labelTimer.start();

    setConnected(false);
}

void Kpa1500Applet::setStatus(const Kpa1500::Status& status)
{
    m_status = status;
    m_valuesDirty = true;

    const float fwd = status.forwardWatts.value_or(0.0f);
    m_pwrGauge->setValue(fwd);
    // Reflected power and SWR only mean anything alongside forward drive —
    // hold both at baseline below 1W, the same gate AcomApplet/VkampApplet
    // use, so an idle amp does not paint a meaningless SWR.
    m_refGauge->setValue(fwd >= 1.0f ? status.reflectedWatts.value_or(0.0f) : 0.0f);
    m_swrGauge->setValue(fwd >= 1.0f ? status.swr.value_or(1.0f) : 1.0f);

    const int fault = status.faultCode.value_or(0);
    if (fault == 0) {
        m_faultLabel->hide();
        m_faultLabel->clear();
        m_clearFaultBtn->hide();
    } else {
        // Raw numeric code: the fault-code-to-name table is not something
        // this integration has confirmed against hardware, and a wrong
        // fault NAME is worse than an honest number (design doc §7).
        m_faultLabel->setText(tr("Fault %1").arg(fault));
        m_faultLabel->show();
        m_clearFaultBtn->setVisible(m_connected);
    }

    refreshControls();
}

void Kpa1500Applet::refreshControls()
{
    const bool operate = m_status.operate.value_or(false);
    m_operateBtn->setText(operate ? QStringLiteral("OPERATE") : QStringLiteral("STANDBY"));
    // The label is a STATE, not an action, so spell that out for an AT
    // rather than leaving it to infer a verb (docs/a11y.md §1).
    m_operateBtn->setAccessibleName(operate ? tr("Amplifier in operate") : tr("Amplifier in standby"));
    if (QAccessible::isActive()) {
        QAccessibleEvent event(m_operateBtn, QAccessible::NameChanged);
        QAccessible::updateAccessibility(&event);
    }
    // The m_connected guard is load-bearing, not cosmetic: a [kpaState] rule
    // and QPushButton:disabled are equal CSS2 specificity, and the variant
    // is appended last, so source order would paint a DISCONNECTED amp in
    // full-brightness "OPERATE" green. Same trap VkampApplet documents on
    // its own bypass button.
    setBtnState(m_operateBtn, operate && m_connected ? QStringLiteral("operate") : QString());

    const bool atuIn = m_status.atuInline.value_or(false);
    m_atuInlineBtn->setText(atuIn ? QStringLiteral("ATU IN") : QStringLiteral("ATU BYP"));
    m_atuInlineBtn->setAccessibleName(atuIn ? tr("Tuner in line") : tr("Tuner bypassed"));
    setBtnState(m_atuInlineBtn, atuIn && m_connected ? QStringLiteral("active") : QString());

    // Read-only reflection of the live `^AN` readback, not a latch of the
    // last button clicked.
    const int ant = m_status.antenna.value_or(0);
    setBtnState(m_ant1Btn, ant == 1 && m_connected ? QStringLiteral("active") : QString());
    setBtnState(m_ant2Btn, ant == 2 && m_connected ? QStringLiteral("active") : QString());
    setBtnState(m_ant3Btn, ant == 3 && m_connected ? QStringLiteral("active") : QString());
}

void Kpa1500Applet::setConnected(bool connected)
{
    m_connected = connected;
    for (auto* btn : {m_operateBtn, m_tuneBtn, m_atuInlineBtn, m_ant1Btn, m_ant2Btn, m_ant3Btn}) {
        btn->setEnabled(connected);
    }

    m_statusPill->setText(connected ? QStringLiteral("CONNECTED") : QStringLiteral("—"));
    AetherSDR::ThemeManager::instance().applyStyleSheet(m_statusPill, connected
        ? "QLabel { background: {{color.background.0}}; color: {{color.accent.success}}; "
          "border: 1px solid {{color.accent.success}}; border-radius: 3px; font-size: 9px; "
          "font-weight: bold; padding: 2px 6px; }"
        : "QLabel { background: {{color.background.1}}; color: {{color.text.label}}; "
          "border: 1px solid {{color.border.strong}}; border-radius: 3px; font-size: 9px; "
          "font-weight: bold; padding: 2px 6px; }");

    if (!connected) {
        // Drop every remembered reading. An unset optional renders as "—",
        // so a disconnected amp reads as "no data" rather than as a
        // confident zero that happens to be the last thing it said.
        m_status = {};
        m_faultLabel->hide();
        m_faultLabel->clear();
        m_clearFaultBtn->hide();
        m_pwrGauge->setValueImmediate(0.0f);
        m_pwrGauge->clearPeak();
        m_refGauge->setValueImmediate(0.0f);
        m_swrGauge->setValueImmediate(1.0f);
        // Consume the dirty flag BEFORE updateValueLabels() so a reply that
        // landed in the last <=100ms cannot repaint real-looking values over
        // the placeholders — the same ordering trap VkampApplet documents.
        m_valuesDirty = true;
        refreshControls();
        updateValueLabels();
    }
}

void Kpa1500Applet::updateValueLabels()
{
    const float fwd = m_status.forwardWatts.value_or(0.0f);
    m_pwrLabel->setText(fwd >= 1.0f
        ? QStringLiteral("PWR  %1").arg(static_cast<int>(fwd))
        : QStringLiteral("PWR"));
    m_refLabel->setText(fwd >= 1.0f
        ? QStringLiteral("REF  %1").arg(static_cast<int>(m_status.reflectedWatts.value_or(0.0f)))
        : QStringLiteral("REF"));
    m_swrLabel->setText(fwd >= 1.0f
        ? QStringLiteral("SWR  %1:1").arg(m_status.swr.value_or(1.0f), 0, 'f', 1)
        : QStringLiteral("SWR"));

    if (!m_valuesDirty) {
        return;
    }
    m_valuesDirty = false;

    m_tempLabel->setText(m_status.tempC
        ? QStringLiteral("TEMP  %1C").arg(*m_status.tempC)
        : QStringLiteral("TEMP  — C"));

    const QString band = m_status.band ? Kpa1500::bandName(*m_status.band) : QString();
    m_bandLabel->setText(QStringLiteral("BAND  %1")
        .arg(band.isEmpty() ? QStringLiteral("—") : band));

    m_atuLabel->setText(QStringLiteral("ATU  %1")
        .arg(Kpa1500::atuModeLabel(m_status.atuMode.value_or(Kpa1500::AtuMode::Unknown))));
}

}  // namespace AetherSDR
