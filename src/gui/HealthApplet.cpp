#include "HealthApplet.h"

#include "core/ThemeManager.h"
#include "models/MeterModel.h"

#include <QDateTime>
#include <QEvent>
#include <QHideEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QShowEvent>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <limits>

namespace AetherSDR {

namespace {

constexpr int kTickMs = 50;
constexpr int kMaxHistory = 220;
constexpr int kRecentWindow = 70;
constexpr qint64 kFreshMs = 900;
constexpr qint64 kStaleMs = 1700;
constexpr float kActivePowerWatts = 0.75f;
constexpr int kSwrSettleFrames = 4;
constexpr int kSwrAdmissionWindow = 5;
// Frames of sustained inactivity or a settled source change before the SWR
// admission quorum is wiped. Matches the idle-baseline dropout tolerance
// (~1.2s) so a single dropped frame or a transient bestSnapshot() source flip
// cannot trap HLTH in "never trusted" and mask a real fault.
constexpr int kSwrDropoutFrames = 24;
constexpr float kMaxReturnLossDb = 45.0f;

enum class GraphMetric {
    Swr,
    ReturnLoss,
    Power
};

float clamp01(float v)
{
    return std::clamp(v, 0.0f, 1.0f);
}

float smoothStep(float edge0, float edge1, float x)
{
    if (edge1 <= edge0)
        return x >= edge1 ? 1.0f : 0.0f;
    const float t = clamp01((x - edge0) / (edge1 - edge0));
    return t * t * (3.0f - 2.0f * t);
}

float finiteOr(float value, float fallback)
{
    return std::isfinite(value) ? value : fallback;
}

float returnLossDbForSwr(float swr)
{
    const float clampedSwr = std::clamp(finiteOr(swr, 1.0f), 1.0f, 99.0f);
    if (clampedSwr <= 1.0005f)
        return kMaxReturnLossDb;

    const float reflectionCoefficient = (clampedSwr - 1.0f) / (clampedSwr + 1.0f);
    if (reflectionCoefficient <= 0.00001f)
        return kMaxReturnLossDb;

    return std::clamp(-20.0f * std::log10(reflectionCoefficient), 0.0f, kMaxReturnLossDb);
}

float admittedSwr(const QVector<float>& values)
{
    if (values.isEmpty()) {
        return 1.0f;
    }

    QVector<float> scratch = values;
    const int index = (scratch.size() - 1) / 4;
    // Low-power directional measurements fail upward as reflected power
    // approaches the radio's noise floor.  The lower quartile requires four
    // of five distinct reports to be elevated before HLTH treats the rise as
    // real, while a sustained mismatch still passes through within 250 ms.
    // nth_element selects the quartile in O(n) without a full sort.
    std::nth_element(scratch.begin(), scratch.begin() + index, scratch.end());
    return scratch.at(index);
}

QColor themeColor(const QString& token, int alpha = 255)
{
    QColor c = ThemeManager::instance().color(token);
    c.setAlpha(alpha);
    return c;
}

// Template builders — return unresolved {{token}} strings so callers can
// route them through ThemeManager::applyStyleSheet() (which registers the
// widget in the inspector reverse-map for Phase 5 token discovery and
// handles live re-theme on theme changes).  Matches the project-wide
// pattern PR #3144 swept across the rest of the codebase.
QString labelStyleTemplate(const QString& colorToken, int px = 10, bool bold = false)
{
    return QStringLiteral(
        "QLabel { color: {{%1}}; font-size: %2px; %3 }")
        .arg(colorToken)
        .arg(px)
        .arg(bold ? QStringLiteral("font-weight: bold;") : QString());
}

QString pillStyleTemplate(const QString& bgToken, const QString& borderToken,
                          const QString& fgToken)
{
    return QStringLiteral(
        "QLabel { background: {{%1}}; border: 1px solid {{%2}}; border-radius: 3px; "
        "color: {{%3}}; font-size: 10px; font-weight: bold; padding: 1px 5px; }")
        .arg(bgToken, borderToken, fgToken);
}

void applyLabelStyle(QWidget* w, const QString& colorToken,
                     int px = 10, bool bold = false)
{
    ThemeManager::instance().applyStyleSheet(
        w, labelStyleTemplate(colorToken, px, bold));
}

void applyPillStyle(QWidget* w, const QString& bgToken,
                    const QString& borderToken, const QString& fgToken)
{
    ThemeManager::instance().applyStyleSheet(
        w, pillStyleTemplate(bgToken, borderToken, fgToken));
}

QRectF laneRect(const QRectF& graphArea, int lane)
{
    constexpr int laneCount = 3;
    const qreal gap = 4.0;
    const qreal laneHeight = (graphArea.height() - gap * (laneCount - 1)) / laneCount;
    return QRectF(graphArea.left(), graphArea.top() + lane * (laneHeight + gap),
                  graphArea.width(), laneHeight);
}

} // namespace

class HealthGraphWidget : public QWidget {
public:
    explicit HealthGraphWidget(QWidget* parent = nullptr)
        : QWidget(parent)
    {
        setMinimumHeight(126);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
                this, [this]() { update(); });
    }

