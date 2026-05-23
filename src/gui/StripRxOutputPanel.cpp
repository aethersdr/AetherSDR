#include "StripRxOutputPanel.h"

#include "ClientCompKnob.h"
#include "EditorFramelessTitleBar.h"
#include "core/AppSettings.h"
#include "core/AudioEngine.h"

#include <QDateTime>
#include <QFontMetrics>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLinearGradient>
#include <QPainter>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace {

// Same dark panel chrome as the other strip cells — frameless title
// bar on transparent background, dark inner fill.
constexpr const char* kWindowStyle =
    "MasterSDR--StripRxOutputPanel { background: #0e1b28;"
    " border: 1px solid #2a3a4a; border-radius: 4px; }"
    "QLabel { background: transparent; color: #8aa8c0; font-size: 11px; }";

constexpr float kMeterMinDb = -60.0f;
constexpr float kMeterMaxDb =   0.0f;

float dbToRatio(float db)
{
    if (db <= kMeterMinDb) return 0.0f;
    if (db >= kMeterMaxDb) return 1.0f;
    return (db - kMeterMinDb) / (kMeterMaxDb - kMeterMinDb);
}

// RX-side gradient meter — visual mirror of the TX HorizMeter (gradient
// fill, dB tick scale, peak-hold hairline) minus the limiter-specific
// bits (no ceiling drag handle, no GR overlay, no input-peak backdrop,
// no LIMIT band).  Single child widget; reads m_peak / m_rms on the
// parent through references so it has no internal state of its own.
//
// Bar gradient renders RMS (the slow, average-loudness reading), with
// a thin white hairline drawn at the bar's leading edge so the eye
// gets a precise level tick where the colour fades.  The cyan hairline
// holds the highest raw peak in the trailing 1.5 s window — the only
// visual indicator of instantaneous peaks.
class RxGradientMeter : public QWidget {
public:
    RxGradientMeter(const float& peak, const float& rms, QWidget* parent)
        : QWidget(parent), m_peak(peak), m_rms(rms)
    {
        // Match the TX meter's geometry: 22 px header (unused on RX —
        // no ceiling handle / value text) + 28 px bar + 22 px tick
        // label band underneath.
        setMinimumHeight(72);
        setMinimumWidth(180);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

    void notePeak(float rawPeakDb) {
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        if (rawPeakDb > m_holdDb || nowMs >= m_holdUntilMs) {
            if (rawPeakDb > m_holdDb) {
                m_holdDb = rawPeakDb;
                m_holdUntilMs = nowMs + 1500;
            } else {
                m_holdDb = rawPeakDb;
            }
        }
    }

protected:
    QRectF barRect() const {
        return QRectF(rect().left() + 1, rect().top() + 22,
                      rect().width() - 2, 28 - 2);
    }

    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const QRectF r = barRect();

        // Track background.
        p.setPen(QPen(QColor("#1a2a3a"), 1));
        p.setBrush(QColor("#0a0e16"));
        p.drawRoundedRect(r, 3, 3);

        // RMS bar — gradient green → amber → red across the full
        // track, plus a thin white tick at the bar's leading edge so
        // the eye gets a precise level indicator where the colour fades.
        const float rmsFrac = dbToRatio(m_rms);
        if (rmsFrac > 0.0f) {
            QLinearGradient g(r.left(), 0, r.right(), 0);
            g.setColorAt(0.00, QColor("#30c060"));
            g.setColorAt(dbToRatio(-12.0f), QColor("#a0c030"));
            g.setColorAt(dbToRatio(-3.0f),  QColor("#d49030"));
            g.setColorAt(1.00, QColor("#c03030"));
            QRectF outR = r;
            outR.setWidth(rmsFrac * r.width());
            p.setPen(Qt::NoPen);
            p.setBrush(g);
            p.drawRoundedRect(outR, 3, 3);
            // Leading-edge tick — sits at the right edge of the fill so
            // the bar visually "follows" the line as RMS moves.
            const double rx = r.left() + rmsFrac * r.width();
            p.setPen(QPen(QColor("#e0e0e0"), 1.5));
            p.drawLine(QPointF(rx, r.top() + 1), QPointF(rx, r.bottom() - 1));
        }

        // Peak-hold hairline — light cyan line at the highest raw
        // block peak in the last 1.5 s window.  The only visual cue
        // for instantaneous peaks; trails the bar's leading edge as
        // audio quiets so the operator can see headroom usage.
        if (m_holdDb > -100.0f) {
            const double hx = r.left() + dbToRatio(m_holdDb) * r.width();
            p.setPen(QPen(QColor("#a0e0ff"), 2.0));
            p.drawLine(QPointF(hx, r.top() + 1), QPointF(hx, r.bottom() - 1));
        }

        // dB scale — tick marks every 12 dB, labels in the band below.
        QFont tickFont = p.font();
        tickFont.setPointSize(7);
        tickFont.setBold(true);
        p.setFont(tickFont);
        const QFontMetrics tickFm(tickFont);
        for (int db = -60; db <= 0; db += 12) {
            const double tickX = r.left()
                + dbToRatio(static_cast<float>(db)) * r.width();
            p.setPen(QPen(QColor("#a0b4c8"), 1.2));
            p.drawLine(QPointF(tickX, r.bottom() - 4),
                       QPointF(tickX, r.bottom() + 4));
            const QString lbl = (db == 0) ? "0" : QString::number(db);
            const int lblW = tickFm.horizontalAdvance(lbl);
            double lblX = tickX - lblW / 2.0 - 1;
            const double minX = rect().left() + 2;
            const double maxX = rect().right() - 2 - (lblW + 2);
            if (lblX < minX) lblX = minX;
            if (lblX > maxX) lblX = maxX;
            const QRectF labelR(lblX, r.bottom() + 5, lblW + 2, 12);
            p.setPen(QColor("#7f93a5"));
            p.drawText(labelR, Qt::AlignCenter, lbl);
        }
    }

private:
    const float& m_peak;
    const float& m_rms;
    float  m_holdDb{-120.0f};
    qint64 m_holdUntilMs{0};
};

} // namespace

