#include "PureSignalApplet.h"
#include "GuardedSlider.h"
#include "Theme.h"
#include "core/AppSettings.h"
#include "core/ThemeManager.h"
#include "core/backends/IRadioBackend.h"
#include "core/backends/RadioCapabilities.h"

#include <QAccessible>
#include <QCheckBox>
#include <QDir>
#include <QFileDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <utility>

namespace AetherSDR {
namespace {
constexpr int kScaleMax = 256;      // IsFeedbackLevelOK's ceiling
constexpr int kWindowLow = 128;     // FeedbackInRange: 128 < fb <= 181
constexpr int kWindowHigh = 181;
constexpr int kAttMin = -28;        // MI0BOT: the HL2 goes negative
constexpr int kAttMax = 31;

// calcc.c's _calcc_state, in its order: what the status map's engineState is.
enum EngineState {
    LReset, LWait, LMoxDelay, LSetup, LCollect, MoxCheck, LCalc, LDelay, LStayOn, LTurnOn
};

quint64 nextRequest()
{
    // GUI thread only. A range of its own, apart from other extension clients.
    static quint64 sequence = 0x5053000000000000ULL;   // "PS"
    return ++sequence;
}

QJsonObject preferences()
{
    return QJsonDocument::fromJson(AppSettings::instance()
        .value(QStringLiteral("PureSignalApplet"), QString{}).toString().toUtf8()).object();
}

void styleButton(QPushButton* button)
{
    ThemeManager::instance().applyStyleSheet(button,
        "QPushButton { background: {{color.background.1}};"
        " border: 1px solid {{color.border.subtle}}; border-radius: 3px;"
        " color: {{color.text.primary}}; font-size: 10px; font-weight: bold; padding: 2px 4px; }"
        "QPushButton:hover { background: {{color.background.2}}; }"
        "QPushButton:disabled { color: {{color.control.unavailable}}; }");
}

void secondary(QLabel* label)
{
    ThemeManager::instance().applyStyleSheet(label,
        "QLabel { color: {{color.text.secondary}}; font-size: 11px; }");
}

void setReadout(QLabel* label, const QString& text)
{
    if (label->text() == text) { return; }
    label->setText(text);
    label->setAccessibleName(text);
    QAccessibleEvent event(label, QAccessible::NameChanged);
    QAccessible::updateAccessibility(&event);
}

QString defaultCorrectionDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
}
} // namespace

// ---------------------------------------------------------------- meter

PureSignalFeedbackMeter::PureSignalFeedbackMeter(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("pureSignalFeedbackMeter"));
    setAccessibleName(tr("PureSignal feedback level"));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this,
            qOverload<>(&QWidget::update));
    m_animTimer.setTimerType(Qt::PreciseTimer);
    m_animTimer.setInterval(kMeterSmootherIntervalMs);
    connect(&m_animTimer, &QTimer::timeout, this, [this] {
        if (!m_smooth.tick(m_animElapsed.restart())) { m_animTimer.stop(); }
        update();
    });
}

PureSignalFeedbackMeter::Band PureSignalFeedbackMeter::bandFor(int level) noexcept
{
    if (level < 0) { return Band::None; }
    if (level > kWindowHigh) { return Band::High; }
    if (level > kWindowLow) { return Band::Good; }
    if (level > 90) { return Band::Marginal; }
    return Band::Low;
}

void PureSignalFeedbackMeter::setLevel(int level)
{
    if (level == m_level) { return; }
    m_level = level;
    setAccessibleDescription(level < 0 ? tr("No feedback") : QString::number(level));
    m_smooth.setTarget(level < 0 ? 0.0f
                                 : static_cast<float>(std::min(level, kScaleMax)) / kScaleMax);
    if (level < 0) { m_smooth.snapToTarget(); }   // no data: empty now, not decaying
    if (!m_smooth.needsAnimation()) {
        m_animTimer.stop();
    } else if (!m_animTimer.isActive()) {
        m_animElapsed.restart();
        m_animTimer.start();
    }
    update();
}