    void setSamples(const QVector<AntennaHealthSample>& samples)
    {
        m_samples = samples;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);

        const QRectF r = rect().adjusted(1, 1, -1, -1);
        p.setPen(QPen(themeColor(QStringLiteral("color.border.strong")), 1));
        p.setBrush(themeColor(QStringLiteral("color.background.0")));
        p.drawRoundedRect(r, 5, 5);

        const QRectF area = r.adjusted(5, 5, -5, -5);
        const QRectF swrLane = laneRect(area, 0);
        const QRectF returnLossLane = laneRect(area, 1);
        const QRectF powerLane = laneRect(area, 2);

        drawGrid(p, swrLane, QStringLiteral("SWR"));
        drawGrid(p, returnLossLane, QStringLiteral("RL"));
        drawGrid(p, powerLane, QStringLiteral("PWR"));

        if (m_samples.size() < 2) {
            p.setPen(themeColor(QStringLiteral("color.text.secondary")));
            p.drawText(area, Qt::AlignCenter, QStringLiteral("RF IDLE"));
            return;
        }

        drawSeverityBands(p, area);
        drawIncidentMarkers(p, area);
        drawTrace(p, swrLane, GraphMetric::Swr,
                  themeColor(QStringLiteral("color.accent.success")));
        drawTrace(p, returnLossLane, GraphMetric::ReturnLoss,
                  themeColor(QStringLiteral("color.accent.warning")));
        drawTrace(p, powerLane, GraphMetric::Power,
                  themeColor(QStringLiteral("color.accent")));
    }

