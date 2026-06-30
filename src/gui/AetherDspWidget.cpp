#include "AetherDspWidget.h"
#include "core/AudioEngine.h"
#include "core/AppSettings.h"
#include "core/NvidiaBnrSettings.h"
#include "GuardedSlider.h"
#include "Theme.h"

#include <QRegularExpression>
#include <QSet>
#include <QHash>

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QStackedWidget>
#include <QRadioButton>
#include <QButtonGroup>
#include <QLineEdit>
#include <QProgressBar>
#include <QMessageBox>
#ifdef HAVE_NVIDIA_AFX
#include "core/NvidiaAfxPack.h"
#endif
#include <QCheckBox>
#include <QLabel>
#include <QPushButton>
#include <QPainter>
#include <QSignalBlocker>
#include "core/ThemeManager.h"

namespace AetherSDR {

namespace {

const QString kWidgetStyle = QStringLiteral(
    "QWidget { color: #c8d8e8; }"
    "QTabWidget::pane { border: 1px solid #304050; background: #0f0f1a; }"
    "QTabBar::tab { background: #1a2a3a; color: #8090a0; padding: 6px 16px;"
    "  border: 1px solid #304050; border-bottom: none; border-radius: 3px 3px 0 0; }"
    "QTabBar::tab:selected { background: #0f0f1a; color: #c8d8e8;"
    "  border-bottom: 1px solid #0f0f1a; }"
    "QGroupBox { border: 1px solid #304050; border-radius: 4px;"
    "  margin-top: 12px; padding-top: 8px; color: #8090a0; }"
    "QGroupBox::title { subcontrol-origin: margin; left: 8px; padding: 0 4px; }"
    "QLabel { color: #8090a0; }"
    "QRadioButton { color: #c8d8e8; }"
    "QCheckBox { color: #c8d8e8; }"
    "QPushButton { background: #1a2a3a; color: #c8d8e8; border: 1px solid #304050;"
    "  border-radius: 3px; padding: 4px 12px; }"
    "QPushButton:hover { background: rgba(0, 112, 192, 180); border: 1px solid #0090e0; }");

// Compact variant — applied when the widget is embedded inside the docked
// PooDoo applet (≤280 px wide).  Tighter tab padding so all 6 tabs fit on
// one row, smaller GroupBox / control margins, narrower slider value
// labels.  The Settings-menu dialog leaves this off.
// Toggle-button look matching the slice DSP buttons (NB / NR / ANF / NRL /
// NRS / NRF / ANFL / BNR).  Used for exclusive-selection groups that are
// otherwise rendered as radio buttons inside a QGroupBox.  Keeps the row
// tight and consistent with the rest of the app.
const QString kToggleStyle = QStringLiteral(
    "QPushButton { background: #1a2a3a; border: 1px solid #205070;"
    "  border-radius: 3px; color: #c8d8e8; font-size: 9px;"
    "  font-weight: bold; padding: 0px 2px; margin: 0px; }"
    "QPushButton:hover { background: #204060; }"
    "QPushButton:checked { background: #0070c0; color: #ffffff;"
    "  border: 1px solid #0090e0; }"
    "QPushButton:disabled { background: #0e1822; color: #4a5868;"
    "  border: 1px solid #1a2838; }");

static QPushButton* makeToggle(const QString& text)
{
    auto* b = new QPushButton(text);
    b->setCheckable(true);
    // Preferred (not Expanding) so each button sizes to its text — the
    // row no longer divides space equally between "Log" and "Trained",
    // which kept clipping the longer labels at 280 px container width.
    b->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    b->setFixedHeight(16);
    b->setStyleSheet(kToggleStyle);
    return b;
}

// Compact reset icon — flat clickable QPushButton that paints its glyph
// rotated 90° CCW through QPainter (Qt stylesheets don't support
// transform).  Glyph is U+21BA "anticlockwise open circle arrow" — the
// conventional undo symbol.  Each parametric tab adds one and clicking
// dispatches to resetCurrentTab().
class ResetIconButton : public QPushButton {
public:
    explicit ResetIconButton(QWidget* parent = nullptr) : QPushButton(parent)
    {
        setToolTip("Reset Defaults");
        setFlat(true);
        setCursor(Qt::PointingHandCursor);
        // Tight to the glyph — no top/side padding.  The 16-px font fits
        // exactly inside an 18×18 click target so the rotated arrow
        // touches the top and side edges.
        setFixedSize(18, 18);
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        setStyleSheet(
            "QPushButton { background: transparent; border: 0; padding: 0;"
            "  margin: 0; }");
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::TextAntialiasing);
        p.setRenderHint(QPainter::Antialiasing);
        QColor c("#8090a0");
        if (isDown())        c = QColor("#00b4d8");
        else if (underMouse()) c = QColor("#c8d8e8");
        p.setPen(c);
        QFont f = font();
        f.setPixelSize(18);
        p.setFont(f);
        p.translate(width() / 2.0, height() / 2.0);
        p.rotate(-90.0);
        QRectF box(-width() / 2.0, -height() / 2.0, width(), height());
        p.drawText(box, Qt::AlignCenter, QString::fromUtf8("\xE2\x86\xBA"));
    }
};

static QPushButton* makeResetIconButton()
{
    return new ResetIconButton;
}

const QString kCompactWidgetStyle = QStringLiteral(
    "QWidget { color: #c8d8e8; font-size: 10px; }"
    "QTabWidget::pane { border: 0; background: transparent; top: -1px; }"
    "QTabBar::tab { background: #1a2a3a; color: #8090a0; padding: 3px 6px;"
    "  font-size: 10px; min-width: 0px;"
    "  border: 1px solid #304050; border-bottom: none; border-radius: 3px 3px 0 0; }"
    "QTabBar::tab:selected { background: #0f0f1a; color: #c8d8e8;"
    "  border-bottom: 1px solid #0f0f1a; }"
    "QGroupBox { border: 1px solid #304050; border-radius: 4px;"
    "  margin-top: 8px; padding-top: 4px; color: #8090a0; font-size: 10px; }"
    "QGroupBox::title { subcontrol-origin: margin; left: 6px; padding: 0 3px; }"
    "QLabel { color: #8090a0; font-size: 10px; }"
    "QRadioButton { color: #c8d8e8; font-size: 10px; spacing: 3px; }"
    "QRadioButton::indicator { width: 10px; height: 10px; }"
    "QCheckBox { color: #c8d8e8; font-size: 10px; spacing: 3px; }"
    "QCheckBox::indicator { width: 10px; height: 10px; }"
    "QPushButton { background: #1a2a3a; color: #c8d8e8; border: 1px solid #304050;"
    "  border-radius: 3px; padding: 2px 8px; font-size: 10px; }"
    "QPushButton:hover { background: rgba(0, 112, 192, 180); border: 1px solid #0090e0; }");

} // namespace

