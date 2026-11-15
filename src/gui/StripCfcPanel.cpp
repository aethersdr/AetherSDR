#include "StripCfcPanel.h"

#include "ClientCfcCurveWidget.h"
#include "ClientCompKnob.h"
#include "EditorFramelessTitleBar.h"
#include "core/AppSettings.h"
#include "core/AudioEngine.h"
#include "core/ClientCfc.h"
#include "core/ThemeManager.h"

#include <QCloseEvent>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QLabel>
#include <QMoveEvent>
#include <QPushButton>
#include <QResizeEvent>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>

#include <array>
#include <cmath>

namespace AetherSDR {

namespace {

// A lit toggle, so Post-EQ on reads as on.
constexpr const char* kToggleStyle =
    "QPushButton {"
    "  background: {{color.background.1}}; border: 1px solid {{color.border.strong}};"
    "  border-radius: 3px; color: {{color.text.secondary}}; font-size: 12px;"
    "  font-weight: bold; padding: 4px 14px;"
    "}"
    "QPushButton:hover { background: {{color.background.2}}; }"
    "QPushButton:checked { color: {{color.accent}}; border: 1px solid {{color.accent}}; }";

constexpr const char* kWindowStyle =
    "QWidget { background: {{color.background.0}}; color: {{color.text.primary}}; }"
    "QLabel  { background: transparent; color: {{color.text.secondary}}; font-size: 11px; }";

constexpr std::array<int, 5> kFftSizes{1024, 2048, 4096, 8192, 16384};

int fftIndex(int size)
{
    for (std::size_t i = 0; i < kFftSizes.size(); ++i) {
        if (kFftSizes[i] == size) {
            return static_cast<int>(i);
        }
    }
    return 0;
}

QDoubleSpinBox* makeSpin(double lo, double hi, double step, int decimals, const QString& suffix,
                         const QString& name)
{
    auto* spin = new QDoubleSpinBox;
    spin->setRange(lo, hi);
    spin->setSingleStep(step);
    spin->setDecimals(decimals);
    spin->setSuffix(suffix);
    spin->setAccessibleName(name);
    spin->setKeyboardTracking(false);
    return spin;
}

}  // namespace

StripCfcPanel::StripCfcPanel(AudioEngine* engine, QWidget* parent)
    : QWidget(parent)
    , m_audio(engine)
{
    const QString title = QString::fromUtf8("Aetherial CFC \xe2\x80\x94 TX");
    setWindowTitle(title);
    ThemeManager::instance().applyStyleSheet(this, kWindowStyle);
    resize(620, 360);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(16, 0, 16, 14);
    root->setSpacing(8);

    auto* titleBar = new EditorFramelessTitleBar;
    titleBar->setTitleText(title);
    root->addWidget(titleBar);

    m_curve = new ClientCfcCurveWidget;
    root->addWidget(m_curve, 1);

    auto* hint = new QLabel(tr("Drag a point to move its band. Double-click to add a band; "
                               "right-click a point to remove it."));
    hint->setWordWrap(true);
    root->addWidget(hint);

    auto* bandRow = new QHBoxLayout;
    bandRow->setSpacing(8);
    m_bandLabel = new QLabel;
    bandRow->addWidget(m_bandLabel);
    m_hz = makeSpin(ClientCfc::kMinHz, ClientCfc::kMaxHz, 10.0, 0, tr(" Hz"), tr("Band frequency"));
    m_comp = makeSpin(0.0, ClientCfc::kMaxCompDb, 0.5, 1, tr(" dB"), tr("Band compression"));
    m_eq = makeSpin(-ClientCfc::kMaxEqDb, ClientCfc::kMaxEqDb, 0.5, 1, tr(" dB"),
                    tr("Band post-EQ"));
    bandRow->addWidget(new QLabel(tr("Freq")));
    bandRow->addWidget(m_hz);
    bandRow->addWidget(new QLabel(tr("Comp")));
    bandRow->addWidget(m_comp);
    bandRow->addWidget(new QLabel(tr("EQ")));
    bandRow->addWidget(m_eq);
    bandRow->addStretch();
    root->addLayout(bandRow);

    auto* row = new QHBoxLayout;
    row->setSpacing(12);
    const auto makeKnob = [](const QString& label, float lo, float hi) {
        auto* k = new ClientCompKnob;
        k->setLabel(label);
        k->setCenterLabelMode(true);
        k->setFixedSize(76, 76);
        k->setRange(lo, hi);
        k->setDefault(0.0f);
        k->setValueFromNorm([lo, hi](float n) { return lo + n * (hi - lo); });
        k->setNormFromValue([lo, hi](float v) { return (v - lo) / (hi - lo); });
        k->setLabelFormat([](float v) { return QString::number(v, 'f', 1) + " dB"; });
        return k;
    };
    m_precomp = makeKnob(tr("Pre-comp"), 0.0f, ClientCfc::kMaxPrecompDb);
    m_postGain = makeKnob(tr("Post"), -ClientCfc::kMaxPostGainDb, ClientCfc::kMaxPostGainDb);
    row->addWidget(m_precomp);
    row->addWidget(m_postGain);

    m_postEq = new QPushButton(tr("Post-EQ"));
    m_postEq->setCheckable(true);
    ThemeManager::instance().applyStyleSheet(m_postEq, kToggleStyle);
    m_postEq->setToolTip(tr("Apply the post-EQ curve and the Post gain after compression"));
    row->addWidget(m_postEq);

    auto* fftColumn = new QVBoxLayout;
    m_fft = new QSlider(Qt::Horizontal);
    m_fft->setRange(0, static_cast<int>(kFftSizes.size()) - 1);
    m_fft->setPageStep(1);
    m_fft->setTickPosition(QSlider::TicksBelow);
    m_fft->setTickInterval(1);
    m_fft->setAccessibleName(tr("CFC FFT size"));
    m_fft->setToolTip(tr("Larger FFTs resolve frequency more finely and add more delay"));
    m_fftLabel = new QLabel;
    fftColumn->addWidget(m_fft);
    fftColumn->addWidget(m_fftLabel);
    row->addLayout(fftColumn, 1);
    root->addLayout(row);

    connect(m_curve, &ClientCfcCurveWidget::bandsEdited, this,
            [this](const std::vector<ClientCfc::Band>& bands) {
        if (auto* const cfc = m_audio ? m_audio->clientCfcTx() : nullptr) {
            cfc->setBands(bands);
            m_audio->saveClientCfcSettings();
        }
        showSelectedBand();
    });
    connect(m_curve, &ClientCfcCurveWidget::selectedBandChanged, this,
            [this](int) { showSelectedBand(); });
    for (QDoubleSpinBox* spin : {m_hz, m_comp, m_eq}) {
        connect(spin, &QDoubleSpinBox::valueChanged, this, [this](double) { editSelectedBand(); });
    }
    const auto wireKnob = [this](ClientCompKnob* knob, void (ClientCfc::*setter)(float)) {
        connect(knob, &ClientCompKnob::valueChanged, this, [this, setter](float v) {
            // Local object operands: see VfoWidget::buildTabContent (#6724).
            if (auto* const cfc = m_audio ? m_audio->clientCfcTx() : nullptr) {
                (cfc->*setter)(v);
                m_audio->saveClientCfcSettings();
            }
        });
    };
    wireKnob(m_precomp, &ClientCfc::setPrecompDb);
    wireKnob(m_postGain, &ClientCfc::setPostGainDb);
    connect(m_postEq, &QPushButton::toggled, this, [this](bool on) {
        if (auto* const cfc = m_audio ? m_audio->clientCfcTx() : nullptr) {
            cfc->setPostEqEnabled(on);
            m_audio->saveClientCfcSettings();
        }
        m_curve->setPostEqActive(on);
    });
    connect(m_fft, &QSlider::valueChanged, this, [this](int index) {
        const int size = kFftSizes[static_cast<std::size_t>(index)];
        updateFftLabel(size);
        if (auto* const cfc = m_audio ? m_audio->clientCfcTx() : nullptr) {
            cfc->setFftSize(size);
            m_audio->saveClientCfcSettings();
        }
    });

    m_poll = new QTimer(this);
    m_poll->setInterval(33);
    connect(m_poll, &QTimer::timeout, this, &StripCfcPanel::pollCompression);

    syncControlsFromEngine();
    if (m_audio) {
        connect(m_audio, &AudioEngine::clientCfcStateChanged, this,
                &StripCfcPanel::syncControlsFromEngine);
    }
}

StripCfcPanel::~StripCfcPanel()
{
    if (auto* const cfc = m_audio ? m_audio->clientCfcTx() : nullptr) {
        cfc->setCompressionDisplayEnabled(false);
    }
}

void StripCfcPanel::showForTx()
{
    syncControlsFromEngine();
    restoreGeometryFromSettings();
    show();
    raise();
    activateWindow();
}

void StripCfcPanel::syncControlsFromEngine()
{
    auto* const cfc = m_audio ? m_audio->clientCfcTx() : nullptr;
    if (!cfc) {
        return;
    }
    m_syncing = true;
    m_curve->setBands(cfc->bands());
    m_curve->setPostEqActive(cfc->postEqEnabled());
    {
        const QSignalBlocker b1(m_precomp);
        const QSignalBlocker b2(m_postGain);
        const QSignalBlocker b3(m_postEq);
        const QSignalBlocker b4(m_fft);
        m_precomp->setValue(cfc->precompDb());
        m_postGain->setValue(cfc->postGainDb());
        m_postEq->setChecked(cfc->postEqEnabled());
        m_fft->setValue(fftIndex(cfc->fftSize()));
    }
    // Adjustable either way, so it can be set before Post-EQ is turned on.
    const QString note = cfc->postEqEnabled()
        ? tr("Gain after the post-EQ")
        : tr("Gain after the post-EQ; takes effect when Post-EQ is on");
    m_postGain->setToolTip(note);
    m_postGain->setAccessibleDescription(note);
    updateFftLabel(cfc->fftSize());
    if (m_curve->selectedBand() < 0) {
        m_curve->setSelectedBand(0);
    }
    showSelectedBand();
    m_syncing = false;
}

void StripCfcPanel::showSelectedBand()
{
    const auto& bands = m_curve->bands();
    const int index = m_curve->selectedBand();
    const bool valid = index >= 0 && index < static_cast<int>(bands.size());
    const QSignalBlocker b1(m_hz);
    const QSignalBlocker b2(m_comp);
    const QSignalBlocker b3(m_eq);
    m_hz->setEnabled(valid);
    m_comp->setEnabled(valid);
    m_eq->setEnabled(valid);
    if (!valid) {
        m_bandLabel->setText(tr("No band selected"));
        return;
    }
    const ClientCfc::Band& b = bands[static_cast<std::size_t>(index)];
    m_bandLabel->setText(tr("Band %1 of %2").arg(index + 1).arg(bands.size()));
    m_hz->setValue(b.hz);
    m_comp->setValue(b.compDb);
    m_eq->setValue(b.eqDb);
}

void StripCfcPanel::editSelectedBand()
{
    auto* const cfc = m_audio ? m_audio->clientCfcTx() : nullptr;
    const int index = m_curve->selectedBand();
    auto bands = m_curve->bands();
    if (m_syncing || !cfc || index < 0 || index >= static_cast<int>(bands.size())) {
        return;
    }
    ClientCfc::Band& b = bands[static_cast<std::size_t>(index)];
    b.hz = static_cast<float>(m_hz->value());
    b.compDb = static_cast<float>(m_comp->value());
    b.eqDb = static_cast<float>(m_eq->value());
    cfc->setBands(bands);
    m_audio->saveClientCfcSettings();
}

void StripCfcPanel::pollCompression()
{
    auto* const cfc = m_audio ? m_audio->clientCfcTx() : nullptr;
    std::array<float, ClientCfc::kDisplayPoints> db{};
    if (cfc && cfc->isEnabled() && cfc->compressionDisplay(db)) {
        m_curve->setCompression(db);
    } else {
        m_curve->clearCompression();
    }
}

void StripCfcPanel::updateFftLabel(int fftSize)
{
    const double ms = fftSize * 1000.0 / 48000.0;
    m_fftLabel->setText(tr("FFT %1 \xc2\xb7 adds %2 ms").arg(fftSize).arg(std::lround(ms)));
}

void StripCfcPanel::saveGeometryToSettings()
{
    AppSettings::instance().setValue("StripCfcPanelGeometry", saveGeometry().toBase64());
}

void StripCfcPanel::restoreGeometryFromSettings()
{
    const QByteArray geom = QByteArray::fromBase64(
        AppSettings::instance().value("StripCfcPanelGeometry", "").toByteArray());
    if (!geom.isEmpty()) {
        m_restoring = true;
        restoreGeometry(geom);
        m_restoring = false;
    }
}

void StripCfcPanel::closeEvent(QCloseEvent* ev)
{
    saveGeometryToSettings();
    QWidget::closeEvent(ev);
}

void StripCfcPanel::moveEvent(QMoveEvent* ev)
{
    QWidget::moveEvent(ev);
    if (!m_restoring) {
        saveGeometryToSettings();
    }
}

void StripCfcPanel::resizeEvent(QResizeEvent* ev)
{
    QWidget::resizeEvent(ev);
    if (!m_restoring) {
        saveGeometryToSettings();
    }
}

void StripCfcPanel::showEvent(QShowEvent* ev)
{
    QWidget::showEvent(ev);
    if (auto* const cfc = m_audio ? m_audio->clientCfcTx() : nullptr) {
        cfc->setCompressionDisplayEnabled(true);
    }
    m_poll->start();
}

void StripCfcPanel::hideEvent(QHideEvent* ev)
{
    QWidget::hideEvent(ev);
    m_poll->stop();
    m_curve->clearCompression();
    if (auto* const cfc = m_audio ? m_audio->clientCfcTx() : nullptr) {
        cfc->setCompressionDisplayEnabled(false);
    }
}

}  // namespace AetherSDR
