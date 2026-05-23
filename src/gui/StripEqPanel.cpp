#include "StripEqPanel.h"
#include "ClientEqEditorCanvas.h"
#include "ClientEqFftAnalyzer.h"
#include "ClientEqIconRow.h"
#include "ClientEqOutputFader.h"
#include "ClientEqParamRow.h"
#include "ComboStyle.h"
#include "EditorFramelessTitleBar.h"
#include "core/AppSettings.h"
#include "core/AudioEngine.h"
#include "core/ClientEq.h"

#include <QCloseEvent>
#include <QComboBox>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QLabel>
#include <QMoveEvent>
#include <QPushButton>
#include <QResizeEvent>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>
#include <vector>

namespace MasterSDR {

namespace {

constexpr int kDefaultWidth  = 900;
constexpr int kDefaultHeight = 520;

constexpr const char* kWindowStyle =
    "QWidget { background: #08121d; color: #d7e7f2; }"
    "QLabel  { background: transparent; color: #8aa8c0; font-size: 11px; }";

// Bypass button visual: unchecked = EQ active (subtle), checked = bypass
// engaged (amber, signals "this is muting your work").  Plugin convention.
const QString kBypassStyle = QStringLiteral(
    "QPushButton {"
    "  background: #0e1b28;"
    "  color: #8aa8c0;"
    "  border: 1px solid #243a4e;"
    "  border-radius: 3px;"
    "  font-size: 11px;"
    "  font-weight: bold;"
    "  padding: 3px 12px;"
    "}"
    "QPushButton:hover { background: #1a2a3a; }"
    "QPushButton:checked {"
    "  background: #3a2a0e;"
    "  color: #f2c14e;"
    "  border: 1px solid #f2c14e;"
    "}"
    "QPushButton:checked:hover { background: #4a3a1e; }");

} // namespace

StripEqPanel::StripEqPanel(AudioEngine* engine, QWidget* parent)
    : QWidget(parent)
    , m_audio(engine)
{
    setWindowTitle("Masterial Parametric EQ");
    setStyleSheet(kWindowStyle);
    resize(kDefaultWidth, kDefaultHeight);

    auto* root = new QVBoxLayout(this);
    // Trim top + bottom margins so the title bar hugs the top edge and
    // the param-row labels sit flush at the bottom — the previous 8 px
    // bottom margin was crowding the band plan against the labels.
    root->setContentsMargins(8, 0, 8, 0);
    root->setSpacing(6);

    // Custom 20 px-tall title bar with the active path heading on the
    // left and the min / max / close trio on the right.  Press-and-drag
    // anywhere on it starts a window move.  Stored as a QWidget* in the
    // header to avoid leaking the inline class — cast at use sites.
    auto* titleBar = new EditorFramelessTitleBar;
    m_titleBar = titleBar;
    root->addWidget(titleBar);

    // Interaction hint + filter family + bypass strip.  Path heading
    // lives in the frameless title bar above instead.
    {
        auto* row = new QHBoxLayout;
        row->setSpacing(8);
        auto* hint = new QLabel(
            "Drag peak/shelf = freq + gain · "
            "drag HP/LP = freq + Q · Shift + drag for Q · "
            "click icon to cycle type");
        hint->setStyleSheet("QLabel { color: #506070; font-size: 10px; }");
        row->addWidget(hint, 1);

        // Smoothing — fractional-octave display smoothing for the FFT
        // analyzer trace.  Doesn't affect EQ math, just the visual.
        // Persisted globally (single user preference, shared between RX
        // and TX editors).
        auto* smoothingLbl = new QLabel("Smoothing:");
        smoothingLbl->setStyleSheet(
            "QLabel { color: #c8d8e8; font-size: 11px; font-weight: bold; }");
        row->addWidget(smoothingLbl);

        auto* smoothingCombo = new QComboBox;
        smoothingCombo->addItem("Off (1/96)", 96);
        smoothingCombo->addItem("1/24",       24);
        smoothingCombo->addItem("1/12",       12);
        smoothingCombo->addItem("1/6",         6);
        smoothingCombo->addItem("1/3",         3);
        smoothingCombo->setFixedHeight(24);
        smoothingCombo->setToolTip(
            "Fractional-octave smoothing applied to the analyzer trace.\n"
            "Lower fraction = smoother (1/3 = most, 1/96 = off).\n"
            "Affects display only — EQ math is unchanged.");
        MasterSDR::applyComboStyle(smoothingCombo);

        const int savedFraction = AppSettings::instance()
            .value("ClientEqSmoothingFraction", "96").toInt();
        const int savedIdx = smoothingCombo->findData(savedFraction);
        smoothingCombo->setCurrentIndex(savedIdx >= 0 ? savedIdx : 0);
        // Cache the saved fraction on a member so it can be applied to
        // m_canvas later — the canvas isn't constructed yet at this point
        // in the toolbar setup.  Pushing to m_canvas here was a no-op
        // because m_canvas is still nullptr.
        m_savedSmoothingFraction = savedFraction;

        connect(smoothingCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this, smoothingCombo](int idx) {
            const int n = smoothingCombo->itemData(idx).toInt();
            if (m_canvas) m_canvas->setSmoothingOctaveFraction(n);
            AppSettings::instance().setValue(
                "ClientEqSmoothingFraction", QString::number(n));
            AppSettings::instance().save();
        });
        row->addWidget(smoothingCombo);

        // Peak Hold — when checked, the analyzer's per-bin peak trace
        // stops decaying so the highest level seen at each frequency
        // stays put.  Useful for spotting resonances while tuning.
        auto* peakHoldBtn = new QPushButton("Peak Hold");
        peakHoldBtn->setCheckable(true);
        peakHoldBtn->setFixedHeight(24);
        peakHoldBtn->setToolTip(
            "Freeze the analyzer peak-hold trace at its highest level.\n"
            "Toggle off to resume normal decay.");
        peakHoldBtn->setStyleSheet(
            "QPushButton {"
            "  background: #0e1b28; color: #c8d8e8;"
            "  border: 1px solid #243a4e; border-radius: 3px;"
            "  padding: 2px 12px; font-size: 11px; font-weight: bold;"
            "}"
            "QPushButton:hover { background: #1a2a3a; }"
            "QPushButton:checked {"
            "  background: #c8a040; color: #0a0a18;"
            "  border-color: #d4b050;"
            "}"
            "QPushButton:checked:hover { background: #d4b050; }");
        connect(peakHoldBtn, &QPushButton::toggled, this, [this](bool on) {
            if (m_canvas) m_canvas->setPeakHoldFrozen(on);
        });
        row->addWidget(peakHoldBtn);

        // Global filter-family selector — applies to HP/LP cascade math.
        // Shelves and peaks keep their native 2nd-order topology.
        m_familyCombo = new QComboBox;
        m_familyCombo->addItem("Butterworth",
            static_cast<int>(ClientEq::FilterFamily::Butterworth));
        m_familyCombo->addItem("Chebyshev",
            static_cast<int>(ClientEq::FilterFamily::Chebyshev));
        m_familyCombo->addItem("Bessel",
            static_cast<int>(ClientEq::FilterFamily::Bessel));
        m_familyCombo->addItem("Elliptic",
            static_cast<int>(ClientEq::FilterFamily::Elliptic));
        m_familyCombo->setFixedHeight(24);
        m_familyCombo->setToolTip(
            "Filter family for HP / LP cascade math.\n"
            "• Butterworth — maximally flat passband\n"
            "• Chebyshev — steeper transition, 1 dB passband ripple\n"
            "• Bessel — linear phase, gentler rolloff\n"
            "• Elliptic — steepest transition, ripple in both bands");
        MasterSDR::applyComboStyle(m_familyCombo);
        connect(m_familyCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this](int idx) {
            ClientEq* eq = (m_path == ClientEqApplet::Path::Rx)
                ? m_audio->clientEqRx() : m_audio->clientEqTx();
            if (!eq) return;
            eq->setFilterFamily(static_cast<ClientEq::FilterFamily>(idx));
            if (m_audio) m_audio->saveClientEqSettings();
            if (m_canvas) m_canvas->update();
        });
        row->addWidget(m_familyCombo);

