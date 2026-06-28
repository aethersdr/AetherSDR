#ifdef HAVE_HIDAPI
#include "RC28MappingDialog.h"

#include "core/AppSettings.h"
#include "core/HidEncoderManager.h"
#include "GuardedSlider.h"   // GuardedComboBox lives here

#include <QCheckBox>
#include <QComboBox>
#include <QSlider>
#include <QDateTime>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QVBoxLayout>

namespace AetherSDR {

// ── Action tables ────────────────────────────────────────────────────────────
// Keep in sync with the if/else chain in MainWindow::dispatchHidAction.
// kHoldActions: actions that latch on/off per hold press (TuneFast, FineTune,
// ToggleRit, etc.) or fire once at the 600 ms threshold (ModeCycle).
struct ActionEntry { const char* id; const char* label; };

static const ActionEntry kActions[] = {
    {"None",        "No Action"},
    {"ToggleAgc",   "AGC"},
    {"ToggleApf",   "APF"},
    {"BandCycle",   "Band"},
    {"BandZoom",    "Band Zoom"},
    {"ModeCycle",   "Mode"},
    {"NextSlice",   "Next Slice"},
    {"PrevSlice",   "Previous Slice"},
    {"SegmentZoom", "Segment Zoom"},
    {"Snap100Hz",   "Snap to Nearest 100 Hz"},
    {"Snap500Hz",   "Snap to Nearest 500 Hz"},
    {"SnapKHz",     "Snap to Nearest 1 kHz"},
    {"Snap100kHz",  "Snap to Nearest 100 kHz"},
    {"Snap500kHz",  "Snap to Nearest 500 kHz"},
    {"StepUp",      "Step Up"},
    {"StepDown",    "Step Down"},
    {"StepCycle",   "Step (legacy)"},
    {"VolumeDown",  "Volume Down (-5)"},
    {"VolumeUp",    "Volume Up (+5)"},
};

static const ActionEntry kHoldActions[] = {
    {"None",        "No Action"},
    {"FineTune",    "Fine Tuning"},
    {"TuneFast",    "Fast Tuning"},
    {"ModeCycle",   "Mode"},
    {"ToggleMute",  "Mute"},
    {"ToggleRit",   "RIT"},
    {"ToggleLock",  "Slice Lock"},
    {"ToggleXit",   "XIT"},
};

static constexpr const char* kGroupStyle =
    "QGroupBox {"
    "  color: #c8d8e8;"
    "  border: 1px solid #203040;"
    "  border-radius: 4px;"
    "  margin-top: 8px;"
    "  padding-top: 6px;"
    "  font-size: 11px;"
    "}"
    "QGroupBox::title {"
    "  subcontrol-origin: margin;"
    "  left: 8px;"
    "  padding: 0 4px;"
    "}";

static constexpr const char* kLabelStyle = "color: #c8d8e8; font-size: 11px;";
static constexpr const char* kDimStyle   = "color: #6080a0; font-size: 11px;";
static constexpr const char* kComboStyle =
    "QComboBox {"
    "  background: #101828; color: #c8d8e8;"
    "  border: 1px solid #203040; border-radius: 3px;"
    "  padding: 2px 6px; font-size: 11px;"
    "}"
    "QComboBox::drop-down { border: none; }"
    "QComboBox QAbstractItemView { background: #101828; color: #c8d8e8; }";

// ── Constructor ───────────────────────────────────────────────────────────────

RC28MappingDialog::RC28MappingDialog(HidEncoderManager* encoder,
                                     QWidget* parent)
    : PersistentDialog("Icom RC-28 Button Mapping", "RC28MappingDialogGeometry", parent)
    , m_encoder(encoder)
{
    setMinimumWidth(420);

    auto* root = new QVBoxLayout(bodyWidget());
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(8);

    buildDeviceSection();
    buildTxBarSection();
    buildAssignSection();
    buildEncoderSection();
    buildLogSection();

    auto* closeBtn = new QPushButton("Close");
    closeBtn->setAutoDefault(false);
    closeBtn->setStyleSheet(
        "QPushButton { background:#203040; color:#c8d8e8; border:1px solid #304050;"
        "  border-radius:3px; padding:4px 16px; font-size:11px; }"
        "QPushButton:hover { background:#2a3f55; }");
    auto* btnRow = new QHBoxLayout;
    btnRow->addStretch();
    btnRow->addWidget(closeBtn);
    root->addLayout(btnRow);

    connect(closeBtn, &QPushButton::clicked, this, &QDialog::close);

    if (m_encoder) {
        connect(m_encoder, &HidEncoderManager::connectionChanged,
                this, &RC28MappingDialog::onConnectionChanged);
        connect(m_encoder, &HidEncoderManager::multipleDevicesDetected,
                this, &RC28MappingDialog::onMultipleDevicesDetected);
        refreshDeviceInfo();
    }
}

// ── Section builders ──────────────────────────────────────────────────────────

void RC28MappingDialog::buildDeviceSection()
{
    auto* group = new QGroupBox("Device");
    group->setStyleSheet(kGroupStyle);

    auto* form = new QFormLayout(group);
    form->setSpacing(4);
    form->setContentsMargins(8, 6, 8, 8);

    auto* statusRow = new QHBoxLayout;
    m_statusDot   = new QLabel("●");
    m_statusDot->setStyleSheet("color: #4060a0; font-size: 14px;");
    m_statusLabel = new QLabel("Not connected");
    m_statusLabel->setStyleSheet(kDimStyle);
    statusRow->addWidget(m_statusDot);
    statusRow->addWidget(m_statusLabel);
    statusRow->addStretch();
    form->addRow(statusRow);

    m_vidPidLabel  = new QLabel("–");
    m_pathLabel    = new QLabel("–");
    m_serialLabel  = new QLabel("–");
    m_firmwareLabel = new QLabel("–");
    for (QLabel* l : {m_vidPidLabel, m_pathLabel, m_serialLabel, m_firmwareLabel})
        l->setStyleSheet(kLabelStyle);

    auto* vidPidKey  = new QLabel("VID:PID");
    auto* pathKey    = new QLabel("Path");
    auto* serialKey  = new QLabel("Serial");
    auto* firmwareKey = new QLabel("Firmware");
    for (QLabel* l : {vidPidKey, pathKey, serialKey, firmwareKey})
        l->setStyleSheet(kDimStyle);

    form->addRow(vidPidKey,   m_vidPidLabel);
    form->addRow(pathKey,     m_pathLabel);
    form->addRow(serialKey,   m_serialLabel);
    form->addRow(firmwareKey, m_firmwareLabel);

    static_cast<QVBoxLayout*>(bodyWidget()->layout())->addWidget(group);
}

void RC28MappingDialog::buildTxBarSection()
{
    auto* group = new QGroupBox("Transmit (PTT) Button");
    group->setStyleSheet(kGroupStyle);
    auto* vbox = new QVBoxLayout(group);
    vbox->setSpacing(4);
    vbox->setContentsMargins(8, 6, 8, 8);

    m_momentaryBtn = new QRadioButton("Momentary  —  hold to transmit, release to stop");
    m_latchedBtn   = new QRadioButton("Latched  —  press to start transmitting, press again to stop");
    m_momentaryBtn->setStyleSheet(kLabelStyle);
    m_latchedBtn->setStyleSheet(kLabelStyle);

    const bool latched =
        HidEncoderManager::rc28MappingField("pttMode", "Momentary") == "Latched";
    m_momentaryBtn->setChecked(!latched);
    m_latchedBtn->setChecked(latched);

    vbox->addWidget(m_momentaryBtn);
    vbox->addWidget(m_latchedBtn);

    auto save = [this] {
        HidEncoderManager::setRc28MappingField(
            "pttMode",
            m_latchedBtn->isChecked() ? QStringLiteral("Latched")
                                      : QStringLiteral("Momentary"));
    };
    connect(m_momentaryBtn, &QRadioButton::toggled, this, save);
    connect(m_latchedBtn,   &QRadioButton::toggled, this, save);

    static_cast<QVBoxLayout*>(bodyWidget()->layout())->addWidget(group);
}

void RC28MappingDialog::buildAssignSection()
{
    auto* group = new QGroupBox("Button Assignments");
    group->setStyleSheet(kGroupStyle);
    auto* form = new QFormLayout(group);
    form->setSpacing(5);
    form->setContentsMargins(8, 6, 8, 8);

    m_f1PressCombo = new GuardedComboBox;
    m_f1HoldCombo  = new GuardedComboBox;
    m_f2PressCombo = new GuardedComboBox;
    m_f2HoldCombo  = new GuardedComboBox;

    for (QComboBox* c : {m_f1PressCombo, m_f1HoldCombo, m_f2PressCombo, m_f2HoldCombo})
        c->setStyleSheet(kComboStyle);

    populateActionCombo(m_f1PressCombo, "f1Press", "StepUp");
    populateActionCombo(m_f1HoldCombo,  "f1Hold",  "TuneFast", true);
    populateActionCombo(m_f2PressCombo, "f2Press", "StepDown");
    populateActionCombo(m_f2HoldCombo,  "f2Hold",  "ModeCycle", true);

    auto makeLabel = [](const char* text) {
        auto* l = new QLabel(text);
        l->setStyleSheet("color:#c8d8e8; font-size:11px;");
        return l;
    };
    form->addRow(makeLabel("F1  —  short press:"), m_f1PressCombo);
    form->addRow(makeLabel("F1  —  long press:"), m_f1HoldCombo);
    form->addRow(makeLabel("F2  —  short press:"), m_f2PressCombo);
    form->addRow(makeLabel("F2  —  long press:"), m_f2HoldCombo);

    static_cast<QVBoxLayout*>(bodyWidget()->layout())->addWidget(group);
}

void RC28MappingDialog::buildEncoderSection()
{
    auto* group = new QGroupBox("Encoder");
    group->setStyleSheet(kGroupStyle);
    auto* vbox = new QVBoxLayout(group);
    vbox->setSpacing(4);
    vbox->setContentsMargins(8, 6, 8, 8);

    auto* invertCheck = new QCheckBox("Invert encoder direction");
    invertCheck->setStyleSheet(kLabelStyle);
    invertCheck->setChecked(
        AppSettings::instance().value("HidEncoderInvertDir", "False").toString() == "True");
    connect(invertCheck, &QCheckBox::toggled, this, [this](bool on) {
        AppSettings::instance().setValue("HidEncoderInvertDir", on ? "True" : "False");
        AppSettings::instance().save();
        if (m_encoder)
            QMetaObject::invokeMethod(m_encoder.data(), [enc = m_encoder.data(), on] {
                enc->setInvertDirection(on);
            });
    });
    vbox->addWidget(invertCheck);

    // Sensitivity divider: how many encoder pulses are required to produce one
    // frequency step.  1 = normal (every pulse fires), 10 = most reduced. (#3841)
    auto* sensRow = new QHBoxLayout;
    auto* sensKey = new QLabel("Pulses per step:");
    sensKey->setStyleSheet(kDimStyle);
    m_sensitivitySlider = new QSlider(Qt::Horizontal);
    m_sensitivitySlider->setRange(1, 10);
    m_sensitivitySlider->setTickPosition(QSlider::TicksBelow);
    m_sensitivitySlider->setTickInterval(1);
    const int savedSens = HidEncoderManager::rc28MappingField("sensitivity", "1").toInt();
    m_sensitivitySlider->setValue(savedSens >= 1 && savedSens <= 10 ? savedSens : 1);
    m_sensitivityValueLabel = new QLabel(QString::number(m_sensitivitySlider->value()));
    m_sensitivityValueLabel->setStyleSheet(kLabelStyle);
    m_sensitivityValueLabel->setFixedWidth(18);
    m_sensitivityValueLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    sensRow->addWidget(sensKey);
    sensRow->addWidget(m_sensitivitySlider);
    sensRow->addWidget(m_sensitivityValueLabel);
    connect(m_sensitivitySlider, &QSlider::valueChanged, this, [this](int v) {
        m_sensitivityValueLabel->setText(QString::number(v));
        HidEncoderManager::setRc28MappingField("sensitivity", QString::number(v));
        emit mappingFieldChanged("sensitivity", QString::number(v));
    });
    vbox->addLayout(sensRow);

    // Auto-snap: after the knob stops for 600 ms, snap to the nearest 1 kHz
    // without recentering the spectrum display. (#3841)
    m_autoSnapCheck = new QCheckBox("Auto-snap to nearest 1 kHz after rotation stops");
    m_autoSnapCheck->setStyleSheet(kLabelStyle);
    m_autoSnapCheck->setChecked(
        HidEncoderManager::rc28MappingField("autoSnap", "False") == "True");
    connect(m_autoSnapCheck, &QCheckBox::toggled, this, [this](bool on) {
        HidEncoderManager::setRc28MappingField("autoSnap", on ? "True" : "False");
        emit mappingFieldChanged("autoSnap", on ? "True" : "False");
    });
    vbox->addWidget(m_autoSnapCheck);

    static_cast<QVBoxLayout*>(bodyWidget()->layout())->addWidget(group);
}

void RC28MappingDialog::buildLogSection()
{
    auto* group = new QGroupBox("Activity");
    group->setStyleSheet(kGroupStyle);
    auto* vbox = new QVBoxLayout(group);
    vbox->setContentsMargins(8, 6, 8, 8);
    vbox->setSpacing(4);

    m_log = new QPlainTextEdit;
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(50);
    m_log->setFixedHeight(100);
    m_log->setStyleSheet(
        "QPlainTextEdit {"
        "  background:#080f18; color:#6080a0;"
        "  border:1px solid #203040; border-radius:3px;"
        "  font-family: monospace; font-size:10px;"
        "}");
    m_log->setPlaceholderText("Button events appear here while the dialog is open…");
    vbox->addWidget(m_log);

    auto* clearBtn = new QPushButton("Clear");
    clearBtn->setAutoDefault(false);
    clearBtn->setFixedWidth(60);
    clearBtn->setStyleSheet(
        "QPushButton { background:#203040; color:#c8d8e8; border:1px solid #304050;"
        "  border-radius:3px; padding:2px 8px; font-size:10px; }"
        "QPushButton:hover { background:#2a3f55; }");
    connect(clearBtn, &QPushButton::clicked, m_log, &QPlainTextEdit::clear);

    auto* row = new QHBoxLayout;
    row->addStretch();
    row->addWidget(clearBtn);
    vbox->addLayout(row);

    static_cast<QVBoxLayout*>(bodyWidget()->layout())->addWidget(group);
}

// ── Helpers ───────────────────────────────────────────────────────────────────

void RC28MappingDialog::populateActionCombo(QComboBox* combo,
                                            const QString& field,
                                            const QString& defaultId,
                                            bool holdOnly)
{
    const ActionEntry* table = holdOnly ? kHoldActions : kActions;
    const int n = holdOnly ? static_cast<int>(sizeof(kHoldActions) / sizeof(kHoldActions[0]))
                           : static_cast<int>(sizeof(kActions) / sizeof(kActions[0]));
    for (int i = 0; i < n; ++i)
        combo->addItem(QString::fromUtf8(table[i].label), QString::fromLatin1(table[i].id));

    const QString saved = HidEncoderManager::rc28MappingField(field, defaultId);
    const int idx = combo->findData(saved);
    combo->setCurrentIndex(idx >= 0 ? idx : 0);

    connect(combo, &QComboBox::currentIndexChanged, this,
            [this, combo, field](int) {
        const QString val = combo->currentData().toString();
        HidEncoderManager::setRc28MappingField(field, val);
        emit mappingFieldChanged(field, val);
    });
}

void RC28MappingDialog::refreshDeviceInfo()
{
    if (!m_encoder) return;

    const bool connected = m_encoder->isOpen();

    if (!connected && m_encoder->isBlockedByMultiple()) {
        // Guard against the narrow race where the ext-ctrl thread has already
        // cleared m_blockedDeviceName but hasn't yet stored the false flag —
        // an empty name would produce garbled dialog text. Fall through to the
        // "not connected" state; the queued connectionChanged signal corrects it.
        const QString blocked = m_encoder->blockedDeviceName();
        if (!blocked.isEmpty()) {
            onMultipleDevicesDetected(blocked);
            return;
        }
    }

    m_statusDot->setStyleSheet(
        connected ? "color:#00c070; font-size:14px;"
                  : "color:#4060a0; font-size:14px;");
    if (connected)
        m_knownDeviceName = m_encoder->deviceName();
    m_statusLabel->setText(connected ? m_knownDeviceName : "Not connected");
    m_statusLabel->setStyleSheet(
        connected ? "color:#c8d8e8; font-size:11px;" : kDimStyle);

    if (connected) {
        m_vidPidLabel->setText(
            QString("0x%1 : 0x%2")
                .arg(m_encoder->vendorId(),  4, 16, QChar('0'))
                .arg(m_encoder->productId(), 4, 16, QChar('0')));
        m_pathLabel->setText(
            m_encoder->devicePath().isEmpty() ? "–" : m_encoder->devicePath());
        m_serialLabel->setText(
            m_encoder->serialNumber().isEmpty() ? "(none)" : m_encoder->serialNumber());
        const uint16_t rel = m_encoder->releaseNumber();
        m_firmwareLabel->setText(
            rel ? QString("%1.%2%3")
                      .arg((rel >> 8) & 0xFF)
                      .arg((rel >> 4) & 0x0F)
                      .arg(rel & 0x0F)
                : "–");
    } else {
        for (QLabel* l : {m_vidPidLabel, m_pathLabel, m_serialLabel, m_firmwareLabel})
            l->setText("–");
    }
}

// ── Slots ─────────────────────────────────────────────────────────────────────

void RC28MappingDialog::onConnectionChanged(bool connected,
                                            const QString& deviceName)
{
    if (connected && !deviceName.isEmpty())
        m_knownDeviceName = deviceName;
    refreshDeviceInfo();
}

void RC28MappingDialog::onMultipleDevicesDetected(const QString& deviceName)
{
    m_statusDot->setStyleSheet("color:#ffaa00; font-size:14px;");
    m_statusLabel->setText(
        QString("Multiple %1s detected on this system. "
                "Only one encoder is supported at a time.").arg(deviceName));
    m_statusLabel->setStyleSheet("color:#ffaa00; font-size:11px;");
    for (QLabel* l : {m_vidPidLabel, m_pathLabel, m_serialLabel, m_firmwareLabel})
        l->setText("–");
}

void RC28MappingDialog::appendButtonEvent(const QString& slotLabel,
                                          const QString& actionName)
{
    if (!m_log) return;
    const QString ts = QDateTime::currentDateTime().toString("HH:mm:ss");
    m_log->appendPlainText(
        QString("[%1]  %2  →  %3").arg(ts, slotLabel, actionName));
}

} // namespace AetherSDR
#endif // HAVE_HIDAPI