void PureSignalFeedbackMeter::paintEvent(QPaintEvent*)
{
    ThemeManager& theme = ThemeManager::instance();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF track = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    QPainterPath clip;
    clip.addRoundedRect(track, 3, 3);
    p.fillPath(clip, theme.color(this, QStringLiteral("color.background.0")));
    p.save();
    p.setClipPath(clip);

    const auto xAt = [&](double v) {
        return track.left() + track.width() * v / kScaleMax;
    };
    // calcc's target window.
    QColor window = theme.color(this, QStringLiteral("color.accent.success"));
    window.setAlpha(40);
    p.fillRect(QRectF(QPointF(xAt(kWindowLow), track.top()),
                      QPointF(xAt(kWindowHigh), track.bottom())), window);

    if (m_level >= 0) {
        QString token;
        switch (bandFor(m_level)) {
        case Band::High:     token = QStringLiteral("color.accent"); break;
        case Band::Good:     token = QStringLiteral("color.accent.success"); break;
        case Band::Marginal: token = QStringLiteral("color.accent.warning"); break;
        default:             token = QStringLiteral("color.accent.danger"); break;
        }
        const double shown = static_cast<double>(m_smooth.value()) * kScaleMax;
        QRectF fill = track.adjusted(1.5, 3, 0, -3);
        fill.setRight(std::max(fill.left(), xAt(shown) - 1.5));
        p.setPen(Qt::NoPen);
        p.setBrush(theme.color(this, token));
        p.drawRoundedRect(fill, 2, 2);
    }

    // Window edges.
    QColor edge = theme.color(this, QStringLiteral("color.text.secondary"));
    edge.setAlpha(140);
    p.setPen(QPen(edge, 1));
    for (int v : {kWindowLow, kWindowHigh}) {
        const double x = std::round(xAt(v)) + 0.5;
        p.drawLine(QPointF(x, track.top()), QPointF(x, track.bottom()));
    }
    p.restore();

    p.setPen(QPen(theme.color(this, QStringLiteral("color.border.subtle")), 1));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(track, 3, 3);

    QFont f = font();
    f.setPixelSize(10);
    f.setBold(true);
    p.setFont(f);
    p.setPen(theme.color(this, QStringLiteral("color.text.primary")));
    p.drawText(track.adjusted(4, 0, -5, 0), Qt::AlignRight | Qt::AlignVCenter,
               m_level < 0 ? QStringLiteral("—") : QString::number(m_level));
}

// ---------------------------------------------------------------- applet