AetherDspWidget::AetherDspWidget(AudioEngine* audio, QWidget* parent)
    : QWidget(parent)
    , m_audio(audio)
{
    setStyleSheet(kWidgetStyle);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(4, 4, 4, 4);
    root->setSpacing(2);

    // Selector row — six exclusive toggle buttons that double as DSP
    // activators.  Checked state == engine enable state; click again
    // to deactivate (chain bypass).  Each button is sized to span the
    // 250 px applet width with 4 px gaps:
    //   6 × 38 px buttons + 5 × 4 px gaps = 248 px
    auto* btnRow = new QHBoxLayout;
    btnRow->setContentsMargins(0, 0, 0, 0);
    btnRow->setSpacing(4);
    static const char* kLabels[NumDsps] = {"NR2", "NR4", "MNR", "DFNR", "RN2", "BNR"};
    for (int i = 0; i < NumDsps; ++i) {
        auto* b = makeToggle(kLabels[i]);
        b->setFixedSize(38, 22);
        b->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        // MNR (MMSE-Wiener spectral NR) is implemented only on macOS —
        // dim the selector on Windows / Linux so users can see it exists
        // but can't enable a path the engine has no backend for.
#ifndef Q_OS_MAC
        if (i == MNR) {
            b->setEnabled(false);
            b->setToolTip("MNR is only available on macOS.");
        }
#endif
        // NR4 (libspecbleach spectral NR) requires clang-cl on Windows to
        // compile its C99 VLAs — disabled when LLVM is not installed.
#ifndef HAVE_SPECBLEACH
        if (i == NR4) {
            b->setEnabled(false);
            b->setToolTip("NR4 requires LLVM (clang-cl) on Windows.\n"
                          "Install LLVM from llvm.org and rebuild to enable NR4.");
        }
#endif
        // BNR (NVIDIA AFX GPU denoiser) is gated at compile time by
        // HAVE_NVIDIA_AFX (defined only on x86_64 Linux/Windows; never on macOS
        // or aarch64 — no Maxine runtime there). Where it IS built, gate the
        // button at runtime on a supported NVIDIA GPU so non-NVIDIA / older-GPU
        // machines get a clear disabled state instead of a download that fails
        // on click. Keep the slot visible so the 6-button row stays balanced.
#ifndef HAVE_NVIDIA_AFX
        if (i == BNR) {
            b->setEnabled(false);
#ifdef Q_OS_MACOS
            b->setToolTip("BNR (NVIDIA GPU denoiser) is not available on macOS.\n"
                          "Use DFNR for AI noise removal.");
#else
            b->setToolTip("BNR requires an NVIDIA RTX/GeForce GPU "
                          "(not available in this build).");
#endif
        }
#else
        if (i == BNR && !NvidiaAfxPack::hasSupportedGpu()) {
            b->setEnabled(false);
            b->setToolTip("BNR requires an NVIDIA RTX 40-series or later GPU.\n"
                          "Use DFNR for AI noise removal on other hardware.");
        }
#endif
        m_dspBtns[i] = b;
        connect(b, &QPushButton::clicked, this,
                [this, i](bool nowChecked) { onDspButtonClicked(i, nowChecked); });
        btnRow->addWidget(b);
    }
    root->addLayout(btnRow);

    // Page stack — one panel per DSP.  Order MUST match DspId.
    m_dspStack = new QStackedWidget;
    m_dspStack->addWidget(buildNr2Page());
    m_dspStack->addWidget(buildNr4Page());
    m_dspStack->addWidget(buildMnrPage());
    m_dspStack->addWidget(buildDfnrPage());
    m_dspStack->addWidget(buildRn2Page());
    m_dspStack->addWidget(buildBnrPage());
    root->addWidget(m_dspStack);

    // Engine → button sync: when DSP state changes externally (chain
    // bypass, slice DSP overlay, Settings dialog) reflect it here.
    if (m_audio) {
        connect(m_audio, &AudioEngine::nr2EnabledChanged,
                this, &AetherDspWidget::syncDspSelectorFromEngine);
        connect(m_audio, &AudioEngine::nr4EnabledChanged,
                this, &AetherDspWidget::syncDspSelectorFromEngine);
        connect(m_audio, &AudioEngine::mnrEnabledChanged,
                this, &AetherDspWidget::syncDspSelectorFromEngine);
        connect(m_audio, &AudioEngine::dfnrEnabledChanged,
                this, &AetherDspWidget::syncDspSelectorFromEngine);
        connect(m_audio, &AudioEngine::rn2EnabledChanged,
                this, &AetherDspWidget::syncDspSelectorFromEngine);
        connect(m_audio, &AudioEngine::nvAfxEnabledChanged,
                this, &AetherDspWidget::syncDspSelectorFromEngine);
        connect(m_audio, &AudioEngine::nvAfxEnabledChanged,
                this, &AetherDspWidget::updateBnrStatus);
    }

    syncDspSelectorFromEngine();
    syncFromEngine();
}

void AetherDspWidget::onDspButtonClicked(int index, bool nowChecked)
{
    if (index < 0 || index >= NumDsps) return;
    // Always bring this DSP's panel forward, regardless of new check
    // state — toggling off keeps the panel visible so the user can
    // re-enable from the same place.
    m_dspStack->setCurrentIndex(index);
    if (!m_audio) return;
    // The NVIDIA license is accepted at download time (the Download button gate),
    // since that's when the licensed bits are fetched. Enabling an
    // already-downloaded BNR doesn't re-prompt.
    // NR2 enable must run FFTW wisdom prep first (#2275) — kick that
    // through MainWindow rather than calling the engine setter directly.
    // NR2 disable + every other DSP go through the engine-thread setter.
    if (index == NR2 && nowChecked) {
        emit nr2EnableWithWisdomRequested();
    } else {
        QMetaObject::invokeMethod(m_audio, [this, index, nowChecked]() {
            switch (index) {
                case NR2:  m_audio->setNr2Enabled(nowChecked); break;
                case NR4:  m_audio->setNr4Enabled(nowChecked); break;
                case MNR:  m_audio->setMnrEnabled(nowChecked); break;
                case DFNR: m_audio->setDfnrEnabled(nowChecked); break;
                case RN2:  m_audio->setRn2Enabled(nowChecked); break;
                case BNR:  m_audio->setNvAfxEnabled(nowChecked); break;  // local AFX
            }
        });
    }
    // Remember the last-enabled module so the RX chain DSP tile can
    // re-enable it when clicked from a fully-bypassed state.  Don't
    // overwrite on disable — we want the value to survive a turn-off.
    if (nowChecked) {
        static const char* kNames[NumDsps] = {"NR2", "NR4", "MNR", "DFNR", "RN2", "BNR"};
        auto& s = AppSettings::instance();
        s.setValue("LastClientNr", QString::fromLatin1(kNames[index]));
        s.save();
    }
    // AudioEngine cascades exclusion (enabling NR2 disables DFNR, etc.)
    // and emits *EnabledChanged signals; syncDspSelectorFromEngine()
    // will update sibling button states without firing setters.
}

void AetherDspWidget::syncDspSelectorFromEngine()
{
    if (!m_audio) return;
    const bool on[NumDsps] = {
        m_audio->nr2Enabled(),
        m_audio->nr4Enabled(),
        m_audio->mnrEnabled(),
        m_audio->dfnrEnabled(),
        m_audio->rn2Enabled(),
        m_audio->nvAfxEnabled(),   // BNR button = local AFX denoiser
    };
    int active = -1;
    for (int i = 0; i < NumDsps; ++i) {
        if (m_dspBtns[i]) {
            QSignalBlocker block(m_dspBtns[i]);
            m_dspBtns[i]->setChecked(on[i]);
        }
        if (on[i] && active < 0) active = i;
    }
    // If something is active, surface its panel.  If nothing's active
    // ("bypass"), keep whichever panel was last visible — don't yank the
    // user back to NR2 just because they clicked the active button off.
    if (active >= 0 && m_dspStack)
        m_dspStack->setCurrentIndex(active);
}

void AetherDspWidget::resetCurrentTab()
{
    if (!m_dspStack) return;
    const int idx = m_dspStack->currentIndex();
    static const char* kNames[NumDsps] = {"NR2", "NR4", "MNR", "DFNR", "RN2", "BNR"};
    const QString name = (idx >= 0 && idx < NumDsps) ? kNames[idx] : QString();
    if (name == "NR2") {
        if (m_nr2GainGroup) m_nr2GainGroup->button(2)->setChecked(true);
        if (m_nr2NpeGroup)  m_nr2NpeGroup->button(0)->setChecked(true);
        if (m_nr2AeCheck)        m_nr2AeCheck->setChecked(true);
        if (m_nr2GainMaxSlider)  m_nr2GainMaxSlider->setValue(150);
        if (m_nr2SmoothSlider)   m_nr2SmoothSlider->setValue(85);
        if (m_nr2QsppSlider)     m_nr2QsppSlider->setValue(20);
    } else if (name == "NR4") {
        if (m_nr4MethodGroup)      m_nr4MethodGroup->button(0)->setChecked(true);
        if (m_nr4AdaptiveCheck)    m_nr4AdaptiveCheck->setChecked(true);
        if (m_nr4ReductionSlider)  m_nr4ReductionSlider->setValue(100);
        if (m_nr4SmoothingSlider)  m_nr4SmoothingSlider->setValue(0);
        if (m_nr4WhiteningSlider)  m_nr4WhiteningSlider->setValue(0);
        if (m_nr4MaskingSlider)    m_nr4MaskingSlider->setValue(50);
        if (m_nr4SuppressionSlider)m_nr4SuppressionSlider->setValue(50);
    } else if (name == "MNR") {
        if (m_mnrEnableCheck)    m_mnrEnableCheck->setChecked(false);
        if (m_mnrStrengthSlider) m_mnrStrengthSlider->setValue(100);
    } else if (name == "DFNR") {
        if (m_dfnrAttenSlider) m_dfnrAttenSlider->setValue(100);
        if (m_dfnrBetaSlider)  m_dfnrBetaSlider->setValue(0);
    }
    // RN2 / BNR have no adjustable parameters — Reset Defaults is a no-op.
}

void AetherDspWidget::setCompactMode(bool on)
{
    setStyleSheet(on ? kCompactWidgetStyle : kWidgetStyle);

    // Slider value labels were sized to fit the full-dialog 40 px slot.
    // In compact mode they're rendered with a smaller font and fit in 30
    // px — narrower labels free up width for the slider grooves so the
    // tile reads well at the 280 px PooDoo container limit.
    const int valWidth = on ? 30 : 40;
    for (auto* lbl : { m_nr2GainMaxLabel, m_nr2SmoothLabel, m_nr2QsppLabel,
                       m_nr4ReductionLabel, m_nr4SmoothingLabel, m_nr4WhiteningLabel,
                       m_nr4MaskingLabel, m_nr4SuppressionLabel,
                       m_mnrStrengthLabel,
                       m_dfnrAttenLabel, m_dfnrBetaLabel }) {
        if (lbl) lbl->setMinimumWidth(valWidth);
    }
    if (m_dfnrAttenLabel) m_dfnrAttenLabel->setFixedWidth(valWidth);
    if (m_dfnrBetaLabel)  m_dfnrBetaLabel->setFixedWidth(valWidth);
}

