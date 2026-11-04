#include "PureSignalApplet.h"
#include "GuardedSlider.h"
#include "Theme.h"
#include "core/AppSettings.h"
#include "core/ThemeManager.h"
#include "core/backends/hl2/Hl2Backend.h"

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

    const auto xAt = [&](int v) {
        return track.left() + track.width() * static_cast<double>(v) / kScaleMax;
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
        const int shown = std::min(m_level, kScaleMax);
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
    qRegisterMetaType<hl2::Hl2PureSignalStatus>("AetherSDR::hl2::Hl2PureSignalStatus");
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
    m_single->setFixedHeight(22);
    m_single->setAccessibleName(tr("PureSignal single calibration"));
    m_single->setToolTip(tr("Calibrate once on the next transmission"));
    styleButton(m_single);
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
    m_attenuation->setRange(hl2::Hl2PureSignalLogic::kAttMin, hl2::Hl2PureSignalLogic::kAttMax);
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

    // Preferences: everything but the enable, which starts off every session.
    const QJsonObject prefs = preferences();
    m_status.autoCalibrate = prefs.value(QStringLiteral("autoCalibrate")).toBool(true);
    m_status.autoAttenuate = prefs.value(QStringLiteral("autoAttenuate")).toBool(true);
    m_status.attenuationDb = hl2::Hl2PureSignalLogic::clampAtt(
        prefs.value(QStringLiteral("attenuationDb")).toInt(hl2::Hl2PureSignalLogic::kAttMax));

    connect(m_enable, &QPushButton::clicked, this, [this](bool on) {
        if (m_backend) { m_backend->setPureSignalEnabled(on); }
        // The backend's answer decides what the button shows.
        QSignalBlocker block(m_enable);
        m_enable->setChecked(m_status.enabled);
    });
    connect(m_auto, &QPushButton::clicked, this, [this](bool on) {
        m_status.autoCalibrate = on;
        if (m_backend) { m_backend->setPureSignalAutoCalibrate(on); }
        savePreferences();
    });
    connect(m_single, &QPushButton::clicked, this, [this] {
        if (m_backend) { m_backend->pureSignalCalibrateOnce(); }
    });
    connect(m_autoAttenuate, &QCheckBox::toggled, this, [this](bool on) {
        m_status.autoAttenuate = on;
        m_attenuation->setEnabled(!on);
        if (m_backend) { m_backend->setPureSignalAutoAttenuate(on); }
        savePreferences();
    });
    connect(m_attenuation, &QSlider::valueChanged, this, [this](int att) {
        showAttenuation(att);
        m_status.attenuationDb = att;
        if (m_backend) { m_backend->setPureSignalAttenuationDb(att); }
        savePreferences();
    });
    connect(m_reset, &QPushButton::clicked, this, [this] {
        if (m_backend) { m_backend->pureSignalResetCorrection(); }
    });
    connect(m_save, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getSaveFileName(this, tr("Save PureSignal correction"),
            QDir(defaultCorrectionDir()).filePath(QStringLiteral("hl2-puresignal.psc")),
            tr("PureSignal correction (*.psc);;All files (*)"));
        if (!path.isEmpty() && m_backend) { m_backend->savePureSignalCorrection(path); }
    });
    connect(m_restore, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(this, tr("Restore PureSignal correction"),
            defaultCorrectionDir(), tr("PureSignal correction (*.psc);;All files (*)"));
        if (!path.isEmpty() && m_backend) { m_backend->restorePureSignalCorrection(path); }
    });
    connect(m_settingsToggle, &QPushButton::toggled, this, &PureSignalApplet::setSettingsExpanded);

    {
        const QSignalBlocker b1(m_auto), b2(m_autoAttenuate), b3(m_attenuation);
        m_auto->setChecked(m_status.autoCalibrate);
        m_autoAttenuate->setChecked(m_status.autoAttenuate);
        m_attenuation->setValue(m_status.attenuationDb);
        m_attenuation->setEnabled(!m_status.autoAttenuate);
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
    using L = hl2::Hl2PureSignalLogic;
    switch (state) {
    case L::LReset:    return tr("Reset");
    case L::LWait:     return tr("Waiting");
    case L::LMoxDelay: return tr("MOX delay");
    case L::LSetup:    return tr("Setup");
    case L::LCollect:  return tr("Collecting");
    case L::MoxCheck:  return tr("MOX check");
    case L::LCalc:     return tr("Calculating");
    case L::LDelay:    return tr("Delay");
    case L::LStayOn:   return tr("Holding");
    case L::LTurnOn:   return tr("Turning on");
    default:           return tr("State %1").arg(state);
    }
}

