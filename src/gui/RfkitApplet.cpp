#include "RfkitApplet.h"
#include "HGauge.h"
#include "MeterSmoother.h"
#include "core/ThemeManager.h"

#include <QAccessible>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QStyle>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace AetherSDR {

namespace {

// Fixed gauge full scales, the same as the amplifier's own display (Forward
// 0-1600 W, Reflected 0-200 W), so the bars read identically on both screens
// and never rescale. The device's max_value is deliberately not used: it
// reports the present power limit, not the meter range.
constexpr int kForwardMaxWatts = 1600;
constexpr int kReflectedMaxWatts = 200;
// Where the forward gauge turns red, as a fraction of its full scale.
constexpr float kRedZoneFraction = 0.85f;

QVector<HGauge::Tick> evenTicks(float max)
{
    QVector<HGauge::Tick> ticks;
    for (float frac : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
        const int v = static_cast<int>(max * frac);
        ticks.append({static_cast<float>(v), QString::number(v)});
    }
    return ticks;
}

// The amplifier's own SWR meter is not linear: 1, 1.5, 2, 3, 5, infinity sit at
// equal spacing, so detail is kept at the low end and a very bad match still
// moves the needle. The gauge here runs over those six positions (0..5) and
// every SWR is mapped onto it, so the applet reads like the amplifier's screen.
constexpr float kSwrGaugeMax = 5.0f;
constexpr float kSwrGaugeYellow = 1.0f;  // SWR 1.5
constexpr float kSwrGaugeRed = 2.0f;     // SWR 2

// Reflected power that corresponds to a given SWR at a given forward power:
// reflection coefficient (S-1)/(S+1), power ratio its square. So 1.5:1 is 4% of
// forward power and 2:1 is about 11%, whatever the power level.
float reflectedWattsAtSwr(float forwardWatts, float swr)
{
    const float gamma = (swr - 1.0f) / (swr + 1.0f);
    return forwardWatts * gamma * gamma;
}

float swrGaugePosition(float swr)
{
    if (!(swr > 1.0f)) { return 0.0f; }              // also catches NaN
    if (swr <= 1.5f) { return (swr - 1.0f) / 0.5f; }
    if (swr <= 2.0f) { return 1.0f + (swr - 1.5f) / 0.5f; }
    if (swr <= 3.0f) { return 2.0f + (swr - 2.0f); }
    if (swr <= 5.0f) { return 3.0f + (swr - 3.0f) / 2.0f; }
    return 4.0f + (1.0f - 5.0f / swr);               // approaches 5 at infinity
}

QLabel* makeValueLabel(QWidget* parent)
{
    auto* lbl = new QLabel(parent);
    // 72px: a kilowatt-class amp needs 4-digit readouts, the same width
    // AmpApplet and the other amplifier applets use.
    lbl->setFixedWidth(72);
    lbl->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    AetherSDR::ThemeManager::instance().applyStyleSheet(lbl,
        "QLabel { color: {{color.text.primary}}; font-size: 11px; font-weight: bold; }");
    return lbl;
}

// One named [rkState="..."] QSS variant, combined with neutralBtnStyle() into
// a single template applied ONCE per button — state changes then just toggle
// the dynamic property and repolish, instead of re-running applyStyleSheet()
// on every transition. Same pattern as VkampApplet/Kpa1500Applet.
QString activeStateStyle(const QString& state, const QString& bg,
                         const QString& border, const QString& fg)
{
    return QStringLiteral(
        "QPushButton[rkState=\"%1\"] { background: %2; border: 2px solid %3; border-radius: 4px; "
        "color: %4; font-size: 10px; font-weight: bold; } "
        "QPushButton[rkState=\"%1\"]:hover { background: %2; border: 2px solid %3; } "
        "QPushButton[rkState=\"%1\"]:pressed { background: %2; border: 2px solid %3; }")
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
    if (btn->property("rkState").toString() == state) { return; }
    btn->setProperty("rkState", state);
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

RfkitApplet::RfkitApplet(QWidget* parent)
    : QWidget(parent)
{
    theme::setContainer(this, QStringLiteral("applet/rfkit"));
    auto* vbox = new QVBoxLayout(this);
    vbox->setContentsMargins(4, 2, 4, 2);
    vbox->setSpacing(2);

    auto& theme = AetherSDR::ThemeManager::instance();

    // ── Header: device name + status pill ────────────────────────────────
    m_nameLabel = new QLabel(this);
    m_nameLabel->setAccessibleName(tr("Amplifier name"));
    theme.applyStyleSheet(m_nameLabel,
        "QLabel { color: {{color.text.label}}; font-size: 10px; font-weight: bold; }");
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
    headerRow->addWidget(m_nameLabel, 1);
    headerRow->addWidget(m_statusPill);
    vbox->addLayout(headerRow);

    // ── PWR / REF / SWR gauges ───────────────────────────────────────────
    m_pwrLabel = makeValueLabel(this);
    m_pwrLabel->setText(QStringLiteral("PWR"));
    m_pwrGauge = new HGauge(0.0f, static_cast<float>(kForwardMaxWatts),
                            kForwardMaxWatts * kRedZoneFraction, QString(), QString(),
                            evenTicks(static_cast<float>(kForwardMaxWatts)), this);
    m_pwrGauge->setWindowPeakEnabled(true);
    m_pwrGauge->setAccessibleName(tr("Forward power"));
    auto* pwrRow = new QHBoxLayout;
    pwrRow->setSpacing(4);
    pwrRow->addWidget(m_pwrLabel);
    pwrRow->addWidget(m_pwrGauge, 1);
    vbox->addLayout(pwrRow);

    m_refLabel = makeValueLabel(this);
    m_refLabel->setText(QStringLiteral("REF"));
    m_refGauge = new HGauge(0.0f, static_cast<float>(kReflectedMaxWatts), kReflectedMaxWatts / 2.0f,
                            QString(), QString(), evenTicks(static_cast<float>(kReflectedMaxWatts)), this);
    m_refGauge->setAccessibleName(tr("Reflected power"));
    m_refGauge->setCautionColor(theme.color(this, QStringLiteral("color.accent.warning")));
    auto* refRow = new QHBoxLayout;
    refRow->setSpacing(4);
    refRow->addWidget(m_refLabel);
    refRow->addWidget(m_refGauge, 1);
    vbox->addLayout(refRow);

    m_swrLabel = makeValueLabel(this);
    m_swrLabel->setText(QStringLiteral("SWR"));
    m_swrGauge = new HGauge(0.0f, kSwrGaugeMax, kSwrGaugeRed, QString(), QString(),
                            {{0.0f, QStringLiteral("1")},   {1.0f, QStringLiteral("1.5")},
                             {2.0f, QStringLiteral("2")},   {3.0f, QStringLiteral("3")},
                             {4.0f, QStringLiteral("5")},   {5.0f, QString::fromUtf8("\xe2\x88\x9e")}},
                            this, kSwrGaugeYellow);
    m_swrGauge->setAccessibleName(tr("SWR"));
    m_swrGauge->setCautionColor(theme.color(this, QStringLiteral("color.accent.warning")));
    // The caution colour is a paint-code colour, so the theme does not repaint it
    // for us: follow a theme change, or a custom theme would go stale until restart.
    connect(&AetherSDR::ThemeManager::instance(), &AetherSDR::ThemeManager::themeChanged, this, [this]() {
        const QColor caution = AetherSDR::ThemeManager::instance().color(
            this, QStringLiteral("color.accent.warning"));
        m_refGauge->setCautionColor(caution);
        m_swrGauge->setCautionColor(caution);
    });
    auto* swrRow = new QHBoxLayout;
    swrRow->setSpacing(4);
    swrRow->addWidget(m_swrLabel);
    swrRow->addWidget(m_swrGauge, 1);
    vbox->addLayout(swrRow);

    vbox->addSpacing(4);

    // ── Info grid: temp / supply / current, band / frequency / interface ──
    auto makeInfo = [this, &theme](const QString& text) {
        auto* lbl = new QLabel(text, this);
        theme.applyStyleSheet(lbl, telemetryLabelStyle());
        return lbl;
    };
    m_tempLabel    = makeInfo(QStringLiteral("TEMP  — C"));
    m_voltsLabel   = makeInfo(QStringLiteral("SUPPLY  — V"));
    m_currentLabel = makeInfo(QStringLiteral("CURR  — A"));
    m_bandLabel    = makeInfo(QStringLiteral("BAND  —"));
    m_freqLabel    = makeInfo(QStringLiteral("FREQ  —"));
    m_ifaceLabel   = makeInfo(QStringLiteral("IF  —"));

    auto* infoGrid = new QGridLayout;
    infoGrid->setHorizontalSpacing(12);
    infoGrid->setVerticalSpacing(2);
    infoGrid->addWidget(m_tempLabel,    0, 0);
    infoGrid->addWidget(m_voltsLabel,   0, 1);
    infoGrid->addWidget(m_currentLabel, 0, 2);
    infoGrid->addWidget(m_bandLabel,    1, 0);
    infoGrid->addWidget(m_freqLabel,    1, 1);
    infoGrid->addWidget(m_ifaceLabel,   1, 2);
    vbox->addLayout(infoGrid);

    // Tuner status line: read-only (the API has no tuner control).
    m_tunerLabel = makeInfo(QStringLiteral("TUNER  —"));
    m_tunerLabel->setWordWrap(true);
    m_tunerLabel->setAccessibleName(tr("Tuner status"));
    vbox->addWidget(m_tunerLabel);

    // The device's own free-text status, as it reports it.
    m_deviceStatusLabel = makeInfo(QString());
    m_deviceStatusLabel->setWordWrap(true);
    m_deviceStatusLabel->setAccessibleName(tr("Amplifier status text"));
    m_deviceStatusLabel->hide();
    vbox->addWidget(m_deviceStatusLabel);

    vbox->addSpacing(4);

    // ── Fault banner (own row, only shown when the device reports an error) ─
    m_faultLabel = new QLabel(this);
    m_faultLabel->setWordWrap(true);
    theme.applyStyleSheet(m_faultLabel,
        "QLabel { color: {{color.accent.danger}}; font-size: 10px; font-weight: bold; }");
    m_faultLabel->hide();
    vbox->addWidget(m_faultLabel);

    // ── OPERATE / RESET ERR row ──────────────────────────────────────────
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
        // Asks; the label only moves when the device's own readback confirms
        // it (Principle II — no optimistic latch).
        const bool operate = m_status.operate
            && *m_status.operate == Rfkit::OperateMode::Operate;
        emit operateToggled(!operate);
    });

