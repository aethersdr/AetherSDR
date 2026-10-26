#include "Kpa1500Applet.h"
#include "HGauge.h"
#include "MeterSmoother.h"
#include "core/AppSettings.h"
#include "core/ThemeManager.h"

#include <QAccessible>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QSignalBlocker>
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

QJsonObject preferences()
{
    return QJsonDocument::fromJson(
        AppSettings::instance().value("Kpa1500Applet").toString().toUtf8()).object();
}

// Enables or dims a control; a dimmed control states why in its tooltip.
void setAvailable(QWidget* w, bool enabled, const QString& reason)
{
    w->setEnabled(enabled);
    w->setToolTip(enabled ? QString() : reason);
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

    m_driveLabel = new QLabel(QStringLiteral("DRIVE  — W"), this);
    m_driveLabel->setAccessibleName(tr("Drive power from the radio"));
    m_supplyLabel = new QLabel(QStringLiteral("PA  — V  — A"), this);
    m_supplyLabel->setAccessibleName(tr("PA supply voltage and current"));
    m_fanLabel = new QLabel(QStringLiteral("FAN  —"), this);
    m_fanLabel->setAccessibleName(tr("Fan speed"));
    m_freqLabel = new QLabel(QStringLiteral("FREQ  — kHz"), this);
    m_freqLabel->setAccessibleName(tr("Frequency the amplifier is set to"));
    for (QLabel* l : {m_driveLabel, m_supplyLabel, m_fanLabel, m_freqLabel}) {
        theme.applyStyleSheet(l, telemetryLabelStyle());
    }

    auto* infoGrid = new QGridLayout;
    infoGrid->setHorizontalSpacing(12);
    infoGrid->setVerticalSpacing(2);
    infoGrid->addWidget(m_tempLabel, 0, 0);
    infoGrid->addWidget(m_bandLabel, 0, 1);
    infoGrid->addWidget(m_atuLabel,  0, 2);
    infoGrid->addWidget(m_driveLabel, 1, 0);
    infoGrid->addWidget(m_supplyLabel, 1, 1);
    infoGrid->addWidget(m_fanLabel, 1, 2);
    infoGrid->addWidget(m_freqLabel, 2, 0, 1, 3);
    vbox->addLayout(infoGrid);

    // The amp's own one-line ATU summary (^DA): side, L and C, stored SWR.
    m_atuDetailLabel = new QLabel(this);
    m_atuDetailLabel->setWordWrap(true);
    m_atuDetailLabel->setAccessibleName(tr("Tuner setting reported by the amplifier"));
    theme.applyStyleSheet(m_atuDetailLabel,
        "QLabel { color: {{color.text.label}}; font-size: 9px; }");
    m_atuDetailLabel->hide();
    vbox->addWidget(m_atuDetailLabel);

    m_attenLabel = new QLabel(this);
    m_attenLabel->setWordWrap(true);
    theme.applyStyleSheet(m_attenLabel,
        "QLabel { color: {{color.accent.warning}}; font-size: 10px; font-weight: bold; }");
    m_attenLabel->hide();
    vbox->addWidget(m_attenLabel);

    vbox->addSpacing(4);

    // ── Fault banner (own row, only shown when a fault stands) ───────────
    m_faultLabel = new QLabel(this);
    m_faultLabel->setWordWrap(true);
    theme.applyStyleSheet(m_faultLabel,
        "QLabel { color: {{color.accent.danger}}; font-size: 10px; font-weight: bold; }");
    m_faultLabel->hide();
    vbox->addWidget(m_faultLabel);

    // A control the amp did not confirm over UDP. Transient: the amp's own
    // readback, shown everywhere else, remains the authority on its state.
    m_noticeLabel = new QLabel(this);
    m_noticeLabel->setObjectName(QStringLiteral("kpa1500ControlNotice"));
    m_noticeLabel->setWordWrap(true);
    theme.applyStyleSheet(m_noticeLabel,
        "QLabel { color: {{color.accent.warning}}; font-size: 10px; font-weight: bold; }");
    m_noticeLabel->hide();
    vbox->addWidget(m_noticeLabel);
    m_noticeTimer.setSingleShot(true);
    m_noticeTimer.setInterval(kNoticeMs);
    connect(&m_noticeTimer, &QTimer::timeout, m_noticeLabel, &QLabel::hide);

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
        tr("Starts a full-search tune on the amplifier's internal tuner. The radio must supply RF "
           "(use the radio's TUNE); this does not key the radio. While a tune is running, "
           "press again to cancel it."));
    theme.applyStyleSheet(m_tuneBtn, makeStateBtnStyle({
        activeStateStyle(QStringLiteral("active"), QStringLiteral("{{color.background.1}}"),
                         QStringLiteral("{{color.accent.warning}}"),
                         QStringLiteral("{{color.accent.warning}}")),
    }));
    connect(m_tuneBtn, &QPushButton::clicked, this, [this]() {
        // Follows the amp's own ^TP readback, not a latch of the last click.
        if (m_status.tuning.value_or(false)) {
            emit tuneCancelRequested();
        } else {
            emit tuneRequested();
        }
    });

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
    // Bare digits carry no meaning to an AT reading a button out of its row
    // context (docs/a11y.md §1).
    m_ant1Btn->setAccessibleName(tr("Antenna port 1"));
    m_ant2Btn->setAccessibleName(tr("Antenna port 2"));
    m_antNumberLabel = new QLabel(this);
    theme.applyStyleSheet(m_antNumberLabel, telemetryLabelStyle());
    m_antNumberLabel->setAccessibleName(tr("Antenna number from the external switch"));
    m_antNumberLabel->hide();
    const QString antBtnStyle = makeStateBtnStyle({
        activeStateStyle(QStringLiteral("active"), QStringLiteral("{{color.background.1}}"),
                         QStringLiteral("{{color.accent}}"),
                         QStringLiteral("{{color.text.primary}}")),
    });
    for (auto* btn : {m_ant1Btn, m_ant2Btn}) {
        theme.applyStyleSheet(btn, antBtnStyle);
    }
    connect(m_ant1Btn, &QPushButton::clicked, this, [this]() { emit antennaSelected(1); });
    connect(m_ant2Btn, &QPushButton::clicked, this, [this]() { emit antennaSelected(2); });

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
    antRow->addWidget(m_antNumberLabel);
    antRow->addStretch();
    antRow->addWidget(m_clearFaultBtn);
    vbox->addLayout(antRow);

    // ── POWER / FAN / TRACK row ──────────────────────────────────────────
    m_powerBtn = makeSmallButton(QStringLiteral("POWER"), this);
    m_powerBtn->setAccessibleDescription(
        tr("Wakes the amplifier or puts it to sleep. The label shows the current state."));
    theme.applyStyleSheet(m_powerBtn, makeStateBtnStyle({
        activeStateStyle(QStringLiteral("on"), QStringLiteral("{{color.background.1}}"),
                         QStringLiteral("{{color.accent.success}}"),
                         QStringLiteral("{{color.text.primary}}")),
    }));
    connect(m_powerBtn, &QPushButton::clicked, this, [this]() {
        emit powerRequested(!m_status.powerOn.value_or(true));
    });

    auto* fanCaption = new QLabel(QStringLiteral("FAN MIN"), this);
    theme.applyStyleSheet(fanCaption, telemetryLabelStyle());
    m_fanCombo = new QComboBox(this);
    m_fanCombo->setObjectName(QStringLiteral("kpa1500FanMinimum"));
    m_fanCombo->setAccessibleName(tr("Minimum fan speed"));
    m_fanCombo->setAccessibleDescription(
        tr("Lowest speed the amplifier's fans run at, 0 (off) to 5 (full). The amplifier "
           "raises the speed further as it heats up."));
    for (int level = Kpa1500::kMinFan; level <= Kpa1500::kMaxFan; ++level) {
        m_fanCombo->addItem(QString::number(level), level);
    }
    m_fanCombo->setCurrentIndex(-1);
    connect(m_fanCombo, &QComboBox::activated, this, [this](int idx) {
        emit fanMinimumRequested(m_fanCombo->itemData(idx).toInt());
        refreshControls();  // the combo shows readback, not the click
    });

    m_followTx = preferences().value("FollowTx").toBool(false);
    m_trackBtn = makeSmallButton(QStringLiteral("TRACK"), this);
    m_trackBtn->setCheckable(true);
    m_trackBtn->setChecked(m_followTx);
    m_trackBtn->setAccessibleName(tr("Follow the TX slice frequency"));
    m_trackBtn->setAccessibleDescription(
        tr("Sends the transmit slice's frequency (split, XIT and FM offset included) to the "
           "amplifier so it changes band and recalls its tuner setting without transmitting. "
           "Changes wait until the radio stops transmitting."));
    theme.applyStyleSheet(m_trackBtn, makeStateBtnStyle({
        activeStateStyle(QStringLiteral("active"), QStringLiteral("{{color.background.1}}"),
                         QStringLiteral("{{color.accent}}"),
                         QStringLiteral("{{color.text.primary}}")),
    }));
    connect(m_trackBtn, &QPushButton::toggled, this, [this](bool on) {
        m_followTx = on;
        QJsonObject doc = preferences();
        doc["FollowTx"] = on;
        auto& settings = AppSettings::instance();
        settings.setValue("Kpa1500Applet",
                          QString::fromUtf8(QJsonDocument(doc).toJson(QJsonDocument::Compact)));
        settings.save();
        m_valuesDirty = true;
        refreshControls();
        emit followTxToggled(on);
    });

    auto* auxRow = new QHBoxLayout;
    auxRow->setSpacing(6);
    auxRow->addWidget(m_powerBtn);
    auxRow->addWidget(fanCaption);
    auxRow->addWidget(m_fanCombo);
    auxRow->addStretch();
    auxRow->addWidget(m_trackBtn);
    vbox->addLayout(auxRow);

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
        // The amp's own two-digit hex code, as the Programming Reference's
        // ^FL table and the front panel print it.
        m_faultLabel->setText(tr("Fault %1")
            .arg(QStringLiteral("%1").arg(fault, 2, 16, QLatin1Char('0')).toUpper()));
        m_faultLabel->show();
        m_clearFaultBtn->setVisible(m_connected);
    }

    refreshControls();
}

