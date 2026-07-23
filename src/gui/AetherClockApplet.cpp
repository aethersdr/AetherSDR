#include "AetherClockApplet.h"

#include "ClockAlignmentWidget.h"
#include "ComboStyle.h"
#include "GuardedSlider.h"  // GuardedComboBox
#include "core/AetherClockEngine.h"
#include "core/AetherClockSettings.h"
#include "core/ClockDiagnostics.h"  // WS-7 acquisition telemetry
#include "core/LogManager.h"        // lcClock (debug-pane dual-write)
#include "core/ThemeManager.h"
#include "core/TimeFrameVoter.h"  // ClockStation / ClockLockState
#include "models/AetherClockModel.h"
#include "models/SliceModel.h"  // complete type required by QPointer<SliceModel> assignment

#include <QComboBox>
#include <QDateTime>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QString>
#include <QTextEdit>
#include <QTime>
#include <QTimer>
#include <QVariant>
#include <QVBoxLayout>
#include <QVector>

#include <algorithm>
#include <cmath>

namespace AetherSDR {

namespace {

// Per-item combo roles: the WWV/WWVB carrier (MHz) and the station enum
// value, so the Tune + Start actions can recover the full preset from the
// current selection without a parallel table.
constexpr int kCarrierRole = Qt::UserRole;
constexpr int kStationRole = Qt::UserRole + 1;

// Status-signal colours (NIST time-signal acquisition states). These are
// semantic indicator colours like the style guide's gauge zones, so they
// stay literal rather than routing through the structural token map.
constexpr const char* kLedNoSignal = "#405060";
constexpr const char* kLedAcquiring = "#ffb800";
constexpr const char* kLedLocked = "#00ff88";

// WS-7 verdict display floors — mirrors of the CITED core gate constants
// (PRD-C §5): TimeFrameVoter.h minFramesForLock 2 (WwvDecoder.cpp voter
// config), minLockQuality 0.05 (WwvDecoder.cpp / WwvbDecoder.cpp). Wording
// thresholds only — the funnel never gates anything.
constexpr int kVerdictMinFrames = 2;
constexpr float kVerdictQualityFloor = 0.05f;
// "Too corrupt" percent for the verdict wording. Loose by design: on the
// banked live corpora the decoders classify essentially every second once
// tick-locked even on a dirty band, so a sub-40% classified minute means the
// timing lock itself is chattering — a genuinely borderline band.
constexpr int kVerdictCorruptPct = 40;

// WS-7 funnel stage cell state. The palette reuses the semantic status
// colours of the lock LED family (literal by the same rationale as kLed*).
enum class StageState { Pending, Active, Done };

QLabel* makeStageCell(QWidget* parent)
{
    auto* cell = new QLabel(parent);
    cell->setAlignment(Qt::AlignCenter);
    cell->setMinimumWidth(28);
    return cell;
}

void applyStageStyle(QLabel* cell, StageState st)
{
    if (!cell)
        return;
    const char* color = "#556070";   // pending: dim slate
    const char* border = "#2a3040";
    if (st == StageState::Done) {
        color = kLedLocked;
        border = "#00a060";
    } else if (st == StageState::Active) {
        color = kLedAcquiring;
        border = "#806000";
    }
    cell->setStyleSheet(
        QStringLiteral("QLabel { font-size: 9px; font-weight: bold;"
                       " background: #0a0a18; border: 1px solid %1;"
                       " border-radius: 3px; padding: 1px 2px; color: %2; }")
            .arg(QLatin1String(border), QLatin1String(color)));
}

// Shared themed button chrome (matches the applet style guide standard
// button: compact, bold, token-driven so it live-re-themes).
const QString& kButtonBase()
{
    static const QString s = ThemeManager::instance().resolve(
        "QPushButton { background: {{color.background.1}};"
        " border: 1px solid {{color.background.2}};"
        " border-radius: 3px; color: {{color.text.primary}};"
        " font-size: 10px; font-weight: bold; padding: 2px 4px; }"
        "QPushButton:hover { background: {{color.background.2}}; }");
    return s;
}

// Green "active" checked state — style guide's activate/running family
// (background #006040, text #00ff88, border #00a060).
const QString kStartActive =
    "QPushButton:checked { background-color: #006040; color: #00ff88;"
    " border: 1px solid #00a060; }";

const QString kDisabledBtn =
    "QPushButton:disabled { background-color: #1a1a2a; color: #556070;"
    " border: 1px solid #2a3040; }";

QLabel* makeSettingLabel(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setFixedWidth(52);
    label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    ThemeManager::instance().applyStyleSheet(
        label, "QLabel { color: {{color.text.secondary}}; font-size: 11px; }");
    return label;
}

// Inset value readout (style guide inset pattern: dark well, subtle border,
// centred primary text).
QLabel* makeInsetReadout(QWidget* parent, int minWidth)
{
    auto* label = new QLabel(parent);
    label->setAlignment(Qt::AlignCenter);
    label->setMinimumWidth(minWidth);
    label->setStyleSheet(QStringLiteral(
        "QLabel { font-size: 11px; background: #0a0a18;"
        " border: 1px solid #1e2e3e; border-radius: 3px;"
        " padding: 1px 4px; color: #c8d8e8; }"));
    return label;
}

struct PresetChoice {
    ClockStation station = ClockStation::Wwv;
    double carrierMHz = 0.0;
};

PresetChoice currentChoice(const QComboBox* combo)
{
    PresetChoice c;
    if (!combo)
        return c;
    const int i = combo->currentIndex();
    if (i < 0)
        return c;
    c.carrierMHz = combo->itemData(i, kCarrierRole).toDouble();
    c.station = ClockStation(combo->itemData(i, kStationRole).toInt());
    return c;
}

void applyLockLed(QLabel* led, ClockLockState state)
{
    if (!led)
        return;
    const char* colour = kLedNoSignal;
    switch (state) {
    case ClockLockState::Acquiring:
        colour = kLedAcquiring;
        break;
    case ClockLockState::Locked:
        colour = kLedLocked;
        break;
    case ClockLockState::NoSignal:
    default:
        colour = kLedNoSignal;
        break;
    }
    led->setStyleSheet(
        QStringLiteral("QLabel { background-color: %1; border-radius: 5px; }")
            .arg(QLatin1String(colour)));
}

void applyStationTag(QLabel* tag, const QString& stationName)
{
    if (!tag)
        return;
    tag->setText(stationName == QStringLiteral("Unknown") ? QStringLiteral("--")
                                                          : stationName);
}

void applyOffsetReadout(QLabel* label, const QDateTime& utc, double offsetMs,
                        bool locked)
{
    if (!label)
        return;
    // A stale offset is worse than none: once the lock is lost the last
    // measurement ages open-endedly, so it clears instead of lingering
    // (WS-4.5 — the live applet showed a 45-minute-old offset as current).
    if (!utc.isValid() || !locked) {
        label->setText(QStringLiteral("--"));
        return;
    }
    // + = host clock BEHIND the broadcast (engine offset convention).
    // Format widens gracefully with magnitude: the plausibility gate bounds
    // legitimate offsets to ±24 h, and a badly wrong host clock deserves a
    // readable number, not a 12-digit seconds count.
    const double s = offsetMs / 1000.0;
    const double as = std::abs(s);
    if (as < 100.0)
        label->setText(QString::asprintf("%+.2f s", s));
    else if (as < 600.0)
        label->setText(QString::asprintf("%+.1f s", s));
    else if (as < 3600.0)
        label->setText(QString::asprintf("%+.0f s", s));
    else
        label->setText(QString::asprintf("%+.1f h", s / 3600.0));
}

// Decode age for the trust line: seconds while under 100 ("6s"), then whole
// minutes ("2m") so the compact label never widens the status row.
QString fmtDecodeAge(qint64 ageMs)
{
    const qint64 ageS = ageMs < 0 ? 0 : ageMs / 1000;
    if (ageS < 100)
        return QStringLiteral("%1s").arg(ageS);
    return QStringLiteral("%1m").arg(ageS / 60);
}

// Listening dial (MHz) formatted for the preset note — "9.999 MHz",
// "2.499 MHz", "0.059 MHz" — trailing zeros trimmed sensibly.
QString fmtDialMHz(double mhz)
{
    QString s = QString::number(mhz, 'f', 3);
    if (s.contains(QLatin1Char('.'))) {
        while (s.endsWith(QLatin1Char('0')))
            s.chop(1);
        if (s.endsWith(QLatin1Char('.')))
            s.chop(1);
    }
    return s + QStringLiteral(" MHz");
}

// Tune-button label + the always-visible per-preset dial note follow the
// current preset selection.
void refreshPresetUi(QComboBox* combo, QPushButton* tuneBtn, QLabel* note)
{
    if (!combo)
        return;
    if (tuneBtn) {
        // U+2192 rightwards arrow — a text glyph, not an icon.
        tuneBtn->setText(
            QStringLiteral("Tune slice → %1").arg(combo->currentText()));
    }
    if (note) {
        const PresetChoice c = currentChoice(combo);
        const QString dial =
            fmtDialMHz(AetherClockEngine::listeningDialMHz(c.carrierMHz));
        // The deliberate carrier−1 kHz USB dial (post-demod high-pass
        // workaround) must read as intent, not error.
        QString text =
            QStringLiteral("dial %1 USB — 1 kHz below carrier").arg(dial);
        if (c.station == ClockStation::Wwvb)
            text += QStringLiteral(" · AGC off on tune");
        note->setText(text);
    }
}

} // namespace

AetherClockApplet::AetherClockApplet(QWidget* parent)
    : QWidget(parent)
{
    buildUi();
}

QSize AetherClockApplet::sizeHint() const
{
    const int scopeH = m_scope ? m_scope->sizeHint().height() : 150;
    const int drawerH = (m_settingsDrawer && !m_settingsDrawer->isHidden())
                            ? m_settingsDrawer->sizeHint().height() + 3
                            : 0;
    // The half-width toggle row is still one fixed 20 px row (folded into the
    // +62 baseline); each warning banner adds height only while it is shown,
    // as do the WS-7 funnel row + verdict line (pre-lock only).
    int warnH = 0;
    if (m_daxWarning && !m_daxWarning->isHidden())
        warnH += m_daxWarning->sizeHint().height() + 3;
    if (m_tuneWarning && !m_tuneWarning->isHidden())
        warnH += m_tuneWarning->sizeHint().height() + 3;
    if (m_funnelRow && !m_funnelRow->isHidden())
        warnH += m_funnelRow->sizeHint().height() + 3;
    if (m_verdictLine && !m_verdictLine->isHidden())
        warnH += m_verdictLine->sizeHint().height() + 3;
    return {260, std::max(240, scopeH + drawerH + warnH + 62)};
}

QSize AetherClockApplet::minimumSizeHint() const
{
    const int scopeH = m_scope ? m_scope->minimumSizeHint().height() : 110;
    const int drawerH = (m_settingsDrawer && !m_settingsDrawer->isHidden())
                            ? m_settingsDrawer->minimumSizeHint().height() + 3
                            : 0;
    int warnH = 0;
    if (m_daxWarning && !m_daxWarning->isHidden())
        warnH += m_daxWarning->minimumSizeHint().height() + 3;
    if (m_tuneWarning && !m_tuneWarning->isHidden())
        warnH += m_tuneWarning->minimumSizeHint().height() + 3;
    if (m_funnelRow && !m_funnelRow->isHidden())
        warnH += m_funnelRow->minimumSizeHint().height() + 3;
    if (m_verdictLine && !m_verdictLine->isHidden())
        warnH += m_verdictLine->minimumSizeHint().height() + 3;
    return {220, std::max(180, scopeH + drawerH + warnH + 50)};
}

void AetherClockApplet::buildUi()
{
    theme::setContainer(this, QStringLiteral("applet/clock"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(2, 2, 2, 2);
    layout->setSpacing(3);

    // Alignment scope — the dominant differentiator, takes all stretch.
    m_scope = new ClockAlignmentWidget(this);
    layout->addWidget(m_scope, 1);

    // Status row: lock LED · station tag · decoded UTC · offset.
    {
        auto* row = new QHBoxLayout;
        row->setSpacing(5);

        m_lockLed = new QLabel(this);
        m_lockLed->setFixedSize(10, 10);
        applyLockLed(m_lockLed, ClockLockState::NoSignal);
        row->addWidget(m_lockLed);

        m_stationTag = new QLabel(QStringLiteral("--"), this);
        ThemeManager::instance().applyStyleSheet(
            m_stationTag,
            "QLabel { color: {{color.text.secondary}}; font-size: 11px;"
            " font-weight: bold; }");
        row->addWidget(m_stationTag);

        // Trust line: decode quality + age since the last decoded frame, so a
        // ticking UTC readout can't be misread as a live-locked clock.
        m_trustLine = new QLabel(QStringLiteral("q--"), this);
        ThemeManager::instance().applyStyleSheet(
            m_trustLine,
            "QLabel { color: {{color.text.secondary}}; font-size: 10px; }");
        row->addWidget(m_trustLine);

        row->addStretch(1);

        m_utcValue = makeInsetReadout(this, 60);
        QFont mono = m_utcValue->font();
        mono.setStyleHint(QFont::Monospace);
        mono.setFamily(QStringLiteral("monospace"));
        m_utcValue->setFont(mono);
        row->addWidget(m_utcValue);

        m_offsetValue = makeInsetReadout(this, 52);
        row->addWidget(m_offsetValue);

        layout->addLayout(row);
    }

    // WS-7 acquisition funnel + verdict (PRD-C). Five gating stages rendered
    // as compact state-tinted cells; the verdict line below translates them
    // into plain language, with the terse technical numbers in its tooltip.
    // Every readout is a measured ClockDiagnostics value — nothing
    // display-derived. Visible whenever the engine runs: Ozy's live field
    // review (2026-07-22) superseded the earlier collapse-on-lock call — a
    // lock shows the whole row green rather than vanishing the surface.
    {
        m_funnelRow = new QWidget(this);
        m_funnelRow->setObjectName(QStringLiteral("clockFunnelRow"));
        auto* row = new QHBoxLayout(m_funnelRow);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(3);
        static const char* kStageNames[5] = {"Car", "Tick", "Frm", "Dec", "Vote"};
        static const char* kStageTips[5] = {
            "Stage 1 — carrier: station tone/subcarrier detected",
            "Stage 2 — ticks: second-edge timing sync",
            "Stage 3 — frame: minute frame anchored (marker sync)",
            "Stage 4 — decode: % of the last 60 s classifying into symbols",
            "Stage 5 — vote: frames in the voter window vs lock floor",
        };
        for (int i = 0; i < 5; ++i) {
            m_stageCells[i] = makeStageCell(m_funnelRow);
            m_stageCells[i]->setText(QLatin1String(kStageNames[i]));
            m_stageCells[i]->setToolTip(QLatin1String(kStageTips[i]));
            applyStageStyle(m_stageCells[i], StageState::Pending);
            row->addWidget(m_stageCells[i], 1);
        }
        m_funnelRow->setVisible(false);
        layout->addWidget(m_funnelRow);

        m_verdictLine = new QLabel(this);
        m_verdictLine->setObjectName(QStringLiteral("clockVerdict"));
        m_verdictLine->setWordWrap(true);
        ThemeManager::instance().applyStyleSheet(
            m_verdictLine,
            "QLabel { color: {{color.text.secondary}}; font-size: 10px; }");
        m_verdictLine->setVisible(false);
        layout->addWidget(m_verdictLine);
    }

    // No-DAX warning directly under the status row: amber, word-wrapped, hidden
    // until a running bound slice is found to have no DAX channel (no audio).
    m_daxWarning = new QLabel(
        QStringLiteral("Bound slice has no DAX channel — no audio"), this);
    m_daxWarning->setObjectName(QStringLiteral("clockDaxWarning"));
    m_daxWarning->setWordWrap(true);
    m_daxWarning->setStyleSheet(
        QStringLiteral("QLabel { color: #ffb800; font-size: 10px; }"));
    m_daxWarning->setVisible(false);
    layout->addWidget(m_daxWarning);

    // Tuned-away warning, same amber pattern as clockDaxWarning. The user can
    // always spin the VFO out from under a running decoder — we detect + surface
    // it, never fight it. Text is set in refreshTuneWarning.
    m_tuneWarning = new QLabel(this);
    m_tuneWarning->setObjectName(QStringLiteral("clockTuneWarning"));
    m_tuneWarning->setWordWrap(true);
    m_tuneWarning->setStyleSheet(
        QStringLiteral("QLabel { color: #ffb800; font-size: 10px; }"));
    m_tuneWarning->setVisible(false);
    layout->addWidget(m_tuneWarning);

    // Control row — Enable · Settings · bound-slice tag · DAX chooser. The
    // primary action lives here, not in the drawer: with the shipped WWV
    // default preset most users never need the settings at all. "Enable" is
    // the applet-family toggle convention (DaxApplet precedent): constant
    // label, checkable, green active state.
    {
        auto* controlRow = new QHBoxLayout;
        controlRow->setSpacing(3);

        m_startStopButton = new QPushButton(QStringLiteral("Enable"), this);
        m_startStopButton->setObjectName(QStringLiteral("clockEnable"));
        m_startStopButton->setAccessibleName(QStringLiteral("AetherClock enable"));
        m_startStopButton->setCheckable(true);
        m_startStopButton->setStyleSheet(kButtonBase() + kStartActive + kDisabledBtn);
        m_startStopButton->setFixedHeight(20);
        m_startStopButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        connect(m_startStopButton, &QPushButton::clicked, this, [this]() {
            // A checkable button toggles before this fires, so isChecked() is
            // the user's intended next state; updateStartStopUi reconciles it
            // with the engine's actual running state afterward.
            const bool wantRun = m_startStopButton && m_startStopButton->isChecked();
            if (!m_engine.isNull()) {
                if (wantRun) {
                    if (!m_slice.isNull()) {
                        m_engine->start(m_slice.data(),
                                        currentChoice(m_presetCombo).station);
                        bindAndWatchBoundSlice();
                    }
                } else {
                    m_engine->stop();
                }
            }
            updateStartStopUi();
            refreshDaxUi();
            refreshTuneWarning();
        });
        controlRow->addWidget(m_startStopButton, 1);

        m_drawerToggle = new QPushButton(this);
        m_drawerToggle->setStyleSheet(kButtonBase());
        m_drawerToggle->setFixedHeight(20);
        m_drawerToggle->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        connect(m_drawerToggle, &QPushButton::clicked, this, [this]() {
            setSettingsExpanded(m_settingsDrawer && m_settingsDrawer->isHidden());
        });
        controlRow->addWidget(m_drawerToggle, 1);

        // Bound-slice indicator ("▸<letter>") — empty unless the engine is
        // running; secondary-text themed like the station tag.
        m_boundSliceTag = new QLabel(this);
        m_boundSliceTag->setToolTip(
            QStringLiteral("Slice the running decoder is bound to"));
        ThemeManager::instance().applyStyleSheet(
            m_boundSliceTag,
            "QLabel { color: {{color.text.secondary}}; font-size: 11px;"
            " font-weight: bold; }");
        controlRow->addWidget(m_boundSliceTag);

        // DAX chooser acts on the strip's SELECTED slice — a client-scoped DAX
        // assignment. index == channel (0 = Off, 1-4). Disabled with no slice.
        m_daxCombo = new GuardedComboBox(this);
        m_daxCombo->setObjectName(QStringLiteral("clockDaxCombo"));
        m_daxCombo->setAccessibleName(QStringLiteral("AetherClock DAX channel"));
        applyComboStyle(m_daxCombo);
        m_daxCombo->setFixedHeight(20);
        m_daxCombo->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        m_daxCombo->addItems({QStringLiteral("DAX Off"), QStringLiteral("DAX 1"),
                              QStringLiteral("DAX 2"), QStringLiteral("DAX 3"),
                              QStringLiteral("DAX 4")});
        connect(m_daxCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
                [this](int idx) {
            // Skip the echo while we sync the combo from the model (VfoWidget
            // precedent); index == DAX channel.
            if (m_updatingDaxFromModel)
                return;
            if (!m_slice.isNull())
                m_slice->setDaxChannel(idx);
        });
        controlRow->addWidget(m_daxCombo, 1);

        layout->addLayout(controlRow);
    }

    buildSettingsDrawer();
    layout->addWidget(m_settingsDrawer);

    setSettingsExpanded(false);

    // 1 Hz display tick — extrapolates the UTC readout between decodes and ages
    // the trust line. Coarse timer: this is a human-glance readout, not timing.
    // Runs only while the engine is running AND a decode anchor exists
    // (updateTickTimer), so it's idle in the detached/stopped state.
    m_tickTimer = new QTimer(this);
    m_tickTimer->setInterval(1000);
    m_tickTimer->setTimerType(Qt::CoarseTimer);
    connect(m_tickTimer, &QTimer::timeout, this,
            [this]() { refreshTrustAndTime(); });

    // Prime the status row + action enables to the detached/no-signal state.
    applyStationTag(m_stationTag, QStringLiteral("Unknown"));
    applyOffsetReadout(m_offsetValue, QDateTime(), 0.0, false);
    applyStaticTooltips();
    updateStartStopUi();
    refreshDaxUi();
    refreshTuneWarning();
    refreshTrustAndTime();  // seeds "--:--:--" / "q--" and the dynamic tooltips
}

void AetherClockApplet::buildSettingsDrawer()
{
    m_settingsDrawer = new QFrame(this);

    auto* drawer = new QVBoxLayout(m_settingsDrawer);
    drawer->setContentsMargins(5, 4, 5, 5);
    drawer->setSpacing(4);

    // Station preset combo.
    {
        auto* row = new QHBoxLayout;
        row->setSpacing(5);
        row->addWidget(makeSettingLabel(QStringLiteral("Station:"), m_settingsDrawer));

        m_presetCombo = new GuardedComboBox(m_settingsDrawer);
        m_presetCombo->setObjectName(QStringLiteral("clockPresetCombo"));
        m_presetCombo->setAccessibleName(QStringLiteral("AetherClock station preset"));
        applyComboStyle(m_presetCombo);
        m_presetCombo->setFixedHeight(20);
        m_presetCombo->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        row->addWidget(m_presetCombo, 1);

        drawer->addLayout(row);
    }

    // Tune slice → <preset>.
    m_tuneButton = new QPushButton(m_settingsDrawer);
    m_tuneButton->setStyleSheet(kButtonBase() + kDisabledBtn);
    m_tuneButton->setFixedHeight(20);
    m_tuneButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    connect(m_tuneButton, &QPushButton::clicked, this,
            &AetherClockApplet::applyPresetSelection);
    drawer->addWidget(m_tuneButton);

    // Always-visible per-preset dial note (text set by refreshPresetUi).
    m_presetNote = new QLabel(m_settingsDrawer);
    m_presetNote->setWordWrap(true);
    ThemeManager::instance().applyStyleSheet(
        m_presetNote,
        "QLabel { color: {{color.text.secondary}}; font-size: 10px; }");
    drawer->addWidget(m_presetNote);

    // WS-7 debug-log pane: read-only scrolling diagnostics at the bottom of
    // the drawer behind its own compact toggle (text-glyph convention, mirrors
    // "▸ Settings"). QTextEdit mechanics per the AetherModem decode log
    // (Ax25HfPacketDecodeDialog); chrome via theme tokens per SupportDialog —
    // deliberately NOT AetherModem's pre-token hex blob. Visibility persists
    // in the AetherClock settings blob (debugLogVisible — never a flat key).
    m_debugToggle = new QPushButton(m_settingsDrawer);
    m_debugToggle->setObjectName(QStringLiteral("clockDebugToggle"));
    m_debugToggle->setAccessibleName(QStringLiteral("AetherClock debug log"));
    m_debugToggle->setStyleSheet(kButtonBase());
    m_debugToggle->setFixedHeight(20);
    m_debugToggle->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    connect(m_debugToggle, &QPushButton::clicked, this, [this]() {
        const bool expand = m_debugLog && m_debugLog->isHidden();
        setDebugExpanded(expand);
        AetherClockSettings::setDebugLogVisible(expand);  // user intent only
    });
    drawer->addWidget(m_debugToggle);

    m_debugLog = new QTextEdit(m_settingsDrawer);
    m_debugLog->setObjectName(QStringLiteral("clockDebugLog"));
    m_debugLog->setReadOnly(true);
    m_debugLog->document()->setMaximumBlockCount(2000);
    m_debugLog->setLineWrapMode(QTextEdit::NoWrap);
    m_debugLog->setFixedHeight(110);
    m_debugLog->setPlaceholderText(
        QStringLiteral("AetherClock diagnostics appear here while running."));
    ThemeManager::instance().applyStyleSheet(
        m_debugLog,
        "QTextEdit { background: {{color.background.0}};"
        " color: {{color.text.secondary}};"
        " font-family: monospace; font-size: 10px;"
        " border: 1px solid {{color.background.1}}; }");
    drawer->addWidget(m_debugLog);

    // Restore last visibility (no write-back on restore).
    setDebugExpanded(AetherClockSettings::debugLogVisible());

    // Populate carriers from the engine preset list, then restore the saved
    // selection under a signal block so restore never persists or applies.
    const QVector<double> wwvCarriers = AetherClockEngine::wwvCarrierFrequenciesMHz();
    {
        QSignalBlocker block(m_presetCombo);
        for (double mhz : wwvCarriers) {
            m_presetCombo->addItem(QStringLiteral("WWV %1 MHz").arg(QString::number(mhz)));
            const int idx = m_presetCombo->count() - 1;
            m_presetCombo->setItemData(idx, mhz, kCarrierRole);
            m_presetCombo->setItemData(idx, int(ClockStation::Wwv), kStationRole);
        }
        m_presetCombo->addItem(QStringLiteral("WWVB 60 kHz"));
        {
            const int idx = m_presetCombo->count() - 1;
            m_presetCombo->setItemData(idx, AetherClockEngine::wwvbCarrierFrequencyMHz(),
                                       kCarrierRole);
            m_presetCombo->setItemData(idx, int(ClockStation::Wwvb), kStationRole);
        }

        int sel = 0;
        if (AetherClockSettings::stationPreset() == QStringLiteral("WWVB")) {
            sel = m_presetCombo->count() - 1;
        } else {
            const double carrier = AetherClockSettings::wwvCarrierMHz();
            for (int i = 0; i < m_presetCombo->count(); ++i) {
                if (ClockStation(m_presetCombo->itemData(i, kStationRole).toInt())
                        == ClockStation::Wwv
                    && std::abs(m_presetCombo->itemData(i, kCarrierRole).toDouble()
                                - carrier)
                           < 1e-6) {
                    sel = i;
                    break;
                }
            }
        }
        m_presetCombo->setCurrentIndex(sel);
    }

    connect(m_presetCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int) {
        if (!m_presetCombo)
            return;
        const PresetChoice c = currentChoice(m_presetCombo);
        // Persist the chosen preset only (radio-authoritative tuning is never
        // saved). WWVB keeps the last WWV carrier untouched.
        if (c.station == ClockStation::Wwvb) {
            AetherClockSettings::setStationPreset(QStringLiteral("WWVB"));
        } else {
            AetherClockSettings::setStationPreset(QStringLiteral("WWV"));
            AetherClockSettings::setWwvCarrierMHz(c.carrierMHz);
        }
        refreshPresetUi(m_presetCombo, m_tuneButton, m_presetNote);

        // While RUNNING the dropdown IS the station switch: a running decoder
        // must never keep decoding the old station under a new label (the
        // mismatch window). Retune is unambiguous intent here — stop, re-preset,
        // restart on the new station, and re-bind. While STOPPED the combo stays
        // browse-only (no tune, no start), so nothing happens below.
        if (!m_engine.isNull() && m_engine->isRunning()) {
            if (!m_slice.isNull()) {
                m_engine->stop();
                m_engine->applyStationPreset(m_slice.data(), c.station, c.carrierMHz);
                m_engine->start(m_slice.data(), c.station);
                bindAndWatchBoundSlice();
            } else {
                // Bound slice differs from the strip selection (or none): can't
                // safely retune — stop and let updateStartStopUi reflect it.
                m_engine->stop();
            }
            updateStartStopUi();
            refreshDaxUi();
            refreshTuneWarning();
        }
    });

    refreshPresetUi(m_presetCombo, m_tuneButton, m_presetNote);
}

void AetherClockApplet::setSettingsExpanded(bool expanded)
{
    if (!m_settingsDrawer)
        return;

    m_settingsDrawer->setVisible(expanded);
    if (m_drawerToggle) {
        // U+25BE / U+25B8 triangles — text glyphs, not emoji icons.
        m_drawerToggle->setText(expanded ? QStringLiteral("▾ Settings")
                                         : QStringLiteral("▸ Settings"));
    }
    setMinimumHeight(minimumSizeHint().height());
    updateGeometry();
    adjustSize();
    if (auto* p = parentWidget())
        p->updateGeometry();
}

void AetherClockApplet::setDebugExpanded(bool expanded)
{
    if (!m_debugLog)
        return;
    m_debugLog->setVisible(expanded);
    if (m_debugToggle) {
        // U+25BE / U+25B8 triangles — text glyphs, not emoji icons.
        m_debugToggle->setText(expanded ? QStringLiteral("▾ Debug")
                                        : QStringLiteral("▸ Debug"));
    }
    setMinimumHeight(minimumSizeHint().height());
    updateGeometry();
    adjustSize();
    if (auto* p = parentWidget())
        p->updateGeometry();
}

void AetherClockApplet::appendDebugLine(const QString& tag, const QString& msg)
{
    // Dual-write (the AetherModem lcAx25 precedent): every line also goes to
    // the lcClock LogManager category, so file logging + Support-dialog
    // visibility come for free.
    qCDebug(lcClock).noquote() << tag << msg;
    if (!m_debugLog || !m_debugLog->isVisible())
        return;  // zero GUI cost while the pane is hidden
    m_debugLog->append(QStringLiteral("%1 %2 %3").arg(
        QTime::currentTime().toString(QStringLiteral("HH:mm:ss.zzz")), tag, msg));
    if (auto* sb = m_debugLog->verticalScrollBar())
        sb->setValue(sb->maximum());  // keep the newest line in view
}

void AetherClockApplet::refreshFunnel()
{
    const bool running = !m_engine.isNull() && m_engine->isRunning();
    AetherClockModel* m = m_model.data();
    const ClockLockState st = m ? m->lockState() : ClockLockState::NoSignal;
    // Visible for the whole run — on lock the row reads full green instead of
    // disappearing (Ozy field review 2026-07-22; also keeps the layout stable
    // when a fading band flaps the lock).
    const bool show = running;
    const bool locked = st == ClockLockState::Locked;

    const bool wasShown = m_funnelRow && m_funnelRow->isVisible();
    if (m_funnelRow)
        m_funnelRow->setVisible(show);
    if (m_verdictLine)
        m_verdictLine->setVisible(show);
    if (wasShown != show) {
        setMinimumHeight(minimumSizeHint().height());
        updateGeometry();
        adjustSize();
        if (auto* p = parentWidget())
            p->updateGeometry();
    }
    if (!show || !m)
        return;

    const ClockDiagnostics& d = m->diagnostics();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    // Fail-since clocks for the timed verdict rows (display-side only).
    if (!d.toneDetected) {
        if (!m_stage1FailSinceMs)
            m_stage1FailSinceMs = now;
    } else {
        m_stage1FailSinceMs = 0;
    }
    if (!d.phaseLocked) {
        if (!m_stage2FailSinceMs)
            m_stage2FailSinceMs = now;
    } else {
        m_stage2FailSinceMs = 0;
    }

    // 3-sample quality trend for the verdict's climbing/flat word.
    m_qualityTrend.append(double(d.voteQuality));
    while (m_qualityTrend.size() > 3)
        m_qualityTrend.removeFirst();

    // Stage tinting: passed stages green, the FIRST unpassed stage amber
    // (that's where acquisition currently sits), the rest dim. A lock is the
    // whole ladder holding at once — full greens across the board.
    const bool pass[5] = {
        locked || d.toneDetected,
        locked || d.phaseLocked,
        locked || d.anchored,
        locked || (d.anchored && d.classifiedPct >= kVerdictCorruptPct),
        locked,
    };
    bool activeAssigned = locked;  // no amber while locked
    for (int i = 0; i < 5; ++i) {
        StageState s = StageState::Pending;
        if (pass[i]) {
            s = StageState::Done;
        } else if (!activeAssigned) {
            s = StageState::Active;
            activeAssigned = true;
        }
        applyStageStyle(m_stageCells[i], s);
    }

    // Live cell text for the value-bearing stages.
    if (m_stageCells[0]) {
        m_stageCells[0]->setText(
            d.toneSnrDb > 0.0f
                ? QString::asprintf("Car %.0fdB", double(d.toneSnrDb))
                : QStringLiteral("Car"));
        m_stageCells[0]->setToolTip(
            QString::asprintf("Stage 1 — carrier: SNR %.1f dB, contrast %.1f, "
                              "detected %s",
                              double(d.toneSnrDb), double(d.pwmContrast),
                              d.toneDetected ? "yes" : "no"));
    }
    if (m_stageCells[1]) {
        m_stageCells[1]->setToolTip(
            std::isnan(d.delayEstMs)
                ? QStringLiteral("Stage 2 — ticks: timing sync %1")
                      .arg(d.phaseLocked ? QStringLiteral("locked")
                                         : QStringLiteral("searching"))
                : QString::asprintf(
                      "Stage 2 — ticks: timing sync %s, delay %.0f ms",
                      d.phaseLocked ? "locked" : "searching",
                      double(d.delayEstMs)));
    }
    if (m_stageCells[2]) {
        m_stageCells[2]->setText(
            (d.anchored && m_lastSecondOfFrame >= 0)
                ? QStringLiteral("Frm :%1")
                      .arg(m_lastSecondOfFrame, 2, 10, QLatin1Char('0'))
                : QStringLiteral("Frm"));
        m_stageCells[2]->setToolTip(
            QStringLiteral("Stage 3 — frame: %1 (bad-frame streak %2)")
                .arg(d.anchored ? QStringLiteral("anchored")
                                : QStringLiteral("marker search"))
                .arg(d.badFrameStreak));
    }
    if (m_stageCells[3]) {
        m_stageCells[3]->setText(QStringLiteral("Dec %1%").arg(d.classifiedPct));
        m_stageCells[3]->setToolTip(
            QStringLiteral("Stage 4 — decode: %1% of the last 60 s classified")
                .arg(d.classifiedPct));
    }
    if (m_stageCells[4]) {
        m_stageCells[4]->setText(QStringLiteral("Vote %1/%2")
                                     .arg(d.framesInWindow)
                                     .arg(d.windowSize > 0 ? d.windowSize : 8));
        m_stageCells[4]->setToolTip(
            QString::asprintf("Stage 5 — vote: %d/%d frames, q %.3f (floor "
                              "%.2f), refusal %s",
                              d.framesInWindow, d.windowSize,
                              double(d.voteQuality),
                              double(kVerdictQualityFloor),
                              qPrintable(m->refusalName())));
    }

    if (m_verdictLine) {
        m_verdictLine->setText(verdictText(d));
        // Terse technical tooltip (Ozy §8: plain line, numbers on hover).
        m_verdictLine->setToolTip(QString::asprintf(
            "SNR %.1f dB · contrast %.1f · %d%% classified · %d/%d frames · "
            "q %.3f/%.2f · refusal %s",
            double(d.toneSnrDb), double(d.pwmContrast), d.classifiedPct,
            d.framesInWindow, d.windowSize, double(d.voteQuality),
            double(kVerdictQualityFloor), qPrintable(m->refusalName())));
    }

    // Debug pane VOTE/GATE lines — on change only, not every second.
    if (d.framesInWindow != m_lastLoggedFrames
        || d.refusalReason != m_lastLoggedRefusal) {
        appendDebugLine(QStringLiteral("VOTE"),
                        QString::asprintf("frames=%d/%d q=%.3f refusal=%s",
                                          d.framesInWindow, d.windowSize,
                                          double(d.voteQuality),
                                          qPrintable(m->refusalName())));
        if (d.refusalReason != m_lastLoggedRefusal)
            appendDebugLine(QStringLiteral("GATE"),
                            QStringLiteral("refusal=%1").arg(m->refusalName()));
        m_lastLoggedFrames = d.framesInWindow;
        m_lastLoggedRefusal = d.refusalReason;
    }
}

QString AetherClockApplet::verdictText(const ClockDiagnostics& d) const
{
    // PRD-C §6: evaluated top-down, first match wins; every condition is a
    // measured stage or a real gate verdict — no fabricated probabilities.
    if (!m_model.isNull()
        && m_model->lockState() == ClockLockState::Locked) {
        return QStringLiteral("Locked — all stages green");
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (!d.toneDetected && m_stage1FailSinceMs
        && now - m_stage1FailSinceMs >= 60000) {
        return QStringLiteral("No carrier — check antenna / band / preset dial");
    }
    const bool wwvb = !m_engine.isNull()
                      && m_engine->configuredStation() == ClockStation::Wwvb;
    if (wwvb && !m_boundSlice.isNull()
        && m_boundSlice->agcMode().compare(QStringLiteral("off"),
                                           Qt::CaseInsensitive) != 0) {
        return QStringLiteral(
            "WWVB needs AGC OFF — AGC pumping flattens the PWM");
    }
    if (d.toneDetected && !d.phaseLocked && m_stage2FailSinceMs
        && now - m_stage2FailSinceMs >= 90000) {
        return wwvb ? QStringLiteral("Carrier present, timing not syncing — "
                                     "very weak signal")
                    : QStringLiteral("Carrier present, timing not syncing — "
                                     "very weak; try another WWV frequency");
    }
    if (d.phaseLocked && !d.anchored)
        return QStringLiteral("Syncing frame… (marker search, up to ~2 min)");
    if (d.anchored && d.classifiedPct < kVerdictCorruptPct) {
        return QStringLiteral(
                   "Decoding %1% of seconds — too corrupt to vote; borderline band")
            .arg(d.classifiedPct);
    }
    if (d.anchored && d.framesInWindow < kVerdictMinFrames) {
        QString s = QStringLiteral("Collecting frames — %1/%2")
                        .arg(d.framesInWindow)
                        .arg(kVerdictMinFrames);
        if (m_lastSecondOfFrame >= 0)
            s += QStringLiteral(", next completes in %1 s")
                     .arg(59 - m_lastSecondOfFrame);
        return s;
    }
    if (d.anchored && d.voteQuality < kVerdictQualityFloor) {
        const bool climbing =
            m_qualityTrend.size() >= 3
            && m_qualityTrend.last() > m_qualityTrend.first() + 0.005;
        return QString::asprintf("Voting — q %.3f/%.2f, %s — keep waiting",
                                 double(d.voteQuality),
                                 double(kVerdictQualityFloor),
                                 climbing ? "climbing" : "flat");
    }
    const auto refusal = ClockLockRefusal(d.refusalReason);
    if (refusal == ClockLockRefusal::Plausibility
        || refusal == ClockLockRefusal::Staleness
        || refusal == ClockLockRefusal::Contested) {
        return QStringLiteral("Decode refused: %1 — waiting for cleaner frames")
            .arg(m_model ? m_model->refusalName()
                         : QString::number(d.refusalReason));
    }
    return QStringLiteral("Listening — measuring signal…");
}

void AetherClockApplet::attach(AetherClockEngine* engine, AetherClockModel* model)
{
    if (!m_engine.isNull())
        disconnect(m_engine, nullptr, this, nullptr);
    if (!m_model.isNull())
        disconnect(m_model, nullptr, this, nullptr);

    m_engine = engine;
    m_model = model;

    // Event-driven fields (LED, station, offset) refreshed on any model change
    // — 1 Hz-class traffic, so a full re-read is trivially cheap. The UTC
    // readout + trust line are NOT set here: they tick from the decode anchor
    // (refreshTrustAndTime) so they stay live between the once-per-frame decodes.
    auto syncStatus = [this]() {
        AetherClockModel* m = m_model.data();
        const ClockLockState st = m ? m->lockState() : ClockLockState::NoSignal;
        applyLockLed(m_lockLed, st);
        applyStationTag(m_stationTag, m ? m->stationName() : QStringLiteral("Unknown"));
        const QDateTime utc = m ? m->decodedUtc() : QDateTime();
        applyOffsetReadout(m_offsetValue, utc, m ? m->offsetMs() : 0.0,
                           st == ClockLockState::Locked);
        // A state edge changes what the UTC readout is allowed to do (tick vs
        // stale) — refresh immediately rather than waiting for the 1 Hz tick.
        refreshTrustAndTime();
    };

    if (!m_model.isNull()) {
        connect(m_model, &AetherClockModel::stateChanged, this, syncStatus);
        connect(m_model, &AetherClockModel::stationChanged, this, syncStatus);
        connect(m_model, &AetherClockModel::offsetMsChanged, this, syncStatus);
        // A fresh decode re-anchors the ticking UTC readout + trust age.
        connect(m_model, &AetherClockModel::decodedUtcChanged, this,
                &AetherClockApplet::updateDecodeAnchor);
        // Quality feeds the trust line and the LED tooltip.
        connect(m_model, &AetherClockModel::lockQualityChanged, this,
                &AetherClockApplet::refreshTrustAndTime);
        // WS-7: each ~1 Hz diagnostics snapshot redraws the funnel + verdict;
        // per-frame decodes and state edges feed the debug pane.
        connect(m_model, &AetherClockModel::diagnosticsChanged, this,
                &AetherClockApplet::refreshFunnel);
        connect(m_model, &AetherClockModel::stateChanged, this, [this](int) {
            appendDebugLine(QStringLiteral("STATE"),
                            m_model ? m_model->stateName() : QString());
            refreshFunnel();  // a Locked edge collapses the funnel immediately
        });
        connect(m_model, &AetherClockModel::frameDecoded, this,
                [this](const ClockFrameInfo& f) {
            appendDebugLine(
                QStringLiteral("FRAME"),
                QString::asprintf(
                    "min=%02d hr=%02d doy=%03d yr=%02d conf=%.2f dut1=%+.1f"
                    " dst=%d%d lsw=%d lyi=%d",
                    f.minute, f.hour, f.doy, f.year2,
                    double(f.frameConfidence), f.dut1Tenths / 10.0,
                    f.dst1 ? 1 : 0, f.dst2 ? 1 : 0,
                    f.leapPending ? 1 : 0, f.leapYear ? 1 : 0));
        });
    }

    if (!m_engine.isNull()) {
        connect(m_engine, &AetherClockEngine::alignmentFrame, this,
                [this](const ClockAlignmentFrame& frame) {
            if (m_scope)
                m_scope->appendFrame(frame);
            // WS-7: the funnel's frame-stage countdown + the per-second debug
            // feed (SEC lines only while the pane is visible — zero cost when
            // hidden, per PRD-C).
            m_lastSecondOfFrame = frame.secondOfFrame;
            if (m_debugLog && m_debugLog->isVisible()) {
                appendDebugLine(
                    QStringLiteral("SEC"),
                    QString::asprintf("sof=%d sym=%d conf=%.2f edge=%+dms",
                                      frame.secondOfFrame, frame.symbol,
                                      double(frame.confidence),
                                      frame.edgeOffsetMs));
            }
        });
        connect(m_engine, &AetherClockEngine::timeDecoded, this,
                [this](const QDateTime& utc, double offsetMs, int quality) {
            appendDebugLine(QStringLiteral("DECODE"),
                            QStringLiteral("%1 offset=%2ms q=%3")
                                .arg(utc.toUTC().toString(Qt::ISODate))
                                .arg(offsetMs, 0, 'f', 0)
                                .arg(quality));
        });
        connect(m_engine, &AetherClockEngine::runningChanged, this,
                [this](bool running) {
            updateStartStopUi();
            if (running) {
                // Fresh acquisition: reset the funnel's verdict clocks/trend.
                m_lastSecondOfFrame = -1;
                m_stage1FailSinceMs = 0;
                m_stage2FailSinceMs = 0;
                m_qualityTrend.clear();
                m_lastLoggedFrames = -1;
                m_lastLoggedRefusal = 0;
            }
            if (!running) {
                // A real stop unbinds the slice; drop its DAX + frequency
                // watches so a later channel/dial edit can't revive a
                // running-state warning.
                if (m_boundSliceDaxConn)
                    disconnect(m_boundSliceDaxConn);
                if (m_boundSliceFreqConn)
                    disconnect(m_boundSliceFreqConn);
                m_boundSliceDaxConn = {};
                m_boundSliceFreqConn = {};
                m_boundSlice = nullptr;
                // Drop the decode anchor so the UTC readout falls back to
                // "--:--:--" and the tick stops (updateTickTimer).
                m_anchorUtc = QDateTime();
                m_anchorHostMs = 0;
            }
            refreshDaxUi();
            updateTickTimer();
            refreshTrustAndTime();
            refreshTuneWarning();
            refreshFunnel();
            // History is useful across a NoSignal dropout; only a real stop
            // clears the scope.
            if (!running && m_scope)
                m_scope->clear();
        });
    }

    syncStatus();
    updateStartStopUi();
    refreshDaxUi();
    refreshTuneWarning();
    refreshFunnel();
    updateDecodeAnchor();  // seed the anchor if the model already has a decode
}

void AetherClockApplet::setSlice(SliceModel* slice)
{
    // Store only; never auto-start on bind, and never double-stop — if the
    // engine is running and the slice is lost, the engine stops itself and
    // its runningChanged(false) drives the UI. Here we just refresh enables.
    if (m_sliceDaxConn)
        disconnect(m_sliceDaxConn);
    m_sliceDaxConn = {};
    m_slice = slice;
    if (slice) {
        // The DAX chooser + display track the selected slice's channel.
        m_sliceDaxConn = connect(slice, &SliceModel::daxChannelChanged, this,
                                 [this](int) { refreshDaxUi(); });
    }
    updateStartStopUi();
    refreshDaxUi();
}

void AetherClockApplet::updateStartStopUi()
{
    const bool haveEngine = !m_engine.isNull();
    const bool haveSlice = !m_slice.isNull();
    const bool running = haveEngine && m_engine->isRunning();

    if (m_startStopButton) {
        QSignalBlocker block(m_startStopButton);
        // Constant "Enable" label (applet-family toggle convention) — the
        // checked/green state carries the running signal, never the text.
        m_startStopButton->setChecked(running);
        // Always able to stop while running; only startable when attached to
        // both an engine and a slice.
        m_startStopButton->setEnabled(running || (haveEngine && haveSlice));
    }
    if (m_tuneButton)
        m_tuneButton->setEnabled(haveEngine && haveSlice);
}

void AetherClockApplet::applyPresetSelection()
{
    // Tune acts on the strip's SELECTED slice via the slice-scoped overload,
    // which works while the engine is stopped (it refuses locked slices
    // internally). Tune is enabled whenever an engine + slice are present — it
    // never requires the engine to be running or a slice to be bound.
    if (m_engine.isNull() || m_slice.isNull() || !m_presetCombo)
        return;
    const PresetChoice c = currentChoice(m_presetCombo);
    m_engine->applyStationPreset(m_slice.data(), c.station, c.carrierMHz);
}

void AetherClockApplet::refreshDaxUi()
{
    const bool running = !m_engine.isNull() && m_engine->isRunning();
    SliceModel* bound = m_boundSlice.data();
    SliceModel* sel = m_slice.data();

    // Bound-slice indicator: "▸<letter>" (U+25B8 text glyph), only while running.
    if (m_boundSliceTag) {
        m_boundSliceTag->setText((running && bound)
                                     ? QStringLiteral("▸") + bound->letter()
                                     : QString());
    }

    // DAX chooser tracks the SELECTED slice (the pick a user change acts on).
    // The QSignalBlocker stops the programmatic setCurrentIndex from echoing
    // back as a user edit; the guard flag is the VfoWidget belt-and-suspenders.
    if (m_daxCombo) {
        m_daxCombo->setEnabled(sel != nullptr);
        const int ch = std::clamp(sel ? sel->daxChannel() : 0, 0, 4);
        m_updatingDaxFromModel = true;
        {
            QSignalBlocker block(m_daxCombo);
            m_daxCombo->setCurrentIndex(ch);
        }
        m_updatingDaxFromModel = false;
    }

    // A running bound slice with no DAX channel is the no-audio condition that
    // drives the warning banner.
    const bool noDaxWhileRunning = running && bound && bound->daxChannel() == 0;

    // Warning banner toggles height, so re-run the geometry path (mirroring
    // setSettingsExpanded) only when its visibility actually flips.
    if (m_daxWarning && m_daxWarning->isHidden() == noDaxWhileRunning) {
        m_daxWarning->setVisible(noDaxWhileRunning);
        setMinimumHeight(minimumSizeHint().height());
        updateGeometry();
        adjustSize();
        if (auto* p = parentWidget())
            p->updateGeometry();
    }
}

void AetherClockApplet::updateDecodeAnchor()
{
    // A decode gives the true broadcast second; pin it to the host clock now so
    // the readout can extrapolate forward between the once-per-frame decodes.
    AetherClockModel* m = m_model.data();
    const QDateTime utc = m ? m->decodedUtc() : QDateTime();
    if (utc.isValid()) {
        m_anchorUtc = utc;
        m_anchorHostMs = QDateTime::currentMSecsSinceEpoch();
    } else {
        m_anchorUtc = QDateTime();
        m_anchorHostMs = 0;
    }
    updateTickTimer();
    refreshTrustAndTime();
}

void AetherClockApplet::updateTickTimer()
{
    if (!m_tickTimer)
        return;
    // Tick only when there is something to extrapolate: engine running AND a
    // decode anchor to extrapolate from.
    const bool running = !m_engine.isNull() && m_engine->isRunning();
    const bool active = running && m_anchorUtc.isValid();
    if (active) {
        if (!m_tickTimer->isActive())
            m_tickTimer->start();
    } else if (m_tickTimer->isActive()) {
        m_tickTimer->stop();
    }
}

void AetherClockApplet::refreshTrustAndTime()
{
    // 1 Hz-cheap: only the time-dependent fields (UTC readout, trust line, and
    // their tooltips). The LED / station / offset / DAX surfaces are event-
    // driven elsewhere and deliberately untouched here.
    const bool running = !m_engine.isNull() && m_engine->isRunning();
    const bool haveDecode = m_anchorUtc.isValid();
    AetherClockModel* m = m_model.data();
    const bool locked =
        running && m && m->lockState() == ClockLockState::Locked;

    // Display-side extrapolation: anchorUtc + (now − anchorHostMs), floored to
    // the second. Monotonic, so the readout never ticks backward between
    // anchors. The engine/model are never touched. Extrapolation is honest
    // ONLY while locked — once the lock is lost the anchor ages open-endedly,
    // and ticking it forward presents a stale decode as a live clock (WS-4.5:
    // the live applet ticked a 45-minute-old decode as current).
    QDateTime shownUtc;
    qint64 ageMs = -1;
    if (running && haveDecode)
        ageMs = QDateTime::currentMSecsSinceEpoch() - m_anchorHostMs;
    if (locked && haveDecode)
        shownUtc = m_anchorUtc.addMSecs(ageMs);
    if (m_utcValue) {
        m_utcValue->setText(
            shownUtc.isValid()
                ? shownUtc.toUTC().toString(QStringLiteral("HH:mm:ss"))
                : QStringLiteral("--:--:--"));
    }
    if (m_trustLine) {
        if (locked && haveDecode) {
            m_trustLine->setText(QStringLiteral("q%1 · %2")
                                     .arg(m ? m->lockQuality() : 0)
                                     .arg(fmtDecodeAge(ageMs)));
        } else if (running && haveDecode) {
            // Lock lost but a past decode exists: show its age, never its
            // quality (that certified a decode that is no longer live).
            m_trustLine->setText(
                QStringLiteral("q-- · last %1").arg(fmtDecodeAge(ageMs)));
        } else {
            m_trustLine->setText(QStringLiteral("q--"));
        }
    }
    updateDynamicTooltips();
}

void AetherClockApplet::updateDynamicTooltips()
{
    AetherClockModel* m = m_model.data();
    const bool running = !m_engine.isNull() && m_engine->isRunning();
    const bool haveDecode = m_anchorUtc.isValid();
    const int quality = m ? m->lockQuality() : 0;

    if (m_lockLed) {
        QString tip = QStringLiteral("Lock: %1 — quality %2/100")
                          .arg(m ? m->stateName() : QStringLiteral("NoSignal"))
                          .arg(quality);
        if (!haveDecode)
            tip += QStringLiteral(" · no decode yet");
        m_lockLed->setToolTip(tip);
    }
    if (m_utcValue) {
        QString tip;
        if (running && haveDecode) {
            const qint64 ageS =
                (QDateTime::currentMSecsSinceEpoch() - m_anchorHostMs) / 1000;
            const bool locked =
                m && m->lockState() == ClockLockState::Locked;
            tip = QStringLiteral(
                      "Decoded broadcast time (UTC). Ticks between decodes; "
                      "last decode %1, %2s ago")
                      .arg(m_anchorUtc.toUTC().toString(QStringLiteral("HH:mm:ss")))
                      .arg(ageS < 0 ? 0 : ageS);
            if (!locked)
                tip += QStringLiteral(" — lock lost, readout cleared");
        } else {
            tip = QStringLiteral(
                "Decoded broadcast time (UTC). Ticks between decodes; "
                "no decode yet");
        }
        m_utcValue->setToolTip(tip);
    }
}

void AetherClockApplet::applyStaticTooltips()
{
    if (m_stationTag)
        m_stationTag->setToolTip(QStringLiteral(
            "Detected station (WWV/WWVH auto-tagged by tick band; -- until "
            "confident)"));
    if (m_trustLine)
        m_trustLine->setToolTip(QStringLiteral(
            "Decode quality 0-100 and time since the last decoded frame"));
    if (m_offsetValue)
        m_offsetValue->setToolTip(QStringLiteral(
            "decoded − host at the second edge. Positive = this computer's "
            "clock is behind the broadcast"));
    if (m_scope)
        m_scope->setToolTip(QStringLiteral(
            "Received envelope vs expected symbol template; ticks mark detected "
            "second edges; bar lane = per-second confidence; glyph lane = "
            "decoded symbols (M = marker)"));
    if (m_presetCombo)
        m_presetCombo->setToolTip(QStringLiteral(
            "Station + carrier preset. Tune applies it to the selected slice; "
            "changing it while running restarts decoding on the new station"));
    if (m_tuneButton)
        m_tuneButton->setToolTip(QStringLiteral(
            "Tune the selected slice to the chosen preset — works while "
            "stopped"));
    if (m_startStopButton)
        m_startStopButton->setToolTip(QStringLiteral(
            "Enable or disable decoding on the selected slice"));
    if (m_drawerToggle)
        m_drawerToggle->setToolTip(QStringLiteral(
            "Show or hide the station / tune settings"));
    if (m_daxCombo)
        m_daxCombo->setToolTip(QStringLiteral(
            "Assign the selected slice's DAX channel (0 = Off) — the audio "
            "path. Tracks the selected slice; the warning banner flags a "
            "running bound slice that has no channel"));
}

void AetherClockApplet::bindAndWatchBoundSlice()
{
    // The applet is the sole start caller, so binding here is authoritative for
    // the running-slice display. Both start paths (Start button, running
    // station-switch) funnel through here so the bind + watch block lives once.
    m_boundSlice = m_slice;

    // Record the preset the running decoder is actually on — the combo reflects
    // the just-started / just-switched selection at every call site. Feeds the
    // tuned-away dial math + banner text.
    const PresetChoice c = currentChoice(m_presetCombo);
    m_runningCarrierMHz = c.carrierMHz;
    m_runningPresetLabel =
        m_presetCombo ? m_presetCombo->currentText() : QString();

    if (m_boundSliceDaxConn)
        disconnect(m_boundSliceDaxConn);
    if (m_boundSliceFreqConn)
        disconnect(m_boundSliceFreqConn);
    m_boundSliceDaxConn = {};
    m_boundSliceFreqConn = {};
    if (!m_boundSlice.isNull()) {
        // DAX channel → no-audio warning; frequency → tuned-away warning.
        m_boundSliceDaxConn =
            connect(m_boundSlice.data(), &SliceModel::daxChannelChanged, this,
                    [this](int) { refreshDaxUi(); });
        m_boundSliceFreqConn =
            connect(m_boundSlice.data(), &SliceModel::frequencyChanged, this,
                    [this](double) { refreshTuneWarning(); });
    }
}

void AetherClockApplet::refreshTuneWarning()
{
    const bool running = !m_engine.isNull() && m_engine->isRunning();
    SliceModel* bound = m_boundSlice.data();

    bool tunedAway = false;
    if (running && bound) {
        const double expectedDial =
            AetherClockEngine::listeningDialMHz(m_runningCarrierMHz);
        // 0.5 kHz tolerance — a RIT-sized nudge won't false-trip the banner.
        tunedAway = std::abs(bound->frequency() - expectedDial) > 0.0005;
    }

    if (!m_tuneWarning)
        return;
    if (tunedAway) {
        m_tuneWarning->setText(
            QStringLiteral("Slice tuned away from %1 — no signal")
                .arg(m_runningPresetLabel));
    }
    // Same visibility-flip geometry pattern as the DAX banner: only re-run the
    // geometry path when the shown/hidden state actually changes.
    if (m_tuneWarning->isHidden() == tunedAway) {
        m_tuneWarning->setVisible(tunedAway);
        setMinimumHeight(minimumSizeHint().height());
        updateGeometry();
        adjustSize();
        if (auto* p = parentWidget())
            p->updateGeometry();
    }
}

} // namespace AetherSDR