    // Read-only indicator: the API has no tuner control, so this shows the
    // device's reported tuner mode and cannot be pressed.
    m_tunerBtn = makeSmallButton(QStringLiteral("TUNER —"), this);
    m_tunerBtn->setFocusPolicy(Qt::NoFocus);
    m_tunerBtn->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_tunerBtn->setAccessibleName(tr("Tuner state"));
    theme.applyStyleSheet(m_tunerBtn, makeStateBtnStyle({
        activeStateStyle(QStringLiteral("tuner"), QStringLiteral("{{color.background.0}}"),
                         QStringLiteral("{{color.accent.success}}"),
                         QStringLiteral("{{color.accent.success}}")),
        activeStateStyle(QStringLiteral("tunerOff"), QStringLiteral("{{color.background.0}}"),
                         QStringLiteral("{{color.accent.danger}}"),
                         QStringLiteral("{{color.accent.danger}}")),
    }));

    m_resetErrorBtn = makeSmallButton(QStringLiteral("RESET ERR"), this);
    m_resetErrorBtn->setAccessibleName(tr("Reset amplifier error"));
    m_resetErrorBtn->setAccessibleDescription(
        tr("Asks the amplifier to clear its error state. Confirms first."));
    theme.applyStyleSheet(m_resetErrorBtn, neutralBtnStyle());
    connect(m_resetErrorBtn, &QPushButton::clicked, this, [this]() {
        // An error latch exists to protect the hardware; make the operator
        // confirm the cause is dealt with rather than clearing it on a stray
        // click. Same pattern as VkampApplet's reset confirm.
        const auto reply = QMessageBox::question(this, tr("Reset Amplifier Error"),
            tr("Clear the RF-KIT's error state? Make sure the cause of the error has been "
               "resolved first."),
            QMessageBox::Yes | QMessageBox::No);
        if (reply == QMessageBox::Yes) {
            emit errorResetRequested();
        }
    });