void AetherDspWidget::setDialogMode(bool on)
{
    if (!on) return;  // applet path is the default; one-way switch for the dialog

    // Dialog-tuned toggle style — same colour palette as kToggleStyle but
    // 13 px font + 2px 4px padding to match the VFO DSP toggle row exactly.
    static const QString kDialogToggleStyle = QStringLiteral(
        "QPushButton { background: #1a2a3a; border: 1px solid #205070;"
        "  border-radius: 3px; color: #c8d8e8; font-size: 13px;"
        "  font-weight: bold; padding: 2px 4px; margin: 0px; }"
        "QPushButton:hover { background: #204060; }"
        "QPushButton:checked { background: #0070c0; color: #ffffff;"
        "  border: 1px solid #0090e0; }"
        "QPushButton:disabled { background: #0e1822; color: #4a5868;"
        "  border: 1px solid #1a2838; }");

    // Bump every existing inline `font-size: Npx` declaration in the
    // widget-level + label/radio/check stylesheets up to 13 px to match
    // the toggle font.  Buttons get the explicit kDialogToggleStyle below.
    static const QRegularExpression kFontSizeRe(
        QStringLiteral("font-size:\\s*\\d+px"));
    const QString kFontReplacement = QStringLiteral("font-size: 13px");

    auto bumpFonts = [&](QWidget* w) {
        QString s = w->styleSheet();
        if (s.isEmpty()) return;
        s.replace(kFontSizeRe, kFontReplacement);
        w->setStyleSheet(s);
    };

    // Set of top-row DSP-selector buttons (NR2/NR4/MNR/DFNR/RN2/BNR)
    // — they get a slightly tighter 60×24 footprint to fit six in a row
    // without forcing the dialog to grow wider.  All other toggle buttons
    // (Gain Method, NPE Method) take the standard 70×26.
    QSet<QPushButton*> topRow;
    for (auto* b : m_dspBtns) if (b) topRow.insert(b);

    bumpFonts(this);
    for (auto* btn : findChildren<QPushButton*>()) {
        // The toggle buttons (NR2/NR4/MNR/DFNR/RN2/BNR + Gain Method +
        // NPE Method) are all setCheckable(true).  ResetIconButton is the
        // only non-checkable QPushButton in the widget — easy to exclude.
        if (!btn->isCheckable()) continue;
        btn->setStyleSheet(kDialogToggleStyle);
        const QSize sz = topRow.contains(btn) ? QSize(60, 24) : QSize(70, 26);
        btn->setMinimumSize(sz);
        btn->setMaximumSize(sz);
        btn->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    }
    for (auto* lbl : findChildren<QLabel*>())       bumpFonts(lbl);
    for (auto* rb  : findChildren<QRadioButton*>()) bumpFonts(rb);
    for (auto* cb  : findChildren<QCheckBox*>())    bumpFonts(cb);
}

void AetherDspWidget::setNr2Available(bool available, const QString& tooltip)
{
    if (auto* btn = m_dspBtns[NR2]) {
        btn->setEnabled(available);
        btn->setToolTip(tooltip);
    }
}

void AetherDspWidget::selectTab(const QString& name)
{
    if (!m_dspStack) return;
    static const char* kNames[NumDsps] = {"NR2", "NR4", "MNR", "DFNR", "RN2", "BNR"};
    for (int i = 0; i < NumDsps; ++i) {
        if (name == kNames[i]) {
            m_dspStack->setCurrentIndex(i);
            return;
        }
    }
}

// ── NR2 Tab ──────────────────────────────────────────────────────────────────

QWidget* AetherDspWidget::buildNr2Page()
{
    auto* page = new QWidget;
    auto* vbox = new QVBoxLayout(page);

    auto labelStyle = QStringLiteral("QLabel { color: #8090a0; font-size: 11px; }");
    auto valStyle   = QStringLiteral("QLabel { color: #c8d8e8; font-size: 11px; min-width: 40px; }");

    // Gain Method — exclusive toggle row, styled like the slice DSP buttons.
    {
        auto* hdr = new QLabel("Gain Method:");
        hdr->setStyleSheet(labelStyle);
        vbox->addWidget(hdr);

        auto* row = new QHBoxLayout;
        // 4 × 48 px buttons evenly spaced across the 250 px applet —
        // five equal-weight stretches (left margin, three gaps, right
        // margin) distribute the 58 px of leftover space at ≈11.6 px each.
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(0);
        m_nr2GainGroup = new QButtonGroup(this);
        m_nr2GainGroup->setExclusive(true);
        const char* gainLabels[] = {"Linear", "Log", "Gamma", "Trained"};
        const char* gainTips[] = {
            "Linear audio amplitude scale for gain computation.",
            "Logarithmic amplitude scale — compresses dynamic range.",
            "Gamma distribution model matching typical speech amplitude patterns.",
            "Noise reduction model trained on real speech and noise samples."
        };
        row->addStretch(1);
        for (int i = 0; i < 4; ++i) {
            auto* b = makeToggle(gainLabels[i]);
            b->setToolTip(gainTips[i]);
            b->setFixedSize(48, 18);
            b->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
            m_nr2GainGroup->addButton(b, i);
            row->addWidget(b);
            row->addStretch(1);
        }
        m_nr2GainGroup->button(2)->setChecked(true);  // Gamma default
        connect(m_nr2GainGroup, &QButtonGroup::idClicked, this, [this](int id) {
            auto& s = AppSettings::instance();
            s.setValue("NR2GainMethod", QString::number(id));
            s.save();
            emit nr2GainMethodChanged(id);
        });
        vbox->addLayout(row);
    }

    // NPE Method — exclusive toggle row.
    {
        auto* hdr = new QLabel("NPE Method:");
        hdr->setStyleSheet(labelStyle);
        vbox->addWidget(hdr);

        auto* row = new QHBoxLayout;
        // 3 × 48 px buttons evenly spaced across the 250 px applet —
        // four equal-weight stretches share the 106 px of leftover space.
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(0);
        m_nr2NpeGroup = new QButtonGroup(this);
        m_nr2NpeGroup->setExclusive(true);
        const char* npeLabels[] = {"OSMS", "MMSE", "NSTAT"};
        const char* npeTips[] = {
            "Optimal Smoothing Minimum Statistics — tracks noise floor using a running minimum estimate.",
            "Minimum Mean Squared Error — minimizes the expected noise estimation error.",
            "Non-Stationary estimation — adapts to noise that changes over time."
        };
        row->addStretch(1);
        for (int i = 0; i < 3; ++i) {
            auto* b = makeToggle(npeLabels[i]);
            b->setToolTip(npeTips[i]);
            b->setFixedSize(48, 18);
            b->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
            m_nr2NpeGroup->addButton(b, i);
            row->addWidget(b);
            row->addStretch(1);
        }
        m_nr2NpeGroup->button(0)->setChecked(true);  // OSMS default
        connect(m_nr2NpeGroup, &QButtonGroup::idClicked, this, [this](int id) {
            auto& s = AppSettings::instance();
            s.setValue("NR2NpeMethod", QString::number(id));
            s.save();
            emit nr2NpeMethodChanged(id);
        });
        vbox->addLayout(row);
    }

    // AE Filter checkbox + Reset Defaults icon on the same row.
    m_nr2AeCheck = new QCheckBox("AE Filter (artifact elimination)");
    m_nr2AeCheck->setToolTip("Reduces ringing and musical artifacts typical of frequency-domain noise reduction.");
    m_nr2AeCheck->setChecked(true);
    connect(m_nr2AeCheck, &QCheckBox::toggled, this, [this](bool on) {
        auto& s = AppSettings::instance();
        s.setValue("NR2AeFilter", on ? "True" : "False");
        s.save();
        emit nr2AeFilterChanged(on);
    });
    {
        auto* aeRow = new QHBoxLayout;
        aeRow->setContentsMargins(0, 0, 0, 0);
        aeRow->setSpacing(0);
        aeRow->addWidget(m_nr2AeCheck);
        aeRow->addStretch(1);
        auto* resetBtn = makeResetIconButton();
        connect(resetBtn, &QPushButton::clicked,
                this, &AetherDspWidget::resetCurrentTab);
        aeRow->addWidget(resetBtn);
        vbox->addLayout(aeRow);
    }

    // Sliders: GainMax, GainSmooth, Q_SPP
    auto* sliderGrid = new QGridLayout;
    int row = 0;

    // Gain Max (reduction depth)
    {
        auto* lbl = new QLabel("Reduction:");
        lbl->setStyleSheet(labelStyle);
        sliderGrid->addWidget(lbl, row, 0);
        m_nr2GainMaxSlider = new GuardedSlider(Qt::Horizontal);
        m_nr2GainMaxSlider->setRange(50, 200);
        m_nr2GainMaxSlider->setValue(150);
        applyPrimarySliderStyle(m_nr2GainMaxSlider);
        m_nr2GainMaxSlider->setToolTip("Maximum noise reduction depth. Higher values suppress more noise but risk distorting speech.");
        sliderGrid->addWidget(m_nr2GainMaxSlider, row, 1);
        m_nr2GainMaxLabel = new QLabel("1.50");
        m_nr2GainMaxLabel->setStyleSheet(valStyle);
        m_nr2GainMaxLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        sliderGrid->addWidget(m_nr2GainMaxLabel, row, 2);
        connect(m_nr2GainMaxSlider, &QSlider::valueChanged, this, [this](int v) {
            float val = v / 100.0f;
            m_nr2GainMaxLabel->setText(QString::number(val, 'f', 2));
            auto& s = AppSettings::instance();
            s.setValue("NR2GainMax", QString::number(val, 'f', 2));
            s.save();
            emit nr2GainMaxChanged(val);
        });
        ++row;
    }

    // Gain Smooth
    {
        auto* lbl = new QLabel("Smoothing:");
        lbl->setStyleSheet(labelStyle);
        sliderGrid->addWidget(lbl, row, 0);
        m_nr2SmoothSlider = new GuardedSlider(Qt::Horizontal);
        m_nr2SmoothSlider->setRange(50, 98);
        m_nr2SmoothSlider->setValue(85);
        applyPrimarySliderStyle(m_nr2SmoothSlider);
        m_nr2SmoothSlider->setToolTip("How smoothly the noise estimate tracks changes. Higher values give steadier but slower adaptation.");
        sliderGrid->addWidget(m_nr2SmoothSlider, row, 1);
        m_nr2SmoothLabel = new QLabel("0.85");
        m_nr2SmoothLabel->setStyleSheet(valStyle);
        m_nr2SmoothLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        sliderGrid->addWidget(m_nr2SmoothLabel, row, 2);
        connect(m_nr2SmoothSlider, &QSlider::valueChanged, this, [this](int v) {
            float val = v / 100.0f;
            m_nr2SmoothLabel->setText(QString::number(val, 'f', 2));
            auto& s = AppSettings::instance();
            s.setValue("NR2GainSmooth", QString::number(val, 'f', 2));
            s.save();
            emit nr2GainSmoothChanged(val);
        });
        ++row;
    }

    // Q_SPP (voice threshold)
    {
        auto* lbl = new QLabel("Threshold:");
        lbl->setStyleSheet(labelStyle);
        sliderGrid->addWidget(lbl, row, 0);
        m_nr2QsppSlider = new GuardedSlider(Qt::Horizontal);
        m_nr2QsppSlider->setRange(5, 50);
        m_nr2QsppSlider->setValue(20);
        applyPrimarySliderStyle(m_nr2QsppSlider);
        m_nr2QsppSlider->setToolTip("Speech detection threshold. Lower values preserve quiet speech but may pass more noise.");
        sliderGrid->addWidget(m_nr2QsppSlider, row, 1);
        m_nr2QsppLabel = new QLabel("0.20");
        m_nr2QsppLabel->setStyleSheet(valStyle);
        m_nr2QsppLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        sliderGrid->addWidget(m_nr2QsppLabel, row, 2);
        connect(m_nr2QsppSlider, &QSlider::valueChanged, this, [this](int v) {
            float val = v / 100.0f;
            m_nr2QsppLabel->setText(QString::number(val, 'f', 2));
            auto& s = AppSettings::instance();
            s.setValue("NR2Qspp", QString::number(val, 'f', 2));
            s.save();
            emit nr2QsppChanged(val);
        });
        ++row;
    }

    vbox->addLayout(sliderGrid);
    vbox->addStretch();
    return page;
}

