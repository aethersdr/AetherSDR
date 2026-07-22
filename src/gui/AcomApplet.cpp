#include "AcomApplet.h"
#include "HGauge.h"
#include "core/AppSettings.h"
#include "core/ThemeManager.h"
#include "MeterSmoother.h"

#include <QAccessible>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QJsonDocument>
#include <QJsonObject>

namespace AetherSDR {

namespace {

constexpr const char* kAcomAppletSettingsKey = "AcomApplet";
constexpr const char* kTempFahrenheitField = "tempFahrenheit";

QJsonObject readAcomAppletSettings()
{
    const QString json = AppSettings::instance()
        .value(kAcomAppletSettingsKey, QString{}).toString();
    if (json.isEmpty()) return {};
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    return doc.isObject() ? doc.object() : QJsonObject{};
}

void writeAcomAppletSettings(const QJsonObject& obj)
{
    auto& settings = AppSettings::instance();
    settings.setValue(kAcomAppletSettingsKey,
        QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact)));
    settings.save();
}

bool readTempFahrenheit()
{
    return readAcomAppletSettings()
        .value(kTempFahrenheitField)
        .toString(QStringLiteral("False")) == QStringLiteral("True");
}

void writeTempFahrenheit(bool enabled)
{
    QJsonObject obj = readAcomAppletSettings();
    obj[kTempFahrenheitField] = enabled ? QStringLiteral("True") : QStringLiteral("False");
    writeAcomAppletSettings(obj);
}

float displayTemp(float degC, bool fahrenheit)
{
    return fahrenheit ? degC * 9.0f / 5.0f + 32.0f : degC;
}

QString formatTemp(float degC, bool fahrenheit)
{
    return QStringLiteral("%1").arg(displayTemp(degC, fahrenheit), 0, 'f', 1);
}

// 5 evenly spaced ticks across [0, max] — used for the PWR/REF gauges,
// which get their scale from the amplifier's own nominal/max constants
// (design doc §6) rather than a fixed range like the SWR gauge.
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
// AmpApplet's makeValueLabel convention.
QLabel* makeValueLabel(QWidget* parent)
{
    auto* lbl = new QLabel(parent);
    lbl->setFixedWidth(46);
    lbl->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    lbl->setStyleSheet("QLabel { color: #c8d8e8; font-size: 11px; font-weight: bold; }");
    return lbl;
}

QString modePillStyle(Acom::Mode mode)
{
    using Acom::Mode;
    switch (mode) {
        case Mode::OperateTx:
            return "QLabel { background: #3a1418; color: #ff8080; border: 1px solid #ff4d4d; "
                   "border-radius: 3px; font-size: 9px; font-weight: bold; padding: 2px 6px; }";
        case Mode::OperateRx:
            return "QLabel { background: #0f2a1c; color: #6be899; border: 1px solid #4dd87a; "
                   "border-radius: 3px; font-size: 9px; font-weight: bold; padding: 2px 6px; }";
        case Mode::Standby:
            return "QLabel { background: #12222e; color: #7fc4dc; border: 1px solid #2a5a70; "
                   "border-radius: 3px; font-size: 9px; font-weight: bold; padding: 2px 6px; }";
        default:
            return "QLabel { background: #1c222a; color: #6b7684; border: 1px solid #303a44; "
                   "border-radius: 3px; font-size: 9px; font-weight: bold; padding: 2px 6px; }";
    }
}

QString modePillText(Acom::Mode mode)
{
    using Acom::Mode;
    switch (mode) {
        case Mode::OperateTx: return QStringLiteral("OPR · TX");
        case Mode::OperateRx: return QStringLiteral("OPR · RX");
        case Mode::Standby:   return QStringLiteral("STANDBY");
        case Mode::PowerOff:  return QStringLiteral("OFF");
        default:              return QStringLiteral("—");
    }
}

}  // namespace