namespace MasterSDR {

StripRxOutputPanel::StripRxOutputPanel(AudioEngine* engine, QWidget* parent)
    : QWidget(parent)
    , m_audio(engine)
{
    const QString title = QString::fromUtf8("Masterial Output \xe2\x80\x94 RX");
    setWindowTitle(title);
    setStyleSheet(kWindowStyle);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 0, 8, 0);
    root->setSpacing(6);

    auto* titleBar = new EditorFramelessTitleBar;
    titleBar->setTitleText(title);
    titleBar->setControlsVisible(false);
    titleBar->setStyleSheet("background: transparent;");
    m_titleBar = titleBar;
    root->addWidget(titleBar);
    // 10 px breathing room between the title and the controls row —
    // matches the TX panel spacing so the two panels read as a pair.
    root->addSpacing(10);

    // Main row: Trim knob | gradient meter | PK/RMS/CRST readouts | MUTE/BOOST chips
    auto* row = new QHBoxLayout;
    row->setSpacing(8);

    // Trim — final post-DSP RX gain stage.  Mirrors the TX panel's
    // post-limiter trim knob: 76×76 px, ±12 dB range, default 0 dB,
    // centre-label readout in dB.
    m_trim = new ClientCompKnob;
    m_trim->setLabel("Trim");
    m_trim->setCenterLabelMode(true);
    m_trim->setRange(-12.0f, 12.0f);
    m_trim->setDefault(0.0f);
    m_trim->setLabelFormat([](float v) {
        return (v >= 0.0f ? "+" : "") + QString::number(v, 'f', 1) + " dB";
    });
    m_trim->setFixedSize(76, 76);
    if (m_audio) m_trim->setValue(m_audio->rxOutputTrimDb());
    connect(m_trim, &ClientCompKnob::valueChanged, this, [this](float db) {
        if (!m_audio) return;
        m_audio->setRxOutputTrimDb(db);
        auto& s = AppSettings::instance();
        s.setValue("RxOutputTrimDb", QString::number(db, 'f', 1));
        s.save();
    });
    row->addWidget(m_trim);

    auto* meter = new RxGradientMeter(m_peakDb, m_rmsDb, this);
    m_meter = meter;
    row->addWidget(meter, 1);

