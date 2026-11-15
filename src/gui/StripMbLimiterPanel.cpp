#include "StripMbLimiterPanel.h"

#include "ClientCompKnob.h"
#include "EditorFramelessTitleBar.h"
#include "core/AppSettings.h"
#include "core/AudioEngine.h"
#include "core/ClientMbLimiter.h"
#include "core/ThemeManager.h"

#include <QCloseEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QLabel>
#include <QMoveEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace AetherSDR {

// A band's gain reduction: a bar that grows down from the top, 0 to 24 dB,
// with the peak since the last poll.
class MbLimiterGrBar : public QWidget {
public:
    explicit MbLimiterGrBar(QWidget* parent = nullptr) : QWidget(parent)
    {
        setFixedSize(14, 140);
    }
    void setReductionDb(float db)
    {
        m_db = std::clamp(-db, 0.0f, kRangeDb);
        update();
    }
    void setActive(bool on)
    {
        m_active = on;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        auto& theme = ThemeManager::instance();
        p.fillRect(rect(), theme.color(this, "color.background.1"));
        p.setPen(theme.color(this, "color.border.strong"));
        p.drawRect(rect().adjusted(0, 0, -1, -1));
        if (m_active && m_db > 0.0f) {
            const int h = static_cast<int>(std::lround(m_db / kRangeDb * (height() - 2)));
            p.fillRect(QRect(1, 1, width() - 2, h), theme.color(this, "color.meter.gainReduction"));
        }
    }

private:
    static constexpr float kRangeDb = 24.0f;
    float m_db{0.0f};
    bool m_active{false};
};

namespace {

constexpr const char* kWindowStyle =
    "QWidget { background: {{color.background.0}}; color: {{color.text.primary}}; }"
    "QLabel  { background: transparent; color: {{color.text.secondary}}; font-size: 11px; }";

ClientCompKnob* knob(const QString& label, int size, float lo, float hi, float def, bool logScale,
                     const QString& unit, int decimals)
{
    auto* k = new ClientCompKnob;
    k->setLabel(label);
    k->setCenterLabelMode(true);
    k->setFixedSize(size, size);
    k->setRange(lo, hi);
    k->setDefault(def);
    if (logScale) {
        k->setValueFromNorm([lo, hi](float n) { return lo * std::pow(hi / lo, n); });
        k->setNormFromValue([lo, hi](float v) {
            return v <= lo ? 0.0f : std::log(v / lo) / std::log(hi / lo);
        });
    } else {
        k->setValueFromNorm([lo, hi](float n) { return lo + n * (hi - lo); });
        k->setNormFromValue([lo, hi](float v) { return (v - lo) / (hi - lo); });
    }
    k->setLabelFormat([unit, decimals](float v) {
        if (unit == QLatin1String("Hz") && v >= 1000.0f) {
            return QString::number(v / 1000.0f, 'f', 2) + QStringLiteral(" kHz");
        }
        return QString::number(v, 'f', decimals) + QLatin1Char(' ') + unit;
    });
    return k;
}

const char* bandName(int band)
{
    static const char* names[] = {QT_TRANSLATE_NOOP("StripMbLimiterPanel", "Low"),
                                  QT_TRANSLATE_NOOP("StripMbLimiterPanel", "Low mid"),
                                  QT_TRANSLATE_NOOP("StripMbLimiterPanel", "High mid"),
                                  QT_TRANSLATE_NOOP("StripMbLimiterPanel", "High")};
    return names[band];
}

}  // namespace