    auto* ctrlRow = new QHBoxLayout;
    ctrlRow->setSpacing(6);
    ctrlRow->addWidget(m_operateBtn);
    ctrlRow->addWidget(m_tunerBtn);
    ctrlRow->addStretch();
    ctrlRow->addWidget(m_resetErrorBtn);
    vbox->addLayout(ctrlRow);

    // ── Antenna row ──────────────────────────────────────────────────────
    auto* antLabel = new QLabel(QStringLiteral("ANT"), this);
    theme.applyStyleSheet(antLabel, telemetryLabelStyle());
    const QString antBtnStyle = makeStateBtnStyle({
        activeStateStyle(QStringLiteral("active"), QStringLiteral("{{color.background.1}}"),
                         QStringLiteral("{{color.accent}}"),
                         QStringLiteral("{{color.text.primary}}")),
    });
    auto* antRow = new QHBoxLayout;
    antRow->setSpacing(6);
    antRow->addWidget(antLabel);
    for (int n = Rfkit::kMinInternalAntenna; n <= Rfkit::kMaxInternalAntenna; ++n) {
        auto* btn = makeSmallButton(QString::number(n), this);
        // Bare digits carry no meaning to an AT reading a button out of its row
        // context (docs/a11y.md §1).
        btn->setAccessibleName(tr("Antenna %1").arg(n));
        theme.applyStyleSheet(btn, antBtnStyle);
        // Read-only display, not an optimistic latch: clicking just asks, and
        // the highlight only moves once the device's list confirms it.
        connect(btn, &QPushButton::clicked, this, [this, n]() {
            emit antennaSelected(Rfkit::Antenna{Rfkit::AntennaType::Internal, n});
        });
        m_antBtns.append(btn);
        antRow->addWidget(btn);
    }
    // The external antenna is shown, not selectable: nobody has yet seen what
    // the amplifier does with a request to switch to it, and it moves the RF
    // path. Same treatment as the tuner and the operational interface.
    m_extAntBtn = makeSmallButton(QStringLiteral("EXT"), this);
    m_extAntBtn->setFocusPolicy(Qt::NoFocus);
    m_extAntBtn->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_extAntBtn->setAccessibleName(tr("External antenna"));
    m_extAntBtn->setAccessibleDescription(
        tr("Shows whether the amplifier's external antenna is in use. It cannot be selected "
           "from AetherSDR."));
    theme.applyStyleSheet(m_extAntBtn, antBtnStyle);
    antRow->addWidget(m_extAntBtn);
    antRow->addStretch();
    vbox->addLayout(antRow);