        // Reset — drops every band back to ClientEq::defaultBand(i),
        // restores the default 10-band count, and resets the filter
        // family to Butterworth.  Saves immediately so the wipe
        // survives a restart.
        auto* resetBtn = new QPushButton("Reset");
        resetBtn->setFixedHeight(24);
        resetBtn->setToolTip("Reset all bands to default values");
        resetBtn->setStyleSheet(
            "QPushButton {"
            "  background: #0e1b28; color: #c8d8e8;"
            "  border: 1px solid #243a4e; border-radius: 3px;"
            "  padding: 2px 12px; font-size: 11px; font-weight: bold;"
            "}"
            "QPushButton:hover { background: #1a2a3a; }"
            "QPushButton:pressed { background: #243a4e; }");
        connect(resetBtn, &QPushButton::clicked, this, [this]() {
            ClientEq* eq = (m_path == ClientEqApplet::Path::Rx)
                ? m_audio->clientEqRx() : m_audio->clientEqTx();
            if (!eq) return;
            eq->setActiveBandCount(ClientEq::kDefaultBandCount);
            for (int i = 0; i < ClientEq::kDefaultBandCount; ++i) {
                eq->setBand(i, ClientEq::defaultBand(i));
            }
            eq->setFilterFamily(ClientEq::FilterFamily::Butterworth);
            if (m_audio) m_audio->saveClientEqSettings();

            if (m_familyCombo) {
                QSignalBlocker b(m_familyCombo);
                m_familyCombo->setCurrentIndex(0);   // Butterworth
            }
            if (m_canvas)   m_canvas->update();
            if (m_iconRow)  m_iconRow->update();
            // refresh() reloads each column's freq / gain / Q text from
            // the engine; update() alone just repaints stale labels.
            if (m_paramRow) m_paramRow->refresh();
        });
        row->addWidget(resetBtn);

