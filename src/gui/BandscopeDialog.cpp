#include "BandscopeDialog.h"

#include "ClientEqFftAnalyzer.h"
#include "core/ThemeManager.h"
#include "core/backends/IRadioBackend.h"
#include "core/backends/RadioCapabilities.h"
#include "models/RadioModel.h"

#include <QColor>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPushButton>
#include <QSizePolicy>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariantMap>

#include <algorithm>
#include <cmath>

namespace AetherSDR {

namespace {

// The analyzer is a fixed-size transform and the record length is a property of
// the radio, so the two have to agree. They do, for the only radio that
// declares the capability today — 2048 words is the depth of the HL2's capture
// FIFO — and a radio whose record is a different length is REFUSED with a
// reason rather than silently zero-padded or truncated, either of which would
// put a frequency axis on the window that was wrong by a factor.
constexpr int kRequiredSamples = ClientEqFftAnalyzer::kFftSize;

// How long a frame request is allowed to go unanswered before this window
// stops waiting for it. NOT A PROTOCOL TIMEOUT and deliberately not tuned to
// one: the backend answers or fails a bandscope.frame on its own schedule and
// has its own guard timer, so this is only the outer bound on an answer that is
// never coming at all because the object that owed it was destroyed. Generous
// on purpose — an answer arriving at 2.9 s is a slow radio, not a lost one, and
// expiring early would show the operator a failure that did not happen. No
// radio has ever answered this verb, so the real distribution is NOT KNOWN.
constexpr int kRequestDeadlineMs = 3000;

QString formatMhz(double hz)
{
    return QStringLiteral("%1").arg(hz / 1e6, 0, 'f', 1);
}

}  // namespace

// ---------------------------------------------------------------------------
// BandscopeTrace
// ---------------------------------------------------------------------------

BandscopeTrace::BandscopeTrace(QWidget* parent)
    : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setAccessibleName(tr("Wideband converter spectrum"));
    setAccessibleDescription(
        tr("Signal level across the radio's converter range, in decibels "
           "relative to full scale. Uncalibrated."));

    auto& tm = ThemeManager::instance();
    // Raw-QPainter widgets bypass applyStyleSheet's reverse map, so the tokens
    // they read have to be declared for the Theme Editor to find them.
    tm.declareWidgetTokens(this, QStringList{
        QStringLiteral("color.background.spectrum"),
        QStringLiteral("color.spectrum.trace"),
        QStringLiteral("color.spectrum.grid"),
        QStringLiteral("color.text.secondary"),
    });
    // A stylesheet-painted widget gets this for free; a painted one must ask.
    connect(&tm, &ThemeManager::themeChanged, this, qOverload<>(&QWidget::update));
}

void BandscopeTrace::setFrame(const QVector<float>& binsDb, double sampleRateHz)
{
    m_bins = binsDb;
    m_sampleRateHz = sampleRateHz;
    update();
}

void BandscopeTrace::clearFrame()
{
    m_bins.clear();
    m_sampleRateHz = 0.0;
    update();
}

void BandscopeTrace::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const auto& theme = ThemeManager::instance();

    const QRectF r = QRectF(rect());
    p.fillRect(r, theme.color(this, QStringLiteral("color.background.spectrum")));

    // Room on the left for the dB labels and below for the MHz labels.
    const QRectF plot = r.adjusted(44, 10, -10, -20);
    if (plot.width() < 8 || plot.height() < 8)
        return;

    drawGrid(p, plot);
    drawTrace(p, plot);
}

void BandscopeTrace::drawGrid(QPainter& p, const QRectF& plot) const
{
    const auto& theme = ThemeManager::instance();
    const QColor grid = theme.color(this, QStringLiteral("color.spectrum.grid"));
    const QColor text = theme.color(this, QStringLiteral("color.text.secondary"));

    QFont f = font();
    f.setPixelSize(10);
    p.setFont(f);

    // Horizontal: every 20 dB across the fixed window.
    p.setPen(QPen(grid, 1));
    for (float db = kTopDb; db >= kBottomDb; db -= 20.0f) {
        const double y = plot.top()
            + plot.height() * double(kTopDb - db) / double(kTopDb - kBottomDb);
        p.setPen(QPen(grid, 1));
        p.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
        p.setPen(text);
        p.drawText(QRectF(0, y - 7, plot.left() - 6, 14),
                   Qt::AlignRight | Qt::AlignVCenter,
                   QStringLiteral("%1").arg(int(db)));
    }

    // Vertical: five frequency ticks, labelled from the radio's own sample
    // rate rather than from a hardcoded span — the span is whatever the
    // converter's first Nyquist zone is on the radio that answered.
    if (m_sampleRateHz <= 0.0)
        return;
    const double spanHz = m_sampleRateHz / 2.0;
    constexpr int kTicks = 5;
    for (int i = 0; i <= kTicks; ++i) {
        const double frac = double(i) / kTicks;
        const double x = plot.left() + plot.width() * frac;
        p.setPen(QPen(grid, 1));
        p.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
        p.setPen(text);
        p.drawText(QRectF(x - 30, plot.bottom() + 2, 60, 16),
                   Qt::AlignHCenter | Qt::AlignTop, formatMhz(spanHz * frac));
    }
}

