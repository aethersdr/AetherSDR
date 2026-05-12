#include "StripFinalOutputPanel.h"

#include "ClientCompKnob.h"
#include "EditorFramelessTitleBar.h"
#include "FramelessMoveHelper.h"
#include "MeterSmoother.h"
#include "core/AppSettings.h"
#include "core/AudioEngine.h"
#include "core/ClientFinalLimiter.h"
#include "core/ClientQuindarTone.h"
#include "core/ClientTxTestTone.h"

#include <QDateTime>
#include <QDialog>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QTimer>
#include <QWindow>
#include <QVBoxLayout>

#include <functional>

#include <algorithm>
#include <cmath>

namespace AetherSDR {

namespace {

// Don't paint a wide-open `QWidget { background: ... }` rule — it
// lands on every descendant QWidget that has WA_StyledBackground
// (knobs, custom-paint widgets) and produces a darker patch over
// the panel's actual bg, which is the strip's group-box.  Just set
// label cascade so text colour/size is consistent.
constexpr const char* kWindowStyle =
    "QLabel { background: transparent; color: #8aa8c0; font-size: 11px; }";

constexpr float kMeterMinDb = -60.0f;
constexpr float kMeterMaxDb =   0.0f;
// Use the project-canonical MeterSmoother poll rate so this panel's
// ballistics match every other meter in the app (ClientCompMeter,
// HGauge, etc.).
constexpr int   kMeterIntervalMs = kMeterSmootherIntervalMs;

float dbToRatio(float db)
{
    if (db <= kMeterMinDb) return 0.0f;
    if (db >= kMeterMaxDb) return 1.0f;
    return (db - kMeterMinDb) / (kMeterMaxDb - kMeterMinDb);
}

// Inline horizontal level meter: green→amber→red gradient bar with
// the post-limiter peak filled, the input peak shown as a darker
// background bar, and the comp-style notch where the ceiling sits.
class HorizMeter : public QWidget {
public:
    explicit HorizMeter(QWidget* parent = nullptr) : QWidget(parent) {
        // 22 px header + 28 px bar + 22 px transparent footer.
        // The footer is intentional padding so the WIDGET'S geometric
        // centre lines up with the BAR'S centre — when the panel's
        // row centres widgets vertically, the bar then sits at the
        // row's centre (rather than getting pushed down by the
        // header's weight).  Footer is left unpainted; parent bg
        // shows through.
        setMinimumHeight(72);
        // Floor the meter at 180 px so it doesn't get squeezed to a
        // sliver when the surrounding row's fixed widgets (knobs,
        // readouts, indicators, rec buttons) exhaust available space.
        setMinimumWidth(180);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setMouseTracking(true);
    }

    // Pluggable callback the panel installs so the meter can drive
    // the engine's ceiling parameter directly without HorizMeter
    // needing Q_OBJECT (it lives in an anonymous namespace).
    using CeilingSetter = std::function<void(float dbCeiling)>;
    void setCeilingSetter(CeilingSetter fn) { m_setCeiling = std::move(fn); }

    void setLevels(float inPeakDb, float outPeakDb, float rawOutPeakDb,
                   float grDb, float ceilingDb) {
        m_inPeakDb  = inPeakDb;
        m_outPeakDb = outPeakDb;
        m_grDb      = grDb;
        m_ceilingDb = ceilingDb;
        // Peak-hold: track the highest *raw* engine block-peak in a
        // 1.5 s window via a millisecond timestamp.  Reading the raw
        // value (not the panel's smoothed display) catches single
        // fast clamps that would otherwise get averaged out before
        // the bar renders.
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        if (rawOutPeakDb > m_holdDb || nowMs >= m_holdUntilMs) {
            if (rawOutPeakDb > m_holdDb) {
                m_holdDb = rawOutPeakDb;
                m_holdUntilMs = nowMs + 1500;
            } else {
                m_holdDb = rawOutPeakDb;  // expired — track current
            }
        }
        update();
    }

protected:
    QRectF barRect() const {
        // Bar lives in the bottom 28 px.  Top 22 px = numeric ceiling
        // readout (~10 px) stacked over the drag triangle (~10 px).
        return QRectF(rect().left() + 1, rect().top() + 22,
                      rect().width() - 2, 28 - 2);
    }

    double ceilingX() const {
        const QRectF r = barRect();
        return r.left() + dbToRatio(m_ceilingDb) * r.width();
    }

    float xToCeilingDb(double x) const {
        const QRectF r = barRect();
        const double ratio = std::clamp((x - r.left()) / std::max(1.0, r.width()),
                                        0.0, 1.0);
        const float db = static_cast<float>(kMeterMinDb
            + ratio * (kMeterMaxDb - kMeterMinDb));
        // Limiter ceiling is clamped to [-12, 0] dB elsewhere; mirror
        // that here so the handle can't be dragged into a region the
        // engine would silently snap back from.
        return std::clamp(db, -12.0f, 0.0f);
    }

    bool isOverHandle(const QPoint& p) const {
        const double cx = ceilingX();
        const QRectF r = rect();
        const QRectF bar = barRect();
        // Top band (text + triangle): 22 px tall, ±8 px around cx.
        const bool inHeader = std::fabs(p.x() - cx) < 8.0
                           && p.y() < r.top() + 22;
        // Dashed ceiling line column inside the bar — narrower hit
        // zone (±5 px) since the line itself is 1 px wide and we
        // don't want to steal cursor cues from the rest of the bar.
        const bool inBarLine = std::fabs(p.x() - cx) < 5.0
                            && p.y() >= bar.top()
                            && p.y() <= bar.bottom();
        return inHeader || inBarLine;
    }

    void mousePressEvent(QMouseEvent* ev) override {
        if (ev->button() == Qt::LeftButton && m_setCeiling) {
            m_dragging = true;
            m_setCeiling(xToCeilingDb(ev->position().x()));
            ev->accept();
            return;
        }
        QWidget::mousePressEvent(ev);
    }

    void mouseMoveEvent(QMouseEvent* ev) override {
        if (m_dragging && (ev->buttons() & Qt::LeftButton) && m_setCeiling) {
            m_setCeiling(xToCeilingDb(ev->position().x()));
            ev->accept();
            return;
        }
        // Update cursor when hovering the handle area without dragging.
        setCursor(isOverHandle(ev->pos()) ? Qt::SizeHorCursor : Qt::ArrowCursor);
        QWidget::mouseMoveEvent(ev);
    }

    void mouseReleaseEvent(QMouseEvent* ev) override {
        if (ev->button() == Qt::LeftButton && m_dragging) {
            m_dragging = false;
            ev->accept();
            return;
        }
        QWidget::mouseReleaseEvent(ev);
    }