    m_labelTimer.setInterval(kMeterReadoutUpdateMs);
    connect(&m_labelTimer, &QTimer::timeout, this, &RfkitApplet::updateValueLabels);
    m_labelTimer.start();

    setConnected(false);
}

void RfkitApplet::updateReflectedZones(float forwardWatts)
{
    const float refMax = static_cast<float>(kReflectedMaxWatts);
    // Rounded to 0.1 W so a wobbling forward power does not rebuild the gauge
    // on every reply.
    const float red = std::min(refMax, std::round(reflectedWattsAtSwr(forwardWatts, 2.0f) * 10.0f) / 10.0f);
    const float yellow = std::min(red, std::round(reflectedWattsAtSwr(forwardWatts, 1.5f) * 10.0f) / 10.0f);
    if (red == m_refRedW && yellow == m_refYellowW) {
        return;
    }
    m_refRedW = red;
    m_refYellowW = yellow;
    m_refGauge->setRange(0.0f, refMax, red, evenTicks(refMax), yellow);
}

void RfkitApplet::setStatus(const Rfkit::Status& status)
{
    m_status = status;
    m_valuesDirty = true;

    if (status.power) {
        const float fwd = static_cast<float>(status.power->forward.value);
        if (fwd >= 1.0f) {
            updateReflectedZones(fwd);
        }
        m_pwrGauge->setValue(fwd);
        // Reflected power and SWR only mean anything alongside forward drive —
        // hold both at baseline below 1W, the same gate the other amplifier
        // applets use, so an idle amp does not paint a meaningless SWR.
        m_refGauge->setValue(fwd >= 1.0f ? static_cast<float>(status.power->reflected.value) : 0.0f);
        m_swrGauge->setValue(fwd >= 1.0f
                             ? swrGaugePosition(static_cast<float>(status.power->swr.value)) : 0.0f);
    }

    const QString error = status.interfaceState ? status.interfaceState->error : QString();
    if (error.isEmpty()) {
        m_faultLabel->hide();
        m_faultLabel->clear();
    } else {
        m_faultLabel->setText(tr("Error: %1").arg(error));
        m_faultLabel->show();
    }

    refreshControls();
    refreshAntennas();
}