void Kpa1500Applet::refreshControls()
{
    // A lit [kpaState] variant outranks :disabled (see the operate note
    // below), so every lit state is gated on the amp being reachable AND awake.
    const bool asleep = m_status.powerOn == false;
    const bool live = m_connected && !asleep;
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
    setBtnState(m_operateBtn, operate && live ? QStringLiteral("operate") : QString());

    const bool atuIn = m_status.atuInline.value_or(false);
    m_atuInlineBtn->setText(atuIn ? QStringLiteral("ATU IN") : QStringLiteral("ATU BYP"));
    m_atuInlineBtn->setAccessibleName(atuIn ? tr("Tuner in line") : tr("Tuner bypassed"));
    setBtnState(m_atuInlineBtn, atuIn && live ? QStringLiteral("active") : QString());

    // Read-only reflection of the live `^AN` readback, not a latch of the
    // last button clicked.
    const int ant = m_status.antenna.value_or(0);
    setBtnState(m_ant1Btn, ant == 1 && live ? QStringLiteral("active") : QString());
    setBtnState(m_ant2Btn, ant == 2 && live ? QStringLiteral("active") : QString());
    const bool external = m_connected && ant > Kpa1500::kMaxAntenna;
    m_antNumberLabel->setText(external ? QStringLiteral("ANT %1").arg(ant) : QString());
    m_antNumberLabel->setVisible(external);

    const bool tuning = m_status.tuning.value_or(false);
    m_tuneBtn->setText(tuning ? QStringLiteral("TUNING") : QStringLiteral("TUNE"));
    const QString tuneName = tuning ? tr("Cancel antenna tune") : tr("Start antenna tuner");
    if (m_tuneBtn->accessibleName() != tuneName) {
        m_tuneBtn->setAccessibleName(tuneName);
        if (QAccessible::isActive()) {
            QAccessibleEvent event(m_tuneBtn, QAccessible::NameChanged);
            QAccessible::updateAccessibility(&event);
        }
    }
    setBtnState(m_tuneBtn, tuning && live ? QStringLiteral("active") : QString());

    // Asleep: only POWER (and TRACK, a local preference) can do anything.
    const QString offline = tr("Not connected to the amplifier");
    const QString sleeping = tr("The amplifier is asleep; press POWER to wake it");
    const QString reason = !m_connected ? offline : sleeping;
    for (QWidget* w : std::initializer_list<QWidget*>{m_operateBtn, m_tuneBtn, m_atuInlineBtn,
                                                      m_ant1Btn, m_ant2Btn}) {
        setAvailable(w, live, reason);
    }
    setAvailable(m_fanCombo, live && m_status.fanMinimum.has_value(),
                 live ? tr("Waiting for the amplifier to report its fan setting") : reason);
    {
        const QSignalBlocker block(m_fanCombo);
        m_fanCombo->setCurrentIndex(
            m_status.fanMinimum ? m_fanCombo->findData(*m_status.fanMinimum) : -1);
    }

    setAvailable(m_powerBtn, m_connected && m_status.powerOn.has_value(),
                 m_connected ? tr("Waiting for the amplifier to report its power state")
                             : offline);
    const bool on = m_status.powerOn.value_or(false);
    m_powerBtn->setText(asleep ? QStringLiteral("ASLEEP") : QStringLiteral("POWER"));
    m_powerBtn->setAccessibleName(asleep ? tr("Amplifier asleep, wake it")
                                         : tr("Amplifier on, put it to sleep"));
    setBtnState(m_powerBtn, on && m_connected ? QStringLiteral("on") : QString());

    setBtnState(m_trackBtn, m_followTx ? QStringLiteral("active") : QString());
}

