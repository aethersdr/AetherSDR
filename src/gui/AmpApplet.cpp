#include "AmpApplet.h"
#include "HGauge.h"
#include "AccessoryPanelWidgets.h"
#include "GuardedSlider.h"
#include "core/AppSettings.h"
#include "models/AmpModel.h"
#include <QElapsedTimer>

#include <QAbstractItemView>
#include <QAccessible>
#include <QVBoxLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalBlocker>
#include <QSpacerItem>
#include "core/ThemeManager.h"
#include "MeterSmoother.h"

namespace AetherSDR {

namespace {
QString fanModeLabel(const QString& mode)
{
    if (mode == "STANDARD") return "Fan: Std";
    if (mode == "CONTEST") return "Fan: Contest";
    if (mode == "BROADCAST") return "Fan: Bcast";

    return "Fan";
}

// The panel key's caption. One letter, because the key is one square: S, C
// and B are the modes' own initials, and the full word is on the tooltip and
// in the accessible name so nothing is only available as an initial.
QString fanModeLetter(const QString& mode)
{
    if (mode == "CONTEST") return QStringLiteral("C");
    if (mode == "BROADCAST") return QStringLiteral("B");
    return QStringLiteral("S");
}
// Left-side label that shows the field name + live value ("PWR 1148").
// Fixed width so all three gauge rows line up.
QLabel* makeValueLabel(QWidget* parent)
{
    auto* lbl = new QLabel(parent);
    lbl->setFixedWidth(72);
    lbl->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    // Fixed vertically, not just horizontally. The bottom pad is meant to be
    // the only item in the column that can change height — that is what makes
    // dragging the panel shorter drain the pad before anything above it moves.
    // The pad's Expanding policy outranks a label's Preferred one today, so
    // leaving this off happens to work; it works by priority rather than by
    // construction, and the tuner's row labels state it outright.
    lbl->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    lbl->setStyleSheet("QLabel { color: #c8d8e8; font-size: 11px; font-weight: bold; }");
    return lbl;
}

// ── Expanded panel design metrics ───────────────────────────────────────────
//
// The size at which every metric below is its literal value; the actual
// metrics are that value times one scale derived from the room the panel has.
// See AccessoryPanelWidgets.h for why the width is a constant and the height
// is measured exactly once.
//
// What the widest row costs at scale 1.0: a port strip — letter, PTT lamp,
// band and bias chips, the source radio's name and the state cell — plus the
// readouts, the fan pull-down and the key below them, and the column's side
// margins.
//
// Deliberately generous. The column's width is not exactly proportional to
// the scale — a button's frame and the readout's minimum width are constants
// inside it — so the contents cost roughly 265 * scale + 63 rather than a
// clean multiple. Solving that against this divisor is what decides whether a
// narrow panel merely cramps or actually clips: at 420 every panel from the
// minimum scale upward has room to spare, while a divisor tight enough to hit
// scale 1.0 at the contents' own 300px would clip anything under 540px wide.
constexpr qreal kDesignWidth  = 420.0;
// Only a first guess: applyDensity replaces it with the measured value as
// soon as there is a laid-out column to measure.
constexpr qreal kDesignHeight = 292.0;
constexpr int   kBottomGap = kPanelBottomGap;

// The panel key stands as tall as the tuner's do, so an operator with both
// applets on the canvas sees one control language rather than two.
constexpr int kKeyDesignHeight = 44;
constexpr int kKeyFontDesignPx = 13;
constexpr int kFanKeyFontDesignPx = 17;

// Style sheets reach the widgets over the next few turns of the event loop, and
// each turn brings the measurement closer: 285px on the first, 243 on the
// second, 234 by the sixth, against the 218 the column actually settles at.
// Bounded, because the tail is asymptotic and the panel cannot spend the
// session re-measuring itself.
constexpr int kMaxCalibrationPasses = 6;
// Breathing room around the widest caption, in design pixels.
constexpr int kKeyPaddingDesignPx = 18;

constexpr const char* kBtnStyle =
    "QPushButton { background: {{color.background.2}}; border: 1px solid {{color.background.2}}; "
    "border-radius: 3px; color: {{color.text.primary}}; font-size: 10px; font-weight: bold; }"
    "QPushButton:hover { background: {{color.background.1}}; }";
constexpr const char* kFaultStyle =
    "QPushButton { background: {{color.accent.danger}}; border: 1px solid {{color.accent.danger}}; "
    "border-radius: 3px; color: {{color.background.0}}; font-size: 10px; font-weight: bold; }";
constexpr const char* kOperateStyle =
    "QPushButton { background: #006030; border: 1px solid #008040; "
    "border-radius: 3px; color: {{color.text.primary}}; font-size: 10px; font-weight: bold; }"
    "QPushButton:hover { background: #007040; }";

// The panel key: resting, and lit while the amplifier is in the state it
// selects. Shared tokens with the tuner's keys — one accessory palette.
constexpr const char* kPanelKeyIdleStyle =
    "QPushButton { background: {{color.background.2}}; border: 1px solid {{color.background.2}}; "
    "border-radius: 3px; color: {{color.text.primary}}; font-weight: bold; }"
    "QPushButton:hover { background: {{color.background.1}}; }"
    "QPushButton:disabled { color: {{color.text.secondary}}; }";
// MEffA lit and optimising. The vendor's indicator turns red for this; red on
// this panel already means a fault, so the amplifier's own "working" green is
// used and the dimmed state below carries the distinction instead.
constexpr const char* kPanelKeyMeffaActiveStyle =
    "QPushButton { background: {{color.accessory.key.meffa.active.background}}; "
    "border: 1px solid {{color.accessory.key.meffa.active.border}}; "
    "border-radius: 3px; color: {{color.accessory.key.meffa.active.foreground}}; "
    "font-weight: bold; }"
    "QPushButton:hover { background: {{color.accessory.key.meffa.active.hover}}; }"
    // Inert until the `setup` group is known. Without an explicit :disabled
    // rule a stylesheet that hard-sets background and colour wins over the
    // disabled palette, and the key would look exactly as it does once live.
    "QPushButton:disabled { background: {{color.background.2}}; "
    "border: 1px solid {{color.background.2}}; color: {{color.text.secondary}}; }";
// MEffA enabled but inapplicable — the PA is in class AAB, where the algorithm
// does not apply (§4.3.1). The vendor dims its indicator for exactly this and
// so does this: outlined, not filled, because the operator HAS enabled it and
// nothing is wrong.
constexpr const char* kPanelKeyMeffaStandbyStyle =
    "QPushButton { background: transparent; "
    "border: 1px solid {{color.accessory.key.meffa.standby.foreground}}; "
    "border-radius: 3px; color: {{color.accessory.key.meffa.standby.foreground}}; "
    "font-weight: bold; }"
    "QPushButton:hover { background: {{color.background.1}}; }"
    "QPushButton:disabled { border: 1px solid {{color.background.2}}; "
    "color: {{color.text.secondary}}; }";
constexpr const char* kPanelKeyStandbyStyle =
    "QPushButton { background: {{color.accessory.key.standby.background}}; "
    "border: 1px solid {{color.accessory.key.standby.foreground}}; border-radius: 3px; "
    "color: {{color.accessory.key.standby.foreground}}; font-weight: bold; }";

constexpr const char* kAmpAppletSettingsKey = "AmpApplet";
constexpr const char* kTempFahrenheitField = "tempFahrenheit";

QJsonObject readAmpAppletSettings()
{
    const QString json = AppSettings::instance()
        .value(kAmpAppletSettingsKey, QString{}).toString();
    if (json.isEmpty()) {
        return {};
    }

    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    return doc.isObject() ? doc.object() : QJsonObject{};
}

void writeAmpAppletSettings(const QJsonObject& obj)
{
    auto& settings = AppSettings::instance();
    settings.setValue(kAmpAppletSettingsKey,
        QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact)));
    settings.save();
}

bool readTempFahrenheit()
{
    return readAmpAppletSettings()
        .value(kTempFahrenheitField)
        .toString(QStringLiteral("False")) == QStringLiteral("True");
}

void writeTempFahrenheit(bool enabled)
{
    QJsonObject obj = readAmpAppletSettings();
    obj[kTempFahrenheitField] =
        enabled ? QStringLiteral("True") : QStringLiteral("False");
    writeAmpAppletSettings(obj);
}

