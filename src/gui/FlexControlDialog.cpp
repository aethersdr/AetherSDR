#include "FlexControlDialog.h"

#include "GuardedSlider.h"
#include "SliceLabel.h"
#include "core/AppSettings.h"
#include "models/SliceModel.h"

#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QConicalGradient>
#include <QElapsedTimer>
#include <QEvent>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QRadialGradient>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSlider>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <utility>

namespace AetherSDR {

namespace {

constexpr double kWheelPi = 3.14159265358979323846;
constexpr double kWheelDetentsPerRev = 48.0;
constexpr double kWheelDetentsPerRad = kWheelDetentsPerRev / (2.0 * kWheelPi);
constexpr int kButtonHoldMs = 650;
constexpr int kDoubleTapGuardMs = 230;

constexpr const char* kFlexControlStyle = R"(
QWidget {
    color: #aeb9cc;
    background: #07101c;
    font-size: 14px;
}
QLabel {
    background: transparent;
}
QFrame#StatusFrame,
QFrame#DeviceFrame,
QFrame#ControlStrip,
QFrame#AuxCell {
    background: qlineargradient(x1:0,y1:0,x2:0,y2:1,
        stop:0 #111d2c, stop:1 #0a1421);
    border: 1px solid #233246;
    border-radius: 7px;
}
QFrame#KnobPanel,
QFrame#PushActions,
QFrame#StatusCell {
    background: #08111d;
    border: 1px solid #1c2a3b;
    border-radius: 7px;
}
QLabel#FlexMark {
    color: #e7edf4;
    font-size: 24px;
    font-weight: 800;
}
QLabel#SubMark,
QLabel#SectionLabel {
    color: #8d99ad;
    font-size: 11px;
    font-weight: 700;
}
QLabel#StatusValue {
    color: #d8e2ef;
    font-size: 15px;
    font-weight: 700;
}
QLabel#StatusValueDim {
    color: #8d99ad;
    font-size: 13px;
    font-weight: 600;
}
QLabel#CaptureHint {
    color: #8d99ad;
    background: #08111d;
    border: 1px solid #1c2a3b;
    border-radius: 7px;
    padding: 8px 10px;
    font-weight: 600;
}
QLabel#CaptureHint[armed="true"] {
    color: #dfffe5;
    background: #11271f;
    border-color: #65d379;
}
QLabel#PushHint {
    color: #8d99ad;
    font-size: 12px;
    font-weight: 700;
    min-height: 18px;
    max-height: 18px;
}
QLabel#LedDot {
    background: #152233;
    border: 1px solid #26374e;
    border-radius: 6px;
    min-width: 12px;
    max-width: 12px;
    min-height: 12px;
    max-height: 12px;
}
QLabel#LedDot[active="true"] {
    background: #65d379;
    border-color: #92f2a0;
}
QPushButton#FlexButton,
QPushButton#PushButton {
    color: #d8e2ef;
    background: qlineargradient(x1:0,y1:0,x2:0,y2:1,
        stop:0 #17283c, stop:1 #0b1625);
    border: 1px solid #31455f;
    border-radius: 7px;
    padding: 8px 12px;
    font-weight: 800;
}
QPushButton#FlexButton:hover,
QPushButton#PushButton:hover,
QPushButton#PhysicalButton:hover {
    border-color: #65d379;
    color: #ffffff;
}
QPushButton#FlexButton:pressed,
QPushButton#FlexButton[selected="true"],
QPushButton#PushButton:pressed {
    background: #173528;
    border-color: #65d379;
    color: #eaffef;
}
QPushButton#PhysicalButton {
    color: #d8e2ef;
    background: #0b1625;
    border: 1px solid #31455f;
    border-radius: 7px;
    padding: 7px 12px;
    font-size: 12px;
    font-weight: 800;
}
QPushButton#PhysicalButton[connected="true"] {
    color: #dfffe5;
    background: #173528;
    border-color: #65d379;
}
QPushButton#OptionButton {
    color: #d8e2ef;
    background: #0b1625;
    border: 1px solid #31455f;
    border-radius: 7px;
    padding: 6px 10px;
    font-size: 12px;
    font-weight: 800;
}
QPushButton#OptionButton:hover {
    border-color: #65d379;
    color: #ffffff;
}
QPushButton#OptionButton:checked {
    color: #dfffe5;
    background: #173528;
    border-color: #65d379;
}
QSlider::groove:horizontal {
    height: 5px;
    border-radius: 2px;
    background: #162437;
}
QSlider::sub-page:horizontal {
    background: #65d379;
    border-radius: 2px;
}
QSlider::handle:horizontal {
    width: 16px;
    margin: -6px 0;
    border-radius: 8px;
    background: #d8e2ef;
    border: 1px solid #65d379;
}
QComboBox {
    color: #d8e2ef;
    background: #08111d;
    border: 1px solid #26374e;
    border-radius: 6px;
    padding: 6px 8px;
    font-size: 12px;
    font-weight: 600;
}
QComboBox:hover {
    border-color: #65d379;
}
QComboBox::drop-down {
    border: 0;
    width: 18px;
}
QComboBox QAbstractItemView {
    color: #d8e2ef;
    background: #08111d;
    selection-background-color: #173528;
    selection-color: #ffffff;
    border: 1px solid #26374e;
}
)";

struct FlexActionDef {
    const char* id;
    const char* label;
};

const FlexActionDef kFlexActions[] = {
    {"WheelFrequency", "Tune Slice"},
    {"BandZoom", "Band Zoom"},
    {"SegmentZoom", "Segment Zoom"},
    {"WheelRit", "RIT (Receive Incremental Tuning)"},
    {"WheelXit", "XIT (Transmit Incremental Tuning)"},
    {"WheelVolume", "Slice AF"},
    {"WheelMasterAf", "Master AF"},
    {"WheelHeadphoneVolume", "Headphone Volume"},
    {"WheelAgcT", "AGCT (Automatic Gain Control Threshold)"},
    {"WheelApf", "APF (Audio Peaking Filter)"},
    {"ClearRit", "Clear RIT"},
    {"ClearXit", "Clear XIT"},
    {"ToggleApf", "Toggle APF"},
    {"NextSlice", "Change Active Slice"},
    {"SplitActiveSlice", "Split Active Slice"},
    {"ToggleMox", "MOX"},
    {"WheelPower", "RF Power"},
    {"WheelCwSpeed", "CW Speed"},
    {"CwxF1", "CWX Macro 1"},
    {"CwxF2", "CWX Macro 2"},
    {"CwxF3", "CWX Macro 3"},
    {"CwxF4", "CWX Macro 4"},
    {"CwxF5", "CWX Macro 5"},
    {"CwxF6", "CWX Macro 6"},
    {"CwxF7", "CWX Macro 7"},
    {"CwxF8", "CWX Macro 8"},
    {"CwxF9", "CWX Macro 9"},
    {"CwxF10", "CWX Macro 10"},
    {"CwxF11", "CWX Macro 11"},
    {"CwxF12", "CWX Macro 12"},
    {"StepUp", "Step Up"},
    {"StepDown", "Step Down"},
    {"ToggleTune", "Toggle Tune"},
    {"ToggleMute", "Toggle Mute"},
    {"ToggleLock", "Toggle Lock"},
    {"PrevSlice", "Previous Slice"},
    {"ToggleAgc", "Toggle AGC"},
    {"VolumeUp", "Slice AF Up"},
    {"VolumeDown", "Slice AF Down"},
    {"None", "None"},
};

QString labelForActionId(const QString& actionId)
{
    for (const auto& def : kFlexActions) {
        if (actionId == QLatin1String(def.id))
            return QString::fromLatin1(def.label);
    }
    return actionId.isEmpty() ? QStringLiteral("None") : actionId;
}