// ── NR4 Tab (libspecbleach) ──────────────────────────────────────────────────

QWidget* AetherDspWidget::buildNr4Page()
{
    auto* page = new QWidget;
    auto* vbox = new QVBoxLayout(page);

    auto labelStyle = QStringLiteral("QLabel { color: #8090a0; font-size: 11px; }");
    auto valStyle   = QStringLiteral("QLabel { color: #c8d8e8; font-size: 11px; min-width: 40px; }");

    // Noise Estimation Method — exclusive toggle row.
    {
        auto* hdr = new QLabel("Noise Estimation:");
        hdr->setStyleSheet(labelStyle);
        vbox->addWidget(hdr);

        auto* row = new QHBoxLayout;
        // 3 × 48 px buttons evenly spaced across 250 px — matches NPE row.
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(0);
        m_nr4MethodGroup = new QButtonGroup(this);
        m_nr4MethodGroup->setExclusive(true);
        const char* methodLabels[] = {"MMSE", "Brandt", "Martin"};
        const char* methodTips[] = {
            "MMSE with Speech Presence Probability — balances noise estimation with speech preservation.",
            "Recursive smoothing using critical frequency bands — good for non-stationary noise.",
            "Minimum statistics using running spectral minima — robust for slowly varying noise floors."
        };
        row->addStretch(1);
        for (int i = 0; i < 3; ++i) {
            auto* b = makeToggle(methodLabels[i]);
            b->setToolTip(methodTips[i]);
            b->setFixedSize(48, 18);
            b->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
            m_nr4MethodGroup->addButton(b, i);
            row->addWidget(b);
            row->addStretch(1);
        }
        m_nr4MethodGroup->button(0)->setChecked(true);
        connect(m_nr4MethodGroup, &QButtonGroup::idClicked, this, [this](int id) {
            auto& s = AppSettings::instance();
            s.setValue("NR4NoiseEstimationMethod", QString::number(id));
            s.save();
            emit nr4NoiseMethodChanged(id);
        });
        vbox->addLayout(row);
    }

    // Adaptive Noise checkbox + Reset Defaults icon on the same row.
    m_nr4AdaptiveCheck = new QCheckBox("Adaptive Noise Estimation");
    m_nr4AdaptiveCheck->setToolTip("Continuously re-estimates the noise floor as conditions change. Disable for stable environments.");
    m_nr4AdaptiveCheck->setChecked(true);
    connect(m_nr4AdaptiveCheck, &QCheckBox::toggled, this, [this](bool on) {
        auto& s = AppSettings::instance();
        s.setValue("NR4AdaptiveNoise", on ? "True" : "False");
        s.save();
        emit nr4AdaptiveNoiseChanged(on);
    });
    {
        auto* adRow = new QHBoxLayout;
        adRow->setContentsMargins(0, 0, 0, 0);
        adRow->setSpacing(0);
        adRow->addWidget(m_nr4AdaptiveCheck);
        adRow->addStretch(1);
        auto* resetBtn = makeResetIconButton();
        connect(resetBtn, &QPushButton::clicked,
                this, &AetherDspWidget::resetCurrentTab);
        adRow->addWidget(resetBtn);
        vbox->addLayout(adRow);
    }

    // Sliders
    auto* sliderGrid = new QGridLayout;
    int row = 0;

    {
        auto* lbl = new QLabel("Reduction (dB):");
        lbl->setStyleSheet(labelStyle);
        sliderGrid->addWidget(lbl, row, 0);
        m_nr4ReductionSlider = new GuardedSlider(Qt::Horizontal);
        m_nr4ReductionSlider->setRange(0, 400);
        m_nr4ReductionSlider->setValue(100);
        applyPrimarySliderStyle(m_nr4ReductionSlider);
        m_nr4ReductionSlider->setToolTip("Maximum noise reduction in dB. Higher values remove more noise but may affect speech.");
        sliderGrid->addWidget(m_nr4ReductionSlider, row, 1);
        m_nr4ReductionLabel = new QLabel("10.0");
        m_nr4ReductionLabel->setStyleSheet(valStyle);
        m_nr4ReductionLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        sliderGrid->addWidget(m_nr4ReductionLabel, row, 2);
        connect(m_nr4ReductionSlider, &QSlider::valueChanged, this, [this](int v) {
            float val = v / 10.0f;
            m_nr4ReductionLabel->setText(QString::number(val, 'f', 1));
            auto& s = AppSettings::instance();
            s.setValue("NR4ReductionAmount", QString::number(val, 'f', 1));
            s.save();
            emit nr4ReductionChanged(val);
        });
        ++row;
    }

    {
        auto* lbl = new QLabel("Smoothing (%):");
        lbl->setStyleSheet(labelStyle);
        sliderGrid->addWidget(lbl, row, 0);
        m_nr4SmoothingSlider = new GuardedSlider(Qt::Horizontal);
        m_nr4SmoothingSlider->setRange(0, 100);
        m_nr4SmoothingSlider->setValue(0);
        applyPrimarySliderStyle(m_nr4SmoothingSlider);
        m_nr4SmoothingSlider->setToolTip("Time-domain smoothing of the noise estimate. Higher values produce steadier but slower reduction.");
        sliderGrid->addWidget(m_nr4SmoothingSlider, row, 1);
        m_nr4SmoothingLabel = new QLabel("0");
        m_nr4SmoothingLabel->setStyleSheet(valStyle);
        m_nr4SmoothingLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        sliderGrid->addWidget(m_nr4SmoothingLabel, row, 2);
        connect(m_nr4SmoothingSlider, &QSlider::valueChanged, this, [this](int v) {
            m_nr4SmoothingLabel->setText(QString::number(v));
            auto& s = AppSettings::instance();
            s.setValue("NR4SmoothingFactor", QString::number(static_cast<float>(v), 'f', 1));
            s.save();
            emit nr4SmoothingChanged(static_cast<float>(v));
        });
        ++row;
    }

    {
        auto* lbl = new QLabel("Whitening (%):");
        lbl->setStyleSheet(labelStyle);
        sliderGrid->addWidget(lbl, row, 0);
        m_nr4WhiteningSlider = new GuardedSlider(Qt::Horizontal);
        m_nr4WhiteningSlider->setRange(0, 100);
        m_nr4WhiteningSlider->setValue(0);
        applyPrimarySliderStyle(m_nr4WhiteningSlider);
        m_nr4WhiteningSlider->setToolTip("Flattens the spectral shape of residual noise so it sounds more uniform.");
        sliderGrid->addWidget(m_nr4WhiteningSlider, row, 1);
        m_nr4WhiteningLabel = new QLabel("0");
        m_nr4WhiteningLabel->setStyleSheet(valStyle);
        m_nr4WhiteningLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        sliderGrid->addWidget(m_nr4WhiteningLabel, row, 2);
        connect(m_nr4WhiteningSlider, &QSlider::valueChanged, this, [this](int v) {
            m_nr4WhiteningLabel->setText(QString::number(v));
            auto& s = AppSettings::instance();
            s.setValue("NR4WhiteningFactor", QString::number(static_cast<float>(v), 'f', 1));
            s.save();
            emit nr4WhiteningChanged(static_cast<float>(v));
        });
        ++row;
    }

    {
        auto* lbl = new QLabel("Masking Depth:");
        lbl->setStyleSheet(labelStyle);
        sliderGrid->addWidget(lbl, row, 0);
        m_nr4MaskingSlider = new GuardedSlider(Qt::Horizontal);
        m_nr4MaskingSlider->setRange(0, 100);
        m_nr4MaskingSlider->setValue(50);
        applyPrimarySliderStyle(m_nr4MaskingSlider);
        m_nr4MaskingSlider->setToolTip("Depth of spectral masking. Higher values suppress more noise in masked frequency regions.");
        sliderGrid->addWidget(m_nr4MaskingSlider, row, 1);
        m_nr4MaskingLabel = new QLabel("0.50");
        m_nr4MaskingLabel->setStyleSheet(valStyle);
        m_nr4MaskingLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        sliderGrid->addWidget(m_nr4MaskingLabel, row, 2);
        connect(m_nr4MaskingSlider, &QSlider::valueChanged, this, [this](int v) {
            float val = v / 100.0f;
            m_nr4MaskingLabel->setText(QString::number(val, 'f', 2));
            auto& s = AppSettings::instance();
            s.setValue("NR4MaskingDepth", QString::number(val, 'f', 2));
            s.save();
            emit nr4MaskingDepthChanged(val);
        });
        ++row;
    }

    {
        auto* lbl = new QLabel("Suppression:");
        lbl->setStyleSheet(labelStyle);
        sliderGrid->addWidget(lbl, row, 0);
        m_nr4SuppressionSlider = new GuardedSlider(Qt::Horizontal);
        m_nr4SuppressionSlider->setRange(0, 100);
        m_nr4SuppressionSlider->setValue(50);
        applyPrimarySliderStyle(m_nr4SuppressionSlider);
        m_nr4SuppressionSlider->setToolTip("Overall suppression strength. Higher values apply more aggressive noise removal.");
        sliderGrid->addWidget(m_nr4SuppressionSlider, row, 1);
        m_nr4SuppressionLabel = new QLabel("0.50");
        m_nr4SuppressionLabel->setStyleSheet(valStyle);
        m_nr4SuppressionLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        sliderGrid->addWidget(m_nr4SuppressionLabel, row, 2);
        connect(m_nr4SuppressionSlider, &QSlider::valueChanged, this, [this](int v) {
            float val = v / 100.0f;
            m_nr4SuppressionLabel->setText(QString::number(val, 'f', 2));
            auto& s = AppSettings::instance();
            s.setValue("NR4SuppressionStrength", QString::number(val, 'f', 2));
            s.save();
            emit nr4SuppressionChanged(val);
        });
        ++row;
    }

    vbox->addLayout(sliderGrid);
    vbox->addStretch();
    return page;
}

