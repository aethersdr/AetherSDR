#include "RfkitApplet.h"
#include "HGauge.h"
#include "MeterSmoother.h"
#include "core/ThemeManager.h"

#include <QAccessible>
#include <QComboBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QStyle>
#include <QVBoxLayout>

#include <algorithm>

namespace AetherSDR {

namespace {

// Gauge full scale used until the device reports its own max_value on the
// first /power reply, and again if a firmware omits it. Only a starting point:
// the applet re-scales to whatever the device says.
constexpr int kDefaultForwardMaxWatts = 1500;
// Where the forward gauge turns red, as a fraction of its full scale.
constexpr float kRedZoneFraction = 0.85f;
// Reflected axis as a fraction of the forward axis when the device does not
// report its own: a full-scale reflected bar the amp can never reach is
// wasted. Same 15% convention the other amplifier applets use.
constexpr float kReflectedFraction = 0.15f;

int defaultReflectedMax(int forwardMax)
{
    return std::max(1, static_cast<int>(forwardMax * kReflectedFraction));
}

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

    m_gaugeFwdMax = kDefaultForwardMaxWatts;
    m_gaugeRefMax = defaultReflectedMax(kDefaultForwardMaxWatts);

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
    m_pwrGauge = new HGauge(0.0f, static_cast<float>(m_gaugeFwdMax),
                            m_gaugeFwdMax * kRedZoneFraction, QString(), QString(),
                            evenTicks(static_cast<float>(m_gaugeFwdMax)), this);
    m_pwrGauge->setWindowPeakEnabled(true);
    m_pwrGauge->setAccessibleName(tr("Forward power"));
    auto* pwrRow = new QHBoxLayout;
    pwrRow->setSpacing(4);
    pwrRow->addWidget(m_pwrLabel);
    pwrRow->addWidget(m_pwrGauge, 1);
    vbox->addLayout(pwrRow);

    m_refLabel = makeValueLabel(this);
    m_refLabel->setText(QStringLiteral("REF"));
    m_refGauge = new HGauge(0.0f, static_cast<float>(m_gaugeRefMax), m_gaugeRefMax / 2.0f,
                            QString(), QString(), evenTicks(static_cast<float>(m_gaugeRefMax)), this);
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
    m_extAntCombo = new QComboBox(this);
    m_extAntCombo->setAccessibleName(tr("External antenna"));
    m_extAntCombo->setAccessibleDescription(
        tr("Selects one of the amplifier's external antenna ports."));
    theme.applyStyleSheet(m_extAntCombo,
        "QComboBox { background: {{color.background.2}}; color: {{color.text.primary}}; "
        "border: 1px solid {{color.border.subtle}}; border-radius: 3px; font-size: 10px; padding: 1px 4px; }"
        "QComboBox:disabled { color: {{color.text.disabled}}; }");
    m_extAntCombo->hide();
    connect(m_extAntCombo, &QComboBox::activated, this, [this](int index) {
        const int number = m_extAntCombo->itemData(index).toInt();
        if (number >= Rfkit::kMinExternalAntenna && number <= Rfkit::kMaxExternalAntenna) {
            emit antennaSelected(Rfkit::Antenna{Rfkit::AntennaType::External, number});
        }
        // Snap back to what the device last reported until it confirms.
        refreshAntennas();
    });
    antRow->addWidget(m_extAntCombo);
    antRow->addStretch();
    vbox->addLayout(antRow);

    m_labelTimer.setInterval(kMeterReadoutUpdateMs);
    connect(&m_labelTimer, &QTimer::timeout, this, &RfkitApplet::updateValueLabels);
    m_labelTimer.start();