void BandscopeTrace::drawTrace(QPainter& p, const QRectF& plot) const
{
    if (m_bins.size() < 2)
        return;

    const auto& theme = ThemeManager::instance();
    const QColor trace = theme.color(this, QStringLiteral("color.spectrum.trace"));

    const double span = double(kTopDb - kBottomDb);
    const auto yOf = [&](float db) {
        const double clamped = std::clamp(double(db), double(kBottomDb),
                                          double(kTopDb));
        return plot.top() + plot.height() * (double(kTopDb) - clamped) / span;
    };

    QPainterPath line;
    const double step = plot.width() / double(m_bins.size() - 1);
    for (qsizetype i = 0; i < m_bins.size(); ++i) {
        const double x = plot.left() + i * step;
        const double y = yOf(m_bins.at(i));
        if (i == 0)
            line.moveTo(x, y);
        else
            line.lineTo(x, y);
    }

    QPainterPath fill = line;
    fill.lineTo(plot.right(), plot.bottom());
    fill.lineTo(plot.left(), plot.bottom());
    fill.closeSubpath();
    QColor wash = trace;
    wash.setAlpha(60);
    p.fillPath(fill, wash);

    p.setPen(QPen(trace, 1.2));
    p.setBrush(Qt::NoBrush);
    p.drawPath(line);
}

// ---------------------------------------------------------------------------
// BandscopeDialog
// ---------------------------------------------------------------------------

BandscopeDialog::BandscopeDialog(RadioModel* model, QWidget* parent)
    : PersistentDialog(QStringLiteral("Wideband Bandscope"),
                       QStringLiteral("BandscopeDialogGeometry"), parent)
    , m_model(model)
{
    theme::setContainer(this, QStringLiteral("dialog/bandscope"));
    setMinimumSize(420, 260);
    resize(900, 380);

    auto* root = new QVBoxLayout(bodyWidget());
    root->setSpacing(8);

    auto* intro = new QLabel(
        tr("The radio's converter, before the receiver's tuning and filtering. "
           "Levels are UNCALIBRATED and are on the converter's own scale — they "
           "are comparable with its clipping threshold and with nothing else."));
    intro->setWordWrap(true);
    ThemeManager::instance().applyStyleSheet(intro,
        "QLabel { color: {{color.text.secondary}}; font-size: 11px; }");
    root->addWidget(intro);

    m_trace = new BandscopeTrace;
    root->addWidget(m_trace, 1);

    auto* buttonRow = new QHBoxLayout;
    m_status = new QLabel;
    m_status->setWordWrap(true);
    ThemeManager::instance().applyStyleSheet(m_status,
        "QLabel { color: {{color.text.secondary}}; font-size: 11px; }");
    buttonRow->addWidget(m_status, 1);
    m_refresh = new QPushButton(tr("Refresh"));
    m_refresh->setAutoDefault(false);
    m_refresh->setToolTip(tr("Ask the radio for one more frame."));
    connect(m_refresh, &QPushButton::clicked, this, &BandscopeDialog::requestFrame);
    buttonRow->addWidget(m_refresh);
    auto* closeBtn = new QPushButton(tr("Close"));
    closeBtn->setAutoDefault(false);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);
    buttonRow->addWidget(closeBtn);
    root->addLayout(buttonRow);

    // The deadline on an outstanding request. Single-shot, started next to the
    // invoke and stopped by releaseRequest(), so it is running exactly when a
    // request is outstanding and at no other time. This is NOT a refresh timer:
    // a window that has drawn its frame has no timer running at all.
    m_deadline = new QTimer(this);
    m_deadline->setSingleShot(true);
    m_deadline->setInterval(kRequestDeadlineMs);
    connect(m_deadline, &QTimer::timeout, this, [this] {
        releaseRequest();
        if (m_refresh)
            m_refresh->setEnabled(true);
        // The trace is LEFT ALONE. An unanswered request says nothing about the
        // frame already on screen, and blanking it would destroy a reading the
        // operator may still be looking at. The status line carries the news.
        //
        // AN ANSWER ARRIVING AFTER THIS IS DROPPED, not drawn: releaseRequest()
        // has already torn down the lambdas that would have matched its id.
        // That is the safe direction — a frame the operator stopped waiting
        // for must not appear as the answer to their next press — and the
        // recovery is a press of Refresh, which either succeeds or is refused
        // by the backend with "already pending", which is itself the truth.
        showStatus(tr("The radio did not answer. Press Refresh to ask again."));
    });

    // THE LINK GOING AWAY. Without this the window is the one surface in this
    // change that can keep showing a dead radio's picture, with a peak readout,
    // for as long as it is left open — every other surface the bandscope work
    // touches (the health rows, resetBandscopeMirrors()) was built so a stale
    // value goes ABSENT rather than stale. capabilitiesChanged is the single
    // connection RadioModel documents for "the capability picture is now
    // different, re-read it", and it fires on every connect/disconnect edge.
    //
    // Only the ABSENT case acts. A republication that still carries the record
    // is a live radio revising something else, and wiping a good frame for that
    // would be its own bug.
    if (m_model) {
        connect(m_model, &RadioModel::capabilitiesChanged, this,
                [this](bool connected, const RadioCapabilities& caps) {
            if (connected && caps.widebandConverterView)
                return;
            releaseRequest();
            if (m_refresh)
                m_refresh->setEnabled(true);
            m_trace->clearFrame();
            showStatus(connected
                ? tr("This radio does not provide a wideband converter view.")
                : tr("The radio disconnected. The last frame is no longer "
                     "current and has been cleared."));
        });
    }

    // One frame on open. THIS IS THE WHOLE REFRESH POLICY, plus the button and
    // the deadline above: no refresh timer, so a window left open costs the
    // radio and the I/O thread nothing at all after the first frame is drawn.
    requestFrame();
}

