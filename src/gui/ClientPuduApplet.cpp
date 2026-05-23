#include "ClientPuduApplet.h"
#include "ClientCompKnob.h"
#include "PooDooLogo.h"
#include "core/AudioEngine.h"
#include "core/ClientPudu.h"

#include <QButtonGroup>
#include <QFrame>
#include <QGridLayout>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <cmath>

namespace MasterSDR {

namespace {

const QString kEnableStyle =
    "QPushButton {"
    "  background: #1a2a3a; border: 1px solid #205070; border-radius: 3px;"
    "  color: #c8d8e8; font-size: 11px; font-weight: bold; padding: 2px 10px;"
    "}"
    "QPushButton:hover { background: #204060; }"
    "QPushButton:checked {"
    "  background: #006040; color: #00ff88; border: 1px solid #00a060;"
    "}";

constexpr const char* kEditStyle =
    "QPushButton {"
    "  background: #1a2a3a; border: 1px solid #205070; border-radius: 3px;"
    "  color: #c8d8e8; font-size: 11px; padding: 2px 10px;"
    "}"
    "QPushButton:hover { background: #204060; }";

const QString kModeStyle = QStringLiteral(
    "QPushButton {"
    "  background: #1a2a3a; border: 1px solid #2a4458; border-radius: 3px;"
    "  color: #8aa8c0; font-size: 11px; font-weight: bold;"
    "  padding: 3px 14px; min-width: 26px;"
    "}"
    "QPushButton:hover { background: #24384e; }"
    "QPushButton:checked {"
    "  background: #3a2a0e; color: #f2c14e; border: 1px solid #f2c14e;"
    "}");

// "|─── text ───|" group label: horizontal line, centred text, horizontal line.
QWidget* makeBracketLabel(const QString& text)
{
    auto* w = new QWidget;
    auto* h = new QHBoxLayout(w);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(4);

    auto* leftLine = new QFrame;
    leftLine->setFrameShape(QFrame::HLine);
    leftLine->setFrameShadow(QFrame::Plain);
    leftLine->setStyleSheet("QFrame { color: #4a5a6a; }");

    auto* lbl = new QLabel(text);
    lbl->setStyleSheet("QLabel { color: #c8d8e8; font-weight: bold; "
                       "font-size: 10px; }");
    lbl->setAlignment(Qt::AlignCenter);

    auto* rightLine = new QFrame;
    rightLine->setFrameShape(QFrame::HLine);
    rightLine->setFrameShadow(QFrame::Plain);
    rightLine->setStyleSheet("QFrame { color: #4a5a6a; }");

    h->addWidget(leftLine, 1);
    h->addWidget(lbl);
    h->addWidget(rightLine, 1);
    return w;
}

} // namespace

ClientPuduApplet::ClientPuduApplet(Side side, QWidget* parent)
    : QWidget(parent)
    , m_side(side)
{
    buildUI();
    hide();
}

ClientPudu* ClientPuduApplet::pudu() const
{
    if (!m_audio) return nullptr;
    return m_side == Side::Rx ? m_audio->clientPuduRx()
                              : m_audio->clientPuduTx();
}

void ClientPuduApplet::savePuduSettings() const
{
    if (!m_audio) return;
    if (m_side == Side::Rx) m_audio->saveClientPuduRxSettings();
    else                    m_audio->saveClientPuduSettings();
}

void ClientPuduApplet::buildUI()
{
    setStyleSheet("QWidget { background: transparent; }");

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(4, 4, 4, 4);
    outer->setSpacing(4);

    // ── Logo ────────────────────────────────────────────────────
    m_logo = new PooDooLogo;
    m_logo->setMinimumHeight(40);
    m_logo->setWordmark(QString::fromUtf8("MasterVoice\xe2\x84\xa2"));
    outer->addWidget(m_logo);

    // ── A/B mode toggle ─────────────────────────────────────────
    {
        auto* row = new QHBoxLayout;
        row->setSpacing(4);
        row->addStretch();
        auto* group = new QButtonGroup(this);
        group->setExclusive(true);

        m_modeA = new QPushButton("Even");
        m_modeA->setCheckable(true);
        m_modeA->setStyleSheet(kModeStyle);
        m_modeA->setFixedHeight(22);
        m_modeA->setToolTip(
            "Aphex-lineage asymmetric shaping — predominantly even "
            "harmonics, warmer, with Big Bottom LF saturation.");
        group->addButton(m_modeA, 0);
        row->addWidget(m_modeA);

        m_modeB = new QPushButton("Odd");
        m_modeB->setCheckable(true);
        m_modeB->setStyleSheet(kModeStyle);
        m_modeB->setFixedHeight(22);
        m_modeB->setToolTip(
            "Behringer-lineage symmetric tanh shaping — pure odd "
            "harmonics, brighter, with a feed-forward bass compressor.");
        group->addButton(m_modeB, 1);
        row->addWidget(m_modeB);
        row->addStretch();

        connect(group, &QButtonGroup::idToggled, this,
                [this](int id, bool checked) {
            if (checked) onModeToggled(id);
        });

        outer->addLayout(row);
    }

    // ── Knob row — all 6 on one line with Poo | gap | Doo grouping ──
    //
    // Grid layout: row 0 = bracket labels, row 1 = knobs.  Knobs
    // occupy columns 0-2 (Poo) and 4-6 (Doo); column 3 is a fixed
    // spacer separating the two groups.
    {
        auto* grid = new QGridLayout;
        grid->setContentsMargins(0, 0, 0, 0);
        grid->setHorizontalSpacing(0);
        grid->setVerticalSpacing(1);
        grid->setColumnMinimumWidth(3, 8);    // gap between Body and Clarity

        grid->addWidget(makeBracketLabel("Body"),    0, 0, 1, 3);
        grid->addWidget(makeBracketLabel("Clarity"), 0, 4, 1, 3);

        auto makeKnob = [](const QString& label) {
            auto* k = new ClientCompKnob;
            k->setLabel(label);
            k->setCenterLabelMode(true);
            k->setInlineEditEnabled(false);
            // 260 px panel budget: 6 × 38 = 228 + 10 px gap +
            // margins comfortably inside the container.  Ring +
            // value row fit in ~48 px vertical.
            k->setFixedSize(38, 48);
            return k;
        };

        m_pooDrive = makeKnob("Drive");
        m_pooDrive->setRange(0.0f, 24.0f);
        m_pooDrive->setDefault(6.0f);
        m_pooDrive->setValueFromNorm([](float n) { return n * 24.0f; });
        m_pooDrive->setNormFromValue([](float v) { return v / 24.0f; });
        m_pooDrive->setLabelFormat([](float v) {
            return QString::number(v, 'f', 1) + " dB";
        });
        connect(m_pooDrive, &ClientCompKnob::valueChanged,
                this, &ClientPuduApplet::applyPooDrive);
        grid->addWidget(m_pooDrive, 1, 0, Qt::AlignHCenter);

        m_pooTune = makeKnob("Tune");
        m_pooTune->setRange(50.0f, 160.0f);
        m_pooTune->setDefault(100.0f);
        m_pooTune->setValueFromNorm([](float n) { return 50.0f + n * 110.0f; });
        m_pooTune->setNormFromValue([](float v) { return (v - 50.0f) / 110.0f; });
        m_pooTune->setLabelFormat([](float v) {
            return QString::number(v, 'f', 0) + " Hz";
        });
        connect(m_pooTune, &ClientCompKnob::valueChanged,
                this, &ClientPuduApplet::applyPooTune);
        grid->addWidget(m_pooTune, 1, 1, Qt::AlignHCenter);

        m_pooMix = makeKnob("Mix");
        m_pooMix->setRange(0.0f, 1.0f);
        m_pooMix->setDefault(0.3f);
        m_pooMix->setValueFromNorm([](float n) { return n; });
        m_pooMix->setNormFromValue([](float v) { return v; });
        m_pooMix->setLabelFormat([](float v) {
            return QString::number(static_cast<int>(v * 100.0f + 0.5f)) + " %";
        });
        connect(m_pooMix, &ClientCompKnob::valueChanged,
                this, &ClientPuduApplet::applyPooMix);
        grid->addWidget(m_pooMix, 1, 2, Qt::AlignHCenter);

        m_dooTune = makeKnob("Tune");
        m_dooTune->setRange(1000.0f, 10000.0f);
        m_dooTune->setDefault(5000.0f);
        m_dooTune->setValueFromNorm([](float n) {
            return 1000.0f * std::pow(10.0f, n);
        });
        m_dooTune->setNormFromValue([](float v) {
            return std::log10(std::max(1000.0f, v) / 1000.0f);
        });
        m_dooTune->setLabelFormat([](float v) {
            return (v >= 1000.0f)
                ? QString::number(v / 1000.0f, 'f', 1) + " kHz"
                : QString::number(v, 'f', 0) + " Hz";
        });
        connect(m_dooTune, &ClientCompKnob::valueChanged,
                this, &ClientPuduApplet::applyDooTune);
        grid->addWidget(m_dooTune, 1, 4, Qt::AlignHCenter);

        m_dooHarmonics = makeKnob("Air");
        m_dooHarmonics->setRange(0.0f, 24.0f);
        m_dooHarmonics->setDefault(6.0f);
        m_dooHarmonics->setValueFromNorm([](float n) { return n * 24.0f; });
        m_dooHarmonics->setNormFromValue([](float v) { return v / 24.0f; });
        m_dooHarmonics->setLabelFormat([](float v) {
            return QString::number(v, 'f', 1) + " dB";
        });
        connect(m_dooHarmonics, &ClientCompKnob::valueChanged,
                this, &ClientPuduApplet::applyDooHarmonics);
        grid->addWidget(m_dooHarmonics, 1, 5, Qt::AlignHCenter);

        m_dooMix = makeKnob("Mix");
        m_dooMix->setRange(0.0f, 1.0f);
        m_dooMix->setDefault(0.3f);
        m_dooMix->setValueFromNorm([](float n) { return n; });
        m_dooMix->setNormFromValue([](float v) { return v; });
        m_dooMix->setLabelFormat([](float v) {
            return QString::number(static_cast<int>(v * 100.0f + 0.5f)) + " %";
        });
        connect(m_dooMix, &ClientCompKnob::valueChanged,
                this, &ClientPuduApplet::applyDooMix);
        grid->addWidget(m_dooMix, 1, 6, Qt::AlignHCenter);

        outer->addLayout(grid);
    }

    // Enable / Edit buttons removed — CHAIN widget handles bypass
    // (single-click) and editor-open (double-click).  Even/Odd mode
    // toggle above remains.
}

void ClientPuduApplet::setAudioEngine(AudioEngine* engine)
{
    m_audio = engine;
    if (!m_audio) return;
    if (m_logo) m_logo->setPudu(pudu());
    syncControlsFromEngine();
}

void ClientPuduApplet::syncControlsFromEngine()
{
    // Bypass dim — render the whole tile at reduced opacity when the
    // stage is bypassed, matching the dim effect on the EQ curve.
    const bool dspEnabled = (m_audio && pudu()) ? pudu()->isEnabled() : true;
    auto* eff = qobject_cast<QGraphicsOpacityEffect*>(graphicsEffect());
    if (!eff) {
        eff = new QGraphicsOpacityEffect(this);
        setGraphicsEffect(eff);
    }
    eff->setOpacity(dspEnabled ? 1.0 : 0.55);

    if (!m_audio || !pudu()) return;
    ClientPudu* p = pudu();

    m_restoring = true;
    {
        const bool isA = (p->mode() == ClientPudu::Mode::Aphex);
        QSignalBlocker ba(m_modeA);
        QSignalBlocker bb(m_modeB);
        m_modeA->setChecked(isA);
        m_modeB->setChecked(!isA);
    }
    { QSignalBlocker b(m_pooDrive);     m_pooDrive->setValue(p->pooDriveDb()); }
    { QSignalBlocker b(m_pooTune);      m_pooTune->setValue(p->pooTuneHz()); }
    { QSignalBlocker b(m_pooMix);       m_pooMix->setValue(p->pooMix()); }
    { QSignalBlocker b(m_dooTune);      m_dooTune->setValue(p->dooTuneHz()); }
    { QSignalBlocker b(m_dooHarmonics); m_dooHarmonics->setValue(p->dooHarmonicsDb()); }
    { QSignalBlocker b(m_dooMix);       m_dooMix->setValue(p->dooMix()); }
    m_restoring = false;
}

void ClientPuduApplet::refreshEnableFromEngine()
{
    syncControlsFromEngine();
}

void ClientPuduApplet::onEnableToggled(bool on)
{
    if (m_restoring || !m_audio) return;
    ClientPudu* p = pudu();
    if (!p) return;
    p->setEnabled(on);
    savePuduSettings();
}

void ClientPuduApplet::onModeToggled(int id)
{
    if (m_restoring || !m_audio) return;
    pudu()->setMode(
        id == 1 ? ClientPudu::Mode::Behringer : ClientPudu::Mode::Aphex);
    savePuduSettings();
}

void ClientPuduApplet::applyPooDrive(float db)
{
    if (m_restoring || !m_audio) return;
    pudu()->setPooDriveDb(db);
    savePuduSettings();
}
void ClientPuduApplet::applyPooTune(float hz)
{
    if (m_restoring || !m_audio) return;
    pudu()->setPooTuneHz(hz);
    savePuduSettings();
}
void ClientPuduApplet::applyPooMix(float v)
{
    if (m_restoring || !m_audio) return;
    pudu()->setPooMix(v);
    savePuduSettings();
}
void ClientPuduApplet::applyDooTune(float hz)
{
    if (m_restoring || !m_audio) return;
    pudu()->setDooTuneHz(hz);
    savePuduSettings();
}
void ClientPuduApplet::applyDooHarmonics(float db)
{
    if (m_restoring || !m_audio) return;
    pudu()->setDooHarmonicsDb(db);
    savePuduSettings();
}
void ClientPuduApplet::applyDooMix(float v)
{
    if (m_restoring || !m_audio) return;
    pudu()->setDooMix(v);
    savePuduSettings();
}

} // namespace MasterSDR