double normalizeWheelAngle(double radians)
{
    while (radians > kWheelPi)
        radians -= 2.0 * kWheelPi;
    while (radians < -kWheelPi)
        radians += 2.0 * kWheelPi;
    return radians;
}

int consumeWholeWheelSteps(double& accumulator)
{
    if (accumulator >= 1.0) {
        const int steps = static_cast<int>(std::floor(accumulator));
        accumulator -= steps;
        return steps;
    }
    if (accumulator <= -1.0) {
        const int steps = static_cast<int>(std::ceil(accumulator));
        accumulator -= steps;
        return steps;
    }
    return 0;
}

QString formatFrequency(double mhz)
{
    const auto hz = static_cast<long long>(std::llround(mhz * 1.0e6));
    const int mhzPart = static_cast<int>(hz / 1000000);
    const int khzPart = static_cast<int>((hz / 1000) % 1000);
    const int hzPart = static_cast<int>(hz % 1000);
    return QString("%1.%2.%3")
        .arg(mhzPart)
        .arg(khzPart, 3, 10, QChar('0'))
        .arg(hzPart, 3, 10, QChar('0'));
}

QString formatStep(int hz)
{
    if (hz >= 1000)
        return QString("%1 kHz").arg(hz / 1000.0, 0, 'f', hz % 1000 == 0 ? 0 : 1);
    return QString("%1 Hz").arg(hz);
}

void repolish(QWidget* widget)
{
    if (!widget)
        return;
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    widget->update();
}

bool isEscapeKeyEvent(QEvent* event)
{
    if (!event)
        return false;
    const QEvent::Type type = event->type();
    if (type != QEvent::ShortcutOverride
        && type != QEvent::KeyPress
        && type != QEvent::KeyRelease) {
        return false;
    }
    auto* keyEvent = static_cast<QKeyEvent*>(event);
    return keyEvent->key() == Qt::Key_Escape
        || keyEvent->key() == Qt::Key_Cancel;
}

bool isWheelActionId(const QString& actionId)
{
    // Latching Aux actions are only the modes that make the knob adjust a
    // continuous control. One-shot actions such as step, zoom, MOX,
    // active-slice advance, split, and macros should not leave an Aux mode on.
    return actionId == QLatin1String("WheelFrequency")
        || actionId == QLatin1String("WheelVolume")
        || actionId == QLatin1String("WheelPower")
        || actionId == QLatin1String("WheelRit")
        || actionId == QLatin1String("WheelXit")
        || actionId == QLatin1String("WheelMasterAf")
        || actionId == QLatin1String("WheelHeadphoneVolume")
        || actionId == QLatin1String("WheelAgcT")
        || actionId == QLatin1String("WheelApf")
        || actionId == QLatin1String("WheelCwSpeed");
}

} // namespace

class VirtualFlexControlWheel : public QWidget {
public:
    explicit VirtualFlexControlWheel(QWidget* parent = nullptr)
        : QWidget(parent)
    {
        setMinimumSize(270, 270);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setFocusPolicy(Qt::StrongFocus);
        setMouseTracking(true);
        setCursor(Qt::PointingHandCursor);
        setAccessibleName(QStringLiteral("FlexControl tuning knob"));
        setAccessibleDescription(QStringLiteral(
            "Click to capture mouse input for circular tuning. Press Escape to release."));

        m_frameClock.start();
        m_spinTimer.setInterval(16);
        m_spinTimer.setTimerType(Qt::PreciseTimer);
        connect(&m_spinTimer, &QTimer::timeout, this, [this] {
            tickSpin();
        });
    }

    bool captured() const { return m_captured; }

    void setStepCallback(std::function<void(int)> callback)
    {
        m_stepCallback = std::move(callback);
    }

    void setCaptureCallback(std::function<void(bool)> callback)
    {
        m_captureCallback = std::move(callback);
    }

    void setLooseness(int value)
    {
        m_looseness = std::clamp(value, 0, 100);
    }

    void animateExternalSteps(int steps)
    {
        if (steps == 0 || m_captured)
            return;
        const int limitedSteps = std::clamp(steps, -72, 72);
        const double kick = static_cast<double>(limitedSteps) * 0.34;
        m_velocity = std::clamp(m_velocity + kick, -22.0, 22.0);
        m_spinDispatchesSteps = false;
        m_frameClock.restart();
        if (!m_spinTimer.isActive())
            m_spinTimer.start();
    }

    void releaseCapture()
    {
        if (!m_captured)
            return;
        m_captured = false;
        m_velocity = 0.0;
        m_stepAccumulator = 0.0;
        m_spinDispatchesSteps = false;
        m_spinTimer.stop();
        if (mouseGrabber() == this)
            releaseMouse();
        if (keyboardGrabber() == this)
            releaseKeyboard();
        if (m_captureCallback)
            m_captureCallback(false);
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        if (!isEnabled())
            p.setOpacity(0.42);

        const QRectF knob = wheelRect();
        const QPointF center = knob.center();
        const double radius = knob.width() / 2.0;
        const QColor accent(m_captured ? "#65d379" : "#31455f");

        p.setPen(Qt::NoPen);
        QRadialGradient tableShadow(center + QPointF(0.0, radius * 0.22), radius * 1.15);
        tableShadow.setColorAt(0.0, QColor(0, 0, 0, 150));
        tableShadow.setColorAt(0.72, QColor(0, 0, 0, 70));
        tableShadow.setColorAt(1.0, QColor(0, 0, 0, 0));
        p.setBrush(tableShadow);
        p.drawEllipse(knob.adjusted(-12.0, 14.0, 12.0, 22.0));

        QConicalGradient rim(center, -m_rotation * 180.0 / kWheelPi);
        rim.setColorAt(0.00, QColor("#e0e8ef"));
        rim.setColorAt(0.12, QColor("#596b7a"));
        rim.setColorAt(0.28, QColor("#111a25"));
        rim.setColorAt(0.45, QColor("#9aa8b2"));
        rim.setColorAt(0.61, QColor("#263544"));
        rim.setColorAt(0.78, QColor("#060a10"));
        rim.setColorAt(1.00, QColor("#e0e8ef"));
        p.setBrush(rim);
        p.setPen(QPen(accent, m_captured ? 2.0 : 1.0));
        p.drawEllipse(knob);

        for (int i = 0; i < 96; ++i) {
            const bool major = (i % 8) == 0;
            const double a = m_rotation + (2.0 * kWheelPi * i / 96.0);
            const double inner = major ? radius * 0.78 : radius * 0.83;
            const double outer = radius * 0.96;
            const QPointF p1(center.x() + std::cos(a) * inner,
                             center.y() + std::sin(a) * inner);
            const QPointF p2(center.x() + std::cos(a) * outer,
                             center.y() + std::sin(a) * outer);
            p.setPen(QPen(major ? QColor("#c3ced9") : QColor("#5d7183"),
                          major ? 1.6 : 0.8));
            p.drawLine(p1, p2);
        }

        QRadialGradient face(center - QPointF(radius * 0.18, radius * 0.23), radius);
        face.setColorAt(0.00, QColor("#526270"));
        face.setColorAt(0.32, QColor("#293848"));
        face.setColorAt(0.70, QColor("#101925"));
        face.setColorAt(1.00, QColor("#060a10"));
        p.setBrush(face);
        p.setPen(QPen(QColor("#0b131d"), 1.5));
        p.drawEllipse(knob.adjusted(radius * 0.13, radius * 0.13,
                                    -radius * 0.13, -radius * 0.13));

        for (int i = 0; i < 8; ++i) {
            const double a = m_rotation + (2.0 * kWheelPi * i / 8.0);
            const QPointF inner(center.x() + std::cos(a) * radius * 0.36,
                                center.y() + std::sin(a) * radius * 0.36);
            const QPointF outer(center.x() + std::cos(a) * radius * 0.57,
                                center.y() + std::sin(a) * radius * 0.57);
            p.setPen(QPen(QColor(255, 255, 255, 18), 0.8));
            p.drawLine(inner, outer);
        }

        const double indentAngle = m_rotation - 0.78;
        const double indentRadius = radius * 0.205;
        const QPointF indent(center.x() + std::cos(indentAngle) * radius * 0.62,
                             center.y() + std::sin(indentAngle) * radius * 0.62);
        QRadialGradient indentGrad(indent - QPointF(radius * 0.06, radius * 0.07),
                                   radius * 0.30);
        indentGrad.setColorAt(0.00, QColor("#00040a"));
        indentGrad.setColorAt(0.58, QColor("#06101a"));
        indentGrad.setColorAt(0.82, QColor("#293b4a"));
        indentGrad.setColorAt(1.00, QColor("#8192a0"));
        p.setBrush(indentGrad);
        p.setPen(QPen(QColor("#03070d"), 1.2));
        p.drawEllipse(indent, indentRadius, indentRadius);
        p.setPen(QPen(QColor(255, 255, 255, 62), 1.0));
        p.drawArc(QRectF(indent.x() - indentRadius * 0.82, indent.y() - indentRadius * 0.82,
                         indentRadius * 1.64, indentRadius * 1.64), 40 * 16, 110 * 16);

        if (m_captured) {
            const QString hint = QStringLiteral("Press Esc\nto release");
            QFont hintFont = font();
            hintFont.setPointSize(10);
            hintFont.setBold(true);
            p.setFont(hintFont);
            const QRectF badge(center.x() - radius * 0.35, center.y() - 20.0,
                               radius * 0.70, 40.0);
            p.setPen(QPen(QColor("#65d379"), 1.0));
            p.setBrush(QColor(8, 17, 29, 235));
            p.drawRoundedRect(badge, 5.0, 5.0);
            p.setPen(QColor("#dfffe5"));
            p.drawText(badge, Qt::AlignCenter, hint);
        }
    }