        // Bypass button moved to the CHAIN widget's single-click
        // gesture.  Keyboard shortcut retired along with the button.

        root->addLayout(row);
    }

    // Main body: icon row + canvas + param row stacked vertically, with
    // the output fader in a sibling column on the right.  The fader spans
    // the full height of the EQ strip so its meter aligns with the
    // canvas's visible range.
    auto* body = new QHBoxLayout;
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(8);

    auto* eqColumn = new QVBoxLayout;
    eqColumn->setContentsMargins(0, 0, 0, 0);
    eqColumn->setSpacing(6);

    m_iconRow = new ClientEqIconRow;
    m_iconRow->setAudioEngine(m_audio);
    eqColumn->addWidget(m_iconRow);

    m_canvas = new ClientEqEditorCanvas;
    m_canvas->setAudioEngine(m_audio);
    // Apply the saved smoothing fraction now that the canvas exists.
    // The combo box already shows the right value from the toolbar build,
    // but the canvas needed to be constructed before this push could land.
    m_canvas->setSmoothingOctaveFraction(m_savedSmoothingFraction);
    eqColumn->addWidget(m_canvas, 1);
    // Forward cutoff-line drag events as a path-tagged signal so MainWindow
    // can dispatch to TransmitModel (TX) or the active SliceModel (RX).
    connect(m_canvas, &ClientEqEditorCanvas::cutoffsDragged,
            this, [this](int audioLow, int audioHigh) {
        emit cutoffsDragRequested(m_path, audioLow, audioHigh);
    });

    m_paramRow = new ClientEqParamRow;
    eqColumn->addWidget(m_paramRow);

    body->addLayout(eqColumn, 1);

    // Output fader — vertical meter + slider + dB readout on the right.
    m_outFader = new ClientEqOutputFader;
    connect(m_outFader, &ClientEqOutputFader::gainChanged,
            this, [this](float linear) {
        ClientEq* eq = (m_path == ClientEqApplet::Path::Rx)
            ? m_audio->clientEqRx() : m_audio->clientEqTx();
        if (!eq) return;
        eq->setMasterGain(linear);
        if (m_audio) m_audio->saveClientEqSettings();
    });
    body->addWidget(m_outFader);

    root->addLayout(body, 1);

    // Selection plumbing: any of the three views announcing a selection
    // change fans out to the other two, plus triggers a paint refresh.
    connect(m_canvas, &ClientEqCurveWidget::selectedBandChanged,
            this, &StripEqPanel::syncSelection);
    connect(m_iconRow, &ClientEqIconRow::bandSelected,
            this, &StripEqPanel::syncSelection);
    connect(m_paramRow, &ClientEqParamRow::bandSelected,
            this, &StripEqPanel::syncSelection);

    // Numeric entries committed via the param row's right-click menu
    // need the same persist + redraw fan-out a canvas drag triggers
    // (issue #2655).  refreshValues() already happened inside the row.
    connect(m_paramRow, &ClientEqParamRow::bandEdited,
            this, [this](int) {
        if (m_audio)    m_audio->saveClientEqSettings();
        if (m_canvas)   m_canvas->update();
        if (m_iconRow)  m_iconRow->refresh();
    });

    // Any canvas-side band mutation refreshes the text-valued widgets
    // (icon row rebuilds on type / count change; param row updates
    // numeric display live during drags).
    connect(m_canvas, &ClientEqCurveWidget::bandsChanged,
            this, [this]() {
        if (m_iconRow)  m_iconRow->refresh();
        if (m_paramRow) {
            m_paramRow->refresh();
            m_paramRow->setSelectedBand(m_canvas->selectedBand());
        }
    });

    // FFT analyzer ticks on a QTimer while the editor is visible.  Pulls
    // the most-recent post-EQ samples from AudioEngine, runs the FFT on
    // the UI thread (microseconds at 256 points), and pushes smoothed
    // bins into the canvas.  The timer is stopped in hideEvent so it
    // doesn't burn CPU while the editor is closed.
    m_fftAnalyzer = std::make_unique<ClientEqFftAnalyzer>();
    m_fftTimer = new QTimer(this);
    m_fftTimer->setInterval(40);  // 25 Hz
    connect(m_fftTimer, &QTimer::timeout,
            this, &StripEqPanel::tickFftAnalyzer);

    restoreGeometryFromSettings();
}

