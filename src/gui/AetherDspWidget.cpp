#include "AetherDspWidget.h"
#include "core/AudioEngine.h"
#include "core/AppSettings.h"
#include "core/NnrSettings.h"
#include "core/NvidiaBnrSettings.h"
#include "models/Nr2SettingsModel.h"
#include "models/Rn2SettingsModel.h"
#include "GuardedSlider.h"
#include "ModemChrome.h"
#include "NrGainStrip.h"
#include "Theme.h"
#include "ScopedChildWidget.h"

#include <QRegularExpression>
#include <QSet>
#include <QHash>

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QStackedWidget>
#include <QFrame>
#include <QRadioButton>
#include <QButtonGroup>
#include <QLineEdit>
#include <QProgressBar>
#include <QMessageBox>
#include <QPointer>
#ifdef HAVE_NVIDIA_AFX
#include "core/NvidiaAfxPack.h"
#endif
#include <QCheckBox>
#include <QLabel>
#include <QPushButton>
#include <QPainter>
#include <QSignalBlocker>
#include "core/ThemeManager.h"

#include <algorithm>
#include <cmath>

namespace AetherSDR {

namespace {

const char* dspNameForIndex(int index)
{
    switch (static_cast<AetherDspWidget::DspId>(index)) {
    case AetherDspWidget::NR2:  return "NR2";
    case AetherDspWidget::NR4:  return "NR4";
    case AetherDspWidget::MNR:  return "MNR";
    case AetherDspWidget::DFNR: return "DFNR";
    case AetherDspWidget::RN2:  return "RN2";
    case AetherDspWidget::BNR:  return "BNR";
    case AetherDspWidget::NNR:  return "NNR";
    case AetherDspWidget::NumDsps:
        break;
    }
    return "";
}

const QString kDfnrUnavailableToolTip = QStringLiteral(
    "DFNR requires DeepFilterNet to be set up and AetherSDR rebuilt.");

void rememberLastClientNr(int index)
{
    const char* name = dspNameForIndex(index);
    if (name[0] == '\0') {
        return;
    }

    auto& s = AppSettings::instance();
    const QString value = QString::fromLatin1(name);
    if (s.value("LastClientNr", QString()).toString() == value) {
        return;
    }

    s.setValue("LastClientNr", value);
    s.save();
}

void clearUnavailableDfnrPreference()
{
#ifndef HAVE_DFNR
    auto& s = AppSettings::instance();
    if (s.value("LastClientNr", QString()).toString() == QLatin1String("DFNR")) {
        s.remove("LastClientNr");
        s.save();
    }
#endif
}

// The AetherDSP body wears the AetherModem window's chrome (ModemChrome.h):
// the same gradient panels, green accent and section labels, at two scales —
// the Settings dialog runs at the modem's own 14 px, the docked applet at a
// compact 11 px. Both come from one sheet, so the two paths cannot drift.
QString widgetStyle(bool compact)
{
    return ModemChrome::styleSheet(compact ? ModemChrome::Scale::Compact
                                           : ModemChrome::Scale::Dialog);
}

// ── Chrome builders — the modem's structural object names ────────────────────

QFrame* modemPanel(const QString& objectName, QWidget* parent = nullptr)
{
    auto* frame = new QFrame(parent);
    frame->setObjectName(objectName);
    // Without this a styled QFrame paints its background from the palette and
    // the gradient in the sheet never shows — the same attribute the modem's
    // own panel() helper sets.
    frame->setAttribute(Qt::WA_StyledBackground, true);
    return frame;
}

QFrame* controlsFrame(QWidget* parent = nullptr)
{
    return modemPanel(QStringLiteral("ControlsFrame"), parent);
}

// One labelled column inside a ControlsFrame. `last` drops the divider that
// separates it from the column on its right.
QFrame* controlCell(QWidget* parent = nullptr, bool last = false)
{
    return modemPanel(last ? QStringLiteral("ControlCellLast")
                           : QStringLiteral("ControlCell"), parent);
}

// Sliders take their look from the chrome sheet rather than the app-wide
// primary style: a per-widget stylesheet would win over the sheet and leave
// this window with the blue grooves of everywhere else. The hover suppressor
// still goes on, since that is behaviour, not styling, and plain QSliders here
// would otherwise lose it (GuardedSlider installs its own).
void applyChromeSliderStyle(QWidget* slider)
{
    if (!slider) return;
    slider->installEventFilter(&detail::SliderHoverSuppressor::instance());
}

QLabel* sectionLabel(const QString& text, QWidget* parent = nullptr)
{
    auto* label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("SectionLabel"));
    return label;
}

// One tab in the method strip. Sized by the layout rather than by its text, so
// the seven share the row evenly and the strip stays the same shape whichever
// labels it carries; Ignored horizontally lets "DFNR" shrink its own text
// rather than force the row wider than the 280 px applet.
static QPushButton* makeTabButton(const QString& text)
{
    auto* b = new QPushButton(text);
    // Selected by property rather than objectName: the caller overwrites
    // objectName with the per-method id the automation bridge addresses these
    // by, which would silently drop them back to plain push buttons.
    b->setProperty("chrome", "tab");
    b->setCheckable(true);
    b->setFlat(true);
    b->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    b->setMinimumWidth(0);
    return b;
}

// An exclusive choice inside a control cell. A real QRadioButton rather than a
// checkable button: this is a one-of-N pick, and the modem chrome draws radios
// as such — which also hands assistive technology the right role, where a row
// of checkable buttons announced each option as its own toggle (#4896).
static QRadioButton* makeOptionRadio(const QString& text)
{
    auto* b = new QRadioButton(text);
    // The enclosing QButtonGroup owns exclusivity; Qt's own sibling-based
    // exclusivity would otherwise also bind radios from two different rows that
    // happen to share a parent widget.
    b->setAutoExclusive(false);
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
        // Icon-only, so the label a screen reader reads has to come from
        // somewhere other than the (empty) text.
        setAccessibleName(QStringLiteral("Reset Defaults"));
        setCursor(Qt::PointingHandCursor);
        // Sized and bordered by the chrome sheet's QPushButton#IconButton rule
        // so it matches the other buttons in its row at either scale, instead
        // of being a bare 18 px glyph floating against the panel.
        setObjectName(QStringLiteral("IconButton"));
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    }

protected:
    void paintEvent(QPaintEvent* event) override
    {
        // Let the chrome sheet draw the button itself (panel, border, hover),
        // then put the glyph on top — a paintEvent that skipped the base class
        // left the icon floating with no button around it.
        QPushButton::paintEvent(event);

        QPainter p(this);
        p.setRenderHint(QPainter::TextAntialiasing);
        p.setRenderHint(QPainter::Antialiasing);
        QColor c = ModemChrome::colour(ModemChrome::Colour::Section);
        if (isDown())          c = ModemChrome::colour(ModemChrome::Colour::GreenBright);
        else if (underMouse()) c = ModemChrome::colour(ModemChrome::Colour::TextBright);
        p.setPen(c);
        QFont f = font();
        f.setPixelSize(std::min(18, qRound(height() * 0.58)));
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

} // namespace

AetherDspWidget::AetherDspWidget(AudioEngine* audio, QWidget* parent)
    : QWidget(parent)
    , m_audio(audio)
{
    AetherSDR::ThemeManager::instance().applyStyleSheet(this, widgetStyle(false));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(4, 4, 4, 4);
    root->setSpacing(6);

    // Method strip — seven exclusive tabs that double as DSP activators.
    // Checked state == engine enable state; click the checked one again to
    // deactivate (chain bypass), which is why these are checkable buttons in a
    // group rather than a QTabBar. They share the row evenly at whatever width
    // the container gives, so the strip fits the 280 px applet and the dialog
    // from the same code.
    auto* tabsFrame = modemPanel(QStringLiteral("TabsFrame"), this);
    auto* btnRow = new QHBoxLayout(tabsFrame);
    btnRow->setContentsMargins(0, 0, 0, 0);
    btnRow->setSpacing(0);
    static const char* kLabels[NumDsps] = {"NR2", "NR4", "MNR", "DFNR", "RN2", "BNR", "NNR"};
    for (int i = 0; i < NumDsps; ++i) {
        auto* b = makeTabButton(kLabels[i]);
        // Name each selector for screen readers and the automation bridge.
        // Checkable buttons report value as "checked"/"unchecked", so without
        // an objectName/accessibleName a driver (or assistive tech) can't tell
        // NR2 from BNR — only a screenshot could. Stable objectName lets a
        // driver target a specific method; accessibleName carries the label.
        b->setObjectName(QStringLiteral("dspMethodBtn") + QLatin1String(kLabels[i]));
        b->setAccessibleName(QString::fromLatin1(kLabels[i])
                             + QStringLiteral(" noise-reduction method"));
        // MNR (MMSE-Wiener spectral NR) is implemented only on macOS —
        // dim the selector on Windows / Linux so users can see it exists
        // but can't enable a path the engine has no backend for.
#ifndef Q_OS_MAC
        if (i == MNR) {
            b->setEnabled(false);
            b->setToolTip("MNR is only available on macOS.");
            b->setAccessibleDescription(tr("MNR is only available on macOS."));
        }
#endif
        // NR4 (libspecbleach spectral NR) requires clang-cl on Windows to
        // compile its C99 VLAs — disabled when LLVM is not installed.
#ifndef HAVE_SPECBLEACH
        if (i == NR4) {
            b->setEnabled(false);
            b->setToolTip("NR4 requires LLVM (clang-cl) on Windows.\n"
                          "Install LLVM from llvm.org and rebuild to enable NR4.");
            b->setAccessibleDescription(
                tr("NR4 requires LLVM (clang-cl) on Windows. Install LLVM and "
                   "rebuild to enable NR4."));
        }
#endif
        // DFNR is cross-platform only when the matching DeepFilterNet library
        // is present at configure time. If HAVE_DFNR is absent, keep the slot
        // visible like the other optional methods but do not let the UI select
        // a no-op engine stub.
#ifndef HAVE_DFNR
        if (i == DFNR) {
            b->setEnabled(false);
            b->setToolTip(kDfnrUnavailableToolTip);
            b->setAccessibleDescription(kDfnrUnavailableToolTip);
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
            b->setAccessibleDescription(
                tr("BNR requires an NVIDIA RTX or GeForce GPU; not available in "
                   "this build."));
#endif
        }
#else
        if (i == BNR && !NvidiaAfxPack::hasSupportedGpu()) {
            b->setEnabled(false);
            if (NvidiaAfxPack::isAfxCapableGpu()) {
                // Recent NVIDIA card, but no AFX pack is published for its arch
                // yet (e.g. sm_120 / RTX 50-series). Don't imply the GPU is too
                // old — say so plainly and point at DFNR. (#3933)
                const QString reason =
                    QStringLiteral("No BNR pack for your GPU (%1) yet — "
                                   "DFNR remains available.")
                        .arg(NvidiaAfxPack::detectArch());
                b->setToolTip(reason);
                b->setAccessibleDescription(reason);
            } else {
                b->setToolTip("BNR requires an NVIDIA RTX 40-series or later GPU.\n"
                              "Use DFNR for AI noise removal on other hardware.");
                b->setAccessibleDescription(
                    tr("BNR requires an NVIDIA RTX 40-series or later GPU. Use "
                       "DFNR for AI noise removal on other hardware."));
            }
        }
#endif
        m_dspBtns[i] = b;
        connect(b, &QPushButton::clicked, this,
                [this, i](bool nowChecked) { onDspButtonClicked(i, nowChecked); });
        btnRow->addWidget(b, 1);
    }
    root->addWidget(tabsFrame);

    // Page stack — one panel per DSP.  Order MUST match DspId.
    m_dspStack = new QStackedWidget;
    m_dspStack->addWidget(buildNr2Page());
    m_dspStack->addWidget(buildNr4Page());
    m_dspStack->addWidget(buildMnrPage());
    m_dspStack->addWidget(buildDfnrPage());
    m_dspStack->addWidget(buildRn2Page());
    m_dspStack->addWidget(buildBnrPage());
    m_dspStack->addWidget(buildNnrPage());
    root->addWidget(m_dspStack, 1);

    // Status strip — the modem's slim footer, shared by every tab: which
    // method is running and how it is configured on the left, and on the right
    // a live trace of how much that method is actually taking out. One strip
    // below the stack rather than one per page, so switching tabs does not
    // move it.
    {
        auto* statusFrame = modemPanel(QStringLiteral("StatusFrame"), this);
        m_statusFrame = statusFrame;
        auto* row = new QHBoxLayout(statusFrame);
        row->setContentsMargins(10, 5, 10, 5);
        row->setSpacing(8);

        m_statusDot = new QLabel(statusFrame);
        m_statusDot->setObjectName(QStringLiteral("StatusDot"));
        row->addWidget(m_statusDot);

        row->addWidget(sectionLabel(QStringLiteral("METHOD"), statusFrame));
        m_statusValue = new QLabel(statusFrame);
        m_statusValue->setObjectName(QStringLiteral("StatusValue"));
        // The reading is the accessible answer to "what is my NR doing" — the
        // trace beside it is decorative to a screen reader, so the text has to
        // carry the state on its own (#4896).
        m_statusValue->setAccessibleName(
            QStringLiteral("Noise reduction status"));
        row->addWidget(m_statusValue);
        row->addStretch(1);

        m_gainLabel = sectionLabel(QStringLiteral("NR GAIN"), statusFrame);
        row->addWidget(m_gainLabel);
        m_gainStrip = new NrGainStrip(statusFrame);
        m_gainStrip->setMinimumHeight(18);
        m_gainStrip->setMaximumHeight(20);
        m_gainStrip->setMinimumWidth(120);
        row->addWidget(m_gainStrip, 2);

        root->addWidget(statusFrame);

        if (m_audio) {
            connect(m_audio, &AudioEngine::nrGainChanged, this,
                    [this](float gain, bool active) {
                if (m_gainStrip) m_gainStrip->setGain(gain, active);
            });
            m_gainStrip->setGain(m_audio->nrGain(), m_audio->nrGainActive());
        }
    }

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

    connect(&Nr2SettingsModel::instance(),
            &Nr2SettingsModel::configChanged,
            this, &AetherDspWidget::syncNr2Settings);

    syncDspSelectorFromEngine();
    syncFromEngine();
    clearUnavailableDfnrPreference();
}

void AetherDspWidget::onDspButtonClicked(int index, bool nowChecked)
{
    if (index < 0 || index >= NumDsps) return;
    if (m_dspBtns[index] && !m_dspBtns[index]->isEnabled()) {
        syncDspSelectorFromEngine();
        return;
    }
#ifndef HAVE_DFNR
    if (index == DFNR) {
        syncDspSelectorFromEngine();
        return;
    }
#endif
    static const char* kNames[NumDsps] = {"NR2", "NR4", "MNR", "DFNR", "RN2", "BNR", "NNR"};
    emit dspMethodUserToggled(QString::fromLatin1(kNames[index]), nowChecked);

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
        AudioEngine* audio = m_audio;
        QPointer<AetherDspWidget> self(this);
        QMetaObject::invokeMethod(audio, [audio, self, index, nowChecked]() {
            switch (index) {
                case NR2:  audio->setNr2Enabled(nowChecked); break;
                case NR4:  audio->setNr4Enabled(nowChecked); break;
                case MNR:  audio->setMnrEnabled(nowChecked); break;
                case DFNR: audio->setDfnrEnabled(nowChecked); break;
                case RN2:  audio->setRn2Enabled(nowChecked); break;
                case BNR:  audio->setNvAfxEnabled(nowChecked); break;  // local AFX
                case NNR:  audio->setNnrEnabled(nowChecked); break;
                case NumDsps: break;
            }
            if (self) {
                QMetaObject::invokeMethod(self.data(), [self]() {
                    if (self) {
                        self->syncDspSelectorFromEngine();
                    }
                }, Qt::QueuedConnection);
            }
        });
    }
    // AudioEngine cascades exclusion (enabling NR2 disables DFNR, etc.)
    // and emits *EnabledChanged signals; syncDspSelectorFromEngine()
    // will update sibling button states and persist the last-enabled method
    // only after engine state proves it actually became active.
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
        m_audio->nnrEnabled(),
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
    if (active >= 0) {
        rememberLastClientNr(active);
    }
    if (active >= 0 && m_dspStack)
        m_dspStack->setCurrentIndex(active);