private:
    qreal xForIndex(int i, const QRectF& area) const
    {
        const int denom = std::max(1, kMaxHistory - 1);
        return area.right() - static_cast<qreal>(m_samples.size() - 1 - i)
            * area.width() / static_cast<qreal>(denom);
    }

    void drawGrid(QPainter& p, const QRectF& lane, const QString& title)
    {
        p.setPen(QPen(themeColor(QStringLiteral("color.border.subtle")), 1));
        p.drawLine(QPointF(lane.left(), lane.center().y()),
                   QPointF(lane.right(), lane.center().y()));
        p.setPen(QPen(themeColor(QStringLiteral("color.background.2")), 1, Qt::DashLine));
        p.drawLine(QPointF(lane.left(), lane.top() + lane.height() * 0.22),
                   QPointF(lane.right(), lane.top() + lane.height() * 0.22));
        p.drawLine(QPointF(lane.left(), lane.bottom() - lane.height() * 0.22),
                   QPointF(lane.right(), lane.bottom() - lane.height() * 0.22));
        p.setPen(themeColor(QStringLiteral("color.text.secondary")));
        QFont f = p.font();
        f.setPixelSize(9);
        f.setBold(true);
        p.setFont(f);
        p.drawText(lane.adjusted(3, 0, -3, 0), Qt::AlignLeft | Qt::AlignTop, title);
    }

    void drawSeverityBands(QPainter& p, const QRectF& area)
    {
        for (int i = 1; i < m_samples.size(); ++i) {
            const auto& s = m_samples[i];
            if (!s.active || s.severity < 0.34f)
                continue;

            const qreal x0 = xForIndex(i - 1, area);
            const qreal x1 = xForIndex(i, area);
            const int alpha = static_cast<int>(45 + clamp01(s.severity) * 92);
            const QColor c = s.severity > 0.72f
                ? themeColor(QStringLiteral("color.accent.danger"), alpha)
                : themeColor(QStringLiteral("color.accent.warning"), alpha);
            p.fillRect(QRectF(std::min(x0, x1), area.top(),
                              std::abs(x1 - x0) + 1.0, area.height()), c);
        }
    }

    void drawIncidentMarkers(QPainter& p, const QRectF& area)
    {
        QFont f = p.font();
        f.setPixelSize(8);
        f.setBold(true);
        p.setFont(f);

        for (int i = 0; i < m_samples.size(); ++i) {
            const auto& s = m_samples[i];
            if (!s.incident)
                continue;

            const qreal x = xForIndex(i, area);
            if (x < area.left() || x > area.right())
                continue;

            const bool critical = s.incidentSeverity >= 0.72f;
            QColor c = critical
                ? themeColor(QStringLiteral("color.accent.danger"))
                : themeColor(QStringLiteral("color.accent.warning"));
            p.setPen(QPen(c, critical ? 1.8 : 1.3));
            p.drawLine(QPointF(x, area.top() + 1.0), QPointF(x, area.bottom() - 1.0));
            p.setBrush(c);
            p.drawEllipse(QPointF(x, area.top() + 3.5), 2.6, 2.6);

            const QColor textBg = themeColor(QStringLiteral("color.background.0"), 215);
            const qreal labelX = std::clamp(x + 3.0, area.left(), area.right() - 22.0);
            const QRectF labelRect(labelX, area.top(), 22.0, 11.0);
            p.setPen(Qt::NoPen);
            p.setBrush(textBg);
            p.drawRoundedRect(labelRect, 2, 2);
            p.setPen(c);
            p.drawText(labelRect.adjusted(2, 0, -2, 0),
                       Qt::AlignCenter, s.incidentLabel.left(4));
        }
    }

    qreal yForSample(const AntennaHealthSample& s, const QRectF& lane,
                     GraphMetric metric) const
    {
        float value = s.powerWatts;
        float average = s.powerAverage;
        float spread = s.powerSpread;
        float minimumScale = 3.0f;
        float averageScale = 0.08f;

        switch (metric) {
        case GraphMetric::Swr:
            value = s.swr;
            average = s.swrAverage;
            spread = s.swrSpread;
            minimumScale = 0.06f;
            averageScale = 0.035f;
            break;
        case GraphMetric::ReturnLoss:
            value = s.returnLossDb;
            average = s.returnLossAverage;
            spread = s.returnLossSpread;
            minimumScale = 1.2f;
            averageScale = 0.045f;
            break;
        case GraphMetric::Power:
            break;
        }

        const float scale = std::max({minimumScale, spread * 2.25f, average * averageScale});
        const float normalized = std::clamp((value - average) / scale, -1.0f, 1.0f);
        return lane.center().y() - normalized * lane.height() * 0.42;
    }

    void drawTrace(QPainter& p, const QRectF& lane, GraphMetric metric, const QColor& color)
    {
        QPainterPath path;
        bool started = false;
        const QRectF fullArea = rect().adjusted(6, 6, -6, -6);

        for (int i = 0; i < m_samples.size(); ++i) {
            const auto& s = m_samples[i];
            const qreal x = xForIndex(i, fullArea);
            if (x < lane.left() - 2.0 || x > lane.right() + 2.0)
                continue;
            const qreal y = yForSample(s, lane, metric);
            if (!started) {
                path.moveTo(x, y);
                started = true;
            } else {
                path.lineTo(x, y);
            }
        }

        if (!started)
            return;

        QColor glow = color;
        glow.setAlpha(60);
        p.setPen(QPen(glow, 4.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawPath(path);

        p.setPen(QPen(color, 1.55, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawPath(path);

        p.setPen(QPen(themeColor(QStringLiteral("color.meter.peak"), 82), 1, Qt::DashLine));
        p.drawLine(QPointF(lane.left(), lane.center().y()),
                   QPointF(lane.right(), lane.center().y()));
    }

    QVector<AntennaHealthSample> m_samples;
};

HealthApplet::HealthApplet(QWidget* parent)
    : QWidget(parent)
{
    theme::setContainer(this, QStringLiteral("applet/hlth"));
    hide();
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    auto* body = new QWidget(this);
    auto* vbox = new QVBoxLayout(body);
    vbox->setContentsMargins(4, 3, 4, 4);
    vbox->setSpacing(4);

    auto* topRow = new QHBoxLayout;
    topRow->setSpacing(4);

    m_statusLabel = new QLabel(QStringLiteral("IDLE"), body);
    m_statusLabel->setAlignment(Qt::AlignCenter);
    m_statusLabel->setMinimumWidth(58);
    m_statusLabel->setToolTip(QStringLiteral("Overall antenna health state. Click HLTH to pause or resume the graph."));
    topRow->addWidget(m_statusLabel);

    m_sourceLabel = new QLabel(QStringLiteral("SRC --"), body);
    m_sourceLabel->setToolTip(QStringLiteral("Current meter source: AMP, TUN, or RAD. HLTH uses the freshest active source."));
    topRow->addWidget(m_sourceLabel);
    topRow->addStretch();

    m_scoreLabel = new QLabel(QStringLiteral("--"), body);
    m_scoreLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_scoreLabel->setMinimumWidth(28);
    m_scoreLabel->setToolTip(QStringLiteral("Health score from 0 to 100 based on SWR, return loss, variance, and power sag."));
    topRow->addWidget(m_scoreLabel);

    vbox->addLayout(topRow);

    m_graph = new HealthGraphWidget(body);
    m_graph->setToolTip(QStringLiteral("Scrolling SWR, return loss, and power trends centered around moving averages. Incident markers show notable changes."));
    vbox->addWidget(m_graph);

    auto* valueRow = new QHBoxLayout;
    valueRow->setSpacing(5);
    m_swrLabel = new QLabel(QStringLiteral("SWR 1.00"), body);
    m_swrLabel->setToolTip(QStringLiteral("Standing wave ratio from the active RF meter source."));
    m_returnLossLabel = new QLabel(QStringLiteral("RL --"), body);
    m_returnLossLabel->setToolTip(QStringLiteral("Return loss in dB calculated from SWR. Higher is better."));
    m_powerLabel = new QLabel(QStringLiteral("PWR 0 W"), body);
    m_powerLabel->setToolTip(QStringLiteral("Smoothed forward output power from the active RF meter source."));
    m_varianceLabel = new QLabel(QStringLiteral("VAR --"), body);
    m_varianceLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_varianceLabel->setToolTip(QStringLiteral("Recent SWR span. Broad variance can point to grounding or counterpoise problems."));
    valueRow->addWidget(m_swrLabel);
    valueRow->addWidget(m_returnLossLabel);
    valueRow->addWidget(m_powerLabel);
    valueRow->addStretch();
    valueRow->addWidget(m_varianceLabel);
    vbox->addLayout(valueRow);

    outer->addWidget(body);

    setAccessibleName(QStringLiteral("Antenna health"));
    setAccessibleDescription(QStringLiteral("Live antenna health trend for SWR, return loss, and output power"));
    setCursor(Qt::PointingHandCursor);
    setToolTip(QStringLiteral("Click to pause or resume HLTH graph"));
    body->installEventFilter(this);
    m_graph->installEventFilter(this);
    m_statusLabel->installEventFilter(this);
    m_sourceLabel->installEventFilter(this);
    m_scoreLabel->installEventFilter(this);
    m_swrLabel->installEventFilter(this);
    m_returnLossLabel->installEventFilter(this);
    m_powerLabel->installEventFilter(this);
    m_varianceLabel->installEventFilter(this);

    m_tickTimer = new QTimer(this);
    m_tickTimer->setInterval(kTickMs);
    connect(m_tickTimer, &QTimer::timeout, this, &HealthApplet::appendFrame);
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
            this, &HealthApplet::applyTheme);
    applyTheme();
}

bool HealthApplet::eventFilter(QObject*, QEvent* event)
{
    if (event->type() == QEvent::MouseButtonRelease) {
        auto* mouseEvent = static_cast<QMouseEvent*>(event);
        if (mouseEvent->button() == Qt::LeftButton) {
            togglePaused();
            return true;
        }
    }

    return false;
}

void HealthApplet::hideEvent(QHideEvent* event)
{
    if (m_tickTimer)
        m_tickTimer->stop();
    QWidget::hideEvent(event);
}

void HealthApplet::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        togglePaused();
        event->accept();
        return;
    }

    QWidget::mouseReleaseEvent(event);
}