float displayTemp(float degC, bool fahrenheit)
{
    if (!fahrenheit) {
        return degC;
    }
    return degC * 9.0f / 5.0f + 32.0f;
}

QString formatTemp(float degC, bool fahrenheit)
{
    return QStringLiteral("%1").arg(displayTemp(degC, fahrenheit), 0, 'f', 1);
}

// Every value in the bottom row is right-aligned in a field this wide and
// drawn in a fixed-width face. The row is four readouts abreast, so any
// reading that changes width shuffles everything to its right — and these
// arrive five times a second, which makes the whole row twitch. The face
// handles a 1 becoming an 8; the field handles 9.9 becoming 10.0.
//
// Five characters is what the widest of them needs: "100.4" for a PA
// temperature in Fahrenheit.
constexpr int kValueFieldChars = 5;

QString pad(const QString& value)
{
    return value.rightJustified(kValueFieldChars);
}

QString voltsReadout(const QString& label, const QString& value)
{
    return QStringLiteral("%1 %2 V").arg(label).arg(pad(value));
}

// The state cell on a port strip. It speaks only when it has something to
// say. Operating is the normal condition and needs no word for it — the
// amplifier's own panel carries none — and keying is already on the PTT lamp
// beside it, so both leave the cell empty. What is left is the states an
// operator has to do something about, and those get their name.
//
// STANDBY never reaches here: the banner has replaced the strips by then.
QString stateCell(const QString& state)
{
    if (state == QLatin1String("FAULT"))
        return QStringLiteral("FAULT");
    if (state == QLatin1String("POWERUP"))
        return QStringLiteral("PWRUP");
    if (state == QLatin1String("SELFCHECK"))
        return QStringLiteral("CHECK");
    return QString();
}

} // namespace

AmpApplet::AmpApplet(QWidget* parent)
    : QWidget(parent)
{
    theme::setContainer(this, QStringLiteral("applet/amp"));
    buildUI();
}

