#include "StripLevelerPanel.h"

#include "ClientCompKnob.h"
#include "ClientLevelerHistoryWidget.h"
#include "EditorFramelessTitleBar.h"
#include "core/AppSettings.h"
#include "core/AudioEngine.h"
#include "core/ClientLeveler.h"
#include "core/ThemeManager.h"

#include <QCloseEvent>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QLabel>
#include <QMoveEvent>
#include <QResizeEvent>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>

#include <cmath>

namespace AetherSDR {

namespace {

constexpr const char* kWindowStyle =
    "QWidget { background: {{color.background.0}}; color: {{color.text.primary}}; }"
    "QLabel  { background: transparent; color: {{color.text.secondary}}; font-size: 11px; }";

ClientCompKnob* baseKnob(const QString& label, int size)
{
    auto* k = new ClientCompKnob;
    k->setLabel(label);
    k->setCenterLabelMode(true);
    k->setFixedSize(size, size);
    return k;
}

// The bottom 2% of the Hang knob reads Off; the rest is log 10..5000 ms.
constexpr float kHangOffNorm = 0.02f;

}  // namespace

namespace LevelerKnobs {

ClientCompKnob* makeMaxGain(int size)
{
    auto* k = baseKnob(QObject::tr("Max gain"), size);
    k->setRange(0.0f, ClientLeveler::kMaxGainDb);
    k->setDefault(ClientLeveler::kDefaultMaxGainDb);
    k->setValueFromNorm([](float n) { return n * ClientLeveler::kMaxGainDb; });
    k->setNormFromValue([](float v) { return v / ClientLeveler::kMaxGainDb; });
    k->setLabelFormat([](float v) { return QString::number(v, 'f', 1) + " dB"; });
    return k;
}

ClientCompKnob* makeAttack(int size)
{
    auto* k = baseKnob(QObject::tr("Attack"), size);
    k->setRange(ClientLeveler::kMinAttackMs, ClientLeveler::kMaxAttackMs);
    k->setDefault(ClientLeveler::kDefaultAttackMs);
    constexpr float lo = ClientLeveler::kMinAttackMs;
    constexpr float hi = ClientLeveler::kMaxAttackMs;
    k->setValueFromNorm([](float n) { return lo + n * (hi - lo); });
    k->setNormFromValue([](float v) { return (v - lo) / (hi - lo); });
    k->setLabelFormat([](float v) { return QString::number(v, 'f', 1) + " ms"; });
    return k;
}

ClientCompKnob* makeDecay(int size)
{
    auto* k = baseKnob(QObject::tr("Decay"), size);
    constexpr float lo = ClientLeveler::kMinDecayMs;
    constexpr float hi = ClientLeveler::kMaxDecayMs;
    k->setRange(lo, hi);
    k->setDefault(ClientLeveler::kDefaultDecayMs);
    k->setValueFromNorm([](float n) { return lo * std::pow(hi / lo, n); });
    k->setNormFromValue([](float v) {
        return v <= lo ? 0.0f : std::log(v / lo) / std::log(hi / lo);
    });
    k->setLabelFormat([](float v) { return QString::number(std::lround(v)) + " ms"; });
    return k;
}

ClientCompKnob* makeHang(int size)
{
    auto* k = baseKnob(QObject::tr("Hang"), size);
    constexpr float lo = ClientLeveler::kMinHangMs;
    constexpr float hi = ClientLeveler::kMaxHangMs;
    k->setRange(0.0f, hi);
    k->setDefault(0.0f);
    k->setValueFromNorm([](float n) {
        return n < kHangOffNorm ? 0.0f
                                : lo * std::pow(hi / lo, (n - kHangOffNorm) / (1.0f - kHangOffNorm));
    });
    k->setNormFromValue([](float v) {
        return v < lo ? 0.0f
                      : kHangOffNorm + (1.0f - kHangOffNorm) * std::log(v / lo) / std::log(hi / lo);
    });
    k->setLabelFormat([](float v) {
        return v < lo ? QObject::tr("Off") : QString::number(std::lround(v)) + " ms";
    });
    return k;
}

}  // namespace LevelerKnobs

StripLevelerPanel::StripLevelerPanel(AudioEngine* engine, QWidget* parent)
    : QWidget(parent)
    , m_audio(engine)
{
    const QString title = QString::fromUtf8("Aetherial LVLR \xe2\x80\x94 TX");
    setWindowTitle(title);
    ThemeManager::instance().applyStyleSheet(this, kWindowStyle);
    resize(520, 260);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(16, 0, 16, 14);
    root->setSpacing(8);

    auto* titleBar = new EditorFramelessTitleBar;
    titleBar->setTitleText(title);
    root->addWidget(titleBar);

    m_history = new ClientLevelerHistoryWidget;
    root->addWidget(m_history, 1);

    auto* row = new QHBoxLayout;
    row->setSpacing(12);
    row->addStretch();
    m_maxGain = LevelerKnobs::makeMaxGain(76);
    m_attack = LevelerKnobs::makeAttack(76);
    m_decay = LevelerKnobs::makeDecay(76);
    m_hang = LevelerKnobs::makeHang(76);
    m_attack->setToolTip(tr("Also the look-ahead: the delay LVLR adds grows with it"));
    m_attack->setAccessibleDescription(m_attack->toolTip());
    for (ClientCompKnob* k : {m_maxGain, m_attack, m_decay, m_hang}) {
        row->addWidget(k);
    }
    row->addStretch();
    root->addLayout(row);

    const auto wire = [this](ClientCompKnob* knob, void (ClientLeveler::*setter)(float)) {
        connect(knob, &ClientCompKnob::valueChanged, this, [this, setter](float v) {
            // Local object operands: see VfoWidget::buildTabContent (#6724).
            if (auto* const lev = m_audio ? m_audio->clientLevelerTx() : nullptr) {
                (lev->*setter)(v);
                m_audio->saveClientLevelerSettings();
            }
        });
    };
    wire(m_maxGain, &ClientLeveler::setMaxGainDb);
    wire(m_attack, &ClientLeveler::setAttackMs);
    wire(m_decay, &ClientLeveler::setDecayMs);
    wire(m_hang, &ClientLeveler::setHangMs);

    m_poll = new QTimer(this);
    m_poll->setInterval(33);
    connect(m_poll, &QTimer::timeout, this, [this]() {
        auto* const lev = m_audio ? m_audio->clientLevelerTx() : nullptr;
        const bool on = lev && lev->isEnabled();
        m_history->setActive(on);
        if (on) {
            m_history->push(lev->gainDb());
        }
    });

    syncControlsFromEngine();
    if (m_audio) {
        connect(m_audio, &AudioEngine::clientLevelerStateChanged, this,
                &StripLevelerPanel::syncControlsFromEngine);
    }
}

StripLevelerPanel::~StripLevelerPanel() = default;

void StripLevelerPanel::showForTx()
{
    syncControlsFromEngine();
    restoreGeometryFromSettings();
    show();
    raise();
    activateWindow();
}

void StripLevelerPanel::syncControlsFromEngine()
{
    auto* const lev = m_audio ? m_audio->clientLevelerTx() : nullptr;
    if (!lev) {
        return;
    }
    const QSignalBlocker b1(m_maxGain);
    const QSignalBlocker b2(m_attack);
    const QSignalBlocker b3(m_decay);
    const QSignalBlocker b4(m_hang);
    m_maxGain->setValue(lev->maxGainDb());
    m_attack->setValue(lev->attackMs());
    m_decay->setValue(lev->decayMs());
    m_hang->setValue(lev->hangMs());
    m_history->setMaxGainDb(lev->maxGainDb());
    m_history->setActive(lev->isEnabled());
}

void StripLevelerPanel::saveGeometryToSettings()
{
    AppSettings::instance().setValue("StripLevelerPanelGeometry", saveGeometry().toBase64());
}

void StripLevelerPanel::restoreGeometryFromSettings()
{
    const QByteArray geom = QByteArray::fromBase64(
        AppSettings::instance().value("StripLevelerPanelGeometry", "").toByteArray());
    if (!geom.isEmpty()) {
        m_restoring = true;
        restoreGeometry(geom);
        m_restoring = false;
    }
}

void StripLevelerPanel::closeEvent(QCloseEvent* ev)
{
    saveGeometryToSettings();
    QWidget::closeEvent(ev);
}

void StripLevelerPanel::moveEvent(QMoveEvent* ev)
{
    QWidget::moveEvent(ev);
    if (!m_restoring) {
        saveGeometryToSettings();
    }
}

void StripLevelerPanel::resizeEvent(QResizeEvent* ev)
{
    QWidget::resizeEvent(ev);
    if (!m_restoring) {
        saveGeometryToSettings();
    }
}

void StripLevelerPanel::showEvent(QShowEvent* ev)
{
    QWidget::showEvent(ev);
    m_poll->start();
}

void StripLevelerPanel::hideEvent(QHideEvent* ev)
{
    QWidget::hideEvent(ev);
    m_poll->stop();
    m_history->clear();
}

}  // namespace AetherSDR