    void mousePressEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::LeftButton && isEnabled()) {
            captureAt(event->position());
            event->accept();
            return;
        }
        QWidget::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        if (m_captured) {
            updateFromPoint(event->position());
            event->accept();
            return;
        }
        QWidget::mouseMoveEvent(event);
    }

    bool event(QEvent* event) override
    {
        if (m_captured && isEscapeKeyEvent(event)) {
            releaseCapture();
            event->accept();
            return true;
        }
        return QWidget::event(event);
    }

    void keyPressEvent(QKeyEvent* event) override
    {
        if (m_captured && isEscapeKeyEvent(event)) {
            releaseCapture();
            event->accept();
            return;
        }
        QWidget::keyPressEvent(event);
    }

    void hideEvent(QHideEvent* event) override
    {
        releaseCapture();
        QWidget::hideEvent(event);
    }

private:
    QRectF wheelRect() const
    {
        const double side = std::min(width() - 18.0, height() - 18.0);
        return QRectF((width() - side) / 2.0, (height() - side) / 2.0, side, side);
    }

    double angleForPoint(const QPointF& pos) const
    {
        const QPointF center = wheelRect().center();
        return std::atan2(pos.y() - center.y(), pos.x() - center.x());
    }

    void captureAt(const QPointF& pos)
    {
        m_captured = true;
        m_velocity = 0.0;
        m_stepAccumulator = 0.0;
        m_spinDispatchesSteps = true;
        m_lastAngle = angleForPoint(pos);
        m_frameClock.restart();
        setFocus(Qt::MouseFocusReason);
        grabMouse(QCursor(Qt::ClosedHandCursor));
        if (m_captureCallback)
            m_captureCallback(true);
        update();
    }

    void updateFromPoint(const QPointF& pos)
    {
        const QRectF knob = wheelRect();
        const QPointF center = knob.center();
        const double dx = pos.x() - center.x();
        const double dy = pos.y() - center.y();
        const double radius = std::hypot(dx, dy);
        if (radius < knob.width() * 0.10)
            return;

        const double angle = std::atan2(dy, dx);
        const double delta = normalizeWheelAngle(angle - m_lastAngle);
        m_lastAngle = angle;

        const double elapsedMs = std::max<qint64>(m_frameClock.restart(), 1);
        const double dt = static_cast<double>(elapsedMs) / 1000.0;
        const double instantVelocity = delta / dt;
        m_stepMultiplier = accelerationForVelocity(instantVelocity);
        applyRotationDelta(delta, true);

        const double loose = static_cast<double>(m_looseness) / 100.0;
        const double blend = 0.24 + loose * 0.34;
        m_velocity = m_velocity * (1.0 - blend) + instantVelocity * blend;
        m_spinDispatchesSteps = true;
        if (std::abs(m_velocity) > 0.05 && !m_spinTimer.isActive())
            m_spinTimer.start();
    }

    void tickSpin()
    {
        const double elapsedMs = std::max<qint64>(m_frameClock.restart(), 1);
        const double dt = static_cast<double>(elapsedMs) / 1000.0;
        if (std::abs(m_velocity) < 0.032) {
            m_velocity = 0.0;
            m_spinDispatchesSteps = false;
            m_spinTimer.stop();
            return;
        }

        m_stepMultiplier = accelerationForVelocity(m_velocity);
        applyRotationDelta(m_velocity * dt, m_captured && m_spinDispatchesSteps);
        const double loose = static_cast<double>(m_looseness) / 100.0;
        const double drag = 9.2 - loose * 6.7;
        m_velocity *= std::exp(-drag * dt);
    }

    int accelerationForVelocity(double radiansPerSecond) const
    {
        const double speed = std::abs(radiansPerSecond);
        if (speed >= 18.0) return 6;
        if (speed >= 14.0) return 5;
        if (speed >= 10.0) return 4;
        if (speed >= 7.0) return 3;
        if (speed >= 4.0) return 2;
        return 1;
    }

    void applyRotationDelta(double delta, bool dispatchSteps)
    {
        if (delta == 0.0)
            return;

        m_rotation = normalizeWheelAngle(m_rotation + delta);
        if (dispatchSteps && m_stepCallback) {
            m_stepAccumulator += delta * kWheelDetentsPerRad;
            const int steps = consumeWholeWheelSteps(m_stepAccumulator);
            if (steps != 0)
                m_stepCallback(steps * std::max(1, m_stepMultiplier));
        }
        update();
    }

    std::function<void(int)> m_stepCallback;
    std::function<void(bool)> m_captureCallback;
    QTimer m_spinTimer;
    QElapsedTimer m_frameClock;
    double m_rotation{0.0};
    double m_lastAngle{0.0};
    double m_velocity{0.0};
    double m_stepAccumulator{0.0};
    int m_looseness{45};
    int m_stepMultiplier{1};
    bool m_captured{false};
    bool m_spinDispatchesSteps{false};
};

namespace {

class FlexControlButton : public QPushButton {
public:
    using ActionCallback = std::function<void(int)>;

    explicit FlexControlButton(const QString& text, QWidget* parent = nullptr)
        : QPushButton(text, parent)
    {
        setObjectName(QStringLiteral("FlexButton"));
        setCursor(Qt::PointingHandCursor);
        setMinimumHeight(48);
        setMaximumHeight(48);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setProperty("selected", false);

        m_holdTimer.setSingleShot(true);
        m_holdTimer.setInterval(kButtonHoldMs);
        connect(&m_holdTimer, &QTimer::timeout, this, [this] {
            if (!m_pressed)
                return;
            m_holdFired = true;
            emitAction(2);
        });

        m_tapTimer.setSingleShot(true);
        m_tapTimer.setInterval(kDoubleTapGuardMs);
        connect(&m_tapTimer, &QTimer::timeout, this, [this] {
            emitAction(0);
        });
    }

    void setActionCallback(ActionCallback callback)
    {
        m_callback = std::move(callback);
    }