void Kpa1500Applet::setConnected(bool connected)
{
    m_connected = connected;

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
        m_noticeTimer.stop();
        m_noticeLabel->hide();
        m_clearFaultBtn->hide();
        m_pwrGauge->setValueImmediate(0.0f);
        m_pwrGauge->clearPeak();
        m_refGauge->setValueImmediate(0.0f);
        m_swrGauge->setValueImmediate(1.0f);
        // Force the repaint so the placeholders replace the last readings now
        // rather than on the next tick.
        m_valuesDirty = true;
        updateValueLabels();
    }
    refreshControls();
}

void Kpa1500Applet::showControlUnconfirmed(const QString& control)
{
    m_noticeLabel->setText(tr("%1 not confirmed by the amplifier").arg(control));
    m_noticeLabel->setAccessibleName(m_noticeLabel->text());
    m_noticeLabel->show();
    m_noticeTimer.start();
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

    m_driveLabel->setText(m_status.inputWatts
        ? QStringLiteral("DRIVE  %1 W").arg(static_cast<int>(*m_status.inputWatts))
        : QStringLiteral("DRIVE  — W"));
    m_supplyLabel->setText(m_status.paVolts && m_status.paAmps
        ? QStringLiteral("PA  %1 V  %2 A").arg(*m_status.paVolts, 0, 'f', 1).arg(*m_status.paAmps)
        : QStringLiteral("PA  — V  — A"));
    m_fanLabel->setText(m_status.fanSpeed
        ? QStringLiteral("FAN  %1").arg(*m_status.fanSpeed)
        : QStringLiteral("FAN  —"));
    m_freqLabel->setText(QStringLiteral("FREQ  %1 kHz%2")
        .arg(m_status.frequencyKhz ? QString::number(*m_status.frequencyKhz) : QStringLiteral("—"),
             m_followTx ? QStringLiteral("  · TRACKING TX") : QString()));

    const QString atuDetail = m_status.atuSetting.value_or(QString());
    m_atuDetailLabel->setText(atuDetail);
    m_atuDetailLabel->setVisible(m_connected && !atuDetail.isEmpty());

    const int atten = m_status.attenuatorCode.value_or(0);
    m_attenLabel->setText(atten ? tr("Attenuator in: %1").arg(Kpa1500::attenuatorReason(atten))
                                : QString());
    m_attenLabel->setVisible(m_connected && atten != 0);

    QStringList identity;
    if (m_status.firmware) {
        identity << tr("Firmware %1").arg(*m_status.firmware);
    }
    if (m_status.serialNumber) {
        identity << tr("S/N %1").arg(*m_status.serialNumber);
    }
    m_statusPill->setToolTip(identity.join(QStringLiteral(" · ")));
}

}  // namespace AetherSDR