    // A method change makes the old method's trace meaningless — start the
    // strip over rather than letting the two run together.
    if (m_gainStrip && active != m_lastActiveDsp) {
        m_gainStrip->reset();
    }
    m_lastActiveDsp = active;
    refreshStatusStrip();
}

// The status strip's left-hand reading: which method the engine has running,
// and the settings that decide what it sounds like. Built from the controls
// rather than from the engine so it stays correct for a method whose knobs the
// engine exposes only indirectly, and so it updates the instant the operator
// moves one.
void AetherDspWidget::refreshStatusStrip()
{
    if (!m_statusValue || !m_statusDot) {
        return;
    }

    int active = -1;
    for (int i = 0; i < NumDsps; ++i) {
        if (m_dspBtns[i] && m_dspBtns[i]->isChecked()) {
            active = i;
            break;
        }
    }

    // Grey is "nothing is running" — distinct from the green of a method that
    // is running but currently passing everything through.
    QColor dotColour = ModemChrome::colour(ModemChrome::Colour::Green);
    QString text;
    switch (active) {
    case NR2: {
        static const char* kGain[] = {"Linear", "Log", "Gamma", "Trained"};
        static const char* kNpe[]  = {"OSMS", "MMSE", "NSTAT"};
        const int gainId = m_nr2GainGroup ? m_nr2GainGroup->checkedId() : -1;
        const int npeId  = m_nr2NpeGroup ? m_nr2NpeGroup->checkedId() : -1;
        text = QStringLiteral("NR2 · %1 / %2")
            .arg(QString::fromLatin1(gainId >= 0 && gainId < 4 ? kGain[gainId] : "?"))
            .arg(QString::fromLatin1(npeId >= 0 && npeId < 3 ? kNpe[npeId] : "?"));
        if (m_nr2AeCheck && m_nr2AeCheck->isChecked())
            text += QStringLiteral(" · AE on");
        if (m_nr2Post2Check && m_nr2Post2Check->isChecked())
            text += QStringLiteral(" · noise fill on");
        break;
    }
    case NR4:
        text = QStringLiteral("NR4 · reduction %1 dB")
            .arg(m_nr4ReductionLabel ? m_nr4ReductionLabel->text() : QString());
        break;
    case MNR:
        text = QStringLiteral("MNR · strength %1")
            .arg(m_mnrStrengthLabel ? m_mnrStrengthLabel->text() : QString());
        break;
    case DFNR:
        text = QStringLiteral("DFNR · attenuation limit %1 dB")
            .arg(m_dfnrAttenLabel ? m_dfnrAttenLabel->text() : QString());
        break;
    case RN2:
        text = QStringLiteral("RN2 · noise floor %1")
            .arg(m_rn2DryMixLabel ? m_rn2DryMixLabel->text() : QString());
        break;
    case BNR: {
        // The BNR label is rich text with its own coloured bullet; the strip
        // has a dot of its own, so take the words and leave both behind.
        QString status = m_bnrAfxStatus
            ? m_bnrAfxStatus->text()
                  .remove(QRegularExpression(QStringLiteral("<[^>]*>")))
                  .remove(QChar(0x25CF))
                  .simplified()
            : QStringLiteral("NVIDIA AFX");
        text = QStringLiteral("BNR · %1").arg(status);
        break;
    }
    case NNR: {
        const int slot = m_nnrModelGroup ? m_nnrModelGroup->checkedId() : 0;
        text = QStringLiteral("NNR · %1 · floor %2")
            .arg(slot == 1 ? QStringLiteral("Premium") : QStringLiteral("Standard"))
            .arg(m_nnrStrengthLabel ? m_nnrStrengthLabel->text() : QString());
        break;
    }
    default:
        dotColour = ThemeManager::instance().color(
            QStringLiteral("color.text.label"));
        text = QStringLiteral("No method running");
        break;
    }

    m_statusValue->setText(text);
    m_statusValue->setAccessibleDescription(text);
    m_statusDot->setStyleSheet(
        QStringLiteral("QLabel#StatusDot { background: %1; border-radius: 6px; "
                       "min-width: 12px; max-width: 12px; min-height: 12px; "
                       "max-height: 12px; }")
            .arg(dotColour.name(QColor::HexRgb)));
}

void AetherDspWidget::resetCurrentTab()
{
    if (!m_dspStack) return;
    const int idx = m_dspStack->currentIndex();
    static const char* kNames[NumDsps] = {"NR2", "NR4", "MNR", "DFNR", "RN2", "BNR", "NNR"};
    const QString name = (idx >= 0 && idx < NumDsps) ? kNames[idx] : QString();
    if (name == "NR2") {
        // click() is intentional: setChecked() would update the UI without
        // emitting QButtonGroup::idClicked, leaving the settings model and
        // live NR2 instances on the previous method.
        if (m_nr2GainGroup) m_nr2GainGroup->button(2)->click();
        if (m_nr2NpeGroup)  m_nr2NpeGroup->button(0)->click();
        if (m_nr2AeCheck)        m_nr2AeCheck->setChecked(true);
        if (m_nr2GainMaxSlider)  m_nr2GainMaxSlider->setValue(100);
        if (m_nr2GainFloorSlider)m_nr2GainFloorSlider->setValue(0);
        if (m_nr2SmoothSlider)   m_nr2SmoothSlider->setValue(85);
        if (m_nr2QsppSlider)     m_nr2QsppSlider->setValue(20);
        // Upstream's post2 defaults: off, 0.15 level, 0.15 white blend.
        if (m_nr2Post2Check)        m_nr2Post2Check->setChecked(false);
        if (m_nr2Post2NlevelSlider) m_nr2Post2NlevelSlider->setValue(15);
        if (m_nr2Post2FactorSlider) m_nr2Post2FactorSlider->setValue(15);
        if (m_nr2Post2TaperSlider)  m_nr2Post2TaperSlider->setValue(2871);
    } else if (name == "NNR") {
        // Every NNR control resets to the value WDSP itself starts from, which
        // is the value its marker is drawn at — NnrControls.h is the one place
        // both come from, so "reset" and "the mark" cannot disagree.
        if (m_nnrStrengthSlider) {
            m_nnrStrengthSlider->setValue(Nnr::kMaskFloorDefaultStrength);
        }
        if (m_nnrModelGroup) {
            if (auto* b = m_nnrModelGroup->button(0)) b->click();
        }
        for (auto& c : m_nnrAdvanced) {
            if (c.slider) {
                c.slider->setValue(
                    static_cast<int>(std::lround(c.spec->defaultValue * c.scale)));
            }
        }
    } else if (name == "NR4") {
        if (m_nr4MethodGroup)      m_nr4MethodGroup->button(0)->setChecked(true);
        if (m_nr4AdaptiveCheck)    m_nr4AdaptiveCheck->setChecked(true);
        if (m_nr4ReductionSlider)  m_nr4ReductionSlider->setValue(100);
        if (m_nr4SmoothingSlider)  m_nr4SmoothingSlider->setValue(0);
        if (m_nr4WhiteningSlider)  m_nr4WhiteningSlider->setValue(0);
        if (m_nr4MaskingSlider)    m_nr4MaskingSlider->setValue(50);
        if (m_nr4SuppressionSlider)m_nr4SuppressionSlider->setValue(50);
    } else if (name == "MNR") {
        if (m_mnrStrengthSlider) m_mnrStrengthSlider->setValue(100);
    } else if (name == "DFNR") {
        if (m_dfnrAttenSlider) m_dfnrAttenSlider->setValue(100);
        if (m_dfnrBetaSlider)  m_dfnrBetaSlider->setValue(0);
    } else if (name == "RN2") {
        if (m_rn2DryMixSlider) m_rn2DryMixSlider->setValue(0);
    }
    // BNR has no adjustable parameters — Reset Defaults is a no-op there.
}