void AmpApplet::buildUI()
{
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    // Same reason as the column inside it — see m_vbox below. Both have to
    // stand down, or the body's own minimum reaches the applet through this
    // one and the floor ratchets anyway.
    outer->setSizeConstraint(QLayout::SetNoConstraint);

    auto* body = new QWidget;
    m_vbox = new QVBoxLayout(body);
    // The column must not dictate how small the panel can be — its minimum is
    // the sum of children the scale has just sized. minimumSizeHint() answers
    // that from the minimum scale instead.
    m_vbox->setSizeConstraint(QLayout::SetNoConstraint);
    auto* vbox = m_vbox;
    vbox->setContentsMargins(4, 2, 4, 2);
    vbox->setSpacing(2);

    // Built first: the panel's keys sit beside the port strips, so they have
    // to exist before that row is assembled.
    buildExpandedUI();

    // ── PWR row ──────────────────────────────────────────────────────────────
    m_pwrLabel = makeValueLabel(this);
    m_pwrLabel->setText("PWR");
    m_fwdGauge = new HGauge(0.0f, 2000.0f, 1500.0f, "", "",
        {{0, "0"}, {500, "500"}, {1000, "1K"}, {1500, "1.5K"}, {2000, "2K"}},
        this, 1000.0f);
    m_fwdGauge->setWindowPeakEnabled(true);
    m_fwdGauge->setAccessibleName(tr("Forward power"));
    auto* pwrRow = new QHBoxLayout;
    // Zero margins, like every other nested layout here. A QLayout that is
    // never given contents margins takes the style's, and those are a constant
    // number of pixels that does NOT follow the panel's scale. Left in, three
    // gauge rows contribute ~33px that the height budget counts at scale 1.0
    // and then keeps charging for at every smaller scale — so the contents
    // never cost what the budget says they do, and the pad stops landing at
    // its minimum when height becomes the limiting term.
    pwrRow->setContentsMargins(0, 0, 0, 0);
    pwrRow->setSpacing(4);
    pwrRow->addWidget(m_pwrLabel);
    pwrRow->addWidget(m_fwdGauge, 1);
    vbox->addLayout(pwrRow);

    // ── DRV row ──────────────────────────────────────────────────────────────
    // Exciter power at the amplifier's input, directly under the output it
    // produces: the pair is the amplifier's gain, and reading it off two
    // stacked bars is the whole reason this row exists. A PGXL delivering
    // 16 W for 11 W of drive is visibly broken here and invisible anywhere
    // else in the application.
    //
    // Full scale is the meter's own declared ceiling — the radio publishes
    // DRV as 10.0..50.0 dBm, and 50 dBm is 100 W. The amplifier reaches rated
    // output well below that, so the top of the scale is a limit, not a
    // target: yellow from 50 W, red from 75 W.
    m_drvLabel = makeValueLabel(this);
    m_drvLabel->setText("DRV");
    m_drvGauge = new HGauge(0.0f, 100.0f, 75.0f, "", "",
        {{0, "0"}, {25, "25"}, {50, "50"}, {75, "75"}, {100, "100"}},
        this, 50.0f);
    m_drvGauge->setAccessibleName(tr("Drive power"));
    auto* drvRow = new QHBoxLayout;
    drvRow->setContentsMargins(0, 0, 0, 0);
    drvRow->setSpacing(4);
    drvRow->addWidget(m_drvLabel);
    drvRow->addWidget(m_drvGauge, 1);
    vbox->addLayout(drvRow);

    // ── SWR row ──────────────────────────────────────────────────────────────
    m_swrLabel = makeValueLabel(this);
    m_swrLabel->setText("SWR");
    m_swrGauge = new HGauge(1.0f, 3.0f, 2.5f, "", "",
        {{1.0f, "1"}, {1.5f, "1.5"}, {2.0f, "2"}, {2.5f, "2.5"}, {3.0f, "3"}},
        this, 2.0f);
    m_swrGauge->setAccessibleName(tr("SWR"));
    auto* swrRow = new QHBoxLayout;
    swrRow->setContentsMargins(0, 0, 0, 0);
    swrRow->setSpacing(4);
    swrRow->addWidget(m_swrLabel);
    swrRow->addWidget(m_swrGauge, 1);
    vbox->addLayout(swrRow);

    // ── Id row ───────────────────────────────────────────────────────────────
    m_idLabel = makeValueLabel(this);
    m_idLabel->setText("Id");
    m_idGauge = new HGauge(0.0f, 70.0f, 60.0f, "", "",
        {{0, "0"}, {10, "10"}, {20, "20"}, {30, "30"}, {40, "40"}, {50, "50"}, {60, "60"}, {70, "70"}},
        this, 50.0f);
    m_idGauge->setAccessibleName(tr("Drain current"));
    auto* idRow = new QHBoxLayout;
    idRow->setContentsMargins(0, 0, 0, 0);
    idRow->setSpacing(4);
    idRow->addWidget(m_idLabel);
    idRow->addWidget(m_idGauge, 1);
    vbox->addLayout(idRow);

    // ── Port status strips ───────────────────────────────────────────────────
    // Between the meters and the controls, where the amplifier's own panel
    // puts them. Expanded presentation only.
    {
        m_portRowsBox = new QWidget;
        // Strips on the left, keys on the right — the keys span both strips
        // rather than sitting under them, which is what makes this one block
        // of the panel rather than two stacked rows of unequal weight.
        auto* portRow = new QHBoxLayout(m_portRowsBox);
        portRow->setContentsMargins(0, 0, 0, 0);
        portRow->setSpacing(4);

        auto* area = new QVBoxLayout;
        area->setContentsMargins(0, 0, 0, 0);
        area->setSpacing(0);

        m_portLiveBox = new QWidget(m_portRowsBox);
        auto* rows = new QVBoxLayout(m_portLiveBox);
        rows->setContentsMargins(0, 0, 0, 0);
        rows->setSpacing(2);
        m_portA = new AccessoryPortRow(QStringLiteral("A"), m_portLiveBox);
        m_portB = new AccessoryPortRow(QStringLiteral("B"), m_portLiveBox);
        for (auto* row : {m_portA, m_portB}) {
            // The amplifier reports no frequency per port. The tuner's strips
            // carry one; leaving an N/A standing here forever would claim a
            // reading is missing rather than that there is none to take.
            row->setFrequencyVisible(false);
            rows->addWidget(row);
        }
        area->addWidget(m_portLiveBox);

        m_standbyBanner = new QLabel(tr("STANDBY"), m_portRowsBox);
        m_standbyBanner->setAlignment(Qt::AlignCenter);
        m_standbyBanner->setVisible(false);
        m_standbyBanner->setAccessibleName(tr("Amplifier in standby"));
        area->addWidget(m_standbyBanner);

        portRow->addLayout(area, 1);
        // Centred in both axes: the layout then takes each key's own size hint
        // — which the scale sets — instead of stretching it to fill the cell.
        // The standby banner replaces the strips beside them, never the keys:
        // pressing STBY is how the amplifier comes back out of standby.
        portRow->addWidget(m_meffaKey, 0, Qt::AlignCenter);
        portRow->addWidget(m_fanKey, 0, Qt::AlignCenter);
        portRow->addWidget(m_stbyKey, 0, Qt::AlignCenter);

        // Fixed, not Maximum: Maximum still lets the layout shrink it, and
        // then a panel being dragged shorter squeezes the strips while the pad
        // below them is still metres deep. See applyDensityAtScale.
        m_portRowsBox->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        vbox->addWidget(m_portRowsBox);
    }

    // ── Control row: [temp / Vdd / Vac stacked] [fan] [operate] ─────────────
    //   Temp, drain voltage and mains voltage sit in the space to the left of
    //   the operate control, in both presentations.
    m_tempFahrenheit = readTempFahrenheit();

    m_tempBtn = new QPushButton(this);
    m_tempBtn->setObjectName(QStringLiteral("ampTempUnitButton"));
    m_tempBtn->setFlat(true);
    m_tempBtn->setFocusPolicy(Qt::TabFocus);
    m_tempBtn->setCursor(Qt::PointingHandCursor);
    m_tempBtn->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    m_tempBtn->setMinimumWidth(76);
    m_tempBtn->setAccessibleDescription("Toggles amplifier temperature between Celsius and Fahrenheit");
    connect(m_tempBtn, &QPushButton::clicked, this, [this]() {
        m_tempFahrenheit = !m_tempFahrenheit;
        writeTempFahrenheit(m_tempFahrenheit);
        updateTempLabel();
    });
    updateTempLabel();

    m_vddLabel = new QLabel(voltsReadout(QStringLiteral("Vdd"), QStringLiteral("—")), this);
    m_vacLabel = new QLabel(voltsReadout(QStringLiteral("Vac"), QStringLiteral("—")), this);
    m_sourceLabel = new QLabel("● RADIO", this);
    for (QLabel* readout : {m_vddLabel, m_vacLabel, m_sourceLabel}) {
        readout->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    }

    // One row along the bottom when the panel has the width for it, stacked
    // when it does not. A grid rather than two layouts, so neither reading is
    // ever reparented between presentations — see applyTelemetryLayout.
    m_telemetryBox = new QWidget;
    m_telemetryGrid = new QGridLayout(m_telemetryBox);
    m_telemetryGrid->setContentsMargins(0, 0, 0, 0);
    m_telemetryGrid->setVerticalSpacing(0);
    m_telemetryBox->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    applyTelemetryLayout();

    auto* btnRow = new QHBoxLayout;
    btnRow->setContentsMargins(0, 0, 0, 0);
    btnRow->setSpacing(6);
    // The readouts take the row's width rather than a stretch beside them,
    // so the grid has slack of its own to put between the last reading and
    // the source indicator at the far end. A stretch here instead would take
    // it all first and leave the grid at its contents' width.
    btnRow->addWidget(m_telemetryBox, 1);

    // Fan speed pull-down — surfaces all three modes instead of making the
    // operator click through them blind (#3905). Item text via
    // fanModeLabel(); uppercase mode stored as itemData so the
    // fanModeChanged contract ("uppercase, ready for sendCommand") is
    // unchanged. Hidden until a direct PGXL connection delivers the first
    // fanmode status.
    // GuardedComboBox (not plain QComboBox): this is a hardware control, so
    // an accidental mouse-wheel scroll while just hovering over it must not
    // silently change fan mode and send a command to the amp — it only
    // responds to wheel input when its dropdown is actually open (#3905).
    m_fanCombo = new GuardedComboBox;
    m_fanCombo->setObjectName(QStringLiteral("ampFanModeCombo"));
    for (const QString& mode : {QStringLiteral("STANDARD"), QStringLiteral("CONTEST"), QStringLiteral("BROADCAST")})
        m_fanCombo->addItem(fanModeLabel(mode), mode);
    // Let the widest item ("Fan: Contest") drive the combo's width instead
    // of leaving it pinned to whatever the stylesheet happens to compute
    // (#4731) — matches the pattern used by every other combo in the app,
    // e.g. ProfileSwitcherApplet, AdaptiveFilterControls.
    m_fanCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_fanCombo->setMinimumContentsLength(12);
    // Belt-and-braces: if a future label ever outgrows the combo anyway,
    // fail visibly (clipped) rather than silently mislabelling the mode.
    m_fanCombo->view()->setTextElideMode(Qt::ElideNone);
    m_fanCombo->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    m_fanCombo->setFocusPolicy(Qt::TabFocus);
    m_fanCombo->setToolTip("Fan Speed\nSelect STANDARD / CONTEST / BROADCAST");
    m_fanCombo->setAccessibleName(QString("Fan speed: %1").arg(m_fanMode));
    m_fanCombo->setAccessibleDescription("Selects STANDARD, CONTEST, or BROADCAST fan mode");
    m_fanCombo->hide();
    connect(m_fanCombo, &QComboBox::currentIndexChanged, this, [this](int index) {
        m_fanMode = m_fanCombo->itemData(index).toString();
        applyFanControls();
        emit fanModeChanged(m_fanMode);
    });
    m_meffaBtn = new QPushButton(QStringLiteral("MEffA"), this);
    m_meffaBtn->setObjectName(QStringLiteral("ampMeffaButton"));
    m_meffaBtn->setFocusPolicy(Qt::TabFocus);
    m_meffaBtn->setCursor(Qt::PointingHandCursor);
    m_meffaBtn->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    m_meffaBtn->hide();
    connect(m_meffaBtn, &QPushButton::clicked, this, [this]() {
        if (!m_meffaSettable) return;
        emit meffaToggled(m_meffaState == QLatin1String("OFF"));
    });
    btnRow->addWidget(m_meffaBtn);
    btnRow->addWidget(m_fanCombo);

    m_operateBtn = new QPushButton("OPERATE");
    m_operateBtn->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    AetherSDR::ThemeManager::instance().applyStyleSheet(m_operateBtn, kBtnStyle);
    m_operateBtn->hide();
    btnRow->addWidget(m_operateBtn);

    vbox->addLayout(btnRow);

    // One pad under the controls, absorbing whatever the scaling did not use.
    // kBottomGap is its floor rather than a margin on the layout so there is a
    // single thing deciding the space below the controls.
    m_bottomStretch = new QSpacerItem(0, kBottomGap,
                                      QSizePolicy::Minimum, QSizePolicy::Fixed);
    vbox->addSpacerItem(m_bottomStretch);

    outer->addWidget(body);

    // The alert overlay is a child of the applet rather than a row in its
    // layout, so it can cover the whole thing. Created last so it sits on top
    // of everything already added.
    m_alertOverlay = new QLabel(this);
    // The body of an M| frame, verbatim from the amplifier. QLabel's AutoText
    // would render anything markup-shaped as rich text — including a remote
    // <img>, which it would then fetch. Device input is not ours to trust
    // (Principle VII), and PlainText is the house answer.
    m_alertOverlay->setTextFormat(Qt::PlainText);
    m_alertOverlay->setAlignment(Qt::AlignCenter);
    m_alertOverlay->setWordWrap(true);
    m_alertOverlay->setVisible(false);
    m_alertOverlay->setAccessibleName(tr("Amplifier alert"));

    // Both operate controls do the same thing: command the state the
    // amplifier is not in. Routed through one lambda so the rail button and
    // the panel key can never disagree about which way the toggle goes.
    for (QPushButton* btn : {m_operateBtn, static_cast<QPushButton*>(m_stbyKey)}) {
        connect(btn, &QPushButton::clicked, this,
                [this]() { emit operateToggled(wantsOperate()); });
    }

    // Label text throttle — update PWR/SWR/Id text at 10 Hz so the digits
    // don't flicker on every incoming meter packet.  Gauge fill still
    // animates at full rate via HGauge's own timer.
    m_labelTimer.setInterval(kMeterReadoutUpdateMs);
    connect(&m_labelTimer, &QTimer::timeout, this, &AmpApplet::updateValueLabels);
    m_labelTimer.start();


    applyDensityAtScale(1.0);
    updatePortRows();
}

