#include "StripWaveformPanel.h"

#include "EditorFramelessTitleBar.h"
#include "StripWaveform.h"
#include "core/AppSettings.h"
#include "core/AudioEngine.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QVBoxLayout>

namespace AetherSDR {

namespace {
constexpr const char* kWindowStyle =
    "QWidget { background: #08121d; color: #d7e7f2; }"
    "QLabel  { background: transparent; color: #8aa8c0; font-size: 11px; }";
}

StripWaveformPanel::StripWaveformPanel(AudioEngine* engine, QWidget* parent)
    : QWidget(parent)
    , m_audio(engine)
{
    const QString title = QString::fromUtf8("Aetherial Waveform \xe2\x80\x94 TX");
    setWindowTitle(title);
    setStyleSheet(kWindowStyle);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 0, 8, 0);
    root->setSpacing(6);

    auto* titleBar = new EditorFramelessTitleBar;
    titleBar->setTitleText(title);
    m_titleBar = titleBar;
    // Embedded inside the strip — no need for the min/max/close trio
    // (the strip's own chrome carries those) and the dark fill bar
    // reads as a heavy header in this row.  Drop both so the panel
    // wears just the title text on the panel's own background.
    titleBar->setControlsVisible(false);
    titleBar->setStyleSheet("background: transparent;");

    // View-mode cycle button lives on the title-bar row.  Single press
    // advances Scope → Envelope → History → Scope.  Default starts on
    // Envelope (m_modeIdx = 1) since CE-SSB is fundamentally about
    // envelope behaviour.
    m_modeBtn = new QPushButton(this);
    m_modeBtn->setFixedSize(78, 18);
    m_modeBtn->setStyleSheet(
        "QPushButton {"
        "  background: #1a2a3a; border: 1px solid #2a4458;"
        "  border-radius: 3px; color: #c8a070;"
        "  font-size: 10px; font-weight: bold; padding: 1px 6px;"
        "}"
        "QPushButton:hover { background: #3a2818; color: #f2c14e;"
        "                    border: 1px solid #f2c14e; }");
    m_modeBtn->setToolTip("Cycle waveform view: Scope → Envelope → History");
    connect(m_modeBtn, &QPushButton::clicked,
            this, &StripWaveformPanel::cycleViewMode);

    // Time-window slider (1–20 s) + readout, also on the title row.
    // Adjusts how much wall-clock audio fits across the plot.
    m_windowSlider = new QSlider(Qt::Horizontal, this);
    m_windowSlider->setRange(1, 20);
    m_windowSlider->setSingleStep(1);
    m_windowSlider->setPageStep(1);   // mouse wheel notch = ±1 sec
    m_windowSlider->setFixedWidth(120);
    m_windowSlider->setFixedHeight(14);
    m_windowSlider->setStyleSheet(
        "QSlider::groove:horizontal { height: 4px; background: #1a2a3a;"
        " border-radius: 2px; }"
        "QSlider::sub-page:horizontal { background: #f2c14e;"
        " border-radius: 2px; }"
        "QSlider::handle:horizontal { width: 10px; height: 10px;"
        " margin: -3px 0; background: #ffffff; border: 1px solid #f2c14e;"
        " border-radius: 5px; }"
        "QSlider::handle:horizontal:hover { background: #ffffff;"
        " border: 1px solid #ffd060; }");
    m_windowSlider->setToolTip(
        tr("Waveform time window — how many seconds of audio fit across "
           "the plot.  Range 1–20 s."));
    connect(m_windowSlider, &QSlider::valueChanged,
            this, &StripWaveformPanel::applyWindowSec);

    m_windowLbl = new QLabel(this);
    m_windowLbl->setFixedWidth(28);
    m_windowLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_windowLbl->setStyleSheet(
        "QLabel { background: transparent; color: #8aa8c0;"
        " font-size: 10px; font-weight: bold; }");

    auto* titleRow = new QHBoxLayout;
    titleRow->setContentsMargins(0, 0, 0, 0);
    titleRow->setSpacing(4);
    titleRow->addWidget(titleBar, 1);
    titleRow->addWidget(m_windowSlider);
    titleRow->addWidget(m_windowLbl);
    titleRow->addWidget(m_modeBtn);
    root->addLayout(titleRow);

    m_waveform = new StripWaveform(this);
    // Restore the saved time window (or default to 20 s) and apply
    // it through the slider so the readout label updates in lockstep.
    const int savedSec = std::clamp(
        AppSettings::instance().value(windowSettingsKey(), "20").toInt(),
        1, 20);
    {
        QSignalBlocker b(m_windowSlider);
        m_windowSlider->setValue(savedSec);
    }
    applyWindowSec(savedSec);
    // 90 Hz repaint rate so the long-window scroll reads as smooth
    // motion.  The engine's post-chain scope tap fires up to ~125 Hz
    // (kTxPostChainEmitMinIntervalMs = 8 ms) so the widget always
    // has fresh data on every frame.
    m_waveform->setRefreshRateHz(90);
    // Default render path — pinned TX in the constructor so the
    // initial paint is consistent.  showForRx() flips the pin and
    // re-wires the source tap to the RX-side scope signal.
    m_waveform->setTransmitting(true);
    root->addWidget(m_waveform, 1);