void AetherDspWidget::setCompactMode(bool on)
{
    AetherSDR::ThemeManager::instance().applyStyleSheet(this, widgetStyle(on));

    // Slider value labels were sized to fit the full-dialog 40 px slot.
    // In compact mode they're rendered with a smaller font and fit in 30
    // px — narrower labels free up width for the slider grooves so the
    // tile reads well at the 280 px PooDoo container limit.
    const int valWidth = on ? 30 : 40;
    for (auto* lbl : { m_nr2GainMaxLabel, m_nr2GainFloorLabel,
                       m_nr2SmoothLabel, m_nr2QsppLabel,
                       m_nr4ReductionLabel, m_nr4SmoothingLabel, m_nr4WhiteningLabel,
                       m_nr4MaskingLabel, m_nr4SuppressionLabel,
                       m_mnrStrengthLabel,
                       m_dfnrAttenLabel, m_dfnrBetaLabel }) {
        if (lbl) lbl->setMinimumWidth(valWidth);
    }
    if (m_dfnrAttenLabel) m_dfnrAttenLabel->setFixedWidth(valWidth);
    if (m_dfnrBetaLabel)  m_dfnrBetaLabel->setFixedWidth(valWidth);

    // The trace needs less room than the reading does; below ~200 px of strip
    // the applet would rather spend the width on the text.
    if (m_gainStrip) m_gainStrip->setMinimumWidth(on ? 70 : 120);
    if (m_gainLabel) m_gainLabel->setVisible(!on);
}

void AetherDspWidget::setDialogMode(bool on)
{
    if (!on) return;  // applet path is the default; one-way switch for the dialog

    // Nothing left to do per-widget: the dialog scale IS the chrome sheet's
    // Dialog scale, which the constructor already applied. This used to hunt
    // down every checkable QPushButton and QLabel to bump inline font sizes,
    // because each control carried its own stylesheet; the controls now
    // inherit one sheet, so a second pass would only fight it.
}

void AetherDspWidget::setNr2Available(bool available, const QString& tooltip)
{
    if (auto* btn = m_dspBtns[NR2]) {
        btn->setEnabled(available);
        btn->setToolTip(tooltip);
        // Why NR2 is unavailable (compressed Opus/SmartLink audio, #1597) has
        // to reach a screen reader too, not just a hover (#4896).
        btn->setAccessibleDescription(tooltip);
    }
}

