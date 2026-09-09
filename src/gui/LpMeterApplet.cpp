#include "LpMeterApplet.h"
#include "HGauge.h"
#include "core/ThemeManager.h"

#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QTimer>
#include <QVBoxLayout>

#include <cmath>

namespace AetherSDR {

namespace {

// Every style below is a ThemeManager template: {{color.*}} tokens only, no
// literals, so the colour ratchet (tools/audit_colours.py) counts nothing new
// and a theme change re-resolves them.
constexpr const char* kValueStyle =
    "QLabel { color: {{color.text.primary}}; font-size: 11px; font-weight: bold; }";
constexpr const char* kValueDimStyle =
    "QLabel { color: {{color.text.disabled}}; font-size: 11px; font-weight: bold; }";
constexpr const char* kValueWarnStyle =
    "QLabel { color: {{color.accent.warning}}; font-size: 11px; font-weight: bold; }";
constexpr const char* kInfoStyle =
    "QLabel { color: {{color.text.secondary}}; font-size: 10px; }";
constexpr const char* kInfoDimStyle =
    "QLabel { color: {{color.text.disabled}}; font-size: 10px; }";
constexpr const char* kCallsignStyle =
    "QLabel { color: {{color.text.label}}; font-size: 10px; font-weight: bold; }";

constexpr const char* kPillFlowing =
    "QLabel { background: {{color.background.success}}; color: {{color.accent.success}}; "
    "border: 1px solid {{color.accent.success}}; border-radius: 3px; font-size: 9px; "
    "font-weight: bold; padding: 2px 6px; }";
constexpr const char* kPillStalled =
    "QLabel { background: {{color.background.warning}}; color: {{color.accent.warning}}; "
    "border: 1px solid {{color.accent.warning}}; border-radius: 3px; font-size: 9px; "
    "font-weight: bold; padding: 2px 6px; }";
constexpr const char* kPillOffline =
    "QLabel { background: {{color.background.2}}; color: {{color.text.disabled}}; "
    "border: 1px solid {{color.border.subtle}}; border-radius: 3px; font-size: 9px; "
    "font-weight: bold; padding: 2px 6px; }";

// SWR is a ratio, not a wattage, so this axis never rescales with the power
// range — the same reasoning AcomApplet's SWR gauge uses. Amber from 2.0,
// red from 3.0.
constexpr float kSwrMin = 1.0f;
constexpr float kSwrMax = 3.0f;
constexpr float kSwrAmber = 2.0f;
constexpr float kSwrRed = 3.0f;

QVector<HGauge::Tick> evenTicks(float max)
{
    // Defensive: static_cast<int> of a non-finite float is UNDEFINED
    // BEHAVIOUR, and inf*0.0f is NaN, so an infinite ceiling made even the
    // first tick UB. The protocol layer now bounds every field so infinity
    // cannot arrive here -- but a GUI helper should not depend on its caller
    // having validated, and this is the last line before the cast.
    if (!std::isfinite(max) || max <= 0.0f) {
        return {{0.0f, QStringLiteral("0")}};
    }
    QVector<HGauge::Tick> ticks;
    for (float frac : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
        const int v = static_cast<int>(max * frac);
        ticks.append({static_cast<float>(v), QString::number(v)});
    }
    return ticks;
}

QVector<HGauge::Tick> swrTicks()
{
    return {{1.0f, QStringLiteral("1")}, {1.5f, QStringLiteral("1.5")},
            {2.0f, QStringLiteral("2")}, {2.5f, QStringLiteral("2.5")},
            {3.0f, QStringLiteral("3")}};
}

QLabel* makeValueLabel(QWidget* parent)
{
    auto* lbl = new QLabel(parent);
    lbl->setFixedWidth(52);
    lbl->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    ThemeManager::instance().applyStyleSheet(lbl, kValueStyle);
    return lbl;
}

QLabel* makeInfoLabel(QWidget* parent)
{
    auto* lbl = new QLabel(parent);
    ThemeManager::instance().applyStyleSheet(lbl, kInfoStyle);
    return lbl;
}

}  // namespace

LpMeterApplet::LpMeterApplet(QWidget* parent)
    : QWidget(parent)
{
    auto* vbox = new QVBoxLayout(this);
    vbox->setContentsMargins(6, 4, 6, 6);
    vbox->setSpacing(4);

    // ── Header: status pill, transport, callsign ─────────────────────────
    auto* header = new QHBoxLayout;
    header->setSpacing(6);
    m_statusPill = new QLabel(tr("OFFLINE"), this);
    header->addWidget(m_statusPill);
    m_sourceLabel = new QLabel(QStringLiteral("—"), this);
    ThemeManager::instance().applyStyleSheet(m_sourceLabel, kInfoStyle);
    header->addWidget(m_sourceLabel);
    header->addStretch(1);
    // The meter's own programmed callsign. Only load-bearing when a station
    // runs more than one LP-100A, but it is free and it confirms at a glance
    // which meter the tile is showing.
    m_callsignLabel = new QLabel(this);
    m_callsignLabel->setTextFormat(Qt::PlainText);
    ThemeManager::instance().applyStyleSheet(m_callsignLabel, kCallsignStyle);
    header->addWidget(m_callsignLabel);
    vbox->addLayout(header);

    // ── PWR ───────────────────────────────────────────────────────────────
    m_ceilingW = m_ceilings.highW;
    m_pwrValue = makeValueLabel(this);
    m_pwrValue->setText(QStringLiteral("PWR"));
    m_pwrGauge = new HGauge(0.0f, static_cast<float>(m_ceilingW),
                            static_cast<float>(m_ceilingW), "", "",
                            evenTicks(static_cast<float>(m_ceilingW)), this);
    m_pwrGauge->setAccessibleName(tr("Power"));
    auto* pwrRow = new QHBoxLayout;
    pwrRow->setSpacing(4);
    pwrRow->addWidget(m_pwrValue);
    pwrRow->addWidget(m_pwrGauge, 1);
    vbox->addLayout(pwrRow);

    // ── SWR ───────────────────────────────────────────────────────────────
    m_swrValue = makeValueLabel(this);
    m_swrValue->setText(QStringLiteral("SWR"));
    m_swrGauge = new HGauge(kSwrMin, kSwrMax, kSwrRed, "", "", swrTicks(), this);
    m_swrGauge->setRange(kSwrMin, kSwrMax, kSwrRed, swrTicks(), kSwrAmber);
    m_swrGauge->setAccessibleName(tr("Standing wave ratio"));
    auto* swrRow = new QHBoxLayout;
    swrRow->setSpacing(4);
    swrRow->addWidget(m_swrValue);
    swrRow->addWidget(m_swrGauge, 1);
    vbox->addLayout(swrRow);

    // ── Info grid: 3 cells per row ───────────────────────────────────────
    auto* grid = new QGridLayout;
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(2);
    m_dbmLabel = makeInfoLabel(this);
    m_rlLabel = makeInfoLabel(this);
    m_zLabel = makeInfoLabel(this);
    m_phaseLabel = makeInfoLabel(this);
    m_rangeLabel = makeInfoLabel(this);
    m_modeLabel = makeInfoLabel(this);
    grid->addWidget(m_dbmLabel, 0, 0);
    grid->addWidget(m_rlLabel, 0, 1);
    grid->addWidget(m_zLabel, 0, 2);
    grid->addWidget(m_phaseLabel, 1, 0);
    grid->addWidget(m_rangeLabel, 1, 1);
    grid->addWidget(m_modeLabel, 1, 2);
    grid->setColumnStretch(0, 1);
    grid->setColumnStretch(1, 1);
    grid->setColumnStretch(2, 1);
    vbox->addLayout(grid);

    // Power-range ceilings are a DISPLAY preference, not connection state, so
    // they live in the applet's own context menu rather than the Peripherals
    // tab — the split CrossNeedleMeterApplet already follows.
    setContextMenuPolicy(Qt::CustomContextMenu);
    connect(this, &QWidget::customContextMenuRequested,
            this, &LpMeterApplet::showContextMenu);

    m_labelTimer = new QTimer(this);
    m_labelTimer->setInterval(100);  // 10 Hz, matching the meter's own rate
    connect(m_labelTimer, &QTimer::timeout, this, [this]() {
        if (!m_labelsDirty) { return; }
        m_labelsDirty = false;
        refreshLabels();
    });
    m_labelTimer->start();

    refreshStatusPill();
    refreshLabels();
}

void LpMeterApplet::setReading(const LpMeter::Reading& reading)
{
    m_reading = reading;
    m_pwrGauge->setValue(static_cast<float>(reading.powerW));
    // Clamp only the GAUGE, never the readout: the needle cannot leave its
    // axis but the number must still tell the truth about an over-range.
    m_swrGauge->setValue(static_cast<float>(
        std::min<double>(std::max<double>(reading.swr, kSwrMin), kSwrMax)));
    m_labelsDirty = true;
}

void LpMeterApplet::setPowerCeiling(double ceilingW, bool autoExpanded)
{
    if (ceilingW <= 0.0) { return; }
    m_ceilingW = ceilingW;
    m_ceilingAutoExpanded = autoExpanded;
    const float c = static_cast<float>(ceilingW);
    // setRange re-maps the current value onto the new axis (HGauge.h), so a
    // steady carrier across a range change does not keep the old fraction.
    m_pwrGauge->setRange(0.0f, c, c, evenTicks(c));
    m_labelsDirty = true;
}

void LpMeterApplet::setSource(const QString& text)
{
    m_sourceLabel->setText(text);
}

void LpMeterApplet::setConnected(bool connected)
{
    if (m_connected == connected) { return; }
    m_connected = connected;
    if (!connected) {
        // Clear rather than freeze: a stale reading surviving into the next
        // session is the bug SpeApplet had to fix.
        m_reading = LpMeter::Reading{};
        m_dataFlowing = false;
        m_ridingAlong = false;
        m_foreignIntervalMs = -1;
        m_pwrGauge->setValueImmediate(0.0f);
        m_swrGauge->setValueImmediate(kSwrMin);
    }
    refreshStatusPill();
    refreshLabels();
}

void LpMeterApplet::setDataFlowing(bool flowing)
{
    if (m_dataFlowing == flowing) { return; }
    m_dataFlowing = flowing;
    refreshStatusPill();
    applyDimming();
}

void LpMeterApplet::setRidingAlong(bool riding, qint64 foreignIntervalMs)
{
    const bool changed = (m_ridingAlong != riding);
    m_ridingAlong = riding;
    m_foreignIntervalMs = foreignIntervalMs;
    if (changed) {
        // The pill TEXT depends on this (SHARED vs LIVE), not just the
        // tooltip, so it has to be rebuilt -- an earlier version updated only
        // the tooltip and the pill sat on "LIVE" while we were demonstrably
        // riding along with another client.
        refreshStatusPill();
    } else {
        // Called once per reading at 10 Hz, so avoid re-applying a stylesheet
        // when only the cadence estimate moved.
        m_statusPill->setToolTip(diagnosticTooltip());
    }
}

void LpMeterApplet::setCeilings(const LpMeter::RangeCeilings& ceilings)
{
    m_ceilings = ceilings;
}

QString LpMeterApplet::diagnosticTooltip() const
{
    QStringList lines;
    if (!m_connected) {
        lines << tr("Not connected.");
    } else if (!m_dataFlowing) {
        lines << tr("Connected, but the meter has stopped answering.");
        lines << tr("The LP-100A can stop responding with the serial link "
                    "still healthy; power-cycling it clears that.");
    } else if (m_ridingAlong) {
        if (m_foreignIntervalMs > 0) {
            lines << tr("Another program is polling this meter every %1 ms.")
                         .arg(m_foreignIntervalMs);
        } else {
            lines << tr("Another program is polling this meter.");
        }
        lines << tr("Reading its replies instead of adding our own polls, so "
                    "the update rate follows that program.");
    } else {
        lines << tr("Polling the meter directly at %1 ms.")
                     .arg(LpMeter::PollGate::kSoloPollIntervalMs);
    }

    lines << QString();
    lines << tr("Power range: %1, full scale %2 W%3")
                 .arg(LpMeter::powerRangeName(m_reading.powerRange))
                 .arg(m_ceilingW, 0, 'f', 0)
                 .arg(m_ceilingAutoExpanded
                          ? tr(" (widened automatically — a reading exceeded "
                               "the configured ceiling)")
                          : QString());
    lines << tr("Set the per-range full scale from this tile's right-click "
                "menu; it must match the ranges configured on the meter.");
    return lines.join(QLatin1Char('\n'));
}

void LpMeterApplet::refreshStatusPill()
{
    auto& tm = ThemeManager::instance();
    if (!m_connected) {
        m_statusPill->setText(tr("OFFLINE"));
        tm.applyStyleSheet(m_statusPill, kPillOffline);
    } else if (!m_dataFlowing) {
        // NOT the same as offline, and the distinction is the whole point:
        // the link is fine and the meter is not answering.
        m_statusPill->setText(tr("NO DATA"));
        tm.applyStyleSheet(m_statusPill, kPillStalled);
    } else {
        m_statusPill->setText(m_ridingAlong ? tr("SHARED") : tr("LIVE"));
        tm.applyStyleSheet(m_statusPill, kPillFlowing);
    }
    m_statusPill->setToolTip(diagnosticTooltip());
    m_statusPill->setAccessibleName(m_statusPill->text());
}

void LpMeterApplet::applyDimming()
{
    auto& tm = ThemeManager::instance();
    const bool live = m_connected && m_dataFlowing;
    // The impedance group is dimmed when the record is not a coherent
    // snapshot: at key-up the meter holds power while Z, phase and SWR have
    // already reverted to idle, so those three no longer describe the
    // transmission the power figure came from.
    const bool zLive = live && m_reading.coherent;
    tm.applyStyleSheet(m_dbmLabel, live ? kInfoStyle : kInfoDimStyle);
    tm.applyStyleSheet(m_rlLabel, zLive ? kInfoStyle : kInfoDimStyle);
    tm.applyStyleSheet(m_zLabel, zLive ? kInfoStyle : kInfoDimStyle);
    tm.applyStyleSheet(m_phaseLabel, zLive ? kInfoStyle : kInfoDimStyle);
    tm.applyStyleSheet(m_rangeLabel, live ? kInfoStyle : kInfoDimStyle);
    tm.applyStyleSheet(m_modeLabel, live ? kInfoStyle : kInfoDimStyle);
    tm.applyStyleSheet(m_swrValue, zLive ? kValueStyle : kValueDimStyle);

    // Over-range: the reading exceeded the gauge's full scale, so the needle
    // is pinned and the number is the only honest surface left.
    const bool over = live && m_ceilingW > 0.0 && m_reading.powerW > m_ceilingW;
    tm.applyStyleSheet(m_pwrValue,
                       over ? kValueWarnStyle : (live ? kValueStyle : kValueDimStyle));
}

void LpMeterApplet::refreshLabels()
{
    const LpMeter::Reading& r = m_reading;

    m_pwrValue->setText(r.powerW >= 100.0
                            ? QStringLiteral("%1 W").arg(r.powerW, 0, 'f', 0)
                            : QStringLiteral("%1 W").arg(r.powerW, 0, 'f', 2));
    m_pwrGauge->setAccessibleDescription(
        tr("%1 watts of %2 full scale").arg(r.powerW, 0, 'f', 1).arg(m_ceilingW, 0, 'f', 0));

    m_swrValue->setText(QStringLiteral("%1").arg(r.swr, 0, 'f', 2));
    m_swrGauge->setAccessibleDescription(tr("SWR %1 to 1").arg(r.swr, 0, 'f', 2));

    // dBm exactly as reported, negatives included.
    m_dbmLabel->setText(tr("dBm %1").arg(r.dBm, 0, 'f', 1));
    // returnLossDb() is +infinity at a perfect match, which is correct but
    // claims a precision the 2-decimal SWR field does not carry: a reported
    // "1.00" only justifies ">= ~52 dB". Show it as a lower bound.
    const double rl = LpMeter::returnLossDb(r.swr);
    m_rlLabel->setText(
        std::isfinite(rl)
            ? tr("RL %1 dB").arg(rl, 0, 'f', 1)
            : tr("RL >%1 dB").arg(LpMeter::maxReportableReturnLossDb(), 0, 'f', 0));
    // |Z| and |phase|. No sign, no R+jX — see the class comment.
    m_zLabel->setText(tr("Z %1 Ω").arg(r.zOhms, 0, 'f', 1));
    m_phaseLabel->setText(tr("Phase %1°").arg(r.phaseDeg, 0, 'f', 1));
    m_rangeLabel->setText(tr("Range %1").arg(LpMeter::powerRangeName(r.powerRange)));
    m_modeLabel->setText(LpMeter::peakHoldModeName(r.peakHoldMode));
    m_callsignLabel->setText(r.callsign);

    for (QLabel* l : {m_dbmLabel, m_rlLabel, m_zLabel, m_phaseLabel,
                      m_rangeLabel, m_modeLabel}) {
        l->setAccessibleName(l->text());
    }
    applyDimming();
}

void LpMeterApplet::showContextMenu(const QPoint& pos)
{
    QMenu menu(this);
    auto* header = menu.addAction(tr("Power range full scale"));
    header->setEnabled(false);
    for (int i = 0; i < 3; ++i) {
        QAction* a = menu.addAction(
            tr("%1: %2 W…").arg(LpMeter::powerRangeName(i))
                           .arg(m_ceilings.forRange(i), 0, 'f', 0));
        connect(a, &QAction::triggered, this, [this, i]() { editCeiling(i); });
    }
    menu.addSeparator();
    QAction* reset = menu.addAction(tr("Reset to defaults"));
    connect(reset, &QAction::triggered, this, [this]() {
        m_ceilings = LpMeter::RangeCeilings{};
        emit ceilingsChanged(m_ceilings, -1);
    });
    menu.exec(mapToGlobal(pos));
}

void LpMeterApplet::editCeiling(int rangeIndex)
{
    bool ok = false;
    const double current = m_ceilings.forRange(rangeIndex);
    const double value = QInputDialog::getDouble(
        this, tr("Power range full scale"),
        tr("Full scale for the meter's %1 range, in watts.\n\n"
           "The LP-100A reports which range it is using but never how many "
           "watts that range covers, so this must match the value configured "
           "on the meter itself.")
            .arg(LpMeter::powerRangeName(rangeIndex)),
        current, 1.0, 10000.0, 0, &ok);
    if (!ok) { return; }
    switch (rangeIndex) {
        case 0: m_ceilings.highW = value; break;
        case 1: m_ceilings.midW = value; break;
        case 2: m_ceilings.lowW = value; break;
        default: return;
    }
    emit ceilingsChanged(m_ceilings, rangeIndex);
}

}  // namespace AetherSDR
