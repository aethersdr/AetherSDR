#include "ClientPuduEditor.h"
#include "ClientCompKnob.h"
#include "EditorFramelessTitleBar.h"
#include "PooDooLogo.h"
#include "core/AppSettings.h"
#include "core/AudioEngine.h"
#include "core/ClientPudu.h"

#include <QButtonGroup>
#include <QCloseEvent>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QLabel>
#include <QMoveEvent>
#include <QPushButton>
#include <QResizeEvent>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <cmath>

namespace MasterSDR {

namespace {

constexpr int kDefaultWidth  = 640;
constexpr int kDefaultHeight = 360;

constexpr const char* kWindowStyle =
    "QWidget { background: #08121d; color: #d7e7f2; }"
    "QLabel  { background: transparent; color: #8aa8c0; font-size: 11px; }";

const QString kBypassStyle = QStringLiteral(
    "QPushButton {"
    "  background: #0e1b28; color: #8aa8c0;"
    "  border: 1px solid #243a4e; border-radius: 3px;"
    "  font-size: 11px; font-weight: bold; padding: 3px 12px;"
    "}"
    "QPushButton:hover { background: #1a2a3a; }"
    "QPushButton:checked {"
    "  background: #3a2a0e; color: #f2c14e; border: 1px solid #f2c14e;"
    "}"
    "QPushButton:checked:hover { background: #4a3a1e; }");

const QString kModeStyle = QStringLiteral(
    "QPushButton {"
    "  background: #1a2a3a; border: 1px solid #2a4458; border-radius: 3px;"
    "  color: #8aa8c0; font-size: 12px; font-weight: bold;"
    "  padding: 4px 20px; min-width: 34px;"
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
    h->setSpacing(6);

    auto* leftLine = new QFrame;
    leftLine->setFrameShape(QFrame::HLine);
    leftLine->setFrameShadow(QFrame::Plain);
    leftLine->setStyleSheet("QFrame { color: #5a6a7a; }");

    auto* lbl = new QLabel(text);
    lbl->setStyleSheet("QLabel { color: #c8d8e8; font-weight: bold; "
                       "font-size: 13px; letter-spacing: 2px; }");
    lbl->setAlignment(Qt::AlignCenter);

    auto* rightLine = new QFrame;
    rightLine->setFrameShape(QFrame::HLine);
    rightLine->setFrameShadow(QFrame::Plain);
    rightLine->setStyleSheet("QFrame { color: #5a6a7a; }");

    h->addWidget(leftLine, 1);
    h->addWidget(lbl);
    h->addWidget(rightLine, 1);
    return w;
}

} // namespace

ClientPuduEditor::ClientPuduEditor(AudioEngine* engine, QWidget* parent)
    : QWidget(parent, Qt::Window | Qt::FramelessWindowHint)
    , m_audio(engine)
{
    setWindowTitle(QString::fromUtf8("Masterial Poodoo\xe2\x84\xa2"));
    setStyleSheet(kWindowStyle);
    resize(kDefaultWidth, kDefaultHeight);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 0, 12, 10);
    root->setSpacing(8);

    auto* titleBar = new EditorFramelessTitleBar;
    m_titleBar = titleBar;
    root->addWidget(titleBar);

    // ── Logo (big in the editor) ────────────────────────────────
    m_logo = new PooDooLogo;
    m_logo->setMinimumHeight(80);
    root->addWidget(m_logo);

    // ── Even / Odd mode toggle — centred in the gap between the
    // PooDoo™ wordmark and the Poo/Doo knob row.  Aphex generates
    // even harmonics (asymmetric shape), Behringer generates odd
    // harmonics (symmetric tanh); labelling by harmonic content is
    // more descriptive than A/B.
    {
        auto* row = new QHBoxLayout;
        row->setSpacing(6);
        row->addStretch();

        auto* group = new QButtonGroup(this);
        group->setExclusive(true);
        m_modeA = new QPushButton("Even");
        m_modeA->setCheckable(true);
        m_modeA->setStyleSheet(kModeStyle);
        m_modeA->setFixedHeight(24);
        m_modeA->setToolTip(
            "Aphex-lineage asymmetric shaping — predominantly even "
            "harmonics, warmer, diode-style character with Big Bottom "
            "LF saturation.");
        group->addButton(m_modeA, 0);
        row->addWidget(m_modeA);

        m_modeB = new QPushButton("Odd");
        m_modeB->setCheckable(true);
        m_modeB->setStyleSheet(kModeStyle);
        m_modeB->setFixedHeight(24);
        m_modeB->setToolTip(
            "Behringer-lineage symmetric tanh shaping — pure odd "
            "harmonics, brighter / edgier, paired with a feed-forward "
            "bass compressor.");
        group->addButton(m_modeB, 1);
        row->addWidget(m_modeB);

        connect(group, &QButtonGroup::idToggled, this,
                [this](int id, bool checked) {
            if (checked) onModeToggled(id);
        });

        row->addStretch();
        root->addLayout(row);
    }

    // Tight 4 px gap between the Even/Odd row and the Poo/Doo bracket
    // labels below.
    root->addSpacing(4);