void HealthApplet::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    if (m_tickTimer && !m_paused) {
        m_tickTimer->start();
        appendFrame();
    }
}

void HealthApplet::setMeterModel(MeterModel* model)
{
    if (m_model == model)
        return;

    if (m_model) {
        // Named-signal disconnects rather than the broad
        // disconnect(m_model, nullptr, this, nullptr) form — keeps any
        // other connections an outside party might have established to
        // the same model intact.
        disconnect(m_model, &MeterModel::txMetersChanged,   this, nullptr);
        disconnect(m_model, &MeterModel::directionalPowerMetersChanged,
                   this, nullptr);
        disconnect(m_model, &MeterModel::tgxlMetersChanged, this, nullptr);
        disconnect(m_model, &MeterModel::ampMetersChanged,  this, nullptr);
    }

    m_model = model;
    if (!m_model)
        return;

    // Emission order matters: MeterModel::updateValues() emits
    // directionalPowerMetersChanged (carrying the INSTANTANEOUS forward power
    // used to gate SWR) in the same cycle as txMetersChanged (the smoothed
    // display power). updateRadioDirectionalMeters() must overwrite the
    // qualifying power AFTER updateRadioMeters() caches the smoothed value, so
    // the SWR gate never qualifies on decaying unkey power. A future MeterModel
    // refactor that splits FWDPWR/SWR into independently-gated signals would
    // break this — keep them co-emitted, or gate on an explicit instantaneous
    // timestamp here. (#4243)
    connect(m_model, &MeterModel::txMetersChanged,
            this, &HealthApplet::updateRadioMeters);
    connect(m_model, &MeterModel::directionalPowerMetersChanged,
            this, &HealthApplet::updateRadioDirectionalMeters);
    connect(m_model, &MeterModel::tgxlMetersChanged,
            this, &HealthApplet::updateTunerMeters);
    connect(m_model, &MeterModel::ampMetersChanged,
            this, [this](float fwdPower, float swr, float) {
        updateAmplifierMeters(fwdPower, swr);
    });
}

void HealthApplet::setPowerScale(int maxWatts, bool hasAmplifier)
{
    if (hasAmplifier) {
        m_powerFullScale = 2000.0f;
    } else if (maxWatts > 100) {
        m_powerFullScale = std::max(600.0f, static_cast<float>(maxWatts) * 1.2f);
    } else {
        m_powerFullScale = 120.0f;
    }
}

void HealthApplet::updateRadioMeters(float fwdPowerWatts, float swr)
{
    cacheMeters(MeterSource::Radio, fwdPowerWatts, swr);
}

void HealthApplet::updateTunerMeters(float fwdPowerWatts, float swr)
{
    cacheMeters(MeterSource::Tuner, fwdPowerWatts, swr);
}

void HealthApplet::updateAmplifierMeters(float fwdPowerWatts, float swr)
{
    cacheMeters(MeterSource::Amplifier, fwdPowerWatts, swr);
}