PureSignalApplet::PureSignalApplet(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("PureSignalApplet"));
    setAccessibleName(tr("PureSignal"));
    theme::setContainer(this, QStringLiteral("applet/puresignal"));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(2, 2, 2, 2);
    root->setSpacing(3);

    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(4);
    m_enable = new QPushButton(tr("PureSignal"), this);
    m_enable->setObjectName(QStringLiteral("pureSignalEnable"));
    m_enable->setCheckable(true);
    m_enable->setFixedHeight(22);
    m_enable->setAccessibleName(tr("PureSignal on"));
    m_enable->setAccessibleDescription(
        tr("Adaptive pre-distortion from the HL2's own feedback receiver"));
    applyToggleButtonStyle(m_enable, ToggleTribe::Success);
    buttons->addWidget(m_enable, 3);
    m_auto = new QPushButton(tr("Auto"), this);
    m_auto->setObjectName(QStringLiteral("pureSignalAuto"));
    m_auto->setCheckable(true);
    m_auto->setFixedHeight(22);
    m_auto->setAccessibleName(tr("PureSignal automatic calibration"));
    applyToggleButtonStyle(m_auto, ToggleTribe::Accent);
    buttons->addWidget(m_auto, 2);
    m_single = new QPushButton(tr("Single"), this);
    m_single->setObjectName(QStringLiteral("pureSignalSingle"));
    // Lit from the press until that one calibration is done, so the press is
    // visible while nothing is keyed yet. The backend decides; a click only
    // asks.
    m_single->setCheckable(true);
    m_single->setFixedHeight(22);
    m_single->setAccessibleName(tr("PureSignal single calibration"));
    m_single->setToolTip(tr("Calibrate once on the next transmission"));
    applyToggleButtonStyle(m_single, ToggleTribe::Warning);
    buttons->addWidget(m_single, 2);
    root->addLayout(buttons);

    auto* meterRow = new QHBoxLayout;
    meterRow->setSpacing(5);
    auto* fbLabel = new QLabel(tr("FB"), this);
    secondary(fbLabel);
    fbLabel->setToolTip(tr("Feedback level. Keep it inside the marked window (128–181)."));
    meterRow->addWidget(fbLabel);
    m_meter = new PureSignalFeedbackMeter(this);
    fbLabel->setBuddy(m_meter);
    meterRow->addWidget(m_meter, 1);
    root->addLayout(meterRow);

    m_readout = new QLabel(this);
    m_readout->setObjectName(QStringLiteral("pureSignalReadout"));
    m_readout->setTextFormat(Qt::PlainText);
    secondary(m_readout);
    root->addWidget(m_readout);

    m_state = new QLabel(this);
    m_state->setObjectName(QStringLiteral("pureSignalState"));
    m_state->setTextFormat(Qt::PlainText);
    m_state->setWordWrap(true);
    root->addWidget(m_state);

    m_settingsToggle = new QPushButton(tr("▸ Settings"), this);
    m_settingsToggle->setObjectName(QStringLiteral("pureSignalSettingsToggle"));
    m_settingsToggle->setCheckable(true);
    m_settingsToggle->setAccessibleName(tr("PureSignal settings"));
    styleButton(m_settingsToggle);
    root->addWidget(m_settingsToggle);

    m_settingsDrawer = new QFrame(this);
    m_settingsDrawer->setObjectName(QStringLiteral("pureSignalSettingsDrawer"));
    auto* drawer = new QVBoxLayout(m_settingsDrawer);
    drawer->setContentsMargins(5, 4, 5, 5);
    drawer->setSpacing(3);
    m_autoAttenuate = new QCheckBox(tr("Auto-attenuate feedback"), m_settingsDrawer);
    m_autoAttenuate->setObjectName(QStringLiteral("pureSignalAutoAttenuate"));
    m_autoAttenuate->setAccessibleName(tr("PureSignal automatic feedback attenuation"));
    drawer->addWidget(m_autoAttenuate);

    auto* attRow = new QHBoxLayout;
    attRow->setSpacing(5);
    auto* attLabel = new QLabel(tr("TX att"), m_settingsDrawer);
    secondary(attLabel);
    attLabel->setToolTip(tr("Receive attenuation while keyed; the LNA runs at 19 − att dB"));
    attRow->addWidget(attLabel);
    auto* slider = new GuardedSlider(Qt::Horizontal, m_settingsDrawer);
    m_attenuation = slider;
    m_attenuation->setObjectName(QStringLiteral("pureSignalAttenuation"));
    m_attenuation->setAccessibleName(tr("PureSignal TX attenuation"));
    m_attenuation->setRange(kAttMin, kAttMax);
    m_attenuation->setSingleStep(1);
    m_attenuation->setPageStep(5);
    applyPrimarySliderStyle(m_attenuation);
    attLabel->setBuddy(m_attenuation);
    attRow->addWidget(m_attenuation, 1);
    m_attenuationValue = new QLabel(m_settingsDrawer);
    m_attenuationValue->setMinimumWidth(38);
    m_attenuationValue->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    secondary(m_attenuationValue);
    attRow->addWidget(m_attenuationValue);
    drawer->addLayout(attRow);

    auto* fileRow = new QHBoxLayout;
    fileRow->setSpacing(4);
    m_reset = new QPushButton(tr("Reset"), m_settingsDrawer);
    m_reset->setObjectName(QStringLiteral("pureSignalReset"));
    m_reset->setAccessibleName(tr("Reset PureSignal correction"));
    m_save = new QPushButton(tr("Save…"), m_settingsDrawer);
    m_save->setObjectName(QStringLiteral("pureSignalSave"));
    m_save->setAccessibleName(tr("Save PureSignal correction"));
    m_restore = new QPushButton(tr("Restore…"), m_settingsDrawer);
    m_restore->setObjectName(QStringLiteral("pureSignalRestore"));
    m_restore->setAccessibleName(tr("Restore PureSignal correction"));
    for (QPushButton* b : {m_reset, m_save, m_restore}) {
        b->setFixedHeight(22);
        styleButton(b);
        fileRow->addWidget(b, 1);
    }
    drawer->addLayout(fileRow);
    root->addWidget(m_settingsDrawer);

    // Auto, auto-attenuate and the attenuation belong to the radio: the
    // backend keeps them per radio and reports them in the status map. This
    // widget keeps only its own chrome (the drawer).
    const QJsonObject prefs = preferences();

    connect(m_enable, &QPushButton::clicked, this, [this](bool on) {
        send(QStringLiteral("enable"), on);
        // The backend's answer decides what the button shows.
        QSignalBlocker block(m_enable);
        m_enable->setChecked(m_status.enabled);
    });
    connect(m_auto, &QPushButton::clicked, this, [this](bool on) {
        send(QStringLiteral("auto"), on);
    });
    connect(m_single, &QPushButton::clicked, this, [this] {
        send(QStringLiteral("single"));
        QSignalBlocker block(m_single);
        m_single->setChecked(m_status.singleCalibrating);
    });
    connect(m_autoAttenuate, &QCheckBox::toggled, this, [this](bool on) {
        m_attenuation->setEnabled(!on);
        send(QStringLiteral("autoatt"), on);
    });
    // One request per setting, not one per drag step: the backend persists
    // each one.
    m_attenuation->setTracking(false);
    connect(m_attenuation, &QSlider::sliderMoved, this, [this](int att) { showAttenuation(att); });
    connect(m_attenuation, &QSlider::valueChanged, this, [this](int att) {
        showAttenuation(att);
        send(QStringLiteral("att"), att);
    });
    connect(m_reset, &QPushButton::clicked, this, [this] {
        send(QStringLiteral("reset"));
    });
    connect(m_save, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getSaveFileName(this, tr("Save PureSignal correction"),
            QDir(defaultCorrectionDir()).filePath(QStringLiteral("hl2-puresignal.psc")),
            tr("PureSignal correction (*.psc);;All files (*)"));
        if (!path.isEmpty()) { send(QStringLiteral("save"), path); }
    });
    connect(m_restore, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(this, tr("Restore PureSignal correction"),
            defaultCorrectionDir(), tr("PureSignal correction (*.psc);;All files (*)"));
        if (!path.isEmpty()) { send(QStringLiteral("restore"), path); }
    });
    connect(m_settingsToggle, &QPushButton::toggled, this, &PureSignalApplet::setSettingsExpanded);

    {
        const QSignalBlocker b1(m_auto), b2(m_autoAttenuate), b3(m_attenuation);
        m_auto->setChecked(m_status.autoCalibrate);
        m_autoAttenuate->setChecked(m_status.autoAttenuate);
        m_attenuation->setValue(m_status.attenuationDb);
        m_attenuation->setEnabled(false);
    }
    showAttenuation(m_status.attenuationDb);
    setSettingsExpanded(prefs.value(QStringLiteral("settingsExpanded")).toBool(false));
    onStatus(m_status);
}