    void setSelected(bool selected)
    {
        setProperty("selected", selected);
        repolish(this);
    }

protected:
    void mousePressEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::LeftButton && isEnabled()) {
            m_tapTimer.stop();
            m_pressed = true;
            m_holdFired = false;
            m_ignoreNextRelease = false;
            m_holdTimer.start();
        }
        QPushButton::mousePressEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent* event) override
    {
        const bool activate = event->button() == Qt::LeftButton
            && m_pressed
            && rect().contains(event->pos())
            && !m_holdFired
            && !m_ignoreNextRelease;
        m_pressed = false;
        m_holdTimer.stop();
        QPushButton::mouseReleaseEvent(event);
        if (activate)
            m_tapTimer.start();
        m_ignoreNextRelease = false;
    }

    void mouseDoubleClickEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::LeftButton && isEnabled()) {
            m_tapTimer.stop();
            m_holdTimer.stop();
            m_holdFired = true;
            m_ignoreNextRelease = true;
            emitAction(1);
            event->accept();
            return;
        }
        QPushButton::mouseDoubleClickEvent(event);
    }

private:
    void emitAction(int action)
    {
        if (m_callback)
            m_callback(action);
    }

    ActionCallback m_callback;
    QTimer m_holdTimer;
    QTimer m_tapTimer;
    bool m_pressed{false};
    bool m_holdFired{false};
    bool m_ignoreNextRelease{false};
};

QLabel* makeLed(const QString& label, bool active, QWidget* parent)
{
    auto* dot = new QLabel(parent);
    dot->setObjectName(QStringLiteral("LedDot"));
    dot->setProperty("active", active);
    dot->setToolTip(label);
    return dot;
}

} // namespace