    // Numeric readouts column — PK / RMS / CRST.  Mirrors the TX panel's
    // styling: 9 px muted-blue tag labels, 11 px bold values, cyan PK,
    // green RMS, white CRST.
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
            outVal = new QLabel(QString::fromUtf8("-\xe2\x88\x9e"), this);
            outVal->setStyleSheet(valCss);
            outVal->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            outVal->setMinimumWidth(44);
            readGrid->addWidget(lbl,    r, 0);
            readGrid->addWidget(outVal, r, 1);
        };
        addRow(0, "PK",   valCssCyan,  m_peakLbl);
        addRow(1, "RMS",  valCssGreen, m_rmsLbl);
        addRow(2, "CRST", valCssWhite, m_crestLbl);

        auto* readWrap = new QWidget(this);
        readWrap->setLayout(readGrid);
        readWrap->setFixedWidth(80);
        readWrap->setAttribute(Qt::WA_StyledBackground, false);
        readWrap->setStyleSheet("background: transparent;");
        row->addWidget(readWrap);
    }

    // MUTE / BOOST chip column on the far right — sized + placed to
    // match the TX panel's OVR/LIMIT/Lim% chip column so the two
    // panels' right edges share a visual rhythm.
    {
        const QString muteStyle =
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
        const QString boostStyle =
            "QPushButton {"
            "  background: #1a2230; border: 1px solid #2a3744;"
            "  border-radius: 3px; color: #506070;"
            "  font-size: 10px; font-weight: bold; padding: 1px;"
            "}"
            "QPushButton:hover { color: #c8d8e8; }"
            "QPushButton:checked {"
            "  background: #1a4a2a; color: #80ff80;"
            "  border: 1px solid #40c060;"
            "}"
            "QPushButton:checked:hover { background: #285a38; }";

        auto* col = new QVBoxLayout;
        col->setContentsMargins(0, 0, 0, 0);
        col->setSpacing(2);

        m_muteBtn = new QPushButton(QStringLiteral("MUTE"), this);
        m_muteBtn->setCheckable(true);
        m_muteBtn->setFixedSize(56, 18);
        m_muteBtn->setStyleSheet(muteStyle);
        m_muteBtn->setToolTip(tr("Mute local audio output."));
        if (m_audio) m_muteBtn->setChecked(m_audio->isMuted());
        connect(m_muteBtn, &QPushButton::toggled, this, [this](bool on) {
            if (m_audio) m_audio->setMuted(on);
        });
        col->addWidget(m_muteBtn);

        m_boostBtn = new QPushButton(QStringLiteral("BOOST"), this);
        m_boostBtn->setCheckable(true);
        m_boostBtn->setFixedSize(56, 18);
        m_boostBtn->setStyleSheet(boostStyle);
        m_boostBtn->setToolTip(
            tr("Soft-knee tanh boost on the RX output (~2× gain on quiet "
               "passages, no hard clipping on loud ones)."));
        if (m_audio) m_boostBtn->setChecked(m_audio->rxBoost());
        connect(m_boostBtn, &QPushButton::toggled, this, [this](bool on) {
            if (m_audio) m_audio->setRxBoost(on);
        });
        col->addWidget(m_boostBtn);

        row->addLayout(col);
    }

    root->addLayout(row);

    // Wire scope-tap → meter.  scopeSamplesReady fires for both sides;
    // RX path filters tx=false.  Cross-thread queued connection — the
    // signal is emitted from the audio thread.
    if (m_audio) {
        connect(m_audio, &AudioEngine::scopeSamplesReady,
                this, &StripRxOutputPanel::onScopeSamples,
                Qt::QueuedConnection);
    }

    // 120 Hz animation tick — kMeterSmootherIntervalMs is the project's
    // canonical poll rate so this panel's ballistics match every other
    // meter in the app.
    m_animTimer = new QTimer(this);
    m_animTimer->setInterval(kMeterSmootherIntervalMs);
    connect(m_animTimer, &QTimer::timeout,
            this, &StripRxOutputPanel::tick);
    m_animClock.start();
    m_animTimer->start();
}

StripRxOutputPanel::~StripRxOutputPanel() = default;