StripMbLimiterPanel::StripMbLimiterPanel(AudioEngine* engine, QWidget* parent)
    : QWidget(parent)
    , m_audio(engine)
{
    const QString title = QString::fromUtf8("Aetherial MB limiter \xe2\x80\x94 TX");
    setWindowTitle(title);
    ThemeManager::instance().applyStyleSheet(this, kWindowStyle);
    resize(560, 360);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(16, 0, 16, 14);
    root->setSpacing(10);

    auto* titleBar = new EditorFramelessTitleBar;
    titleBar->setTitleText(title);
    root->addWidget(titleBar);

    // Crossovers.
    auto* xRow = new QHBoxLayout;
    xRow->setSpacing(12);
    xRow->addStretch();
    for (int i = 0; i < kCrossovers; ++i) {
        const auto u = static_cast<std::size_t>(i);
        m_crossover[u] = knob(tr("X%1").arg(i + 1), 76, ClientMbLimiter::kMinCrossoverHz[u],
                              ClientMbLimiter::kMaxCrossoverHz[u],
                              ClientMbLimiter::kDefaultCrossoverHz[u], true, QStringLiteral("Hz"), 0);
        m_crossover[u]->setToolTip(tr("Crossover %1: where band %1 ends and band %2 begins")
                                       .arg(i + 1)
                                       .arg(i + 2));
        m_crossover[u]->setAccessibleDescription(m_crossover[u]->toolTip());
        xRow->addWidget(m_crossover[u]);
    }
    xRow->addStretch();
    root->addLayout(xRow);

    // A column per band.
    auto* grid = new QGridLayout;
    grid->setHorizontalSpacing(18);
    grid->setVerticalSpacing(6);
    for (int b = 0; b < kBands; ++b) {
        const auto u = static_cast<std::size_t>(b);
        auto* name = new QLabel(tr(bandName(b)));
        name->setAlignment(Qt::AlignCenter);
        m_bandRange[u] = new QLabel;
        m_bandRange[u]->setAlignment(Qt::AlignCenter);
        m_gr[u] = new MbLimiterGrBar;
        m_gr[u]->setAccessibleName(tr("%1 band gain reduction").arg(tr(bandName(b))));
        m_threshold[u] = knob(tr("Threshold"), 76, ClientMbLimiter::kMinThresholdDb,
                              ClientMbLimiter::kMaxThresholdDb, ClientMbLimiter::kDefaultThresholdDb,
                              false, QStringLiteral("dB"), 1);
        m_release[u] = knob(tr("Release"), 76, ClientMbLimiter::kMinReleaseMs,
                            ClientMbLimiter::kMaxReleaseMs, ClientMbLimiter::kDefaultReleaseMs, true,
                            QStringLiteral("ms"), 0);
        m_threshold[u]->setAccessibleName(tr("%1 band threshold").arg(tr(bandName(b))));
        m_release[u]->setAccessibleName(tr("%1 band release").arg(tr(bandName(b))));
        grid->addWidget(name, 0, b, Qt::AlignHCenter);
        grid->addWidget(m_bandRange[u], 1, b, Qt::AlignHCenter);
        grid->addWidget(m_gr[u], 2, b, Qt::AlignHCenter);
        grid->addWidget(m_threshold[u], 3, b, Qt::AlignHCenter);
        grid->addWidget(m_release[u], 4, b, Qt::AlignHCenter);
    }
    root->addLayout(grid);
    root->addStretch(1);

    const auto save = [this]() {
        if (m_audio) {
            m_audio->saveClientMbLimiterSettings();
        }
    };
    for (int i = 0; i < kCrossovers; ++i) {
        connect(m_crossover[static_cast<std::size_t>(i)], &ClientCompKnob::valueChanged, this,
                [this, i, save](float v) {
                    // Local object operands: see VfoWidget::buildTabContent (#6724).
                    if (auto* const l = m_audio ? m_audio->clientMbLimiterTx() : nullptr) {
                        l->setCrossoverHz(i, v);
                        save();
                    }
                });
    }
    for (int b = 0; b < kBands; ++b) {
        connect(m_threshold[static_cast<std::size_t>(b)], &ClientCompKnob::valueChanged, this,
                [this, b, save](float v) {
                    if (auto* const l = m_audio ? m_audio->clientMbLimiterTx() : nullptr) {
                        l->setThresholdDb(b, v);
                        save();
                    }
                });
        connect(m_release[static_cast<std::size_t>(b)], &ClientCompKnob::valueChanged, this,
                [this, b, save](float v) {
                    if (auto* const l = m_audio ? m_audio->clientMbLimiterTx() : nullptr) {
                        l->setReleaseMs(b, v);
                        save();
                    }
                });
    }

    m_poll = new QTimer(this);
    m_poll->setInterval(33);
    connect(m_poll, &QTimer::timeout, this, [this]() {
        auto* const l = m_audio ? m_audio->clientMbLimiterTx() : nullptr;
        const bool on = l && l->isEnabled();
        for (int b = 0; b < kBands; ++b) {
            auto* bar = m_gr[static_cast<std::size_t>(b)];
            bar->setActive(on);
            bar->setReductionDb(on ? l->takeGainReductionDb(b) : 0.0f);
        }
    });

    syncControlsFromEngine();
    if (m_audio) {
        connect(m_audio, &AudioEngine::clientMbLimiterStateChanged, this,
                &StripMbLimiterPanel::syncControlsFromEngine);
    }
}