void PureSignalApplet::setBackend(hl2::Hl2Backend* backend)
{
    if (backend == m_backend.data()) { return; }
    for (const QMetaObject::Connection& c : std::as_const(m_connections)) { disconnect(c); }
    m_connections.clear();
    const bool was = isAvailable();
    m_backend = backend;
    hl2::Hl2PureSignalStatus fresh;
    fresh.autoCalibrate = m_status.autoCalibrate;
    fresh.autoAttenuate = m_status.autoAttenuate;
    fresh.attenuationDb = m_status.attenuationDb;
    if (backend) {
        m_connections.append(connect(backend, &hl2::Hl2Backend::pureSignalStatusChanged,
                                     this, &PureSignalApplet::onStatus));
        // A family switch destroys the backend without unbinding it first.
        m_connections.append(connect(backend, &QObject::destroyed, this, [this] {
            for (const QMetaObject::Connection& c : std::as_const(m_connections)) { disconnect(c); }
            m_connections.clear();
            m_backend.clear();
            hl2::Hl2PureSignalStatus gone;
            gone.autoCalibrate = m_status.autoCalibrate;
            gone.autoAttenuate = m_status.autoAttenuate;
            gone.attenuationDb = m_status.attenuationDb;
            onStatus(gone);
            emit availabilityChanged(false);
        }));
        pushPreferences();
        fresh.available = backend->pureSignalStatus().available;
        fresh.unavailableReason = backend->pureSignalStatus().unavailableReason;
    }
    onStatus(fresh);
    if (was != isAvailable()) { emit availabilityChanged(isAvailable()); }
}

void PureSignalApplet::pushPreferences()
{
    if (!m_backend) { return; }
    m_backend->setPureSignalAutoCalibrate(m_status.autoCalibrate);
    m_backend->setPureSignalAutoAttenuate(m_status.autoAttenuate);
    m_backend->setPureSignalAttenuationDb(m_status.attenuationDb);
}

void PureSignalApplet::showAttenuation(int att)
{
    m_attenuationValue->setText(tr("%1 dB").arg(att));
}

void PureSignalApplet::onStatus(const hl2::Hl2PureSignalStatus& s)
{
    // The operator's switches are this widget's; the backend reports the rest.
    const bool autoCal = m_status.autoCalibrate;
    const bool autoAtt = m_status.autoAttenuate;
    m_status = s;
    m_status.autoCalibrate = autoCal;
    m_status.autoAttenuate = autoAtt;

    const bool bound = !m_backend.isNull();
    const bool usable = bound && (s.available || s.enabled);
    {
        const QSignalBlocker block(m_enable);
        m_enable->setChecked(s.enabled);
    }
    m_enable->setEnabled(usable);
    m_auto->setEnabled(bound);
    m_single->setEnabled(bound && s.enabled);
    m_reset->setEnabled(bound && s.enabled);
    m_save->setEnabled(bound && s.enabled && s.correcting);
    m_restore->setEnabled(bound && s.enabled);

    // Auto-attenuate moves the attenuation; follow it without echoing it back.
    if (s.autoAttenuate && m_status.autoAttenuate && m_attenuation->value() != s.attenuationDb) {
        const QSignalBlocker block(m_attenuation);
        m_attenuation->setValue(s.attenuationDb);
        showAttenuation(s.attenuationDb);
    }
    if (!autoAtt) { m_status.attenuationDb = m_attenuation->value(); }

    m_meter->setLevel(s.enabled && (s.keyed || s.feedbackLevel > 0) ? s.feedbackLevel : -1);
    setReadout(m_readout, s.enabled
        ? tr("Att %1 dB · Cal %2 · %3").arg(s.attenuationDb).arg(s.calibrationCount)
              .arg(engineStateName(s.engineState))
        : tr("Att %1 dB").arg(m_attenuation->value()));

    QString text;
    QString token = QStringLiteral("color.text.secondary");
    if (!bound) {
        text = tr("Hermes-Lite 2 only");
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
    QJsonObject document = preferences();
    document.insert(QStringLiteral("autoCalibrate"), m_status.autoCalibrate);
    document.insert(QStringLiteral("autoAttenuate"), m_status.autoAttenuate);
    document.insert(QStringLiteral("attenuationDb"), m_attenuation->value());
    document.insert(QStringLiteral("settingsExpanded"), !m_settingsDrawer->isHidden());
    const QString text = QString::fromUtf8(QJsonDocument(document).toJson(QJsonDocument::Compact));
    if (AppSettings::instance().value(QStringLiteral("PureSignalApplet"), QString{}).toString() == text) {
        return;
    }
    AppSettings::instance().setValue(QStringLiteral("PureSignalApplet"), text);
    AppSettings::instance().save();
}

} // namespace AetherSDR
