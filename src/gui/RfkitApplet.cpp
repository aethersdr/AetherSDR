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
// on every transition. Same pattern as VkampApplet/Kpa1500Applet. `widget` is
// the QSS type selector: the read-only indicators are QLabels styled to match
// the buttons beside them.
QString activeStateStyle(const QString& state, const QString& bg,
                         const QString& border, const QString& fg,
                         const QString& widget = QStringLiteral("QPushButton"))
{
    return QStringLiteral(
        "%5[rkState=\"%1\"] { background: %2; border: 2px solid %3; border-radius: 4px; "
        "color: %4; font-size: 10px; font-weight: bold; } "
        "%5[rkState=\"%1\"]:hover { background: %2; border: 2px solid %3; } "
        "%5[rkState=\"%1\"]:pressed { background: %2; border: 2px solid %3; }")
        .arg(state, bg, border, fg, widget);
}

QString neutralBtnStyle(const QString& widget = QStringLiteral("QPushButton"))
{
    return QStringLiteral(
        "%1 { background: {{color.background.2}}; border: 1px solid {{color.background.2}}; "
        "border-radius: 3px; color: {{color.text.primary}}; font-size: 10px; font-weight: bold; }"
        "%1:hover { background: {{color.background.1}}; }"
        "%1:disabled { background: {{color.background.0}}; border: 1px solid {{color.border.subtle}}; "
        "color: {{color.text.disabled}}; }").arg(widget);
}

QString makeStateBtnStyle(std::initializer_list<QString> stateVariants,
                          const QString& widget = QStringLiteral("QPushButton"))
{
    QString sheet = neutralBtnStyle(widget);
    for (const auto& variant : stateVariants) {
        sheet += variant;
    }
    return sheet;
}

void setBtnState(QWidget* btn, const QString& state)
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

// A read-only indicator (tuner, external antenna): a QLabel, so an AT reads it
// as text rather than offering a press that does nothing. It takes the height of
// the button row it sits in, so it matches the buttons beside it.
QLabel* makeIndicator(const QString& text, QWidget* parent)
{
    auto* lbl = new QLabel(text, parent);
    lbl->setAlignment(Qt::AlignCenter);
    lbl->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);
    lbl->setContentsMargins(6, 0, 6, 0);
    return lbl;
}

// Set an accessible name and announce it, only when it actually changed: the
// callers run on every status update, and docs/a11y.md asks for settled values.
void setAnnouncedName(QWidget* widget, const QString& name)
{
    if (widget->accessibleName() == name) {
        return;
    }
    widget->setAccessibleName(name);
    if (QAccessible::isActive()) {
        QAccessibleEvent event(widget, QAccessible::NameChanged);
        QAccessible::updateAccessibility(&event);
    }
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
    const QString indicator = QStringLiteral("QLabel");
    m_tunerIndicator = makeIndicator(QStringLiteral("TUNER —"), this);
    m_tunerIndicator->setAccessibleName(tr("Tuner state unknown"));
    m_tunerIndicator->setAccessibleDescription(
        tr("Shows whether the amplifier's tuner is in the signal path. It cannot be changed "
           "from AetherSDR."));
    theme.applyStyleSheet(m_tunerIndicator, makeStateBtnStyle({
        activeStateStyle(QStringLiteral("tuner"), QStringLiteral("{{color.background.0}}"),
                         QStringLiteral("{{color.accent.success}}"),
                         QStringLiteral("{{color.accent.success}}"), indicator),
        activeStateStyle(QStringLiteral("tunerOff"), QStringLiteral("{{color.background.0}}"),
                         QStringLiteral("{{color.accent.danger}}"),
                         QStringLiteral("{{color.accent.danger}}"), indicator),
    }, indicator));

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
    ctrlRow->addWidget(m_tunerIndicator);
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
    const QString extIndicatorStyle = makeStateBtnStyle({
        activeStateStyle(QStringLiteral("active"), QStringLiteral("{{color.background.1}}"),
                         QStringLiteral("{{color.accent}}"),
                         QStringLiteral("{{color.text.primary}}"), indicator),
    }, indicator);
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
    m_extIndicator = makeIndicator(QStringLiteral("EXT"), this);
    m_extIndicator->setAccessibleName(tr("External antenna"));
    m_extIndicator->setAccessibleDescription(extIndicatorDescription());
    theme.applyStyleSheet(m_extIndicator, extIndicatorStyle);
    antRow->addWidget(m_extIndicator);
    antRow->addStretch();
    vbox->addLayout(antRow);

    m_labelTimer.setInterval(kMeterReadoutUpdateMs);
    connect(&m_labelTimer, &QTimer::timeout, this, &RfkitApplet::updateValueLabels);
    // Started by setConnected(true): with no amplifier there is nothing to show.

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
    m_refGauge->setZones(red, yellow);
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
    setAnnouncedName(m_operateBtn, !known ? tr("Amplifier state unknown")
                                   : operate ? tr("Amplifier in operate") : tr("Amplifier in standby"));
    // The m_connected guard is load-bearing, not cosmetic: a [rkState] rule and
    // QPushButton:disabled are equal CSS2 specificity and the variant is
    // appended last, so source order would paint a DISCONNECTED amp in
    // full-brightness "OPERATE" green. Same trap VkampApplet documents.
    setBtnState(m_operateBtn, operate && m_connected ? QStringLiteral("operate") : QString());
}