StripMbLimiterPanel::~StripMbLimiterPanel() = default;

void StripMbLimiterPanel::showForTx()
{
    syncControlsFromEngine();
    restoreGeometryFromSettings();
    show();
    raise();
    activateWindow();
}

void StripMbLimiterPanel::syncControlsFromEngine()
{
    auto* const l = m_audio ? m_audio->clientMbLimiterTx() : nullptr;
    if (!l) {
        return;
    }
    for (int i = 0; i < kCrossovers; ++i) {
        auto* k = m_crossover[static_cast<std::size_t>(i)];
        const QSignalBlocker block(k);
        k->setValue(l->crossoverHz(i));
    }
    for (int b = 0; b < kBands; ++b) {
        const auto u = static_cast<std::size_t>(b);
        const QSignalBlocker b1(m_threshold[u]);
        const QSignalBlocker b2(m_release[u]);
        m_threshold[u]->setValue(l->thresholdDb(b));
        m_release[u]->setValue(l->releaseMs(b));
        const int lo = b == 0 ? 0 : static_cast<int>(std::lround(l->crossoverHz(b - 1)));
        const QString range = b == kBands - 1
                                  ? tr("above %1 Hz").arg(lo)
                                  : b == 0 ? tr("below %1 Hz").arg(std::lround(l->crossoverHz(0)))
                                           : tr("%1\xe2\x80\x93%2 Hz")
                                                 .arg(lo)
                                                 .arg(std::lround(l->crossoverHz(b)));
        m_bandRange[u]->setText(range);
        m_gr[u]->setActive(l->isEnabled());
    }
}

void StripMbLimiterPanel::saveGeometryToSettings()
{
    AppSettings::instance().setValue("StripMbLimiterPanelGeometry", saveGeometry().toBase64());
}

void StripMbLimiterPanel::restoreGeometryFromSettings()
{
    const QByteArray geom = QByteArray::fromBase64(
        AppSettings::instance().value("StripMbLimiterPanelGeometry", "").toByteArray());
    if (!geom.isEmpty()) {
        m_restoring = true;
        restoreGeometry(geom);
        m_restoring = false;
    }
}

void StripMbLimiterPanel::closeEvent(QCloseEvent* ev)
{
    saveGeometryToSettings();
    QWidget::closeEvent(ev);
}

void StripMbLimiterPanel::moveEvent(QMoveEvent* ev)
{
    QWidget::moveEvent(ev);
    if (!m_restoring) {
        saveGeometryToSettings();
    }
}

void StripMbLimiterPanel::resizeEvent(QResizeEvent* ev)
{
    QWidget::resizeEvent(ev);
    if (!m_restoring) {
        saveGeometryToSettings();
    }
}

void StripMbLimiterPanel::showEvent(QShowEvent* ev)
{
    QWidget::showEvent(ev);
    if (auto* const l = m_audio ? m_audio->clientMbLimiterTx() : nullptr) {
        for (int b = 0; b < kBands; ++b) {
            l->takeGainReductionDb(b);  // drop what was held while hidden
        }
    }
    m_poll->start();
}

void StripMbLimiterPanel::hideEvent(QHideEvent* ev)
{
    QWidget::hideEvent(ev);
    m_poll->stop();
}

}  // namespace AetherSDR
