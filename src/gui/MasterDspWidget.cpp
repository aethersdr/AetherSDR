#include "MasterDspWidget.h"
#include "core/AudioEngine.h"
#include "core/AppSettings.h"
#include "GuardedSlider.h"

#include <QRegularExpression>
#include <QSet>

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QStackedWidget>
#include <QRadioButton>
#include <QButtonGroup>
#include <QCheckBox>
#include <QLabel>
#include <QPushButton>
#include <QPainter>
#include <QSignalBlocker>

namespace MasterSDR {

namespace {

const QString kSliderStyle = QStringLiteral(
    "QSlider::groove:horizontal { height: 4px; background: #304050; border-radius: 2px; }"
    "QSlider::handle:horizontal { width: 12px; height: 12px; margin: -4px 0;"
    "  background: #c8d8e8; border-radius: 6px; }"
    "QSlider::handle:horizontal:hover { background: #00b4d8; }");

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

MasterDspWidget::MasterDspWidget(AudioEngine* audio, QWidget* parent)
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
        // BNR (NVIDIA GPU neural denoising) is gated at compile time
        // by HAVE_BNR — VfoWidget hides its button entirely; here we
        // keep the slot visible so the 6-button row stays balanced and
        // just dim it for builds without the BNR backend.
#ifndef HAVE_BNR
        if (i == BNR) {
            b->setEnabled(false);
            b->setToolTip("BNR requires the NVIDIA Broadcast SDK and an NVIDIA GPU.");
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
                this, &MasterDspWidget::syncDspSelectorFromEngine);
        connect(m_audio, &AudioEngine::nr4EnabledChanged,
                this, &MasterDspWidget::syncDspSelectorFromEngine);
        connect(m_audio, &AudioEngine::mnrEnabledChanged,
                this, &MasterDspWidget::syncDspSelectorFromEngine);
        connect(m_audio, &AudioEngine::dfnrEnabledChanged,
                this, &MasterDspWidget::syncDspSelectorFromEngine);
        connect(m_audio, &AudioEngine::rn2EnabledChanged,
                this, &MasterDspWidget::syncDspSelectorFromEngine);
        connect(m_audio, &AudioEngine::bnrEnabledChanged,
                this, &MasterDspWidget::syncDspSelectorFromEngine);
    }

    syncDspSelectorFromEngine();
    syncFromEngine();
}

void MasterDspWidget::onDspButtonClicked(int index, bool nowChecked)
{
    if (index < 0 || index >= NumDsps) return;
    // Always bring this DSP's panel forward, regardless of new check
    // state — toggling off keeps the panel visible so the user can
    // re-enable from the same place.
    m_dspStack->setCurrentIndex(index);
    if (!m_audio) return;
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
                case BNR:  m_audio->setBnrEnabled(nowChecked); break;
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

void MasterDspWidget::syncDspSelectorFromEngine()
{
    if (!m_audio) return;
    const bool on[NumDsps] = {
        m_audio->nr2Enabled(),
        m_audio->nr4Enabled(),
        m_audio->mnrEnabled(),
        m_audio->dfnrEnabled(),
        m_audio->rn2Enabled(),
        m_audio->bnrEnabled(),
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

void MasterDspWidget::resetCurrentTab()
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

void MasterDspWidget::setCompactMode(bool on)
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

void MasterDspWidget::setDialogMode(bool on)
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

void MasterDspWidget::setNr2Available(bool available, const QString& tooltip)
{
    if (auto* btn = m_dspBtns[NR2]) {
        btn->setEnabled(available);
        btn->setToolTip(tooltip);
    }
}

void MasterDspWidget::selectTab(const QString& name)
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

QWidget* MasterDspWidget::buildNr2Page()
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
                this, &MasterDspWidget::resetCurrentTab);
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
        m_nr2GainMaxSlider->setStyleSheet(kSliderStyle);
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
        m_nr2SmoothSlider->setStyleSheet(kSliderStyle);
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
        m_nr2QsppSlider->setStyleSheet(kSliderStyle);
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

QWidget* MasterDspWidget::buildNr4Page()
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
                this, &MasterDspWidget::resetCurrentTab);
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
        m_nr4ReductionSlider->setStyleSheet(kSliderStyle);
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
        m_nr4SmoothingSlider->setStyleSheet(kSliderStyle);
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
        m_nr4WhiteningSlider->setStyleSheet(kSliderStyle);
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
        m_nr4MaskingSlider->setStyleSheet(kSliderStyle);
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
        m_nr4SuppressionSlider->setStyleSheet(kSliderStyle);
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

QWidget* MasterDspWidget::buildMnrPage()
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
                this, &MasterDspWidget::resetCurrentTab);
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
        m_mnrStrengthSlider->setStyleSheet(kSliderStyle);
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
    info->setStyleSheet("QLabel { color: #8aa8c0; font-size: 11px; }");
    vbox->addSpacing(8);
    vbox->addWidget(info);

    vbox->addStretch();
    return page;
}