// ── MNR Tab ──────────────────────────────────────────────────────────────────

QWidget* AetherDspWidget::buildMnrPage()
{
    auto* page = new QWidget;
    auto* vbox = new QVBoxLayout(page);

    auto labelStyle = QStringLiteral("QLabel { color: #8090a0; font-size: 11px; }");
    auto valStyle   = QStringLiteral("QLabel { color: #c8d8e8; font-size: 11px; min-width: 40px; }");

    m_mnrEnableCheck = new QCheckBox("Enable MNR (macOS only)");
    m_mnrEnableCheck->setToolTip("MMSE-Wiener spectral noise reduction with asymmetric gain smoothing.\n"
                                 "Removes consistent background noise while preserving speech quality.");
    {
        auto* hdrRow = new QHBoxLayout;
        hdrRow->setContentsMargins(0, 0, 0, 0);
        hdrRow->addWidget(m_mnrEnableCheck);
        hdrRow->addStretch(1);
        auto* resetBtn = makeResetIconButton();
        connect(resetBtn, &QPushButton::clicked,
                this, &AetherDspWidget::resetCurrentTab);
        hdrRow->addWidget(resetBtn);
        vbox->addLayout(hdrRow);
    }
    connect(m_mnrEnableCheck, &QCheckBox::toggled, this, [this](bool checked) {
        auto& s = AppSettings::instance();
        s.setValue("MnrEnabled", checked ? "True" : "False");
        s.save();
        emit mnrEnabledChanged(checked);
    });

    {
        auto* row = new QHBoxLayout;
        auto* lbl = new QLabel("Strength");
        lbl->setStyleSheet(labelStyle);
        row->addWidget(lbl);

        m_mnrStrengthSlider = new GuardedSlider(Qt::Horizontal);
        m_mnrStrengthSlider->setRange(0, 100);
        m_mnrStrengthSlider->setValue(100);
        applyPrimarySliderStyle(m_mnrStrengthSlider);
        m_mnrStrengthSlider->setToolTip("Adjust noise reduction aggressiveness (0 = mild, 100 = maximum)");
        row->addWidget(m_mnrStrengthSlider, 1);

        m_mnrStrengthLabel = new QLabel("100%");
        m_mnrStrengthLabel->setStyleSheet(valStyle);
        row->addWidget(m_mnrStrengthLabel);
        vbox->addLayout(row);

        connect(m_mnrStrengthSlider, &QSlider::valueChanged, this, [this](int value) {
            float normalized = value / 100.0f;
            m_mnrStrengthLabel->setText(QString::number(value) + "%");
            auto& s = AppSettings::instance();
            s.setValue("MnrStrength", QString::number(normalized, 'f', 2));
            s.save();
            emit mnrStrengthChanged(normalized);
        });
    }

    auto* info = new QLabel("Asymmetric temporal smoothing: fast release (~15ms) for quick noise suppression,\n"
                            "gentle attack (~64ms) to preserve speech transients without artifacts.");
    info->setWordWrap(true);
    AetherSDR::ThemeManager::instance().applyStyleSheet(info, "QLabel { color: {{color.text.secondary}}; font-size: 11px; }");
    vbox->addSpacing(8);
    vbox->addWidget(info);

    vbox->addStretch();
    return page;
}

// ── RN2 Tab ─────────────────────────────────────────────────────────────────

QWidget* AetherDspWidget::buildRn2Page()
{
    auto* page = new QWidget;
    auto* vbox = new QVBoxLayout(page);
    auto* lbl = new QLabel(
        "RNNoise — open-source recurrent neural-network voice denoiser. "
        "Removes stationary background noise (fans, hum, white-noise floor) "
        "while preserving speech.  Lightweight and CPU-only.  No adjustable "
        "parameters.");
    lbl->setWordWrap(true);
    lbl->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    AetherSDR::ThemeManager::instance().applyStyleSheet(lbl, "QLabel { color: {{color.text.secondary}}; font-size: 12px; }");
    vbox->addWidget(lbl);
    vbox->addStretch();
    return page;
}

// ── BNR Tab ─────────────────────────────────────────────────────────────────

