#include "Kpa500Applet.h"
#include "AmpAppletStyles.h"
#include "HGauge.h"
#include "core/ThemeManager.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QComboBox>
#include <QSignalBlocker>

namespace AetherSDR {

namespace {

// Pill state and text driven by connection + respond + operate flags.
AmpPillState kpa500PillState(bool connected, bool responding, bool operate)
{
    if (!connected || !responding)
        return AmpPillState::Neutral;
    return operate ? AmpPillState::OperateRx : AmpPillState::Standby;
}

QString kpa500PillText(bool connected, bool responding, bool operate)
{
    if (!connected)
        return QStringLiteral("DISCONNECTED");
    if (!responding)
        return QStringLiteral("NO RESPONSE");
    return operate ? QStringLiteral("OPERATE") : QStringLiteral("STANDBY");
}

// Gauge label on the left — fixed width keeps the gauge column aligned.
QLabel* makeReadoutLabel(QWidget* parent)
{
    auto* lbl = new QLabel(parent);
    lbl->setFixedWidth(72);
    lbl->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    AetherSDR::ThemeManager::instance().applyStyleSheet(lbl,
        "QLabel { color: {{color.text.primary}}; font-size: 11px; font-weight: bold; }");
    return lbl;
}

// Tick marks: 0 / 125 / 250 / 375 / 500 W.
QVector<HGauge::Tick> kpa500PwrTicks()
{
    QVector<HGauge::Tick> ticks;
    for (int w : {0, 125, 250, 375, 500})
        ticks.append({static_cast<float>(w), QString::number(w)});
    return ticks;
}

}  // namespace

Kpa500Applet::Kpa500Applet(QWidget* parent)
    : QWidget(parent)
{
    theme::setContainer(this, QStringLiteral("applet/kpa500"));
    auto* vbox = new QVBoxLayout(this);
    vbox->setContentsMargins(4, 2, 4, 2);
    vbox->setSpacing(2);

    // ── Header: status pill ───────────────────────────────────────────────
    m_statusPill = new QLabel(
        kpa500PillText(m_connected, m_responding, m_operate), this);
    AetherSDR::ThemeManager::instance().applyStyleSheet(m_statusPill,
        ampPillStyle(kpa500PillState(m_connected, m_responding, m_operate)));
    m_statusPill->setAlignment(Qt::AlignCenter);
    auto* headerRow = new QHBoxLayout;
    headerRow->addStretch();
    headerRow->addWidget(m_statusPill);
    vbox->addLayout(headerRow);

    // ── PWR row: gauge (0–500 W) + label ─────────────────────────────────
    m_pwrLabel = makeReadoutLabel(this);
    m_pwrLabel->setText("PWR");
    m_pwrGauge = new HGauge(0.0f, 500.0f, 500.0f, "", "",
        kpa500PwrTicks(), this);
    m_pwrGauge->setWindowPeakEnabled(true);
    m_pwrGauge->setAccessibleName(tr("Forward power"));
    auto* pwrRow = new QHBoxLayout;
    pwrRow->setSpacing(4);
    pwrRow->addWidget(m_pwrLabel);
    pwrRow->addWidget(m_pwrGauge, 1);
    vbox->addLayout(pwrRow);

    vbox->addSpacing(4);

    // ── Info grid: SWR / Temp / Volt / Curr / Band ────────────────────────
    // KPA500 has no internal ATU, so SWR is a readout, not a separate gauge.
    // Volt and current are PA supply — similar to ACOM's HV/Id readout pair.
    auto& theme = AetherSDR::ThemeManager::instance();
    static constexpr const char* kTelStyle =
        "QLabel { color: {{color.text.primary}}; font-size: 10px; }";

    m_swrLabel  = new QLabel("SWR  —", this);
    theme.applyStyleSheet(m_swrLabel, kTelStyle);

    m_tempLabel = new QLabel("TMP  —", this);
    theme.applyStyleSheet(m_tempLabel, kTelStyle);

    m_voltLabel = new QLabel("V  — V", this);
    theme.applyStyleSheet(m_voltLabel, kTelStyle);

    m_currLabel = new QLabel("I  — A", this);
    theme.applyStyleSheet(m_currLabel, kTelStyle);

    m_bandLabel = new QLabel(this);
    theme.applyStyleSheet(m_bandLabel, kTelStyle);
    m_bandLabel->hide();

    // CLEAR fault button — compact, sits in grid so button row stays 2 wide.
    m_clearFaultBtn = new QPushButton("CLEAR", this);
    m_clearFaultBtn->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    AetherSDR::ThemeManager::instance().applyStyleSheet(m_clearFaultBtn,
        ampClearFaultBtnStyle());
    m_clearFaultBtn->setEnabled(false);
    connect(m_clearFaultBtn, &QPushButton::clicked,
            this, &Kpa500Applet::clearFaultClicked);

    // Row 0: SWR | Temp | Band
    // Row 1: Volt | Curr | CLEAR
    auto* infoGrid = new QGridLayout;
    infoGrid->setHorizontalSpacing(12);
    infoGrid->setVerticalSpacing(2);
    infoGrid->addWidget(m_swrLabel,      0, 0);
    infoGrid->addWidget(m_tempLabel,     0, 1);
    infoGrid->addWidget(m_bandLabel,     0, 2);
    infoGrid->addWidget(m_voltLabel,     1, 0);
    infoGrid->addWidget(m_currLabel,     1, 1);
    infoGrid->addWidget(m_clearFaultBtn, 1, 2);
    vbox->addLayout(infoGrid);

    vbox->addSpacing(4);

    // ── Fan minimum speed control ─────────────────────────────────────────
    // Source: KPA500 Programmer's Reference Rev. A2, §^FC — 0 (off) to 6 (high).
    {
        auto* fanRow = new QHBoxLayout;
        fanRow->setSpacing(6);
        auto* fanLbl = new QLabel("FAN MIN", this);
        theme.applyStyleSheet(fanLbl, kTelStyle);
        m_fanCombo = new QComboBox(this);
        m_fanCombo->setAccessibleName(tr("Fan minimum speed"));
        for (int i = 0; i <= 6; ++i) {
            if (i == 0)
                m_fanCombo->addItem(QStringLiteral("0 – Off"));
            else if (i == 6)
                m_fanCombo->addItem(QStringLiteral("6 – High"));
            else
                m_fanCombo->addItem(QString::number(i));
        }
        m_fanCombo->setEnabled(false);
        theme.applyStyleSheet(m_fanCombo,
            "QComboBox { background: {{color.background.1}}; border: 1px solid {{color.background.2}}; "
            "border-radius: 3px; color: {{color.text.primary}}; font-size: 10px; padding: 1px 4px; }"
            "QComboBox::drop-down { border: none; }"
            "QComboBox:disabled { color: {{color.text.secondary}}; border-color: {{color.background.1}}; }");
        connect(m_fanCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this](int index) {
            emit fanSpeedChangeRequested(index);  // index == speed (0–6)
        });
        fanRow->addWidget(fanLbl);
        fanRow->addWidget(m_fanCombo);
        fanRow->addStretch();
        vbox->addLayout(fanRow);
    }