void AetherDspWidget::selectTab(const QString& name)
{
    if (!m_dspStack) return;
    static const char* kNames[NumDsps] = {"NR2", "NR4", "MNR", "DFNR", "RN2", "BNR", "NNR"};
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
    vbox->setContentsMargins(0, 0, 0, 0);
    vbox->setSpacing(6);

    // Three panels, the way the modem groups its controls: what the method is,
    // what it does afterwards, and the mask itself.
    auto* methodFrame = controlsFrame(page);
    auto* methodRow = new QHBoxLayout(methodFrame);
    methodRow->setContentsMargins(12, 10, 12, 10);
    methodRow->setSpacing(16);
    vbox->addWidget(methodFrame);

    auto labelStyle = QStringLiteral(
        "QLabel { color: #8090a0; font-size: 11px; }"
        "QLabel:disabled { color: #48515a; }");
    auto valStyle = QStringLiteral(
        "QLabel { color: #c8d8e8; font-size: 11px; min-width: 40px; }"
        "QLabel:disabled { color: #48515a; }");

    // Every label on this page is styled through here rather than each one
    // calling setStyleSheet itself. That is what the hardcoded-colour ratchet
    // asks for -- it counts call sites, not colours, so fourteen scattered
    // calls are fourteen places to migrate when these two strings become
    // theme tokens, and this is one.
    const auto styled = [](QLabel* label, const QString& style) {
        label->setStyleSheet(style);
        return label;
    };

    // Gain Method — exclusive toggle row, styled like the slice DSP buttons.
    {
        auto* gainCell = controlCell(methodFrame);
        auto* cellBox = new QVBoxLayout(gainCell);
        cellBox->setContentsMargins(0, 0, 16, 0);
        cellBox->setSpacing(8);
        cellBox->addWidget(sectionLabel(QStringLiteral("GAIN METHOD"), gainCell));

        // Two columns rather than one row of four. A QRadioButton clips its
        // text rather than eliding it, and four of them on one line lost their
        // last characters ("Linea", "Train") as soon as the widget was narrower
        // than the Settings dialog — which it is inside the Aetherial strip,
        // and far more so in the docked applet.
        auto* row = new QGridLayout;
        row->setContentsMargins(0, 0, 0, 0);
        row->setHorizontalSpacing(14);
        row->setVerticalSpacing(6);
        m_nr2GainGroup = new QButtonGroup(this);
        m_nr2GainGroup->setExclusive(true);
        const char* gainLabels[] = {"Linear", "Log", "Gamma", "Trained"};
        const char* gainTips[] = {
            "Gaussian speech model optimized for linear amplitude accuracy.",
            "Gaussian speech model optimized for logarithmic amplitude accuracy.",
            "Gamma speech model with soft speech-presence weighting.",
            "Experimental piecewise suppression curve for comparison."
        };
        for (int i = 0; i < 4; ++i) {
            auto* b = makeOptionRadio(gainLabels[i]);
            // objectName unchanged from when these were toggle buttons: the
            // automation bridge addresses them by it (#3646).
            b->setObjectName(
                QStringLiteral("nr2GainMethod%1Button").arg(i));
            b->setAccessibleName(
                QStringLiteral("NR2 gain method %1").arg(gainLabels[i]));
            b->setToolTip(gainTips[i]);
            b->setAccessibleDescription(QString::fromLatin1(gainTips[i]));
            m_nr2GainGroup->addButton(b, i);
            row->addWidget(b, i / 2, i % 2);
        }
        row->setColumnStretch(2, 1);
        m_nr2GainGroup->button(2)->setChecked(true);  // Gamma default
        connect(m_nr2GainGroup, &QButtonGroup::idClicked, this, [this](int id) {
            Nr2SettingsModel::instance().setGainMethod(id);
            updateNr2ControlAvailability();
            emit nr2GainMethodChanged(id);
            refreshStatusStrip();
        });
        cellBox->addLayout(row);
        methodRow->addWidget(gainCell);
    }

    // NPE Method — exclusive toggle row.
    {
        auto* npeCell = controlCell(methodFrame, /*last=*/true);
        auto* cellBox = new QVBoxLayout(npeCell);
        cellBox->setContentsMargins(0, 0, 0, 0);
        cellBox->setSpacing(8);
        cellBox->addWidget(
            sectionLabel(QStringLiteral("NOISE ESTIMATION"), npeCell));

        // Two columns, like GAIN METHOD beside it and for the same reason: a
        // QRadioButton clips its text, and three on one line lost the "T" off
        // NSTAT as soon as the window came down to its opening size.
        auto* row = new QGridLayout;
        row->setContentsMargins(0, 0, 0, 0);
        row->setHorizontalSpacing(14);
        row->setVerticalSpacing(6);
        m_nr2NpeGroup = new QButtonGroup(this);
        m_nr2NpeGroup->setExclusive(true);
        const char* npeLabels[] = {"OSMS", "MMSE", "NSTAT"};
        const char* npeTips[] = {
            "Optimal Smoothing Minimum Statistics — tracks noise floor using a running minimum estimate.",
            "Minimum Mean Squared Error — minimizes the expected noise estimation error.",
            "Non-stationary estimator designed for noise that changes over time."
        };
        for (int i = 0; i < 3; ++i) {
            auto* b = makeOptionRadio(npeLabels[i]);
            b->setObjectName(
                QStringLiteral("nr2NpeMethod%1Button").arg(i));
            b->setAccessibleName(
                QStringLiteral("NR2 noise estimation %1").arg(npeLabels[i]));
            b->setToolTip(npeTips[i]);
            b->setAccessibleDescription(QString::fromLatin1(npeTips[i]));
            m_nr2NpeGroup->addButton(b, i);
            row->addWidget(b, i / 2, i % 2);
        }
        row->setColumnStretch(2, 1);
        m_nr2NpeGroup->button(0)->setChecked(true);  // OSMS default
        connect(m_nr2NpeGroup, &QButtonGroup::idClicked, this, [this](int id) {
            Nr2SettingsModel::instance().setNpeMethod(id);
            emit nr2NpeMethodChanged(id);
            refreshStatusStrip();
        });
        cellBox->addLayout(row);
        methodRow->addWidget(npeCell);
        methodRow->addStretch(1);
    }

    // AE Filter checkbox + Reset Defaults icon on the same row.
    m_nr2AeCheck = new QCheckBox("AE Filter (artifact elimination)");
    m_nr2AeCheck->setObjectName(QStringLiteral("nr2AeFilterCheck"));
    m_nr2AeCheck->setAccessibleName(QStringLiteral("NR2 AE Filter"));
    m_nr2AeCheck->setToolTip("Reduces ringing and musical artifacts typical of frequency-domain noise reduction.");
    m_nr2AeCheck->setChecked(true);
    connect(m_nr2AeCheck, &QCheckBox::toggled, this, [this](bool on) {
        Nr2SettingsModel::instance().setAeFilter(on);
        emit nr2AeFilterChanged(on);
        refreshStatusStrip();
    });
    {
        auto* resetBtn = makeResetIconButton();
        connect(resetBtn, &QPushButton::clicked,
                this, &AetherDspWidget::resetCurrentTab);
        methodRow->addWidget(resetBtn, 0, Qt::AlignVCenter);
    }

    // ── Noise fill (WDSP's post2 psychoacoustic stage, #5702) ─────────────
    // Spectral NR leaves the gaps between syllables completely silent, which
    // operators hear as the receiver going dead. This mixes a controlled
    // amount of noise back in -- partly the genuine residual just removed,
    // partly synthetic -- over a tapered low band. Off by default, as WDSP
    // ships it, so nothing changes for an existing install until it is asked
    // for.
    m_nr2Post2Check = new QCheckBox("Noise fill (psychoacoustic)");
    m_nr2Post2Check->setObjectName(QStringLiteral("nr2Post2RunCheck"));
    m_nr2Post2Check->setAccessibleName(QStringLiteral("NR2 noise fill"));
    m_nr2Post2Check->setToolTip(
        "Mixes noise back into the gaps so the receiver does not sound dead\n"
        "between syllables, and can let very weak signals through.\n"
        "Also band-limits the output to the fill band.");
    m_nr2Post2Check->setAccessibleDescription(m_nr2Post2Check->toolTip());
    m_nr2Post2Check->setChecked(Nr2SettingsModel::instance().config().post2Run);
    connect(m_nr2Post2Check, &QCheckBox::toggled, this, [this](bool on) {
        Nr2SettingsModel::instance().setPost2Run(on);
        if (m_nr2Post2NlevelSlider) m_nr2Post2NlevelSlider->setEnabled(on);
        if (m_nr2Post2FactorSlider) m_nr2Post2FactorSlider->setEnabled(on);
        if (m_nr2Post2TaperSlider)  m_nr2Post2TaperSlider->setEnabled(on);
        emit nr2Post2RunChanged(on);
        emit nr2Post2SettingsChanged();
        refreshStatusStrip();
    });
    {
        auto* postFrame = controlsFrame(page);
        auto* postBox = new QVBoxLayout(postFrame);
        postBox->setContentsMargins(12, 10, 12, 10);
        postBox->setSpacing(8);
        postBox->addWidget(
            sectionLabel(QStringLiteral("POST PROCESSING"), postFrame));
        auto* row = new QHBoxLayout;
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(20);
        row->addWidget(m_nr2AeCheck);
        row->addWidget(m_nr2Post2Check);
        row->addStretch(1);
        postBox->addLayout(row);
        vbox->addWidget(postFrame);
    }

    // Sliders: GainMax, GainSmooth, Q_SPP
    auto* maskFrame = controlsFrame(page);
    auto* maskBox = new QVBoxLayout(maskFrame);
    maskBox->setContentsMargins(12, 10, 12, 10);
    maskBox->setSpacing(8);
    maskBox->addWidget(sectionLabel(QStringLiteral("MASK"), maskFrame));
    auto* sliderGrid = new QGridLayout;
    sliderGrid->setContentsMargins(0, 0, 0, 0);
    sliderGrid->setHorizontalSpacing(12);
    sliderGrid->setVerticalSpacing(6);
    sliderGrid->setColumnStretch(1, 1);
    int row = 0;

    // Gain Max (reduction depth)
    {
        auto* lbl = new QLabel("Reduction:");
        styled(lbl, labelStyle);
        sliderGrid->addWidget(lbl, row, 0);
        m_nr2GainMaxSlider = new GuardedSlider(Qt::Horizontal);
        m_nr2GainMaxSlider->setObjectName(
            QStringLiteral("nr2GainMaxSlider"));
        m_nr2GainMaxSlider->setAccessibleName(
            QStringLiteral("NR2 Reduction"));
        m_nr2GainMaxSlider->setRange(50, 200);
        m_nr2GainMaxSlider->setValue(100);
        static_cast<GuardedSlider*>(m_nr2GainMaxSlider)
            ->setDragValueFormatter([](int value) {
                return QString::number(value / 100.0f, 'f', 2);
            });
        applyChromeSliderStyle(m_nr2GainMaxSlider);
        m_nr2GainMaxSlider->setToolTip(
            "Maximum spectral gain. Lower values force deeper reduction; "
            "higher values retain more of the input level.");
        sliderGrid->addWidget(m_nr2GainMaxSlider, row, 1);
        m_nr2GainMaxLabel = new QLabel("1.00");
        styled(m_nr2GainMaxLabel, valStyle);
        m_nr2GainMaxLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        sliderGrid->addWidget(m_nr2GainMaxLabel, row, 2);
        connect(m_nr2GainMaxSlider, &QSlider::valueChanged, this, [this](int v) {
            float val = v / 100.0f;
            m_nr2GainMaxLabel->setText(QString::number(val, 'f', 2));
            Nr2SettingsModel::instance().setGainMax(val);
            emit nr2GainMaxChanged(val);
        });
        ++row;
    }

    // Gain floor (naturalness / musical-noise tradeoff)
    {
        auto* lbl = new QLabel("Naturalness:");
        styled(lbl, labelStyle);
        sliderGrid->addWidget(lbl, row, 0);
        m_nr2GainFloorSlider = new GuardedSlider(Qt::Horizontal);
        m_nr2GainFloorSlider->setObjectName(
            QStringLiteral("nr2GainFloorSlider"));
        m_nr2GainFloorSlider->setAccessibleName(
            QStringLiteral("NR2 Naturalness"));
        m_nr2GainFloorSlider->setAccessibleDescription(
            QStringLiteral("Minimum spectral gain from 0.00 to 0.15"));
        m_nr2GainFloorSlider->setRange(0, 15);
        m_nr2GainFloorSlider->setSingleStep(1);
        m_nr2GainFloorSlider->setPageStep(5);
        m_nr2GainFloorSlider->setValue(0);
        static_cast<GuardedSlider*>(m_nr2GainFloorSlider)
            ->setDragValueFormatter([](int value) {
                return QString::number(value / 100.0f, 'f', 2);
            });
        applyChromeSliderStyle(m_nr2GainFloorSlider);
        m_nr2GainFloorSlider->setToolTip(
            "Minimum spectral gain. 0.00 permits the gain mask's full "
            "suppression; higher values retain more broadband sound to reduce "
            "metallic or musical artifacts.");
        sliderGrid->addWidget(m_nr2GainFloorSlider, row, 1);
        m_nr2GainFloorLabel = new QLabel("0.00");
        styled(m_nr2GainFloorLabel, valStyle);
        m_nr2GainFloorLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        sliderGrid->addWidget(m_nr2GainFloorLabel, row, 2);
        connect(m_nr2GainFloorSlider, &QSlider::valueChanged,
                this, [this](int value) {
            const float gainFloor = value / 100.0f;
            m_nr2GainFloorLabel->setText(
                QString::number(gainFloor, 'f', 2));
            Nr2SettingsModel::instance().setGainFloor(gainFloor);
            emit nr2GainFloorChanged(gainFloor);
        });
        ++row;
    }

    // Gain Smooth
    {
        auto* lbl = new QLabel("Smoothing:");
        styled(lbl, labelStyle);
        sliderGrid->addWidget(lbl, row, 0);
        m_nr2SmoothSlider = new GuardedSlider(Qt::Horizontal);
        m_nr2SmoothSlider->setObjectName(
            QStringLiteral("nr2GainSmoothSlider"));
        m_nr2SmoothSlider->setAccessibleName(
            QStringLiteral("NR2 Smoothing"));
        m_nr2SmoothSlider->setRange(50, 98);
        m_nr2SmoothSlider->setValue(85);
        static_cast<GuardedSlider*>(m_nr2SmoothSlider)
            ->setDragValueFormatter([](int value) {
                return QString::number(value / 100.0f, 'f', 2);
            });
        applyChromeSliderStyle(m_nr2SmoothSlider);
        m_nr2SmoothSlider->setToolTip(
            "Temporal smoothing of the spectral gain mask. Higher values "
            "change more slowly and can reduce musical artifacts.");
        sliderGrid->addWidget(m_nr2SmoothSlider, row, 1);
        m_nr2SmoothLabel = new QLabel("0.85");
        styled(m_nr2SmoothLabel, valStyle);
        m_nr2SmoothLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        sliderGrid->addWidget(m_nr2SmoothLabel, row, 2);
        connect(m_nr2SmoothSlider, &QSlider::valueChanged, this, [this](int v) {
            float val = v / 100.0f;
            m_nr2SmoothLabel->setText(QString::number(val, 'f', 2));
            Nr2SettingsModel::instance().setGainSmooth(val);
            emit nr2GainSmoothChanged(val);
        });
        ++row;
    }

    // Q_SPP (voice threshold)
    {
        m_nr2QsppTitleLabel = new QLabel("Threshold:");
        styled(m_nr2QsppTitleLabel, labelStyle);
        sliderGrid->addWidget(m_nr2QsppTitleLabel, row, 0);
        m_nr2QsppSlider = new GuardedSlider(Qt::Horizontal);
        m_nr2QsppSlider->setObjectName(
            QStringLiteral("nr2QsppSlider"));
        m_nr2QsppSlider->setAccessibleName(
            QStringLiteral("NR2 Voice Threshold"));
        m_nr2QsppSlider->setRange(5, 50);
        m_nr2QsppSlider->setValue(20);
        static_cast<GuardedSlider*>(m_nr2QsppSlider)
            ->setDragValueFormatter([](int value) {
                return QString::number(value / 100.0f, 'f', 2);
            });
        applyChromeSliderStyle(m_nr2QsppSlider);
        m_nr2QsppSlider->setToolTip(
            "Speech-presence threshold used by the Linear and Gamma gain "
            "methods. Lower values preserve quiet speech but may pass more "
            "noise.");
        sliderGrid->addWidget(m_nr2QsppSlider, row, 1);
        m_nr2QsppLabel = new QLabel("0.20");
        styled(m_nr2QsppLabel, valStyle);
        m_nr2QsppLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        sliderGrid->addWidget(m_nr2QsppLabel, row, 2);
        connect(m_nr2QsppSlider, &QSlider::valueChanged, this, [this](int v) {
            float val = v / 100.0f;
            m_nr2QsppLabel->setText(QString::number(val, 'f', 2));
            Nr2SettingsModel::instance().setQspp(val);
            emit nr2QsppChanged(val);
        });
        ++row;
    }

    // The two noise-fill controls, dimmed until the stage is switched on.
    {
        const Nr2SettingsModel::Config cfg = Nr2SettingsModel::instance().config();

        m_nr2Post2NlevelSlider = new GuardedSlider(Qt::Horizontal);
        m_nr2Post2NlevelSlider->setObjectName(QStringLiteral("nr2Post2NlevelSlider"));
        m_nr2Post2NlevelSlider->setAccessibleName(tr("NR2 noise fill level"));
        m_nr2Post2NlevelSlider->setAccessibleDescription(
            tr("How much noise is mixed back into the gaps."));
        m_nr2Post2NlevelSlider->setRange(0, 100);
        m_nr2Post2NlevelSlider->setValue(
            static_cast<int>(std::lround(cfg.post2Nlevel * 100.0f)));
        m_nr2Post2NlevelSlider->setEnabled(cfg.post2Run);
        m_nr2Post2NlevelSlider->setToolTip("How much noise is mixed back in. 0 injects nothing.");
        applyChromeSliderStyle(m_nr2Post2NlevelSlider);
        auto* nlevelTitle = new QLabel("Fill level:");
        styled(nlevelTitle, labelStyle);
        sliderGrid->addWidget(nlevelTitle, row, 0);
        sliderGrid->addWidget(m_nr2Post2NlevelSlider, row, 1);
        m_nr2Post2NlevelLabel = new QLabel(QString::number(cfg.post2Nlevel, 'f', 2));
        styled(m_nr2Post2NlevelLabel, valStyle);
        m_nr2Post2NlevelLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        sliderGrid->addWidget(m_nr2Post2NlevelLabel, row, 2);
        connect(m_nr2Post2NlevelSlider, &QSlider::valueChanged, this, [this](int v) {
            const float val = v / 100.0f;
            m_nr2Post2NlevelLabel->setText(QString::number(val, 'f', 2));
            Nr2SettingsModel::instance().setPost2Nlevel(val);
            emit nr2Post2SettingsChanged();
        });
        ++row;

        // 0 mixes back the noise this reduction actually removed; 1 replaces
        // it with synthetic white. The blend is what makes the fill sound like
        // the band rather than like a hiss generator.
        m_nr2Post2FactorSlider = new GuardedSlider(Qt::Horizontal);
        m_nr2Post2FactorSlider->setObjectName(QStringLiteral("nr2Post2FactorSlider"));
        m_nr2Post2FactorSlider->setAccessibleName(tr("NR2 noise fill character"));
        m_nr2Post2FactorSlider->setAccessibleDescription(
            tr("Blend between the removed noise and synthetic white noise."));
        m_nr2Post2FactorSlider->setRange(0, 100);
        m_nr2Post2FactorSlider->setValue(
            static_cast<int>(std::lround(cfg.post2Factor * 100.0f)));
        m_nr2Post2FactorSlider->setEnabled(cfg.post2Run);
        m_nr2Post2FactorSlider->setToolTip(
            "0 = the noise actually removed from this signal\n"
            "1 = synthetic white noise");
        applyChromeSliderStyle(m_nr2Post2FactorSlider);
        auto* factorTitle = new QLabel("Fill character:");
        styled(factorTitle, labelStyle);
        sliderGrid->addWidget(factorTitle, row, 0);
        sliderGrid->addWidget(m_nr2Post2FactorSlider, row, 1);
        m_nr2Post2FactorLabel = new QLabel(QString::number(cfg.post2Factor, 'f', 2));
        styled(m_nr2Post2FactorLabel, valStyle);
        m_nr2Post2FactorLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        sliderGrid->addWidget(m_nr2Post2FactorLabel, row, 2);
        connect(m_nr2Post2FactorSlider, &QSlider::valueChanged, this, [this](int v) {
            const float val = v / 100.0f;
            m_nr2Post2FactorLabel->setText(QString::number(val, 'f', 2));
            Nr2SettingsModel::instance().setPost2Factor(val);
            emit nr2Post2SettingsChanged();
        });
        ++row;

        // The band limit, which is NOT cosmetic: the stage zeroes every bin
        // above it, so enabling noise fill lowpasses the audio here. Left at
        // the default an AM, FM or ESSB listener would lose their highs with
        // no control to explain it.
        m_nr2Post2TaperSlider = new GuardedSlider(Qt::Horizontal);
        m_nr2Post2TaperSlider->setObjectName(QStringLiteral("nr2Post2TaperSlider"));
        m_nr2Post2TaperSlider->setAccessibleName(tr("NR2 noise fill bandwidth"));
        m_nr2Post2TaperSlider->setAccessibleDescription(
            tr("Highest frequency the noise fill covers. Audio above it is removed."));
        m_nr2Post2TaperSlider->setRange(300, 6000);
        m_nr2Post2TaperSlider->setValue(
            static_cast<int>(std::lround(cfg.post2TaperHz)));
        m_nr2Post2TaperSlider->setEnabled(cfg.post2Run);
        m_nr2Post2TaperSlider->setToolTip(
            "Highest frequency the fill covers.\n"
            "AUDIO ABOVE THIS IS REMOVED, so raise it for AM, FM or wide SSB.\n"
            "2871 Hz matches WDSP's own default band.");
        applyChromeSliderStyle(m_nr2Post2TaperSlider);
        auto* taperTitle = styled(new QLabel("Fill bandwidth:"), labelStyle);
        sliderGrid->addWidget(taperTitle, row, 0);
        sliderGrid->addWidget(m_nr2Post2TaperSlider, row, 1);
        m_nr2Post2TaperLabel = styled(
            new QLabel(QString::number(static_cast<int>(cfg.post2TaperHz))), valStyle);
        m_nr2Post2TaperLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        sliderGrid->addWidget(m_nr2Post2TaperLabel, row, 2);
        connect(m_nr2Post2TaperSlider, &QSlider::valueChanged, this, [this](int v) {
            m_nr2Post2TaperLabel->setText(QString::number(v));
            Nr2SettingsModel::instance().setPost2TaperHz(static_cast<float>(v));
            emit nr2Post2SettingsChanged();
        });
        ++row;
    }

    maskBox->addLayout(sliderGrid);
    vbox->addWidget(maskFrame);
    vbox->addStretch();
    updateNr2ControlAvailability();
    return page;
}