// BNR runs the local in-process NVIDIA AFX denoiser on this machine's GPU.
// The runtime is downloaded on demand; this reflects install/active state and
// lists the installed components (version + sha256) below the controls.
void AetherDspWidget::updateBnrStatus()
{
#ifdef HAVE_NVIDIA_AFX
    const bool installed = NvidiaAfxPack::isInstalled();
    const bool busy = m_bnrAfxPack && m_bnrAfxPack->busy();
    const bool updatable = installed && m_bnrAfxPack && m_bnrAfxPack->updateAvailable();
    // Fetch the pack queries once and reuse (this runs on every BNR toggle).
    const auto installedList = installed ? NvidiaAfxPack::installedComponents()
                                         : QList<NvidiaAfxPack::ComponentInfo>();
    const auto latestList = m_bnrAfxPack ? m_bnrAfxPack->latestComponents()
                                         : QList<NvidiaAfxPack::ComponentInfo>();
    // A cancelled download leaves verified components staged for resume.
    const auto staged = installed ? QList<NvidiaAfxPack::ComponentInfo>()
                                  : NvidiaAfxPack::stagedComponents();
    const int totalComps = latestList.size();
    const bool partial = !installed && !staged.isEmpty() && totalComps > 0;
    if (m_bnrAfxStatus && !busy) {
        const bool on = m_audio && m_audio->nvAfxEnabled();
        if (installed && on && !updatable) {
            // Green dot + text while the denoiser is running.
            const QString green = AetherSDR::ThemeManager::instance()
                                      .value(QStringLiteral("color.accent.success"));
            m_bnrAfxStatus->setText(
                QStringLiteral("<span style='color:%1;'>● Active</span>").arg(green));
        } else {
            m_bnrAfxStatus->setText(installed
                                        ? (updatable ? QStringLiteral("Installed — update available")
                                                     : QStringLiteral("Installed — ready"))
                                    : partial ? tr("Partially downloaded (%1/%2)")
                                                    .arg(staged.size()).arg(totalComps)
                                              : QStringLiteral("Not installed"));
        }
    }
    if (m_bnrAfxDownloadBtn && !busy) {
        m_bnrAfxDownloadBtn->setText(installed
                                         ? (updatable ? QStringLiteral("Update")
                                                      : QStringLiteral("Re-download"))
                                     : partial ? tr("Resume download")
                                               : QStringLiteral("Download (~1 GB)"));
        m_bnrAfxDownloadBtn->setEnabled(true);
    }
    if (m_bnrAfxIntensitySlider)
        m_bnrAfxIntensitySlider->setEnabled(installed);

    // Don't disturb the live download rows mid-flight — the per-component
    // signals own them while busy. Otherwise reflect the installed manifest
    // (annotating any component whose pinned version moved on, → newer), or the
    // partially-downloaded set when a download was cancelled.
    if (!busy) {
        if (installed) {
            QHash<QString, QString> latest;
            for (const auto& c : latestList)
                latest.insert(c.name, c.version);
            QStringList names;
            for (const auto& c : installedList) names << c.name;
            rebuildBnrRows(names);
            for (int i = 0; i < installedList.size(); ++i)
                setBnrRowDetail(i, installedList[i].version, installedList[i].sha256,
                                installedList[i].bytes, latest.value(installedList[i].name));
        } else if (partial) {
            QStringList names;
            for (const auto& c : staged) names << c.name;
            rebuildBnrRows(names);
            for (int i = 0; i < staged.size(); ++i)
                setBnrRowDetail(i, staged[i].version, staged[i].sha256, staged[i].bytes);
        } else {
            clearBnrRows();
        }
    }
#else
    if (m_bnrAfxStatus)
        m_bnrAfxStatus->setText(QStringLiteral("Not available in this build"));
    clearBnrRows();
#endif
}

// One-time NVIDIA license acceptance, shown the first time BNR is enabled
// (the AFX runtime + denoiser model are NVIDIA-licensed). Flows NVIDIA's terms
// down to the end user (SWLA §1.3.3) and carries the Works Notice (PST §1.7.1).
bool AetherDspWidget::ensureBnrLicenseAccepted()
{
    if (NvidiaBnrSettings::licenseAccepted())
        return true;

    QMessageBox box(this);
    box.setWindowTitle(tr("NVIDIA Software License — BNR"));
    box.setIcon(QMessageBox::Information);
    box.setTextFormat(Qt::RichText);
    box.setText(tr("<b>BNR uses NVIDIA Maxine software and a denoiser model.</b>"));
    box.setInformativeText(tr(
        "BNR uses components provided by NVIDIA Corporation, governed by "
        "NVIDIA's license agreements:"
        "<ul><li>NVIDIA Software License Agreement</li>"
        "<li>Product-Specific Terms for NVIDIA AI Products</li>"
        "<li>NVIDIA Community Model License</li></ul>"
        "Licensed for use on NVIDIA RTX / GeForce RTX GPUs on a single-user "
        "PC/workstation. The full texts ship with the downloaded BNR pack "
        "(<tt>licenses/</tt>); see also "
        "<a href=\"https://www.nvidia.com/en-us/agreements/enterprise-software/"
        "nvidia-software-license-agreement/\">NVIDIA's Software License Agreement</a>."
        "<br><br>By clicking <b>Accept</b> you agree to NVIDIA's license terms."));
    auto* acceptBtn = box.addButton(tr("Accept"), QMessageBox::AcceptRole);
    box.addButton(tr("Decline"), QMessageBox::RejectRole);
    box.exec();
    if (box.clickedButton() == acceptBtn) {
        NvidiaBnrSettings::setLicenseAccepted(true);
        return true;
    }
    return false;
}

QWidget* AetherDspWidget::buildBnrPage()
{
    auto* page = new QWidget;
    auto* vbox = new QVBoxLayout(page);
    vbox->setContentsMargins(10, 20, 10, 0);

    auto* info = new QLabel("GPU-accelerated AI noise removal (NVIDIA Maxine) — "
                            "runs in-process on a local NVIDIA GPU.");
    info->setWordWrap(true);
    AetherSDR::ThemeManager::instance().applyStyleSheet(info, "QLabel { color: {{color.text.secondary}}; font-size: 12px; }");
    vbox->addWidget(info);

    auto* g = new QGridLayout;
    g->setContentsMargins(0, 12, 10, 0);
    g->setColumnStretch(1, 1);

    // Status (row 0 — above the intensity line)
    g->addWidget(new QLabel("Status"), 0, 0);
    m_bnrAfxStatus = new QLabel;
    m_bnrAfxStatus->setAccessibleName(tr("BNR status"));
    AetherSDR::ThemeManager::instance().applyStyleSheet(m_bnrAfxStatus, "QLabel { color: {{color.text.secondary}}; font-size: 11px; }");
    g->addWidget(m_bnrAfxStatus, 0, 1);

    // Intensity (row 1)
    g->addWidget(new QLabel("Intensity"), 1, 0);
    m_bnrAfxIntensitySlider = new QSlider(Qt::Horizontal);
    m_bnrAfxIntensitySlider->setRange(0, 100);
    m_bnrAfxIntensitySlider->setValue(static_cast<int>(NvidiaBnrSettings::intensity() * 100));
    applyPrimarySliderStyle(m_bnrAfxIntensitySlider);
    m_bnrAfxIntensitySlider->setAccessibleName(tr("BNR intensity"));
    m_bnrAfxIntensitySlider->setAccessibleDescription(tr("Denoising strength, 0 = passthrough, 100 = maximum."));
    m_bnrAfxIntensitySlider->setToolTip("Denoising strength (0 = passthrough, 100 = max).");
    g->addWidget(m_bnrAfxIntensitySlider, 1, 1);
    m_bnrAfxIntensityLabel = new QLabel(QString::number(m_bnrAfxIntensitySlider->value()));
    m_bnrAfxIntensityLabel->setFixedWidth(40);
    g->addWidget(m_bnrAfxIntensityLabel, 1, 2);

    // Download button — created here, placed at the bottom-left of the page below.
    m_bnrAfxDownloadBtn = new QPushButton("Download");
    m_bnrAfxDownloadBtn->setAccessibleName(tr("Download BNR runtime"));
    m_bnrAfxDownloadBtn->setAccessibleDescription(tr("Download the NVIDIA AFX runtime and denoiser model "
                                                     "for this GPU into the app cache (one-time, ~1 GB)."));
    m_bnrAfxDownloadBtn->setToolTip("Download the NVIDIA AFX runtime + denoiser model "
                                    "for this GPU into the app's cache (one-time).");
    connect(m_bnrAfxIntensitySlider, &QSlider::valueChanged, this, [this](int v) {
        m_bnrAfxIntensityLabel->setText(QString::number(v));
        const float r = v / 100.0f;
        NvidiaBnrSettings::setIntensity(r);
        // Capture the engine pointer by value, not `this`: the functor runs
        // later on the AudioEngine thread, and this widget may be destroyed
        // before it drains (capturing `this`->m_audio would be a cross-thread UAF).
        if (auto* audio = m_audio)
            QMetaObject::invokeMethod(audio, [audio, r]() { audio->setNvAfxIntensity(r); });
    });
    vbox->addLayout(g);

    // Per-component list — one row each, a progress bar while downloading that
    // swaps to the installed version + sha + size when done. The same rows show
    // the installed manifest in the steady state (built by updateBnrStatus).
    // A shared grid keeps the name / size / bar columns aligned across rows so
    // every bar starts at the same x and is the same width.
    m_bnrAfxList = new QWidget;
    m_bnrAfxListLayout = new QGridLayout(m_bnrAfxList);
    m_bnrAfxListLayout->setContentsMargins(0, 12, 10, 0);
    // 24px between name|size and size|bar (the grid's only two column gaps).
    m_bnrAfxListLayout->setHorizontalSpacing(24);
    m_bnrAfxListLayout->setVerticalSpacing(4);
    m_bnrAfxListLayout->setColumnStretch(2, 1);   // bar/detail column expands
    vbox->addWidget(m_bnrAfxList);

    vbox->addStretch();

    // Download / Resume / Update button, pinned to the bottom-left.
    auto* dlRow = new QHBoxLayout;
    dlRow->setContentsMargins(0, 8, 0, 0);
    dlRow->addWidget(m_bnrAfxDownloadBtn);
    dlRow->addStretch(1);
    vbox->addLayout(dlRow);

#ifdef HAVE_NVIDIA_AFX
    m_bnrAfxPack = new NvidiaAfxPack(this);
    connect(m_bnrAfxPack, &NvidiaAfxPack::planReady, this,
            [this](const QList<NvidiaAfxPack::ComponentInfo>& comps) {
        QStringList names;
        for (const auto& c : comps) names << c.name;
        rebuildBnrRows(names);
        if (m_bnrAfxStatus) m_bnrAfxStatus->setText(QStringLiteral("Downloading…"));
        if (m_bnrAfxDownloadBtn) m_bnrAfxDownloadBtn->setEnabled(false);
    });
    connect(m_bnrAfxPack, &NvidiaAfxPack::componentProgress, this,
            [this](int i, int pct, qint64 bytes, const QString& rateEta) {
        setBnrRowProgress(i, pct, bytes, rateEta);
    });
    connect(m_bnrAfxPack, &NvidiaAfxPack::componentFinished, this,
            [this](int i, const NvidiaAfxPack::ComponentInfo& info) {
        setBnrRowDetail(i, info.version, info.sha256, info.bytes);
    });
    connect(m_bnrAfxPack, &NvidiaAfxPack::finished, this, [this](bool ok, const QString& msg) {
        if (!ok && m_bnrAfxStatus) m_bnrAfxStatus->setText(QStringLiteral("Failed: %1").arg(msg));
        updateBnrStatus();
    });
    connect(m_bnrAfxDownloadBtn, &QPushButton::clicked, this, [this]() {
        // Downloading fetches NVIDIA-licensed bits, so it needs the same
        // one-time acceptance gate as enabling BNR (#bnr-license).
        if (!ensureBnrLicenseAccepted()) return;
        if (m_bnrAfxPack) m_bnrAfxPack->install();   // CUDA from PyPI + hosted AFX bits
    });
#else
    m_bnrAfxDownloadBtn->setEnabled(false);
#endif

    updateBnrStatus();
    return page;
}