FlexControlDialog::FlexControlDialog(QWidget* parent)
    : PersistentDialog(QStringLiteral("AetherControl"),
                       QStringLiteral("FlexControlDialogGeometry"),
                       parent)
{
    setModal(false);
    setWindowModality(Qt::NonModal);
    setMinimumSize(430, 610);

    bodyWidget()->setStyleSheet(QString::fromLatin1(kFlexControlStyle));
    auto* root = new QVBoxLayout(bodyWidget());
    root->setSizeConstraint(QLayout::SetMinimumSize);
    root->setContentsMargins(14, 14, 14, 14);
    root->setSpacing(10);

    auto* header = new QFrame;
    header->setObjectName(QStringLiteral("StatusFrame"));
    header->setMinimumHeight(70);
    header->setMaximumHeight(70);
    header->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(16, 12, 16, 12);
    headerLayout->setSpacing(12);

    auto* titleColumn = new QVBoxLayout;
    titleColumn->setSpacing(0);
    auto* title = new QLabel(QStringLiteral("AetherControl"));
    title->setObjectName(QStringLiteral("FlexMark"));
    auto* subtitle = new QLabel(QStringLiteral("Virtual Tuning Controller"));
    subtitle->setObjectName(QStringLiteral("SubMark"));
    titleColumn->addWidget(title);
    titleColumn->addWidget(subtitle);
    headerLayout->addLayout(titleColumn, 1);

    m_physicalButton = new QPushButton(header);
    m_physicalButton->setObjectName(QStringLiteral("PhysicalButton"));
    m_physicalButton->setMinimumWidth(96);
    m_physicalButton->setCursor(Qt::PointingHandCursor);
    connect(m_physicalButton, &QPushButton::clicked, this, [this] {
        if (m_physicalButton->property("connected").toBool())
            emit physicalDisconnectRequested();
        else
            emit physicalDetectRequested();
    });
    headerLayout->addWidget(m_physicalButton, 0, Qt::AlignVCenter);

    m_compactButton = new QPushButton(QStringLiteral("Compact"), header);
    m_compactButton->setObjectName(QStringLiteral("OptionButton"));
    m_compactButton->setCheckable(true);
    m_compactButton->setCursor(Qt::PointingHandCursor);
    m_compactButton->setToolTip(
        QStringLiteral("Hide configuration controls for a smaller AetherControl window."));
    m_compactButton->setChecked(AppSettings::instance()
        .value("FlexControlCompactMode", "False").toString() == "True");
    connect(m_compactButton, &QPushButton::toggled, this, [this](bool on) {
        auto& settings = AppSettings::instance();
        settings.setValue("FlexControlCompactMode", on ? "True" : "False");
        settings.save();
        updateCompactMode();
    });
    headerLayout->addWidget(m_compactButton, 0, Qt::AlignVCenter);
    setPhysicalReady(false);
    root->addWidget(header);

    m_deviceFrame = new QFrame;
    auto* device = m_deviceFrame;
    device->setObjectName(QStringLiteral("DeviceFrame"));
    auto* deviceLayout = new QVBoxLayout(device);
    deviceLayout->setContentsMargins(12, 12, 12, 12);
    deviceLayout->setSpacing(10);

    auto* auxPanel = new QWidget(device);
    auxPanel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* auxGrid = new QGridLayout(auxPanel);
    auxGrid->setContentsMargins(0, 0, 0, 0);
    auxGrid->setHorizontalSpacing(8);
    auxGrid->setVerticalSpacing(7);
    const QStringList ledNames{QStringLiteral("MODE"),
                               QStringLiteral("ON/OFF"),
                               QStringLiteral("TOGGLE")};
    auto makeActionCombo = [](QWidget* parent) {
        auto* combo = new QComboBox(parent);
        combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        combo->setMinimumContentsLength(9);
        combo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        for (const auto& def : kFlexActions)
            combo->addItem(QString::fromLatin1(def.label), QString::fromLatin1(def.id));
        return combo;
    };
    auto defaultAuxAction = [](int buttonIndex, int actionIndex) {
        static const char* defaults[3][2] = {
            {"StepUp",     "StepDown"},
            {"ToggleMox",  "ToggleTune"},
            {"ToggleMute", "ToggleLock"},
        };
        return QString::fromLatin1(defaults[buttonIndex][actionIndex]);
    };

    m_singleTapHeader = new QLabel(QStringLiteral("Single Tap"), device);
    m_singleTapHeader->setObjectName(QStringLiteral("SectionLabel"));
    m_singleTapHeader->setAlignment(Qt::AlignCenter);
    auxGrid->addWidget(m_singleTapHeader, 2, 0, 1, 3);
    m_compactHiddenWidgets.append(m_singleTapHeader);

    m_doubleTapHeader = new QLabel(QStringLiteral("Double Tap"), device);
    m_doubleTapHeader->setObjectName(QStringLiteral("SectionLabel"));
    m_doubleTapHeader->setAlignment(Qt::AlignCenter);
    auxGrid->addWidget(m_doubleTapHeader, 4, 0, 1, 3);
    m_compactHiddenWidgets.append(m_doubleTapHeader);

    for (int i = 0; i < 3; ++i) {
        auto* indicator = new QFrame;
        indicator->setObjectName(QStringLiteral("AuxCell"));
        indicator->setMinimumHeight(34);
        indicator->setMaximumHeight(34);
        indicator->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        auto* indicatorLayout = new QHBoxLayout(indicator);
        indicatorLayout->setContentsMargins(8, 6, 8, 6);
        indicatorLayout->setSpacing(6);
        auto* dot = makeLed(ledNames[i], false, indicator);
        m_auxDots.append(dot);
        auto* label = new QLabel(ledNames[i]);
        label->setObjectName(QStringLiteral("SectionLabel"));
        indicatorLayout->addWidget(dot, 0, Qt::AlignVCenter);
        indicatorLayout->addWidget(label, 1, Qt::AlignVCenter);
        auxGrid->addWidget(indicator, 0, i);

        auto* auxButton = new FlexControlButton(QStringLiteral("Aux%1").arg(i + 1), device);
        auxButton->setToolTip(
            QStringLiteral("Select the Aux%1 FlexControl function. Double-click for supported secondary actions.").arg(i + 1));
        auxButton->setActionCallback([this, i](int gesture) {
            activateAuxButton(i, gesture);
        });
        m_auxButtons.append(auxButton);
        auxGrid->addWidget(auxButton, 1, i);

        auto* combo = makeActionCombo(device);
        const QString key = QStringLiteral("FlexControlBtn%1Action0").arg(i + 1);
        const QString fallback = defaultAuxAction(i, 0);
        const QString saved = AppSettings::instance().value(key, fallback).toString();
        const int savedIndex = combo->findData(saved);
        combo->setCurrentIndex(savedIndex >= 0 ? savedIndex : combo->findData(fallback));
        connect(combo, &QComboBox::currentIndexChanged, this, [this, i, combo](int) {
            auto& settings = AppSettings::instance();
            settings.setValue(QStringLiteral("FlexControlBtn%1Action0").arg(i + 1),
                              combo->currentData().toString());
            settings.save();
            if (m_activeAux == i && auxActionControlsWheel(i))
                updateModeReadout();
            else if (m_activeAux == i)
                clearAuxSelection();
            emit flexControlSettingsChanged();
        });
        m_auxCombos.append(combo);
        m_compactHiddenWidgets.append(combo);
        auxGrid->addWidget(combo, 3, i);

        auto* doubleCombo = makeActionCombo(device);
        const QString doubleKey = QStringLiteral("FlexControlBtn%1Action1").arg(i + 1);
        const QString doubleFallback = defaultAuxAction(i, 1);
        const QString doubleSaved = AppSettings::instance().value(doubleKey, doubleFallback).toString();
        const int doubleSavedIndex = doubleCombo->findData(doubleSaved);
        doubleCombo->setCurrentIndex(doubleSavedIndex >= 0
            ? doubleSavedIndex : doubleCombo->findData(doubleFallback));
        connect(doubleCombo, &QComboBox::currentIndexChanged, this, [this, i, doubleCombo](int) {
            auto& settings = AppSettings::instance();
            settings.setValue(QStringLiteral("FlexControlBtn%1Action1").arg(i + 1),
                              doubleCombo->currentData().toString());
            settings.save();
            emit flexControlSettingsChanged();
        });
        m_auxDoubleCombos.append(doubleCombo);
        m_compactHiddenWidgets.append(doubleCombo);
        auxGrid->addWidget(doubleCombo, 5, i);

        auxGrid->setColumnStretch(i, 1);
    }
    deviceLayout->addWidget(auxPanel);

    m_knobPanel = new QFrame;
    auto* knobPanel = m_knobPanel;
    knobPanel->setObjectName(QStringLiteral("KnobPanel"));
    knobPanel->setMinimumHeight(420);
    auto* knobLayout = new QVBoxLayout(knobPanel);
    knobLayout->setContentsMargins(12, 12, 12, 12);
    knobLayout->setSpacing(8);

    m_wheel = new VirtualFlexControlWheel(knobPanel);
    m_wheel->setStepCallback([this](int steps) {
        recordVirtualTune(steps);
    });
    m_wheel->setCaptureCallback([this](bool active) {
        setCaptureHintActive(active);
    });
    const int looseness = std::clamp(
        AppSettings::instance().value("FlexControlVirtualWheelLooseness", "45").toInt(),
        0, 100);
    m_wheel->setLooseness(looseness);
    knobLayout->addWidget(m_wheel, 1);

    auto* pushButton = new FlexControlButton(QStringLiteral("PUSH"), knobPanel);
    pushButton->setObjectName(QStringLiteral("PushButton"));
    pushButton->setMinimumHeight(40);
    pushButton->setToolTip(QStringLiteral("Run the configured FlexControl knob-button action."));
    pushButton->setActionCallback([this](int gesture) {
        if (gesture == 0)
            clearAuxSelection();
        emit virtualButtonPressed(4, gesture);
    });
    m_pushButton = pushButton;
    knobLayout->addWidget(pushButton);

    m_pushHintLabel = new QLabel(QStringLiteral("Push returns the wheel to Tune Slice when Aux is active."),
                                 knobPanel);
    m_pushHintLabel->setObjectName(QStringLiteral("PushHint"));
    m_pushHintLabel->setAlignment(Qt::AlignCenter);
    m_pushHintLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    m_pushHintLabel->setMinimumWidth(0);
    knobLayout->addWidget(m_pushHintLabel);
    m_compactHiddenWidgets.append(m_pushHintLabel);

    m_pushActionsFrame = new QFrame(knobPanel);
    m_pushActionsFrame->setObjectName(QStringLiteral("PushActions"));
    auto* pushActionsLayout = new QHBoxLayout(m_pushActionsFrame);
    pushActionsLayout->setContentsMargins(10, 8, 10, 8);
    pushActionsLayout->setSpacing(8);
    auto* pushSingleLabel = new QLabel(QStringLiteral("Single Tap"), m_pushActionsFrame);
    pushSingleLabel->setObjectName(QStringLiteral("SectionLabel"));
    auto* pushDoubleLabel = new QLabel(QStringLiteral("Double Tap"), m_pushActionsFrame);
    pushDoubleLabel->setObjectName(QStringLiteral("SectionLabel"));
    m_pushSingleCombo = makeActionCombo(m_pushActionsFrame);
    m_pushDoubleCombo = makeActionCombo(m_pushActionsFrame);
    m_pushSingleCombo->setMinimumContentsLength(7);
    m_pushDoubleCombo->setMinimumContentsLength(7);
    const QString pushTapAction = AppSettings::instance()
        .value("FlexControlBtn4Action0", QStringLiteral("StepUp")).toString();
    const int pushTapIndex = m_pushSingleCombo->findData(pushTapAction);
    m_pushSingleCombo->setCurrentIndex(pushTapIndex >= 0
        ? pushTapIndex : m_pushSingleCombo->findData(QStringLiteral("StepUp")));
    const QString pushDoubleAction = AppSettings::instance()
        .value("FlexControlBtn4Action1", QStringLiteral("StepDown")).toString();
    const int pushDoubleIndex = m_pushDoubleCombo->findData(pushDoubleAction);
    m_pushDoubleCombo->setCurrentIndex(pushDoubleIndex >= 0
        ? pushDoubleIndex : m_pushDoubleCombo->findData(QStringLiteral("StepDown")));
    connect(m_pushSingleCombo, &QComboBox::currentIndexChanged, this, [this](int) {
        auto& settings = AppSettings::instance();
        settings.setValue("FlexControlBtn4Action0", m_pushSingleCombo->currentData().toString());
        settings.save();
        refreshButtonActions();
        emit flexControlSettingsChanged();
    });
    connect(m_pushDoubleCombo, &QComboBox::currentIndexChanged, this, [this](int) {
        auto& settings = AppSettings::instance();
        settings.setValue("FlexControlBtn4Action1", m_pushDoubleCombo->currentData().toString());
        settings.save();
        refreshButtonActions();
        emit flexControlSettingsChanged();
    });
    pushActionsLayout->addWidget(pushSingleLabel);
    pushActionsLayout->addWidget(m_pushSingleCombo, 1);
    pushActionsLayout->addWidget(pushDoubleLabel);
    pushActionsLayout->addWidget(m_pushDoubleCombo, 1);
    knobLayout->addWidget(m_pushActionsFrame);
    m_compactHiddenWidgets.append(m_pushActionsFrame);

    deviceLayout->addWidget(knobPanel);
    root->addWidget(device);

    m_captureHint = new QLabel(QStringLiteral("Click the knob to capture circular tuning."));
    m_captureHint->setObjectName(QStringLiteral("CaptureHint"));
    m_captureHint->setProperty("armed", false);
    root->addWidget(m_captureHint);
    m_compactHiddenWidgets.append(m_captureHint);

    auto* controlStrip = new QFrame;
    m_controlStrip = controlStrip;
    controlStrip->setObjectName(QStringLiteral("ControlStrip"));
    const QString spinTooltip = QStringLiteral(
        "Adjusts only the mouse/trackpad coasting feel of the virtual tuning wheel. "
        "It does not affect the physical FlexControl device.");
    controlStrip->setToolTip(spinTooltip);
    auto* controlLayout = new QHBoxLayout(controlStrip);
    controlLayout->setContentsMargins(14, 10, 14, 10);
    controlLayout->setSpacing(12);

    auto* tightLabel = new QLabel(QStringLiteral("TIGHT"));
    tightLabel->setObjectName(QStringLiteral("SectionLabel"));
    auto* looseLabel = new QLabel(QStringLiteral("LOOSE"));
    looseLabel->setObjectName(QStringLiteral("SectionLabel"));
    m_spinSlider = new GuardedSlider(Qt::Horizontal);
    m_spinSlider->setRange(0, 100);
    m_spinSlider->setValue(looseness);
    m_spinSlider->setMinimumWidth(110);
    m_spinSlider->setMaximumWidth(145);
    m_spinSlider->setToolTip(spinTooltip);
    tightLabel->setToolTip(spinTooltip);
    looseLabel->setToolTip(spinTooltip);
    connect(m_spinSlider, &QSlider::valueChanged, this, [this](int value) {
        if (m_wheel)
            m_wheel->setLooseness(value);
        auto& settings = AppSettings::instance();
        settings.setValue("FlexControlVirtualWheelLooseness", QString::number(value));
        settings.save();
    });
    m_externalSpinButton = new QPushButton(QStringLiteral("Auto Spin"), controlStrip);
    m_externalSpinButton->setObjectName(QStringLiteral("OptionButton"));
    m_externalSpinButton->setCheckable(true);
    m_externalSpinButton->setCursor(Qt::PointingHandCursor);
    m_externalSpinButton->setToolTip(QStringLiteral(
        "Animate the virtual wheel when the selected slice frequency changes externally."));
    connect(m_externalSpinButton, &QPushButton::toggled, this, [this](bool on) {
        auto& settings = AppSettings::instance();
        settings.setValue("FlexControlVirtualExternalSpin", on ? "True" : "False");
        settings.save();
        updateOptionButtons();
    });
    m_reverseButton = new QPushButton(QStringLiteral("Reverse"), controlStrip);
    m_reverseButton->setObjectName(QStringLiteral("OptionButton"));
    m_reverseButton->setCheckable(true);
    m_reverseButton->setCursor(Qt::PointingHandCursor);
    m_reverseButton->setToolTip(QStringLiteral("Reverse the virtual and physical FlexControl tuning direction."));
    connect(m_reverseButton, &QPushButton::toggled, this, [this](bool on) {
        auto& settings = AppSettings::instance();
        settings.setValue("FlexControlInvertDir", on ? "True" : "False");
        settings.save();
        updateOptionButtons();
        emit flexControlSettingsChanged();
    });
    controlLayout->addWidget(tightLabel);
    controlLayout->addWidget(m_spinSlider, 0);
    controlLayout->addWidget(looseLabel);
    controlLayout->addStretch(1);
    controlLayout->addWidget(m_externalSpinButton);
    controlLayout->addWidget(m_reverseButton);
    root->addWidget(controlStrip);
    m_compactHiddenWidgets.append(controlStrip);

    auto* statusFrame = new QFrame;
    statusFrame->setObjectName(QStringLiteral("StatusFrame"));
    statusFrame->setMinimumHeight(148);
    statusFrame->setMaximumHeight(148);
    statusFrame->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* statusLayout = new QGridLayout(statusFrame);
    statusLayout->setContentsMargins(10, 10, 10, 10);
    statusLayout->setSpacing(8);

    auto makeStatusCell = [](const QString& name, QLabel** valueOut) {
        auto* frame = new QFrame;
        frame->setObjectName(QStringLiteral("StatusCell"));
        auto* layout = new QVBoxLayout(frame);
        layout->setContentsMargins(10, 8, 10, 8);
        layout->setSpacing(2);
        auto* label = new QLabel(name);
        label->setObjectName(QStringLiteral("SectionLabel"));
        auto* value = new QLabel(QStringLiteral("-"));
        value->setObjectName(QStringLiteral("StatusValue"));
        value->setWordWrap(false);
        value->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        value->setMinimumHeight(20);
        frame->setMinimumHeight(58);
        frame->setMaximumHeight(64);
        layout->addWidget(label);
        layout->addWidget(value);
        *valueOut = value;
        return frame;
    };

    statusLayout->addWidget(makeStatusCell(QStringLiteral("WHEEL"), &m_modeLabel), 0, 0);
    statusLayout->addWidget(makeStatusCell(QStringLiteral("SLICE"), &m_sliceLabel), 0, 1);
    statusLayout->addWidget(makeStatusCell(QStringLiteral("FREQUENCY"), &m_frequencyLabel), 1, 0);
    statusLayout->addWidget(makeStatusCell(QStringLiteral("STEP"), &m_stepLabel), 1, 1);
    statusLayout->setColumnStretch(0, 1);
    statusLayout->setColumnStretch(1, 1);
    root->addWidget(statusFrame);

    m_wheel->setEnabled(false);
    updateAuxIndicators();
    updateModeReadout();
    updateOptionButtons();
    updateCompactMode();
    setStepSize(m_stepHz);
    updateSliceReadout();

    m_releaseShortcut = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    m_releaseShortcut->setContext(Qt::ApplicationShortcut);
    m_releaseShortcut->setEnabled(false);
    connect(m_releaseShortcut, &QShortcut::activated, this, [this] {
        if (m_wheel && m_wheel->captured()) {
            m_ignoreNextReject = true;
            releaseWheelCapture();
            QTimer::singleShot(0, this, [this] {
                m_ignoreNextReject = false;
            });
        }
    });

    qApp->installEventFilter(this);
}