PureSignalApplet::~PureSignalApplet() = default;
QSize PureSignalApplet::sizeHint() const { return {260, layout()->sizeHint().height()}; }
QSize PureSignalApplet::minimumSizeHint() const { return {220, layout()->minimumSize().height()}; }

QString PureSignalApplet::engineStateName(int state)
{
    switch (state) {
    case LReset:    return tr("Reset");
    case LWait:     return tr("Waiting");
    case LMoxDelay: return tr("MOX delay");
    case LSetup:    return tr("Setup");
    case LCollect:  return tr("Collecting");
    case MoxCheck:  return tr("MOX check");
    case LCalc:     return tr("Calculating");
    case LDelay:    return tr("Delay");
    case LStayOn:   return tr("Holding");
    case LTurnOn:   return tr("Turning on");
    default:           return tr("State %1").arg(state);
    }
}

PureSignalView PureSignalView::fromFields(const QVariantMap& f, PureSignalView v)
{
    const auto b = [&f](const char* key, bool& out) {
        const auto it = f.constFind(QLatin1String(key));
        if (it != f.constEnd()) { out = it->toBool(); }
    };
    const auto i = [&f](const char* key, int& out) {
        const auto it = f.constFind(QLatin1String(key));
        if (it != f.constEnd()) { out = it->toInt(); }
    };
    b("available", v.available);
    if (f.contains(QStringLiteral("unavailableReason"))) {
        v.unavailableReason = f.value(QStringLiteral("unavailableReason")).toString();
    }
    b("enabled", v.enabled);
    b("autoCalibrate", v.autoCalibrate);
    b("autoAttenuate", v.autoAttenuate);
    b("calibrating", v.calibrating);
    b("singleCalibrating", v.singleCalibrating);
    b("correcting", v.correcting);
    i("feedbackLevel", v.feedbackLevel);
    i("calibrationCount", v.calibrationCount);
    i("engineState", v.engineState);
    i("attenuationDb", v.attenuationDb);
    i("attMin", v.attMin);
    i("attMax", v.attMax);
    b("keyed", v.keyed);
    return v;
}