    vbox->addSpacing(4);

    // ── Fault banner (shown only when an active fault is present) ─────────
    m_faultLabel = new QLabel(this);
    m_faultLabel->setWordWrap(true);
    theme.applyStyleSheet(m_faultLabel,
        "QLabel { color: {{color.accent.error}}; font-size: 10px; font-weight: bold; }");
    m_faultLabel->hide();
    vbox->addWidget(m_faultLabel);

    // ── Button row: STANDBY / OPERATE ────────────────────────────────────
    auto* btnRow = new QHBoxLayout;
    btnRow->setSpacing(6);
    btnRow->addStretch();

    m_standbyBtn = new QPushButton("STANDBY", this);
    m_standbyBtn->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    AetherSDR::ThemeManager::instance().applyStyleSheet(m_standbyBtn, ampNeutralBtnStyle());
    connect(m_standbyBtn, &QPushButton::clicked, this,
            [this]() { emit operateToggled(false); });
    btnRow->addWidget(m_standbyBtn);

    m_operateBtn = new QPushButton("OPERATE", this);
    m_operateBtn->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    AetherSDR::ThemeManager::instance().applyStyleSheet(m_operateBtn, ampNeutralBtnStyle());
    connect(m_operateBtn, &QPushButton::clicked, this,
            [this]() { emit operateToggled(true); });
    btnRow->addWidget(m_operateBtn);