    // ── Knob row: all 6 on one line with Poo | gap | Doo grouping ──
    //
    // Grid: row 0 = bracket group labels (|─Poo─| and |─Doo─|),
    // row 1 = knobs.  Columns 0-2 host Poo; column 3 is a fixed
    // spacer separating the two groups; columns 4-6 host Doo.
    {
        auto* grid = new QGridLayout;
        grid->setContentsMargins(0, 0, 0, 0);
        grid->setHorizontalSpacing(12);
        grid->setVerticalSpacing(4);
        grid->setColumnMinimumWidth(3, 32);   // gap between Poo and Doo

        grid->addWidget(makeBracketLabel("Poo"), 0, 0, 1, 3);
        grid->addWidget(makeBracketLabel("Doo"), 0, 4, 1, 3);

        auto makeKnob = [](const QString& label) {
            auto* k = new ClientCompKnob;
            k->setLabel(label);
            k->setCenterLabelMode(true);
            // Tighter vertical footprint in center-label mode since
            // the label sits inside the ring now.  Ring + value row.
            k->setFixedSize(76, 76);
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
                this, &ClientPuduEditor::applyPooDrive);
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
                this, &ClientPuduEditor::applyPooTune);
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
                this, &ClientPuduEditor::applyPooMix);
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
                this, &ClientPuduEditor::applyDooTune);
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
                this, &ClientPuduEditor::applyDooHarmonics);
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
                this, &ClientPuduEditor::applyDooMix);
        grid->addWidget(m_dooMix, 1, 6, Qt::AlignHCenter);

        root->addLayout(grid);
    }

    root->addStretch();

    if (m_audio && pudu()) {
        m_logo->setPudu(pudu());
    }

    syncControlsFromEngine();
}

ClientPuduEditor::~ClientPuduEditor() = default;

ClientPudu* ClientPuduEditor::pudu() const
{
    if (!m_audio) return nullptr;
    return m_side == Side::Rx ? m_audio->clientPuduRx()
                              : m_audio->clientPuduTx();
}

void ClientPuduEditor::savePuduSettings() const
{
    if (!m_audio) return;
    if (m_side == Side::Rx) m_audio->saveClientPuduRxSettings();
    else                    m_audio->saveClientPuduSettings();
}

void ClientPuduEditor::showForTx()
{
    m_side = Side::Tx;
    if (m_logo && pudu()) m_logo->setPudu(pudu());
    const QString title = QString::fromUtf8("Masterial Poodoo\xe2\x84\xa2 \xe2\x80\x94 TX");
    if (m_titleBar)
        static_cast<EditorFramelessTitleBar*>(m_titleBar)->setTitleText(title);
    setWindowTitle(title);
    syncControlsFromEngine();
    restoreGeometryFromSettings();
    show();
    raise();
    activateWindow();
}

void ClientPuduEditor::showForRx()
{
    m_side = Side::Rx;
    if (m_logo && pudu()) m_logo->setPudu(pudu());
    const QString title = QString::fromUtf8("Masterial Poodoo\xe2\x84\xa2 \xe2\x80\x94 RX");
    if (m_titleBar)
        static_cast<EditorFramelessTitleBar*>(m_titleBar)->setTitleText(title);
    setWindowTitle(title);
    syncControlsFromEngine();
    restoreGeometryFromSettings();
    show();
    raise();
    activateWindow();
}

void ClientPuduEditor::syncControlsFromEngine()
{
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

void ClientPuduEditor::onModeToggled(int id)
{
    if (m_restoring || !m_audio) return;
    pudu()->setMode(
        id == 1 ? ClientPudu::Mode::Behringer : ClientPudu::Mode::Aphex);
    savePuduSettings();
}

void ClientPuduEditor::applyPooDrive(float db)
{
    if (m_restoring || !m_audio) return;
    pudu()->setPooDriveDb(db);
    savePuduSettings();
}
void ClientPuduEditor::applyPooTune(float hz)
{
    if (m_restoring || !m_audio) return;
    pudu()->setPooTuneHz(hz);
    savePuduSettings();
}
void ClientPuduEditor::applyPooMix(float v)
{
    if (m_restoring || !m_audio) return;
    pudu()->setPooMix(v);
    savePuduSettings();
}
void ClientPuduEditor::applyDooTune(float hz)
{
    if (m_restoring || !m_audio) return;
    pudu()->setDooTuneHz(hz);
    savePuduSettings();
}
void ClientPuduEditor::applyDooHarmonics(float db)
{
    if (m_restoring || !m_audio) return;
    pudu()->setDooHarmonicsDb(db);
    savePuduSettings();
}
void ClientPuduEditor::applyDooMix(float v)
{
    if (m_restoring || !m_audio) return;
    pudu()->setDooMix(v);
    savePuduSettings();
}

void ClientPuduEditor::saveGeometryToSettings()
{
    if (m_restoring) return;
    AppSettings::instance().setValue(
        "ClientPuduEditorGeometry",
        QString::fromLatin1(saveGeometry().toBase64()));
}

void ClientPuduEditor::restoreGeometryFromSettings()
{
    m_restoring = true;
    const QString b64 = AppSettings::instance()
        .value("ClientPuduEditorGeometry", "").toString();
    if (!b64.isEmpty()) {
        restoreGeometry(QByteArray::fromBase64(b64.toLatin1()));
    }
    m_restoring = false;
}

void ClientPuduEditor::closeEvent(QCloseEvent* ev)
{ saveGeometryToSettings(); QWidget::closeEvent(ev); }
void ClientPuduEditor::moveEvent(QMoveEvent* ev)
{ saveGeometryToSettings(); QWidget::moveEvent(ev); }
void ClientPuduEditor::resizeEvent(QResizeEvent* ev)
{ saveGeometryToSettings(); QWidget::resizeEvent(ev); }
void ClientPuduEditor::showEvent(QShowEvent* ev)
{ QWidget::showEvent(ev); }
void ClientPuduEditor::hideEvent(QHideEvent* ev)
{ saveGeometryToSettings(); QWidget::hideEvent(ev); }

} // namespace MasterSDR