    void wheelEvent(QWheelEvent* ev) override {
        if (!m_setCeiling) { QWidget::wheelEvent(ev); return; }
        // 120 angleDelta units = one wheel notch.  0.1 dB per notch
        // (1 dB per ten notches) lands fine resolution.  Ctrl
        // multiplies by 10 for coarse adjustment.
        const int notches = ev->angleDelta().y() / 120;
        if (notches == 0) { QWidget::wheelEvent(ev); return; }
        const float step = ev->modifiers().testFlag(Qt::ControlModifier)
                         ? 1.0f : 0.1f;
        const float target = m_ceilingDb + step * static_cast<float>(notches);
        m_setCeiling(std::clamp(target, -12.0f, 0.0f));
        ev->accept();
    }

    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const QRectF r = barRect();

        // Track background.
        p.setPen(QPen(QColor("#1a2a3a"), 1));
        p.setBrush(QColor("#0a0e16"));
        p.drawRoundedRect(r, 3, 3);

        // Input-peak bar — drawn first so the (brighter) output bar
        // sits on top of it.  Slightly muted so it reads as a "you
        // sent this much in" backdrop versus the output reading.
        const double inFill = dbToRatio(m_inPeakDb) * r.width();
        if (inFill > 0.0) {
            QRectF inR = r;
            inR.setWidth(inFill);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(80, 110, 140, 110));
            p.drawRoundedRect(inR, 3, 3);
        }

        // Output bar — gradient green → amber → red across full track.
        const double outFill = dbToRatio(m_outPeakDb) * r.width();
        if (outFill > 0.0) {
            QLinearGradient g(r.left(), 0, r.right(), 0);
            g.setColorAt(0.00, QColor("#30c060"));
            g.setColorAt(dbToRatio(-12.0f), QColor("#a0c030"));
            g.setColorAt(dbToRatio(-3.0f),  QColor("#d49030"));
            g.setColorAt(1.00, QColor("#c03030"));
            QRectF outR = r;
            outR.setWidth(outFill);
            p.setPen(Qt::NoPen);
            p.setBrush(g);
            p.drawRoundedRect(outR, 3, 3);
        }

        // Peak-hold hairline — bright cyan vertical line at the
        // highest output peak in the last 1.5 s.  Drawn ahead of the
        // ceiling so the latter still wins when they overlap.
        if (m_holdDb > -100.0f) {
            const double hx = r.left() + dbToRatio(m_holdDb) * r.width();
            p.setPen(QPen(QColor("#a0e0ff"), 2.0));
            p.drawLine(QPointF(hx, r.top() + 1), QPointF(hx, r.bottom() - 1));
        }

        // GR overlay — translucent red fill from the right edge whose
        // width tracks how many dB the limiter is currently cutting.
        // Conservative scale: 1 dB GR = ~3.3% of the bar width so even
        // a small clamp is visible.
        if (m_grDb < -0.05f) {
            const double grSpan = std::clamp(-m_grDb / 30.0, 0.0, 1.0)
                                  * r.width();
            QRectF grR = r;
            grR.setX(r.right() - grSpan);
            grR.setWidth(grSpan);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(255, 80, 80, 110));
            p.drawRoundedRect(grR, 3, 3);
        }

        // Ceiling notch — vertical hairline marking where the limiter
        // cuts in.  Amber matches the "limit" indicator's accent.
        const double cx = r.left() + dbToRatio(m_ceilingDb) * r.width();
        p.setPen(QPen(QColor("#f2c14e"), 1.2, Qt::DashLine));
        p.drawLine(QPointF(cx, r.top() + 1), QPointF(cx, r.bottom() - 1));

        // dB scale — tick marks at every 12 dB step, drawn so they
        // straddle the bar's bottom edge (visible against both the
        // gradient fill above and the empty space below).  Each tick
        // gets a numeric label in the footer band underneath.
        QFont tickFont = p.font();
        tickFont.setPointSize(7);
        tickFont.setBold(true);
        p.setFont(tickFont);
        const QFontMetrics tickFm(tickFont);
        for (int db = -60; db <= 0; db += 12) {
            const double tickX = r.left() + dbToRatio(static_cast<float>(db)) * r.width();
            // 4 px above + 4 px below the bar bottom edge — the upper
            // half cuts into the gradient (bright contrast), the lower
            // half drops into the footer band.
            p.setPen(QPen(QColor("#a0b4c8"), 1.2));
            p.drawLine(QPointF(tickX, r.bottom() - 4),
                       QPointF(tickX, r.bottom() + 4));
            // Label centred under the tick.  Use "0" for the right
            // edge instead of "-0" so the value reads cleanly.
            const QString lbl = (db == 0) ? "0" : QString::number(db);
            const int lblW = tickFm.horizontalAdvance(lbl);
            // Centre the label on the tick, then clamp so the leftmost
            // (-60) and rightmost (0) labels don't bleed off the
            // widget when the tick sits flush with the bar edge.
            double lblX = tickX - lblW / 2.0 - 1;
            const double minX = rect().left() + 2;
            const double maxX = rect().right() - 2 - (lblW + 2);
            if (lblX < minX) lblX = minX;
            if (lblX > maxX) lblX = maxX;
            const QRectF labelR(lblX, r.bottom() + 5, lblW + 2, 12);
            p.setPen(QColor("#7f93a5"));
            p.drawText(labelR, Qt::AlignCenter, lbl);
        }

        // Ceiling drag handle — amber triangle pointing DOWN at the
        // bar from above.  Vertex sits 1 px above the bar; base 8 px
        // higher.  Hit-tested via isOverHandle() (entire top band).
        QPainterPath handle;
        const double hCx = cx;
        const double hY  = r.top() - 1;
        handle.moveTo(hCx,     hY);
        handle.lineTo(hCx - 5, hY - 8);
        handle.lineTo(hCx + 5, hY - 8);
        handle.closeSubpath();
        p.setPen(Qt::NoPen);
        p.setBrush(QColor("#f2c14e"));
        p.drawPath(handle);

        // Numeric readout centred horizontally on the triangle and
        // stacked ABOVE it.  The marker visually reads "value pointing
        // at the bar."  When the value would clip off the widget at
        // the left or right edge, the rect is shifted inward.
        QFont vf = p.font();
        vf.setPointSize(8);
        vf.setBold(true);
        p.setFont(vf);
        p.setPen(QColor("#f2c14e"));
        const QString txt = QString::number(m_ceilingDb, 'f', 1) + " dB";
        const QFontMetrics fm(vf);
        const int tw = fm.horizontalAdvance(txt);
        // Centre over the triangle, then nudge inward if we'd clip
        // off the widget on either edge.
        double tx = hCx - tw / 2.0;
        const double widgetLeft  = rect().left() + 2;
        const double widgetRight = rect().right() - 2 - tw;
        if (tx < widgetLeft)  tx = widgetLeft;
        if (tx > widgetRight) tx = widgetRight;
        // Text sits in the top 12 px of the 22 px header band — above
        // the triangle (which occupies the next ~10 px down).
        const QRectF textRect(tx, rect().top(), tw + 2, 12);
        p.drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, txt);
    }