BandscopeDialog::~BandscopeDialog()
{
    releaseRequest();
}

void BandscopeDialog::releaseRequest()
{
    if (m_deadline)
        m_deadline->stop();
    QObject::disconnect(m_okConn);
    QObject::disconnect(m_errConn);
    m_okConn = {};
    m_errConn = {};
    m_requestId = 0;
}

void BandscopeDialog::showStatus(const QString& text)
{
    if (m_status)
        m_status->setText(text);
}

void BandscopeDialog::requestFrame()
{
    if (m_requestId != 0)
        return;   // one outstanding; the button stays disabled until it answers
    if (!m_model) {
        showStatus(tr("No radio."));
        return;
    }

    // Asked fresh every time. The backend object is replaced on reconnect and
    // on a family swap, so neither it nor the capability record may be cached.
    IRadioBackend* backend = m_model->backend();
    const RadioCapabilities caps = m_model->backendCapabilities();
    if (!backend || !caps.widebandConverterView) {
        m_trace->clearFrame();
        showStatus(tr("This radio does not provide a wideband converter view."));
        return;
    }
    const WidebandConverterView& wide = *caps.widebandConverterView;
    if (wide.blockSamples != kRequiredSamples) {
        m_trace->clearFrame();
        showStatus(tr("This radio delivers %1-sample records; this window can "
                      "only transform %2.")
                       .arg(wide.blockSamples)
                       .arg(kRequiredSamples));
        return;
    }

    // REQUEST IDS ARE NOT ALLOCATED CENTRALLY, and that is a trap rather than a
    // convenience. AutomationServer counts up from 1 (`++m_extensionRequestId`)
    // and RadioModel reserves UINT64_MAX for its inline Icom wake. A third
    // counter starting at 1 would match another caller's replies as its own and
    // have its own matched by theirs — both quietly, because every id is valid.
    // So this one starts far above any counter that increments per operator
    // action and is monotonic from there.
    constexpr quint64 kRequestIdBase = 0x0100000000000000ull;
    static quint64 nextId = kRequestIdBase;
    m_requestId = nextId++;
    const quint64 id = m_requestId;

    m_okConn = connect(backend, &IRadioBackend::extensionResult, this,
                       [this, id](quint64 replyId, const QVariant& value) {
        if (replyId != id)
            return;
        releaseRequest();
        onFrame(value);
    });
    m_errConn = connect(backend, &IRadioBackend::extensionError, this,
                        [this, id](quint64 replyId, const QString& reason) {
        if (replyId != id)
            return;
        releaseRequest();
        if (m_refresh)
            m_refresh->setEnabled(true);
        showStatus(tr("No frame: %1").arg(reason));
    });

    if (m_refresh)
        m_refresh->setEnabled(false);
    showStatus(tr("Waiting for a frame…"));
    // STARTED BEFORE THE INVOKE, for the same reason the two connections above
    // are made before it: invokeBackendExtension can emit extensionError
    // SYNCHRONOUSLY (not connected, already pending), and that handler calls
    // releaseRequest(). Arming the deadline afterwards would start a timer for
    // a request that had already been answered and released, and its timeout
    // would then overwrite the error the operator needs to read.
    if (m_deadline)
        m_deadline->start();
    m_model->invokeBackendExtension(wide.frameNamespace, wide.frameVerb, id);
}