void RfkitApplet::refreshControls()
{
    const bool known = m_status.operate.has_value();
    const bool operate = known && *m_status.operate == Rfkit::OperateMode::Operate;
    // Until /operate-mode has answered the state is unknown: show "—" and keep
    // the button off, so a click cannot send OPERATE on a guess.
    m_operateBtn->setText(!known ? QStringLiteral("—")
                          : operate ? QStringLiteral("OPERATE") : QStringLiteral("STANDBY"));
    m_operateBtn->setEnabled(m_connected && known);
    // The label is a STATE, not an action, so spell that out for an AT rather
    // than leaving it to infer a verb (docs/a11y.md §1). Announce a change
    // only when the name actually changed: this runs on every status update.
    const QString a11yName = !known ? tr("Amplifier state unknown")
                             : operate ? tr("Amplifier in operate") : tr("Amplifier in standby");
    if (m_operateBtn->accessibleName() != a11yName) {
        m_operateBtn->setAccessibleName(a11yName);
        if (QAccessible::isActive()) {
            QAccessibleEvent event(m_operateBtn, QAccessible::NameChanged);
            QAccessible::updateAccessibility(&event);
        }
    }
    // The m_connected guard is load-bearing, not cosmetic: a [rkState] rule and
    // QPushButton:disabled are equal CSS2 specificity and the variant is
    // appended last, so source order would paint a DISCONNECTED amp in
    // full-brightness "OPERATE" green. Same trap VkampApplet documents.
    setBtnState(m_operateBtn, operate && m_connected ? QStringLiteral("operate") : QString());
}

void RfkitApplet::refreshAntennas()
{
    const bool listKnown = m_status.antennas.has_value();
    const QVector<Rfkit::AntennaWithState> list = m_status.antennas.value_or(QVector<Rfkit::AntennaWithState>());

    auto find = [&list](Rfkit::AntennaType type, int number) -> const Rfkit::AntennaWithState* {
        for (const auto& item : list) {
            if (item.antenna.type == type && item.antenna.number == number) {
                return &item;
            }
        }
        return nullptr;
    };

    for (int i = 0; i < m_antBtns.size(); ++i) {
        QPushButton* btn = m_antBtns.at(i);
        const int number = Rfkit::kMinInternalAntenna + i;
        const auto* item = find(Rfkit::AntennaType::Internal, number);
        // Every button stays visible; one the amplifier does not list (or that
        // it has not listed yet) is dimmed, with the reason on hover.
        btn->setEnabled(m_connected && item && item->state != Rfkit::AntennaState::Disabled);
        btn->setToolTip(item ? QString()
                        : listKnown ? tr("Not reported by the amplifier") : tr("Waiting for the amplifier's antenna list"));
        const bool active = m_connected && item && item->state == Rfkit::AntennaState::Active;
        setBtnState(btn, active ? QStringLiteral("active") : QString());
    }

    // Read-only external indicator: listed by the amplifier, lit while it
    // reports the external antenna active, with the switch port when it gives
    // one ("EXT 16").
    const Rfkit::AntennaWithState* ext = nullptr;
    for (const auto& item : list) {
        if (item.antenna.type == Rfkit::AntennaType::External) {
            ext = &item;
            break;
        }
    }
    const bool extActive = m_connected && ext && ext->state == Rfkit::AntennaState::Active;
    // Always shown; dimmed with a reason when the amplifier does not list it.
    m_extAntBtn->setEnabled(m_connected && ext != nullptr);
    m_extAntBtn->setToolTip(ext ? QString()
                            : listKnown ? tr("Not reported by the amplifier")
                                        : tr("Waiting for the amplifier's antenna list"));
    m_extAntBtn->setText(extActive && ext->externalPort > 0
                             ? QStringLiteral("EXT %1").arg(ext->externalPort)
                             : QStringLiteral("EXT"));
    setBtnState(m_extAntBtn, extActive ? QStringLiteral("active") : QString());
}

void RfkitApplet::setConnected(bool connected)
{
    m_connected = connected;
    m_resetErrorBtn->setEnabled(connected);
    if (!connected) {
        m_tunerBtn->setText(QStringLiteral("TUNER —"));
        setBtnState(m_tunerBtn, QString());
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
        // Drop every remembered reading. An unset optional renders as "—", so
        // a disconnected amp reads as "no data" rather than as a confident
        // zero that happens to be the last thing it said.
        m_status = {};
        m_faultLabel->hide();
        m_faultLabel->clear();
        m_pwrGauge->setValueImmediate(0.0f);
        m_pwrGauge->clearPeak();
        m_refGauge->setValueImmediate(0.0f);
        m_swrGauge->setValueImmediate(0.0f);
        // Force the repaint so the placeholders replace the last readings now
        // rather than on the next tick.
        m_valuesDirty = true;
    }
    refreshControls();
    refreshAntennas();
    if (!connected) {
        updateValueLabels();
    }
}