private:
    float  m_inPeakDb{-120.0f};
    float  m_outPeakDb{-120.0f};
    float  m_grDb{0.0f};
    float  m_ceilingDb{-1.0f};
    float  m_holdDb{-120.0f};
    qint64 m_holdUntilMs{0};
    bool   m_dragging{false};
    CeilingSetter m_setCeiling;
};

} // namespace

StripFinalOutputPanel::StripFinalOutputPanel(AudioEngine* engine, QWidget* parent)
    : QWidget(parent)
    , m_audio(engine)
{
    const QString title = QString::fromUtf8(
        "Aetherial Final Output Stage \xe2\x80\x94 TX");
    setWindowTitle(title);
    setStyleSheet(kWindowStyle);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 0, 8, 0);
    root->setSpacing(6);

    auto* titleBar = new EditorFramelessTitleBar;
    titleBar->setTitleText(title);
    // Embedded inside the strip — drop the heavy fill + trio so the
    // panel reads as a clean row of controls instead of carrying a
    // duplicate title bar over the strip's own chrome.
    titleBar->setControlsVisible(false);
    titleBar->setStyleSheet("background: transparent;");
    root->addWidget(titleBar);
    // 10 px breathing room between the title and the controls row
    // — keeps the chip column tops from kissing the title text.
    root->addSpacing(10);

    // Single horizontal row: enable toggle | ceiling knob | meter | LIMIT LED
    auto* row = new QHBoxLayout;
    row->setSpacing(8);

    // LIM / DC / TONE — three small toggles stacked vertically on
    // the left.  Sized + styled to match the OVR / LIMIT indicator
    // tiles in the right column so the panel reads as two columns
    // of identical chip-style controls flanking the meter.
    {
        const QString amberStyle =
            "QPushButton {"
            "  background: #1a2230; border: 1px solid #2a3744;"
            "  border-radius: 3px; color: #506070;"
            "  font-size: 10px; font-weight: bold; padding: 1px;"
            "}"
            "QPushButton:hover { color: #c8d8e8; }"
            "QPushButton:checked {"
            "  background: #3a2a0e; color: #f2c14e;"
            "  border: 1px solid #f2c14e;"
            "}"
            "QPushButton:checked:hover { background: #4a3a18; }";
        const QString redCheckStyle =
            "QPushButton {"
            "  background: #1a2230; border: 1px solid #2a3744;"
            "  border-radius: 3px; color: #506070;"
            "  font-size: 10px; font-weight: bold; padding: 1px;"
            "}"
            "QPushButton:hover { color: #c8d8e8; }"
            "QPushButton:checked {"
            "  background: #4a1818; color: #ff8080;"
            "  border: 1px solid #ff4040;"
            "}"
            "QPushButton:checked:hover { background: #5a2828; }";

        auto* col = new QVBoxLayout;
        col->setContentsMargins(0, 0, 0, 0);
        col->setSpacing(2);

        m_enable = new QPushButton("LIM", this);
        m_enable->setCheckable(true);
        m_enable->setFixedSize(56, 18);
        m_enable->setStyleSheet(amberStyle);
        m_enable->setToolTip("Enable the dedicated final-stage brickwall "
                             "limiter at the tail of the TX chain.");
        connect(m_enable, &QPushButton::toggled,
                this, &StripFinalOutputPanel::applyEnable);
        col->addWidget(m_enable);

        m_dcBtn = new QPushButton("DC", this);
        m_dcBtn->setCheckable(true);
        m_dcBtn->setFixedSize(56, 18);
        m_dcBtn->setStyleSheet(amberStyle);
        m_dcBtn->setToolTip(tr("25 Hz high-pass filter at the chain tail "
                               "to strip any DC offset before transmit."));
        connect(m_dcBtn, &QPushButton::toggled,
                this, &StripFinalOutputPanel::applyDcBlock);
        col->addWidget(m_dcBtn);

        m_toneBtn = new QPushButton("TONE", this);
        m_toneBtn->setCheckable(true);
        m_toneBtn->setFixedSize(56, 18);
        m_toneBtn->setStyleSheet(redCheckStyle);
        m_toneBtn->setToolTip(tr("1 kHz test tone injected at the head of "
                                 "the chain.  Right-click for the freq/"
                                 "level editor."));
        connect(m_toneBtn, &QPushButton::toggled,
                this, &StripFinalOutputPanel::applyTestToneEnabled);
        m_toneBtn->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(m_toneBtn, &QPushButton::customContextMenuRequested,
                this, [this](const QPoint&) { showToneEditor(); });
        col->addWidget(m_toneBtn);

        // Quindar tones (#2262) — Apollo-era K / BK or 2525/2475 Hz
        // sine on PTT engage/disengage.  Same red-checked styling as
        // TONE since both make audible sound.  Right-click opens the
        // style/freq/WPM editor.
        m_quinBtn = new QPushButton("QUIN", this);
        m_quinBtn->setCheckable(true);
        m_quinBtn->setFixedSize(56, 18);
        m_quinBtn->setStyleSheet(redCheckStyle);
        m_quinBtn->setToolTip(tr("Quindar tones on PTT engage/disengage. "
                                 "Right-click for the style/freq/WPM "
                                 "editor."));
        connect(m_quinBtn, &QPushButton::toggled,
                this, &StripFinalOutputPanel::applyQuindarEnabled);
        m_quinBtn->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(m_quinBtn, &QPushButton::customContextMenuRequested,
                this, [this](const QPoint&) { showQuindarEditor(); });
        col->addWidget(m_quinBtn);

        // Drop the trailing stretch — the OVR/LIMIT/% column on the
        // far right doesn't have one, so its three chips distribute
        // through the row's full height with extra space between them.
        // Matching that behaviour here makes the two columns look
        // visually identical.
        row->addLayout(col);
    }

    // Ceiling control was a separate knob; it now lives as a draggable
    // amber handle on the meter (mirroring the compressor's threshold
    // triangle).  See HorizMeter + setCeilingSetter() below.

    // Trim — master output gain post-limiter.  Caps at +12 / −12 dB
    // so users can boost average loudness above what the ceiling
    // alone would allow without giving them enough rope to dig into
    // distortion territory.
    m_trim = new ClientCompKnob;
    m_trim->setLabel("Trim");
    m_trim->setCenterLabelMode(true);
    m_trim->setRange(-12.0f, 12.0f);
    m_trim->setDefault(0.0f);
    m_trim->setLabelFormat([](float v) {
        return (v >= 0.0f ? "+" : "") + QString::number(v, 'f', 1) + " dB";
    });
    m_trim->setFixedSize(76, 76);
    connect(m_trim, &ClientCompKnob::valueChanged,
            this, &StripFinalOutputPanel::applyTrim);
    row->addWidget(m_trim);

    auto* meter = new HorizMeter(this);
    meter->setCeilingSetter([this](float db) { applyCeiling(db); });
    m_meter = meter;
    row->addWidget(meter, 1);

    // Numeric readouts column — PK, RMS, GR, CRST.  Three-letter
    // labels in muted blue, value text in series colour.  Matches
    // the meter's gradient palette so the eye links bar to numbers.
    {
        const QString labelCss =
            "QLabel { color: #506070; font-size: 9px; font-weight: bold;"
            " padding: 0; }";
        const QString valCssCyan =
            "QLabel { color: #56ccf2; font-size: 11px; font-weight: bold;"
            " padding: 0; }";
        const QString valCssGreen =
            "QLabel { color: #6fcf97; font-size: 11px; font-weight: bold;"
            " padding: 0; }";
        const QString valCssAmber =
            "QLabel { color: #f2c14e; font-size: 11px; font-weight: bold;"
            " padding: 0; }";
        const QString valCssWhite =
            "QLabel { color: #d7e7f2; font-size: 11px; font-weight: bold;"
            " padding: 0; }";

        auto* readGrid = new QGridLayout;
        readGrid->setHorizontalSpacing(4);
        readGrid->setVerticalSpacing(0);
        readGrid->setContentsMargins(0, 0, 0, 0);

        auto addRow = [&](int r, const QString& tag, const QString& valCss,
                          QLabel*& outVal) {
            auto* lbl = new QLabel(tag, this);
            lbl->setStyleSheet(labelCss);
            lbl->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
            outVal = new QLabel("--", this);
            outVal->setStyleSheet(valCss);
            outVal->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            outVal->setMinimumWidth(44);
            readGrid->addWidget(lbl,    r, 0);
            readGrid->addWidget(outVal, r, 1);
        };
        addRow(0, "PK",   valCssCyan,  m_pkValue);
        addRow(1, "RMS",  valCssGreen, m_rmsValue);
        addRow(2, "GR",   valCssAmber, m_grValue);
        addRow(3, "CRST", valCssWhite, m_crestValue);

        auto* readWrap = new QWidget(this);
        readWrap->setLayout(readGrid);
        readWrap->setFixedWidth(80);
        // Inherit the panel's background instead of getting the
        // strip's wide `QWidget { background: ... }` rule painted on
        // top — that produced a slightly darker stripe behind the
        // readouts vs. the rest of the panel.
        readWrap->setAttribute(Qt::WA_StyledBackground, false);
        readWrap->setStyleSheet("background: transparent;");
        row->addWidget(readWrap);
    }

    // Indicator stack — OVR latch on top, LIMIT in the middle, the
    // trailing-window activity % at the bottom.
    {
        auto* col = new QVBoxLayout;
        col->setContentsMargins(0, 0, 0, 0);
        col->setSpacing(2);

        // OVR latches red on a pre-limiter clip and self-clears
        // after 1.5 s; the panel can be clicked at any time to clear
        // immediately.  QPushButton over QLabel for the free clicked()
        // signal — the styling matches the LIMIT label below.
        auto* ovrBtn = new QPushButton("OVR", this);
        ovrBtn->setFixedSize(56, 18);
        ovrBtn->setCursor(Qt::PointingHandCursor);
        ovrBtn->setFlat(true);
        ovrBtn->setStyleSheet(
            "QPushButton { background: #1a2230; border: 1px solid #2a3744;"
            " border-radius: 3px; color: #506070; font-size: 10px;"
            " font-weight: bold; padding: 1px; }"
            "QPushButton:hover { color: #c8d8e8; }");
        ovrBtn->setToolTip(tr("Latches red on any sample that hits "
                              "0 dBFS BEFORE the limiter — i.e. the "
                              "limiter is the only thing that saved "
                              "you.  Click to clear."));
        connect(ovrBtn, &QPushButton::clicked, this, [this]() {
            m_ovrLatchUntilMs = 0;
            // Reset baseline so subsequent clip count delta doesn't
            // immediately re-trigger from old samples.
            if (m_audio) {
                if (auto* lim = m_audio->clientFinalLimiterTx())
                    m_lastClipCount = lim->clipPreLimiterCount();
            }
        });
        m_ovrLed = ovrBtn;
        col->addWidget(ovrBtn);

        m_limitLed = new QLabel("LIMIT", this);
        m_limitLed->setAlignment(Qt::AlignCenter);
        m_limitLed->setFixedSize(56, 18);
        m_limitLed->setStyleSheet(
            "QLabel { background: #1a2230; border: 1px solid #2a3744;"
            " border-radius: 3px; color: #506070; font-size: 10px;"
            " font-weight: bold; padding: 1px; }");
        col->addWidget(m_limitLed);

        m_activityLbl = new QLabel("Lim: 0%", this);
        m_activityLbl->setAlignment(Qt::AlignCenter);
        m_activityLbl->setFixedSize(56, 18);
        m_activityLbl->setStyleSheet(
            "QLabel { background: transparent; color: #506070;"
            " font-size: 10px; font-weight: bold; padding: 1px; }");
        m_activityLbl->setToolTip(tr(
            "Limiter activity — what percentage of the trailing 3 s of "
            "audio the brickwall limiter was actively clamping peaks.\n\n"
            "Greys at <50% (limiter mostly idle), ambers at 50–90% "
            "(working hard), reds above 90% (clamping nearly all the "
            "time — back off upstream gain)."));
        col->addWidget(m_activityLbl);

        row->addLayout(col);
    }

    root->addLayout(row);

    // Initial values from engine.
    syncControlsFromEngine();

    // 120 Hz MeterSmoother polling (project-canonical rate).
    m_meterTimer = new QTimer(this);
    m_meterTimer->setInterval(kMeterIntervalMs);
    connect(m_meterTimer, &QTimer::timeout,
            this, &StripFinalOutputPanel::tickMeters);
    m_animClock.start();
    m_meterTimer->start();
}

