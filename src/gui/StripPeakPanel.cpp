#include "StripPeakPanel.h"

#include "ClientCompKnob.h"
#include "ClientDbHistoryWidget.h"
#include "EditorFramelessTitleBar.h"
#include "core/AppSettings.h"
#include "core/AudioEngine.h"
#include "core/ClientCessb.h"
#include "core/ThemeManager.h"

#include <QCloseEvent>
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

#include <algorithm>
#include <cmath>

namespace AetherSDR {

namespace {

constexpr const char* kWindowStyle =
    "QWidget { background: {{color.background.0}}; color: {{color.text.primary}}; }"
    "QLabel  { background: transparent; color: {{color.text.secondary}}; font-size: 11px; }";

// A lit toggle, so CESSB on reads as on.
constexpr const char* kToggleStyle =
    "QPushButton {"
    "  background: {{color.background.1}}; border: 1px solid {{color.border.strong}};"
    "  border-radius: 3px; color: {{color.text.secondary}}; font-size: 12px;"
    "  font-weight: bold; padding: 4px 14px;"
    "}"
    "QPushButton:hover { background: {{color.background.2}}; }"
    "QPushButton:checked { color: {{color.accent}}; border: 1px solid {{color.accent}}; }";

ClientCompKnob* baseKnob(const QString& label, int size)
{
    auto* k = new ClientCompKnob;
    k->setLabel(label);
    k->setCenterLabelMode(true);
    k->setFixedSize(size, size);
    return k;
}

QString dbLabel(float v)
{
    return QString::number(v, 'f', 1) + " dB";
}

}  // namespace

namespace PeakKnobs {

ClientCompKnob* makeClip(int size)
{
    auto* k = baseKnob(QObject::tr("Clip"), size);
    constexpr float hi = ClientCessb::kMaxClipDb;
    k->setRange(0.0f, hi);
    k->setDefault(ClientCessb::kDefaultClipDb);
    k->setValueFromNorm([](float n) { return n * hi; });
    k->setNormFromValue([](float v) { return v / hi; });
    k->setLabelFormat(dbLabel);
    return k;
}

ClientCompKnob* makeCeiling(int size)
{
    auto* k = baseKnob(QObject::tr("Ceiling"), size);
    constexpr float lo = ClientCessb::kMinOutputDb;
    constexpr float hi = ClientCessb::kMaxOutputDb;
    k->setRange(lo, hi);
    k->setDefault(ClientCessb::kDefaultOutputDb);
    k->setValueFromNorm([](float n) { return lo + n * (hi - lo); });
    k->setNormFromValue([](float v) { return (v - lo) / (hi - lo); });
    k->setLabelFormat(dbLabel);
    return k;
}

}  // namespace PeakKnobs

StripPeakPanel::StripPeakPanel(AudioEngine* engine, QWidget* parent)
    : QWidget(parent)
    , m_audio(engine)
{
    const QString title = QString::fromUtf8("Aetherial Peak control \xe2\x80\x94 TX");
    setWindowTitle(title);
    ThemeManager::instance().applyStyleSheet(this, kWindowStyle);
    resize(520, 280);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(16, 0, 16, 14);
    root->setSpacing(8);

    auto* titleBar = new EditorFramelessTitleBar;
    titleBar->setTitleText(title);
    root->addWidget(titleBar);

    m_history = new ClientDbHistoryWidget;
    m_history->setAxis(kTraceFloorDb, 0.0f);
    m_history->setAccessibleName(tr("SSB envelope peak over the last five seconds"));
    root->addWidget(m_history, 1);

    auto* row = new QHBoxLayout;
    row->setSpacing(12);
    row->addStretch();
    m_clip = PeakKnobs::makeClip(76);
    m_clip->setToolTip(tr("How far the envelope is driven into the clipper. More clip, "
                          "more average power, more distortion."));
    m_clip->setAccessibleDescription(m_clip->toolTip());
    row->addWidget(m_clip);

    m_cessb = new QPushButton(tr("CESSB"));
    m_cessb->setObjectName(QStringLiteral("aetherTxPeakCessb"));
    m_cessb->setCheckable(true);
    ThemeManager::instance().applyStyleSheet(m_cessb, kToggleStyle);
    m_cessb->setToolTip(tr("Controlled-envelope SSB: removes the overshoot the bandpass "
                           "after the clipper puts back, so the ceiling holds."));
    m_cessb->setAccessibleDescription(m_cessb->toolTip());
    row->addWidget(m_cessb, 0, Qt::AlignVCenter);

    m_ceiling = PeakKnobs::makeCeiling(76);
    m_ceiling->setToolTip(tr("The largest SSB envelope this stage lets through, in dBFS"));
    m_ceiling->setAccessibleDescription(m_ceiling->toolTip());
    row->addWidget(m_ceiling);
    row->addStretch();
    root->addLayout(row);

    m_passband = new QLabel;
    m_passband->setObjectName(QStringLiteral("aetherTxPeakPassband"));
    m_passband->setAlignment(Qt::AlignCenter);
    root->addWidget(m_passband);

    const auto wire = [this](ClientCompKnob* knob, void (ClientCessb::*setter)(float)) {
        connect(knob, &ClientCompKnob::valueChanged, this, [this, setter](float v) {
            // Local object operands: see VfoWidget::buildTabContent (#6724).
            if (auto* const peak = m_audio ? m_audio->clientCessbTx() : nullptr) {
                (peak->*setter)(v);
                m_audio->saveClientCessbSettings();
            }
        });
    };
    wire(m_clip, &ClientCessb::setClipDb);
    wire(m_ceiling, &ClientCessb::setOutputDb);
    connect(m_cessb, &QPushButton::toggled, this, [this](bool on) {
        if (auto* const peak = m_audio ? m_audio->clientCessbTx() : nullptr) {
            peak->setOvershootControl(on);
            m_audio->saveClientCessbSettings();
        }
    });

    m_poll = new QTimer(this);
    m_poll->setInterval(33);
    connect(m_poll, &QTimer::timeout, this, [this]() {
        auto* const peak = m_audio ? m_audio->clientCessbTx() : nullptr;
        const bool on = peak && peak->isEnabled();
        m_history->setActive(on);
        if (on) {
            m_history->push(std::max(peak->takeEnvelopePeakDb(), kTraceFloorDb));
        }
    });

    syncControlsFromEngine();
    if (m_audio) {
        connect(m_audio, &AudioEngine::clientCessbStateChanged, this,
                &StripPeakPanel::syncControlsFromEngine);
    }
}

StripPeakPanel::~StripPeakPanel() = default;

void StripPeakPanel::showForTx()
{
    syncControlsFromEngine();
    restoreGeometryFromSettings();
    show();
    raise();
    activateWindow();
}

void StripPeakPanel::syncControlsFromEngine()
{
    auto* const peak = m_audio ? m_audio->clientCessbTx() : nullptr;
    if (!peak) {
        return;
    }
    const QSignalBlocker b1(m_clip);
    const QSignalBlocker b2(m_ceiling);
    const QSignalBlocker b3(m_cessb);
    m_clip->setValue(peak->clipDb());
    m_ceiling->setValue(peak->outputDb());
    m_cessb->setChecked(peak->overshootControl());
    m_history->setReference(peak->outputDb(), tr("Ceiling"));
    m_history->setActive(peak->isEnabled());
    m_passband->setText(tr("Passband %1\xe2\x80\x93%2 Hz, inside the radio TX filter")
                            .arg(std::lround(peak->lowHz()))
                            .arg(std::lround(peak->highHz())));
    m_passband->setAccessibleName(m_passband->text());
}

void StripPeakPanel::saveGeometryToSettings()
{
    AppSettings::instance().setValue("StripPeakPanelGeometry", saveGeometry().toBase64());
}

void StripPeakPanel::restoreGeometryFromSettings()
{
    const QByteArray geom = QByteArray::fromBase64(
        AppSettings::instance().value("StripPeakPanelGeometry", "").toByteArray());
    if (!geom.isEmpty()) {
        m_restoring = true;
        restoreGeometry(geom);
        m_restoring = false;
    }
}

void StripPeakPanel::closeEvent(QCloseEvent* ev)
{
    saveGeometryToSettings();
    QWidget::closeEvent(ev);
}

void StripPeakPanel::moveEvent(QMoveEvent* ev)
{
    QWidget::moveEvent(ev);
    if (!m_restoring) {
        saveGeometryToSettings();
    }
}

void StripPeakPanel::resizeEvent(QResizeEvent* ev)
{
    QWidget::resizeEvent(ev);
    if (!m_restoring) {
        saveGeometryToSettings();
    }
}

void StripPeakPanel::showEvent(QShowEvent* ev)
{
    QWidget::showEvent(ev);
    if (auto* const peak = m_audio ? m_audio->clientCessbTx() : nullptr) {
        peak->takeEnvelopePeakDb();  // drop the peak held while hidden
    }
    m_poll->start();
}

void StripPeakPanel::hideEvent(QHideEvent* ev)
{
    QWidget::hideEvent(ev);
    m_poll->stop();
    m_history->clear();
}

}  // namespace AetherSDR
