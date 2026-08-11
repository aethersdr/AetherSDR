#include "GreenHeronApplet.h"

#include "core/GreenHeronProtocol.h"
#include "core/PeripheralSettings.h"
#include "core/ThemeManager.h"
#include "models/GreenHeronModel.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

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

// Matches the other accessory applets' tile chrome.
constexpr const char* kButtonBase =
    "QPushButton { background: #1a2a3a; border: 1px solid #203040; "
    "border-radius: 3px; padding: 3px 4px; font-size: 10px; color: #c8d8e8; "
    "text-align: left; }"
    "QPushButton:hover { background: #243848; }"
    "QPushButton:disabled { color: #55606c; background: #141d26; "
    "font-style: italic; }";

const QString kSelectedActive =
    "QPushButton:checked { background-color: #006040; color: #00ff88; "
    "border: 1px solid #00a060; font-weight: bold; }";

constexpr const char* kLabelStyle =
    "color: #8090a0; font-size: 10px; font-weight: bold;";

const QString kFieldStyle =
    QStringLiteral("background: {{color.background.1}}; "
                   "border: 1px solid {{color.background.1}}; border-radius: 3px; "
                   "padding: 2px 4px; color: {{color.text.primary}}; font-size: 10px;");

} // namespace

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
        hostLabel->setStyleSheet(kLabelStyle);
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
        portLabel->setStyleSheet(kLabelStyle);
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
        switchLabel->setStyleSheet(kLabelStyle);
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
        m_connectBtn->setStyleSheet(
            QString(kButtonBase)
            + "QPushButton { font-size: 10px; font-weight: bold; "
              "text-align: center; }");
        connect(m_connectBtn, &QPushButton::clicked,
                this, &GreenHeronApplet::toggleConnection);
        row->addWidget(m_connectBtn);

        outer->addLayout(row);
    }

    m_statusLabel = new QLabel(tr("Not connected"));
    m_statusLabel->setObjectName(QStringLiteral("greenHeronStatus"));
    m_statusLabel->setAlignment(Qt::AlignCenter);
    m_statusLabel->setStyleSheet("color: #606878; font-size: 10px;");
    outer->addWidget(m_statusLabel);

    // ── The chosen switch's antenna ports ───────────────────────────────────
    m_portHost = new QWidget;
    m_portLayout = new QVBoxLayout(m_portHost);
    m_portLayout->setContentsMargins(0, 0, 0, 0);
    m_portLayout->setSpacing(2);
    outer->addWidget(m_portHost);

    m_noteLabel = new QLabel;
    m_noteLabel->setObjectName(QStringLiteral("greenHeronNote"));
    m_noteLabel->setWordWrap(true);
    m_noteLabel->setStyleSheet("color: #c77800; font-size: 10px;");
    m_noteLabel->hide();
    outer->addWidget(m_noteLabel);
}

QString GreenHeronApplet::selectedSwitch() const
{
    return m_wantedSwitch;
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
    if (names.isEmpty() && !m_wantedSwitch.isEmpty()) {
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

    // Re-select the remembered switch if the device still has it; otherwise
    // fall back to the first one rather than showing an empty list next to a
    // connected server.
    const int index = names.indexOf(m_wantedSwitch);
    if (index >= 0) {
        m_switchCombo->setCurrentIndex(index);
    } else if (!names.isEmpty()) {
        m_switchCombo->setCurrentIndex(0);
        m_wantedSwitch = names.first();
        PeripheralSettings::setDeviceString(kSettingsDevice, kSettingsSwitch,
                                            m_wantedSwitch);
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
        button->setStyleSheet(QString(kButtonBase) + kSelectedActive);
        button->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        button->setMinimumWidth(0);
        button->setAccessibleName(port);
        connect(button, &QPushButton::clicked, this, [this, switchName, port]() {
            if (m_model->selectPort(switchName, port)) {
                note(tr("Sent %1 → %2").arg(switchName, port));
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

    const QString switchName = m_wantedSwitch;
    const GreenHeronSwitchState state = m_model->switchState(switchName);

    if (switchName != m_builtForSwitch || state.ports != m_builtForPorts) {
        m_builtForSwitch = switchName;
        m_builtForPorts = state.ports;
        rebuildPortList();
    }

    // Stale means the link dropped and what is on screen is the last thing the
    // device said. Leave it visible — it is still the best information
    // available — but refuse input, since nothing would reach the relays.
    m_portHost->setEnabled(m_model->isConnected());

    const QMap<QString, QString> held = m_model->locksBySwitch(switchName);
    for (const QString& port : state.ports) {
        QPushButton* button = m_portButtons.value(port);
        if (button == nullptr) {
            continue;
        }
        const QSignalBlocker blocker(button);
        const QString holder = held.value(port);

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
    }
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
    QString colour = QStringLiteral("#606878");
    if (m_model->isConnected()) {
        const GreenHeronSwitchState state = m_model->switchState(m_wantedSwitch);
        if (state.ports.isEmpty()) {
            text = m_wantedSwitch.isEmpty()
                       ? tr("Connected — waiting for roster")
                       : tr("Connected — %1 not reported").arg(m_wantedSwitch);
            colour = QStringLiteral("#c77800");
        } else {
            text = state.selected.isEmpty()
                       ? tr("%1 connected").arg(m_wantedSwitch)
                       : tr("%1 on %2").arg(m_wantedSwitch, state.selected);
            colour = QStringLiteral("#2e7d32");
        }
    } else if (m_model->isStale()) {
        text = tr("Reconnecting — state is stale");
        colour = QStringLiteral("#c77800");
    } else if (wanted) {
        text = tr("Connecting…");
        colour = QStringLiteral("#c77800");
    } else {
        text = tr("Not connected");
    }

    m_statusLabel->setText(text);
    m_statusLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 10px;").arg(colour));
    m_statusLabel->setAccessibleName(tr("Green Heron connection status"));
    m_statusLabel->setAccessibleDescription(text);
}

void GreenHeronApplet::note(const QString& text)
{
    m_noteLabel->setText(text);
    m_noteLabel->setAccessibleDescription(text);
    m_noteLabel->setVisible(!text.isEmpty());
    m_noteTimer->start();
}

} // namespace AetherSDR