void RfkitApplet::updateValueLabels()
{
    const bool havePower = m_status.power.has_value();
    const float fwd = havePower ? static_cast<float>(m_status.power->forward.value) : 0.0f;
    m_pwrLabel->setText(fwd >= 1.0f
        ? QStringLiteral("PWR  %1").arg(static_cast<int>(fwd))
        : QStringLiteral("PWR"));
    m_refLabel->setText(fwd >= 1.0f
        ? QStringLiteral("REF  %1").arg(m_status.power->reflected.value)
        : QStringLiteral("REF"));
    m_swrLabel->setText(fwd >= 1.0f
        ? QStringLiteral("SWR %1:1").arg(m_status.power->swr.value, 0, 'f', 2)
        : QStringLiteral("SWR"));

    if (!m_valuesDirty) {
        return;
    }
    m_valuesDirty = false;

    const QString name = m_status.info
        ? (m_status.info->customName.isEmpty() ? m_status.info->device : m_status.info->customName)
        : QString();
    m_nameLabel->setText(name);

    if (havePower) {
        const auto& p = *m_status.power;
        m_tempLabel->setText(QStringLiteral("TEMP  %1C").arg(static_cast<int>(Rfkit::temperatureCelsius(p.temperature))));
        m_voltsLabel->setText(QStringLiteral("SUPPLY  %1V").arg(p.voltage.value, 0, 'f', 1));
        m_currentLabel->setText(QStringLiteral("CURR  %1A").arg(p.current.value, 0, 'f', 1));
    } else {
        m_tempLabel->setText(QStringLiteral("TEMP  — C"));
        m_voltsLabel->setText(QStringLiteral("SUPPLY  — V"));
        m_currentLabel->setText(QStringLiteral("CURR  — A"));
    }

    if (m_status.data) {
        m_bandLabel->setText(QStringLiteral("BAND  %1").arg(Rfkit::formatBand(m_status.data->band)));
        m_freqLabel->setText(QStringLiteral("FREQ  %1").arg(Rfkit::formatFrequency(m_status.data->frequency)));
        const QString text = m_status.data->status.trimmed();
        m_deviceStatusLabel->setText(tr("STATUS  %1").arg(text));
        m_deviceStatusLabel->setVisible(!text.isEmpty());
    } else {
        m_bandLabel->setText(QStringLiteral("BAND  —"));
        m_freqLabel->setText(QStringLiteral("FREQ  —"));
        m_deviceStatusLabel->clear();
        m_deviceStatusLabel->hide();
    }

    m_ifaceLabel->setText(QStringLiteral("IF  %1").arg(m_status.interfaceState
        ? Rfkit::interfaceLabel(m_status.interfaceState->active) : QStringLiteral("—")));

    if (m_status.tuner) {
        const auto& t = *m_status.tuner;
        QString text = QStringLiteral("TUNER  %1  %2")
            .arg(Rfkit::tunerModeLabel(t.mode), Rfkit::tunerSetupLabel(t.setup));
        if (t.l) {
            text += QStringLiteral("  L %1%2").arg(t.l->value).arg(t.l->unit.isEmpty() ? QString() : QStringLiteral(" ") + t.l->unit);
        }
        if (t.c) {
            text += QStringLiteral("  C %1%2").arg(t.c->value).arg(t.c->unit.isEmpty() ? QString() : QStringLiteral(" ") + t.c->unit);
        }
        m_tunerLabel->setText(text);

        // Enabled = the tuner is in the signal path (manual, auto or mid-tune);
        // disabled = the amplifier reports it off or bypassed.
        const bool known = t.mode != Rfkit::TunerMode::Unknown;
        const bool enabled = t.mode == Rfkit::TunerMode::Manual
            || t.mode == Rfkit::TunerMode::Auto
            || t.mode == Rfkit::TunerMode::AutoTuning;
        m_tunerBtn->setText(!known ? QStringLiteral("TUNER —")
                            : enabled ? QStringLiteral("TUNER ON") : QStringLiteral("TUNER OFF"));
        m_tunerBtn->setAccessibleName(!known ? tr("Tuner state unknown")
                                      : enabled ? tr("Tuner enabled") : tr("Tuner disabled"));
        setBtnState(m_tunerBtn, !known || !m_connected ? QString()
                    : enabled ? QStringLiteral("tuner") : QStringLiteral("tunerOff"));
    } else {
        m_tunerLabel->setText(QStringLiteral("TUNER  —"));
        m_tunerBtn->setText(QStringLiteral("TUNER —"));
        setBtnState(m_tunerBtn, QString());
    }
}

}  // namespace AetherSDR