void AmpApplet::applyTelemetryLayout()
{
    if (!m_telemetryGrid) return;
    const bool inRow = m_floating;
    if (m_telemetryBox->layout() && m_telemetryInRow == inRow
            && m_telemetryGrid->count() > 0) {
        return;
    }
    m_telemetryInRow = inRow;

    QWidget* const cells[] = {m_tempBtn, m_vddLabel, m_vacLabel, m_sourceLabel};
    for (QWidget* cell : cells) {
        m_telemetryGrid->removeWidget(cell);
    }
    // Along the bottom on the panel, the way the amplifier's own front panel
    // runs them; down the side in the rail, which is one tile wide and has
    // nowhere to put four readings abreast.
    for (int i = 0; i < 3; ++i) {
        if (inRow) m_telemetryGrid->addWidget(cells[i], 0, i);
        else       m_telemetryGrid->addWidget(cells[i], i, 0);
    }
    // The source indicator is not a reading — it says which path the readings
    // came down. It goes to the far end of the row rather than trailing the
    // measurements, with the slack between, so it reads as a separate thing.
    if (inRow) {
        m_telemetryGrid->addWidget(cells[3], 0, 4, Qt::AlignRight | Qt::AlignVCenter);
        m_telemetryGrid->setColumnStretch(3, 1);
    } else {
        m_telemetryGrid->addWidget(cells[3], 3, 0);
        m_telemetryGrid->setColumnStretch(3, 0);
    }
}

void AmpApplet::buildExpandedUI()
{
    m_fanKey = new PanelKey(fanModeLetter(m_fanMode), this);
    AetherSDR::ThemeManager::instance().applyStyleSheet(m_fanKey, kPanelKeyIdleStyle);
    m_fanKey->hide();
    // Cycles through the pull-down's own items rather than keeping a second
    // list: the combo's currentIndexChanged is the one place that publishes a
    // mode change, so the key cannot command something the rail disagrees
    // with. Wheel input is not accepted — this is a hardware control, and a
    // scroll over a key the operator is only passing across must not change
    // fan speed (the same reason the pull-down is a GuardedComboBox, #3905).
    connect(m_fanKey, &QPushButton::clicked, this, [this]() {
        if (m_fanCombo->count() <= 0) return;
        m_fanCombo->setCurrentIndex((m_fanCombo->currentIndex() + 1) % m_fanCombo->count());
    });

    // "ME" rather than "MEffA": one key's width, and the tooltip carries the
    // full name and the state it is actually in.
    m_meffaKey = new PanelKey(QStringLiteral("ME"), this);
    AetherSDR::ThemeManager::instance().applyStyleSheet(m_meffaKey, kPanelKeyIdleStyle);
    m_meffaKey->hide();
    connect(m_meffaKey, &QPushButton::clicked, this, [this]() {
        if (!m_meffaSettable) return;
        emit meffaToggled(m_meffaState == QLatin1String("OFF"));
    });

    m_stbyKey = new PanelKey(tr("STBY"), this);
    m_stbyKey->setToolTip(tr("Toggle standby — the amplifier drops out of circuit"));
    m_stbyKey->setAccessibleName(tr("STBY"));
    m_stbyKey->setAccessibleDescription(m_stbyKey->toolTip());
    AetherSDR::ThemeManager::instance().applyStyleSheet(m_stbyKey, kPanelKeyIdleStyle);
    m_stbyKey->hide();

    // Seed size: the widest caption at the design font, measured now, while
    // the key still has its natural size hint.
    QFont seedFont = m_stbyKey->font();
    seedFont.setPixelSize(kKeyFontDesignPx);
    m_keySeedWidth = QFontMetrics(seedFont).horizontalAdvance(m_stbyKey->text())
                     + kKeyPaddingDesignPx;
}

// ── Model ───────────────────────────────────────────────────────────────────

void AmpApplet::setAmpModel(AmpModel* model)
{
    if (m_model == model) return;
    if (m_model) {
        // BOTH directions. disconnect(m_model, …, this, …) only drops
        // connections whose SENDER is the model; the command connections below
        // have the applet as sender and would survive a reattach, leaving one
        // press writing the setup group down two amplifiers' sockets.
        disconnect(m_model, nullptr, this, nullptr);
        disconnect(this, nullptr, m_model, nullptr);
    }
    m_model = model;
    if (!m_model) return;

    connect(m_model, &AmpModel::portsChanged, this, &AmpApplet::updatePortRows);
    connect(m_model, &AmpModel::antennaMapChanged, this, &AmpApplet::updateActivePort);
    connect(m_model, &AmpModel::ampStateChanged, this, &AmpApplet::setState);
    connect(m_model, &AmpModel::alertChanged, this, &AmpApplet::setAlertText);
    // MEffA rides the model rather than the wiring layer, like every other
    // amplifier-owned state on this panel. The model is also what a write has
    // to go through — a `setup` write carries the whole configuration group
    // and only the model holds the rest of it — so both directions live here
    // and the applet needs no help from MainWindow to operate the control.
    connect(m_model, &AmpModel::meffaChanged, this, [this](const QString& state) {
        setMeffa(state, m_model->canWriteSetup());
    });
    connect(this, &AmpApplet::meffaToggled, m_model, &AmpModel::setMeffaEnabled);
    connect(this, &AmpApplet::fanModeChanged, m_model, &AmpModel::setFanMode);
    // Seed from what the model already holds. The applet is attached after
    // the connection is wired, so anything that arrived in between — an
    // alert standing when the applet was built, the first state word — would
    // otherwise be missed until the device happened to send it again.
    if (!m_model->stateText().isEmpty()) setState(m_model->stateText());
    setAlertText(m_model->alert());
    setMeffa(m_model->meffa(), m_model->canWriteSetup());
    updatePortRows();
}

void AmpApplet::setTxAntenna(const QString& antenna)
{
    if (m_txAntenna == antenna) return;
    m_txAntenna = antenna;
    updateActivePort();
}

// ── Presentation ────────────────────────────────────────────────────────────

void AmpApplet::setFloating(bool floating)
{
    if (floating == m_floating) return;
    m_floating = floating;
    applyDensity();
    if (!m_floating || m_calibrationPasses > 0) return;

    // Calibrate on the next turn of the event loop, not here.
    //
    // Almost all of this column's height is text, and the text's size comes
    // from the style sheets applyDensity has just set — which Qt applies when
    // it next delivers events, not on the call. Measured now, every label still
    // carries the application's default font and the column reads far taller
    // than it will ever be: 285px against the 221 it settles at, a third too
    // much. That figure divides every later scale, so the panel starts
    // shrinking its contents while there is still an inch of empty space under
    // them — which is the opposite of the rule the bottom pad exists to keep.
    //
    // The measurement takes a few turns to settle; calibrateNaturalHeight
    // re-schedules itself until it does.
    QTimer::singleShot(0, this, [this]() {
        calibrateNaturalHeight();
        applyDensity();
    });
}

qreal AmpApplet::contentScale() const
{
    // Docked, the rail gives every tile the same width and a fixed height;
    // scaling there would make one tile disagree with its neighbours.
    if (!m_floating) return 1.0;
    const qreal naturalH = m_naturalContentHeight > 1.0 ? m_naturalContentHeight
                                                        : kDesignHeight;
    return panelContentScale(size(), kDesignWidth, naturalH);
}

void AmpApplet::applyDensity()
{
    // The write lives here, not in resizeEvent. m_appliedScale answers "what
    // scale are the children at", and resizeEvent is not the only caller that
    // changes it — setFloating and setDirectConnected apply a density too. Set
    // from the one place that does the applying, it cannot go stale.
    m_appliedScale = contentScale();
    applyDensityAtScale(m_appliedScale);
}