void HealthApplet::updateRadioDirectionalMeters(float forwardPowerWatts,
                                                 float reflectedPowerWatts,
                                                 float swr,
                                                 bool reflectedPowerMeasured)
{
    Q_UNUSED(reflectedPowerWatts)
    Q_UNUSED(reflectedPowerMeasured)

    m_radioSnapshot.swrQualifyingPowerWatts =
        std::max(0.0f, finiteOr(forwardPowerWatts, 0.0f));
    m_radioSnapshot.swr = std::clamp(finiteOr(swr, 1.0f), 1.0f, 99.0f);
    m_radioSnapshot.swrQualifyingPowerUpdatedAtMs = m_model
        ? m_model->fwdPowerUpdatedAtMs()
        : QDateTime::currentMSecsSinceEpoch();
    m_radioSnapshot.swrSampleUpdatedAtMs = m_model
        ? m_model->swrUpdatedAtMs()
        : QDateTime::currentMSecsSinceEpoch();
}

void HealthApplet::cacheMeters(MeterSource source, float fwdPowerWatts, float swr)
{
    MeterSnapshot* dst = nullptr;
    switch (source) {
    case MeterSource::Radio:     dst = &m_radioSnapshot; break;
    case MeterSource::Tuner:     dst = &m_tunerSnapshot; break;
    case MeterSource::Amplifier: dst = &m_ampSnapshot; break;
    case MeterSource::None:      return;
    }

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    dst->powerWatts = std::max(0.0f, finiteOr(fwdPowerWatts, 0.0f));
    // The "qualifying" fields feed the SWR gate. For Tuner/Amplifier the
    // forward power is already instantaneous, so they mirror powerWatts here.
    // For Radio these are placeholders immediately overwritten by
    // updateRadioDirectionalMeters() with the true instantaneous power.
    dst->swrQualifyingPowerWatts = dst->powerWatts;
    dst->swr = std::clamp(finiteOr(swr, 1.0f), 1.0f, 99.0f);
    dst->updatedAtMs = nowMs;
    dst->swrQualifyingPowerUpdatedAtMs = nowMs;
    dst->swrSampleUpdatedAtMs = nowMs;
    dst->valid = true;
}

HealthApplet::MeterSnapshot HealthApplet::bestSnapshot(qint64 nowMs,
                                                       MeterSource* source) const
{
    auto fresh = [nowMs](const MeterSnapshot& s) {
        return s.valid && (nowMs - s.updatedAtMs) <= kFreshMs;
    };
    auto staleButVisible = [nowMs](const MeterSnapshot& s) {
        return s.valid && (nowMs - s.updatedAtMs) <= kStaleMs;
    };
    auto activeEnough = [](const MeterSnapshot& s) {
        return s.powerWatts > 0.25f || s.swr > 1.015f;
    };

    if (fresh(m_ampSnapshot) && activeEnough(m_ampSnapshot)) {
        if (source) *source = MeterSource::Amplifier;
        return m_ampSnapshot;
    }
    if (fresh(m_tunerSnapshot) && activeEnough(m_tunerSnapshot)) {
        if (source) *source = MeterSource::Tuner;
        return m_tunerSnapshot;
    }
    if (fresh(m_radioSnapshot)) {
        if (source) *source = MeterSource::Radio;
        return m_radioSnapshot;
    }

    const MeterSnapshot* best = nullptr;
    MeterSource bestSource = MeterSource::None;
    auto consider = [&](const MeterSnapshot& s, MeterSource candidate) {
        if (!staleButVisible(s))
            return;
        if (!best || s.updatedAtMs > best->updatedAtMs) {
            best = &s;
            bestSource = candidate;
        }
    };
    consider(m_ampSnapshot, MeterSource::Amplifier);
    consider(m_tunerSnapshot, MeterSource::Tuner);
    consider(m_radioSnapshot, MeterSource::Radio);

    if (best) {
        if (source) *source = bestSource;
        return *best;
    }

    if (source) *source = MeterSource::None;
    return {};
}