void StripRxOutputPanel::showForRx()
{
    syncControlsFromEngine();
    show();
    raise();
    activateWindow();
}

void StripRxOutputPanel::syncControlsFromEngine()
{
    if (!m_audio) return;
    if (m_muteBtn) {
        QSignalBlocker b(m_muteBtn);
        m_muteBtn->setChecked(m_audio->isMuted());
    }
    if (m_boostBtn) {
        QSignalBlocker b(m_boostBtn);
        m_boostBtn->setChecked(m_audio->rxBoost());
    }
    if (m_trim) {
        QSignalBlocker b(m_trim);
        m_trim->setValue(m_audio->rxOutputTrimDb());
    }
}

void StripRxOutputPanel::onScopeSamples(const QByteArray& monoFloat32,
                                       int /*sampleRate*/, bool tx)
{
    if (tx) return;  // RX-only meter

    const int n = monoFloat32.size() / static_cast<int>(sizeof(float));
    if (n <= 0) return;

    const auto* samples = reinterpret_cast<const float*>(monoFloat32.constData());
    float blockPeak = 0.0f;
    double sumSq = 0.0;
    for (int i = 0; i < n; ++i) {
        const float a = std::fabs(samples[i]);
        if (a > blockPeak) blockPeak = a;
        sumSq += static_cast<double>(samples[i]) * samples[i];
    }
    const float blockRms = static_cast<float>(std::sqrt(sumSq / n));
    constexpr float kFloor = -120.0f;
    const float blockPeakDb = blockPeak > 0.0f
        ? 20.0f * std::log10(blockPeak) : kFloor;
    const float blockRmsDb  = blockRms > 0.0f
        ? 20.0f * std::log10(blockRms) : kFloor;

    // Feed the smoothers in normalised [0, 1] space.  setTarget on
    // every block — the smoother's asymmetric attack/release does
    // the right thing on its own (matches ClientCompMeter,
    // ClientCompApplet, etc.).  Brief peaks that get smoothed away
    // are still captured on the hairline via notePeak() below.
    m_peakSmooth.setTarget(dbToRatio(blockPeakDb));
    m_rmsSmooth.setTarget(dbToRatio(blockRmsDb));

    // Feed the raw block-peak into the meter's hold tracker so single
    // brief peaks register on the hairline before they decay out.
    if (m_meter) {
        static_cast<RxGradientMeter*>(m_meter)->notePeak(blockPeakDb);
    }
}

void StripRxOutputPanel::tick()
{
    // Advance both smoothers by the wall-clock elapsed since the last
    // tick — MeterSmoother integrates over real time so the animation
    // is timer-jitter-tolerant.
    const qint64 dt = m_animClock.restart();
    m_peakSmooth.tick(dt);
    m_rmsSmooth.tick(dt);

    // Map the smoothers' [0, 1] display values back to dB for the
    // labels and the gradient bar.  A ratio of 0 maps to -60 dB which
    // the formatter prints as "-∞".
    constexpr float kRange = kMeterMaxDb - kMeterMinDb;
    m_peakDb = kMeterMinDb + m_peakSmooth.value() * kRange;
    m_rmsDb  = kMeterMinDb + m_rmsSmooth.value()  * kRange;

    // Update text readouts and repaint the bar.
    auto fmt = [](float db) -> QString {
        if (db <= -60.0f) return QString::fromUtf8("-\xe2\x88\x9e");
        return QString::number(db, 'f', 1);
    };
    if (m_peakLbl) m_peakLbl->setText(fmt(m_peakDb));
    if (m_rmsLbl)  m_rmsLbl->setText(fmt(m_rmsDb));
    if (m_crestLbl) {
        // Crest factor (peak − RMS) — only meaningful when both readings
        // are above the noise floor; show "--" otherwise so the user
        // doesn't read a misleading 60 dB value while idle.
        if (m_peakDb > -60.0f && m_rmsDb > -60.0f) {
            const float crest = m_peakDb - m_rmsDb;
            m_crestLbl->setText(QString::number(crest, 'f', 1));
        } else {
            m_crestLbl->setText("--");
        }
    }
    if (m_meter)   m_meter->update();
}

} // namespace MasterSDR