bool PureSignalApplet::supports(const RadioCapabilities& caps)
{
    return caps.extensionNamespaces.contains(QStringLiteral("hl2"))
        && caps.extensions.value(QStringLiteral("hl2")).toMap()
               .value(QStringLiteral("puresignal")).toInt() == 1;
}

void PureSignalApplet::setBackend(IRadioBackend* backend)
{
    if (backend && !supports(backend->capabilities())) { backend = nullptr; }
    if (backend == m_backend.data()) { return; }
    const bool was = isAvailable();
    unbind();
    m_backend = backend;
    if (backend) {
        const QPointer<IRadioBackend> source = backend;
        m_connections.append(connect(backend, &IRadioBackend::extensionStatus, this,
            [this, source](const QString& ns, const QString& kind, const QVariantMap& fields) {
                if (source && source == m_backend && ns == QLatin1String("hl2")
                    && kind == QLatin1String("puresignal")) {
                    onFields(fields);
                }
            }));
        m_connections.append(connect(backend, &IRadioBackend::extensionResult, this,
            [this, source](quint64 id, const QVariant& value) {
                if (!source || source != m_backend || !m_pending.remove(id)) { return; }
                onFields(value.toMap());
            }));
        m_connections.append(connect(backend, &IRadioBackend::extensionError, this,
            [this, source](quint64 id, const QString& reason) {
                if (!source || source != m_backend || !m_pending.remove(id)) { return; }
                m_error = reason;
                onStatus(m_status);
            }));
        // A family switch destroys the backend without unbinding it first.
        m_connections.append(connect(backend, &QObject::destroyed, this, [this] {
            unbind();
            emit availabilityChanged(false);
        }));
        send(QStringLiteral("get"));
    } else {
        onStatus(m_status);
    }
    if (was != isAvailable()) { emit availabilityChanged(isAvailable()); }
}

void PureSignalApplet::unbind()
{
    for (const QMetaObject::Connection& c : std::as_const(m_connections)) { disconnect(c); }
    m_connections.clear();
    m_pending.clear();
    m_error.clear();
    m_backend.clear();
    // Keep the operator's switches; everything the radio reported goes.
    PureSignalView gone;
    gone.autoCalibrate = m_status.autoCalibrate;
    gone.autoAttenuate = m_status.autoAttenuate;
    gone.attenuationDb = m_status.attenuationDb;
    m_status = gone;
    onStatus(m_status);
}

void PureSignalApplet::send(const QString& verb, const QVariant& arg)
{
    if (!m_backend) { return; }
    m_error.clear();
    const quint64 id = nextRequest();
    m_pending.insert(id);
    m_backend->invokeExtension(QStringLiteral("hl2"), QStringLiteral("puresignal.") + verb, id, arg);
}

void PureSignalApplet::onFields(const QVariantMap& fields)
{
    if (fields.isEmpty()) { return; }
    onStatus(PureSignalView::fromFields(fields, m_status));
}

void PureSignalApplet::showAttenuation(int att)
{
    m_attenuationValue->setText(tr("%1 dB").arg(att));
}