namespace {
// "245 MB" / "8.4 KB" — compact download size.
QString humanSize(qint64 bytes)
{
    if (bytes <= 0) return {};
    double v = double(bytes);
    const char* unit = "B";
    if (v >= 1024.0) { v /= 1024.0; unit = "KB"; }
    if (v >= 1024.0) { v /= 1024.0; unit = "MB"; }
    if (v >= 1024.0) { v /= 1024.0; unit = "GB"; }
    return QStringLiteral("%1 %2").arg(v, 0, 'f', v < 10.0 ? 1 : 0).arg(QLatin1String(unit));
}
} // namespace

// Build one grid row per component: [ name | size | bar/detail ]. Columns are
// shared across rows so every bar aligns and is the same width. Rows start in
// the "queued" download state; setBnrRowProgress / setBnrRowDetail update them.
// Fonts are left to inherit so the rows match the rest of the dialog.
void AetherDspWidget::rebuildBnrRows(const QStringList& names)
{
    clearBnrRows();
    if (!m_bnrAfxListLayout) return;
    int row = 0;
    for (const QString& name : names) {
        BnrCompRow r;
        r.name = new QLabel(name);
        AetherSDR::ThemeManager::instance().applyStyleSheet(r.name,
            "QLabel { color: {{color.text.primary}}; }");
        r.size = new QLabel;
        r.size->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        AetherSDR::ThemeManager::instance().applyStyleSheet(r.size,
            "QLabel { color: {{color.text.secondary}}; }");
        r.bar = new QProgressBar;
        r.bar->setTextVisible(false);   // chunk fills flush-left; text is overlaid
        r.bar->setFixedHeight(16);
        // Dimmer accent for the fill so the overlaid text keeps contrast over
        // both the chunk and the dark groove (the bright accent washed it out).
        AetherSDR::ThemeManager::instance().applyStyleSheet(r.bar,
            "QProgressBar { background: {{color.background.0}};"
            " border: 1px solid {{color.border.strong}}; border-radius: 3px; }"
            "QProgressBar::chunk { background: {{color.accent.dim}}; border-radius: 2px; }");
        r.bar->setRange(0, 100);
        r.bar->setValue(0);
        // Status text as a transparent overlay so its 10px left pad doesn't inset
        // the chunk (QProgressBar padding would push the fill in too).
        r.barText = new QLabel(QStringLiteral("queued"), r.bar);
        AetherSDR::ThemeManager::instance().applyStyleSheet(r.barText,
            "QLabel { padding-left: 10px; background: transparent;"
            " color: {{color.text.primary}}; }");
        auto* bl = new QHBoxLayout(r.bar);
        bl->setContentsMargins(0, 0, 0, 0);
        bl->addWidget(r.barText);
        r.detail = new QLabel;
        r.detail->setTextFormat(Qt::RichText);
        r.detail->setTextInteractionFlags(Qt::TextSelectableByMouse);
        r.detail->hide();
        AetherSDR::ThemeManager::instance().applyStyleSheet(r.detail,
            "QLabel { color: {{color.text.secondary}}; }");
        m_bnrAfxListLayout->addWidget(r.name,   row, 0);
        m_bnrAfxListLayout->addWidget(r.size,   row, 1);
        // Bar and detail share the same cell; only one is visible at a time.
        m_bnrAfxListLayout->addWidget(r.bar,    row, 2);
        m_bnrAfxListLayout->addWidget(r.detail, row, 2);
        m_bnrAfxRows.append(r);
        ++row;
    }
}

void AetherDspWidget::setBnrRowProgress(int i, int percent, qint64 bytes, const QString& rateEta)
{
    if (i < 0 || i >= m_bnrAfxRows.size()) return;
    BnrCompRow& r = m_bnrAfxRows[i];
    if (r.detail) r.detail->hide();
    if (r.size && bytes > 0) r.size->setText(humanSize(bytes));
    if (!r.bar) return;
    r.bar->show();
    QString text;
    if (percent < 0) {                       // resolving / extracting
        r.bar->setRange(0, 0);               // indeterminate
        text = rateEta.isEmpty() ? QStringLiteral("working…") : rateEta;
    } else {
        r.bar->setRange(0, 100);
        r.bar->setValue(percent);
        text = rateEta.isEmpty() ? QStringLiteral("%1%").arg(percent)
                                 : QStringLiteral("%1%  ·  %2").arg(percent).arg(rateEta);
    }
    if (r.barText) r.barText->setText(text);
}

// Swap a row from its progress bar to the installed version/sha/size line.
void AetherDspWidget::setBnrRowDetail(int i, const QString& version,
                                      const QString& sha256, qint64 bytes,
                                      const QString& newVersion)
{
    if (i < 0 || i >= m_bnrAfxRows.size()) return;
    BnrCompRow& r = m_bnrAfxRows[i];
    if (r.bar) r.bar->hide();
    if (r.size) r.size->setText(humanSize(bytes));
    if (!r.detail) return;
    // Version inline; full sha256 lives in the tooltip (it's too wide to show).
    // When the build pins a newer version, append a "→ x.y" update hint.
    QString text = QStringLiteral("<b>%1</b>").arg(version.toHtmlEscaped());
    if (!newVersion.isEmpty() && newVersion != version)
        text += QStringLiteral(" <span style='color:#d8a000;'>→ %1</span>").arg(newVersion.toHtmlEscaped());
    r.detail->setText(text);
    if (!sha256.isEmpty())
        r.detail->setToolTip(QStringLiteral("%1\nsha256: %2").arg(version, sha256));
    r.detail->show();
}

void AetherDspWidget::clearBnrRows()
{
    for (BnrCompRow& r : m_bnrAfxRows) {
        delete r.name;
        delete r.size;
        delete r.bar;
        delete r.detail;
    }
    m_bnrAfxRows.clear();
}

// ── DFNR Tab ────────────────────────────────────────────────────────────────