    vbox->addLayout(btnRow);

    // ── Label-text throttle: 10 Hz, same as AcomApplet / AmpApplet ────────
    m_labelTimer.setInterval(100);  // 10 Hz
    connect(&m_labelTimer, &QTimer::timeout,
            this, &Kpa500Applet::updateValueLabels);
    m_labelTimer.start();

    setConnected(false);
}

// ── Public API ────────────────────────────────────────────────────────────────

void Kpa500Applet::applyStatus(const Kpa500::Status& status)
{
    if (status.forwardPowerW) {
        m_fwdW = *status.forwardPowerW;
        m_pwrGauge->setValue(m_fwdW);
        m_pwrDirty = true;
    }
    if (status.swr) {
        m_swr = *status.swr;
        m_infoDirty = true;
    }
    if (status.paTemperatureC) {
        m_tempC = *status.paTemperatureC;
        m_infoDirty = true;
    }
    if (status.paVoltageV) {
        m_voltV = *status.paVoltageV;
        m_infoDirty = true;
    }
    if (status.paCurrentA) {
        m_currA = *status.paCurrentA;
        m_infoDirty = true;
    }
    if (status.band) {
        m_band = *status.band;
        m_infoDirty = true;
    }
    if (status.faultCode) {
        m_faultCode = *status.faultCode;
        m_faultDirty = true;
    }
    if (status.fanMinSpeed) {
        m_fanSpeed = *status.fanMinSpeed;
        m_fanDirty = true;
    }
    if (status.operate) {
        m_operate = *status.operate;
        m_statusDirty = true;
    }
    // Identification fields — only arrive on connect-time one-shot queries.
    // Cache them and update the pill tooltip whenever either changes.
    bool idChanged = false;
    if (status.firmwareVersion && *status.firmwareVersion != m_firmware) {
        m_firmware = *status.firmwareVersion;
        idChanged = true;
    }
    if (status.serialNumber && *status.serialNumber != m_serial) {
        m_serial = *status.serialNumber;
        idChanged = true;
    }
    if (idChanged && (!m_firmware.isEmpty() || !m_serial.isEmpty()))
        setDiagnosticTooltip(QStringLiteral("FW %1  SN %2").arg(m_firmware, m_serial));
}

void Kpa500Applet::setConnected(bool connected)
{
    m_connected = connected;

    m_standbyBtn->setEnabled(connected);
    m_operateBtn->setEnabled(connected);

    if (!connected) {
        m_responding = false;
        m_operate    = false;
        m_fwdW       = 0.0f;
        m_swr        = 0.0f;
        m_tempC      = 0.0f;
        m_voltV      = 0.0f;
        m_currA      = 0.0f;
        m_band       = -1;
        m_faultCode  = -1;
        m_fanSpeed   = -1;

        m_fanCombo->setEnabled(false);

        m_pwrGauge->setValueImmediate(0.0f);
        m_pwrGauge->clearPeak();

        m_faultLabel->hide();
        m_faultLabel->clear();
        m_clearFaultBtn->setEnabled(false);
        m_bandLabel->hide();

        m_pwrDirty    = false;
        m_infoDirty   = false;
        m_faultDirty  = false;
        m_fanDirty    = false;
        m_statusDirty = false;

        // Reset text readouts to placeholder dashes.
        m_pwrLabel->setText("PWR");
        m_swrLabel->setText("SWR  —");
        m_tempLabel->setText("TMP  —");
        m_voltLabel->setText("V  — V");
        m_currLabel->setText("I  — A");
    }

    updateStatusPill();
}