AcomApplet::AcomApplet(QWidget* parent)
    : QWidget(parent)
{
    theme::setContainer(this, QStringLiteral("applet/acom"));
    auto* vbox = new QVBoxLayout(this);
    vbox->setContentsMargins(4, 2, 4, 2);
    vbox->setSpacing(2);

    // ── Header: source (connection status) + status pill (mode) ────────────
    // Source lives here rather than in the info grid — it's connection
    // status, not a telemetry reading, and keeping it out of the grid lets
    // the grid stay a clean 3-cells-per-row layout (temp/HV/Id, band/carrier/
    // uptime) instead of splitting a field across a shared cell.
    m_sourceLabel = new QLabel("● —", this);
    m_sourceLabel->setStyleSheet("QLabel { color: #00b4d8; font-size: 9px; }");
    m_statusPill = new QLabel(modePillText(m_mode), this);
    m_statusPill->setStyleSheet(modePillStyle(m_mode));
    m_statusPill->setAlignment(Qt::AlignCenter);
    auto* headerRow = new QHBoxLayout;
    headerRow->addWidget(m_sourceLabel);
    headerRow->addStretch();
    headerRow->addWidget(m_statusPill);
    vbox->addLayout(headerRow);

    // ── PWR row ───────────────────────────────────────────────────────────
    m_pwrLabel = makeValueLabel(this);
    m_pwrLabel->setText("PWR");
    m_pwrGauge = new HGauge(0.0f, 700.0f, 600.0f, "", "",
        evenTicks(700.0f), this);
    m_pwrGauge->setBallistics({0.030f, 0.800f});
    m_pwrGauge->setAccessibleName(tr("Forward power"));
    auto* pwrRow = new QHBoxLayout;
    pwrRow->setSpacing(4);
    pwrRow->addWidget(m_pwrLabel);
    pwrRow->addWidget(m_pwrGauge, 1);
    vbox->addLayout(pwrRow);

    // ── REF row (reflected power) ────────────────────────────────────────
    m_refLabel = makeValueLabel(this);
    m_refLabel->setText("REF");
    m_refGauge = new HGauge(0.0f, 150.0f, 114.0f, "", "",
        evenTicks(150.0f), this);
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

    // ── Info grid: temp / HV / Id / band / carrier / uptime / source ───────
    static const char* kTelStyle = "QLabel { color: #c8d8e8; font-size: 10px; }";

    m_tempFahrenheit = readTempFahrenheit();
    m_tempBtn = new QPushButton(this);
    m_tempBtn->setObjectName(QStringLiteral("acomTempUnitButton"));
    m_tempBtn->setFlat(true);
    m_tempBtn->setFocusPolicy(Qt::TabFocus);
    m_tempBtn->setCursor(Qt::PointingHandCursor);
    m_tempBtn->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    m_tempBtn->setAccessibleDescription("Toggles amplifier temperature between Celsius and Fahrenheit");
    AetherSDR::ThemeManager::instance().applyStyleSheet(m_tempBtn,
        "QPushButton { background: transparent; border: 1px solid transparent; "
        "color: #c8d8e8; font-size: 10px; text-align: left; padding: 0 2px; }"
        "QPushButton:hover { border-color: #203040; color: #ffffff; }"
        "QPushButton:focus { border-color: #00b4d8; }");
    connect(m_tempBtn, &QPushButton::clicked, this, [this]() {
        m_tempFahrenheit = !m_tempFahrenheit;
        writeTempFahrenheit(m_tempFahrenheit);
        updateTempLabel();
    });
    updateTempLabel();

    m_hvLabel = new QLabel("HV  — V", this);
    m_hvLabel->setStyleSheet(kTelStyle);
    m_idLabel = new QLabel("Id  — A", this);
    m_idLabel->setStyleSheet(kTelStyle);
    m_bandLabel = new QLabel(this);
    m_bandLabel->setStyleSheet(kTelStyle);
    m_bandLabel->hide();
    m_uptimeLabel = new QLabel(this);
    m_uptimeLabel->setStyleSheet(kTelStyle);
    m_uptimeLabel->hide();

    // Compact — sized to sit inline in the info grid rather than the full
    // button row (see the grid comment below for why it moved here).
    m_clearFaultBtn = new QPushButton("CLEAR", this);
    m_clearFaultBtn->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    AetherSDR::ThemeManager::instance().applyStyleSheet(m_clearFaultBtn,
        "QPushButton { background: #2a2210; border: 1px solid #5a4a1a; "
        "border-radius: 3px; color: #ffb84d; font-size: 9px; font-weight: bold; padding: 1px 4px; }"
        "QPushButton:hover { background: #3a2e14; }"
        "QPushButton:disabled { background: #181c22; border: 1px solid #232a33; color: #3a4552; }");
    m_clearFaultBtn->setEnabled(false);
    connect(m_clearFaultBtn, &QPushButton::clicked, this, &AcomApplet::clearFaultClicked);

    // 3 cells per row: temp/HV/Id, then band/uptime/clear. Carrier frequency
    // only reads nonzero while the amp senses actual RF drive — idle/standby
    // leaves it blank most of the time in practice — so instead of a mostly-
    // empty cell, CLEAR lives there. That also frees the button row below to
    // 3 buttons (STANDBY/OPERATE/OFF) instead of 4, which were clipping at
    // default width.
    auto* infoGrid = new QGridLayout;
    infoGrid->setHorizontalSpacing(12);
    infoGrid->setVerticalSpacing(2);
    infoGrid->addWidget(m_tempBtn,      0, 0);
    infoGrid->addWidget(m_hvLabel,      0, 1);
    infoGrid->addWidget(m_idLabel,      0, 2);
    infoGrid->addWidget(m_bandLabel,    1, 0);
    infoGrid->addWidget(m_uptimeLabel,  1, 1);
    infoGrid->addWidget(m_clearFaultBtn, 1, 2);
    vbox->addLayout(infoGrid);

    vbox->addSpacing(4);

    // ── Fault banner (own row, full width, only shown when active) ─────────
    m_faultLabel = new QLabel(this);
    m_faultLabel->setWordWrap(true);
    m_faultLabel->setStyleSheet(
        "QLabel { color: #ff8080; font-size: 10px; font-weight: bold; }");
    m_faultLabel->hide();
    vbox->addWidget(m_faultLabel);

    // ── Button row: STANDBY / OPERATE / OFF ─────────────────────────────────
    static const char* kBtnStyle =
        "QPushButton { background: {{color.background.2}}; border: 1px solid {{color.background.2}}; "
        "border-radius: 3px; color: {{color.text.primary}}; font-size: 10px; font-weight: bold; }"
        "QPushButton:hover { background: {{color.background.1}}; }";

    auto* btnRow = new QHBoxLayout;
    btnRow->setSpacing(6);
    btnRow->addStretch();

    m_standbyBtn = new QPushButton("STANDBY", this);
    m_standbyBtn->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    AetherSDR::ThemeManager::instance().applyStyleSheet(m_standbyBtn, kBtnStyle);
    connect(m_standbyBtn, &QPushButton::clicked, this, [this]() { emit operateToggled(false); });
    btnRow->addWidget(m_standbyBtn);

    m_operateBtn = new QPushButton("OPERATE", this);
    m_operateBtn->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    AetherSDR::ThemeManager::instance().applyStyleSheet(m_operateBtn, kBtnStyle);
    connect(m_operateBtn, &QPushButton::clicked, this, [this]() { emit operateToggled(true); });
    btnRow->addWidget(m_operateBtn);

    m_offBtn = new QPushButton("OFF", this);
    m_offBtn->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    AetherSDR::ThemeManager::instance().applyStyleSheet(m_offBtn, kBtnStyle);
    connect(m_offBtn, &QPushButton::clicked, this, &AcomApplet::offClicked);
    btnRow->addWidget(m_offBtn);
    vbox->addLayout(btnRow);

    // Label text throttle — matches AmpApplet's 10 Hz readout convention.
    m_labelTimer.setInterval(kMeterReadoutUpdateMs);
    connect(&m_labelTimer, &QTimer::timeout, this, &AcomApplet::updateValueLabels);
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

void AcomApplet::setPowerRange(float nominalW, float maxW)
{
    m_pwrGauge->setRange(0.0f, maxW, nominalW, evenTicks(maxW));
}

void AcomApplet::setReflectedRange(float nominalW, float maxW)
{
    m_refGauge->setRange(0.0f, maxW, nominalW, evenTicks(maxW));
}

void AcomApplet::setForwardPower(float watts)
{
    m_fwdWatts = watts;
    m_pwrGauge->setValue(watts);
    if (watts > m_peakFwd) {
        m_peakFwd = watts;
        m_pwrGauge->setPeakValue(watts);
        m_peakTimer->start();
    }
}

void AcomApplet::setReflectedPower(float watts)
{
    m_reflectedWatts = watts;
    // Reflected power is only meaningful while there's forward drive — hold the
    // needle at baseline below 1 W so the gauge agrees with its readout label,
    // which already blanks under the same condition (updateValueLabels). Wiring
    // sets forward power first each frame, so m_fwdWatts is current here.
    m_refGauge->setValue(m_fwdWatts >= 1.0f ? watts : 0.0f);
}

void AcomApplet::setSwr(float swr)
{
    m_swrVal = swr;
    // Same forward-power gate as the SWR label: without forward drive SWR is
    // undefined, so hold the gauge at 1.0 rather than tracking a garbage ratio.
    m_swrGauge->setValue(m_fwdWatts >= 1.0f ? swr : 1.0f);
}

void AcomApplet::setDrainCurrent(float amps)
{
    m_drainAmps = amps;
    m_diagDirty = true;
}

void AcomApplet::setDrainVoltage(float volts)
{
    m_drainVolts = volts;
    m_diagDirty = true;
}

void AcomApplet::setTemp(float degC)
{
    m_tempC = degC;
    m_hasTemp = true;
    m_tempDirty = true;
}

void AcomApplet::updateTempLabel()
{
    const QString unit = m_tempFahrenheit ? QStringLiteral("F") : QStringLiteral("C");
    const QString val = m_hasTemp ? formatTemp(m_tempC, m_tempFahrenheit) : QStringLiteral("—");
    m_tempBtn->setText(QStringLiteral("%1 %2").arg(val, unit));
    const QString nextUnit = m_tempFahrenheit ? tr("Celsius") : tr("Fahrenheit");
    m_tempBtn->setToolTip(tr("Amplifier temperature\nClick to show degrees %1").arg(nextUnit));
    m_tempBtn->setAccessibleName(tr("Amplifier temperature %1").arg(m_tempBtn->text()));
    if (QAccessible::isActive()) {
        QAccessibleEvent event(m_tempBtn, QAccessible::NameChanged);
        QAccessible::updateAccessibility(&event);
    }
}

void AcomApplet::setBand(const QString& band)
{
    m_pendingBand = band;
    m_bandDirty = true;
}

void AcomApplet::setUptime(quint32 totalSeconds)
{
    m_pendingUptimeSeconds = totalSeconds;
    m_uptimeDirty = true;
}

void AcomApplet::setSource(const QString& text)
{
    m_sourceLabel->setText(QStringLiteral("● %1").arg(text));
}

namespace {
QString activeBtnStyle(const QString& bg, const QString& border)
{
    return QStringLiteral(
        "QPushButton { background: %1; border: 1px solid %2; border-radius: 3px; "
        "color: {{color.text.primary}}; font-size: 10px; font-weight: bold; } "
        "QPushButton:hover { background: %1; }").arg(bg, border);
}

QString neutralBtnStyle()
{
    return QStringLiteral(
        "QPushButton { background: {{color.background.2}}; border: 1px solid {{color.background.2}}; "
        "border-radius: 3px; color: {{color.text.primary}}; font-size: 10px; font-weight: bold; }"
        "QPushButton:hover { background: {{color.background.1}}; }");
}
}  // namespace

void AcomApplet::setMode(Acom::Mode mode)
{
    m_mode = mode;
    m_statusPill->setText(modePillText(mode));
    m_statusPill->setStyleSheet(modePillStyle(mode));

    const bool isStandby = (mode == Acom::Mode::Standby);
    const bool isOperate = (mode == Acom::Mode::OperateTx || mode == Acom::Mode::OperateRx);
    const bool isOff     = (mode == Acom::Mode::PowerOff || mode == Acom::Mode::Unknown);

    auto& theme = AetherSDR::ThemeManager::instance();
    theme.applyStyleSheet(m_standbyBtn,
        isStandby ? activeBtnStyle("#12222e", "#2a5a70") : neutralBtnStyle());
    theme.applyStyleSheet(m_operateBtn,
        isOperate ? activeBtnStyle("#006030", "#008040") : neutralBtnStyle());
    theme.applyStyleSheet(m_offBtn,
        isOff ? activeBtnStyle("#3a2a2a", "#5a3a3a") : neutralBtnStyle());
}

void AcomApplet::setFaultText(const QString& text)
{
    if (text.isEmpty()) {
        m_faultLabel->hide();
        m_faultLabel->clear();
        return;
    }
    m_faultLabel->setText(text);
    m_faultLabel->show();
}

void AcomApplet::setClearFaultEnabled(bool enabled)
{
    // Stays visible always — only enabled state changes — so it reads as
    // "here, nothing to clear right now" rather than disappearing entirely.
    m_clearFaultBtn->setEnabled(enabled);
}

void AcomApplet::setDiagnosticTooltip(const QString& text)
{
    m_statusPill->setToolTip(text);
}

void AcomApplet::setConnected(bool connected)
{
    m_standbyBtn->setEnabled(connected);
    m_operateBtn->setEnabled(connected);
    m_offBtn->setEnabled(connected);
    if (!connected) {
        setMode(Acom::Mode::Unknown);
        setFaultText(QString());
        setClearFaultEnabled(false);
        m_sourceLabel->setText(QStringLiteral("● —"));
        m_bandLabel->hide();
        m_uptimeLabel->hide();
        m_bandDirty = false;
        m_uptimeDirty = false;
        m_drainAmps = 0.0f;
        m_drainVolts = 0.0f;
        m_diagDirty = false;
        m_hvLabel->setText("HV  — V");
        m_idLabel->setText("Id  — A");
        m_hasTemp = false;
        m_tempDirty = false;
        updateTempLabel();
        m_fwdWatts = 0.0f;
        m_reflectedWatts = 0.0f;
        m_swrVal = 1.0f;
        // Clear the forward-power peak hold too — otherwise a stale peak from
        // the prior session survives (m_peakFwd is not otherwise reset), and a
        // reconnect within the 2.5 s peak-hold window suppresses the new
        // session's peak marker until a reading exceeds the old peak.
        m_peakFwd = 0.0f;
        if (m_peakTimer) m_peakTimer->stop();
        m_pwrGauge->setValueImmediate(0.0f);
        m_pwrGauge->clearPeak();
        m_refGauge->setValueImmediate(0.0f);
        m_swrGauge->setValueImmediate(1.0f);
        updateValueLabels();
    }
}

void AcomApplet::updateValueLabels()
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

    if (m_tempDirty) {
        m_tempDirty = false;
        updateTempLabel();
    }
    if (m_diagDirty) {
        m_diagDirty = false;
        // Text readouts, not gauges — the protocol has no nominal/max
        // current constant to size an axis against (design doc, "Id gauge
        // scale" note).
        m_idLabel->setText(QStringLiteral("Id  %1A").arg(m_drainAmps, 0, 'f', 1));
        m_hvLabel->setText(QStringLiteral("HV  %1V").arg(m_drainVolts, 0, 'f', 1));
    }
    if (m_bandDirty) {
        m_bandDirty = false;
        m_bandLabel->setText(QStringLiteral("BAND  %1").arg(m_pendingBand));
        m_bandLabel->show();
    }
    if (m_uptimeDirty) {
        m_uptimeDirty = false;
        // The amp's "system clock" telemetry field is a cumulative total-
        // operating-time counter, not time since the last power-on —
        // confirmed against real 600S telemetry showing a value far larger
        // than any single session (it does not reset per power cycle). "ON"
        // + day/hour format reads naturally for that and stays bounded in
        // length, unlike an unbounded H:MM:SS string that eventually
        // overflows its grid cell.
        const quint32 totalMinutes = m_pendingUptimeSeconds / 60;
        const quint32 totalHours = totalMinutes / 60;
        const quint32 minutes = totalMinutes % 60;

        QString text;
        if (totalHours >= 24) {
            const quint32 days = totalHours / 24;
            const quint32 hours = totalHours % 24;
            text = QStringLiteral("ON  %1d %2h").arg(days).arg(hours);
        } else {
            text = QStringLiteral("ON  %1h %2m").arg(totalHours).arg(minutes);
        }
        m_uptimeLabel->setText(text);
        m_uptimeLabel->show();
    }
}

}  // namespace AetherSDR