void AetherDspWidget::updateNr2ControlAvailability()
{
    if (!m_nr2GainGroup || !m_nr2QsppSlider || !m_nr2QsppLabel) {
        return;
    }

    const int gainMethod = m_nr2GainGroup->checkedId();
    const bool thresholdAvailable = gainMethod == 0 || gainMethod == 2;
    const QString tooltip = thresholdAvailable
        ? QStringLiteral(
            "Speech-presence threshold used by this gain method. Lower "
            "values preserve quiet speech but may pass more noise.")
        : QStringLiteral(
            "Voice Threshold does not affect the selected gain method.");

    // The tooltip is the whole explanation of why this row is unavailable
    // under the current gain method, so it belongs on the accessible channel
    // too — otherwise a screen-reader user hears "dimmed" and no reason
    // (#4896). Set unconditionally: the reason is equally true either way.
    if (m_nr2QsppTitleLabel) {
        m_nr2QsppTitleLabel->setEnabled(thresholdAvailable);
        m_nr2QsppTitleLabel->setToolTip(tooltip);
        m_nr2QsppTitleLabel->setAccessibleDescription(tooltip);
    }
    m_nr2QsppSlider->setEnabled(thresholdAvailable);
    m_nr2QsppSlider->setToolTip(tooltip);
    m_nr2QsppSlider->setAccessibleDescription(tooltip);
    m_nr2QsppLabel->setEnabled(thresholdAvailable);
    m_nr2QsppLabel->setToolTip(tooltip);
    m_nr2QsppLabel->setAccessibleDescription(tooltip);
}

// ── NR4 Tab (libspecbleach) ──────────────────────────────────────────────────