void HealthApplet::appendFrame()
{
    if (m_paused || !isVisible())
        return;

    MeterSource source = MeterSource::None;
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const MeterSnapshot snapshot = bestSnapshot(nowMs, &source);
    const bool active = snapshot.valid
        && (nowMs - snapshot.updatedAtMs) <= kStaleMs
        && snapshot.powerWatts >= kActivePowerWatts;
    if (active) {
        ++m_activeFrames;
    } else {
        m_activeFrames = 0;
    }
    const bool swrQualifyingPowerFresh =
        snapshot.swrQualifyingPowerUpdatedAtMs > 0
        && (nowMs - snapshot.swrQualifyingPowerUpdatedAtMs) <= kFreshMs;
    const bool swrCandidate = active
        && swrQualifyingPowerFresh
        && snapshot.swrQualifyingPowerWatts >= kActivePowerWatts;

    // Only Radio forward power is smoothed by MeterModel, so only Radio SWR
    // carries the key/unkey transient this PR filters. Tuner and Amplifier
    // forward power is never smoothed, so trust their raw SWR immediately —
    // gating them on the settle window would delay a genuine amp/tuner fault by
    // ~250ms for no benefit.
    const bool needsSettle = source == MeterSource::Radio;

    // Tolerate a brief inactive dropout or a transient bestSnapshot() source
    // flip before wiping the admission quorum. bestSnapshot() reselects the
    // source every tick from a bare 900ms/1700ms freshness boundary with no
    // hysteresis, so a second meter whose cadence straddles it oscillates the
    // source; clearing the window on every flip would trap HLTH in "never
    // trusted" and mask a real fault (the opposite of what #4243 needs). Only
    // reset after sustained inactivity or a settled source change.
    m_swrInactiveFrames = active ? 0 : (m_swrInactiveFrames + 1);
    m_swrSourceMismatchFrames =
        (source == m_swrAdmissionSource) ? 0 : (m_swrSourceMismatchFrames + 1);
    // Acquire the first source immediately (None -> real); only a *sustained*
    // dropout or source change resets an already-established window.
    if (m_swrAdmissionSource == MeterSource::None
        || m_swrInactiveFrames > kSwrDropoutFrames
        || m_swrSourceMismatchFrames > kSwrDropoutFrames) {
        m_swrAdmissionWindow.clear();
        m_lastAdmittedSwrSampleMs = 0;
        m_swrAdmissionSource = source;
        m_swrInactiveFrames = 0;
        m_swrSourceMismatchFrames = 0;
    }
    // Admit only samples from the tracked admission source so a transient flip
    // to a second meter cannot mix foreign SWR into the quorum.
    if (needsSettle
        && swrCandidate
        && source == m_swrAdmissionSource
        && snapshot.swrSampleUpdatedAtMs > 0
        && snapshot.swrSampleUpdatedAtMs != m_lastAdmittedSwrSampleMs) {
        m_swrAdmissionWindow.append(snapshot.swr);
        while (m_swrAdmissionWindow.size() > kSwrAdmissionWindow) {
            m_swrAdmissionWindow.removeFirst();
        }
        m_lastAdmittedSwrSampleMs = snapshot.swrSampleUpdatedAtMs;
    }

    const bool swrTrusted = swrCandidate
        && (!needsSettle
            || (m_activeFrames >= kSwrSettleFrames
                && m_swrAdmissionWindow.size() >= kSwrAdmissionWindow));

    const float targetPower = active ? snapshot.powerWatts : 0.0f;
    float targetSwr;
    if (!active) {
        targetSwr = 1.0f;
    } else if (!needsSettle) {
        // Tuner/Amplifier: unsmoothed power, no transient to filter.
        targetSwr = snapshot.swr;
    } else if (swrTrusted) {
        targetSwr = admittedSwr(m_swrAdmissionWindow);
    } else {
        // Radio, still settling — hold the last trusted display value.
        targetSwr = m_displaySwr;
    }

    const float powerAlpha = targetPower > m_displayPower ? 0.45f : 0.18f;
    m_displayPower += (targetPower - m_displayPower) * powerAlpha;
    m_displaySwr += (targetSwr - m_displaySwr) * (active ? 0.32f : 0.14f);
    const float displayReturnLossDb = returnLossDbForSwr(m_displaySwr);

    if (active) {
        m_idleFrames = 0;
        // Baseline + telemetry only accumulate once SWR is trusted. For Radio
        // this intentionally withholds the first ~250ms of a transmit so a
        // key-up transient cannot poison the baseline (the #4243 fix); a fast
        // Radio fault confined to that settle window is deferred by design.
        // Tuner/Amplifier trust immediately (needsSettle == false), so they
        // have no such blind spot.
        if (swrTrusted) {
            if (!m_baselineReady) {
                m_powerAverage = std::max(1.0f, m_displayPower);
                m_swrAverage = std::max(1.0f, m_displaySwr);
                m_returnLossAverage = displayReturnLossDb;
                m_baselineReady = true;
            } else {
                m_powerAverage += (m_displayPower - m_powerAverage) * 0.035f;
                m_swrAverage += (m_displaySwr - m_swrAverage) * 0.030f;
                m_returnLossAverage += (displayReturnLossDb - m_returnLossAverage) * 0.030f;
            }
            pushRecent(m_displayPower, m_displaySwr, displayReturnLossDb);
        }
    } else {
        ++m_idleFrames;
        if (m_idleFrames > 24) {
            m_recentPower.clear();
            m_recentSwr.clear();
            m_recentReturnLoss.clear();
            recomputeRecentStats();
            m_baselineReady = false;
            m_powerAverage = std::max(1.0f, m_displayPower);
            m_swrAverage = 1.0f;
            m_returnLossAverage = kMaxReturnLossDb;
            m_lastSeverity = 0.0f;
            m_lastReturnLossDb = kMaxReturnLossDb;
            m_incidentCooldownFrames = 0;
        }
    }

    recomputeRecentStats();

    AntennaHealthSample sample;
    sample.powerWatts = m_displayPower;
    sample.swr = m_displaySwr;
    sample.powerAverage = m_baselineReady ? m_powerAverage : std::max(1.0f, m_displayPower);
    sample.swrAverage = m_baselineReady ? m_swrAverage : 1.0f;
    sample.returnLossDb = displayReturnLossDb;
    sample.returnLossAverage = m_baselineReady ? m_returnLossAverage : kMaxReturnLossDb;
    sample.powerSpread = std::max({m_powerStdDev, sample.powerAverage * 0.025f, m_powerFullScale * 0.006f});
    sample.swrSpread = std::max({m_swrStdDev, m_swrSpan * 0.5f, 0.025f});
    sample.returnLossSpread = std::max({m_returnLossStdDev, m_returnLossSpan * 0.5f, 0.75f});
    sample.active = active;
    sample.severity = swrTrusted
        ? computeSeverity(sample.powerWatts, sample.swr)
        : 0.0f;

    if (m_incidentCooldownFrames > 0)
        --m_incidentCooldownFrames;

    if (sample.active && m_incidentCooldownFrames == 0) {
        QString incidentLabel;
        if (sample.severity >= 0.72f && m_lastSeverity < 0.50f) {
            incidentLabel = QStringLiteral("SWR");
        } else if (sample.returnLossDb <= 10.0f && m_lastReturnLossDb > 11.5f) {
            incidentLabel = QStringLiteral("RL");
        } else if (m_swrSpan >= 0.28f && sample.severity >= 0.34f
                   && sample.severity > m_lastSeverity + 0.16f) {
            incidentLabel = QStringLiteral("VAR");
        } else if (sample.severity >= 0.34f && m_lastSeverity < 0.22f) {
            incidentLabel = QStringLiteral("WATCH");
        }

        if (!incidentLabel.isEmpty()) {
            sample.incident = true;
            sample.incidentSeverity = sample.severity;
            sample.incidentLabel = incidentLabel;
            m_incidentCooldownFrames = 80;
        }
    }

    m_lastSeverity = sample.active ? sample.severity : 0.0f;
    m_lastReturnLossDb = sample.returnLossDb;

    m_history.append(sample);
    while (m_history.size() > kMaxHistory)
        m_history.removeFirst();

    if (m_graph)
        m_graph->setSamples(m_history);
    updateStatusLabels(sample, source);
}