void Kpa500Applet::setResponding(bool responding)
{
    m_responding = responding;
    if (!responding) {
        // Clear live readouts — "NO RESPONSE" next to a confident SWR 1.2:1
        // is the dangerous state (#4953 triage, safety label). Band is kept
        // (it doesn't change when the amp goes silent); fault banner and
        // firmware tooltip are kept too (they describe a condition, not a
        // live measurement).
        m_fwdW  = 0.0f;
        m_swr   = 0.0f;
        m_tempC = 0.0f;
        m_voltV = 0.0f;
        m_currA = 0.0f;
        m_pwrDirty  = false;
        m_infoDirty = false;
        m_pwrLabel->setText("PWR");
        m_swrLabel->setText("SWR  —");
        m_tempLabel->setText("TMP  —");
        m_voltLabel->setText("V  — V");
        m_currLabel->setText("I  — A");
        m_pwrGauge->setValueImmediate(0.0f);
        m_pwrGauge->clearPeak();
    }
    updateStatusPill();
}

void Kpa500Applet::setDiagnosticTooltip(const QString& text)
{
    m_statusPill->setToolTip(text);
}

// ── Private ───────────────────────────────────────────────────────────────────

void Kpa500Applet::updateStatusPill()
{
    m_statusPill->setText(kpa500PillText(m_connected, m_responding, m_operate));
    auto& theme = AetherSDR::ThemeManager::instance();
    theme.applyStyleSheet(m_statusPill,
        ampPillStyle(kpa500PillState(m_connected, m_responding, m_operate)));
    theme.applyStyleSheet(m_standbyBtn,
        (m_connected && m_responding && !m_operate)
            ? ampStandbyBtnStyle()
            : ampNeutralBtnStyle());
    theme.applyStyleSheet(m_operateBtn,
        (m_connected && m_responding && m_operate)
            ? ampOperateActiveBtnStyle()
            : ampNeutralBtnStyle());
}

void Kpa500Applet::updateValueLabels()
{
    if (m_pwrDirty) {
        m_pwrDirty = false;
        m_pwrLabel->setText(m_fwdW >= 1.0f
            ? QStringLiteral("PWR  %1").arg(static_cast<int>(m_fwdW))
            : QStringLiteral("PWR"));
    }
    if (m_infoDirty) {
        m_infoDirty = false;

        // SWR: only meaningful with forward drive (same gate as ACOM).
        m_swrLabel->setText(m_fwdW >= 1.0f && m_swr >= 1.0f
            ? QStringLiteral("SWR  %1:1").arg(m_swr, 0, 'f', 1)
            : QStringLiteral("SWR  —"));

        // Temperature.
        m_tempLabel->setText(m_tempC > 0.0f
            ? QStringLiteral("TMP  %1°C").arg(static_cast<int>(m_tempC))
            : QStringLiteral("TMP  —"));

        // PA supply voltage + current.
        m_voltLabel->setText(m_voltV > 0.0f
            ? QStringLiteral("V  %1").arg(m_voltV, 0, 'f', 1)
            : QStringLiteral("V  — V"));
        m_currLabel->setText(m_currA > 0.0f
            ? QStringLiteral("I  %1A").arg(m_currA, 0, 'f', 1)
            : QStringLiteral("I  — A"));

        // Band label.
        if (m_band >= 0) {
            m_bandLabel->setText(
                QStringLiteral("BAND  %1").arg(Kpa500::bandName(m_band)));
            m_bandLabel->show();
        } else {
            m_bandLabel->hide();
        }
    }
    if (m_faultDirty) {
        m_faultDirty = false;
        if (m_faultCode > 0) {
            m_faultLabel->setText(
                QStringLiteral("FAULT %1").arg(m_faultCode));
            m_faultLabel->show();
            m_clearFaultBtn->setEnabled(true);
        } else {
            m_faultLabel->hide();
            m_faultLabel->clear();
            m_clearFaultBtn->setEnabled(false);
        }
    }
    if (m_fanDirty) {
        m_fanDirty = false;
        if (m_fanSpeed >= 0 && m_fanSpeed <= 6) {
            QSignalBlocker b(m_fanCombo);
            m_fanCombo->setCurrentIndex(m_fanSpeed);
            m_fanCombo->setEnabled(m_connected);
        }
    }
    if (m_statusDirty) {
        m_statusDirty = false;
        updateStatusPill();
    }
}

}  // namespace AetherSDR