QWidget* AetherDspWidget::buildDfnrPage()
{
    auto* page = new QWidget;
    auto* vbox = new QVBoxLayout(page);
    // 20 px top breathing room between the DSP selector buttons and
    // the info paragraph; 10 px left margin for the controls body.
    vbox->setContentsMargins(10, 20, 0, 0);

    // GroupBox dropped — the rest of the AetherDSP applet uses simple
    // labelled rows, so the rounded-frame chrome around DFNR was the
    // odd one out.
    auto* grid = new QGridLayout;
    grid->setColumnStretch(1, 1);

    auto& s = AppSettings::instance();

    auto* info = new QLabel("AI-powered speech enhancement — higher fidelity than RNNoise "
                            "in high-noise HF environments. CPU-only, 10 ms latency, 48 kHz.");
    info->setWordWrap(true);
    AetherSDR::ThemeManager::instance().applyStyleSheet(info, "QLabel { color: {{color.text.secondary}}; font-size: 12px; }");
    {
        auto* infoRow = new QHBoxLayout;
        infoRow->setContentsMargins(0, 0, 10, 0);
        infoRow->addWidget(info);
        vbox->addLayout(infoRow);
    }

    // Reset Defaults on its own row between the info paragraph and the
    // slider grid — right-aligned with 10 px right padding to nudge it
    // inboard so it lines up over the slider value-label column below.
    {
        auto* resetRow = new QHBoxLayout;
        resetRow->setContentsMargins(0, 10, 10, 0);
        resetRow->addStretch(1);
        auto* dfnrResetBtn = makeResetIconButton();
        connect(dfnrResetBtn, &QPushButton::clicked,
                this, &AetherDspWidget::resetCurrentTab);
        resetRow->addWidget(dfnrResetBtn);
        vbox->addLayout(resetRow);
    }

    grid->addWidget(new QLabel("Attenuation Limit"), 1, 0);
    m_dfnrAttenSlider = new QSlider(Qt::Horizontal);
    m_dfnrAttenSlider->setRange(0, 100);
    m_dfnrAttenSlider->setValue(static_cast<int>(s.value("DfnrAttenLimit", "100").toFloat()));
    applyPrimarySliderStyle(m_dfnrAttenSlider);
    m_dfnrAttenSlider->setToolTip("Maximum noise attenuation in dB.\n"
                                   "0 dB = passthrough (no denoising)\n"
                                   "100 dB = maximum noise removal\n\n"
                                   "For weak signals: 20–30 dB\n"
                                   "For casual listening: 40–60 dB\n"
                                   "For strong signals: 80–100 dB");
    grid->addWidget(m_dfnrAttenSlider, 1, 1);
    m_dfnrAttenLabel = new QLabel(QString::number(m_dfnrAttenSlider->value()));
    m_dfnrAttenLabel->setFixedWidth(40);
    grid->addWidget(m_dfnrAttenLabel, 1, 2);

    connect(m_dfnrAttenSlider, &QSlider::valueChanged, this, [this](int v) {
        m_dfnrAttenLabel->setText(QString::number(v));
        float db = static_cast<float>(v);
        auto& s = AppSettings::instance();
        s.setValue("DfnrAttenLimit", QString::number(db, 'f', 0));
        s.save();
        emit dfnrAttenLimitChanged(db);
    });

    grid->addWidget(new QLabel("Post-Filter Beta"), 2, 0);
    m_dfnrBetaSlider = new QSlider(Qt::Horizontal);
    m_dfnrBetaSlider->setRange(0, 30);
    m_dfnrBetaSlider->setValue(static_cast<int>(s.value("DfnrPostFilterBeta", "0.0").toFloat() * 100));
    applyPrimarySliderStyle(m_dfnrBetaSlider);
    m_dfnrBetaSlider->setToolTip("Post-filter strength for additional noise suppression.\n"
                                  "0 = disabled (default)\n"
                                  "0.05–0.15 = subtle additional filtering\n"
                                  "0.15–0.30 = aggressive post-processing");
    grid->addWidget(m_dfnrBetaSlider, 2, 1);
    m_dfnrBetaLabel = new QLabel(QString::number(m_dfnrBetaSlider->value() / 100.0f, 'f', 2));
    m_dfnrBetaLabel->setFixedWidth(40);
    grid->addWidget(m_dfnrBetaLabel, 2, 2);

    connect(m_dfnrBetaSlider, &QSlider::valueChanged, this, [this](int v) {
        float beta = v / 100.0f;
        m_dfnrBetaLabel->setText(QString::number(beta, 'f', 2));
        auto& s = AppSettings::instance();
        s.setValue("DfnrPostFilterBeta", QString::number(beta, 'f', 2));
        s.save();
        emit dfnrPostFilterBetaChanged(beta);
    });

    vbox->addLayout(grid);
    vbox->addStretch();
    return page;
}

// ── Sync from saved settings ─────────────────────────────────────────────────

void AetherDspWidget::syncFromEngine()
{
    syncDspSelectorFromEngine();

    auto& s = AppSettings::instance();

    int gainMethod = s.value("NR2GainMethod", "2").toInt();
    if (auto* btn = m_nr2GainGroup->button(gainMethod))
        btn->setChecked(true);

    int npeMethod = s.value("NR2NpeMethod", "0").toInt();
    if (auto* btn = m_nr2NpeGroup->button(npeMethod))
        btn->setChecked(true);

    bool aeFilter = s.value("NR2AeFilter", "True").toString() == "True";
    m_nr2AeCheck->setChecked(aeFilter);

    int gainMax = static_cast<int>(s.value("NR2GainMax", "1.50").toFloat() * 100);
    m_nr2GainMaxSlider->setValue(gainMax);
    m_nr2GainMaxLabel->setText(QString::number(gainMax / 100.0f, 'f', 2));

    int smooth = static_cast<int>(s.value("NR2GainSmooth", "0.85").toFloat() * 100);
    m_nr2SmoothSlider->setValue(smooth);
    m_nr2SmoothLabel->setText(QString::number(smooth / 100.0f, 'f', 2));

    int qspp = static_cast<int>(s.value("NR2Qspp", "0.20").toFloat() * 100);
    m_nr2QsppSlider->setValue(qspp);
    m_nr2QsppLabel->setText(QString::number(qspp / 100.0f, 'f', 2));

    if (m_mnrEnableCheck) {
        { QSignalBlocker sb(m_mnrEnableCheck);
          m_mnrEnableCheck->setChecked(m_audio->mnrEnabled()); }
        { QSignalBlocker sb(m_mnrStrengthSlider);
          int strength = static_cast<int>(m_audio->mnrStrength() * 100.0f);
          m_mnrStrengthSlider->setValue(strength);
          m_mnrStrengthLabel->setText(QString::number(strength) + "%"); }
    }

    int noiseMethod = s.value("NR4NoiseEstimationMethod", "0").toInt();
    if (auto* btn = m_nr4MethodGroup->button(noiseMethod))
        btn->setChecked(true);

    bool adaptive = s.value("NR4AdaptiveNoise", "True").toString() == "True";
    m_nr4AdaptiveCheck->setChecked(adaptive);

    int reduction = static_cast<int>(s.value("NR4ReductionAmount", "10.0").toFloat() * 10);
    m_nr4ReductionSlider->setValue(reduction);
    m_nr4ReductionLabel->setText(QString::number(reduction / 10.0f, 'f', 1));

    int smoothing = static_cast<int>(s.value("NR4SmoothingFactor", "0.0").toFloat());
    m_nr4SmoothingSlider->setValue(smoothing);
    m_nr4SmoothingLabel->setText(QString::number(smoothing));

    int whitening = static_cast<int>(s.value("NR4WhiteningFactor", "0.0").toFloat());
    m_nr4WhiteningSlider->setValue(whitening);
    m_nr4WhiteningLabel->setText(QString::number(whitening));

    int masking = static_cast<int>(s.value("NR4MaskingDepth", "0.50").toFloat() * 100);
    m_nr4MaskingSlider->setValue(masking);
    m_nr4MaskingLabel->setText(QString::number(masking / 100.0f, 'f', 2));

    int suppression = static_cast<int>(s.value("NR4SuppressionStrength", "0.50").toFloat() * 100);
    m_nr4SuppressionSlider->setValue(suppression);
    m_nr4SuppressionLabel->setText(QString::number(suppression / 100.0f, 'f', 2));

    if (m_dfnrAttenSlider) {
        int atten = static_cast<int>(s.value("DfnrAttenLimit", "100").toFloat());
        m_dfnrAttenSlider->setValue(atten);
        m_dfnrAttenLabel->setText(QString::number(atten));
    }
    if (m_dfnrBetaSlider) {
        int beta = static_cast<int>(s.value("DfnrPostFilterBeta", "0.0").toFloat() * 100);
        m_dfnrBetaSlider->setValue(beta);
        m_dfnrBetaLabel->setText(QString::number(beta / 100.0f, 'f', 2));
    }
}

} // namespace AetherSDR