FlexControlDialog::~FlexControlDialog()
{
    releaseWheelCapture();
    if (qApp)
        qApp->removeEventFilter(this);
    if (m_slice) {
        disconnect(m_frequencyConnection);
        disconnect(m_stepConnection);
        disconnect(m_letterConnection);
    }
}

void FlexControlDialog::setSlice(SliceModel* slice)
{
    if (m_slice == slice) {
        updateSliceReadout();
        return;
    }

    if (m_slice) {
        disconnect(m_frequencyConnection);
        disconnect(m_stepConnection);
        disconnect(m_letterConnection);
    }

    releaseWheelCapture();
    m_slice = slice;
    m_haveLastSliceFrequency = false;
    if (m_wheel)
        m_wheel->setEnabled(slice != nullptr);

    if (slice) {
        m_lastSliceFrequencyMhz = slice->frequency();
        m_haveLastSliceFrequency = true;
        const QPointer<SliceModel> trackedSlice(slice);
        m_frequencyConnection = connect(slice, &SliceModel::frequencyChanged,
                                        this, [this, trackedSlice](double mhz) {
            if (m_slice != trackedSlice)
                return;
            animateExternalTune(mhz);
            updateSliceReadout();
        });
        m_stepConnection = connect(slice, &SliceModel::stepChanged,
                                   this, [this, trackedSlice](int hz, const QVector<int>&) {
            if (m_slice != trackedSlice)
                return;
            if (hz > 0)
                setStepSize(hz);
        });
        m_letterConnection = connect(slice, &SliceModel::letterChanged,
                                     this, [this, trackedSlice](const QString&) {
            if (m_slice == trackedSlice)
                updateSliceReadout();
        });
    }

    updateSliceReadout();
}