void AmpApplet::applyDensityAtScale(qreal scale)
{
    auto& theme = AetherSDR::ThemeManager::instance();
    const bool f = m_floating;
    const qreal s = scale;
    auto px = [s](int base) { return qMax(1, qRound(base * s)); };

    // No bottom margin: m_bottomStretch owns the space under the controls.
    m_vbox->setContentsMargins(f ? px(12) : 4, f ? px(10) : 2,
                               f ? px(12) : 4, f ? 0 : 2);
    m_vbox->setSpacing(f ? px(8) : 2);

    // Expanded fills the window it was given; docked stays the fixed-height
    // tile the rail stacks.
    setSizePolicy(QSizePolicy::Preferred,
                  f ? QSizePolicy::Preferred : QSizePolicy::Fixed);

    for (auto* lbl : {m_pwrLabel, m_drvLabel, m_swrLabel, m_idLabel}) {
        lbl->setFixedWidth(f ? px(96) : 72);
        theme.applyStyleSheet(lbl, QStringLiteral(
            "QLabel { color: {{color.text.primary}}; font-size: %1px; font-weight: bold; }")
            .arg(f ? px(14) : 11));
    }
    for (auto* gauge : {m_fwdGauge, m_drvGauge, m_swrGauge, m_idGauge}) {
        gauge->setFixedHeight(f ? px(34) : 24);
        // The bar grows with the panel; without this its tick lettering would
        // not, which is most of what "it just stretches" looks like.
        gauge->setMetricScale(f ? s : 1.0);
    }

    m_portA->setScale(f ? s : 1.0);
    m_portB->setScale(f ? s : 1.0);

    // The SWR bar carries its scale as a gradient across the empty track —
    // the one piece of the panel that is a colour, not a layout, so it is
    // resolved from the theme rather than copied off the hardware.
    QGradientStops swrStops;
    if (f) {
        const ThemeGradient swrScale =
            theme.gradient(this, QStringLiteral("color.accessory.swrScale"));
        for (const ThemeGradientStop& stop : swrScale.stops) {
            swrStops.append({stop.at, stop.color});
        }
    }
    m_swrGauge->setTrackGradient(swrStops);

    applyMeffaControls();
    applyTelemetryLayout();
    m_telemetryGrid->setHorizontalSpacing(f ? px(14) : 0);
    applyTelemetryStyles(s);
    applyAlertStyle();

    theme.applyStyleSheet(m_standbyBanner, QStringLiteral(
        "QLabel { border: 2px solid {{color.accessory.key.standby.foreground}}; "
        "border-radius: 3px; background: {{color.accessory.key.standby.background}}; "
        "color: {{color.accessory.key.standby.foreground}}; "
        "letter-spacing: 2px; font-size: %1px; font-weight: bold; }")
        .arg(px(20)));
    // The banner stands in for both strips, so it claims their combined height
    // — otherwise the panel jumps every time the amplifier enters standby.
    m_standbyBanner->setMinimumHeight(m_portA->sizeHint().height() * 2 + 2);

    // Expanding only when popped out: docked, the rail already fixes the
    // tile's height and there is no slack for a pad to take.
    //
    // This is the ONLY item in the column that may change height. Everything
    // above it is vertically Fixed, so the layout has exactly one place to put
    // spare height and one place to take it from: dragging the panel shorter
    // drains the pad to kBottomGap before anything else moves. Sizing the
    // contents is the scale's job, not the layout's.
    m_bottomStretch->changeSize(0, kBottomGap, QSizePolicy::Minimum,
                                f ? QSizePolicy::Expanding : QSizePolicy::Fixed);

    m_portRowsBox->setVisible(f);

    QFont keyFont = m_stbyKey->font();
    keyFont.setPixelSize(px(kKeyFontDesignPx));
    m_stbyKey->setFont(keyFont);
    // One glyph in a square box carries a larger face than a four-letter
    // caption in a letterbox one, or it floats in the middle of the key.
    QFont fanFont = m_fanKey->font();
    fanFont.setPixelSize(px(kFanKeyFontDesignPx));
    m_fanKey->setFont(fanFont);
    // Two glyphs, so it takes the caption face rather than the fan key's
    // single-glyph one.
    QFont meffaFont = m_meffaKey->font();
    meffaFont.setPixelSize(px(kKeyFontDesignPx));
    m_meffaKey->setFont(meffaFont);
    applyKeySize(f ? s : 1.0);

    // Only one of each pair is ever up, and neither is shown before the
    // amplifier has reported a state — a control that cannot say what it is
    // set to is worse than none.
    applyStateToControls();
    applyFanControls();

    m_vbox->invalidate();
}

void AmpApplet::calibrateNaturalHeight()
{
    // What the column costs at scale 1.0 — the figure every later scale is a
    // multiple of.
    //
    // Always measured with the scale forced to 1.0 first. That is the rule
    // that matters: re-deriving it from a SCALED layout feeds the scale back
    // into its own input, and it does not settle — rounding and the widgets'
    // own minimums stop the contents being exactly proportional, the leftover
    // lands in the divisor, and the next scale reads larger every time.
    //
    // Re-running it at scale 1.0, by contrast, is just a better measurement of
    // the same thing, and it takes more than one turn to get: almost all of
    // this column is text, and the text's size arrives from style sheets Qt
    // applies over the following turns of the event loop. Measured on the
    // first turn the column reads 285px, on the second 243, and it settles at
    // 221 — a third too much at the start, and that figure divides every later
    // scale, so the panel shrinks its contents while there is still an inch of
    // empty space under them. So it re-measures until the figure stops moving,
    // and then stops for good.
    if (!m_vbox || m_calibrationPasses >= kMaxCalibrationPasses) return;
    applyDensityAtScale(1.0);
    m_vbox->activate();
    const qreal content = m_vbox->sizeHint().height() - kBottomGap;
    if (content <= 1.0) return;
    ++m_calibrationPasses;
    m_naturalContentHeight = content;
    if (m_calibrationPasses >= kMaxCalibrationPasses) return;
    QTimer::singleShot(0, this, [this]() {
        calibrateNaturalHeight();
        applyDensity();
    });
}

QSize AmpApplet::minimumSizeHint() const
{
    if (!m_floating) return QWidget::minimumSizeHint();
    // The layout's own minimum may briefly exceed this and the contents
    // overflow for that one pass; the resize that caused it then lowers the
    // scale and they fit again.
    const qreal natural = m_naturalContentHeight > 1.0 ? m_naturalContentHeight
                                                       : kDesignHeight;
    return panelMinimumSize(kDesignWidth, natural);
}

void AmpApplet::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    layOutAlertOverlay();

    // Re-derive the metrics for the new size, but only when the scale has
    // actually moved: a resize arrives for every pixel of a window drag and
    // applyDensity re-applies a dozen style sheets.
    const qreal s = contentScale();
    if (!qFuzzyCompare(s, m_appliedScale)) {
        applyDensity();
    }
}

void AmpApplet::applyKeySize(qreal scale)
{
    if (!m_stbyKey || m_keySeedWidth <= 0) return;
    const int h = qMax(1, qRound(kKeyDesignHeight * scale));
    m_stbyKey->setTargetSize(panelKeySize(h, m_keySeedWidth, scale));
    // 1:1. The letterbox width exists to hold a word; this key holds a letter,
    // and a square reads as the toggle it is rather than as a second STBY.
    m_fanKey->setTargetSize(QSize(h, h));
    // "ME" is a caption, not a glyph, so it takes the letterbox the standby
    // key takes — the two read as peers, which is what they are.
    m_meffaKey->setTargetSize(panelKeySize(h, m_keySeedWidth, scale));
}