QWidget* AetherDspWidget::buildNr4Page()
{
    auto* page = new QWidget;
    auto* vbox = new QVBoxLayout(page);
    vbox->setContentsMargins(0, 0, 0, 0);
    vbox->setSpacing(6);

    auto labelStyle = QStringLiteral("QLabel { color: #8090a0; font-size: 11px; }");
    auto valStyle   = QStringLiteral("QLabel { color: #c8d8e8; font-size: 11px; min-width: 40px; }");

    auto* methodFrame = controlsFrame(page);
    auto* methodRow = new QHBoxLayout(methodFrame);
    methodRow->setContentsMargins(12, 10, 12, 10);
    methodRow->setSpacing(16);
    vbox->addWidget(methodFrame);

    // Noise Estimation Method — exclusive radio row.
    {
        auto* cell = controlCell(methodFrame, /*last=*/true);
        auto* cellBox = new QVBoxLayout(cell);
        cellBox->setContentsMargins(0, 0, 0, 0);
        cellBox->setSpacing(8);
        cellBox->addWidget(
            sectionLabel(QStringLiteral("NOISE ESTIMATION"), cell));

        auto* row = new QHBoxLayout;
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(14);
        m_nr4MethodGroup = new QButtonGroup(this);
        m_nr4MethodGroup->setExclusive(true);
        const char* methodLabels[] = {"MMSE", "Brandt", "Martin"};
        const char* methodTips[] = {
            "MMSE with Speech Presence Probability — balances noise estimation with speech preservation.",
            "Recursive smoothing using critical frequency bands — good for non-stationary noise.",
            "Minimum statistics using running spectral minima — robust for slowly varying noise floors."
        };
        for (int i = 0; i < 3; ++i) {
            auto* b = makeOptionRadio(methodLabels[i]);
            b->setObjectName(
                QStringLiteral("nr4NoiseMethod%1Button").arg(i));
            b->setAccessibleName(
                QStringLiteral("NR4 noise estimation %1").arg(methodLabels[i]));
            b->setToolTip(methodTips[i]);
            b->setAccessibleDescription(QString::fromLatin1(methodTips[i]));
            m_nr4MethodGroup->addButton(b, i);
            row->addWidget(b);
        }
        row->addStretch(1);
        m_nr4MethodGroup->button(0)->setChecked(true);
        connect(m_nr4MethodGroup, &QButtonGroup::idClicked, this, [this](int id) {
            auto& s = AppSettings::instance();
            s.setValue("NR4NoiseEstimationMethod", QString::number(id));
            s.save();
            emit nr4NoiseMethodChanged(id);
        });
        cellBox->addLayout(row);
        methodRow->addWidget(cell);
        methodRow->addStretch(1);
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
        auto* resetBtn = makeResetIconButton();
        connect(resetBtn, &QPushButton::clicked,
                this, &AetherDspWidget::resetCurrentTab);
        methodRow->addWidget(resetBtn, 0, Qt::AlignVCenter);
    }

    // Sliders
    auto* paramFrame = controlsFrame(page);
    auto* paramBox = new QVBoxLayout(paramFrame);
    paramBox->setContentsMargins(12, 10, 12, 10);
    paramBox->setSpacing(8);
    paramBox->addWidget(sectionLabel(QStringLiteral("SPECTRAL"), paramFrame));
    paramBox->addWidget(m_nr4AdaptiveCheck);
    auto* sliderGrid = new QGridLayout;
    sliderGrid->setContentsMargins(0, 0, 0, 0);
    sliderGrid->setHorizontalSpacing(12);
    sliderGrid->setVerticalSpacing(6);
    sliderGrid->setColumnStretch(1, 1);
    int row = 0;

    {
        auto* lbl = new QLabel("Reduction (dB):");
        lbl->setStyleSheet(labelStyle);
        sliderGrid->addWidget(lbl, row, 0);
        m_nr4ReductionSlider = new GuardedSlider(Qt::Horizontal);
        m_nr4ReductionSlider->setRange(0, 400);
        m_nr4ReductionSlider->setValue(100);
        applyChromeSliderStyle(m_nr4ReductionSlider);
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
        applyChromeSliderStyle(m_nr4SmoothingSlider);
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
        applyChromeSliderStyle(m_nr4WhiteningSlider);
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
        applyChromeSliderStyle(m_nr4MaskingSlider);
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
        applyChromeSliderStyle(m_nr4SuppressionSlider);
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

    paramBox->addLayout(sliderGrid);
    vbox->addWidget(paramFrame);
    vbox->addStretch();
    return page;
}

// ── MNR Tab ──────────────────────────────────────────────────────────────────

QWidget* AetherDspWidget::buildMnrPage()
{
    auto* page = new QWidget;
    auto* vbox = new QVBoxLayout(page);
    vbox->setContentsMargins(0, 0, 0, 0);
    vbox->setSpacing(6);
    auto* frame = controlsFrame(page);
    vbox->addWidget(frame);
    auto* body = new QVBoxLayout(frame);
    body->setContentsMargins(12, 10, 12, 10);
    body->setSpacing(8);
    auto* headerRow = new QHBoxLayout;
    headerRow->setContentsMargins(0, 0, 0, 0);
    headerRow->addWidget(sectionLabel(QStringLiteral("MMSE-WIENER"), frame));
    headerRow->addStretch(1);
    body->addLayout(headerRow);

    auto labelStyle = QStringLiteral("QLabel { color: #8090a0; font-size: 11px; }");
    auto valStyle   = QStringLiteral("QLabel { color: #c8d8e8; font-size: 11px; min-width: 40px; }");

    {
        auto* hdrRow = new QHBoxLayout;
        hdrRow->setContentsMargins(0, 0, 0, 0);
        hdrRow->addStretch(1);
        auto* resetBtn = makeResetIconButton();
        connect(resetBtn, &QPushButton::clicked,
                this, &AetherDspWidget::resetCurrentTab);
        hdrRow->addWidget(resetBtn);
        body->addLayout(hdrRow);
    }
    {
        auto* row = new QHBoxLayout;
        auto* lbl = new QLabel("Strength");
        lbl->setStyleSheet(labelStyle);
        row->addWidget(lbl);

        m_mnrStrengthSlider = new GuardedSlider(Qt::Horizontal);
        m_mnrStrengthSlider->setObjectName(QStringLiteral("mnrStrengthSlider"));
        m_mnrStrengthSlider->setAccessibleName(QStringLiteral("MNR Strength"));
        m_mnrStrengthSlider->setAccessibleDescription(
            QStringLiteral("Noise-reduction synthesis strength from 0 to 100 percent"));
        m_mnrStrengthSlider->setRange(0, 100);
        m_mnrStrengthSlider->setValue(100);
        applyChromeSliderStyle(m_mnrStrengthSlider);
        m_mnrStrengthSlider->setToolTip("Adjust noise reduction aggressiveness (0 = bypass, 100 = maximum)");
        row->addWidget(m_mnrStrengthSlider, 1);

        m_mnrStrengthLabel = new QLabel("100%");
        m_mnrStrengthLabel->setStyleSheet(valStyle);
        row->addWidget(m_mnrStrengthLabel);
        body->addLayout(row);

        connect(m_mnrStrengthSlider, &QSlider::valueChanged, this, [this](int value) {
            float normalized = value / 100.0f;
            m_mnrStrengthLabel->setText(QString::number(value) + "%");
            auto& s = AppSettings::instance();
            s.setValue("MnrStrength", QString::number(normalized, 'f', 2));
            s.save();
            emit mnrStrengthChanged(normalized);
        });
    }

    auto* info = new QLabel("Smoothed minimum-statistics tracking learns steady background noise,\n"
                            "then applies a shared Wiener mask that preserves stereo balance.");
    info->setWordWrap(true);
    AetherSDR::ThemeManager::instance().applyStyleSheet(info, "QLabel { color: {{color.text.secondary}}; font-size: 11px; }");
    body->addSpacing(8);
    body->addWidget(info);

    vbox->addStretch();
    return page;
}

// ── RN2 Tab ─────────────────────────────────────────────────────────────────

QWidget* AetherDspWidget::buildRn2Page()
{
    auto* page = new QWidget;
    auto* vbox = new QVBoxLayout(page);
    vbox->setContentsMargins(0, 0, 0, 0);
    vbox->setSpacing(6);
    auto* frame = controlsFrame(page);
    vbox->addWidget(frame);
    auto* body = new QVBoxLayout(frame);
    body->setContentsMargins(12, 10, 12, 10);
    body->setSpacing(8);
    auto* headerRow = new QHBoxLayout;
    headerRow->setContentsMargins(0, 0, 0, 0);
    headerRow->addWidget(sectionLabel(QStringLiteral("RNNOISE"), frame));
    headerRow->addStretch(1);
    body->addLayout(headerRow);
    auto* lbl = new QLabel(
        "RNNoise — open-source recurrent neural-network voice denoiser. "
        "Removes stationary background noise (fans, hum, white-noise floor) "
        "while preserving speech.  Lightweight and CPU-only.");
    lbl->setWordWrap(true);
    lbl->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    AetherSDR::ThemeManager::instance().applyStyleSheet(lbl, "QLabel { color: {{color.text.secondary}}; font-size: 12px; }");
    {
        auto* infoRow = new QHBoxLayout;
        infoRow->setContentsMargins(0, 0, 10, 0);
        infoRow->addWidget(lbl);
        body->addLayout(infoRow);
    }

    {
        auto* rn2ResetBtn = makeResetIconButton();
        connect(rn2ResetBtn, &QPushButton::clicked,
                this, &AetherDspWidget::resetCurrentTab);
        headerRow->addWidget(rn2ResetBtn);
    }

    auto* grid = new QGridLayout;
    grid->setColumnStretch(1, 1);

    // Dry mix. RNNoise gates hard between phrases, which some operators hear
    // as the receiver going dead rather than quiet. Retaining a slice of the
    // original spectrum leaves a constant floor under the speech. Default 0 is
    // RN2's behavior since it shipped, so nothing changes until it is asked for.
    auto* dryTitle = new QLabel("Noise Floor");
    grid->addWidget(dryTitle, 0, 0);
    m_rn2DryMixSlider = new QSlider(Qt::Horizontal);
    m_rn2DryMixSlider->setObjectName(QStringLiteral("rn2DryMixSlider"));
    m_rn2DryMixSlider->setAccessibleName(tr("RN2 noise floor"));
    m_rn2DryMixSlider->setAccessibleDescription(
        tr("Percentage of the original signal RN2 leaves under the denoised "
           "audio. Zero is full noise suppression."));
    m_rn2DryMixSlider->setRange(
        0, static_cast<int>(Rn2SettingsModel::kMaxRxDryMix * 100.0f));
    m_rn2DryMixSlider->setValue(static_cast<int>(
        Rn2SettingsModel::instance().config().rxDryMix * 100.0f + 0.5f));
    applyChromeSliderStyle(m_rn2DryMixSlider);
    m_rn2DryMixSlider->setToolTip(
        "How much of the original signal RN2 leaves under the denoised audio.\n"
        "0% = full suppression (default) — silent between phrases\n"
        "10–20% = a steady, quiet noise floor so the receiver still sounds live\n\n"
        "Affects received audio only; the transmit denoiser is unchanged.");
    grid->addWidget(m_rn2DryMixSlider, 0, 1);
    m_rn2DryMixLabel = new QLabel(
        QString::number(m_rn2DryMixSlider->value()) + QStringLiteral("%"));
    m_rn2DryMixLabel->setFixedWidth(40);
    grid->addWidget(m_rn2DryMixLabel, 0, 2);

    connect(m_rn2DryMixSlider, &QSlider::valueChanged, this, [this](int v) {
        m_rn2DryMixLabel->setText(QString::number(v) + QStringLiteral("%"));
        const float mix = static_cast<float>(v) / 100.0f;
        Rn2SettingsModel::instance().setRxDryMix(mix);
        emit rn2DryMixChanged(mix);
    });

    body->addLayout(grid);
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
#ifdef HAVE_NVIDIA_AFX
        // If no afx-bits pack is published for this GPU's arch (e.g. sm_120 /
        // RTX 50-series), don't offer a Download that would 404 — disable it and
        // say why, steering the user to DFNR. (#3933)
        if (!NvidiaAfxPack::hasSupportedGpu()) {
            m_bnrAfxDownloadBtn->setEnabled(false);
            if (NvidiaAfxPack::isAfxCapableGpu()) {
                m_bnrAfxDownloadBtn->setText(tr("No pack for this GPU"));
                m_bnrAfxDownloadBtn->setToolTip(
                    QStringLiteral("No BNR pack for your GPU (%1) yet — use DFNR.")
                        .arg(NvidiaAfxPack::detectArch()));
            }
        }
#endif
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

    const QPointer<AetherDspWidget> self(this);
    ScopedChildWidget<QMessageBox> boxOwner(this);
    QMessageBox& box = *boxOwner.get();
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
    if (!self || !boxOwner) {
        return false;
    }
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
    vbox->setContentsMargins(0, 0, 0, 0);
    vbox->setSpacing(6);
    auto* frame = controlsFrame(page);
    vbox->addWidget(frame);
    auto* body = new QVBoxLayout(frame);
    body->setContentsMargins(12, 10, 12, 10);
    body->setSpacing(8);
    auto* headerRow = new QHBoxLayout;
    headerRow->setContentsMargins(0, 0, 0, 0);
    headerRow->addWidget(sectionLabel(QStringLiteral("NVIDIA AFX"), frame));
    headerRow->addStretch(1);
    body->addLayout(headerRow);

    auto* info = new QLabel("GPU-accelerated AI noise removal (NVIDIA Maxine) — "
                            "runs in-process on a local NVIDIA GPU.");
    info->setWordWrap(true);
    AetherSDR::ThemeManager::instance().applyStyleSheet(info, "QLabel { color: {{color.text.secondary}}; font-size: 12px; }");
    body->addWidget(info);

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
    applyChromeSliderStyle(m_bnrAfxIntensitySlider);
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
    body->addLayout(g);

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
    {
        auto* listFrame = controlsFrame(page);
        auto* listBox = new QVBoxLayout(listFrame);
        listBox->setContentsMargins(12, 10, 12, 10);
        listBox->setSpacing(8);
        listBox->addWidget(
            sectionLabel(QStringLiteral("INSTALLED COMPONENTS"), listFrame));
        m_bnrAfxList->setParent(listFrame);
        listBox->addWidget(m_bnrAfxList);
        vbox->addWidget(listFrame);
    }

    vbox->addStretch();

    // Download / Resume / Update button, pinned to the bottom-left.
    auto* dlRow = new QHBoxLayout;
    dlRow->setContentsMargins(0, 8, 0, 0);
    dlRow->addWidget(m_bnrAfxDownloadBtn);
    dlRow->addStretch(1);
    body->addLayout(dlRow);

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

namespace {

// QSlider with one visible tick at a fixed position: where WDSP's own default
// for that control sits. Stock QSlider ticks are drawn at a repeating
// interval, which is the wrong shape for "home is here" — this is one mark,
// at one place, in the accent colour.
class MarkedSlider : public QSlider {
public:
    MarkedSlider(double markerFraction, QWidget* parent = nullptr)
        : QSlider(Qt::Horizontal, parent)
        , m_fraction(std::clamp(markerFraction, 0.0, 1.0))
    {
    }

protected:
    void paintEvent(QPaintEvent* e) override
    {
        QSlider::paintEvent(e);

        QStyleOptionSlider opt;
        initStyleOption(&opt);
        const QRect groove =
            style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderGroove, this);
        const QRect handle =
            style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderHandle, this);

        // Span the handle travels, so the mark lands under the handle when the
        // control is at its default rather than a few pixels off at the ends.
        const int span = groove.width() - handle.width();
        if (span <= 0) {
            return;
        }
        const int x = groove.left() + handle.width() / 2
                    + static_cast<int>(std::lround(m_fraction * span));

        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, false);
        // Amber, the chrome's second accent: the mark has to stay legible
        // where it sits on top of the green fill, and a green-on-green tick
        // disappeared exactly where it matters — at a control left on its
        // default.
        QColor c = ModemChrome::colour(ModemChrome::Colour::Amber);
        c.setAlpha(230);
        p.setPen(QPen(c, 2));
        // Above the groove rather than below it: the handle is 14 px here and
        // covered a tick drawn underneath, which is where a default-valued
        // control always puts it.
        const int top = groove.top() - 5;
        p.drawLine(x, top, x, top + 4);
    }

private:
    double m_fraction;
};

}  // namespace