StripEqPanel::~StripEqPanel() = default;

void StripEqPanel::tickFftAnalyzer()
{
    if (!m_audio || !m_canvas || !m_fftAnalyzer) return;

    std::vector<float> samples(ClientEqFftAnalyzer::kFftSize, 0.0f);
    bool ok = false;
    if (m_path == ClientEqApplet::Path::Rx) {
        ok = m_audio->copyRecentClientEqRxSamples(
                samples.data(), ClientEqFftAnalyzer::kFftSize);
    } else {
        ok = m_audio->copyRecentClientEqTxSamples(
                samples.data(), ClientEqFftAnalyzer::kFftSize);
    }
    if (!ok) return;

    m_fftAnalyzer->update(samples.data(), ClientEqFftAnalyzer::kFftSize);

    ClientEq* eq = (m_path == ClientEqApplet::Path::Rx)
        ? m_audio->clientEqRx() : m_audio->clientEqTx();
    const double fs = eq ? eq->sampleRate() : 24000.0;
    m_canvas->setFftBinsDb(m_fftAnalyzer->magnitudesDb(), fs);

    // Feed the output fader a peak level from the same samples.  Cheap —
    // one pass over the 2048-sample block while we already have it warm.
    if (m_outFader) {
        float peak = 0.0f;
        for (float s : samples) {
            peak = std::max(peak, std::fabs(s));
        }
        m_outFader->setPeakLinear(peak);
    }
}

void StripEqPanel::syncBypassFromEq()
{
    // No in-editor bypass control — bypass lives on the CHAIN widget.
    // Left as a no-op so existing callers compile; the canvas still
    // recolours itself via its own enabled check.
}

void StripEqPanel::syncSelection(int idx)
{
    if (m_iconRow)  m_iconRow->setSelectedBand(idx);
    if (m_canvas)   m_canvas->setSelectedBand(idx);
    if (m_paramRow) m_paramRow->setSelectedBand(idx);
    // Param row values also move under drags / type cycles, so refresh
    // display text whenever anything gets selected.
    if (m_paramRow) m_paramRow->refreshValues();
}

void StripEqPanel::showForPath(ClientEqApplet::Path path)
{
    m_path = path;
    if (!m_audio || !m_canvas) return;

    ClientEq* eq = (path == ClientEqApplet::Path::Rx)
        ? m_audio->clientEqRx() : m_audio->clientEqTx();
    m_canvas->setEq(eq);
    if (m_iconRow)  m_iconRow->setEq(eq);
    if (m_paramRow) m_paramRow->setEq(eq);
    if (m_outFader && eq) m_outFader->setGainLinear(eq->masterGain());
    if (m_familyCombo && eq) {
        QSignalBlocker b(m_familyCombo);
        m_familyCombo->setCurrentIndex(static_cast<int>(eq->filterFamily()));
    }
    syncBypassFromEq();
    // Clear selection on path swap — the previously-selected index
    // almost certainly doesn't correspond to the other path's bands.
    syncSelection(-1);
    const QString title = path == ClientEqApplet::Path::Rx
        ? QStringLiteral("Masterial Parametric EQ — RX")
        : QStringLiteral("Masterial Parametric EQ — TX");
    // m_titleBar is always an EditorFramelessTitleBar* — kept as
    // QWidget* in the header so the inline class stays cpp-only.
    if (m_titleBar)
        static_cast<EditorFramelessTitleBar*>(m_titleBar)->setTitleText(title);
    setWindowTitle(title);

    // Re-apply the cached cutoffs for whichever path we're switching to,
    // so RX/TX swaps restore the correct dashed guides without waiting
    // for the next slice/filter event.
    if (m_canvas) {
        if (path == ClientEqApplet::Path::Rx)
            m_canvas->setFilterCutoffs(m_rxFilterLowCutHz, m_rxFilterHighCutHz);
        else
            m_canvas->setFilterCutoffs(m_txFilterLowCutHz, m_txFilterHighCutHz);
    }

    if (!isVisible()) {
        show();
    }
    raise();
    activateWindow();
}