StripFinalOutputPanel::~StripFinalOutputPanel() = default;

void StripFinalOutputPanel::showForTx()
{
    show();
    raise();
    activateWindow();
}

void StripFinalOutputPanel::syncControlsFromEngine()
{
    if (!m_audio) return;
    auto* lim = m_audio->clientFinalLimiterTx();
    if (!lim) return;
    if (m_enable) {
        QSignalBlocker b(m_enable);
        m_enable->setChecked(lim->isEnabled());
    }
    if (m_trim) {
        QSignalBlocker b(m_trim);
        m_trim->setValue(lim->outputTrimDb());
    }
    if (m_dcBtn) {
        QSignalBlocker b(m_dcBtn);
        m_dcBtn->setChecked(lim->dcBlockEnabled());
    }
    if (m_toneBtn) {
        if (auto* tone = m_audio->clientTxTestTone()) {
            QSignalBlocker b(m_toneBtn);
            m_toneBtn->setChecked(tone->isEnabled());
        }
    }
    if (m_quinBtn) {
        if (auto* q = m_audio->clientQuindarTone()) {
            QSignalBlocker b(m_quinBtn);
            m_quinBtn->setChecked(q->isEnabled());
        }
    }
    // Seed the clip-count baseline so we don't latch OVR from
    // historical clips on startup or after preset reload.
    m_lastClipCount = lim->clipPreLimiterCount();
    m_ovrLatchUntilMs = 0;
}