void BandscopeDialog::onFrame(const QVariant& reply)
{
    if (m_refresh)
        m_refresh->setEnabled(true);

    const QVariantMap map = reply.toMap();
    const QList<float> samples = map.value(QStringLiteral("samples")).value<QList<float>>();
    const double sampleRateHz = map.value(QStringLiteral("sampleRateHz")).toDouble();
    // The backend states whether these levels have ever been compared with a
    // real signal. READ, not assumed: hardcoding "uncalibrated" here would make
    // the window unable to stop saying it if a radio ever arrived that could.
    // Today nothing sets it true, and the word is on the screen because of that
    // and not because it is baked in.
    const bool calibrated = map.value(QStringLiteral("calibrated"), false).toBool();
    if (samples.size() != qsizetype(kRequiredSamples) || sampleRateHz <= 0.0) {
        m_trace->clearFrame();
        showStatus(tr("The radio returned a record this window cannot read."));
        return;
    }

    // ONE analyzer per frame, reset first. ClientEqFftAnalyzer smooths its bins
    // with an attack/decay follower sized for a 25 Hz audio feed; frames here
    // arrive seconds apart and on request, so a smoothed bin would show a level
    // from the last time the operator pressed Refresh. reset() makes the next
    // update() report the transform unsmoothed — a contract
    // `bandscope_analyzer_test` pins, so a change to the follower breaks a test
    // rather than this window.
    ClientEqFftAnalyzer analyzer;
    analyzer.reset();
    analyzer.update(samples.constData(), int(samples.size()));

    // THE ANALYZER'S SCALE IS 6.02 dB LOW AND THIS WINDOW IS WHERE THAT STOPS
    // BEING A DETAIL. ClientEqFftAnalyzer normalises by 2/N, the single-sided
    // normalisation for an UNWINDOWED transform, and never removes the Hann
    // window's 0.5 coherent gain — so on its raw scale a converter sitting on
    // its rail reads -6, kTopDb = 0 is unreachable by any sine, and this
    // window's "Peak … dBFS" would disagree by 6 dB with the ADC-peak row in
    // Radio Health, which is a true time-domain peak of the same block in the
    // same units. The one comparison this window exists to enable is against
    // the clip threshold, so that error is the whole feature.
    //
    // Corrected HERE and not in the analyzer on purpose: the EQ editor has
    // drawn the uncorrected scale since it shipped and reads it only as a
    // shape. See ClientEqFftAnalyzer::coherentGainCorrectionDb().
    //
    // WHAT THIS DOES AND DOES NOT BUY. After it, a single sinusoid's bin is its
    // own amplitude in dBFS and agrees with the time-domain peak. A broadband
    // signal still does not: its energy is spread over many bins, so the peak
    // BIN sits below the peak SAMPLE by however wide the signal is. The two
    // readouts are on the same scale now; they are not the same measurement.
    //
    // ARITHMETIC, NOT A MEASUREMENT. No radio has answered this verb, so the
    // correction has never been checked against a converter driven to a known
    // level. What is checked is that it is the exact inverse of the window this
    // analyzer builds (`bandscope_analyzer_test`).
    const float corrDb = analyzer.coherentGainCorrectionDb();
    const std::vector<float>& db = analyzer.magnitudesDb();
    QVector<float> bins;
    bins.reserve(qsizetype(db.size()));
    for (const float v : db)
        bins.push_back(v + corrDb);
    m_trace->setFrame(bins, sampleRateHz);

    // The peak and where it is: the one number an operator reads this window
    // for. Stated as uncalibrated every time it is stated at all.
    //
    // BIN 0 IS EXCLUDED. It is DC, and a direct-sampling converter's DC offset
    // lands there whether or not anything is on the antenna, so a peak readout
    // that included it could report the converter's own bias as the strongest
    // signal on the band. The TRACE still draws it — hiding a bin the transform
    // produced would be a different kind of dishonesty — but the readout does
    // not name it. Nothing here has been run against a radio, so how large that
    // bin actually is on this hardware is NOT KNOWN.
    qsizetype peakBin = 1;
    for (qsizetype i = 2; i < bins.size(); ++i) {
        if (bins.at(i) > bins.at(peakBin))
            peakBin = i;
    }
    const double binHz = sampleRateHz / double(kRequiredSamples);
    showStatus(tr("Peak %1 dBFS%2 near %3 MHz · span 0 to %4 MHz · "
                  "one frame, on request")
                   .arg(double(bins.at(peakBin)), 0, 'f', 1)
                   .arg(calibrated ? QString() : tr(" (uncalibrated)"))
                   .arg(formatMhz(double(peakBin) * binHz))
                   .arg(formatMhz(sampleRateHz / 2.0)));
}

}  // namespace AetherSDR