void AmpApplet::applyTelemetryStyles(qreal scale)
{
    auto& theme = AetherSDR::ThemeManager::instance();
    const bool f = m_floating;
    auto px = [scale](int base) { return qMax(1, qRound(base * scale)); };
    const int bodyPx = f ? px(13) : 10;

    theme.applyStyleSheet(m_tempBtn, QStringLiteral(
        "QPushButton { background: transparent; border: 1px solid transparent; "
        "color: {{color.text.primary}}; font-family: monospace; font-size: %1px; "
        "text-align: left; padding: 0 2px; }"
        "QPushButton:hover { border-color: {{color.background.2}}; color: {{color.text.primary}}; }"
        "QPushButton:focus { border-color: {{color.accent.bright}}; }").arg(bodyPx));

    // Vdd and Vac are not proxied by the radio. Without the direct connection
    // they have no value to show, so they read as unavailable rather than as
    // a reading that happens to be dashes.
    const QString tone = m_directConnected ? QStringLiteral("{{color.text.primary}}")
                                           : QStringLiteral("{{color.text.disabled}}");
    for (auto* lbl : {m_vddLabel, m_vacLabel}) {
        theme.applyStyleSheet(lbl, QStringLiteral(
            "QLabel { color: %1; font-family: monospace; font-size: %2px; }")
            .arg(tone).arg(bodyPx));
    }

    theme.applyStyleSheet(m_sourceLabel, QStringLiteral(
        "QLabel { color: %1; font-size: %2px; }")
        .arg(m_directConnected ? QStringLiteral("{{color.accent.bright}}")
                               : QStringLiteral("{{color.text.label}}"))
        .arg(f ? px(11) : 9));

    theme.applyStyleSheet(m_fanCombo, QStringLiteral(
        "QComboBox { background: {{color.background.2}}; border: 1px solid {{color.background.2}}; "
        "border-radius: 3px; padding: 1px 4px; color: {{color.text.primary}}; "
        "font-size: %1px; font-weight: bold; }"
        "QComboBox:hover { background: {{color.background.1}}; }"
        "QComboBox::drop-down { border: none; width: %2px; }"
        // The popup view is a separate top-level (Qt::Popup) window, so it
        // doesn't inherit the combo's font — it must be set explicitly here or
        // the popup paints at the app's default UI font while the combo's own
        // width (and elision) is computed from the rule above. On a
        // larger-than-default UI font that mismatch elides "Fan: Contest", the
        // longest item, into a garbled label (#4731).
        "QComboBox QAbstractItemView { background: {{color.background.2}}; color: {{color.text.primary}}; "
        "selection-background-color: {{color.background.1}}; font-size: %1px; font-weight: bold; }")
        .arg(f ? px(11) : 10).arg(f ? px(14) : 14));
}

// ── Alerts ──────────────────────────────────────────────────────────────────

void AmpApplet::layOutAlertOverlay()
{
    if (!m_alertOverlay) return;
    m_alertOverlay->setGeometry(rect());
}

void AmpApplet::applyAlertStyle()
{
    if (!m_alertOverlay) return;
    // Unlike the tuner, which announces a successful tune on the same
    // channel, every amplifier message seen so far is something to act on —
    // and the protocol carries no severity field to tell them apart. So they
    // all land on the attention colour, which is the safe way round: a
    // warning shown as good news is worse than the reverse.
    //
    // Opaque ground: the overlay has to obscure the readings under it, not
    // tint them. The applet's own darkest ground rather than a literal black,
    // so it still reads correctly under a light theme.
    AetherSDR::ThemeManager::instance().applyStyleSheet(m_alertOverlay, QStringLiteral(
        "QLabel { background: {{color.background.0}}; color: {{color.accent.danger}}; "
        "border: none; padding: 8px; font-size: %1px; font-weight: bold; }")
        .arg(m_floating ? 26 : 15));
}

void AmpApplet::setAlertText(const QString& text)
{
    const QString shown = text.trimmed();
    if (m_alertOverlay->text() == shown) return;
    m_alertOverlay->setText(shown);
    applyAlertStyle();

    // The amplifier raises the alert and later clears it with an empty frame;
    // the overlay simply follows those two, so its dwell time is whatever the
    // device chose.
    if (shown.isEmpty()) {
        m_alertOverlay->hide();
        return;
    }
    layOutAlertOverlay();
    m_alertOverlay->raise();
    m_alertOverlay->show();

    // Announce it: the banner appears and disappears on the amplifier's
    // schedule, and a reader that is not looking at it would otherwise never
    // learn the amplifier had faulted.
    m_alertOverlay->setAccessibleDescription(shown);
    if (QAccessible::isActive()) {
        QAccessibleEvent event(m_alertOverlay, QAccessible::Alert);
        QAccessible::updateAccessibility(&event);
    }
}

// ── Port strips ─────────────────────────────────────────────────────────────

void AmpApplet::updatePortRows()
{
    if (!m_portA || !m_portB) return;

    // Standby takes the whole area: out of circuit, there is no per-port
    // reading left to show.
    m_portLiveBox->setVisible(!m_standby);
    m_standbyBanner->setVisible(m_standby);

    if (m_model && m_model->hasPortInfo()) {
        applyPortInfo(m_portA, m_model->portA());
        applyPortInfo(m_portB, m_model->portB());
    } else {
        // The radio-relayed "amplifier" object carries no per-port block at
        // all, so without the direct connection there is nothing to put in
        // these cells. They stay empty rather than repeating the one radio
        // this client happens to be connected to onto both ports. Which port
        // is keyed is still knowable — it is in the state word.
        for (auto* row : {m_portA, m_portB}) {
            row->setBandText(QString());
            row->setBiasText(QString());
            row->setSourceText(QString());
        }
        m_portA->setPtt(m_stateWord == QLatin1String("TRANSMIT_A"));
        m_portB->setPtt(m_stateWord == QLatin1String("TRANSMIT_B"));
    }

    const QString cell = stateCell(m_stateWord);
    m_portA->setStateText(cell);
    m_portB->setStateText(cell);
    updateActivePort();
}

void AmpApplet::applyPortInfo(AccessoryPortRow* row, const AmpPortInfo& info)
{
    // The band is the live reading — the amplifier reports bandX=0 for a port
    // nothing is driving, which the strip shows as N/A. The source radio and
    // the bias profile are configuration: they describe how the port is set
    // up whether or not RF is going through it, so they are shown either way
    // rather than blanked to imply the port does not exist.
    row->setBandText(info.band);
    row->setBiasText(info.bias);
    row->setSourceText(info.source);
    row->setPtt(info.ptt);
}

void AmpApplet::updateActivePort()
{
    if (!m_portA || !m_portB) return;

    // Which port carries transmit comes from the amplifier's own antenna →
    // output map read with the transmit slice's antenna — exactly FlexLib's
    // Amplifier.OutputConfiguredForAntenna. The state word answers it only
    // once RF is already flowing (TRANSMIT_A / TRANSMIT_B), which is too late
    // to be what tells the operator where it is about to go.
    //
    // No match means the radio is transmitting on an antenna that does not
    // run through the amplifier at all. Outlining nothing is the honest
    // answer; outlining a port would claim RF is passing through it.
    const QString out = m_model ? m_model->outputForAntenna(m_txAntenna) : QString();
    m_portA->setActive(out.compare(QLatin1String("PORTA"), Qt::CaseInsensitive) == 0);
    m_portB->setActive(out.compare(QLatin1String("PORTB"), Qt::CaseInsensitive) == 0);
}

// ── Telemetry ───────────────────────────────────────────────────────────────

void AmpApplet::setRadioMeters(float watts, float swr, bool powerValid)
{
    // A relay update that carried no forward power is NOT the relay being the
    // live meter source. The radio's amplifier meters arrive as one signal for
    // FWD, RL, TEMP and DRV alike, so a temperature update would otherwise
    // stamp the relay fresh at 0 W and hold the amplifier's own socket off for
    // as long as the radio kept reporting temperature — which is precisely the
    // station where the socket is the only source of power and SWR (#4805).
    if (!powerValid) return;
    m_radioMeters.restart();
    applyMeters(watts, swr);
}

void AmpApplet::setDeviceMeters(float watts, float swr)
{
    // Dropped on the floor while the relay is live, rather than applied and
    // then overwritten: two sources writing the same gauge at different rates
    // is what made the bar jitter between two slightly different numbers.
    if (m_radioMeters.isValid()
            && m_radioMeters.elapsed() < kRelayMeterFreshnessMs) {
        return;
    }
    applyMeters(watts, swr);
}

void AmpApplet::applyMeters(float watts, float swr)
{
    // Power BEFORE SWR, and neither value written to its cache first.
    // setFwdPower reads the PREVIOUS m_fwdWatts to spot the crossing in and
    // out of "there is power flowing", which is what clears the SWR bar at
    // idle and restores it when power resumes; assigning m_fwdWatts here
    // would make that comparison read the new value against itself and the
    // crossing would never be seen. setSwr then reads the power that
    // setFwdPower has just stored.
    setFwdPower(watts);
    setSwr(swr);
}