void PureSignalApplet::onStatus(const PureSignalView& s)
{
    if (m_attenuation && s.attMin < s.attMax
        && (m_attenuation->minimum() != s.attMin || m_attenuation->maximum() != s.attMax)) {
        const QSignalBlocker block(m_attenuation);
        m_attenuation->setRange(s.attMin, s.attMax);
    }
    // The backend owns every value shown here, the switches included.
    m_status = s;

    const bool bound = !m_backend.isNull();
    const bool usable = bound && (s.available || s.enabled);
    {
        const QSignalBlocker block(m_enable);
        m_enable->setChecked(s.enabled);
    }
    m_enable->setEnabled(usable);
    {
        // A dimmed button says why to a screen reader too, not only in the
        // state line below it.
        const QString why = bound && !s.available && !s.enabled
            ? (s.unavailableReason.isEmpty() ? tr("Unavailable") : s.unavailableReason)
            : tr("Adaptive pre-distortion from the HL2's own feedback receiver");
        if (m_enable->accessibleDescription() != why)
            m_enable->setAccessibleDescription(why);
    }
    m_auto->setEnabled(bound);
    m_single->setEnabled(bound && s.enabled);
    m_reset->setEnabled(bound && s.enabled);
    m_save->setEnabled(bound && s.enabled && s.correcting);
    m_restore->setEnabled(bound && s.enabled);
    {
        // Follow the backend without echoing it back.
        const QSignalBlocker b1(m_auto), b2(m_autoAttenuate), b3(m_attenuation), b4(m_single);
        m_auto->setChecked(s.autoCalibrate);
        m_single->setChecked(s.enabled && s.singleCalibrating);
        m_autoAttenuate->setChecked(s.autoAttenuate);
        m_autoAttenuate->setEnabled(bound);
        m_attenuation->setEnabled(bound && !s.autoAttenuate);
        if (!m_attenuation->isSliderDown() && m_attenuation->value() != s.attenuationDb) {
            m_attenuation->setValue(s.attenuationDb);
            showAttenuation(s.attenuationDb);
        }
    }

    m_meter->setLevel(s.enabled && (s.keyed || s.feedbackLevel > 0) ? s.feedbackLevel : -1);
    setReadout(m_readout, s.enabled
        ? tr("Att %1 dB · Cal %2 · %3").arg(s.attenuationDb).arg(s.calibrationCount)
              .arg(engineStateName(s.engineState))
        : tr("Att %1 dB").arg(m_attenuation->value()));

    QString text;
    QString token = QStringLiteral("color.text.secondary");
    if (!bound) {
        text = tr("Hermes-Lite 2 only");
    } else if (!m_error.isEmpty()) {
        text = m_error;
        token = QStringLiteral("color.accent.danger");
    } else if (!s.available && !s.enabled) {
        text = s.unavailableReason.isEmpty() ? tr("Unavailable") : s.unavailableReason;
        token = QStringLiteral("color.accent.warning");
    } else if (!s.enabled) {
        text = tr("Off — uses DDC 2 and 3 for feedback");
    } else if (s.correcting) {
        text = s.keyed ? tr("Correcting") : tr("Correction held — ready");
        token = QStringLiteral("color.accent.success");
    } else if (s.calibrating) {
        text = s.keyed ? tr("Calibrating…") : tr("Armed — key into a load to calibrate");
        token = QStringLiteral("color.accent.warning");
    } else {
        text = tr("On — press Auto or Single to calibrate");
    }
    if (m_state->text() != text || m_state->property("token").toString() != token) {
        m_state->setProperty("token", token);
        ThemeManager::instance().applyStyleSheet(m_state,
            QStringLiteral("QLabel { color: {{%1}}; font-size: 11px; font-weight: bold; }").arg(token));
        setReadout(m_state, text);
    }
}

void PureSignalApplet::setSettingsExpanded(bool expanded)
{
    m_settingsDrawer->setVisible(expanded);
    {
        const QSignalBlocker block(m_settingsToggle);
        m_settingsToggle->setChecked(expanded);
    }
    m_settingsToggle->setText(expanded ? tr("▾ Settings") : tr("▸ Settings"));
    m_settingsToggle->setAccessibleDescription(expanded ? tr("Expanded") : tr("Collapsed"));
    setMinimumHeight(minimumSizeHint().height());
    updateGeometry();
    if (QWidget* parent = parentWidget()) { parent->updateGeometry(); }
    savePreferences();
}

void PureSignalApplet::savePreferences()
{
    if (!m_settingsDrawer || !m_attenuation) { return; }
    // GUI chrome only; the radio's PureSignal settings live in the backend's
    // radio_settings document.
    QJsonObject document;
    document.insert(QStringLiteral("settingsExpanded"), !m_settingsDrawer->isHidden());
    const QString text = QString::fromUtf8(QJsonDocument(document).toJson(QJsonDocument::Compact));
    if (AppSettings::instance().value(QStringLiteral("PureSignalApplet"), QString{}).toString() == text) {
        return;
    }
    AppSettings::instance().setValue(QStringLiteral("PureSignalApplet"), text);
    AppSettings::instance().save();
}

} // namespace AetherSDR