void HealthApplet::applyTheme()
{
    if (!m_statusLabel)
        return;

    applyLabelStyle(m_sourceLabel,     QStringLiteral("color.text.secondary"), 10, true);
    applyLabelStyle(m_swrLabel,        QStringLiteral("color.accent.success"), 10, true);
    applyLabelStyle(m_returnLossLabel, QStringLiteral("color.accent.warning"), 10, true);
    applyLabelStyle(m_powerLabel,      QStringLiteral("color.accent"),         10, true);
    applyLabelStyle(m_varianceLabel,   QStringLiteral("color.text.secondary"), 10, false);

    if (m_graph)
        m_graph->update();

    if (m_paused) {
        showPausedState();
    } else if (!m_history.isEmpty()) {
        updateStatusLabels(m_history.constLast(), m_lastSource);
    } else {
        updateStatusLabels({}, MeterSource::None);
    }
}

void HealthApplet::togglePaused()
{
    m_paused = !m_paused;
    if (m_paused) {
        if (m_tickTimer)
            m_tickTimer->stop();
        showPausedState();
    } else {
        if (m_tickTimer && isVisible())
            m_tickTimer->start();
        appendFrame();
    }
}

void HealthApplet::showPausedState()
{
    if (!m_statusLabel)
        return;

    m_statusLabel->setText(QStringLiteral("PAUSED"));
    applyPillStyle(m_statusLabel, QStringLiteral("color.background.1"),
                   QStringLiteral("color.accent"),
                   QStringLiteral("color.text.primary"));
    m_scoreLabel->setText(QStringLiteral("II"));
    applyLabelStyle(m_scoreLabel, QStringLiteral("color.accent"), 11, true);
}

void HealthApplet::pushRecent(float powerWatts, float swr, float returnLossDb)
{
    m_recentPower.append(powerWatts);
    m_recentSwr.append(swr);
    m_recentReturnLoss.append(returnLossDb);
    while (m_recentPower.size() > kRecentWindow)
        m_recentPower.removeFirst();
    while (m_recentSwr.size() > kRecentWindow)
        m_recentSwr.removeFirst();
    while (m_recentReturnLoss.size() > kRecentWindow)
        m_recentReturnLoss.removeFirst();
}

void HealthApplet::recomputeRecentStats()
{
    auto stddev = [](const QVector<float>& values, float* spanOut) {
        if (values.isEmpty()) {
            if (spanOut) *spanOut = 0.0f;
            return 0.0f;
        }

        float minValue = std::numeric_limits<float>::max();
        float maxValue = std::numeric_limits<float>::lowest();
        double sum = 0.0;
        for (float v : values) {
            minValue = std::min(minValue, v);
            maxValue = std::max(maxValue, v);
            sum += v;
        }
        const double mean = sum / static_cast<double>(values.size());
        double squares = 0.0;
        for (float v : values) {
            const double d = static_cast<double>(v) - mean;
            squares += d * d;
        }
        if (spanOut) *spanOut = maxValue - minValue;
        return static_cast<float>(std::sqrt(squares / static_cast<double>(values.size())));
    };

    m_powerStdDev = stddev(m_recentPower, nullptr);
    m_swrStdDev = stddev(m_recentSwr, &m_swrSpan);
    m_returnLossStdDev = stddev(m_recentReturnLoss, &m_returnLossSpan);
}