void StripFinalOutputPanel::applyEnable(bool on)
{
    if (!m_audio) return;
    if (auto* lim = m_audio->clientFinalLimiterTx()) {
        lim->setEnabled(on);
        m_audio->saveClientFinalLimiterSettings();
    }
}

void StripFinalOutputPanel::applyCeiling(float db)
{
    if (!m_audio) return;
    if (auto* lim = m_audio->clientFinalLimiterTx()) {
        lim->setCeilingDb(db);
        m_audio->saveClientFinalLimiterSettings();
    }
}

void StripFinalOutputPanel::applyTrim(float db)
{
    if (!m_audio) return;
    if (auto* lim = m_audio->clientFinalLimiterTx()) {
        lim->setOutputTrimDb(db);
        m_audio->saveClientFinalLimiterSettings();
    }
}

void StripFinalOutputPanel::applyDcBlock(bool on)
{
    if (!m_audio) return;
    if (auto* lim = m_audio->clientFinalLimiterTx()) {
        lim->setDcBlockEnabled(on);
        m_audio->saveClientFinalLimiterSettings();
    }
}

void StripFinalOutputPanel::applyTestToneEnabled(bool on)
{
    if (!m_audio) return;
    if (auto* tone = m_audio->clientTxTestTone()) {
        tone->setEnabled(on);
    }
}

void StripFinalOutputPanel::applyTestToneFreq(float hz)
{
    if (!m_audio) return;
    if (auto* tone = m_audio->clientTxTestTone()) tone->setFrequencyHz(hz);
}

void StripFinalOutputPanel::applyTestToneLevel(float db)
{
    if (!m_audio) return;
    if (auto* tone = m_audio->clientTxTestTone()) tone->setLevelDb(db);
}