QWidget* AetherDspWidget::buildNnrPage()
{
    auto* page = new QWidget;
    auto* vbox = new QVBoxLayout(page);
    vbox->setContentsMargins(0, 0, 0, 0);
    vbox->setSpacing(6);
    auto* frame = controlsFrame(page);
    vbox->addWidget(frame);
    auto* body = new QVBoxLayout(frame);
    body->setContentsMargins(12, 10, 12, 10);
    body->setSpacing(8);
    auto* headerRow = new QHBoxLayout;
    headerRow->setContentsMargins(0, 0, 0, 0);
    headerRow->addWidget(sectionLabel(QStringLiteral("NEURAL NR"), frame));
    headerRow->addStretch(1);
    body->addLayout(headerRow);

    auto* info = new QLabel(
        "Neural noise reduction trained on off-air HF: over a hundred noise "
        "recordings from real receivers, with speech put through an SSB "
        "transmit chain before mixing. Voice modes only — it treats a steady "
        "carrier as noise and removes it.\n\n"
        "Runs after the AGC, so set the AGC threshold as far above the noise "
        "floor as is practical. An AGC riding the noise floor moves the level "
        "faster than this model's own 2-second level tracker follows, and the "
        "result sounds worse than no noise reduction at all.");
    info->setWordWrap(true);
    AetherSDR::ThemeManager::instance().applyStyleSheet(
        info, "QLabel { color: {{color.text.secondary}}; font-size: 12px; }");
    {
        auto* infoRow = new QHBoxLayout;
        infoRow->setContentsMargins(0, 0, 10, 0);
        infoRow->addWidget(info);
        body->addLayout(infoRow);
    }

    {
        auto* resetBtn = makeResetIconButton();
        connect(resetBtn, &QPushButton::clicked, this, &AetherDspWidget::resetCurrentTab);
        headerRow->addWidget(resetBtn);
    }

    auto* grid = new QGridLayout;
    grid->setColumnStretch(1, 1);
    int row = 0;

    // Strength — the one control WDSP's own guide puts in front of operators.
    // It sets how far any single bin may be attenuated, so raising it leaves
    // MORE of the genuine received noise in place. That is the right answer on
    // a weak signal, and the tooltip says so, because "more is better" is the
    // wrong instinct here.
    grid->addWidget(new QLabel("Strength:"), row, 0);
    m_nnrStrengthSlider = new MarkedSlider(Nnr::maskFloorMarkerPosition());
    m_nnrStrengthSlider->setObjectName(QStringLiteral("nnrStrengthSlider"));
    m_nnrStrengthSlider->setAccessibleName(tr("NNR strength"));
    m_nnrStrengthSlider->setAccessibleDescription(
        tr("How far neural noise reduction may attenuate each frequency bin."));
    m_nnrStrengthSlider->setRange(0, 100);
    m_nnrStrengthSlider->setValue(NnrSettings::strength());
    applyChromeSliderStyle(m_nnrStrengthSlider);
    m_nnrStrengthSlider->setToolTip(
        "How much of the received noise NNR may remove.\n"
        "Lower leaves more of the real band noise in place, which often makes\n"
        "a weak signal easier to follow — the mark is WDSP's default.");
    grid->addWidget(m_nnrStrengthSlider, row, 1);
    m_nnrStrengthLabel = new QLabel(QString::number(m_nnrStrengthSlider->value()));
    m_nnrStrengthLabel->setFixedWidth(40);
    grid->addWidget(m_nnrStrengthLabel, row, 2);
    connect(m_nnrStrengthSlider, &QSlider::valueChanged, this, [this](int v) {
        m_nnrStrengthLabel->setText(QString::number(v));
        NnrSettings::setStrength(v);
        if (m_audio) {
            AudioEngine* audio = m_audio;
            QMetaObject::invokeMethod(audio, [audio, v]() { audio->setNnrStrength(v); });
        }
        emit nnrStrengthChanged(v);
        refreshStatusStrip();
    });
    ++row;

    // Model — both are compiled in, so this is a CPU budget choice rather than
    // an availability one. Premium costs roughly twice the processor time for
    // about half a dB; the engine reports back which slot actually took.
    grid->addWidget(new QLabel("Model:"), row, 0);
    {
        auto* modelRow = new QHBoxLayout;
        modelRow->setContentsMargins(0, 0, 0, 0);
        modelRow->setSpacing(14);
        m_nnrModelGroup = new QButtonGroup(this);
        m_nnrModelGroup->setExclusive(true);
        const char* kModelNames[2] = {"Standard", "Premium"};
        for (int i = 0; i < 2; ++i) {
            auto* b = makeOptionRadio(kModelNames[i]);
            b->setObjectName(QStringLiteral("nnrModelBtn") + QLatin1String(kModelNames[i]));
            b->setAccessibleName(QString::fromLatin1(kModelNames[i])
                                 + QStringLiteral(" neural noise reduction model"));
            b->setToolTip(i == 0
                ? QStringLiteral("Standard — the default. About 13% of one CPU core.")
                : QStringLiteral("Premium — measurably better at poor signal-to-noise,\n"
                                 "and about twice the CPU. Not suitable for a Pi."));
            b->setAccessibleDescription(b->toolTip());
            m_nnrModelGroup->addButton(b, i);
            modelRow->addWidget(b);
        }
        modelRow->addStretch(1);
        grid->addLayout(modelRow, row, 1, 1, 2);
        if (auto* b = m_nnrModelGroup->button(NnrSettings::model())) {
            QSignalBlocker block(b);
            b->setChecked(true);
        }
        connect(m_nnrModelGroup, &QButtonGroup::idClicked, this, [this](int slot) {
            NnrSettings::setModel(slot);
            if (m_audio) {
                AudioEngine* audio = m_audio;
                QMetaObject::invokeMethod(audio, [audio, slot]() { audio->setNnrModel(slot); });
            }
            emit nnrModelChanged(slot);
            refreshStatusStrip();
        });
    }
    ++row;

    body->addLayout(grid);

    // The six WDSP leaves undocumented. Exposed by decision (RFC #5684 §8),
    // each marked where WDSP itself starts it so an operator who has wandered
    // can see where home is. Their own panel: they are a different kind of
    // control from the two above, and the heading says why they are here.
    auto* advFrame = controlsFrame(page);
    auto* advBox = new QVBoxLayout(advFrame);
    advBox->setContentsMargins(12, 10, 12, 10);
    advBox->setSpacing(8);
    advBox->addWidget(sectionLabel(
        QStringLiteral("ADVANCED — WDSP LEAVES THESE UNDOCUMENTED"), advFrame));
    auto* advGrid = new QGridLayout;
    advGrid->setContentsMargins(0, 0, 0, 0);
    advGrid->setHorizontalSpacing(12);
    advGrid->setVerticalSpacing(6);
    advGrid->setColumnStretch(1, 1);
    row = 0;
    m_nnrAdvanced = {
        {&Nnr::kAlpha,          "Alpha:",    2, 100.0, nullptr, nullptr,
         [](double v) { NnrSettings::setAlpha(v); }},
        {&Nnr::kAlphaKnee,      "Knee:",     1,  10.0, nullptr, nullptr,
         [](double v) { NnrSettings::setAlphaKnee(v); }},
        {&Nnr::kTau,            "Tau:",      2, 100.0, nullptr, nullptr,
         [](double v) { NnrSettings::setTau(v); }},
        {&Nnr::kMaxGain,        "Max gain:", 1,  10.0, nullptr, nullptr,
         [](double v) { NnrSettings::setMaxGain(v); }},
        {&Nnr::kSmoothAttack,   "Attack:",   0,   1.0, nullptr, nullptr,
         [](double v) { NnrSettings::setSmoothAttackMs(v); }},
        {&Nnr::kSmoothRelease,  "Release:",  0,   1.0, nullptr, nullptr,
         [](double v) { NnrSettings::setSmoothReleaseMs(v); }},
    };
    const double stored[6] = {
        NnrSettings::alpha(), NnrSettings::alphaKnee(), NnrSettings::tau(),
        NnrSettings::maxGain(), NnrSettings::smoothAttackMs(),
        NnrSettings::smoothReleaseMs(),
    };
    for (std::size_t i = 0; i < m_nnrAdvanced.size(); ++i) {
        auto& c = m_nnrAdvanced[i];
        advGrid->addWidget(new QLabel(QString::fromLatin1(c.title)), row, 0);
        c.slider = new MarkedSlider(Nnr::markerPosition(*c.spec));
        c.slider->setObjectName(QStringLiteral("nnrAdvSlider%1").arg(i));
        c.slider->setAccessibleName(tr("NNR %1").arg(QString::fromLatin1(c.title)
                                                     .remove(QLatin1Char(':'))));
        c.slider->setRange(static_cast<int>(std::lround(c.spec->minimum * c.scale)),
                           static_cast<int>(std::lround(c.spec->maximum * c.scale)));
        c.slider->setValue(static_cast<int>(std::lround(stored[i] * c.scale)));
        applyChromeSliderStyle(c.slider);
        // Tau is the level tracker the AGC note above refers to, so its
        // tooltip carries the connection rather than leaving the operator to
        // infer it from a Greek letter.
        const QString extra = (c.spec == &Nnr::kTau)
            ? QStringLiteral("\nHow fast the model follows level changes. Raise it "
                             "if an active AGC makes the output pump.")
            : QString();
        c.slider->setToolTip(
            QStringLiteral("%1 %2 — WDSP's default is %3%4. The mark is that value.")
                .arg(QString::fromLatin1(c.title).remove(QLatin1Char(':')))
                .arg(QString::fromLatin1(c.spec->unit).isEmpty()
                         ? QString() : QStringLiteral("(%1)").arg(QString::fromLatin1(c.spec->unit)))
                .arg(c.spec->defaultValue, 0, 'f', c.decimals)
                .arg(QString::fromLatin1(c.spec->unit)) + extra);
        advGrid->addWidget(c.slider, row, 1);
        c.value = new QLabel(QString::number(stored[i], 'f', c.decimals));
        c.value->setFixedWidth(40);
        advGrid->addWidget(c.value, row, 2);
        connect(c.slider, &QSlider::valueChanged, this, [this, i](int raw) {
            auto& ctl = m_nnrAdvanced[i];
            const double v = raw / ctl.scale;
            ctl.value->setText(QString::number(v, 'f', ctl.decimals));
            ctl.apply(v);
            if (m_audio) {
                AudioEngine* audio = m_audio;
                QMetaObject::invokeMethod(audio, [audio]() { audio->applyNnrTuning(); });
            }
        });
        ++row;
    }

    advBox->addLayout(advGrid);
    vbox->addWidget(advFrame);
    vbox->addStretch(1);
    return page;
}