float HealthApplet::computeSeverity(float powerWatts, float swr) const
{
    if (powerWatts < kActivePowerWatts)
        return 0.0f;

    const float avgSWR = std::max(1.0f, m_swrAverage);
    const float swrDelta = std::abs(swr - avgSWR);
    const float returnLossDb = returnLossDbForSwr(swr);
    const float highSWR = smoothStep(1.65f, 2.50f, swr);
    const float poorReturnLoss = 1.0f - smoothStep(8.0f, 15.0f, returnLossDb);
    const float swrSpanPenalty = smoothStep(0.12f, 0.42f, m_swrSpan);
    const float swrDeltaPenalty = smoothStep(0.08f, 0.30f, swrDelta);

    float coupledPowerSag = 0.0f;
    if (m_powerAverage > 5.0f && swr > avgSWR + 0.06f) {
        const float sag = std::max(0.0f, (m_powerAverage - powerWatts) / m_powerAverage);
        coupledPowerSag = smoothStep(0.18f, 0.48f, sag);
    }

    return std::max({highSWR, poorReturnLoss, swrSpanPenalty, swrDeltaPenalty, coupledPowerSag});
}

void HealthApplet::updateStatusLabels(const AntennaHealthSample& sample,
                                      MeterSource source)
{
    if (!m_statusLabel)
        return;

    m_lastSource = source;
    m_sourceLabel->setText(QStringLiteral("SRC %1").arg(sourceText(source)));
    m_swrLabel->setText(QStringLiteral("SWR %1").arg(sample.swr, 0, 'f', 2));
    m_returnLossLabel->setText(sample.active
        ? QStringLiteral("RL %1dB").arg(sample.returnLossDb, 0, 'f', 0)
        : QStringLiteral("RL --"));
    m_powerLabel->setText(QStringLiteral("PWR %1").arg(formatPower(sample.powerWatts)));
    m_varianceLabel->setText(sample.active
        ? QStringLiteral("VAR %1").arg(m_swrSpan, 0, 'f', 2)
        : QStringLiteral("VAR --"));

    if (!sample.active) {
        m_statusLabel->setText(QStringLiteral("IDLE"));
        applyPillStyle(m_statusLabel, QStringLiteral("color.background.0"),
                       QStringLiteral("color.border.strong"),
                       QStringLiteral("color.text.secondary"));
        m_scoreLabel->setText(QStringLiteral("--"));
        applyLabelStyle(m_scoreLabel, QStringLiteral("color.text.primary"), 11, true);
        return;
    }

    const int score = std::clamp(static_cast<int>(std::round(100.0f - sample.severity * 72.0f)),
                                 0, 100);
    m_scoreLabel->setText(QString::number(score));

    if (sample.severity >= 0.72f) {
        m_statusLabel->setText(QStringLiteral("GROUND?"));
        applyPillStyle(m_statusLabel, QStringLiteral("color.background.tx"),
                       QStringLiteral("color.accent.danger"),
                       QStringLiteral("color.accent.danger"));
        applyLabelStyle(m_scoreLabel, QStringLiteral("color.accent.danger"), 11, true);
    } else if (sample.severity >= 0.34f) {
        m_statusLabel->setText(QStringLiteral("WATCH"));
        applyPillStyle(m_statusLabel, QStringLiteral("color.background.tx"),
                       QStringLiteral("color.accent.warning"),
                       QStringLiteral("color.accent.warning"));
        applyLabelStyle(m_scoreLabel, QStringLiteral("color.accent.warning"), 11, true);
    } else {
        m_statusLabel->setText(QStringLiteral("OK"));
        applyPillStyle(m_statusLabel, QStringLiteral("color.background.1"),
                       QStringLiteral("color.accent.success"),
                       QStringLiteral("color.accent.success"));
        applyLabelStyle(m_scoreLabel, QStringLiteral("color.accent.success"), 11, true);
    }
}

QString HealthApplet::sourceText(MeterSource source) const
{
    switch (source) {
    case MeterSource::Radio:     return QStringLiteral("RAD");
    case MeterSource::Tuner:     return QStringLiteral("TUN");
    case MeterSource::Amplifier: return QStringLiteral("AMP");
    case MeterSource::None:      break;
    }
    return QStringLiteral("--");
}

QString HealthApplet::formatPower(float watts) const
{
    if (watts >= 995.0f)
        return QStringLiteral("%1 kW").arg(watts / 1000.0f, 0, 'f', 2);
    if (watts >= 100.0f)
        return QStringLiteral("%1 W").arg(watts, 0, 'f', 0);
    if (watts >= 10.0f)
        return QStringLiteral("%1 W").arg(watts, 0, 'f', 1);
    return QStringLiteral("%1 W").arg(watts, 0, 'f', 2);
}

} // namespace AetherSDR