QString RfkitApplet::extIndicatorDescription() const
{
    return tr("Shows whether the amplifier's external antenna is in use. It cannot be selected "
              "from AetherSDR.");
}

void RfkitApplet::refreshAntennas()
{
    const bool listKnown = m_status.antennas.has_value();
    const QVector<Rfkit::AntennaWithState> list = m_status.antennas.value_or(QVector<Rfkit::AntennaWithState>());
    // Why a port is dimmed, on hover; empty when it is not dimmed.
    const auto dimReason = [this, listKnown](bool listed, bool disabled) -> QString {
        if (!m_connected) { return tr("Not connected"); }
        if (!listKnown) { return tr("Waiting for the amplifier's antenna list"); }
        if (!listed) { return tr("Not reported by the amplifier"); }
        if (disabled) { return tr("Disabled on the amplifier"); }
        return QString();
    };

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
        const bool disabled = item && item->state == Rfkit::AntennaState::Disabled;
        btn->setEnabled(m_connected && item && !disabled);
        // The reason goes on the accessible channel too: an AT never sees a tooltip.
        const QString reason = dimReason(item != nullptr, disabled);
        btn->setToolTip(reason);
        btn->setAccessibleDescription(reason);
        const bool active = m_connected && item && item->state == Rfkit::AntennaState::Active;
        setBtnState(btn, active ? QStringLiteral("active") : QString());
        // The highlight is colour only; say it for an AT too.
        setAnnouncedName(btn, active ? tr("Antenna %1, in use").arg(number) : tr("Antenna %1").arg(number));
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
    m_extIndicator->setEnabled(m_connected && ext != nullptr);
    const QString extReason = dimReason(ext != nullptr, false);
    m_extIndicator->setToolTip(extReason);
    m_extIndicator->setAccessibleDescription(extReason.isEmpty() ? extIndicatorDescription()
                                             : extIndicatorDescription() + QLatin1Char(' ') + extReason);
    m_extIndicator->setText(extActive && ext->externalPort > 0
                             ? QStringLiteral("EXT %1").arg(ext->externalPort)
                             : QStringLiteral("EXT"));
    setBtnState(m_extIndicator, extActive ? QStringLiteral("active") : QString());
    setAnnouncedName(m_extIndicator, !extActive ? tr("External antenna, not in use")
                     : ext->externalPort > 0 ? tr("External antenna, port %1, in use").arg(ext->externalPort)
                                             : tr("External antenna, in use"));
}

void RfkitApplet::setConnected(bool connected)
{
    m_connected = connected;
    m_resetErrorBtn->setEnabled(connected);
    if (connected) {
        m_labelTimer.start();
    } else {
        m_labelTimer.stop();
        m_tunerIndicator->setText(QStringLiteral("TUNER —"));
        setAnnouncedName(m_tunerIndicator, tr("Tuner state unknown"));
        setBtnState(m_tunerIndicator, QString());
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
        m_tunerIndicator->setText(!known ? QStringLiteral("TUNER —")
                            : enabled ? QStringLiteral("TUNER ON") : QStringLiteral("TUNER OFF"));
        setAnnouncedName(m_tunerIndicator, !known ? tr("Tuner state unknown")
                                     : enabled ? tr("Tuner enabled") : tr("Tuner disabled"));
        setBtnState(m_tunerIndicator, !known || !m_connected ? QString()
                    : enabled ? QStringLiteral("tuner") : QStringLiteral("tunerOff"));
    } else {
        m_tunerLabel->setText(QStringLiteral("TUNER  —"));
        m_tunerIndicator->setText(QStringLiteral("TUNER —"));
        setAnnouncedName(m_tunerIndicator, tr("Tuner state unknown"));
        setBtnState(m_tunerIndicator, QString());
    }
}

}  // namespace AetherSDR