    applyViewMode();

    if (m_audio) {
        // TX-side tap: post-final-limiter — the exact bytes packetised
        // for VITA-49.  Fires only when the user is transmitting.
        connect(m_audio, &AudioEngine::txPostChainScopeReady,
                m_waveform, [this](const QByteArray& mono, int sr) {
            if (m_side != Side::Tx || !m_waveform) return;
            m_waveform->appendScopeSamples(mono, sr, /*tx=*/true);
        });
        // RX-side tap: dedicated rxPostChainScopeReady — same 8 ms
        // throttle as the TX-side feed so the strip's RX scroll tracks
        // wall clock at short windows.  The shared scopeSamplesReady
        // signal that the floating WaveApplet uses keeps its 25 ms
        // throttle, so both consumers get what they need.
        connect(m_audio, &AudioEngine::rxPostChainScopeReady,
                m_waveform, [this](const QByteArray& mono, int sr) {
            if (m_side != Side::Rx || !m_waveform) return;
            m_waveform->appendScopeSamples(mono, sr, /*tx=*/false);
        });
    }
}

StripWaveformPanel::~StripWaveformPanel() = default;

void StripWaveformPanel::showForTx()
{
    m_side = Side::Tx;
    const QString title = QString::fromUtf8("Aetherial Waveform \xe2\x80\x94 TX");
    setWindowTitle(title);
    if (m_titleBar)
        static_cast<EditorFramelessTitleBar*>(m_titleBar)->setTitleText(title);
    if (m_waveform) m_waveform->setTransmitting(true);
    // Restore the TX-side saved zoom window.
    const int savedSec = std::clamp(
        AppSettings::instance().value(windowSettingsKey(), "20").toInt(),
        1, 20);
    if (m_windowSlider) {
        QSignalBlocker b(m_windowSlider);
        m_windowSlider->setValue(savedSec);
    }
    applyWindowSec(savedSec);
    show();
    raise();
    activateWindow();
}

void StripWaveformPanel::showForRx()
{
    m_side = Side::Rx;
    const QString title = QString::fromUtf8("Aetherial Waveform \xe2\x80\x94 RX");
    setWindowTitle(title);
    if (m_titleBar)
        static_cast<EditorFramelessTitleBar*>(m_titleBar)->setTitleText(title);
    // RX side wants the widget to render its RX buffer — flip the
    // transmitting pin off so the StripWaveform falls back to RX.
    if (m_waveform) m_waveform->setTransmitting(false);
    // Restore the RX-side saved zoom window (independent of TX).
    const int savedSec = std::clamp(
        AppSettings::instance().value(windowSettingsKey(), "20").toInt(),
        1, 20);
    if (m_windowSlider) {
        QSignalBlocker b(m_windowSlider);
        m_windowSlider->setValue(savedSec);
    }
    applyWindowSec(savedSec);
    show();
    raise();
    activateWindow();
}

void StripWaveformPanel::syncControlsFromEngine()
{
    // No engine-driven controls yet — kept for API symmetry with the
    // other strip panels so the AetherialAudioStrip can iterate every
    // panel uniformly when applying a preset.
}

void StripWaveformPanel::cycleViewMode()
{
    m_modeIdx = (m_modeIdx + 1) % 3;
    applyViewMode();
}

void StripWaveformPanel::applyWindowSec(int sec)
{
    sec = std::clamp(sec, 1, 20);
    if (m_waveform) m_waveform->setZoomWindowMs(sec * 1000);
    if (m_windowLbl) m_windowLbl->setText(QString("%1 s").arg(sec));
    AppSettings::instance().setValue(windowSettingsKey(), QString::number(sec));
}

QString StripWaveformPanel::windowSettingsKey() const
{
    // Independent persistence per side so TX zoom and RX zoom don't
    // overwrite each other when the user toggles the strip mode.
    return m_side == Side::Rx
        ? QStringLiteral("AetherialStripWaveformRxWindowSec")
        : QStringLiteral("AetherialStripWaveformWindowSec");
}

void StripWaveformPanel::applyViewMode()
{
    if (!m_waveform) return;
    StripWaveform::ViewMode mode = StripWaveform::ViewMode::Envelope;
    QString label = "ENVELOPE";
    switch (m_modeIdx) {
        case 0: mode = StripWaveform::ViewMode::Graph;    label = "SCOPE";    break;
        case 1: mode = StripWaveform::ViewMode::Envelope; label = "ENVELOPE"; break;
        case 2: mode = StripWaveform::ViewMode::Bars;     label = "HISTORY";  break;
    }
    m_waveform->setViewMode(mode);
    if (m_modeBtn) m_modeBtn->setText(label);
}

} // namespace AetherSDR
