#include "GreenHeronApplet.h"

#include "core/GreenHeronProtocol.h"
#include "core/PeripheralSettings.h"
#include "core/ThemeManager.h"
#include "models/GreenHeronModel.h"

#include <QAccessible>
#include <QComboBox>
#include <QDoubleValidator>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>
#include <QtMath>

#include <cmath>

#include <iterator>

namespace AetherSDR {

namespace {

// The peripheral settings blob is shared across accessory devices and keyed by
// device name (Constitution Principle V — one owned object, not loose flat
// keys). "GreenHeron" is this feature's sub-object inside it.
constexpr const char* kSettingsDevice = "GreenHeron";
constexpr const char* kSettingsHost   = "Host";
constexpr const char* kSettingsPort   = "Port";
constexpr const char* kSettingsSwitch = "Switch";

constexpr int kNoteTimeoutMs = 5000;

// Everything here is a ThemeManager token template, applied through
// applyStyleSheet() rather than setStyleSheet(). Two reasons beyond the
// hardcoded-colour ratchet: tokens follow the theme (and the applet's scope
// in the container tree), and applyStyleSheet re-applies on themeChanged, so
// the tile repaints on a theme switch with no per-call-site wiring.
//
// The port rows are Success-tribe toggles per docs/theming/toggle-button-
// tokens.md — a checked row means "this antenna is connected", which is the
// enable/activate semantic, not a generic mode selector.
const QString kPortButtonStyle = QStringLiteral(
    "QPushButton { background: {{color.toggle.background}}; "
    "border: 1px solid {{color.toggle.border}}; border-radius: 3px; "
    "padding: 3px 4px; font-size: 10px; "
    "color: {{color.toggle.foreground}}; text-align: left; }"
    "QPushButton:hover { background: {{color.background.2}}; }"
    "QPushButton:disabled { color: {{color.toggle.foreground.disabled}}; "
    "background: {{color.toggle.background.disabled}}; "
    "border: 1px solid {{color.toggle.border.disabled}}; font-style: italic; }"
    "QPushButton:checked { "
    "background-color: {{color.toggle.success.background.checked}}; "
    "color: {{color.toggle.success.foreground.checked}}; "
    "border: 1px solid {{color.toggle.success.border.checked}}; "
    "font-weight: bold; }");

const QString kConnectButtonStyle = QStringLiteral(
    "QPushButton { background: {{color.toggle.background}}; "
    "border: 1px solid {{color.toggle.border}}; border-radius: 3px; "
    "padding: 2px 2px; font-size: 10px; font-weight: bold; "
    "color: {{color.toggle.foreground}}; text-align: center; }"
    "QPushButton:hover { background: {{color.background.2}}; }");

const QString kLabelStyle = QStringLiteral(
    "color: {{color.text.label}}; font-size: 10px; font-weight: bold;");

// One template, one token slot — the status line's colour IS its state, so
// the caller passes the token rather than each call site owning a hex value.
QString statusStyle(const QString& colourToken)
{
    return QStringLiteral("color: {{%1}}; font-size: 10px;").arg(colourToken);
}

// Announce a text-only change to a screen reader (docs/a11y.md, "Live value
// updates"). Callers fire this ONLY when something actually changed: the
// device republishes every ~0.5–3.4 s, and re-announcing an unchanged row on
// every push would make the tile unusable with Orca / NVDA / VoiceOver.
void announce(QWidget* widget, QAccessible::Event type)
{
    QAccessibleEvent event(widget, type);
    QAccessible::updateAccessibility(&event);
}

const QString kFieldStyle =
    QStringLiteral("background: {{color.background.1}}; "
                   "border: 1px solid {{color.background.1}}; border-radius: 3px; "
                   "padding: 2px 4px; color: {{color.text.primary}}; font-size: 10px;");

} // namespace

// ── RotorCompass ────────────────────────────────────────────────────────────

namespace {

// Heading 0 is north and grows clockwise, which is the rotator's convention
// and not Qt's. Screen y grows downward, so a heading maps to a point as
// (sin, -cos) rather than the usual (cos, sin).
QPointF headingPoint(const QPointF& centre, double radius, double headingDeg)
{
    const double rad = qDegreesToRadians(headingDeg);
    return {centre.x() + radius * std::sin(rad),
            centre.y() - radius * std::cos(rad)};
}

constexpr int kCompassPreferredPx = 190;
constexpr int kCompassMinimumPx   = 120;

} // namespace

RotorCompass::RotorCompass(QWidget* parent) : QWidget(parent)
{
    setAccessibleName(tr("Rotator compass"));
    // The dial restates the readout; a screen reader that has just been given
    // the heading in text does not need it again as a graphic.
    setAccessibleDescription(
        tr("A dial repeating the rotator heading shown beside it."));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

QSize RotorCompass::sizeHint() const
{
    return {kCompassPreferredPx, kCompassPreferredPx};
}

QSize RotorCompass::minimumSizeHint() const
{
    return {kCompassMinimumPx, kCompassMinimumPx};
}

void RotorCompass::setHeading(double reported, bool hasAsked, double asked)
{
    // An absolute epsilon, NOT qFuzzyCompare: that compares relative to
    // magnitude and is documented as unusable against zero, which is due
    // north and therefore a heading this widget will genuinely be handed.
    // Well below kHeadingDecimals, so the guard can never swallow a change
    // the readout beside it is showing.
    constexpr double kSame = 1e-6;
    const auto same = [](double a, double b) { return std::abs(a - b) < kSame; };

    if (same(m_reported, reported) && m_hasAsked == hasAsked
        && (!hasAsked || same(m_asked, asked))) {
        return;
    }
    m_reported = reported;
    m_hasAsked = hasAsked;
    m_asked    = asked;
    // update() only. NOT QAccessible::updateAccessibility(): POINT lands at
    // ~0.97 s and dithers at rest, and the readout label already owns the one
    // announcement channel this tile has (see m_lastRotorAnnouncement).
    update();
}

void RotorCompass::paintEvent(QPaintEvent* /*ev*/)
{
    auto& theme = ThemeManager::instance();
    const QColor ringColour   = theme.color(this, QStringLiteral("color.text.disabled"));
    const QColor labelColour  = theme.color(this, QStringLiteral("color.text.label"));
    const QColor needleColour = theme.color(this, QStringLiteral("color.text.primary"));

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QRectF box = rect();
    const double side = std::min(box.width(), box.height());
    const QPointF centre = box.center();
    // Room for the cardinal letters outside the ring.
    const double radius = side / 2.0 - 14.0;
    if (radius <= 4.0) {
        return;
    }

    p.setPen(QPen(ringColour, 1.0));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(centre, radius, radius);

    // Ticks every 30°, the cardinals longer and lettered.
    QFont tickFont = font();
    tickFont.setPointSizeF(std::max(7.0, tickFont.pointSizeF() - 1.0));
    p.setFont(tickFont);
    const QFontMetricsF fm(tickFont);

    for (int deg = 0; deg < 360; deg += 30) {
        const bool cardinal = (deg % 90) == 0;
        const double inner = radius - (cardinal ? 8.0 : 4.0);
        p.setPen(QPen(cardinal ? labelColour : ringColour, cardinal ? 1.4 : 1.0));
        p.drawLine(headingPoint(centre, inner, deg),
                   headingPoint(centre, radius, deg));

        if (!cardinal) {
            continue;
        }
        const QString letter = deg == 0   ? tr("N")
                             : deg == 90  ? tr("E")
                             : deg == 180 ? tr("S")
                                          : tr("W");
        const QPointF at = headingPoint(centre, radius + 8.0, deg);
        const QRectF glyph(at.x() - fm.horizontalAdvance(letter) / 2.0,
                           at.y() - fm.height() / 2.0,
                           fm.horizontalAdvance(letter), fm.height());
        p.setPen(labelColour);
        p.drawText(glyph, Qt::AlignCenter, letter);
    }

    // The commanded heading, when there is one: an open tick outside the
    // ring, deliberately unlike the needle so the two are never confused for
    // one reading. Drawn first so the needle wins any overlap — the reported
    // heading is the fact and the asked one is only context.
    if (m_hasAsked) {
        QPen ghost(labelColour, 1.4, Qt::DashLine);
        p.setPen(ghost);
        p.drawLine(headingPoint(centre, radius - 6.0, m_asked),
                   headingPoint(centre, radius + 4.0, m_asked));
    }

    // The needle: reported heading, always.
    const QPointF tip  = headingPoint(centre, radius - 6.0, m_reported);
    const QPointF left = headingPoint(centre, 5.0, m_reported - 90.0);
    const QPointF rght = headingPoint(centre, 5.0, m_reported + 90.0);

    QPainterPath needle;
    needle.moveTo(tip);
    needle.lineTo(left);
    needle.lineTo(rght);
    needle.closeSubpath();

    p.setPen(Qt::NoPen);
    p.setBrush(needleColour);
    p.drawPath(needle);

    // Hub, so the pivot reads as a pivot at small sizes.
    p.setBrush(needleColour);
    p.drawEllipse(centre, 2.5, 2.5);
}


GreenHeronApplet::GreenHeronApplet(QWidget* parent)
    : QWidget(parent)
{
    theme::setContainer(this, QStringLiteral("applet/greenheron"));
    setObjectName(QStringLiteral("greenHeronApplet"));
    setAccessibleName(tr("Green Heron antenna switch"));
    setAccessibleDescription(
        tr("Antenna selection for one switch on a Green Heron Everyware "
           "server. Antennas held by another switch are shown as in use and "
           "cannot be selected."));
    hide();
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    setMaximumWidth(260);

    m_model = new GreenHeronModel(this);

    m_noteTimer = new QTimer(this);
    m_noteTimer->setSingleShot(true);
    m_noteTimer->setInterval(kNoteTimeoutMs);
    connect(m_noteTimer, &QTimer::timeout, this, [this]() {
        m_noteLabel->clear();
        m_noteLabel->hide();
    });

    buildUI();

    connect(m_model, &GreenHeronModel::panelChanged,
            this, &GreenHeronApplet::syncFromModel);
    connect(m_model, &GreenHeronModel::connectionStateChanged,
            this, &GreenHeronApplet::syncFromModel);
    connect(m_model, &GreenHeronModel::errorOccurred,
            this, &GreenHeronApplet::note);

    // Restore the last address and switch. Connecting is deliberately left to
    // the operator unless the shared peripheral auto-reconnect preference is
    // on — the switch is often shared with other station software, so opening
    // the client should not silently claim a session.
    const QString savedHost =
        PeripheralSettings::deviceString(kSettingsDevice, kSettingsHost);
    m_hostEdit->setText(savedHost);
    m_portSpin->setValue(PeripheralSettings::deviceInt(
        kSettingsDevice, kSettingsPort, GreenHeron::kDefaultPort));
    m_wantedSwitch = PeripheralSettings::deviceString(kSettingsDevice, kSettingsSwitch);
    refreshSwitchChoices();

    if (!savedHost.isEmpty() && PeripheralSettings::autoReconnect()) {
        toggleConnection();
    }

    syncFromModel();
}

void GreenHeronApplet::buildUI()
{
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(4, 4, 4, 4);
    outer->setSpacing(4);

    // ── Server address ──────────────────────────────────────────────────────
    {
        auto* row = new QHBoxLayout;
        row->setSpacing(4);

        auto* hostLabel = new QLabel(tr("IP"));
        ThemeManager::instance().applyStyleSheet(hostLabel, kLabelStyle);
        row->addWidget(hostLabel);

        m_hostEdit = new QLineEdit;
        m_hostEdit->setObjectName(QStringLiteral("greenHeronHost"));
        m_hostEdit->setAccessibleName(tr("Everyware server IP address"));
        m_hostEdit->setAccessibleDescription(
            tr("IP address or host name of the Green Heron Everyware server "
               "that drives the antenna switches."));
        m_hostEdit->setPlaceholderText(tr("192.0.2.10"));
        m_hostEdit->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        m_hostEdit->setMinimumWidth(0);
        ThemeManager::instance().applyStyleSheet(
            m_hostEdit, QStringLiteral("QLineEdit { %1 }").arg(kFieldStyle));
        connect(m_hostEdit, &QLineEdit::returnPressed,
                this, &GreenHeronApplet::toggleConnection);
        row->addWidget(m_hostEdit, 1);

        auto* portLabel = new QLabel(tr("Port"));
        ThemeManager::instance().applyStyleSheet(portLabel, kLabelStyle);
        row->addWidget(portLabel);

        m_portSpin = new QSpinBox;
        m_portSpin->setObjectName(QStringLiteral("greenHeronPort"));
        m_portSpin->setAccessibleName(tr("Everyware server port"));
        m_portSpin->setAccessibleDescription(
            tr("TCP port the Everyware server listens on. The default is %1.")
                .arg(GreenHeron::kDefaultPort));
        m_portSpin->setRange(1, 65535);
        m_portSpin->setValue(GreenHeron::kDefaultPort);
        m_portSpin->setButtonSymbols(QAbstractSpinBox::NoButtons);
        m_portSpin->setFixedWidth(52);
        m_portSpin->setAlignment(Qt::AlignRight);
        ThemeManager::instance().applyStyleSheet(
            m_portSpin, QStringLiteral("QSpinBox { %1 }").arg(kFieldStyle));
        row->addWidget(m_portSpin);

        outer->addLayout(row);
    }

    // ── Which switch this radio is fed from + connect ────────────────────────
    {
        auto* row = new QHBoxLayout;
        row->setSpacing(4);

        auto* switchLabel = new QLabel(tr("Switch"));
        ThemeManager::instance().applyStyleSheet(switchLabel, kLabelStyle);
        row->addWidget(switchLabel);

        m_switchCombo = new QComboBox;
        m_switchCombo->setObjectName(QStringLiteral("greenHeronSwitch"));
        m_switchCombo->setAccessibleName(tr("Antenna switch"));
        m_switchCombo->setAccessibleDescription(
            tr("Which of the server's switches this radio is connected to. "
               "Only that switch's antennas are shown."));
        m_switchCombo->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        m_switchCombo->setMinimumWidth(0);
        ThemeManager::instance().applyStyleSheet(
            m_switchCombo,
            QStringLiteral("QComboBox { %1 }").arg(kFieldStyle)
                + "QComboBox::drop-down { border: none; }"
                  "QComboBox QAbstractItemView { background: {{color.background.1}}; "
                  "color: {{color.text.primary}}; "
                  "selection-background-color: {{color.background.2}}; }");
        connect(m_switchCombo, &QComboBox::currentTextChanged, this,
                [this](const QString& name) {
                    if (name.isEmpty()) {
                        return;
                    }
                    m_wantedSwitch = name;
                    PeripheralSettings::setDeviceString(kSettingsDevice,
                                                        kSettingsSwitch, name);
                    syncFromModel();
                });
        row->addWidget(m_switchCombo, 1);

        m_connectBtn = new QPushButton(tr("Connect"));
        m_connectBtn->setObjectName(QStringLiteral("greenHeronConnect"));
        m_connectBtn->setAccessibleName(tr("Connect to Everyware server"));
        m_connectBtn->setFixedWidth(72);
        ThemeManager::instance().applyStyleSheet(m_connectBtn,
                                                 kConnectButtonStyle);
        connect(m_connectBtn, &QPushButton::clicked,
                this, &GreenHeronApplet::toggleConnection);
        row->addWidget(m_connectBtn);

        outer->addLayout(row);
    }

    m_statusLabel = new QLabel(tr("Not connected"));
    m_statusLabel->setObjectName(QStringLiteral("greenHeronStatus"));
    m_statusLabel->setAlignment(Qt::AlignCenter);
    ThemeManager::instance().applyStyleSheet(
        m_statusLabel, statusStyle(QStringLiteral("color.text.disabled")));
    outer->addWidget(m_statusLabel);

    // ── The chosen switch's antenna ports ───────────────────────────────────
    m_portHost = new QWidget;
    m_portHost->setObjectName(QStringLiteral("greenHeronPorts"));
    m_portLayout = new QVBoxLayout(m_portHost);
    m_portLayout->setContentsMargins(0, 0, 0, 0);
    m_portLayout->setSpacing(2);
    outer->addWidget(m_portHost);

    // ── Rotator ─────────────────────────────────────────────────────────────
    //
    // Below the antennas because that is the order the operator works in, and
    // hidden as a whole until the device reports a heading: a server whose
    // rotator controller is off announces nothing at all, so an empty rotator
    // row would be a permanent fixture on most installations.
    {
        m_rotorSection = new QWidget;
        m_rotorSection->setObjectName(QStringLiteral("greenHeronRotorSection"));
        auto* section = new QVBoxLayout(m_rotorSection);
        section->setContentsMargins(0, 2, 0, 0);
        section->setSpacing(2);

        auto* headerRow = new QHBoxLayout;
        headerRow->setSpacing(4);

        auto* rotorLabel = new QLabel(tr("Rotor"));
        ThemeManager::instance().applyStyleSheet(rotorLabel, kLabelStyle);
        headerRow->addWidget(rotorLabel);

        // One rotator is the normal case, and a combo offering a single choice
        // is furniture. It appears only when the device reports more than one.
        m_rotorCombo = new QComboBox;
        m_rotorCombo->setObjectName(QStringLiteral("greenHeronRotor"));
        m_rotorCombo->setAccessibleName(tr("Rotator"));
        m_rotorCombo->setAccessibleDescription(
            tr("Which of the server's rotators this tile drives."));
        m_rotorCombo->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        m_rotorCombo->setMinimumWidth(0);
        ThemeManager::instance().applyStyleSheet(
            m_rotorCombo,
            QStringLiteral("QComboBox { %1 }").arg(kFieldStyle)
                + "QComboBox::drop-down { border: none; }"
                  "QComboBox QAbstractItemView { background: {{color.background.1}}; "
                  "color: {{color.text.primary}}; "
                  "selection-background-color: {{color.background.2}}; }");
        m_rotorCombo->hide();
        connect(m_rotorCombo, &QComboBox::currentTextChanged, this,
                [this](const QString& name) {
                    if (name.isEmpty()) {
                        return;
                    }
                    m_wantedRotor = name;
                    syncRotorFromModel();
                });
        headerRow->addWidget(m_rotorCombo, 1);

        m_rotorReadout = new QLabel;
        m_rotorReadout->setObjectName(QStringLiteral("greenHeronRotorReadout"));
        m_rotorReadout->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        ThemeManager::instance().applyStyleSheet(
            m_rotorReadout, statusStyle(QStringLiteral("color.text.primary")));
        headerRow->addWidget(m_rotorReadout, 1);

        section->addLayout(headerRow);

        auto* turnRow = new QHBoxLayout;
        turnRow->setSpacing(4);

        m_headingEdit = new QLineEdit;
        m_headingEdit->setObjectName(QStringLiteral("greenHeronHeading"));
        m_headingEdit->setAccessibleName(tr("Heading to turn to"));
        m_headingEdit->setAccessibleDescription(
            tr("Degrees, 0 to 360. Typing here only proposes a heading; the "
               "Turn button sends it. There is no stop command for this "
               "rotator, so a turn cannot be cancelled once it starts."));
        m_headingEdit->setPlaceholderText(tr("degrees"));
        m_headingEdit->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        m_headingEdit->setMinimumWidth(0);
        // The C locale, matching the wire and the readout, so a dot is never
        // the thing that gets rejected on a field whose only legal output is a
        // wire heading.
        //
        // It does NOT make the field dot-only, and that was measured rather
        // than assumed: QDoubleValidator answers Intermediate — not Invalid —
        // for a foreign decimal mark, so QLineEdit accepts the keystroke under
        // either locale and "64,3" lands in the field intact. sendTurn()
        // normalises it. Both halves are load-bearing; neither is redundant.
        auto* validator = new QDoubleValidator(GreenHeron::kMinHeadingDegrees,
                                               GreenHeron::kMaxHeadingDegrees,
                                               GreenHeron::kHeadingDecimals,
                                               m_headingEdit);
        validator->setLocale(QLocale::c());
        m_headingEdit->setValidator(validator);
        ThemeManager::instance().applyStyleSheet(
            m_headingEdit, QStringLiteral("QLineEdit { %1 }").arg(kFieldStyle));
        connect(m_headingEdit, &QLineEdit::returnPressed,
                this, &GreenHeronApplet::sendTurn);
        turnRow->addWidget(m_headingEdit, 1);

        m_turnBtn = new QPushButton(tr("Turn"));
        m_turnBtn->setObjectName(QStringLiteral("greenHeronTurn"));
        m_turnBtn->setAccessibleName(tr("Turn the rotator"));
        m_turnBtn->setAccessibleDescription(
            tr("Send the typed heading to the rotator. This starts a rotation "
               "that cannot be stopped from here."));
        m_turnBtn->setFixedWidth(72);
        ThemeManager::instance().applyStyleSheet(m_turnBtn, kConnectButtonStyle);
        connect(m_turnBtn, &QPushButton::clicked, this, &GreenHeronApplet::sendTurn);
        turnRow->addWidget(m_turnBtn);

        section->addLayout(turnRow);

        // Inside the section, so the POINT gate and the silence teardown
        // cover it without a second copy of either rule.
        m_compass = new RotorCompass;
        m_compass->setObjectName(QStringLiteral("greenHeronRotorCompass"));
        // Explicit, and load-bearing: without it the compass is merely an
        // un-hidden child, and the first time a rotator appears in the DOCKED
        // tile Qt would show it along with the rest of the section.
        m_compass->hide();
        section->addWidget(m_compass);

        m_rotorSection->hide();
        outer->addWidget(m_rotorSection);
    }

    m_noteLabel = new QLabel;
    m_noteLabel->setObjectName(QStringLiteral("greenHeronNote"));
    m_noteLabel->setWordWrap(true);
    ThemeManager::instance().applyStyleSheet(
        m_noteLabel, statusStyle(QStringLiteral("color.accent.warning")));
    m_noteLabel->hide();
    outer->addWidget(m_noteLabel);
}

QString GreenHeronApplet::selectedSwitch() const
{
    return effectiveSwitch();
}

QString GreenHeronApplet::selectedRotor() const
{
    return effectiveRotor();
}

QString GreenHeronApplet::effectiveRotor() const
{
    // Only rotators that are actually reporting: a name the device has stopped
    // naming is a controller that is off, and holding onto it would put a Turn
    // button in front of an antenna nobody is driving.
    const QStringList live = m_model->rotorNames();
    if (m_wantedRotor.isEmpty()) {
        // Nothing chosen — one rotator is the normal case and there is no
        // choice to lose.
        return live.isEmpty() ? QString{} : live.first();
    }
    // A CHOICE THE OPERATOR MADE IS NEVER SILENTLY REPOINTED, exactly as
    // effectiveSwitch() above refuses to. Falling back to live.first() here
    // would mean: operator picks Rotor-B, types a heading, B's controller goes
    // off, and Turn aims Rotor-A instead — with no stop verb to recall it.
    // Showing nothing is the honest answer; the row returns when B does.
    return live.contains(m_wantedRotor) ? m_wantedRotor : QString{};
}

QString GreenHeronApplet::effectiveSwitch() const
{
    // The roster arrives over several reads and has no terminator, so "not on
    // the roster" never means "gone" with any certainty — it may simply be
    // announced in a read that has not landed yet. A remembered choice is
    // therefore never overridden: the tile shows it, finds no ports for it
    // until its SWITCHADD lands, and so draws nothing clickable. Repointing at
    // another switch would put THAT switch's antennas under the operator's
    // cursor, and these buttons move real relays.
    if (!m_wantedSwitch.isEmpty()) {
        return m_wantedSwitch;
    }
    // Nothing remembered — first run. Showing the first switch beats showing an
    // empty tile next to a connected server, and there is no choice to lose.
    const QStringList names = m_model->displayOrder();
    return names.isEmpty() ? QString{} : names.first();
}

void GreenHeronApplet::toggleConnection()
{
    if (m_model->isWanted()) {
        m_model->disconnectFromHost();
        return;
    }

    const QString host = m_hostEdit->text().trimmed();
    if (host.isEmpty()) {
        note(tr("Enter the Everyware server's IP address first"));
        return;
    }
    const auto port = static_cast<quint16>(m_portSpin->value());

    PeripheralSettings::setDeviceString(kSettingsDevice, kSettingsHost, host);
    PeripheralSettings::setDeviceInt(kSettingsDevice, kSettingsPort, port);

    m_model->connectToHost(host, port);
}

// ── Switch chooser ──────────────────────────────────────────────────────────

void GreenHeronApplet::refreshSwitchChoices()
{
    // The device names its own switches, so the list can only be populated
    // once a roster has arrived. Until then the combo carries the remembered
    // choice alone, so the operator can see what it will re-select.
    QStringList names = m_model->displayOrder();
    // Keep the remembered choice in the list until the device actually
    // announces it, so the operator sees what the tile is pointed at rather
    // than a switch it silently fell back to. Appended at the end rather than
    // re-sorted: displayOrder()'s comparator lives in the model, and this entry
    // is transient — it merges into its sorted place the moment its SWITCHADD
    // arrives.
    if (!m_wantedSwitch.isEmpty() && !names.contains(m_wantedSwitch)) {
        names.append(m_wantedSwitch);
    }
    // Rebuild only when the roster actually changed: clearing the combo drops
    // the current selection, and doing that on every status push would fight
    // an operator who is mid-choice.
    QStringList current;
    current.reserve(m_switchCombo->count());
    for (int i = 0; i < m_switchCombo->count(); ++i) {
        current.append(m_switchCombo->itemText(i));
    }
    if (current == names) {
        return;
    }

    const QSignalBlocker blocker(m_switchCombo);
    m_switchCombo->clear();
    m_switchCombo->addItems(names);

    // Re-select the remembered switch if the device has announced it; otherwise
    // show the first one rather than an empty list next to a connected server.
    //
    // The fallback does NOT write back to m_wantedSwitch or to settings. The
    // roster spans several TCP reads, so a switch announced late is absent here
    // for a moment through no fault of the operator; persisting the fallback
    // would discard their choice on every connect and then drive the wrong
    // switch's relays. Leaving it be means the combo snaps back on its own once
    // the late SWITCHADD lands.
    const int index = names.indexOf(m_wantedSwitch);
    if (index >= 0) {
        m_switchCombo->setCurrentIndex(index);
    } else if (!names.isEmpty()) {
        m_switchCombo->setCurrentIndex(0);
    }
}

// ── Port list ───────────────────────────────────────────────────────────────

void GreenHeronApplet::rebuildPortList()
{
    while (QLayoutItem* item = m_portLayout->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    m_portButtons.clear();

    const QString switchName = m_builtForSwitch;
    if (switchName.isEmpty() || m_builtForPorts.isEmpty()) {
        return;
    }

    for (const QString& port : m_builtForPorts) {
        auto* button = new QPushButton(port);
        button->setCheckable(true);
        ThemeManager::instance().applyStyleSheet(button, kPortButtonStyle);
        button->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        button->setMinimumWidth(0);
        button->setAccessibleName(port);
        connect(button, &QPushButton::clicked, this, [this, switchName, port]() {
            if (m_model->selectPort(switchName, port)) {
                // A command that went out is not a warning. It is still not a
                // confirmation either — the tile only lights when the device
                // echoes the move back.
                showNote(tr("Sent %1 → %2").arg(switchName, port),
                         QStringLiteral("color.accent.success"));
            }
        });
        m_portLayout->addWidget(button);
        m_portButtons.insert(port, button);
    }
}

void GreenHeronApplet::syncFromModel()
{
    refreshSwitchChoices();
    updateStatus();
    syncRotorFromModel();

    // effectiveSwitch(), not m_wantedSwitch: the buttons built here send to the
    // relays, so they must name the same switch the combo is showing.
    const QString switchName = effectiveSwitch();
    const GreenHeronSwitchState state = m_model->switchState(switchName);

    if (switchName != m_builtForSwitch || state.ports != m_builtForPorts) {
        m_builtForSwitch = switchName;
        m_builtForPorts = state.ports;
        rebuildPortList();
    }

    // Stale means what is on screen is the last thing the device said, not
    // something this connection has confirmed — the link is down, or it is
    // back up and the replay has not landed. Leave it visible; it is still the
    // best information available. Refuse input either way: in the first case
    // nothing would reach the relays, and in the second something would, from
    // a roster that predates the drop.
    m_portHost->setEnabled(m_model->isReady());

    const QMap<QString, QString> held = m_model->locksBySwitch(switchName);
    for (const QString& port : state.ports) {
        QPushButton* button = m_portButtons.value(port);
        if (button == nullptr) {
            continue;
        }
        const QSignalBlocker blocker(button);
        const QString holder = held.value(port);
        const QString previousDescription = button->accessibleDescription();

        if (state.selected == port) {
            button->setText(tr("%1  ·  ON").arg(port));
            button->setChecked(true);
            button->setEnabled(true);
            button->setToolTip(tr("%1 is on %2").arg(switchName, port));
            button->setAccessibleDescription(
                tr("%1 is currently selected on %2").arg(port, switchName));
        } else if (!holder.isEmpty()) {
            // An antenna feeds one switch at a time, so the device would
            // refuse this anyway. Say who has it rather than offering a click
            // that silently does nothing.
            button->setText(tr("%1  ·  in use by %2").arg(port, holder));
            button->setChecked(false);
            button->setEnabled(false);
            button->setToolTip(tr("In use by %1").arg(holder));
            button->setAccessibleDescription(
                tr("%1 is in use by %2 and cannot be selected on %3")
                    .arg(port, holder, switchName));
        } else {
            button->setText(port);
            button->setChecked(false);
            button->setEnabled(true);
            button->setToolTip(tr("Select %1 on %2").arg(port, switchName));
            button->setAccessibleDescription(
                tr("Select %1 on %2").arg(port, switchName));
        }

        if (button->accessibleDescription() != previousDescription) {
            announce(button, QAccessible::DescriptionChanged);
        }
    }
}

// ── Rotator ─────────────────────────────────────────────────────────────────

void GreenHeronApplet::refreshRotorChoices()
{
    const QStringList live = m_model->rotorNames();

    QStringList current;
    current.reserve(m_rotorCombo->count());
    for (int i = 0; i < m_rotorCombo->count(); ++i) {
        current.append(m_rotorCombo->itemText(i));
    }
    if (current != live) {
        const QSignalBlocker blocker(m_rotorCombo);
        m_rotorCombo->clear();
        m_rotorCombo->addItems(live);
        const int index = live.indexOf(m_wantedRotor);
        m_rotorCombo->setCurrentIndex(index >= 0 ? index : 0);
    }
    // A chooser offering one choice is furniture; the device reports one
    // rotator on every installation seen so far.
    m_rotorCombo->setVisible(live.size() > 1);
}

void GreenHeronApplet::syncRotorFromModel()
{
    refreshRotorChoices();

    // A commanded heading describes one rotator on one power cycle. Left to
    // rot, it would come back an hour later beside a live reading as an
    // "asked" this session never sent — the same phantom kRotorSilentAfterMs
    // exists to kill, one layer up.
    const QStringList live = m_model->rotorNames();
    for (auto it = m_commanded.begin(); it != m_commanded.end();) {
        it = live.contains(it.key()) ? std::next(it) : m_commanded.erase(it);
    }

    const QString name = effectiveRotor();
    const bool present = !name.isEmpty();

    // The whole section, not just the button. There is no "rotator offline"
    // state to render: the device reports absence and a powered-off controller
    // identically, by saying nothing, so anything drawn here would be a claim
    // the wire does not support.
    m_rotorSection->setVisible(present);
    if (!present) {
        if (!m_lastRotorAnnouncement.isEmpty()) {
            m_lastRotorAnnouncement.clear();
            // The rotator going away is worth saying; announced on the readout
            // because that is the widget carrying the description, and it says
            // so before the row is gone from the tree.
            m_rotorReadout->setAccessibleDescription(
                tr("The rotator has stopped reporting a heading"));
            announce(m_rotorReadout, QAccessible::DescriptionChanged);
        }
        return;
    }

    const GreenHeronRotorState rotor = m_model->rotorState(name);
    const QString reported =
        QStringLiteral("%1°").arg(rotor.heading, 0, 'f', GreenHeron::kHeadingDecimals);

    QString readout = reported;
    QString description = tr("%1 reports %2").arg(name, reported);
    const auto asked = m_commanded.constFind(name);
    if (asked != m_commanded.constEnd()) {
        const QString askedText =
            QStringLiteral("%1°").arg(*asked, 0, 'f', GreenHeron::kHeadingDecimals);
        const QString deltaText =
            QStringLiteral("%1°").arg(GreenHeron::headingDelta(rotor.heading, *asked),
                                      0, 'f', GreenHeron::kHeadingDecimals);
        // Reported, commanded and the gap — never conflated, and never
        // resolved into a verdict. The rotator settles about two degrees short
        // along whichever way it turned and dithers over a wider band than
        // that at rest, so "arrived" is not a statement this data can support.
        readout = tr("%1 · asked %2 · Δ%3").arg(reported, askedText, deltaText);
        description = tr("%1 reports %2; %3 was asked for, a difference of %4")
                          .arg(name, reported, askedText, deltaText);
    }

    // The same two numbers the readout just rendered — the compass is a second
    // view of that line, never a second source for it.
    m_compass->setHeading(rotor.heading,
                          asked != m_commanded.constEnd(),
                          asked != m_commanded.constEnd() ? *asked : 0.0);

    m_rotorReadout->setText(readout);
    m_rotorReadout->setAccessibleName(tr("Rotator heading"));
    m_rotorReadout->setAccessibleDescription(description);
    m_rotorReadout->setToolTip(description);

    // The heading is live and dithers at rest, so it is deliberately NOT
    // announced on every POINT — that is roughly once a second, and it would
    // make the tile unusable with a screen reader. Only the rotator appearing
    // is announced here; a command going out is announced by the note.
    if (m_lastRotorAnnouncement != name) {
        m_lastRotorAnnouncement = name;
        announce(m_rotorReadout, QAccessible::DescriptionChanged);
    }

    // Live and connected is the whole gate. isReady() is the ANTENNA panel's
    // gate — it waits on SWITCHADD, which has nothing to say about a rotator —
    // and the model's own liveness check is the stronger statement anyway: it
    // means a POINT arrived on this very socket.
    m_headingEdit->setEnabled(true);
    m_turnBtn->setEnabled(true);
}

void GreenHeronApplet::setFloating(bool on)
{
    if (m_floating == on) {
        return;
    }
    m_floating = on;
    // Visibility only — the compass keeps its heading either way, so docking
    // and floating again does not blank the dial until the next POINT.
    m_compass->setVisible(on);
}

void GreenHeronApplet::sendTurn()
{
    const QString name = effectiveRotor();
    if (name.isEmpty()) {
        note(tr("No rotator is reporting a heading"));
        return;
    }

    const QString text = m_headingEdit->text().trimmed();
    if (text.isEmpty()) {
        note(tr("Type a heading in degrees first"));
        return;
    }
    // The validator lets a comma through (see buildUI — Intermediate, not
    // Invalid), so an operator on a comma-decimal system types "64,3" and it
    // reaches here verbatim. Normalising is what stops that becoming either a
    // refused turn that looks like a broken button or, worse, a 643 read as a
    // group separator.
    bool ok = false;
    QString normalised = text;
    normalised.replace(QLatin1Char(','), QLatin1Char('.'));
    const double degrees = normalised.toDouble(&ok);
    if (!ok) {
        note(tr("\"%1\" is not a heading").arg(text));
        return;
    }

    if (!m_model->turnTo(name, degrees)) {
        // turnTo() has already set the reason through errorOccurred.
        return;
    }

    // Recorded ONLY to render "asked X · ΔY" beside the reported heading. The
    // readout keeps showing what the device reports; nothing here writes a
    // commanded value into the position display.
    m_commanded.insert(name, degrees);
    showNote(tr("Sent %1 → %2°")
                 .arg(name,
                      QString::number(degrees, 'f', GreenHeron::kHeadingDecimals)),
             QStringLiteral("color.accent.success"));
    syncRotorFromModel();
}

void GreenHeronApplet::updateStatus()
{
    const bool wanted = m_model->isWanted();
    m_connectBtn->setText(wanted ? tr("Disconnect") : tr("Connect"));
    m_connectBtn->setAccessibleName(wanted ? tr("Disconnect from Everyware server")
                                           : tr("Connect to Everyware server"));
    m_hostEdit->setEnabled(!wanted);
    m_portSpin->setEnabled(!wanted);

    QString text;
    // Idle reads as neither good nor bad, so it takes the muted text token
    // rather than a status colour.
    QString colourToken = QStringLiteral("color.text.disabled");
    if (m_model->isConnected() && !m_model->isReady()) {
        // TCP is up and the device has not spoken yet. Saying "connected"
        // here would put a green light on a panel nobody has vouched for.
        text = tr("Connected — waiting for switch state");
        colourToken = QStringLiteral("color.accent.warning");
    } else if (m_model->isConnected()) {
        const QString switchName = effectiveSwitch();
        const GreenHeronSwitchState state = m_model->switchState(switchName);
        if (state.ports.isEmpty()) {
            text = switchName.isEmpty()
                       ? tr("Connected — waiting for roster")
                       : tr("Connected — %1 not reported").arg(switchName);
            colourToken = QStringLiteral("color.accent.warning");
        } else {
            text = state.selected.isEmpty()
                       ? tr("%1 connected").arg(switchName)
                       : tr("%1 on %2").arg(switchName, state.selected);
            colourToken = QStringLiteral("color.accent.success");
        }
    } else if (m_model->isStale()) {
        text = tr("Reconnecting — state is stale");
        colourToken = QStringLiteral("color.accent.warning");
    } else if (wanted) {
        text = tr("Connecting…");
        colourToken = QStringLiteral("color.accent.warning");
    } else {
        text = tr("Not connected");
    }

    const bool statusChanged = m_statusLabel->text() != text;
    m_statusLabel->setText(text);
    ThemeManager::instance().applyStyleSheet(m_statusLabel,
                                             statusStyle(colourToken));
    m_statusLabel->setAccessibleName(tr("Green Heron connection status"));
    m_statusLabel->setAccessibleDescription(text);
    if (statusChanged) {
        announce(m_statusLabel, QAccessible::DescriptionChanged);
    }
}

void GreenHeronApplet::note(const QString& text)
{
    // The plain form is the error/refusal one, so it keeps the warning token.
    // It stays a single-argument slot because errorOccurred connects straight
    // to it, and Qt's function-pointer connect does not honour default
    // arguments.
    showNote(text, QStringLiteral("color.accent.warning"));
}

void GreenHeronApplet::showNote(const QString& text, const QString& colourToken)
{
    m_noteLabel->setText(text);
    ThemeManager::instance().applyStyleSheet(m_noteLabel, statusStyle(colourToken));
    m_noteLabel->setAccessibleDescription(text);
    m_noteLabel->setVisible(!text.isEmpty());
    m_noteTimer->start();
}

} // namespace AetherSDR