QWidget* AetherDspWidget::buildDfnrPage()
{
    auto* page = new QWidget;
    auto* vbox = new QVBoxLayout(page);
    vbox->setContentsMargins(0, 0, 0, 0);
    vbox->setSpacing(6);
    auto* frame = controlsFrame(page);
    vbox->addWidget(frame);
    auto* body = new QVBoxLayout(frame);
    body->setContentsMargins(12, 10, 12, 10);
    body->setSpacing(8);
    auto* headerRow = new QHBoxLayout;
    headerRow->setContentsMargins(0, 0, 0, 0);
    headerRow->addWidget(sectionLabel(QStringLiteral("DEEPFILTERNET"), frame));
    headerRow->addStretch(1);
    body->addLayout(headerRow);
    // 20 px top breathing room between the DSP selector buttons and
    // the info paragraph; 10 px left margin for the controls body.

    // GroupBox dropped — the rest of the AetherDSP applet uses simple
    // labelled rows, so the rounded-frame chrome around DFNR was the
    // odd one out.
    auto* grid = new QGridLayout;
    grid->setColumnStretch(1, 1);

    auto& s = AppSettings::instance();

    auto* info = new QLabel("AI-powered speech enhancement — higher fidelity than RNNoise "
                            "in high-noise HF environments. CPU-only, 10 ms latency, 48 kHz.");
#ifndef HAVE_DFNR
    info->setText("DFNR is unavailable in this build. Set up the platform "
                  "DeepFilterNet library and rebuild AetherSDR to enable it.");
    info->setToolTip(kDfnrUnavailableToolTip);
#endif
    info->setWordWrap(true);
    AetherSDR::ThemeManager::instance().applyStyleSheet(info, "QLabel { color: {{color.text.secondary}}; font-size: 12px; }");
    {
        auto* infoRow = new QHBoxLayout;
        infoRow->setContentsMargins(0, 0, 10, 0);
        infoRow->addWidget(info);
        body->addLayout(infoRow);
    }

    // Reset Defaults on its own row between the info paragraph and the
    // slider grid — right-aligned with 10 px right padding to nudge it
    // inboard so it lines up over the slider value-label column below.
    {
        auto* dfnrResetBtn = makeResetIconButton();
        connect(dfnrResetBtn, &QPushButton::clicked,
                this, &AetherDspWidget::resetCurrentTab);
#ifndef HAVE_DFNR
        dfnrResetBtn->setEnabled(false);
        dfnrResetBtn->setToolTip(kDfnrUnavailableToolTip);
        dfnrResetBtn->setAccessibleDescription(kDfnrUnavailableToolTip);
#endif
        headerRow->addWidget(dfnrResetBtn);
    }

    auto* attenTitle = new QLabel("Attenuation Limit");
    grid->addWidget(attenTitle, 1, 0);
    m_dfnrAttenSlider = new QSlider(Qt::Horizontal);
    m_dfnrAttenSlider->setObjectName(QStringLiteral("dfnrAttenLimitSlider"));
    m_dfnrAttenSlider->setAccessibleName(tr("DFNR attenuation limit"));
    m_dfnrAttenSlider->setAccessibleDescription(
        tr("Maximum noise attenuation in dB for DeepFilterNet noise reduction."));
    m_dfnrAttenSlider->setRange(0, 100);
    m_dfnrAttenSlider->setValue(static_cast<int>(s.value("DfnrAttenLimit", "100").toFloat()));
    applyChromeSliderStyle(m_dfnrAttenSlider);
    m_dfnrAttenSlider->setToolTip("Maximum noise attenuation in dB.\n"
                                   "0 dB = passthrough (no denoising)\n"
                                   "100 dB = maximum noise removal\n\n"
                                   "For weak signals: 20–30 dB\n"
                                   "For casual listening: 40–60 dB\n"
                                   "For strong signals: 80–100 dB");
#ifndef HAVE_DFNR
    attenTitle->setEnabled(false);
    attenTitle->setToolTip(kDfnrUnavailableToolTip);
    attenTitle->setAccessibleDescription(kDfnrUnavailableToolTip);
    m_dfnrAttenSlider->setEnabled(false);
    m_dfnrAttenSlider->setToolTip(kDfnrUnavailableToolTip);
    m_dfnrAttenSlider->setAccessibleDescription(kDfnrUnavailableToolTip);
#endif
    grid->addWidget(m_dfnrAttenSlider, 1, 1);
    m_dfnrAttenLabel = new QLabel(QString::number(m_dfnrAttenSlider->value()));
    m_dfnrAttenLabel->setFixedWidth(40);
#ifndef HAVE_DFNR
    m_dfnrAttenLabel->setEnabled(false);
    m_dfnrAttenLabel->setToolTip(kDfnrUnavailableToolTip);
    m_dfnrAttenLabel->setAccessibleDescription(kDfnrUnavailableToolTip);
#endif
    grid->addWidget(m_dfnrAttenLabel, 1, 2);

    connect(m_dfnrAttenSlider, &QSlider::valueChanged, this, [this](int v) {
        m_dfnrAttenLabel->setText(QString::number(v));
        float db = static_cast<float>(v);
        auto& s = AppSettings::instance();
        s.setValue("DfnrAttenLimit", QString::number(db, 'f', 0));
        s.save();
        emit dfnrAttenLimitChanged(db);
    });

    auto* betaTitle = new QLabel("Post-Filter Beta");
    grid->addWidget(betaTitle, 2, 0);
    m_dfnrBetaSlider = new QSlider(Qt::Horizontal);
    m_dfnrBetaSlider->setObjectName(QStringLiteral("dfnrPostFilterBetaSlider"));
    m_dfnrBetaSlider->setAccessibleName(tr("DFNR post-filter beta"));
    m_dfnrBetaSlider->setAccessibleDescription(
        tr("Post-filter strength for DeepFilterNet noise reduction."));
    m_dfnrBetaSlider->setRange(0, 30);
    m_dfnrBetaSlider->setValue(static_cast<int>(s.value("DfnrPostFilterBeta", "0.0").toFloat() * 100));
    applyChromeSliderStyle(m_dfnrBetaSlider);
    m_dfnrBetaSlider->setToolTip("Post-filter strength for additional noise suppression.\n"
                                  "0 = disabled (default)\n"
                                  "0.05–0.15 = subtle additional filtering\n"
                                  "0.15–0.30 = aggressive post-processing");
#ifndef HAVE_DFNR
    betaTitle->setEnabled(false);
    betaTitle->setToolTip(kDfnrUnavailableToolTip);
    betaTitle->setAccessibleDescription(kDfnrUnavailableToolTip);
    m_dfnrBetaSlider->setEnabled(false);
    m_dfnrBetaSlider->setToolTip(kDfnrUnavailableToolTip);
    m_dfnrBetaSlider->setAccessibleDescription(kDfnrUnavailableToolTip);
#endif
    grid->addWidget(m_dfnrBetaSlider, 2, 1);
    m_dfnrBetaLabel = new QLabel(QString::number(m_dfnrBetaSlider->value() / 100.0f, 'f', 2));
    m_dfnrBetaLabel->setFixedWidth(40);
#ifndef HAVE_DFNR
    m_dfnrBetaLabel->setEnabled(false);
    m_dfnrBetaLabel->setToolTip(kDfnrUnavailableToolTip);
    m_dfnrBetaLabel->setAccessibleDescription(kDfnrUnavailableToolTip);
#endif
    grid->addWidget(m_dfnrBetaLabel, 2, 2);

    connect(m_dfnrBetaSlider, &QSlider::valueChanged, this, [this](int v) {
        float beta = v / 100.0f;
        m_dfnrBetaLabel->setText(QString::number(beta, 'f', 2));
        auto& s = AppSettings::instance();
        s.setValue("DfnrPostFilterBeta", QString::number(beta, 'f', 2));
        s.save();
        emit dfnrPostFilterBetaChanged(beta);
    });

    body->addLayout(grid);
    vbox->addStretch();
    return page;
}

// ── Sync from the feature-owned settings model ───────────────────────────────

void AetherDspWidget::syncNr2Settings()
{
    const Nr2SettingsModel::Config config =
        Nr2SettingsModel::instance().config();

    // The post-processing group, which a profile switch or a reset changes
    // from outside this widget. Signals blocked: this is a refresh FROM the
    // model, so re-emitting would write the value we just read back into it.
    if (m_nr2Post2Check) {
        QSignalBlocker blocker(m_nr2Post2Check);
        m_nr2Post2Check->setChecked(config.post2Run);
    }
    if (m_nr2Post2NlevelSlider) {
        QSignalBlocker blocker(m_nr2Post2NlevelSlider);
        m_nr2Post2NlevelSlider->setValue(
            static_cast<int>(std::lround(config.post2Nlevel * 100.0f)));
        m_nr2Post2NlevelSlider->setEnabled(config.post2Run);
        if (m_nr2Post2NlevelLabel) {
            m_nr2Post2NlevelLabel->setText(
                QString::number(config.post2Nlevel, 'f', 2));
        }
    }
    if (m_nr2Post2FactorSlider) {
        QSignalBlocker blocker(m_nr2Post2FactorSlider);
        m_nr2Post2FactorSlider->setValue(
            static_cast<int>(std::lround(config.post2Factor * 100.0f)));
        m_nr2Post2FactorSlider->setEnabled(config.post2Run);
        if (m_nr2Post2FactorLabel) {
            m_nr2Post2FactorLabel->setText(
                QString::number(config.post2Factor, 'f', 2));
        }
    }
    if (m_nr2Post2TaperSlider) {
        QSignalBlocker blocker(m_nr2Post2TaperSlider);
        m_nr2Post2TaperSlider->setValue(
            static_cast<int>(std::lround(config.post2TaperHz)));
        m_nr2Post2TaperSlider->setEnabled(config.post2Run);
        if (m_nr2Post2TaperLabel) {
            m_nr2Post2TaperLabel->setText(
                QString::number(static_cast<int>(config.post2TaperHz)));
        }
    }

    if (QAbstractButton* button =
            m_nr2GainGroup->button(config.gainMethod)) {
        QSignalBlocker blocker(button);
        button->setChecked(true);
    }
    if (QAbstractButton* button =
            m_nr2NpeGroup->button(config.npeMethod)) {
        QSignalBlocker blocker(button);
        button->setChecked(true);
    }

    {
        QSignalBlocker blocker(m_nr2AeCheck);
        m_nr2AeCheck->setChecked(config.aeFilter);
    }

    const int gainMax = std::clamp(
        static_cast<int>(std::lround(config.gainMax * 100.0f)), 50, 200);
    {
        QSignalBlocker blocker(m_nr2GainMaxSlider);
        m_nr2GainMaxSlider->setValue(gainMax);
    }
    m_nr2GainMaxLabel->setText(
        QString::number(gainMax / 100.0f, 'f', 2));

    const int gainFloor = std::clamp(
        static_cast<int>(std::lround(config.gainFloor * 100.0f)), 0, 15);
    {
        QSignalBlocker blocker(m_nr2GainFloorSlider);
        m_nr2GainFloorSlider->setValue(gainFloor);
    }
    m_nr2GainFloorLabel->setText(
        QString::number(gainFloor / 100.0f, 'f', 2));

    const int gainSmooth = std::clamp(
        static_cast<int>(std::lround(config.gainSmooth * 100.0f)), 50, 98);
    {
        QSignalBlocker blocker(m_nr2SmoothSlider);
        m_nr2SmoothSlider->setValue(gainSmooth);
    }
    m_nr2SmoothLabel->setText(
        QString::number(gainSmooth / 100.0f, 'f', 2));

    const int qspp = std::clamp(
        static_cast<int>(std::lround(config.qspp * 100.0f)), 5, 50);
    {
        QSignalBlocker blocker(m_nr2QsppSlider);
        m_nr2QsppSlider->setValue(qspp);
    }
    m_nr2QsppLabel->setText(QString::number(qspp / 100.0f, 'f', 2));

    updateNr2ControlAvailability();
}

// ── Sync from engine and saved settings ──────────────────────────────────────

void AetherDspWidget::syncFromEngine()
{
    syncDspSelectorFromEngine();
    syncNr2Settings();

    auto& s = AppSettings::instance();

    if (m_mnrStrengthSlider) {
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
    // Last, so the reading reflects every control this pass just moved.
    refreshStatusStrip();
}

} // namespace AetherSDR