void AmpApplet::setDrivePower(float watts, bool valid)
{
    m_haveDrive = valid;
    m_drvWatts = valid ? watts : 0.0f;
    m_drvGauge->setValue(m_drvWatts);
    // The whole ROW goes, not just the number. A bar parked at the left stop
    // is what "no drive" looks like, and an amplifier that publishes no DRV
    // meter is not being driven with nothing — it is not telling us. Leaving
    // an empty gauge on screen would assert the first and mean the second.
    m_drvLabel->setVisible(valid);
    m_drvGauge->setVisible(valid);
    updateDriveLabel();
}

void AmpApplet::updateDriveLabel()
{
    if (!m_drvLabel) return;
    // The meter floors at its declared low bound, 10 dBm = 0.01 W, so a
    // reading an order of magnitude above that is real drive rather than the
    // floor. Below it the row keeps its name and drops the number, the way
    // PWR/SWR/Id do.
    if (m_haveDrive && m_drvWatts >= 0.1f) {
        m_drvLabel->setText(QStringLiteral("DRV  %1").arg(m_drvWatts, 0, 'f', 1));
    } else {
        m_drvLabel->setText(QStringLiteral("DRV"));
    }
}

void AmpApplet::setFwdPower(float watts)
{
    const bool wasPowered = (m_fwdWatts >= 5.0f);
    m_fwdWatts = watts;
    m_fwdGauge->setValue(watts);
    const bool isPowered = (watts >= 5.0f);
    if (!isPowered && wasPowered)
        m_swrGauge->setValue(1.0f);   // clear bar — SWR is unmeasurable at idle
    else if (isPowered && !wasPowered)
        m_swrGauge->setValue(m_swrVal); // power resumed — restore cached value
    // Peak marker: HGauge's sliding window, fed by setValue above (canon).
    // Label text is updated by the 100 ms timer (updateValueLabels).
}

void AmpApplet::setSwr(float swr)
{
    m_swrVal = swr;
    // Only drive the gauge when there is forward power — SWR is not meaningful
    // at idle and the radio/PGXL may report stale or noise values.
    if (m_fwdWatts >= 5.0f)
        m_swrGauge->setValue(swr);
    // Label text is updated by the 100 ms timer (updateValueLabels).
}

void AmpApplet::setTemp(float degC)
{
    m_tempA = degC;
    m_hasTempA = true;
    updateTempLabel();
}

void AmpApplet::setTempB(float degC)
{
    m_tempB = degC;
    m_hasTempB = true;
    updateTempLabel();
}

void AmpApplet::updateTempLabel()
{
    if (!m_tempBtn) {
        return;
    }

    const QString tempA = m_hasTempA
        ? formatTemp(m_tempA, m_tempFahrenheit)
        : QStringLiteral("—");
    const QString unit = m_tempFahrenheit
        ? QStringLiteral("F")
        : QStringLiteral("C");

    // Both sensors are named. The amplifier's own panel runs them unlabelled
    // as "24.4/24.2 C", which is fine on hardware where the operator knows
    // which is which and nothing else on screen is a temperature; here two
    // bare numbers say nothing about what either one is measuring.
    //
    // HL is the wire's own name for the second (`hltemp`). PA is not — the
    // first arrives as a bare `temp`, and PA is what an unqualified
    // temperature on a power amplifier is. A one-line change if 4O3A ever
    // says otherwise.
    if (m_hasTempB) {
        m_tempBtn->setText(
            QStringLiteral("PA %1 / HL %2 %3")
                .arg(pad(tempA))
                .arg(pad(formatTemp(m_tempB, m_tempFahrenheit)))
                .arg(unit));
    } else {
        m_tempBtn->setText(QStringLiteral("PA %1 %2").arg(pad(tempA)).arg(unit));
    }

    const QString nextUnit = m_tempFahrenheit
        ? tr("Celsius")
        : tr("Fahrenheit");
    m_tempBtn->setToolTip(
        tr("Amplifier temperature\nClick to show degrees %1").arg(nextUnit));
    m_tempBtn->setAccessibleName(
        tr("Amplifier temperature %1").arg(m_tempBtn->text()));
    if (QAccessible::isActive()) {
        QAccessibleEvent event(m_tempBtn, QAccessible::NameChanged);
        QAccessible::updateAccessibility(&event);
    }
}

void AmpApplet::setDrainCurrent(float amps)
{
    m_drainAmps = amps;
    m_idGauge->setValue(amps);
    // Label text is updated by the 100 ms timer (updateValueLabels).
}

void AmpApplet::updateValueLabels()
{
    updateDriveLabel();

    // PWR: only show value when there is meaningful power
    if (m_fwdWatts >= 5.0f)
        m_pwrLabel->setText(QStringLiteral("PWR  %1").arg(static_cast<int>(m_fwdWatts)));
    else
        m_pwrLabel->setText("PWR");

    // SWR: only show value when power is present (SWR is unmeasurable at idle)
    if (m_fwdWatts >= 5.0f)
        m_swrLabel->setText(QStringLiteral("SWR  %1:1").arg(m_swrVal, 0, 'f', 1));
    else
        m_swrLabel->setText("SWR");

    // Id: only show value when current is flowing
    if (m_drainAmps >= 0.5f)
        m_idLabel->setText(QStringLiteral("Id    %1").arg(static_cast<int>(m_drainAmps)));
    else
        m_idLabel->setText("Id");
}

void AmpApplet::setDrainVoltage(float volts)
{
    if (!m_directConnected) return;
    // Reported as it arrives, including zero.
    //
    // A PGXL keeps its drain rail down while it is idle and only brings it up
    // on entering OPERATE, so vdd=0.0 is the normal reading for most of the
    // time the amplifier is switched on — not a fault, and not a missing
    // value. This used to print a dash below 1 V, which reads as "nothing
    // arrived": the operator sees an empty field on connect, toggles standby
    // to make the reading appear, and concludes the client dropped the first
    // frames. Zero volts is a true reading and says the rail is down; the
    // dash is kept for the one case where we genuinely have nothing, which is
    // no direct connection at all (see setDirectConnected).
    m_vddLabel->setText(voltsReadout(QStringLiteral("Vdd"),
                                     QString::number(volts, 'f', 1)));
}

void AmpApplet::setMainsVoltage(int volts)
{
    if (!m_directConnected) return;
    m_mainsVolts = volts;
    m_vacLabel->setText(voltsReadout(QStringLiteral("Vac"), QString::number(volts)));
}

void AmpApplet::setFanMode(const QString& mode)
{
    const QString upper = mode.toUpper();
    int idx = m_fanCombo->findData(upper);
    if (idx < 0) {
        // Leave m_fanMode and the combo's selection as they were — updating
        // one but not the other would desync what's displayed from what
        // AmpApplet thinks the mode is.
        // And do NOT reveal the control: it would then assert a mode the
        // amplifier never confirmed, which is the one thing m_haveFanMode
        // exists to prevent.
        qWarning() << "AmpApplet: unknown fanmode" << upper;
    } else {
        m_fanMode = upper;
        // Reflecting an incoming PGXL status — block signals so this
        // doesn't fire currentIndexChanged and echo a redundant
        // "setup fanmode=" command back to the amp (#3905).
        QSignalBlocker blocker(m_fanCombo);
        m_fanCombo->setCurrentIndex(idx);
        m_haveFanMode = true;
    }
    applyFanControls();
}

void AmpApplet::applyFanControls()
{
    m_fanCombo->setAccessibleName(QString("Fan speed: %1").arg(m_fanMode));

    m_fanKey->setText(fanModeLetter(m_fanMode));
    m_fanKey->setToolTip(tr("Fan speed: %1\nClick to cycle standard, contest, broadcast")
                             .arg(m_fanMode));
    m_fanKey->setAccessibleName(QString("Fan speed: %1").arg(m_fanMode));
    m_fanKey->setAccessibleDescription(
        tr("Cycles standard, contest and broadcast fan speed. Currently %1.")
            .arg(m_fanMode));
    if (QAccessible::isActive()) {
        QAccessibleEvent event(m_fanKey, QAccessible::NameChanged);
        QAccessible::updateAccessibility(&event);
    }

    // Exactly one of the two is up, and it is the one that belongs to the
    // presentation. Neither before the amplifier has reported a mode.
    m_fanCombo->setVisible(m_haveFanMode && !m_floating);
    m_fanKey->setVisible(m_haveFanMode && m_floating);

}