    setConnected(false);
}

void RfkitApplet::updateGaugeScale()
{
    if (!m_status.power) {
        return;
    }
    const int fwdMax = m_status.power->forward.maxValue > 0
        ? m_status.power->forward.maxValue : kDefaultForwardMaxWatts;
    const int refMax = m_status.power->reflected.maxValue > 0
        ? m_status.power->reflected.maxValue : defaultReflectedMax(fwdMax);
    if (fwdMax != m_gaugeFwdMax) {
        m_gaugeFwdMax = fwdMax;
        m_pwrGauge->setRange(0.0f, static_cast<float>(fwdMax), fwdMax * kRedZoneFraction,
                             evenTicks(static_cast<float>(fwdMax)));
    }
    if (refMax != m_gaugeRefMax) {
        m_gaugeRefMax = refMax;
        m_refGauge->setRange(0.0f, static_cast<float>(refMax), refMax / 2.0f,
                             evenTicks(static_cast<float>(refMax)));
    }
}

void RfkitApplet::setStatus(const Rfkit::Status& status)
{
    m_status = status;
    m_valuesDirty = true;

    if (status.power) {
        updateGaugeScale();
        const float fwd = static_cast<float>(status.power->forward.value);
        m_pwrGauge->setValue(fwd);
        // Reflected power and SWR only mean anything alongside forward drive —
        // hold both at baseline below 1W, the same gate the other amplifier
        // applets use, so an idle amp does not paint a meaningless SWR.
        m_refGauge->setValue(fwd >= 1.0f ? static_cast<float>(status.power->reflected.value) : 0.0f);
        m_swrGauge->setValue(fwd >= 1.0f ? status.power->swr.value : 1.0f);
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
    const bool operate = m_status.operate
        && *m_status.operate == Rfkit::OperateMode::Operate;
    m_operateBtn->setText(operate ? QStringLiteral("OPERATE") : QStringLiteral("STANDBY"));
    // The label is a STATE, not an action, so spell that out for an AT rather
    // than leaving it to infer a verb (docs/a11y.md §1).
    m_operateBtn->setAccessibleName(operate ? tr("Amplifier in operate") : tr("Amplifier in standby"));
    if (QAccessible::isActive()) {
        QAccessibleEvent event(m_operateBtn, QAccessible::NameChanged);
        QAccessible::updateAccessibility(&event);
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
        // Until the list arrives, show every button (disabled); once it has,
        // show only the antennas the device actually has.
        btn->setVisible(!listKnown || item != nullptr);
        btn->setEnabled(m_connected && item && item->state != Rfkit::AntennaState::Disabled);
        const bool active = m_connected && item && item->state == Rfkit::AntennaState::Active;
        setBtnState(btn, active ? QStringLiteral("active") : QString());
    }

    // External antennas: rebuild the combo only when the usable set changes, so
    // an open drop-down is not torn down by every poll.
    QVector<int> numbers;
    int activeNumber = 0;
    for (const auto& item : list) {
        if (item.antenna.type != Rfkit::AntennaType::External
            || item.state == Rfkit::AntennaState::Disabled) {
            continue;
        }
        numbers.append(item.antenna.number);
        if (item.state == Rfkit::AntennaState::Active) {
            activeNumber = item.antenna.number;
        }
    }
    QVector<int> shown;
    for (int i = 1; i < m_extAntCombo->count(); ++i) {
        shown.append(m_extAntCombo->itemData(i).toInt());
    }
    if (shown != numbers) {
        const QSignalBlocker blocker(m_extAntCombo);
        m_extAntCombo->clear();
        m_extAntCombo->addItem(QStringLiteral("EXT"), 0);  // placeholder row: no external active
        for (int number : numbers) {
            m_extAntCombo->addItem(Rfkit::antennaLabel({Rfkit::AntennaType::External, number}), number);
        }
    }
    {
        const QSignalBlocker blocker(m_extAntCombo);
        m_extAntCombo->setCurrentIndex(std::max(0, m_extAntCombo->findData(activeNumber)));
    }
    m_extAntCombo->setVisible(!numbers.isEmpty());
    m_extAntCombo->setEnabled(m_connected);
}

void RfkitApplet::setConnected(bool connected)
{
    m_connected = connected;
    m_operateBtn->setEnabled(connected);
    m_resetErrorBtn->setEnabled(connected);

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
        m_swrGauge->setValueImmediate(1.0f);
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
        ? QStringLiteral("SWR  %1:1").arg(m_status.power->swr.value, 0, 'f', 1)
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
    } else {
        m_tunerLabel->setText(QStringLiteral("TUNER  —"));
    }
}

}  // namespace AetherSDR