void FlexControlDialog::refreshButtonActions()
{
    auto& settings = AppSettings::instance();
    for (int i = 0; i < m_auxCombos.size(); ++i) {
        auto* combo = m_auxCombos.value(i);
        const QString key = QStringLiteral("FlexControlBtn%1Action0").arg(i + 1);
        const QString fallback = (i == 0) ? QStringLiteral("StepUp")
                               : (i == 1) ? QStringLiteral("ToggleMox")
                                          : QStringLiteral("ToggleMute");
        if (combo) {
            const QString saved = settings.value(key, fallback).toString();
            const int savedIndex = combo->findData(saved);
            const QSignalBlocker blocker(combo);
            combo->setCurrentIndex(savedIndex >= 0 ? savedIndex : combo->findData(fallback));
        }

        auto* doubleCombo = m_auxDoubleCombos.value(i);
        const QString doubleKey = QStringLiteral("FlexControlBtn%1Action1").arg(i + 1);
        const QString doubleFallback = (i == 0) ? QStringLiteral("StepDown")
                                    : (i == 1) ? QStringLiteral("ToggleTune")
                                               : QStringLiteral("ToggleLock");
        if (doubleCombo) {
            const QString saved = settings.value(doubleKey, doubleFallback).toString();
            const int savedIndex = doubleCombo->findData(saved);
            const QSignalBlocker blocker(doubleCombo);
            doubleCombo->setCurrentIndex(savedIndex >= 0
                ? savedIndex : doubleCombo->findData(doubleFallback));
        }
    }
    if (m_pushButton) {
        const QString tapAction = settings
            .value("FlexControlBtn4Action0", QStringLiteral("StepUp")).toString();
        const QString doubleAction = settings
            .value("FlexControlBtn4Action1", QStringLiteral("StepDown")).toString();
        m_pushButton->setText(QStringLiteral("PUSH"));
        m_pushButton->setToolTip(QStringLiteral("Single tap: %1\nDouble tap: %2")
            .arg(labelForActionId(tapAction), labelForActionId(doubleAction)));
        if (m_pushHintLabel) {
            m_pushHintLabel->setToolTip(QStringLiteral(
                "When an Aux wheel mode is selected, a single tap returns the wheel to Tune Slice. "
                "Otherwise, it runs the configured single-tap action."));
        }
        if (m_pushSingleCombo) {
            const int index = m_pushSingleCombo->findData(tapAction);
            const QSignalBlocker blocker(m_pushSingleCombo);
            m_pushSingleCombo->setCurrentIndex(index >= 0
                ? index : m_pushSingleCombo->findData(QStringLiteral("StepUp")));
        }
        if (m_pushDoubleCombo) {
            const int index = m_pushDoubleCombo->findData(doubleAction);
            const QSignalBlocker blocker(m_pushDoubleCombo);
            m_pushDoubleCombo->setCurrentIndex(index >= 0
                ? index : m_pushDoubleCombo->findData(QStringLiteral("StepDown")));
        }
    }
    if (m_activeAux >= 0 && !auxActionControlsWheel(m_activeAux))
        clearAuxSelection();
    else
        updateModeReadout();
    updateOptionButtons();
}

void FlexControlDialog::setStepSize(int hz)
{
    if (hz <= 0)
        return;
    m_stepHz = hz;
    if (m_stepLabel)
        m_stepLabel->setText(formatStep(hz));
}

void FlexControlDialog::setPhysicalReady(bool ready, const QString& port)
{
    if (!m_physicalButton)
        return;
    const QString displayPort = port.isEmpty()
        ? AppSettings::instance().value("FlexControlPort").toString()
        : port;
    m_physicalButton->setText(ready ? QStringLiteral("Connected")
                                    : QStringLiteral("Detect"));
    m_physicalButton->setProperty("connected", ready);
    if (ready && !displayPort.isEmpty()) {
        m_physicalButton->setToolTip(
            QStringLiteral("Physical FlexControl is connected on %1. Click to disconnect.")
                .arg(displayPort));
    } else {
        m_physicalButton->setToolTip(ready
            ? QStringLiteral("Physical FlexControl is connected. Click to disconnect.")
            : QStringLiteral("Detect and connect a physical FlexControl device."));
    }
    repolish(m_physicalButton);
}

void FlexControlDialog::reflectButtonPress(int button, int action)
{
    if (action != 0)
        return;
    setActiveAuxButton(button);
}

void FlexControlDialog::setActiveAuxButton(int button)
{
    if (button >= 1 && button <= m_auxCombos.size()) {
        const int index = button - 1;
        if (auxActionControlsWheel(index)) {
            m_activeAux = index;
        } else {
            m_activeAux = -1;
        }
        updateAuxIndicators();
        updateModeReadout();
        return;
    }
    clearAuxSelection();
}

bool FlexControlDialog::externalSpinEnabled() const
{
    return AppSettings::instance()
        .value("FlexControlVirtualExternalSpin", "True").toString() == "True";
}

void FlexControlDialog::animateExternalTune(double mhz)
{
    if (!m_haveLastSliceFrequency) {
        m_lastSliceFrequencyMhz = mhz;
        m_haveLastSliceFrequency = true;
        return;
    }

    const double previous = m_lastSliceFrequencyMhz;
    m_lastSliceFrequencyMhz = mhz;
    if (!externalSpinEnabled() || !m_wheel || m_wheel->captured() || m_stepHz <= 0)
        return;

    const double deltaHz = (mhz - previous) * 1.0e6;
    int steps = static_cast<int>(std::llround(deltaHz / static_cast<double>(m_stepHz)));
    if (steps == 0 && std::abs(deltaHz) >= 1.0)
        steps = deltaHz > 0.0 ? 1 : -1;
    if (AppSettings::instance().value("FlexControlInvertDir", "False").toString() == "True")
        steps = -steps;
    m_wheel->animateExternalSteps(steps);
}

void FlexControlDialog::updateOptionButtons()
{
    if (m_externalSpinButton) {
        const QSignalBlocker blocker(m_externalSpinButton);
        m_externalSpinButton->setChecked(externalSpinEnabled());
        repolish(m_externalSpinButton);
    }
    if (m_reverseButton) {
        const bool reversed =
            AppSettings::instance().value("FlexControlInvertDir", "False").toString() == "True";
        const QSignalBlocker blocker(m_reverseButton);
        m_reverseButton->setChecked(reversed);
        repolish(m_reverseButton);
    }
    if (m_compactButton) {
        const bool compact = AppSettings::instance()
            .value("FlexControlCompactMode", "False").toString() == "True";
        const QSignalBlocker blocker(m_compactButton);
        m_compactButton->setChecked(compact);
        repolish(m_compactButton);
    }
}

void FlexControlDialog::updateCompactMode()
{
    const bool compact = m_compactButton && m_compactButton->isChecked();
    setMaximumHeight(QWIDGETSIZE_MAX);
    setMinimumSize(QSize(0, 0));
    for (auto* widget : m_compactHiddenWidgets) {
        if (widget) {
            widget->setVisible(!compact);
        }
    }

    if (m_deviceFrame) {
        m_deviceFrame->setMaximumHeight(QWIDGETSIZE_MAX);
        m_deviceFrame->setSizePolicy(QSizePolicy::Expanding,
                                     compact ? QSizePolicy::Fixed : QSizePolicy::Expanding);
    }

    if (m_wheel) {
        if (compact) {
            m_wheel->setMaximumSize(270, 270);
            m_wheel->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        } else {
            m_wheel->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
            m_wheel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        }
        if (m_knobPanel && m_knobPanel->layout())
            m_knobPanel->layout()->setAlignment(m_wheel, compact ? Qt::AlignHCenter : Qt::Alignment());
    }

    if (m_knobPanel) {
        m_knobPanel->setMinimumHeight(compact ? 360 : 420);
        m_knobPanel->setMaximumHeight(compact ? 360 : QWIDGETSIZE_MAX);
        m_knobPanel->setSizePolicy(QSizePolicy::Expanding,
                                   compact ? QSizePolicy::Fixed : QSizePolicy::Expanding);
    }
    if (m_deviceFrame && m_deviceFrame->layout())
        m_deviceFrame->layout()->setAlignment(m_knobPanel,
                                              compact ? Qt::AlignTop : Qt::Alignment());
    if (m_deviceFrame && m_deviceFrame->layout()) {
        m_deviceFrame->layout()->activate();
        if (compact)
            m_deviceFrame->setMaximumHeight(m_deviceFrame->minimumSizeHint().height());
    }
    if (auto* bodyLayout = bodyWidget()->layout()) {
        bodyLayout->setAlignment(m_deviceFrame, compact ? Qt::AlignTop : Qt::Alignment());
        bodyLayout->activate();
    }

    if (compact) {
        applyCompactWindowSize();
        QTimer::singleShot(0, this, [this] {
            applyCompactWindowSize();
        });
    } else {
        const QSize required = minimumSizeHint().expandedTo(QSize(430, 0));
        setMinimumSize(required);
        if (isVisible() && (width() < required.width() || height() < required.height()))
            resize(size().expandedTo(required));
    }
}