void StripFinalOutputPanel::showToneEditor()
{
    if (!m_audio) return;
    auto* tone = m_audio->clientTxTestTone();
    if (!tone) return;

    // Lightweight modal dialog — two sliders, live-updating.  Closed
    // by hitting Escape or clicking outside.  Tone state stays where
    // the user left it; the dialog is purely a setter UI.
    auto* dlg = new QDialog(this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowTitle(tr("Test Tone"));
    dlg->setStyleSheet(
        "QDialog { background: #08121d; }"
        "QLabel  { color: #c8d8e8; font-size: 11px; }"
        "QSlider::groove:horizontal { height: 4px; background: #203040;"
        " border-radius: 2px; }"
        "QSlider::sub-page:horizontal { background: #f2c14e;"
        " border-radius: 2px; }"
        "QSlider::handle:horizontal { width: 12px; height: 12px;"
        " margin: -4px 0; background: #c8d8e8; border: 1px solid #f2c14e;"
        " border-radius: 6px; }");

    auto* form = new QFormLayout(dlg);
    form->setContentsMargins(12, 12, 12, 12);
    form->setSpacing(10);

    // Frequency: log mapping 50 Hz → 5 kHz across slider 0..1000.
    auto* freqSlider = new QSlider(Qt::Horizontal, dlg);
    freqSlider->setRange(0, 1000);
    auto freqToSlider = [](float hz) {
        const float n = std::log(std::clamp(hz, 50.0f, 5000.0f) / 50.0f)
                      / std::log(100.0f);
        return static_cast<int>(std::round(n * 1000.0f));
    };
    auto sliderToFreq = [](int v) {
        const float n = static_cast<float>(v) / 1000.0f;
        return 50.0f * std::pow(100.0f, n);
    };
    freqSlider->setValue(freqToSlider(tone->frequencyHz()));

    auto* freqLbl = new QLabel(dlg);
    freqLbl->setMinimumWidth(72);
    freqLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    auto fmtFreq = [](float hz) {
        return hz >= 1000.0f
            ? QString::number(hz / 1000.0f, 'f', 2) + " kHz"
            : QString::number(hz, 'f', 0) + " Hz";
    };
    freqLbl->setText(fmtFreq(tone->frequencyHz()));
    connect(freqSlider, &QSlider::valueChanged, dlg,
            [this, freqLbl, sliderToFreq, fmtFreq](int v) {
        const float hz = sliderToFreq(v);
        applyTestToneFreq(hz);
        freqLbl->setText(fmtFreq(hz));
    });
    auto* freqRow = new QHBoxLayout;
    freqRow->addWidget(freqSlider, 1);
    freqRow->addWidget(freqLbl);
    form->addRow(tr("Freq"), freqRow);

    // Level: linear -60..0 dBFS.
    auto* lvlSlider = new QSlider(Qt::Horizontal, dlg);
    lvlSlider->setRange(-60, 0);
    lvlSlider->setValue(static_cast<int>(std::round(tone->levelDb())));
    auto* lvlLbl = new QLabel(dlg);
    lvlLbl->setMinimumWidth(72);
    lvlLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    lvlLbl->setText(QString::number(tone->levelDb(), 'f', 0) + " dBFS");
    connect(lvlSlider, &QSlider::valueChanged, dlg,
            [this, lvlLbl](int v) {
        applyTestToneLevel(static_cast<float>(v));
        lvlLbl->setText(QString::number(v) + " dBFS");
    });
    auto* lvlRow = new QHBoxLayout;
    lvlRow->addWidget(lvlSlider, 1);
    lvlRow->addWidget(lvlLbl);
    form->addRow(tr("Level"), lvlRow);

    dlg->resize(360, 120);
    dlg->show();
}

void StripFinalOutputPanel::applyQuindarEnabled(bool on)
{
    if (!m_audio) return;
    if (auto* q = m_audio->clientQuindarTone()) {
        q->setEnabled(on);
        m_audio->saveClientQuindarSettings();
    }
}

void StripFinalOutputPanel::showQuindarEditor()
{
    if (!m_audio) return;
    auto* q = m_audio->clientQuindarTone();
    if (!q) return;

    auto* dlg = new QDialog(this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setObjectName("quindarToneEditor");
    const bool frameless = AppSettings::instance()
                               .value("FramelessWindow", "True")
                               .toString()
                           == "True";
    // Custom chrome uses the same draggable title bar as the rest of
    // the strip's editors when frameless windows are enabled.
    if (frameless) {
        dlg->setWindowFlags(dlg->windowFlags() | Qt::FramelessWindowHint);
    }
    dlg->setWindowTitle(tr("Quindar Tones"));
    dlg->setStyleSheet(
        "QDialog { background: #08121d; }"
        "QLabel  { color: #c8d8e8; font-size: 11px; }"
        "QPushButton { background: #1a2230; color: #c8d8e8;"
        " border: 1px solid #2a3744; border-radius: 3px;"
        " padding: 4px 12px; font-size: 11px; }"
        "QPushButton:hover { background: #243042; color: #f2c14e; }"
        "QPushButton:checked { background: #3a2a0e; color: #f2c14e;"
        " border: 1px solid #f2c14e; }"
        "QSlider::groove:horizontal { height: 4px; background: #203040;"
        " border-radius: 2px; }"
        "QSlider::sub-page:horizontal { background: #f2c14e;"
        " border-radius: 2px; }"
        "QSlider::handle:horizontal { width: 12px; height: 12px;"
        " margin: -4px 0; background: #c8d8e8; border: 1px solid #f2c14e;"
        " border-radius: 6px; }"
        "QSpinBox { background: #1a2230; color: #c8d8e8;"
        " border: 1px solid #2a3744; border-radius: 2px;"
        " padding: 1px 4px; font-size: 11px; }");

    auto* dlgRoot = new QVBoxLayout(dlg);
    dlgRoot->setContentsMargins(0, 0, 0, 0);
    dlgRoot->setSpacing(0);

    // Title bar — matches the AppletPanel ContainerTitleBar look used
    // by TX Controls / RX Controls / etc. so the strip's editors
    // visually align with the rest of the docked applet stack.
    // Inline rather than reusing ContainerTitleBar directly because
    // ContainerTitleBar has float/close signals tied to
    // ContainerWidget lifecycle that don't apply to a modal dialog.
    auto* titleBar = new QWidget(dlg);
    titleBar->setObjectName("editorFramelessTitleBar");
    titleBar->setFixedHeight(18);
    titleBar->setVisible(frameless);
    titleBar->setAttribute(Qt::WA_StyledBackground, true);
    titleBar->setStyleSheet(
        "QWidget { background: qlineargradient(x1:0,y1:0,x2:0,y2:1,"
        "stop:0 #5a7494, stop:0.5 #384e68, stop:1 #1e2e3e); "
        "border-bottom: 1px solid #0a1a28; }");
    titleBar->setCursor(Qt::OpenHandCursor);
    auto* tbLayout = new QHBoxLayout(titleBar);
    tbLayout->setContentsMargins(6, 0, 2, 0);
    tbLayout->setSpacing(4);
    auto* tbTitle = new QLabel(tr("Quindar Tones"), titleBar);
    tbTitle->setStyleSheet(
        "QLabel { background: transparent; color: #e0ecf4;"
        " font-size: 10px; font-weight: bold; }");
    tbLayout->addWidget(tbTitle);
    tbLayout->addStretch();
    auto* tbCloseBtn = new QPushButton(QString::fromUtf8("\xc3\x97"), titleBar);
    tbCloseBtn->setFixedSize(16, 16);
    tbCloseBtn->setStyleSheet(
        "QPushButton { background: transparent; border: none;"
        " color: #c8d8e8; font-size: 11px; font-weight: bold;"
        " padding: 0px 4px; }"
        "QPushButton:hover { color: #ffffff; }");
    tbCloseBtn->setCursor(Qt::ArrowCursor);
    tbCloseBtn->setToolTip(tr("Close"));
    connect(tbCloseBtn, &QPushButton::clicked, dlg, &QDialog::accept);
    tbLayout->addWidget(tbCloseBtn);

    // Drag-to-move via the shared frameless move helper.  Single inline
    // event filter installed on the bar + label so a left-press anywhere
    // on the title (not the close button) moves the dialog.
    struct MoveFilter : public QObject {
        QWidget* host;
        explicit MoveFilter(QWidget* h) : QObject(h), host(h) {}
        bool eventFilter(QObject* /*o*/, QEvent* ev) override {
            if (ev->type() == QEvent::MouseMove) {
                return FramelessMoveHelper::move(host, static_cast<QMouseEvent*>(ev));
            }
            if (ev->type() == QEvent::MouseButtonRelease) {
                return FramelessMoveHelper::finish(host, static_cast<QMouseEvent*>(ev));
            }
            if (ev->type() == QEvent::MouseButtonPress) {
                auto* me = static_cast<QMouseEvent*>(ev);
                return FramelessMoveHelper::start(host, me);
            }
            return false;
        }
    };
    auto* mf = new MoveFilter(titleBar);
    titleBar->installEventFilter(mf);
    tbTitle->installEventFilter(mf);

    dlgRoot->addWidget(titleBar);

    // Body host — all the editor controls below use `outer` so the
    // existing layout code keeps working unchanged once this widget
    // owns the QVBoxLayout that was previously on the dialog itself.
    auto* bodyHost = new QWidget(dlg);
    auto* outer = new QVBoxLayout(bodyHost);
    outer->setContentsMargins(12, 12, 12, 12);
    outer->setSpacing(10);
    dlgRoot->addWidget(bodyHost);

    // ── Style segmented control ──
    auto* styleRow = new QHBoxLayout;
    styleRow->setSpacing(0);
    auto* styleLbl = new QLabel(tr("Style"), dlg);
    styleLbl->setMinimumWidth(48);
    auto* toneStyleBtn  = new QPushButton(tr("Tone"),  dlg);
    auto* morseStyleBtn = new QPushButton(tr("Morse"), dlg);
    toneStyleBtn->setCheckable(true);
    morseStyleBtn->setCheckable(true);
    const bool startMorse = (q->style() == ClientQuindarTone::Style::Morse);
    toneStyleBtn->setChecked(!startMorse);
    morseStyleBtn->setChecked(startMorse);
    styleRow->addWidget(styleLbl);
    styleRow->addWidget(toneStyleBtn);
    styleRow->addWidget(morseStyleBtn);
    styleRow->addStretch();
    outer->addLayout(styleRow);

    auto* form = new QFormLayout;
    form->setContentsMargins(0, 0, 0, 0);
    form->setSpacing(8);

    // Level
    auto* lvlSlider = new QSlider(Qt::Horizontal, dlg);
    lvlSlider->setRange(-20, 0);
    lvlSlider->setValue(static_cast<int>(std::round(q->levelDb())));
    auto* lvlLbl = new QLabel(dlg);
    lvlLbl->setMinimumWidth(72);
    lvlLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    lvlLbl->setText(QString::number(q->levelDb(), 'f', 0) + " dBFS");
    connect(lvlSlider, &QSlider::valueChanged, dlg,
            [this, lvlLbl](int v) {
        if (auto* qt = m_audio->clientQuindarTone()) {
            qt->setLevelDb(static_cast<float>(v));
            m_audio->saveClientQuindarSettings();
        }
        lvlLbl->setText(QString::number(v) + " dBFS");
    });
    auto* lvlRow = new QHBoxLayout;
    lvlRow->addWidget(lvlSlider, 1);
    lvlRow->addWidget(lvlLbl);
    form->addRow(tr("Level"), lvlRow);

    // ── Tone fields ──
    auto* toneIntroSpin  = new QSpinBox(dlg);
    toneIntroSpin->setRange(2400, 2700);
    toneIntroSpin->setSuffix(" Hz");
    toneIntroSpin->setValue(static_cast<int>(std::round(q->introFreqHz())));
    auto* toneOutroSpin  = new QSpinBox(dlg);
    toneOutroSpin->setRange(2400, 2700);
    toneOutroSpin->setSuffix(" Hz");
    toneOutroSpin->setValue(static_cast<int>(std::round(q->outroFreqHz())));
    auto* toneDurSpin    = new QSpinBox(dlg);
    toneDurSpin->setRange(100, 500);
    toneDurSpin->setSuffix(" ms");
    toneDurSpin->setValue(q->durationMs());
    form->addRow(tr("Intro"),    toneIntroSpin);
    form->addRow(tr("Outro"),    toneOutroSpin);
    form->addRow(tr("Duration"), toneDurSpin);

    // ── Morse fields ──
    auto* morseWpmSpin = new QSpinBox(dlg);
    morseWpmSpin->setRange(20, 60);
    morseWpmSpin->setSuffix(" WPM");
    morseWpmSpin->setValue(q->morseWpm());
    auto* morsePitchSlider = new QSlider(Qt::Horizontal, dlg);
    morsePitchSlider->setRange(400, 1200);
    morsePitchSlider->setValue(static_cast<int>(std::round(q->morsePitchHz())));
    auto* morsePitchLbl = new QLabel(dlg);
    morsePitchLbl->setMinimumWidth(72);
    morsePitchLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    morsePitchLbl->setText(QString::number(q->morsePitchHz(), 'f', 0) + " Hz");
    auto* morsePitchRow = new QHBoxLayout;
    morsePitchRow->addWidget(morsePitchSlider, 1);
    morsePitchRow->addWidget(morsePitchLbl);
    form->addRow(tr("WPM"),   morseWpmSpin);
    form->addRow(tr("Pitch"), morsePitchRow);

    outer->addLayout(form);

    // ── Test + Done row ──
    auto* testRow = new QHBoxLayout;
    auto* testIntroBtn = new QPushButton(tr("▶ Test intro"), dlg);
    auto* testOutroBtn = new QPushButton(tr("▶ Test outro"), dlg);
    auto* doneBtn      = new QPushButton(tr("Done"),         dlg);
    doneBtn->setDefault(true);
    testRow->addWidget(testIntroBtn);
    testRow->addWidget(testOutroBtn);
    testRow->addStretch();
    testRow->addWidget(doneBtn);
    outer->addLayout(testRow);


    auto refreshFieldsForStyle = [=]() {
        const bool morse = morseStyleBtn->isChecked();
        toneIntroSpin->setEnabled(!morse);
        toneOutroSpin->setEnabled(!morse);
        toneDurSpin->setEnabled(!morse);
        morseWpmSpin->setEnabled(morse);
        morsePitchSlider->setEnabled(morse);
        morsePitchLbl->setEnabled(morse);
    };
    refreshFieldsForStyle();

    auto setStyle = [=](bool morse) {
        toneStyleBtn->setChecked(!morse);
        morseStyleBtn->setChecked(morse);
        if (auto* qt = m_audio->clientQuindarTone()) {
            qt->setStyle(morse ? ClientQuindarTone::Style::Morse
                               : ClientQuindarTone::Style::Tone);
            m_audio->saveClientQuindarSettings();
        }
        refreshFieldsForStyle();
    };
    connect(toneStyleBtn,  &QPushButton::clicked, dlg, [=]() { setStyle(false); });
    connect(morseStyleBtn, &QPushButton::clicked, dlg, [=]() { setStyle(true);  });

    connect(toneIntroSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            dlg, [this](int v) {
        if (auto* qt = m_audio->clientQuindarTone()) {
            qt->setIntroFreqHz(static_cast<float>(v));
            m_audio->saveClientQuindarSettings();
        }
    });
    connect(toneOutroSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            dlg, [this](int v) {
        if (auto* qt = m_audio->clientQuindarTone()) {
            qt->setOutroFreqHz(static_cast<float>(v));
            m_audio->saveClientQuindarSettings();
        }
    });
    connect(toneDurSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            dlg, [this](int v) {
        if (auto* qt = m_audio->clientQuindarTone()) {
            qt->setDurationMs(v);
            m_audio->saveClientQuindarSettings();
        }
    });
    connect(morseWpmSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            dlg, [this](int v) {
        if (auto* qt = m_audio->clientQuindarTone()) {
            qt->setMorseWpm(v);
            m_audio->saveClientQuindarSettings();
        }
    });
    connect(morsePitchSlider, &QSlider::valueChanged, dlg,
            [this, morsePitchLbl](int v) {
        if (auto* qt = m_audio->clientQuindarTone()) {
            qt->setMorsePitchHz(static_cast<float>(v));
            m_audio->saveClientQuindarSettings();
        }
        morsePitchLbl->setText(QString::number(v) + " Hz");
    });

    // Test buttons drive the DSP module directly without flipping
    // MOX, so the user can audition the configured tone.  The
    // dedicated QuindarLocalSink runs on the operator's audio output
    // device whenever the engine has an RX stream open, so the
    // audition is audible immediately regardless of TX state.
    connect(testIntroBtn, &QPushButton::clicked, dlg, [this]() {
        if (auto* qt = m_audio->clientQuindarTone()) qt->startIntro();
    });
    connect(testOutroBtn, &QPushButton::clicked, dlg, [this]() {
        if (auto* qt = m_audio->clientQuindarTone()) qt->startOutro();
    });

    connect(doneBtn, &QPushButton::clicked, dlg, &QDialog::accept);

    dlg->resize(380, 280);
    dlg->show();
}

void StripFinalOutputPanel::tickMeters()
{
    if (!m_audio) return;
    auto* lim = m_audio->clientFinalLimiterTx();
    if (!lim) return;

    const float inPeak   = lim->inputPeakDb();
    const float outPeak  = lim->outputPeakDb();
    const float outRms   = lim->outputRmsDb();
    const float gr       = lim->gainReductionDb();
    const bool  active   = lim->active();
    const float activity = lim->limiterActivityPct();
    const quint64 clipCount = lim->clipPreLimiterCount();

    // Feed the smoothers in normalised [0, 1] space — setTarget every
    // tick with the latest accessor value, no "only rise" guard.  The
    // smoother's asymmetric attack/release does the right thing on
    // its own (matches ClientCompMeter, ClientCompApplet, etc.).
    // GR is mapped via -gr/30 so a 30 dB cut reads as a full bar
    // (matches the existing red-fill overlay scale in HorizMeter).
    auto dbRatio = [](float db) {
        if (db <= kMeterMinDb) return 0.0f;
        if (db >= kMeterMaxDb) return 1.0f;
        return (db - kMeterMinDb) / (kMeterMaxDb - kMeterMinDb);
    };
    m_inPeakSmooth.setTarget(dbRatio(inPeak));
    m_outPeakSmooth.setTarget(dbRatio(outPeak));
    m_outRmsSmooth.setTarget(dbRatio(outRms));
    m_grSmooth.setTarget(std::clamp(-gr / 30.0f, 0.0f, 1.0f));

    const qint64 dt = m_animClock.restart();
    m_inPeakSmooth.tick(dt);
    m_outPeakSmooth.tick(dt);
    m_outRmsSmooth.tick(dt);
    m_grSmooth.tick(dt);

    constexpr float kRange = kMeterMaxDb - kMeterMinDb;
    m_inPeakDb  = kMeterMinDb + m_inPeakSmooth.value()  * kRange;
    m_outPeakDb = kMeterMinDb + m_outPeakSmooth.value() * kRange;
    m_outRmsDb  = kMeterMinDb + m_outRmsSmooth.value()  * kRange;
    m_grDb      = -m_grSmooth.value() * 30.0f;

    m_active = active;

    // OVR latch — clip count is monotonic; any increase since last
    // tick lights the LED for ~1.5 s.
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (clipCount > m_lastClipCount) {
        m_ovrLatchUntilMs = nowMs + 1500;
        m_lastClipCount = clipCount;
    }
    const bool ovrActive = nowMs < m_ovrLatchUntilMs;

    auto fmtDb = [](float v) {
        if (v <= -100.0f) return QString("-∞");
        return QString::number(v, 'f', 1);
    };
    if (m_pkValue)    m_pkValue->setText(fmtDb(m_outPeakDb));
    if (m_rmsValue)   m_rmsValue->setText(fmtDb(m_outRmsDb));
    if (m_grValue)    m_grValue->setText(fmtDb(m_grDb));
    if (m_crestValue) {
        const float crest = m_outPeakDb - m_outRmsDb;
        m_crestValue->setText(QString::number(crest, 'f', 1));
    }

    // OVR + LIMIT styling.
    if (m_ovrLed) {
        m_ovrLed->setStyleSheet(
            ovrActive
                ? "QPushButton { background: #4a1818; border: 1px solid #ff4040;"
                  " border-radius: 3px; color: #ff4040; font-size: 10px;"
                  " font-weight: bold; padding: 1px; }"
                  "QPushButton:hover { color: #ffffff; }"
                : "QPushButton { background: #1a2230; border: 1px solid #2a3744;"
                  " border-radius: 3px; color: #506070; font-size: 10px;"
                  " font-weight: bold; padding: 1px; }"
                  "QPushButton:hover { color: #c8d8e8; }");
    }

    if (m_activityLbl) {
        const int pct = static_cast<int>(std::clamp(activity, 0.0f, 1.0f) * 100.0f);
        m_activityLbl->setText(QString("Lim: %1%").arg(pct));
        const QString tint = pct > 90 ? "#ff4040"
                           : pct > 50 ? "#f2c14e"
                                      : "#506070";
        m_activityLbl->setStyleSheet(
            QString("QLabel { background: transparent; color: %1;"
                    " font-size: 10px; font-weight: bold; padding: 1px; }")
                .arg(tint));
    }

    if (auto* hm = dynamic_cast<HorizMeter*>(m_meter)) {
        // Raw engine peak (outPeak) feeds the hold marker so single-
        // block clamps register; smoothed m_outPeakDb feeds the bar
        // gradient so it doesn't jitter visually.
        hm->setLevels(m_inPeakDb, m_outPeakDb, outPeak,
                      m_grDb, lim->ceilingDb());
    }
    if (m_limitLed) {
        // Flash red when actively clamping — alternate bright/dim each
        // tick so the eye picks it up at peripheral vision.
        if (m_active) m_limitFlashOn = !m_limitFlashOn;
        else          m_limitFlashOn = false;
        const char* css =
            !m_active
                ? "QLabel { background: #1a2230; border: 1px solid #2a3744;"
                  " border-radius: 3px; color: #506070; font-size: 10px;"
                  " font-weight: bold; padding: 1px; }"
            : m_limitFlashOn
                ? "QLabel { background: #ff3030; border: 1px solid #ff8080;"
                  " border-radius: 3px; color: #ffffff; font-size: 10px;"
                  " font-weight: bold; padding: 1px; }"
                : "QLabel { background: #4a1818; border: 1px solid #ff4040;"
                  " border-radius: 3px; color: #ff8080; font-size: 10px;"
                  " font-weight: bold; padding: 1px; }";
        m_limitLed->setStyleSheet(css);
    }
}

void StripFinalOutputPanel::setQuindarActive(bool active)
{
    if (m_quinActive == active) return;
    m_quinActive = active;
    if (!m_quinBtn) return;

    // Signal-driven flash — only re-applies the stylesheet when the
    // active state actually changes (twice per PTT: start of intro,
    // end of intro = start of Live; same for outro).  No polling.
    const QString css = active
        ? "QPushButton {"
          "  background: #ff3030; color: #ffffff;"
          "  border: 1px solid #ff8080;"
          "  border-radius: 3px; font-size: 10px;"
          "  font-weight: bold; padding: 1px;"
          "}"
        : "QPushButton {"
          "  background: #1a2230; border: 1px solid #2a3744;"
          "  border-radius: 3px; color: #506070;"
          "  font-size: 10px; font-weight: bold; padding: 1px;"
          "}"
          "QPushButton:hover { color: #c8d8e8; }"
          "QPushButton:checked {"
          "  background: #4a1818; color: #ff8080;"
          "  border: 1px solid #ff4040;"
          "}"
          "QPushButton:checked:hover { background: #5a2828; }";
    m_quinBtn->setStyleSheet(css);
}

} // namespace AetherSDR