void StripEqPanel::setTxFilterCutoffs(int lowHz, int highHz)
{
    m_txFilterLowCutHz = lowHz;
    m_txFilterHighCutHz = highHz;
    if (m_canvas && m_path == ClientEqApplet::Path::Tx)
        m_canvas->setFilterCutoffs(lowHz, highHz);
}

void StripEqPanel::setRxFilterCutoffs(int audioLowHz, int audioHighHz)
{
    m_rxFilterLowCutHz = audioLowHz;
    m_rxFilterHighCutHz = audioHighHz;
    if (m_canvas && m_path == ClientEqApplet::Path::Rx)
        m_canvas->setFilterCutoffs(audioLowHz, audioHighHz);
}

void StripEqPanel::refreshFromEngine()
{
    ClientEq* eq = (m_path == ClientEqApplet::Path::Rx)
        ? (m_audio ? m_audio->clientEqRx() : nullptr)
        : (m_audio ? m_audio->clientEqTx() : nullptr);
    if (m_iconRow)  m_iconRow->refresh();
    if (m_canvas)   m_canvas->update();
    if (m_paramRow) m_paramRow->refresh();
    if (eq && m_familyCombo) {
        QSignalBlocker b(m_familyCombo);
        m_familyCombo->setCurrentIndex(static_cast<int>(eq->filterFamily()));
    }
    syncBypassFromEq();
}

void StripEqPanel::closeEvent(QCloseEvent* ev)
{
    saveGeometryToSettings();
    QWidget::closeEvent(ev);
}

void StripEqPanel::showEvent(QShowEvent* ev)
{
    QWidget::showEvent(ev);
    if (m_fftAnalyzer) m_fftAnalyzer->reset();
    if (m_fftTimer && !m_fftTimer->isActive()) m_fftTimer->start();
}

void StripEqPanel::hideEvent(QHideEvent* ev)
{
    QWidget::hideEvent(ev);
    if (m_fftTimer) m_fftTimer->stop();
    // Clear the canvas's last FFT snapshot so it doesn't paint stale
    // energy next time the window opens.
    if (m_canvas) m_canvas->setFftBinsDb({}, 24000.0);
}

void StripEqPanel::moveEvent(QMoveEvent* ev)
{
    QWidget::moveEvent(ev);
    if (!m_restoring && isVisible()) saveGeometryToSettings();
}

void StripEqPanel::resizeEvent(QResizeEvent* ev)
{
    QWidget::resizeEvent(ev);
    if (!m_restoring && isVisible()) saveGeometryToSettings();
}

void StripEqPanel::saveGeometryToSettings()
{
    auto& s = AppSettings::instance();
    const QRect g = geometry();
    s.setValue("StripEqPanel_X", QString::number(g.x()));
    s.setValue("StripEqPanel_Y", QString::number(g.y()));
    s.setValue("StripEqPanel_W", QString::number(g.width()));
    s.setValue("StripEqPanel_H", QString::number(g.height()));
    s.save();
}

void StripEqPanel::restoreGeometryFromSettings()
{
    m_restoring = true;
    auto& s = AppSettings::instance();
    const int w = s.value("StripEqPanel_W",
                          QString::number(kDefaultWidth)).toString().toInt();
    const int h = s.value("StripEqPanel_H",
                          QString::number(kDefaultHeight)).toString().toInt();
    resize(std::max(w, 600), std::max(h, 320));

    // Only honour saved position if it's non-default — otherwise let the
    // window manager pick a reasonable spawn spot.
    const QString xs = s.value("StripEqPanel_X", "").toString();
    const QString ys = s.value("StripEqPanel_Y", "").toString();
    if (!xs.isEmpty() && !ys.isEmpty()) {
        move(xs.toInt(), ys.toInt());
    }
    m_restoring = false;
}

} // namespace MasterSDR