void FlexControlDialog::applyCompactWindowSize()
{
    if (!m_compactButton || !m_compactButton->isChecked())
        return;

    setMaximumHeight(QWIDGETSIZE_MAX);
    setMinimumSize(QSize(0, 0));
    if (m_deviceFrame && m_deviceFrame->layout()) {
        m_deviceFrame->layout()->activate();
        m_deviceFrame->setMaximumHeight(m_deviceFrame->minimumSizeHint().height());
    }
    if (auto* bodyLayout = bodyWidget()->layout())
        bodyLayout->activate();

    const QSize required = minimumSizeHint().expandedTo(QSize(410, 0));
    setMinimumSize(required);
    setFixedHeight(required.height());
    if (isVisible()) {
        const int targetWidth = std::max(width(), required.width());
        resize(targetWidth, required.height());
    }
}

void FlexControlDialog::reject()
{
    if (m_ignoreNextReject || (m_wheel && m_wheel->captured())) {
        m_ignoreNextReject = false;
        releaseWheelCapture();
        return;
    }
    PersistentDialog::reject();
}

bool FlexControlDialog::event(QEvent* event)
{
    if (handleEscapeRelease(event))
        return true;
    return PersistentDialog::event(event);
}

bool FlexControlDialog::eventFilter(QObject* watched, QEvent* event)
{
    if (handleEscapeRelease(event))
        return true;
    return PersistentDialog::eventFilter(watched, event);
}

void FlexControlDialog::keyPressEvent(QKeyEvent* event)
{
    if (handleEscapeRelease(event))
        return;
    PersistentDialog::keyPressEvent(event);
}

void FlexControlDialog::closeEvent(QCloseEvent* event)
{
    releaseWheelCapture();
    PersistentDialog::closeEvent(event);
}

void FlexControlDialog::recordVirtualTune(int steps)
{
    if (steps == 0)
        return;
    if (AppSettings::instance().value("FlexControlInvertDir", "False").toString() == "True")
        steps = -steps;
    const QString actionId = activeKnobActionId();
    emit virtualWheelSteps(actionId, steps);
}

bool FlexControlDialog::handleEscapeRelease(QEvent* event)
{
    if (!m_wheel || !m_wheel->captured() || !isEscapeKeyEvent(event))
        return false;
    m_ignoreNextReject = true;
    releaseWheelCapture();
    event->accept();
    QTimer::singleShot(0, this, [this] {
        m_ignoreNextReject = false;
    });
    return true;
}

void FlexControlDialog::releaseWheelCapture()
{
    if (m_wheel)
        m_wheel->releaseCapture();
}

void FlexControlDialog::activateAuxButton(int index, int gesture)
{
    if (index < 0 || index >= m_auxCombos.size())
        return;
    if (gesture == 0 && auxActionControlsWheel(index)) {
        m_activeAux = index;
        updateAuxIndicators();
        updateModeReadout();
    }
    emit virtualButtonPressed(index + 1, gesture);
}

void FlexControlDialog::clearAuxSelection()
{
    m_activeAux = -1;
    updateAuxIndicators();
    updateModeReadout();
}

void FlexControlDialog::updateAuxIndicators()
{
    for (int i = 0; i < m_auxDots.size(); ++i) {
        const bool active = i == m_activeAux;
        if (auto* dot = m_auxDots.value(i)) {
            dot->setProperty("active", active);
            repolish(dot);
        }
        if (auto* button = m_auxButtons.value(i)) {
            button->setProperty("selected", active);
            repolish(button);
        }
    }
}

void FlexControlDialog::updateModeReadout()
{
    if (!m_modeLabel)
        return;
    const QString wheelAction = activeKnobActionId();
    QString text = labelForActionId(wheelAction);
    QString tooltip = QStringLiteral("The wheel currently controls %1.").arg(text);
    QString pushHint = QStringLiteral("Push tap runs configured action.");
    if (m_activeAux >= 0) {
        const QString auxLabel = labelForActionId(auxActionId(m_activeAux));
        if (auxActionControlsWheel(m_activeAux)) {
            text = QStringLiteral("Aux%1: %2").arg(m_activeAux + 1).arg(auxLabel);
            tooltip = QStringLiteral("Aux%1 selected; the wheel controls %2.")
                          .arg(m_activeAux + 1)
                          .arg(auxLabel);
            pushHint = QStringLiteral("Push tap returns wheel to Tune Slice.");
        } else {
            text = QStringLiteral("%1 (Aux%2: %3)")
                       .arg(text)
                       .arg(m_activeAux + 1)
                       .arg(auxLabel);
            tooltip = QStringLiteral("Aux%1 action is %2; the wheel still controls %3.")
                          .arg(m_activeAux + 1)
                          .arg(auxLabel)
                          .arg(labelForActionId(wheelAction));
        }
    }
    m_modeLabel->setText(text);
    m_modeLabel->setToolTip(tooltip);
    if (m_pushHintLabel)
        m_pushHintLabel->setText(pushHint);
}

QString FlexControlDialog::auxActionId(int index) const
{
    auto* combo = m_auxCombos.value(index);
    if (!combo)
        return QStringLiteral("None");
    return combo->currentData().toString();
}

QString FlexControlDialog::activeKnobActionId() const
{
    if (m_activeAux < 0)
        return QStringLiteral("WheelFrequency");
    const QString actionId = auxActionId(m_activeAux);
    if (!isWheelActionId(actionId))
        return QStringLiteral("WheelFrequency");
    return actionId.isEmpty() ? QStringLiteral("WheelFrequency") : actionId;
}

bool FlexControlDialog::auxActionControlsWheel(int index) const
{
    return isWheelActionId(auxActionId(index));
}

void FlexControlDialog::updateSliceReadout()
{
    if (!m_slice) {
        if (m_sliceLabel)
            m_sliceLabel->setText(QStringLiteral("None"));
        if (m_frequencyLabel)
            m_frequencyLabel->setText(QStringLiteral("-"));
        return;
    }

    if (m_sliceLabel) {
        m_sliceLabel->setText(SliceLabel::unicodeForm(m_slice->sliceId(), m_slice->letter()));
    }
    if (m_frequencyLabel)
        m_frequencyLabel->setText(formatFrequency(m_slice->frequency()));
}

void FlexControlDialog::setCaptureHintActive(bool active)
{
    if (!m_captureHint)
        return;
    m_captureHint->setText(active
        ? QStringLiteral("Mouse locked to FlexControl. Press ESC to release.")
        : QStringLiteral("Click the knob to capture circular tuning."));
    m_captureHint->setProperty("armed", active);
    if (m_releaseShortcut)
        m_releaseShortcut->setEnabled(active);
    repolish(m_captureHint);
}

} // namespace AetherSDR