// ── RN2 Tab ─────────────────────────────────────────────────────────────────

QWidget* MasterDspWidget::buildRn2Page()
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
    lbl->setStyleSheet("QLabel { color: #8aa8c0; font-size: 12px; }");
    vbox->addWidget(lbl);
    vbox->addStretch();
    return page;
}

// ── BNR Tab ─────────────────────────────────────────────────────────────────

QWidget* MasterDspWidget::buildBnrPage()
{
    auto* page = new QWidget;
    auto* vbox = new QVBoxLayout(page);
    auto* lbl = new QLabel(
        "NVIDIA Broadcast — GPU-accelerated AI noise removal.  Strongest "
        "against non-stationary noise (typing, traffic, dogs barking).  "
        "Requires an NVIDIA GPU with the Broadcast SDK.  Intensity is "
        "controlled from the slice overlay menu.");
    lbl->setWordWrap(true);
    lbl->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    lbl->setStyleSheet("QLabel { color: #8aa8c0; font-size: 12px; }");
    vbox->addWidget(lbl);
    vbox->addStretch();
    return page;
}

// ── DFNR Tab ────────────────────────────────────────────────────────────────

QWidget* MasterDspWidget::buildDfnrPage()
{
    auto* page = new QWidget;
    auto* vbox = new QVBoxLayout(page);
    // 20 px top breathing room between the DSP selector buttons and
    // the info paragraph; 10 px left margin for the controls body.
    vbox->setContentsMargins(10, 20, 0, 0);

    // GroupBox dropped — the rest of the MasterDsp applet uses simple
    // labelled rows, so the rounded-frame chrome around DFNR was the
    // odd one out.
    auto* grid = new QGridLayout;
    grid->setColumnStretch(1, 1);

    auto& s = AppSettings::instance();

    auto* info = new QLabel("AI-powered speech enhancement — higher fidelity than RNNoise "
                            "in high-noise HF environments. CPU-only, 10 ms latency, 48 kHz.");
    info->setWordWrap(true);
    info->setStyleSheet("QLabel { color: #8aa8c0; font-size: 12px; }");
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
                this, &MasterDspWidget::resetCurrentTab);
        resetRow->addWidget(dfnrResetBtn);
        vbox->addLayout(resetRow);
    }

    grid->addWidget(new QLabel("Attenuation Limit"), 1, 0);
    m_dfnrAttenSlider = new QSlider(Qt::Horizontal);
    m_dfnrAttenSlider->setRange(0, 100);
    m_dfnrAttenSlider->setValue(static_cast<int>(s.value("DfnrAttenLimit", "100").toFloat()));
    m_dfnrAttenSlider->setStyleSheet(kSliderStyle);
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
    m_dfnrBetaSlider->setStyleSheet(kSliderStyle);
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

void MasterDspWidget::syncFromEngine()
{
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

} // namespace MasterSDR