void AmpApplet::setMeffa(const QString& state, bool settable)
{
    const QString word = state.trimmed().toUpper();
    if (m_meffaState == word && m_meffaSettable == settable) return;
    m_meffaState = word;
    m_meffaSettable = settable;
    applyMeffaControls();
}

void AmpApplet::applyMeffaControls()
{
    if (!m_meffaBtn || !m_meffaKey) return;
    auto& theme = AetherSDR::ThemeManager::instance();

    // Three states, one operator-controlled bit. OFF is disabled; ACTIVE is
    // enabled and optimising; STANDBY is enabled but inapplicable because the
    // PA is in class AAB, which is where SSB and AM put it. Presenting STANDBY
    // as "off" would tell an operator their setting had not taken, and
    // presenting it as "on" would claim the algorithm is doing something.
    const bool off     = (m_meffaState == QLatin1String("OFF"));
    const bool standby = (m_meffaState == QLatin1String("STANDBY"));
    const bool active  = (m_meffaState == QLatin1String("ACTIVE"));

    const QString caption =
        off     ? tr("MEffA off")
        : standby ? tr("MEffA on — idle in class AAB")
        : active  ? tr("MEffA on — optimising")
                  : tr("MEffA");
    const QString description =
        off     ? tr("Maximum Efficiency Algorithm is disabled. Activates it.")
        : standby ? tr("Maximum Efficiency Algorithm is enabled but does not "
                       "apply in class AAB, which SSB and AM use. Disables it.")
        : active  ? tr("Maximum Efficiency Algorithm is optimising the "
                       "amplifier. Disables it.")
                  : tr("Maximum Efficiency Algorithm state is not known yet.");

    const char* style = active  ? kPanelKeyMeffaActiveStyle
                      : standby ? kPanelKeyMeffaStandbyStyle
                                : kPanelKeyIdleStyle;
    theme.applyStyleSheet(m_meffaKey, style);
    theme.applyStyleSheet(m_meffaBtn, style);

    for (QWidget* w : {static_cast<QWidget*>(m_meffaKey),
                       static_cast<QWidget*>(m_meffaBtn)}) {
        w->setToolTip(caption);
        w->setAccessibleName(caption);
        w->setAccessibleDescription(description);
        // Shown but inert until the whole `setup` group is known: a write has
        // to send four other values back, and pressing a control that cannot
        // complete is worse than one that visibly cannot be pressed yet.
        w->setEnabled(m_meffaSettable);
    }
    // Nothing before the amplifier has reported a state, on the same rule the
    // fan controls follow: a control that cannot say what it is set to is
    // worse than none.
    const bool known = !m_meffaState.isEmpty();
    m_meffaBtn->setVisible(known && !m_floating);
    m_meffaKey->setVisible(known && m_floating);

    // Announce on whichever of the pair is actually up. MEffA is the one
    // control here whose STATE lives in its accessible name — the visible text
    // stays "MEffA" in all three — so announcing the hidden one tells a screen
    // reader nothing, and the docked rail button is the one showing by
    // default. (#4896)
    if (QAccessible::isActive()) {
        QWidget* shown = m_floating ? static_cast<QWidget*>(m_meffaKey)
                                    : static_cast<QWidget*>(m_meffaBtn);
        QAccessibleEvent event(shown, QAccessible::NameChanged);
        QAccessible::updateAccessibility(&event);
    }
}

void AmpApplet::setState(const QString& state)
{
    // Standby is the state STANDBY, and nothing else — the mapping FlexLib
    // makes (Amplifier.Operate is State != Standby).
    //
    // Deliberately NOT "anything that is not IDLE/OPERATE/TRANSMIT". Read
    // that way a FAULT is standby, and the panel puts up a STANDBY banner
    // over the port strips — telling the operator the amplifier is out of
    // circuit because someone chose that, at the moment it has tripped, and
    // hiding the cell that would have said FAULT behind the banner.
    const QString word = state.trimmed().toUpper();
    if (word == m_stateWord) return;
    m_stateWord = word;
    m_standby = (m_stateWord == QLatin1String("STANDBY"));
    m_operating = (m_stateWord == QLatin1String("IDLE")
                   || m_stateWord == QLatin1String("OPERATE")
                   || m_stateWord.startsWith(QLatin1String("TRANSMIT")));
    applyStateToControls();
    updatePortRows();
}

bool AmpApplet::wantsOperate() const
{
    // What a press asks for. Not simply "not standby": POWERUP and SELFCHECK
    // are the amplifier coming up toward operate, so a press there asks for
    // operate, which is what it did before the panel existed. A fault is the
    // exception — the amplifier has tripped, and the useful thing to command
    // is standby, not to insist it operate.
    return !m_operating && m_stateWord != QLatin1String("FAULT");
}

void AmpApplet::applyStateToControls()
{
    auto& theme = AetherSDR::ThemeManager::instance();
    const bool known = !m_stateWord.isEmpty();

    // The rail button names the state the amplifier is IN, the way it always
    // has. The panel key names the state it SELECTS and lights when the
    // amplifier is in it — a key that renamed itself would leave the operator
    // reading the label to work out which way it moves.
    //
    // Three states are neither of the two the button toggles between, and the
    // one reading that must not happen is any of them shown as OPERATE in the
    // operating colour. POWERUP and SELFCHECK are the amplifier on its way up;
    // FAULT is it having tripped. Each says so.
    const bool faulted = (m_stateWord == QLatin1String("FAULT"));
    m_operateBtn->setText(faulted ? tr("FAULT")
                          : m_stateWord == QLatin1String("POWERUP") ? tr("PWRUP")
                          : m_stateWord == QLatin1String("SELFCHECK") ? tr("CHECK")
                          : m_operating ? tr("OPERATE")
                                        : tr("STANDBY"));
    theme.applyStyleSheet(m_operateBtn, faulted     ? kFaultStyle
                                        : m_operating ? kOperateStyle
                                                      : kBtnStyle);
    theme.applyStyleSheet(m_stbyKey, m_standby ? kPanelKeyStandbyStyle
                                               : kPanelKeyIdleStyle);
    m_stbyKey->setAccessibleDescription(
        wantsOperate() ? tr("Amplifier is not operating. Activates operate.")
                       : tr("Amplifier is operating. Activates standby."));

    m_operateBtn->setVisible(known && !m_floating);
    m_stbyKey->setVisible(known && m_floating);
}

void AmpApplet::setDirectConnected(bool direct)
{
    m_directConnected = direct;
    m_sourceLabel->setText(direct ? QStringLiteral("● DIRECT")
                                  : QStringLiteral("● RADIO"));
    if (!direct) {
        // Vdd and Vac are not proxied by the radio — clear the stale values.
        m_vddLabel->setText(voltsReadout(QStringLiteral("Vdd"), QStringLiteral("—")));
        m_vacLabel->setText(voltsReadout(QStringLiteral("Vac"), QStringLiteral("—")));
        // Fan mode is only available via the direct PGXL protocol — drop it
        // until the amplifier is back rather than leaving a control up that
        // can no longer command anything.
        m_haveFanMode = false;
        applyFanControls();
    }
    // contentScale(), not m_appliedScale: that member is only resizeEvent's
    // cache key for "has the scale moved", and nothing else writes it — so
    // between popping the applet out and the first resize that actually moves
    // the scale it still reads 1.0, and a connect or disconnect in that window
    // would style the readouts at a size nothing else on the panel is at.
    applyTelemetryStyles(contentScale());
    updatePortRows();
}

void AmpApplet::setMeff(const QString& meff)
{
    // The RELAYED MEffA state, off the radio's amplifier telemetry rather than
    // the port-9008 socket. On a station with no direct socket this is the
    // only place the state appears at all, so it reads out here — but it can
    // never be WRITTEN from here: a `setup` write carries the whole
    // configuration group and only the direct connection can read the rest of
    // it. Hence settable=false; the control shows the state and stays inert.
    //
    // The socket path (AmpModel::meffaChanged) calls setMeffa directly with
    // the real writability, and arrives on a connected station before this
    // does, so it wins where both exist.
    if (m_meffaSettable) return;   // the socket owns it; do not downgrade
    setMeffa(meff, false);
}

} // namespace AetherSDR
