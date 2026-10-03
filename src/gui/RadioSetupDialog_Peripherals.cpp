// Radio Setup > Peripherals: a device list on the left, one detail page per
// device on the right. Each kind of device has its own builder, and each
// builder lays out its own page; nothing is created into a hidden grid and
// moved afterwards.
//
// What a device shows (its list line and its status label) is derived from its
// PeripheralDeviceStatus and from the connection's own state. No text is read
// back to decide anything.
//
// Presentation refreshes are driven by the connection and model signals that
// change it (see wirePresentationSignals in buildPeripheralsTab), by edits to
// the dialog's own fields and by the page being shown. There is no polling.

#include "RadioSetupDialog.h"
#include "RadioSetupDialogCommon.h"
#include "PeripheralAuthConnectFlow.h"
#include "PeripheralAuthStore.h"
#include "PeripheralConnectionSource.h"
#include "ScopedChildWidget.h"
#include "core/AcomConnection.h"
#include "core/AppSettings.h"
#include "core/LpMeterConnection.h"
#include "core/PeripheralRemovalGuard.h"
#include "core/PeripheralSettings.h"
#include "core/PgxlConnection.h"
#include "core/SpeConnection.h"
#include "core/TgxlConnection.h"
#include "core/ThemeManager.h"
#include "core/VkampConnection.h"
#include "models/AmpModel.h"
#include "models/AntennaGeniusModel.h"
#include "models/RadioModel.h"
#include "models/TunerModel.h"

#include <QAccessible>
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QEvent>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHash>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPair>
#include <QPointer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStackedWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

namespace AetherSDR {

using PeripheralConnectionSource::Source;
using Attention = PeripheralDeviceStatus::Attention;

// Everything Setup knows about one peripheral: who it is, which widgets are its
// own, how to ask its connection questions, and what it currently shows.
struct PeripheralDeviceUi {
    enum class Kind {
        AuthNetwork,      // TGXL, PGXL, Antenna Genius, ShackSwitch: host + port, maybe a code
        SerialOrNetwork,  // ACOM, SPE Expert, LP-100A: serial port or ser2net host
        Vkamp,            // host + port, plus the amplifier model
    };

    // Identity
    QString id;
    QString label;        // list and page title
    QString shortName;    // used in sentences: "TGXL", "Antenna Genius"
    Kind kind{Kind::AuthNetwork};
    int defaultPort{0};
    bool radioRelay{false};        // TGXL and PGXL can also be reached through the radio
    bool isShackSwitch{false};     // shares the Antenna Genius model
    bool sharedAgModel{false};     // Antenna Genius and ShackSwitch

    // Persistence: flat keys for the network devices, a PeripheralSettings
    // object for the rest.
    QString ipKey;
    QString portKey;
    QString settingsGroup;

    // Credentials (AuthNetwork devices only)
    bool hasAuthCode{false};
    bool endpointScopedCredential{false};
    PeripheralAuthStore::Device authDevice{PeripheralAuthStore::Device::Tgxl};

    // Connection state and actions, supplied by buildPeripheralsTab.
    std::function<bool()> isConnected;
    std::function<bool()> isConnecting;       // optional
    std::function<bool()> hasRadioRelay;      // optional
    std::function<void()> disconnectNow;
    std::function<void(const QString&, quint16)> connectTo;       // flat-key devices
    std::function<void(const QString&)> applyCode;
    std::function<void()> clearCode;
    std::function<QString()> peerAddress;
    std::function<quint16()> peerPort;
    std::function<QString()> liveEndpoint;    // empty unless connected to this device
    std::function<QString()> discoveredHost;  // radio-reported address, if any
    int discoveredPort{0};
    std::function<void()> afterRemoval;       // device-specific teardown after Remove
    std::function<void()> openWebUi;          // ShackSwitch only
    std::function<void(const QString&, quint16)> connectNetwork;  // serial-or-network devices
    std::function<void(const QString&)> connectSerial;
    std::function<QString()> description;

    // Serial-or-network presentation
    QString fixedBaud;
    QString networkPlaceholder;
    QString networkDescription;
    QString networkTooltip;

    // Widgets this device owns (null where it has none)
    QWidget* page{nullptr};
    QLabel* statusLabel{nullptr};
    QLineEdit* addressEdit{nullptr};
    QSpinBox* portSpin{nullptr};
    QLineEdit* networkHostEdit{nullptr};   // serial-or-network devices
    QStackedWidget* addressStack{nullptr};
    QStackedWidget* portStack{nullptr};
    QComboBox* modeCombo{nullptr};
    QPushButton* connectButton{nullptr};
    QPushButton* webUiButton{nullptr};
    QPushButton* serialRefreshButton{nullptr};
    QLineEdit* codeEdit{nullptr};
    QPushButton* showButton{nullptr};
    QPushButton* clearButton{nullptr};
    QComboBox* modelCombo{nullptr};
    QCheckBox* autoConnectCheck{nullptr};
    QLabel* editHint{nullptr};
    QHash<QWidget*, QPair<QString, QString>> fieldHelp;

    PeripheralDeviceStatus state;

    // The address as the operator sees it, whichever widget holds it.
    QString addressText() const
    {
        if (addressEdit) {
            return addressEdit->text().trimmed();
        }
        if (!addressStack) {
            return {};
        }
        QWidget* current = addressStack->currentWidget();
        if (QComboBox* serial = current->findChild<QComboBox*>()) {
            QString text = serial->currentData().toString();
            if (text == QStringLiteral("__custom__")) {
                if (QLineEdit* custom = current->findChild<QLineEdit*>(
                        QString(), Qt::FindDirectChildrenOnly)) {
                    text = custom->text().trimmed();
                }
            }
            return text;
        }
        if (QLineEdit* host = current->findChild<QLineEdit*>()) {
            return host->text().trimmed();
        }
        return {};
    }
};

namespace {

const QString kButtonStyle =
    "QPushButton { background: {{color.background.1}}; "
    "border: 1px solid {{color.background.2}}; border-radius: 3px; "
    "color: {{color.text.primary}}; font-size: 11px; font-weight: bold; "
    "padding: 3px 10px; }"
    "QPushButton:hover { background: {{color.background.2}}; }";
const QString kComboStyle =
    "QComboBox { background: {{color.background.1}}; border: 1px solid {{color.background.2}}; "
    "border-radius: 3px; color: {{color.text.primary}}; font-size: 12px; padding: 2px 4px; }"
    "QComboBox::drop-down { border: none; }";
const QString kSpinStyle =
    "QSpinBox { background: {{color.background.1}}; border: 1px solid {{color.background.2}}; "
    "border-radius: 3px; color: {{color.text.primary}}; font-size: 12px; padding: 2px 4px; }";
const QString kGroupBoxStyle =
    "QGroupBox { border: 1px solid {{color.background.2}}; border-radius: 4px; "
    "margin-top: 8px; padding-top: 12px; color: {{color.text.secondary}}; "
    "font-weight: bold; } QGroupBox::title { subcontrol-origin: margin; "
    "left: 10px; padding: 0 4px; }";

QString peripheralTr(const char* text)
{
    return RadioSetupDialog::tr(text);
}

// ── What a device shows ────────────────────────────────────────────────────

bool effectivelyConnecting(const PeripheralDeviceUi& ui)
{
    return ui.state.connecting || (ui.isConnecting && ui.isConnecting());
}

Source sourceOf(const PeripheralDeviceUi& ui)
{
    if (ui.isConnected()) {
        return Source::Direct;
    }
    if (ui.radioRelay && ui.hasRadioRelay && ui.hasRadioRelay()) {
        return Source::Radio;
    }
    return Source::Offline;
}

// The status label's text: the attention message when there is one, otherwise
// the connection state with its qualifiers.
QString statusText(const PeripheralDeviceUi& ui)
{
    const PeripheralDeviceStatus& state = ui.state;
    if (state.needsAttention()) {
        return state.message;
    }
    QString text;
    if (effectivelyConnecting(ui)) {
        text = peripheralTr("Connecting…");
    } else if (state.source == Source::Direct) {
        text = peripheralTr("Connected");
    } else if (state.source == Source::Radio) {
        text = peripheralTr("Not connected directly; using the radio relay");
    } else {
        text = peripheralTr("Not connected");
    }
    if (state.discardedAuthCode) {
        text += peripheralTr(" — entered code was discarded before verification; enter it again");
    }
    if (state.codeNotSaved) {
        text += QStringLiteral(" — ")
            + peripheralTr("device did not request authentication; code not saved");
    }
    if (!state.note.isEmpty()) {
        text += QStringLiteral(" — ") + state.note;
    }
    return text;
}

QString statusAccessibleName(const PeripheralDeviceUi& ui)
{
    if (ui.state.needsAttention()) {
        return RadioSetupDialog::tr("%1 needs attention").arg(ui.label);
    }
    if (effectivelyConnecting(ui)) {
        return RadioSetupDialog::tr("%1 connecting").arg(ui.label);
    }
    return PeripheralConnectionSource::describe(
        ui.state.source, ui.shortName, !ui.radioRelay).accessibleName;
}

const char* statusColorToken(const PeripheralDeviceUi& ui)
{
    switch (ui.state.attention) {
    case Attention::InvalidCode:
    case Attention::AuthBlocked:
    case Attention::ConnectError:
    case Attention::CredentialError:
        return "{{color.accent.danger}}";
    case Attention::CredentialWarning:
    case Attention::RemovalPending:
        return "{{color.accent.warning}}";
    case Attention::None:
        break;
    }
    if (!ui.state.note.isEmpty() || ui.state.discardedAuthCode || ui.state.codeNotSaved) {
        return "{{color.accent.warning}}";
    }
    return ui.state.source == Source::Direct ? "{{color.accent.success}}"
                                             : "{{color.text.secondary}}";
}

// Name and description follow the visible state, and assistive technology is
// told when they change (the pattern updateDStarSliceStateLabel uses).
void applyStatusLabel(PeripheralDeviceUi& ui)
{
    QLabel* label = ui.statusLabel;
    if (!label) {
        return;
    }
    const QString text = statusText(ui);
    const QString name = statusAccessibleName(ui);
    const QString color = QString::fromLatin1(statusColorToken(ui));
    const bool textChanged = label->text() != text;
    const bool nameChanged = label->accessibleName() != name;
    const bool descriptionChanged = label->accessibleDescription() != text;
    if (label->property("peripheralStatusColor").toString() != color) {
        label->setProperty("peripheralStatusColor", color);
        ThemeManager::instance().applyStyleSheet(label,
            QStringLiteral("QLabel { color: %1; font-size: 11px; }").arg(color));
    }
    if (!textChanged && !nameChanged && !descriptionChanged) {
        return;
    }
    label->setText(text);
    label->setAccessibleName(name);
    label->setAccessibleDescription(text);
    if (nameChanged) {
        QAccessibleEvent event(label, QAccessible::NameChanged);
        QAccessible::updateAccessibility(&event);
    }
    if (descriptionChanged) {
        QAccessibleEvent event(label, QAccessible::DescriptionChanged);
        QAccessible::updateAccessibility(&event);
    }
}

// A connection came up or went away. A recovered connection retires the error
// from its prior attempt; a lost one keeps it (the failure that preceded the
// disconnect is the reason the operator needs to see).
void linkChanged(PeripheralDeviceUi& ui)
{
    const bool connected = ui.isConnected();
    ui.state.connecting = false;
    ui.state.codeNotSaved = false;
    if (connected) {
        ui.state.attention = Attention::None;
        ui.state.message.clear();
        if (ui.state.pendingAuthCode) {
            // The device never asked for the code, so it was not saved.
            ui.state.pendingAuthCode = false;
            ui.state.discardedAuthCode = false;
            ui.state.codeNotSaved = true;
        }
    } else {
        ui.state.note.clear();
    }
}

void connectionFailed(PeripheralDeviceUi& ui, const QString& error, bool authBlocked)
{
    ui.state.connecting = false;
    ui.state.attention = Attention::ConnectError;
    ui.state.message = peripheralTr("Connection failed: %1").arg(error);
    if (authBlocked) {
        ui.state.pendingAuthCode = false;
    }
}

void codeAccepted(PeripheralDeviceUi& ui)
{
    ui.state.pendingAuthCode = false;
    ui.state.discardedAuthCode = false;
    ui.state.note = PeripheralAuthStore::persistentStoreAvailable()
        ? QString() : peripheralTr("code for this session only");
}

bool codeDiscarded(PeripheralDeviceUi& ui)
{
    if (!ui.state.pendingAuthCode) {
        return false;
    }
    ui.state.pendingAuthCode = false;
    ui.state.discardedAuthCode = true;
    return true;
}

// ── Detail page layout ─────────────────────────────────────────────────────

struct DetailPage {
    QWidget* page{nullptr};
    QVBoxLayout* layout{nullptr};
    QGroupBox* group{nullptr};
    QGridLayout* form{nullptr};
    int row{0};
};

DetailPage beginDetailPage(PeripheralDeviceUi& ui, QWidget* stackParent)
{
    DetailPage detail;
    detail.page = new QWidget(stackParent);
    detail.page->setObjectName(QStringLiteral("peripheralDetail_%1").arg(ui.id));
    ThemeManager::instance().applyStyleSheet(detail.page,
        QStringLiteral("QWidget#%1 { background: {{color.background.0}}; }")
            .arg(detail.page->objectName()));
    detail.layout = new QVBoxLayout(detail.page);
    detail.layout->setContentsMargins(8, 12, 0, 0);
    detail.layout->setSpacing(10);

    auto* heading = new QHBoxLayout;
    auto* title = new QLabel(ui.label, detail.page);
    ThemeManager::instance().applyStyleSheet(title,
        "QLabel { color: {{color.text.primary}}; font-size: 15px; font-weight: bold; }");
    heading->addWidget(title);
    heading->addStretch();
    detail.layout->addLayout(heading);

    // Always shown: it is the device's state for everyone, including a screen
    // reader, not only a place for errors.
    ui.statusLabel = new QLabel(detail.page);
    ui.statusLabel->setObjectName(QStringLiteral("peripheralStatus_%1").arg(ui.id));
    ui.statusLabel->setWordWrap(true);
    detail.layout->addWidget(ui.statusLabel);

    detail.group = new QGroupBox(RadioSetupDialog::tr("Connection Settings"), detail.page);
    ThemeManager::instance().applyStyleSheet(detail.group, kGroupBoxStyle);
    detail.form = new QGridLayout(detail.group);
    detail.form->setHorizontalSpacing(12);
    detail.form->setVerticalSpacing(10);
    detail.form->setColumnMinimumWidth(0, 120);
    // Keep fields at roughly 65% of the available field area, with the
    // remainder as trailing space; minimum hints protect smaller windows.
    detail.form->setColumnStretch(1, 65);
    detail.form->setColumnStretch(2, 35);
    return detail;
}

void addFormRow(DetailPage& detail, const QString& caption, QWidget* field)
{
    auto* label = new QLabel(caption);
    applyLabelStyle(label);
    detail.form->addWidget(label, detail.row, 0, Qt::AlignVCenter);
    detail.form->addWidget(field, detail.row, 1);
    ++detail.row;
}

// The "Connect automatically" toggle, the Connect button row and the stretch.
// `extraButtons` follow the edit hint.
void finishDetailPage(DetailPage& detail, PeripheralDeviceUi& ui,
                      const QList<QWidget*>& extraButtons)
{
    detail.group->setMaximumWidth(760);
    detail.layout->addWidget(detail.group, 0, Qt::AlignTop);

    if (ui.kind == PeripheralDeviceUi::Kind::AuthNetwork) {
        auto* autoConnect = new QCheckBox(RadioSetupDialog::tr("Connect automatically"), detail.page);
        autoConnect->setObjectName(QStringLiteral("peripheralAutoConnect_%1").arg(ui.id));
        autoConnect->setAccessibleName(RadioSetupDialog::tr("%1 connect automatically").arg(ui.label));
        autoConnect->setAccessibleDescription(RadioSetupDialog::tr(
            "Connect to %1 on startup, when the radio or discovery reports it, and after a "
            "drop. Turn off to connect only when you click Connect.").arg(ui.label));
        ThemeManager::instance().applyStyleSheet(autoConnect,
            "QCheckBox { color: {{color.text.primary}}; font-size: 11px; spacing: 8px; }"
            + kCheckBoxIndicator);
        autoConnect->setChecked(PeripheralSettings::autoConnect(ui.id));
        // Written at once. The connections read it when they decide to
        // reconnect, so nothing needs to be pushed to them here.
        QObject::connect(autoConnect, &QCheckBox::toggled, autoConnect,
                         [id = ui.id](bool on) { PeripheralSettings::setAutoConnect(id, on); });
        detail.layout->addWidget(autoConnect);
        ui.autoConnectCheck = autoConnect;
    }

    ui.editHint = new QLabel(detail.page);
    ui.editHint->setObjectName(QStringLiteral("peripheralEditHint_%1").arg(ui.id));
    ui.editHint->setWordWrap(true);
    ThemeManager::instance().applyStyleSheet(ui.editHint,
        "QLabel { color: {{color.text.secondary}}; font-size: 11px; }");

    auto* buttonRow = new QHBoxLayout;
    buttonRow->addWidget(ui.connectButton);
    buttonRow->addWidget(ui.editHint, 1, Qt::AlignVCenter);
    for (QWidget* button : extraButtons) {
        buttonRow->addWidget(button);
    }
    buttonRow->addStretch();
    detail.layout->addLayout(buttonRow);
    detail.layout->addStretch();
    ui.page = detail.page;

    // Remember the help text of the fields that are locked while connected.
    for (QWidget* field : {static_cast<QWidget*>(ui.addressEdit), static_cast<QWidget*>(ui.addressStack),
                           static_cast<QWidget*>(ui.portSpin), static_cast<QWidget*>(ui.portStack),
                           static_cast<QWidget*>(ui.modeCombo)}) {
        if (field) {
            ui.fieldHelp.insert(field, {field->accessibleDescription(), field->toolTip()});
        }
    }
}

QPushButton* makeConnectButton(PeripheralDeviceUi& ui)
{
    auto* button = new QPushButton(ui.isConnected() ? QStringLiteral("Disconnect")
                                                    : QStringLiteral("Connect"));
    button->setObjectName(QStringLiteral("peripheralConnect_%1").arg(ui.id));
    button->setAccessibleName(RadioSetupDialog::tr("Connect or disconnect %1").arg(ui.label));
    ThemeManager::instance().applyStyleSheet(button, kButtonStyle);
    return button;
}

QSpinBox* makePortSpin(const PeripheralDeviceUi& ui)
{
    auto* spin = new QSpinBox;
    spin->setObjectName(QStringLiteral("peripheralPort_%1").arg(ui.id));
    spin->setRange(1, 65535);
    ThemeManager::instance().applyStyleSheet(spin, kSpinStyle);
    spin->setAccessibleName(RadioSetupDialog::tr("%1 port").arg(ui.label));
    return spin;
}

// Return a device's inputs to their defaults after Remove.
void resetFields(PeripheralDeviceUi& ui)
{
    if (ui.addressEdit) {
        ui.addressEdit->setText(QString()); // Removal is a reset, not an unsaved edit.
        ui.addressEdit->setProperty("peripheralSavedHost", QString());
        ui.addressEdit->setProperty("peripheralSavedPort", QString());
    }
    if (ui.addressStack) {
        for (QLineEdit* edit : ui.addressStack->findChildren<QLineEdit*>()) {
            edit->clear();
        }
    }
    if (ui.portSpin) {
        ui.portSpin->setValue(ui.defaultPort);
        if (ui.addressEdit) {
            ui.addressEdit->setProperty("peripheralPrefillPort", ui.portSpin->value());
        }
    }
    if (ui.portStack) {
        for (QSpinBox* spin : ui.portStack->findChildren<QSpinBox*>()) {
            spin->setValue(ui.defaultPort);
        }
        ui.portStack->setCurrentIndex(0);
    }
    if (ui.modeCombo) {
        const QSignalBlocker blocked(ui.modeCombo);
        ui.modeCombo->setCurrentIndex(0);
    }
    if (ui.addressStack) {
        for (QComboBox* serialChoice : ui.addressStack->findChildren<QComboBox*>()) {
            const QSignalBlocker blocked(serialChoice);
            serialChoice->setCurrentIndex(0);
            if (QLineEdit* custom = serialChoice->parentWidget()->findChild<QLineEdit*>(
                    QString(), Qt::FindDirectChildrenOnly)) {
                custom->setVisible(serialChoice->currentData().toString()
                                   == QStringLiteral("__custom__"));
            }
        }
        ui.addressStack->setCurrentIndex(0);
    }
    if (ui.codeEdit) {
        ui.codeEdit->clear();
    }
    ui.state = PeripheralDeviceStatus{};
}

// Wiped on a deliberate Remove, and on Setup closing with a cleared field: only
// the target this field was editing is cleared, never one saved since.
void closeSaverForNetworkField(PeripheralDeviceUi* ui, QVector<std::function<void()>>& savers)
{
    savers.append([ui]() {
        QLineEdit* ipEdit = ui->addressEdit;
        auto& settings = AppSettings::instance();
        if (!ipEdit || !ipEdit->isModified()) {
            return;
        }
        if (!ipEdit->text().trimmed().isEmpty()) {
            return;
        }
        const QString savedIp = settings.value(ui->ipKey, QString()).toString().trimmed();
        // Only clear the target this field was editing. Another entry point
        // may have saved a new connection since Setup last synchronized.
        if (savedIp.isEmpty() || savedIp != ipEdit->property("peripheralSavedHost").toString()
            || settings.value(ui->portKey, QString()).toString()
                != ipEdit->property("peripheralSavedPort").toString()) {
            return;
        }
        // The user cleared a previously-saved IP. If still connected (e.g.
        // auto-connect ran at startup), disconnect first so downstream
        // visibility handlers see the cleared settings.
        settings.remove(ui->ipKey);
        settings.remove(ui->portKey);
        settings.save();
        if (ui->isConnected()) {
            ui->disconnectNow();
        }
    });
}

// Same for the devices whose target lives in a PeripheralSettings object.
void closeSaverForSettingsGroup(PeripheralDeviceUi* ui, QVector<std::function<void()>>& savers)
{
    savers.append([ui]() {
        QLineEdit* netIpEdit = ui->networkHostEdit;
        if (!netIpEdit || !netIpEdit->text().trimmed().isEmpty()) {
            return;
        }
        const QString savedIp = PeripheralSettings::deviceString(ui->settingsGroup, "ManualIp");
        if (savedIp.isEmpty()) {
            return;
        }
        PeripheralSettings::clearDeviceField(ui->settingsGroup, "ManualIp");
        PeripheralSettings::clearDeviceField(ui->settingsGroup, "ManualPort");
        // Only disconnect if the live connection is actually the network target
        // being cleared; it may be connected over Serial, which this saved IP
        // has nothing to do with.
        if (ui->isConnected() && ui->description
            && ui->description().startsWith(savedIp + ":")) {
            ui->disconnectNow();
        }
    });
}

} // namespace

PeripheralDeviceUi* RadioSetupDialog::peripheralDevice(const QString& id) const
{
    for (const auto& ui : m_peripheralDevices) {
        if (ui->id == id) {
            return ui.get();
        }
    }
    return nullptr;
}

PeripheralDeviceStatus RadioSetupDialog::peripheralStatusForTest(const QString& id) const
{
    const PeripheralDeviceUi* ui = peripheralDevice(id);
    return ui ? ui->state : PeripheralDeviceStatus{};
}

void RadioSetupDialog::editPeripheralStatusForTest(
    const QString& id, const std::function<void(PeripheralDeviceStatus&)>& edit)
{
    if (PeripheralDeviceUi* ui = peripheralDevice(id)) {
        edit(ui->state);
    }
}

// ── TGXL, PGXL, Antenna Genius, ShackSwitch ────────────────────────────────

void RadioSetupDialog::buildAuthNetworkDevice(PeripheralDeviceUi& ui, QWidget* stackParent,
                                              const std::function<void()>& refresh)
{
    PeripheralDeviceUi* const u = &ui;
    auto& settings = AppSettings::instance();
    DetailPage detail = beginDetailPage(ui, stackParent);

    // Address: the saved target, or the live connection's peer.
    ui.addressEdit = new QLineEdit;
    ui.addressEdit->setObjectName(QStringLiteral("peripheralAddress_%1").arg(ui.id));
    ui.addressEdit->setPlaceholderText(QStringLiteral("e.g. 192.168.1.100"));
    ui.addressEdit->setAccessibleName(tr("%1 address").arg(ui.label));
    applyEditStyle(ui.addressEdit);
    ui.addressEdit->setMinimumWidth(140);
    const QString savedIp = settings.value(ui.ipKey, "").toString();
    if (!savedIp.isEmpty()) {
        ui.addressEdit->setText(savedIp);
    } else if (ui.isConnected()) {
        ui.addressEdit->setText(ui.peerAddress());
    }

    ui.portSpin = makePortSpin(ui);
    const int savedPort = settings.value(ui.portKey, "0").toInt();
    if (savedPort > 0) {
        ui.portSpin->setValue(savedPort);
    } else if (ui.isConnected() && ui.peerPort() > 0) {
        ui.portSpin->setValue(ui.peerPort());
    } else {
        ui.portSpin->setValue(ui.defaultPort);
    }
    ui.addressEdit->setProperty("peripheralPrefillPort", ui.portSpin->value());
    ui.addressEdit->setProperty("peripheralSavedHost", savedIp.trimmed());
    ui.addressEdit->setProperty("peripheralSavedPort", settings.value(ui.portKey, QString()).toString());
    // Only radio-reported endpoints are discovery retries. lastHost may instead
    // belong to a removed manual/WAN target; absence of a saved IP proves nothing.
    auto markDiscovery = [u]() {
        if (u->discoveredHost) {
            u->addressEdit->setProperty("peripheralDiscoveredHost", u->discoveredHost());
            u->addressEdit->setProperty("peripheralDiscoveredPort", u->discoveredPort);
        }
    };
    markDiscovery();

    addFormRow(detail, tr("IP Address"), ui.addressEdit);
    addFormRow(detail, tr("Port"), ui.portSpin);

    ui.connectButton = makeConnectButton(ui);

    // The connect call itself: with a code field, a typed code is handed to the
    // connection after the target switch; without one it is just the target.
    std::function<void(const QString&, quint16)> connectFn = ui.connectTo;
    if (ui.hasAuthCode) {
        connectFn = [u, refresh](const QString& host, quint16 port) {
            connectPeripheralWithCode(u->codeEdit, u->state, host, port, u->connectTo, u->applyCode);
        };
    }

    connect(ui.connectButton, &QPushButton::clicked, this,
            [u, connectFn, markDiscovery, refresh, &settings]() {
        PeripheralDeviceStatus& state = u->state;
        state.attention = Attention::None;
        state.message.clear();
        state.discardedAuthCode = false;
        const QString ip = u->addressEdit->text().trimmed();
        if (!u->isConnected() && PeripheralRemovalGuard::pending(u->authDevice)) {
            state.attention = Attention::RemovalPending;
            state.message = RadioSetupDialog::tr(
                "Credential deletion from a removal is still pending — "
                "retry after the keychain request finishes");
            refresh();
            return;
        }
        if (u->isConnected()) {
            // If the user cleared the IP field before clicking, wipe the saved
            // manual IP/port FIRST: the disconnect signal fires synchronously
            // and downstream handlers (e.g. SS button visibility) read these
            // settings to decide whether to keep showing the device.
            if (ip.isEmpty()) {
                settings.remove(u->ipKey);
                settings.remove(u->portKey);
                settings.save();
            }
            u->disconnectNow();
        } else if (ip.isEmpty()) {
            // Empty IP while disconnected: if a manual IP was saved previously,
            // treat this click as "save back to default": clear the persisted
            // manual IP/port so the device stops auto-connecting.
            if (!settings.value(u->ipKey, "").toString().isEmpty()) {
                settings.remove(u->ipKey);
                settings.remove(u->portKey);
                settings.save();
            }
        } else {
            const int port = u->portSpin->value();
            u->state.connecting = true;
            markDiscovery();
            connectPeripheralTarget(u->addressEdit, u->ipKey, u->portKey,
                                    static_cast<quint16>(port), connectFn);
            if (u->state.needsAttention()) {
                u->state.connecting = false; // refused before a socket was opened
            }
            u->addressEdit->setProperty("peripheralSavedHost",
                settings.value(u->ipKey, QString()).toString().trimmed());
            u->addressEdit->setProperty("peripheralSavedPort",
                settings.value(u->portKey, QString()).toString());
            u->addressEdit->setProperty("peripheralPrefillPort", u->portSpin->value());
            u->addressEdit->setModified(false);
        }
        refresh();
    });

    // A blank code field means "reuse the saved code". Only an explicit Show
    // action loads a saved value into this automation-redacted field.
    if (ui.hasAuthCode) {
        const QString label = ui.shortName;
        auto* edit = new QLineEdit;
        ui.codeEdit = edit;
        edit->setObjectName(QStringLiteral("peripheralAuth_%1_4").arg(ui.id));
        edit->setEchoMode(QLineEdit::Password);
        edit->setProperty("aetherSensitiveValue", true);
        edit->setProperty("authDeviceLabel", label);
        edit->setPlaceholderText(PeripheralAuthStore::persistentStoreAvailable()
            ? tr("Blank uses saved code") : tr("Code for this session only"));
        edit->setAccessibleName(tr("%1 authorization code").arg(label));
        const QString blankDescription = PeripheralAuthStore::persistentStoreAvailable()
            ? tr("Leave blank to use the code saved for this device address when authentication is requested. Enter the code again if its address changed. A new code replaces the saved one only after the device accepts it.")
            : tr("Enter a code for this session when the device requests authentication.");
        edit->setProperty("authBlankDescription", blankDescription);
        edit->setAccessibleDescription(blankDescription);
        connect(edit, &QLineEdit::textChanged, edit, [edit](const QString& text) {
            edit->setAccessibleDescription(text.isEmpty()
                ? edit->property("authBlankDescription").toString()
                : RadioSetupDialog::tr("A code has been entered. It replaces the saved code only after the device accepts it."));
        });
        applyEditStyle(edit);

        auto* show = new QPushButton(tr("Show"));
        ui.showButton = show;
        show->setObjectName(QStringLiteral("peripheralAuth_%1_5").arg(ui.id));
        ThemeManager::instance().applyStyleSheet(show, kButtonStyle);
        show->setAccessibleName(tr("%1 show authorization code").arg(label));
        auto concealSavedCode = [edit, show, label]() {
            edit->setProperty("authRevealGeneration",
                edit->property("authRevealGeneration").toULongLong() + 1);
            if (edit->property("peripheralSavedCodeRevealed").toBool()) {
                edit->setProperty("peripheralSavedCodeRevealed", false);
                edit->clear();
            }
            edit->setEchoMode(QLineEdit::Password);
            show->setText(RadioSetupDialog::tr("Show"));
            show->setAccessibleName(RadioSetupDialog::tr("%1 show authorization code").arg(label));
        };
        connect(this, &QDialog::finished, edit, concealSavedCode);
        // Connected: the identity the connection itself saves under.
        // Otherwise: the same identity from what is typed in the form.
        auto currentEndpoint = [u]() {
            const QString live = u->liveEndpoint ? u->liveEndpoint() : QString();
            return !live.isEmpty() ? live
                : PeripheralAuthStore::configuredEndpoint(
                      u->addressEdit->text(), static_cast<quint16>(u->portSpin->value()));
        };
        connect(edit, &QLineEdit::textChanged, edit, [edit]() {
            edit->setProperty("authRevealGeneration",
                edit->property("authRevealGeneration").toULongLong() + 1);
        });
        connect(edit, &QLineEdit::textEdited, edit, [edit]() {
            // An operator edit is a replacement; merely showing a saved code is not.
            edit->setProperty("peripheralSavedCodeRevealed", false);
        });
        const PeripheralAuthStore::Device authDevice = ui.authDevice;
        connect(show, &QPushButton::clicked, this,
                [edit, show, label, authDevice, currentEndpoint, concealSavedCode, refresh]() {
            if (edit->echoMode() == QLineEdit::Normal) {
                concealSavedCode();
                return;
            }
            auto reveal = [edit, show, label]() {
                edit->setEchoMode(QLineEdit::Normal);
                show->setText(RadioSetupDialog::tr("Hide"));
                show->setAccessibleName(RadioSetupDialog::tr("%1 hide authorization code").arg(label));
            };
            if (!edit->text().isEmpty()) {
                reveal();
                return;
            }
            const QString endpoint = currentEndpoint();
            if (endpoint.isEmpty()) {
                edit->setPlaceholderText(RadioSetupDialog::tr("Code blank"));
                return;
            }
            const quint64 generation = edit->property("authRevealGeneration").toULongLong() + 1;
            edit->setProperty("authRevealGeneration", generation);
            PeripheralAuthStore::load(authDevice, endpoint, edit,
                [edit, endpoint, generation, currentEndpoint, reveal, refresh](
                    const PeripheralAuthStore::LoadResult& result) {
                    if (generation != edit->property("authRevealGeneration").toULongLong()
                        || endpoint != currentEndpoint() || !edit->isVisible()
                        || !edit->text().isEmpty()) {
                        return;
                    }
                    if (result.status == PeripheralAuthStore::LoadStatus::Found) {
                        edit->setProperty("peripheralSavedCodeRevealed", true);
                        edit->setProperty("authRevealedEndpoint", endpoint);
                        edit->setText(result.code);
                        reveal();
                    } else {
                        edit->setPlaceholderText(RadioSetupDialog::tr("Code blank"));
                    }
                    refresh(); // the credential store answered: refresh what it implies
                });
        });
        // Invalidate delayed loads and clear revealed vault data on a target edit.
        connect(ui.addressEdit, &QLineEdit::textChanged, edit, concealSavedCode);
        connect(ui.portSpin, &QSpinBox::valueChanged, edit, concealSavedCode);

        auto* clear = new QPushButton(tr("Clear code"));
        ui.clearButton = clear;
        clear->setObjectName(QStringLiteral("peripheralAuth_%1_6").arg(ui.id));
        ThemeManager::instance().applyStyleSheet(clear, kButtonStyle);
        clear->setAccessibleName(tr("%1 clear saved authorization code").arg(label));
        connect(clear, &QPushButton::clicked, edit, concealSavedCode);
        connect(clear, &QPushButton::clicked, this, [this, u, edit, authDevice, refresh]() {
            edit->clear();
            u->state.pendingAuthCode = false;
            u->state.discardedAuthCode = false;
            PeripheralAuthStore::clear(authDevice, this,
                [u, refresh](PeripheralAuthStore::ClearResult result) {
                    if (result == PeripheralAuthStore::ClearResult::SessionCleared) {
                        u->state.attention = Attention::CredentialWarning;
                        u->state.message = RadioSetupDialog::tr(
                            "Keychain unavailable; session code cleared, stored-code deletion unconfirmed");
                    } else if (result == PeripheralAuthStore::ClearResult::Failed) {
                        u->state.attention = Attention::CredentialError;
                        u->state.message = RadioSetupDialog::tr(
                            "Saved code remains in keychain; retry Clear code");
                    } else {
                        u->state.attention = Attention::None;
                        u->state.message.clear();
                        u->state.pendingAuthCode = false;
                        u->state.discardedAuthCode = false;
                        u->state.note.clear();
                    }
                    refresh();
                });
            if (u->clearCode) {
                u->clearCode();
            }
        });

        addFormRow(detail, tr("Auth. Code"), edit);
        auto* authButtons = new QWidget(detail.group);
        auto* authLayout = new QHBoxLayout(authButtons);
        authLayout->setContentsMargins(0, 0, 0, 0);
        authLayout->setSpacing(6);
        authLayout->addStretch();
        authLayout->addWidget(show);
        authLayout->addWidget(clear);
        detail.form->addWidget(authButtons, detail.row++, 1);
    }

    QList<QWidget*> extras;
    if (ui.openWebUi) {
        ui.webUiButton = new QPushButton(QStringLiteral("⚙ Web UI"));
        ui.webUiButton->setObjectName(QStringLiteral("peripheralWebUi_%1").arg(ui.id));
        ui.webUiButton->setAccessibleName(tr("Open the %1 web interface").arg(ui.label));
        ThemeManager::instance().applyStyleSheet(ui.webUiButton, kButtonStyle);
        ui.webUiButton->setToolTip(tr("Open ShackSwitch web interface"));
        connect(ui.webUiButton, &QPushButton::clicked, this, [u]() { u->openWebUi(); });
        extras.append(ui.webUiButton);
    }

    closeSaverForNetworkField(u, m_peripheralRowSavers);
    finishDetailPage(detail, ui, extras);
}

// ── ACOM, SPE Expert, LP-100A ──────────────────────────────────────────────

void RadioSetupDialog::buildSerialNetworkDevice(PeripheralDeviceUi& ui, QWidget* stackParent,
                                                const std::function<void()>& refresh,
                                                const std::shared_ptr<QVector<std::function<void()>>>& pageReseeds)
{
    PeripheralDeviceUi* const u = &ui;
    const QString group = ui.settingsGroup;
    DetailPage detail = beginDetailPage(ui, stackParent);

    ui.modeCombo = new QComboBox;
    ui.modeCombo->setObjectName(QStringLiteral("peripheralMode_%1").arg(ui.id));
    ThemeManager::instance().applyStyleSheet(ui.modeCombo, kComboStyle);
    ui.modeCombo->setAccessibleName(tr("%1 connection type").arg(ui.label));
#ifdef HAVE_SERIALPORT
    ui.modeCombo->addItem("Serial", "Serial");
#endif
    ui.modeCombo->addItem("Network", "Network");
    if (!ui.networkTooltip.isEmpty()) {
        ui.modeCombo->setToolTip(ui.networkTooltip);
    }

    // Address: a serial-port combo (with a "Custom..." fallback, same pattern as
    // the CW/keying Port Configuration group) or a plain host field, swapped
    // through a QStackedWidget.
    ui.addressStack = new QStackedWidget;
    ui.addressStack->setObjectName(QStringLiteral("peripheralAddress_%1").arg(ui.id));
    int serialPageIdx = -1;
    QComboBox* serialCombo = nullptr;
    QLineEdit* serialCustomEdit = nullptr;
#ifdef HAVE_SERIALPORT
    {
        auto* serialPage = new QWidget;
        auto* lay = new QHBoxLayout(serialPage);
        lay->setContentsMargins(0, 0, 0, 0);
        serialCombo = new QComboBox;
        ThemeManager::instance().applyStyleSheet(serialCombo, kComboStyle);
        serialCombo->setAccessibleName(tr("%1 serial port").arg(ui.label));
        serialCustomEdit = new QLineEdit;
        serialCustomEdit->setPlaceholderText("/dev/ttyUSB0");
        serialCustomEdit->setAccessibleName(tr("%1 custom serial port").arg(ui.label));
        applyEditStyle(serialCustomEdit);
        populateSerialPortCombo(serialCombo, serialCustomEdit,
                                PeripheralSettings::deviceString(group, "SerialPort"));
        serialCustomEdit->setVisible(serialCombo->currentData().toString() == "__custom__");
        connect(serialCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [serialCombo, serialCustomEdit](int idx) {
            serialCustomEdit->setVisible(serialCombo->itemData(idx).toString() == "__custom__");
        });
        // Refresh a retained page without discarding its current edits.
        auto reseed = [combo = QPointer<QComboBox>(serialCombo),
                       edit = QPointer<QLineEdit>(serialCustomEdit)]() {
            if (!combo || !edit) {
                return;
            }
            edit->setVisible(refreshSerialPortCombo(combo, edit));
        };
        m_serialPortReseeds.append(reseed);
        pageReseeds->append(reseed);
        lay->addWidget(serialCombo, 1);
        lay->addWidget(serialCustomEdit, 1);
        serialPageIdx = ui.addressStack->addWidget(serialPage);
    }
#endif
    auto* netPage = new QWidget;
    auto* netLay = new QHBoxLayout(netPage);
    netLay->setContentsMargins(0, 0, 0, 0);
    auto* netIpEdit = new QLineEdit;
    ui.networkHostEdit = netIpEdit;
    netIpEdit->setObjectName(QStringLiteral("peripheralNetworkHost_%1").arg(ui.id));
    netIpEdit->setPlaceholderText(ui.networkPlaceholder);
    netIpEdit->setAccessibleName(tr("%1 network address").arg(ui.label));
    netIpEdit->setAccessibleDescription(ui.networkDescription);
    applyEditStyle(netIpEdit);
    netIpEdit->setText(PeripheralSettings::deviceString(group, "ManualIp"));
    if (!ui.networkTooltip.isEmpty()) {
        netIpEdit->setToolTip(ui.networkTooltip);
    }
    netLay->addWidget(netIpEdit);
    const int netPageIdx = ui.addressStack->addWidget(netPage);

    // Port column: fixed text for serial (mandated by the device's own protocol
    // spec) or a port spin box for network mode.
    ui.portStack = new QStackedWidget;
    ui.portStack->setObjectName(QStringLiteral("peripheralPort_%1").arg(ui.id));
    int serialBaudIdx = -1;
#ifdef HAVE_SERIALPORT
    {
        auto* fixedLbl = new QLabel(ui.fixedBaud);
        ThemeManager::instance().applyStyleSheet(fixedLbl,
            "QLabel { color: {{color.text.secondary}}; font-size: 11px; }");
        serialBaudIdx = ui.portStack->addWidget(fixedLbl);
    }
#endif
    auto* netPortSpin = new QSpinBox;
    netPortSpin->setRange(1, 65535);
    netPortSpin->setValue(PeripheralSettings::deviceInt(group, "ManualPort", ui.defaultPort));
    netPortSpin->setAccessibleName(tr("%1 network port").arg(ui.label));
    ThemeManager::instance().applyStyleSheet(netPortSpin, kSpinStyle);
    const int netPortIdx = ui.portStack->addWidget(netPortSpin);

    auto applyMode = [stack = ui.addressStack, ports = ui.portStack,
                      serialPageIdx, serialBaudIdx, netPageIdx, netPortIdx](const QString& mode) {
#ifdef HAVE_SERIALPORT
        if (mode == "Serial" && serialPageIdx >= 0) {
            stack->setCurrentIndex(serialPageIdx);
            ports->setCurrentIndex(serialBaudIdx);
            return;
        }
#endif
        Q_UNUSED(serialPageIdx);
        Q_UNUSED(serialBaudIdx);
        stack->setCurrentIndex(netPageIdx);
        ports->setCurrentIndex(netPortIdx);
    };
    {
        const QString savedMode = PeripheralSettings::deviceString(group, "ConnectionMode",
#ifdef HAVE_SERIALPORT
            "Serial"
#else
            "Network"
#endif
        );
        const int idx = ui.modeCombo->findData(savedMode);
        ui.modeCombo->setCurrentIndex(idx >= 0 ? idx : 0);
    }
    applyMode(ui.modeCombo->currentData().toString());
    connect(ui.modeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [group, combo = ui.modeCombo, applyMode, refresh](int idx) {
        const QString mode = combo->itemData(idx).toString();
        PeripheralSettings::setDeviceString(group, "ConnectionMode", mode);
        applyMode(mode);
        refresh();
    });

    ui.connectButton = makeConnectButton(ui);
    connect(ui.connectButton, &QPushButton::clicked, this,
            [u, group, netIpEdit, netPortSpin, serialCombo, serialCustomEdit, refresh]() {
        if (u->isConnected()) {
            u->disconnectNow();
            return;
        }
        u->state.attention = Attention::None;
        u->state.message.clear();
        const QString mode = u->modeCombo->currentData().toString();
        if (mode == "Network") {
            const QString ip = netIpEdit->text().trimmed();
            if (ip.isEmpty()) {
                refresh();
                return;
            }
            const int port = netPortSpin->value();
            PeripheralSettings::setDeviceString(group, "ManualIp", ip);
            PeripheralSettings::setDeviceInt(group, "ManualPort", port);
            u->state.connecting = true;
            u->connectNetwork(ip, static_cast<quint16>(port));
        }
#ifdef HAVE_SERIALPORT
        else {
            QString port = serialCombo->currentData().toString();
            if (port == "__custom__") {
                port = serialCustomEdit->text().trimmed();
            }
            if (port.isEmpty()) {
                refresh();
                return;
            }
            PeripheralSettings::setDeviceString(group, "SerialPort", port);
            u->state.connecting = true;
            u->connectSerial(port);
        }
#else
        Q_UNUSED(serialCombo);
        Q_UNUSED(serialCustomEdit);
#endif
        refresh();
    });

    addFormRow(detail, tr("Connection Type"), ui.modeCombo);
    addFormRow(detail, tr("Address/Serial Port"), ui.addressStack);
    addFormRow(detail, tr("TCP Port/Speed"), ui.portStack);

    QList<QWidget*> extras;
#ifdef HAVE_SERIALPORT
    ui.serialRefreshButton = new QPushButton(tr("Refresh Serial Ports"), detail.page);
    ui.serialRefreshButton->setObjectName(QStringLiteral("peripheralSerialRefresh_%1").arg(ui.id));
    ui.serialRefreshButton->setAutoDefault(false);
    ui.serialRefreshButton->setAccessibleName(tr("%1 refresh serial ports").arg(ui.label));
    ThemeManager::instance().applyStyleSheet(ui.serialRefreshButton, kButtonStyle);
    connect(ui.serialRefreshButton, &QPushButton::clicked, this, [pageReseeds]() {
        for (const auto& reseed : *pageReseeds) {
            reseed();
        }
    });
    extras.append(ui.serialRefreshButton);
#else
    Q_UNUSED(pageReseeds);
#endif

    // The network host field is the only free-text target; serial mode resolves
    // to a discovered port or an explicit Custom entry committed on Connect.
    closeSaverForSettingsGroup(u, m_peripheralRowSavers);
    finishDetailPage(detail, ui, extras);
}

// ── VK3AMP ─────────────────────────────────────────────────────────────────

void RadioSetupDialog::buildVkampDevice(PeripheralDeviceUi& ui, QWidget* stackParent,
                                        const std::function<void()>& refresh)
{
    PeripheralDeviceUi* const u = &ui;
    DetailPage detail = beginDetailPage(ui, stackParent);

    ui.addressEdit = new QLineEdit;
    ui.addressEdit->setObjectName(QStringLiteral("peripheralAddress_%1").arg(ui.id));
    ui.addressEdit->setPlaceholderText(QStringLiteral("e.g. 192.168.1.50"));
    applyEditStyle(ui.addressEdit);
    ui.addressEdit->setText(PeripheralSettings::deviceString("Vkamp", "ManualIp"));
    // Name answers "what is this?", description answers "what do I type?"
    // (docs/a11y.md section 2).
    ui.addressEdit->setAccessibleName(tr("VK3AMP address"));
    ui.addressEdit->setAccessibleDescription(tr("IP address or host name of the VK3AMP amplifier"));

    ui.portSpin = makePortSpin(ui);
    ui.portSpin->setValue(PeripheralSettings::deviceInt("Vkamp", "ManualPort", ui.defaultPort));
    ui.portSpin->setAccessibleName(tr("VK3AMP control port"));
    ui.portSpin->setAccessibleDescription(tr("TCP control port, 1 to 65535, default 5005"));

    ui.connectButton = makeConnectButton(ui);
    ui.connectButton->setAccessibleName(tr("Connect or disconnect the VK3AMP amplifier"));
    connect(ui.connectButton, &QPushButton::clicked, this, [u, refresh]() {
        if (u->isConnected()) {
            u->disconnectNow();
            return;
        }
        u->state.attention = Attention::None;
        u->state.message.clear();
        const QString ip = u->addressEdit->text().trimmed();
        if (ip.isEmpty()) {
            refresh();
            return;
        }
        const int port = u->portSpin->value();
        PeripheralSettings::setDeviceString("Vkamp", "ManualIp", ip);
        PeripheralSettings::setDeviceInt("Vkamp", "ManualPort", port);
        // Immediate feedback: a cold connect can legitimately take several
        // seconds (this amplifier's own network stack only answers broadcast
        // ARP, which can stall Windows' unicast-first neighbor-cache
        // reconfirmation; see VkampConnection.h), and a status stuck on "Not
        // connected" reads as frozen rather than in progress.
        u->state.connecting = true;
        refresh();
        u->connectNetwork(ip, static_cast<quint16>(port));
        refresh();
    });

    // The hardware variant: 600W/1000W/2000W ship as distinct rated-power
    // classes and the wire protocol has no model field to detect which one this
    // is. Picking the wrong one only misscales the forward-power gauge, so this
    // defaults to W2000 (the originally-confirmed unit) rather than blocking.
    ui.modelCombo = new QComboBox;
    ui.modelCombo->setObjectName(QStringLiteral("peripheralAmplifierModel"));
    ThemeManager::instance().applyStyleSheet(ui.modelCombo, kComboStyle);
    ui.modelCombo->setAccessibleName(tr("VK3AMP amplifier model"));
    ui.modelCombo->setAccessibleDescription(
        tr("Rated output of your unit. Sets the power meter's full scale; it does not change "
           "anything on the amplifier."));
    for (auto v : {Vkamp::Variant::W600, Vkamp::Variant::W1000, Vkamp::Variant::W2000}) {
        ui.modelCombo->addItem(Vkamp::variantLabel(v), static_cast<int>(v));
    }
    {
        const int savedVariant = PeripheralSettings::deviceInt(
            "Vkamp", "Variant", static_cast<int>(Vkamp::Variant::W2000));
        const int idx = ui.modelCombo->findData(savedVariant);
        ui.modelCombo->setCurrentIndex(idx >= 0 ? idx : ui.modelCombo->count() - 1);
    }
    connect(ui.modelCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this, combo = ui.modelCombo](int idx) {
        PeripheralSettings::setDeviceInt("Vkamp", "Variant", combo->itemData(idx).toInt());
        emit vkampVariantChanged();
    });

    addFormRow(detail, tr("IP Address"), ui.addressEdit);
    addFormRow(detail, tr("Port"), ui.portSpin);
    addFormRow(detail, tr("Amplifier Model"), ui.modelCombo);

    m_peripheralRowSavers.append([u]() {
        if (!u->addressEdit || !u->addressEdit->text().trimmed().isEmpty()) {
            return;
        }
        const QString savedIp = PeripheralSettings::deviceString("Vkamp", "ManualIp");
        if (savedIp.isEmpty()) {
            return;
        }
        PeripheralSettings::clearDeviceField("Vkamp", "ManualIp");
        PeripheralSettings::clearDeviceField("Vkamp", "ManualPort");
        if (u->isConnected() && u->description().startsWith(savedIp + ":")) {
            u->disconnectNow();
        }
    });
    finishDetailPage(detail, ui, {});
}

// ── The tab ────────────────────────────────────────────────────────────────

namespace {

// Calls `refresh` whenever the page is shown.
class ShowNotifier final : public QObject {
public:
    ShowNotifier(QObject* parent, std::function<void()> onShow)
        : QObject(parent), m_onShow(std::move(onShow)) {}

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (event->type() == QEvent::Show) {
            m_onShow();
        }
        return QObject::eventFilter(watched, event);
    }

private:
    std::function<void()> m_onShow;
};

} // namespace

QWidget* RadioSetupDialog::buildPeripheralsTab()
{
    auto* page = new QWidget;
    page->setObjectName(QStringLiteral("peripheralsPage"));
    ThemeManager::instance().applyStyleSheet(page,
        "QWidget#peripheralsPage { background: {{color.background.0}}; }");
    auto* vbox = new QVBoxLayout(page);
    vbox->setSpacing(8);

    // Reseeds for this page's serial-port combos (ACOM, SPE, LP-100A). Each
    // device appends to both this and m_serialPortReseeds; this copy drives the
    // page's own "Refresh serial ports" button, the member drives showEvent().
    auto serialReseeds = std::make_shared<QVector<std::function<void()>>>();

    // Refresh is re-entrant (it can change the selection, which refreshes), so
    // a nested request just asks the running one to go round again.
    struct RefreshState {
        std::function<void()> body;
        bool running{false};
        bool again{false};
    };
    auto refreshState = std::make_shared<RefreshState>();
    const std::function<void()> refresh = [refreshState]() {
        if (!refreshState->body) {
            return;
        }
        if (refreshState->running) {
            refreshState->again = true;
            return;
        }
        refreshState->running = true;
        do {
            refreshState->again = false;
            refreshState->body();
        } while (refreshState->again);
        refreshState->running = false;
    };

    auto& settings = AppSettings::instance();
    m_peripheralDevices.clear();

    auto* content = new QWidget(page);
    content->setObjectName(QStringLiteral("peripheralContent"));
    ThemeManager::instance().applyStyleSheet(content,
        "QWidget#peripheralContent { background: {{color.background.0}}; }");
    auto* columns = new QHBoxLayout(content);
    columns->setContentsMargins(0, 0, 0, 0);
    columns->setSpacing(12);

    auto* detailStack = new QStackedWidget(content);
    detailStack->setObjectName(QStringLiteral("peripheralDetailStack"));
    auto* emptyPage = new QWidget(detailStack);
    auto* emptyLayout = new QVBoxLayout(emptyPage);
    auto* emptyText = new QLabel(
        tr("No peripherals added.\n\nChoose Add to configure a device."), emptyPage);
    emptyText->setAlignment(Qt::AlignCenter);
    ThemeManager::instance().applyStyleSheet(emptyText,
        "QLabel { color: {{color.text.secondary}}; font-size: 12px; }");
    emptyLayout->addWidget(emptyText);
    detailStack->addWidget(emptyPage);

    // ── Describe and build each device ────────────────────────────────────
    auto addDevice = [this](const QString& id, const QString& label, const QString& shortName,
                            PeripheralDeviceUi::Kind kind, int defaultPort) {
        auto ui = std::make_shared<PeripheralDeviceUi>();
        ui->id = id;
        ui->label = label;
        ui->shortName = shortName;
        ui->kind = kind;
        ui->defaultPort = defaultPort;
        m_peripheralDevices.push_back(ui);
        return ui;
    };
    const auto isShackSwitchTarget = [this]() {
        return AntennaGeniusModel::isShackSwitch(m_ag->connectedDevice());
    };

    if (m_tgxl) {
        auto ui = addDevice(QStringLiteral("tgxl"), tr("Tuner Genius XL"), QStringLiteral("TGXL"),
                            PeripheralDeviceUi::Kind::AuthNetwork, 9010);
        ui->radioRelay = true;
        ui->ipKey = QStringLiteral("TGXL_ManualIp");
        ui->portKey = QStringLiteral("TGXL_ManualPort");
        ui->hasAuthCode = true;
        ui->authDevice = PeripheralAuthStore::Device::Tgxl;
        ui->isConnected = [this]() { return m_tgxl->isConnected(); };
        ui->isConnecting = [this]() { return m_tgxl->isConnecting(); };
        ui->hasRadioRelay = [this]() {
            return m_model->isConnected() && !m_model->tunerModel().handle().isEmpty();
        };
        ui->disconnectNow = [this]() { m_tgxl->disconnect(); };
        ui->connectTo = [this](const QString& host, quint16 port) { m_tgxl->connectToTgxl(host, port); };
        ui->applyCode = [this](const QString& code) { m_tgxl->setAuthCode(code); };
        ui->clearCode = [this]() { m_tgxl->setAuthCode(QString()); };
        ui->peerAddress = [this]() { return m_tgxl->peerAddress(); };
        ui->peerPort = [this]() { return m_tgxl->peerPort(); };
        ui->liveEndpoint = [this]() {
            return m_tgxl->isConnected()
                ? PeripheralAuthStore::endpoint(m_tgxl->attemptHost(), m_tgxl->peerAddress(),
                                                m_tgxl->peerPort())
                : QString();
        };
        ui->discoveredHost = [this]() { return m_model->tunerModel().tgxlIp(); };
        ui->discoveredPort = 9010;
        ui->afterRemoval = [this]() { m_tgxl->disconnect(); m_tgxl->setAuthCode({}); };
        buildAuthNetworkDevice(*ui, detailStack, refresh);
        // Pre-fill the radio-discovered TGXL address when nothing is saved (#1039).
        if (ui->addressEdit->text().isEmpty()) {
            const QString discovered = m_model->tunerModel().tgxlIp();
            if (!discovered.isEmpty()) {
                ui->addressEdit->setText(discovered);
                ui->addressEdit->setProperty("peripheralDiscoveredHost", discovered);
                ui->addressEdit->setProperty("peripheralDiscoveredPort", 9010);
            }
        }
    }
    if (m_pgxl) {
        auto ui = addDevice(QStringLiteral("pgxl"), tr("Power Genius XL"), QStringLiteral("PGXL"),
                            PeripheralDeviceUi::Kind::AuthNetwork, 9008);
        ui->radioRelay = true;
        ui->ipKey = QStringLiteral("PGXL_ManualIp");
        ui->portKey = QStringLiteral("PGXL_ManualPort");
        ui->hasAuthCode = true;
        ui->authDevice = PeripheralAuthStore::Device::Pgxl;
        ui->isConnected = [this]() { return m_pgxl->isConnected(); };
        ui->isConnecting = [this]() { return m_pgxl->isConnecting(); };
        ui->hasRadioRelay = [this]() {
            return m_model->isConnected() && !m_model->amplifier().handle().isEmpty();
        };
        ui->disconnectNow = [this]() { m_pgxl->disconnect(); };
        ui->connectTo = [this](const QString& host, quint16 port) { m_pgxl->connectToPgxl(host, port); };
        ui->applyCode = [this](const QString& code) { m_pgxl->setAuthCode(code); };
        ui->clearCode = [this]() { m_pgxl->setAuthCode(QString()); };
        ui->peerAddress = [this]() { return m_pgxl->peerAddress(); };
        ui->peerPort = [this]() { return m_pgxl->peerPort(); };
        ui->liveEndpoint = [this]() {
            return m_pgxl->isConnected()
                ? PeripheralAuthStore::endpoint(m_pgxl->attemptHost(), m_pgxl->peerAddress(),
                                                m_pgxl->peerPort())
                : QString();
        };
        ui->discoveredHost = [this]() { return m_model->amplifier().ip(); };
        ui->discoveredPort = 9008;
        ui->afterRemoval = [this]() { m_pgxl->disconnect(); m_pgxl->setAuthCode({}); };
        buildAuthNetworkDevice(*ui, detailStack, refresh);
    }
    if (m_ag) {
        // Antenna Genius and ShackSwitch share one model; each owns the state
        // only while the model is serving it.
        auto ag = addDevice(QStringLiteral("ag"), tr("Antenna Genius"), QStringLiteral("Antenna Genius"),
                            PeripheralDeviceUi::Kind::AuthNetwork, 9007);
        ag->sharedAgModel = true;
        ag->endpointScopedCredential = true;
        ag->ipKey = QStringLiteral("AG_ManualIp");
        ag->portKey = QStringLiteral("AG_ManualPort");
        ag->hasAuthCode = true;
        ag->authDevice = PeripheralAuthStore::Device::AntennaGenius;
        ag->isConnected = [this, isShackSwitchTarget]() {
            return m_ag->isConnected() && !isShackSwitchTarget();
        };
        ag->isConnecting = [this, isShackSwitchTarget]() {
            return m_ag->isConnecting() && !isShackSwitchTarget();
        };
        ag->disconnectNow = [this]() { m_ag->disconnectFromDevice(); };
        ag->connectTo = [this](const QString& host, quint16 port) { m_ag->connectToAddress(host, port); };
        ag->applyCode = [this](const QString& code) { m_ag->setAuthCode(code); };
        ag->clearCode = [this, isShackSwitchTarget]() {
            // The shared model may currently be serving ShackSwitch. Clearing
            // AG's saved code must not reset that other target's auth block.
            if (!isShackSwitchTarget()) {
                m_ag->setAuthCode(QString());
            }
        };
        ag->peerAddress = [this]() { return m_ag->peerAddress(); };
        ag->peerPort = [this]() { return m_ag->peerPort(); };
        ag->liveEndpoint = [this, isShackSwitchTarget]() {
            return m_ag->isConnected() && !isShackSwitchTarget()
                ? PeripheralAuthStore::endpoint(m_ag->attemptHost(), m_ag->peerAddress(),
                                                m_ag->peerPort())
                : QString();
        };
        ag->afterRemoval = [this, isShackSwitchTarget]() {
            if (!isShackSwitchTarget()) {
                // Already disconnected before deletion. Repeating it here would
                // cancel an unrelated ShackSwitch request deferred during it.
                m_ag->setAuthCode({});
            }
        };
        buildAuthNetworkDevice(*ag, detailStack, refresh);

        // ShackSwitch has no code field of its own: the credential is the
        // shared model's, removed through the Antenna Genius guard.
        auto ss = addDevice(QStringLiteral("shackswitch"), tr("ShackSwitch"), QStringLiteral("ShackSwitch"),
                            PeripheralDeviceUi::Kind::AuthNetwork, kShackSwitchControlPort);
        ss->isShackSwitch = true;
        ss->sharedAgModel = true;
        ss->endpointScopedCredential = true;
        ss->ipKey = QStringLiteral("SS_ManualIp");
        ss->portKey = QStringLiteral("SS_ControlPort");
        ss->authDevice = PeripheralAuthStore::Device::AntennaGenius;
        ss->isConnected = [this, isShackSwitchTarget]() {
            return m_ag->isConnected() && isShackSwitchTarget();
        };
        ss->isConnecting = [this, isShackSwitchTarget]() {
            return m_ag->isConnecting() && isShackSwitchTarget();
        };
        ss->disconnectNow = [this]() { m_ag->disconnectFromDevice(); };
        ss->connectTo = [this](const QString& ip, quint16 /*port*/) {
            // Always connect on port 9007 (AG control protocol)
            AgDeviceInfo info;
            info.ip     = QHostAddress(ip);
            info.port   = kShackSwitchControlPort;
            info.serial = QStringLiteral("ShackSwitch-manual");
            info.name   = QStringLiteral("ShackSwitch");
            m_ag->resetAuthBudgetFor(info);
            m_ag->connectToDevice(info);
        };
        ss->peerAddress = [this]() { return m_ag->peerAddress(); };
        ss->peerPort = []() { return kShackSwitchControlPort; };
        ss->afterRemoval = [this, isShackSwitchTarget]() {
            if (isShackSwitchTarget()) {
                m_ag->setAuthCode({});
            }
        };
        ss->openWebUi = [this, isShackSwitchTarget]() {
            auto& s = AppSettings::instance();
            QString ip = s.value("SS_ManualIp", "").toString();
            // Only use the live address if the connected device is actually the ShackSwitch.
            if (ip.isEmpty() && m_ag->isConnected() && isShackSwitchTarget()) {
                ip = m_ag->peerAddress();
            }
            if (ip.isEmpty()) {
                return;
            }
            // Use the beacon's web port only when it advertises a valid one (>1024).
            int port = 0;
            if (m_ag->isConnected()) {
                const auto& dev = m_ag->connectedDevice();
                if (AntennaGeniusModel::isShackSwitch(dev) && dev.webPort > 1024) {
                    port = dev.webPort;
                }
            }
            if (port <= 1024) {
                port = s.value("SS_WebPort", "5000").toInt();
            }
            if (port <= 1024) {
                port = 5000;
            }
            QDesktopServices::openUrl(QUrl("http://" + ip + ":" + QString::number(port) + "/"));
        };
        buildAuthNetworkDevice(*ss, detailStack, refresh);
    }

    // Serial-or-network devices share one builder; only their facts differ.
    auto serialDevice = [&](const QString& id, const QString& label, const QString& shortName,
                            const QString& group, int defaultPort, const QString& baud,
                            const QString& placeholder, const QString& description,
                            const QString& tooltip) {
        auto ui = addDevice(id, label, shortName, PeripheralDeviceUi::Kind::SerialOrNetwork, defaultPort);
        ui->settingsGroup = group;
        ui->fixedBaud = baud;
        ui->networkPlaceholder = placeholder;
        ui->networkDescription = description;
        ui->networkTooltip = tooltip;
        return ui;
    };
    if (m_acom) {
        auto ui = serialDevice(QStringLiteral("acom"), tr("ACOM Amplifier"), QStringLiteral("ACOM amplifier"),
            QStringLiteral("Acom"), 7000, QStringLiteral("9600 8N1"),
            QStringLiteral("ser2net host, raw mode — e.g. 192.168.1.52"),
            tr("IP address or host name of the raw-mode serial proxy"), QString());
        ui->isConnected = [this]() { return m_acom->isConnected(); };
        ui->disconnectNow = [this]() { m_acom->disconnect(); };
        ui->connectNetwork = [this](const QString& h, quint16 p) { m_acom->connectNetwork(h, p); };
        ui->connectSerial = [this](const QString& port) { m_acom->connectSerial(port); };
        ui->description = [this]() { return m_acom->description(); };
        ui->afterRemoval = [this]() { m_acom->disconnect(); };
        buildSerialNetworkDevice(*ui, detailStack, refresh, serialReseeds);
    }
    if (m_spe) {
        // Network mode is a ser2net proxy. Raw and telnet modes both work for
        // monitoring and control, but powering the amplifier ON over the
        // network drives the proxy's DTR/RTS lines via RFC 2217, so that one
        // feature needs `telnet(rfc2217=true)`. Surface the reference config
        // where the operator is already looking.
        const QString ser2netTip = QStringLiteral(
            "Network mode connects through a ser2net serial-to-TCP proxy.\n"
            "Monitoring and control work with the port in raw or telnet mode.\n"
            "Powering the amplifier ON over the network additionally needs\n"
            "RFC 2217 COM-port control, i.e. an rfc2217-enabled telnet port:\n"
            "\n"
            "connection: &spe\n"
            "    accepter: telnet(rfc2217=true),64002\n"
            "    enable: on\n"
            "    options:\n"
            "      kickolduser: true\n"
            "    connector: serialdev,\n"
            "              /dev/ttyUSB0");
        auto ui = serialDevice(QStringLiteral("spe"), tr("SPE Expert Amplifier"),
            QStringLiteral("SPE Expert amplifier"), QStringLiteral("SpeExpert"), 7000,
            QStringLiteral("115200 8N1"), QStringLiteral("ser2net host — e.g. 192.168.1.52"),
            tr("IP address or host name of the serial proxy"), ser2netTip);
        ui->isConnected = [this]() { return m_spe->isConnected(); };
        ui->disconnectNow = [this]() { m_spe->disconnect(); };
        ui->connectNetwork = [this](const QString& h, quint16 p) { m_spe->connectNetwork(h, p); };
        ui->connectSerial = [this](const QString& port) { m_spe->connectSerial(port); };
        ui->description = [this]() { return m_spe->description(); };
        ui->afterRemoval = [this]() { m_spe->disconnect(); };
        buildSerialNetworkDevice(*ui, detailStack, refresh, serialReseeds);
    }
    if (m_vkamp) {
        // TCP control/status only for v1 (docs/architecture/vkamp-amplifier-design.md
        // section 3.3 and 9: serial is a different wire format, deferred), so
        // this is a plain host/port pair with no Serial/Network toggle.
        auto ui = addDevice(QStringLiteral("vkamp"), tr("VK3AMP Amplifier"), QStringLiteral("VK3AMP amplifier"),
                            PeripheralDeviceUi::Kind::Vkamp, 5005);
        ui->settingsGroup = QStringLiteral("Vkamp");
        ui->isConnected = [this]() { return m_vkamp->isConnected(); };
        ui->disconnectNow = [this]() { m_vkamp->disconnect(); };
        ui->connectNetwork = [this](const QString& h, quint16 p) { m_vkamp->connectNetwork(h, p); };
        ui->description = [this]() { return m_vkamp->description(); };
        ui->afterRemoval = [this]() { m_vkamp->disconnect(); };
        buildVkampDevice(*ui, detailStack, refresh);
    }
    if (m_lpMeter) {
        auto ui = serialDevice(QStringLiteral("lp100a"), tr("LP-100A Meter"), QStringLiteral("LP-100A meter"),
            QStringLiteral("Lp100a"), 2000, QStringLiteral("115200 8N1"),
            QStringLiteral("ser2net host, raw mode — e.g. 192.168.1.7"),
            tr("IP address or host name of the raw-mode serial proxy"), QString());
        // 2000 rather than 7000: ser2net's own common default.
        ui->isConnected = [this]() { return m_lpMeter->isConnected(); };
        ui->disconnectNow = [this]() { m_lpMeter->disconnect(); };
        ui->connectNetwork = [this](const QString& h, quint16 p) { m_lpMeter->connectNetwork(h, p); };
        ui->connectSerial = [this](const QString& port) { m_lpMeter->connectSerial(port); };
        ui->description = [this]() { return m_lpMeter->description(); };
        ui->afterRemoval = [this]() { m_lpMeter->disconnect(); };
        buildSerialNetworkDevice(*ui, detailStack, refresh, serialReseeds);
    }
    // ── Reconnect checkbox ────────────────────────────────────────────────
    auto* reconnectCheck = new QCheckBox(tr("Reconnect automatically"));
    reconnectCheck->setObjectName(QStringLiteral("peripheralAutoReconnect"));
    reconnectCheck->setAccessibleDescription(tr("Reconnect peripherals after a connection drops."));
    ThemeManager::instance().applyStyleSheet(reconnectCheck,
        "QCheckBox { color: {{color.text.primary}}; font-size: 11px; spacing: 8px; }"
        + kCheckBoxIndicator);
    reconnectCheck->setChecked(PeripheralSettings::autoReconnect());
    connect(reconnectCheck, &QCheckBox::toggled, this, [this](bool on) {
        PeripheralSettings::setAutoReconnect(on);
        // Propagate immediately to live connection objects. Each network device
        // also checks its own "Connect automatically" when it decides to retry.
        if (m_tgxl) {
            m_tgxl->setAutoReconnect(on);
        }
        if (m_pgxl) {
            m_pgxl->setAutoReconnect(on);
        }
        if (m_ag) {
            m_ag->setAutoReconnect(on);
        }
        if (m_acom) {
            m_acom->setAutoReconnect(on);
        }
        if (m_spe) {
            m_spe->setAutoReconnect(on);
        }
        if (m_lpMeter) {
            m_lpMeter->setAutoReconnect(on);
        }
        // NOTE: m_vkamp is deliberately NOT propagated here. That is a
        // pre-existing gap from #4919, not an intentional omission:
        // VkampConnection has setAutoReconnect() and the startup block in
        // MainWindow_Wiring.cpp calls it, so toggling this mid-session reaches
        // every peripheral except that one. It is left alone because it is not
        // this feature's to change and needs its own one-line fix.
    });

    // ── Device list, Add and Remove ───────────────────────────────────────
    auto* listGroup = new QGroupBox(tr("Devices"), content);
    listGroup->setMinimumWidth(250);
    listGroup->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    ThemeManager::instance().applyStyleSheet(listGroup, kGroupBoxStyle);
    auto* listLayout = new QVBoxLayout(listGroup);
    auto* deviceList = new QListWidget(listGroup);
    deviceList->setObjectName(QStringLiteral("peripheralDeviceList"));
    deviceList->setAccessibleName(tr("Configured peripherals"));
    deviceList->setAlternatingRowColors(true);
    deviceList->setSpacing(3);
    ThemeManager::instance().applyStyleSheet(deviceList,
        "QListWidget { background: {{color.background.0}}; alternate-background-color: {{color.background.0}}; "
        "color: {{color.text.primary}}; "
        "border: 1px solid {{color.background.1}}; font-size: 11px; }"
        "QListWidget::item { background: {{color.background.1}}; padding: 8px 7px; margin-right: 6px; "
        "border: 1px solid {{color.background.2}}; border-radius: 0px; }"
        "QListWidget::item:alternate { background: {{color.background.2}}; }"
        "QListWidget::item:selected, QListWidget::item:alternate:selected { background: {{color.accent}}; "
        "color: {{color.background.0}}; }");
    deviceList->setMinimumHeight(200);
    deviceList->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    listLayout->addWidget(deviceList);

    const QString actionStyle =
        "QPushButton { background: {{color.background.1}}; "
        "border: 1px solid {{color.background.2}}; border-radius: 3px; "
        "color: {{color.text.primary}}; font-size: 11px; font-weight: bold; "
        "padding: 5px 8px; }"
        "QPushButton:hover { background: {{color.background.2}}; }";
    auto* listActions = new QHBoxLayout;
    auto* addButton = new QPushButton(tr("Add"), listGroup);
    addButton->setObjectName(QStringLiteral("peripheralAddButton"));
    addButton->setAccessibleName(tr("Add peripheral device"));
    addButton->setAccessibleDescription(tr("Choose a device type to configure"));
    ThemeManager::instance().applyStyleSheet(addButton, actionStyle);
    auto* addMenu = new QMenu(addButton);
    ThemeManager::instance().applyStyleSheet(addMenu,
        "QMenu::item:disabled { color: {{color.text.disabled}}; }");
    addButton->setMenu(addMenu);
    auto* removeButton = new QPushButton(tr("Remove"), listGroup);
    removeButton->setObjectName(QStringLiteral("peripheralRemoveButton"));
    removeButton->setAccessibleName(tr("Remove selected peripheral device"));
    removeButton->setAccessibleDescription(tr("Select a configured peripheral to remove it"));
    removeButton->setToolTip(tr("Remove this device and clear its saved connection details"));
    removeButton->setEnabled(false);
    ThemeManager::instance().applyStyleSheet(removeButton, actionStyle);
    listActions->addWidget(addButton, 1);
    listActions->addWidget(removeButton, 1);
    listLayout->addLayout(listActions);
    auto* removalNotice = new QLabel(listGroup);
    removalNotice->setObjectName(QStringLiteral("peripheralRemovalNotice"));
    removalNotice->setAccessibleName(tr("Peripheral removal status"));
    removalNotice->setWordWrap(true);
    removalNotice->hide();
    ThemeManager::instance().applyStyleSheet(removalNotice,
        "QLabel { color: {{color.accent.warning}}; font-size: 11px; }");
    listLayout->addWidget(removalNotice);
    listLayout->addSpacing(8);
    listLayout->addWidget(reconnectCheck);
    listLayout->addSpacing(8);
    auto* helpButton = new QPushButton(tr("Connection Help"), listGroup);
    helpButton->setObjectName(QStringLiteral("peripheralConnectionHelp"));
    helpButton->setAutoDefault(false);
    helpButton->setAccessibleDescription(tr("Explain local and remote peripheral connections."));
    ThemeManager::instance().applyStyleSheet(helpButton, actionStyle);
    connect(helpButton, &QPushButton::clicked, this, [this]() {
        QMessageBox::information(this, tr("Peripheral connections"),
            tr("Add a device and enter its address for a direct connection. "
               "Use this for devices that are not discovered on your local network, "
               "including remote, VPN, and SmartLink setups.\n\n"
               "Configured devices connect when the radio connects. "
               "Reconnect automatically also retries after a connection drops. "
               "Turn off Connect automatically on a device to connect it only when you "
               "click Connect.\n\n"
               "If a device requests authorization, enter its code. "
               "An accepted code is saved securely when a credential store is available. "
               "Disconnect before changing the connection address or port."));
    });
    listLayout->addWidget(helpButton);
    columns->addWidget(listGroup, 1);
    columns->addWidget(detailStack, 2);
    vbox->insertWidget(0, content);

    // A saved code is concealed again whenever another page is selected.
    connect(detailStack, &QStackedWidget::currentChanged, detailStack, [detailStack]() {
        for (QLineEdit* edit : detailStack->findChildren<QLineEdit*>()) {
            if (!edit->property("aetherSensitiveValue").toBool()) {
                continue;
            }
            edit->setProperty("authRevealGeneration",
                edit->property("authRevealGeneration").toULongLong() + 1);
            if (edit->property("peripheralSavedCodeRevealed").toBool()) {
                edit->setProperty("peripheralSavedCodeRevealed", false);
                edit->clear();
            }
            edit->setEchoMode(QLineEdit::Password);
            QString showName = edit->objectName();
            showName.chop(1);
            if (QPushButton* show = detailStack->findChild<QPushButton*>(showName + QLatin1Char('5'))) {
                show->setText(RadioSetupDialog::tr("Show"));
                show->setAccessibleName(RadioSetupDialog::tr("%1 show authorization code")
                    .arg(edit->property("authDeviceLabel").toString()));
            }
        }
    });

    // Pages by device id, so the list can select one.
    auto pageIndex = std::make_shared<QHash<QString, int>>();
    for (const auto& ui : m_peripheralDevices) {
        pageIndex->insert(ui->id, detailStack->addWidget(ui->page));
    }

    // ── Which devices are listed ──────────────────────────────────────────
    // activeIds: what the list shows. configuredIds: what is persisted. Recovery
    // rows (a blocked device the radio reported) may be in the first only.
    auto activeIds = std::make_shared<QStringList>();
    if (const std::optional<QStringList> savedIds = PeripheralSettings::visibleDeviceIds()) {
        *activeIds = *savedIds;
    }
    {
        // Other entry points (notably the AG applet) also configure targets.
        // A saved list must not hide a real manual connection.
        auto configuredTarget = [&settings](const PeripheralDeviceUi& ui) {
            if (ui.kind == PeripheralDeviceUi::Kind::AuthNetwork) {
                return !settings.value(ui.ipKey, "").toString().isEmpty();
            }
            return !PeripheralSettings::deviceString(ui.settingsGroup, "ManualIp").isEmpty()
                || !PeripheralSettings::deviceString(ui.settingsGroup, "SerialPort").isEmpty();
        };
        for (const auto& ui : m_peripheralDevices) {
            if (configuredTarget(*ui) && !activeIds->contains(ui->id)) {
                activeIds->append(ui->id);
            }
        }
    }
    auto configuredIds = std::make_shared<QStringList>(*activeIds);

    auto refreshList = [deviceList, this, activeIds]() {
        const QString current = deviceList->currentItem()
            ? deviceList->currentItem()->data(Qt::UserRole).toString() : QString();
        deviceList->clear();
        for (const auto& ui : m_peripheralDevices) {
            if (!activeIds->contains(ui->id)) {
                continue;
            }
            auto* item = new QListWidgetItem(ui->label, deviceList);
            item->setData(Qt::UserRole, ui->id);
            if (ui->id == current) {
                deviceList->setCurrentItem(item);
            }
        }
    };
    auto findItem = [deviceList](const QString& id) -> QListWidgetItem* {
        for (int i = 0; i < deviceList->count(); ++i) {
            QListWidgetItem* item = deviceList->item(i);
            if (item->data(Qt::UserRole).toString() == id) {
                return item;
            }
        }
        return nullptr;
    };
    connect(deviceList, &QListWidget::currentItemChanged, this,
            [detailStack, pageIndex, removeButton](QListWidgetItem* item) {
        removeButton->setEnabled(item != nullptr);
        removeButton->setAccessibleDescription(item
            ? RadioSetupDialog::tr("Removes the device from this list and clears its saved connection details")
            : RadioSetupDialog::tr("Select a configured peripheral to remove it"));
        detailStack->setCurrentIndex(
            item ? pageIndex->value(item->data(Qt::UserRole).toString(), 0) : 0);
    });

    // Add menu: an entry for a device already in the list is disabled, and says
    // why in its text so it is shown and spoken, not only in a status tip.
    for (const auto& ui : m_peripheralDevices) {
        QAction* action = addMenu->addAction(ui->label);
        action->setData(ui->id);
        action->setProperty("peripheralBaseText", ui->label);
    }
    auto updateAddMenu = [addMenu, configuredIds, addButton]() {
        bool available = false;
        for (QAction* action : addMenu->actions()) {
            const bool canAdd = !configuredIds->contains(action->data().toString());
            const QString base = action->property("peripheralBaseText").toString();
            action->setEnabled(canAdd);
            action->setText(canAdd ? base : RadioSetupDialog::tr("%1 (already added)").arg(base));
            action->setStatusTip(canAdd ? QString() : RadioSetupDialog::tr("Already added"));
            available |= canAdd;
        }
        addButton->setEnabled(available);
    };
    // Add creates the row and selects it. Nothing else changes.
    for (QAction* action : addMenu->actions()) {
        const QString id = action->data().toString();
        connect(action, &QAction::triggered, this,
                [id, activeIds, configuredIds, refreshList, findItem, updateAddMenu]() {
            if (!configuredIds->contains(id)) {
                configuredIds->append(id);
                PeripheralSettings::setVisibleDeviceIds(*configuredIds);
            }
            if (!activeIds->contains(id)) {
                activeIds->append(id);
                refreshList();
            }
            if (QListWidgetItem* item = findItem(id)) {
                item->listWidget()->setCurrentItem(item);
            }
            updateAddMenu();
        });
    }
    // A deliberate manual connection also promotes a recovery row.
    for (const auto& ui : m_peripheralDevices) {
        if (ui->id != QStringLiteral("tgxl") && ui->id != QStringLiteral("pgxl")) {
            continue;
        }
        connect(ui->connectButton, &QPushButton::clicked, this,
                [id = ui->id, key = ui->ipKey, configuredIds]() {
            if (!configuredIds->contains(id)
                && !AppSettings::instance().value(key, QString()).toString().isEmpty()) {
                configuredIds->append(id);
                PeripheralSettings::setVisibleDeviceIds(*configuredIds);
            }
        });
    }
    connect(addMenu, &QMenu::aboutToShow, this, updateAddMenu);

    // Remove: reset the device's fields and settings, drop the row.
    auto finishRemoval = [this, activeIds, configuredIds, deviceList, refreshList, updateAddMenu,
                          &settings](PeripheralDeviceUi& ui) {
        if (ui.kind == PeripheralDeviceUi::Kind::AuthNetwork) {
            // Remove resets the device to its defaults, "Connect automatically"
            // included; a hidden row never keeps a silent "do not connect".
            PeripheralSettings::resetAutoConnect(ui.id);
            if (ui.autoConnectCheck) {
                const QSignalBlocker blocked(ui.autoConnectCheck);
                ui.autoConnectCheck->setChecked(true);
            }
            settings.remove(ui.ipKey);
            settings.remove(ui.portKey);
            settings.save();
        } else {
            PeripheralSettings::clearDeviceConnection(ui.settingsGroup);
        }
        if (ui.afterRemoval) {
            ui.afterRemoval();
        }
        resetFields(ui);
        activeIds->removeAll(ui.id);
        configuredIds->removeAll(ui.id);
        PeripheralSettings::setVisibleDeviceIds(*configuredIds);
        refreshList();
        updateAddMenu();
        if (deviceList->currentRow() < 0 && deviceList->count() > 0) {
            deviceList->setCurrentRow(0);
        }
        emit peripheralRemoved(ui.id);
    };
    connect(removeButton, &QPushButton::clicked, this,
            [this, deviceList, finishRemoval, removalNotice, content, refresh]() {
        if (m_peripheralRemovalPending) {
            return;
        }
        QListWidgetItem* item = deviceList->currentItem();
        if (!item) {
            return;
        }
        PeripheralDeviceUi* const ui = peripheralDevice(item->data(Qt::UserRole).toString());
        if (!ui) {
            return;
        }
        removalNotice->hide();
        if (ui->kind != PeripheralDeviceUi::Kind::AuthNetwork) {
            if (confirmPeripheralRemoval(ui->label)) {
                finishRemoval(*ui);
            }
            return;
        }
        if (PeripheralRemovalGuard::pending(ui->authDevice)) {
            showRemovalNotice(removalNotice, tr(
                "%1 credential deletion from an earlier Remove is still pending. "
                "Retry after the keychain request finishes; restart the app if it stays stuck.")
                .arg(ui->label));
            return;
        }
        if (!confirmPeripheralRemoval(ui->label)) {
            return;
        }
        // Acquire before disconnect: model signals can synchronously request
        // another manual or discovered connection during teardown.
        auto removal = std::make_shared<PeripheralRemovalGuard>(ui->authDevice);
        m_peripheralRemovalPending = true;
        QString removalEndpoint;
        const bool sharedTargetSelected = ui->sharedAgModel
            && AntennaGeniusModel::isShackSwitch(m_ag->connectedDevice()) == ui->isShackSwitch;
        if (ui->sharedAgModel) {
            // The ShackSwitch connect path ignores the saved port and saves its
            // credential under the fixed control port.
            const quint16 endpointPort = ui->isShackSwitch ? kShackSwitchControlPort
                : static_cast<quint16>(AppSettings::instance().value(ui->portKey, 9007).toUInt());
            removalEndpoint = PeripheralAuthStore::configuredEndpoint(
                AppSettings::instance().value(ui->ipKey).toString(), endpointPort);
            if (sharedTargetSelected) {
                const QString peer = PeripheralAuthStore::endpoint(
                    m_ag->attemptHost(), m_ag->peerAddress(), m_ag->peerPort());
                if (!peer.isEmpty()) {
                    removalEndpoint = peer;
                }
            }
        }
        if (!ui->sharedAgModel) {
            ui->disconnectNow();
        } else if (sharedTargetSelected) {
            m_ag->disconnectFromDevice();
        }
        const QString pendingMessage = tr("Removing %1. Wait for credential deletion before closing Setup.")
            .arg(ui->label);
        showRemovalNotice(removalNotice, pendingMessage);
        content->setEnabled(false);
        content->setAccessibleDescription(pendingMessage);
        // Bound the modal wait, not the vault operation: QtKeychain cannot
        // promise cancellation. Keep its reconnect lease until completion, but
        // never apply a late Remove to newer settings.
        auto timedOut = std::make_shared<bool>(false);
        auto* deadline = new QTimer(this);
        deadline->setObjectName(QStringLiteral("peripheralRemovalDeadline"));
        deadline->setSingleShot(true);
        constexpr int kRemovalWaitMs = 15000;
        connect(deadline, &QTimer::timeout, this,
                [this, timedOut, ui, removalNotice, content]() {
            *timedOut = true;
            m_peripheralRemovalPending = false;
            // Suppress only the abandoned row's clear-on-close edit. Other rows
            // may contain deliberate clears made before Remove.
            if (ui->addressEdit) {
                ui->addressEdit->setModified(false);
            }
            const QString message = tr(
                "%1 removal timed out; stored-code deletion is unconfirmed. "
                "Configuration was kept. You can close Setup. Reconnection remains "
                "blocked until the keychain request finishes; restart the app if it stays stuck.")
                .arg(ui->label);
            showRemovalNotice(removalNotice, message);
            content->setAccessibleDescription(message);
        });
        deadline->start(kRemovalWaitMs);
        const auto completed = [self = QPointer<RadioSetupDialog>(this), removal, ui, finishRemoval,
                                timedOut, deadline = QPointer<QTimer>(deadline), removalNotice,
                                content, refresh](PeripheralAuthStore::ClearResult result) {
            if (deadline) {
                deadline->stop();
                deadline->deleteLater();
            }
            if (!self || *timedOut) {
                return; // Release the transient guard even after owner teardown.
            }
            content->setEnabled(true);
            content->setAccessibleDescription(QString());
            removalNotice->hide();
            if (result == PeripheralAuthStore::ClearResult::Cleared
                || result == PeripheralAuthStore::ClearResult::SessionCleared) {
                finishRemoval(*ui);
                if (result == PeripheralAuthStore::ClearResult::SessionCleared) {
                    showRemovalNotice(removalNotice, RadioSetupDialog::tr(
                        "%1 removed. Keychain unavailable; stored-code deletion unconfirmed.")
                        .arg(ui->label));
                }
            } else {
                // Connection state may change independently after failure. Keep
                // the failed operation visible until another Remove.
                const bool unknownOwner = result == PeripheralAuthStore::ClearResult::UnknownOwner;
                const QString message = unknownOwner ? RadioSetupDialog::tr(
                    "%1 removal stopped: credential ownership is unknown. Connect this device "
                    "to establish its peer address, then retry Remove. Configuration was kept.").arg(ui->label)
                    : RadioSetupDialog::tr(
                    "%1 removal failed: saved code remains in keychain. The connection was stopped; "
                    "normal reconnect events may connect it again. Retry Remove.").arg(ui->label);
                showRemovalNotice(removalNotice, message);
                ui->state.attention = Attention::CredentialError;
                ui->state.message = unknownOwner ? message
                    : RadioSetupDialog::tr("Saved code remains in keychain; retry Remove");
                refresh();
            }
            self->m_peripheralRemovalPending = false;
        };
        if (ui->endpointScopedCredential) {
            PeripheralAuthStore::clearForEndpoint(ui->authDevice, removalEndpoint, qApp, completed);
        } else {
            PeripheralAuthStore::clear(ui->authDevice, qApp, completed);
        }
    });

    // ── Presentation ──────────────────────────────────────────────────────
    // From connection state and cached credential metadata only. No socket or
    // keychain request is made by a refresh.
    auto updateFieldAvailability = [](PeripheralDeviceUi& ui, QWidget* field, bool connected,
                                      const QString& reason) {
        if (!field) {
            return;
        }
        const auto help = ui.fieldHelp.value(field);
        field->setEnabled(!connected);
        field->setAccessibleDescription(help.first + (connected
            ? (help.first.isEmpty() ? QString() : QStringLiteral(" ")) + reason : QString()));
        field->setToolTip(help.second + (connected
            ? (help.second.isEmpty() ? QString() : QStringLiteral("\n")) + reason : QString()));
    };
    refreshState->body = [this, deviceList, activeIds, configuredIds, refreshList, updateAddMenu,
                          updateFieldAvailability, &settings]() {
        bool addedRow = false;
        // The AG applet can configure a target while Setup remains open.
        if (PeripheralDeviceUi* ag = peripheralDevice(QStringLiteral("ag"))) {
            const QString agHost = settings.value("AG_ManualIp", QString()).toString().trimmed();
            if (!agHost.isEmpty()) {
                if (!configuredIds->contains(ag->id)) {
                    configuredIds->append(ag->id);
                    PeripheralSettings::setVisibleDeviceIds(*configuredIds);
                }
                if (!activeIds->contains(ag->id)) {
                    activeIds->append(ag->id);
                    addedRow = true;
                }
                // Programmatic pre-fills are not edits. Replace a stale discovered
                // endpoint, but preserve address or port edits in progress.
                if (!ag->addressEdit->isModified()
                    && ag->portSpin->value() == ag->addressEdit->property("peripheralPrefillPort").toInt()) {
                    const int savedPort = settings.value("AG_ManualPort", 9007).toInt();
                    if (ag->addressEdit->text() != agHost || ag->portSpin->value() != savedPort) {
                        ag->addressEdit->setText(agHost);
                        ag->portSpin->setValue(savedPort);
                    }
                    ag->addressEdit->setProperty("peripheralPrefillPort", ag->portSpin->value());
                    ag->addressEdit->setProperty("peripheralSavedHost", agHost);
                    ag->addressEdit->setProperty("peripheralSavedPort",
                        settings.value("AG_ManualPort", QString()).toString());
                }
            }
        }
        // A blocked TGXL or PGXL gets its recovery controls even with an
        // explicitly empty saved list, without persisting discovery as manual
        // connection intent.
        for (const auto& ui : m_peripheralDevices) {
            const bool tgxl = ui->id == QStringLiteral("tgxl");
            const bool pgxl = ui->id == QStringLiteral("pgxl");
            if (!tgxl && !pgxl) {
                continue;
            }
            const bool blocked = tgxl ? m_tgxl->isAuthBlocked() : m_pgxl->isAuthBlocked();
            const QString host = tgxl ? m_tgxl->reconnectHost() : m_pgxl->reconnectHost();
            if (!blocked || host.isEmpty()) {
                continue;
            }
            if (!activeIds->contains(ui->id)) {
                activeIds->append(ui->id);
                addedRow = true;
            }
            if (ui->addressEdit->text().isEmpty() && !ui->addressEdit->isModified()) {
                ui->addressEdit->setText(host);
                ui->portSpin->setValue(tgxl ? m_tgxl->reconnectPort() : m_pgxl->reconnectPort());
            }
            ui->addressEdit->setProperty("peripheralDiscoveredHost", ui->discoveredHost());
            ui->addressEdit->setProperty("peripheralDiscoveredPort", ui->discoveredPort);
            if (!ui->state.needsAttention()) {
                ui->state.attention = Attention::AuthBlocked;
                ui->state.message = tr("Authorization blocked; enter a code and click Connect to retry");
            }
        }
        if (addedRow) {
            refreshList();
            updateAddMenu();
            if (deviceList->currentRow() < 0 && deviceList->count() > 0) {
                deviceList->setCurrentRow(0);
            }
        }

        for (const auto& uiPtr : m_peripheralDevices) {
            PeripheralDeviceUi& ui = *uiPtr;
            const bool connected = ui.isConnected();
            ui.state.source = sourceOf(ui);
            const bool connecting = effectivelyConnecting(ui);
            ui.connectButton->setText(connected ? QStringLiteral("Disconnect")
                                                : QStringLiteral("Connect"));
            applyStatusLabel(ui);

            const QString reason = connected
                ? tr("Disconnect to change connection settings.") : QString();
            updateFieldAvailability(ui, ui.addressEdit, connected, reason);
            updateFieldAvailability(ui, ui.addressStack, connected, reason);
            updateFieldAvailability(ui, ui.portSpin, connected, reason);
            updateFieldAvailability(ui, ui.portStack, connected, reason);
            updateFieldAvailability(ui, ui.modeCombo, connected, reason);
            if (ui.editHint) {
                ui.editHint->setText(reason);
                ui.editHint->setVisible(connected);
            }
            if (ui.serialRefreshButton) {
                ui.serialRefreshButton->setEnabled(!connected);
                ui.serialRefreshButton->setAccessibleDescription(reason);
                ui.serialRefreshButton->setToolTip(reason);
            }

            // List line: name, address, where the data comes from, and whether
            // the device needs the operator.
            const auto source = PeripheralConnectionSource::describe(
                ui.state.source, ui.shortName, !ui.radioRelay);
            const QString address = ui.addressText().isEmpty() ? tr("Address not set") : ui.addressText();
            QString stateWord = connecting ? tr("Connecting…") : source.indicator;
            QString spokenWord = connecting ? tr("Connecting") : source.word;
            if (ui.state.needsAttention()) {
                stateWord += QStringLiteral(" — ") + tr("Needs attention");
                spokenWord += QStringLiteral(", ") + tr("Needs attention");
            }
            for (int index = 0; index < deviceList->count(); ++index) {
                QListWidgetItem* item = deviceList->item(index);
                if (item->data(Qt::UserRole).toString() != ui.id) {
                    continue;
                }
                item->setText(ui.label + QLatin1Char('\n') + address + QLatin1Char('\n') + stateWord);
                item->setToolTip(address + QLatin1Char('\n') + statusText(ui));
                item->setData(Qt::AccessibleTextRole,
                              ui.label + QStringLiteral(": ") + address + QStringLiteral(", ") + spokenWord);
                item->setData(Qt::AccessibleDescriptionRole, statusText(ui));
            }

            if (!ui.hasAuthCode) {
                continue;
            }
            // The code field's hint follows what the credential store has cached
            // for this device's current endpoint.
            const QString live = ui.liveEndpoint ? ui.liveEndpoint() : QString();
            const QString endpoint = connected && !live.isEmpty()
                ? live
                : PeripheralAuthStore::configuredEndpoint(
                      ui.addressEdit->text(), static_cast<quint16>(ui.portSpin->value()));
            QLineEdit* edit = ui.codeEdit;
            if (edit->property("peripheralSavedCodeRevealed").toBool()
                && edit->property("authRevealedEndpoint").toString() != endpoint) {
                edit->setProperty("peripheralSavedCodeRevealed", false);
                edit->clear();
                edit->setEchoMode(QLineEdit::Password);
            }
            const bool revealed = edit->echoMode() == QLineEdit::Normal;
            ui.showButton->setText(revealed ? tr("Hide") : tr("Show"));
            ui.showButton->setAccessibleName(revealed
                ? tr("%1 hide authorization code").arg(ui.label)
                : tr("%1 show authorization code").arg(ui.label));
            const auto saved = PeripheralAuthStore::cachedStatus(ui.authDevice, endpoint);
            const bool hasCode = saved && saved->status == PeripheralAuthStore::LoadStatus::Found;
            edit->setPlaceholderText(hasCode ? QStringLiteral("****") : tr("Code blank"));
            const QString blankDescription = hasCode
                ? tr("A code is available for this address. Leave blank to reuse it, or enter a replacement code.")
                : !saved ? tr("Code blank. A saved code will be checked when connecting.")
                : saved->status == PeripheralAuthStore::LoadStatus::Unavailable
                    ? tr("Code blank. The saved code is unavailable.")
                    : tr("Code blank. Enter a code if the device requires authorization.");
            edit->setProperty("authBlankDescription", blankDescription);
            edit->setAccessibleDescription(edit->text().isEmpty() ? blankDescription
                : tr("A code has been entered. It replaces the saved code only after the device accepts it."));
            edit->setToolTip(edit->accessibleDescription());
        }
    };

    // ── What triggers a refresh ───────────────────────────────────────────
    // Connection and model signals, the operator's own field edits, and the
    // page being shown. Each connection also updates its status first.
    auto onLink = [this, refresh](const QString& id) {
        if (PeripheralDeviceUi* ui = peripheralDevice(id)) {
            linkChanged(*ui);
        }
        refresh();
    };
    auto onFailure = [this, refresh](const QString& id, const QString& error, bool blocked) {
        if (PeripheralDeviceUi* ui = peripheralDevice(id)) {
            connectionFailed(*ui, error, blocked);
        }
        refresh();
    };
    auto onAccepted = [this, refresh](const QString& id) {
        if (PeripheralDeviceUi* ui = peripheralDevice(id)) {
            codeAccepted(*ui);
        }
        refresh();
    };
    auto onDiscarded = [this, refresh](const QString& id) {
        if (PeripheralDeviceUi* ui = peripheralDevice(id)) {
            if (codeDiscarded(*ui)) {
                linkChanged(*ui);
            }
        }
        refresh();
    };
    auto onBlockCleared = [this, refresh](const QString& id) {
        if (PeripheralDeviceUi* ui = peripheralDevice(id)) {
            if (ui->state.attention == Attention::AuthBlocked) {
                ui->state.attention = Attention::None;
                ui->state.message.clear();
            }
        }
        refresh();
    };
    if (m_tgxl) {
        connect(m_tgxl, &TgxlConnection::connected, this, [onLink]() { onLink(QStringLiteral("tgxl")); });
        connect(m_tgxl, &TgxlConnection::disconnected, this, [onLink]() { onLink(QStringLiteral("tgxl")); });
        connect(m_tgxl, &TgxlConnection::connectionFailed, this, [this, onFailure](const QString& error) {
            onFailure(QStringLiteral("tgxl"), error, m_tgxl->isAuthBlocked());
        });
        connect(m_tgxl, &TgxlConnection::unreachable, this, refresh);
        connect(m_tgxl, &TgxlConnection::authCodeRequired, this, refresh);
        connect(m_tgxl, &TgxlConnection::authCodeAccepted, this,
                [onAccepted](const QString&) { onAccepted(QStringLiteral("tgxl")); });
        connect(m_tgxl, &TgxlConnection::enteredAuthCodeDiscarded, this,
                [onDiscarded]() { onDiscarded(QStringLiteral("tgxl")); });
        connect(m_tgxl, &TgxlConnection::authBlockCleared, this,
                [onBlockCleared]() { onBlockCleared(QStringLiteral("tgxl")); });
    }
    if (m_pgxl) {
        connect(m_pgxl, &PgxlConnection::connected, this, [onLink]() { onLink(QStringLiteral("pgxl")); });
        connect(m_pgxl, &PgxlConnection::disconnected, this, [onLink]() { onLink(QStringLiteral("pgxl")); });
        connect(m_pgxl, &PgxlConnection::connectionFailed, this, [this, onFailure](const QString& error) {
            onFailure(QStringLiteral("pgxl"), error, m_pgxl->isAuthBlocked());
        });
        connect(m_pgxl, &PgxlConnection::unreachable, this, refresh);
        connect(m_pgxl, &PgxlConnection::authCodeRequired, this, refresh);
        connect(m_pgxl, &PgxlConnection::authCodeAccepted, this,
                [onAccepted](const QString&) { onAccepted(QStringLiteral("pgxl")); });
        connect(m_pgxl, &PgxlConnection::enteredAuthCodeDiscarded, this,
                [onDiscarded]() { onDiscarded(QStringLiteral("pgxl")); });
        connect(m_pgxl, &PgxlConnection::authBlockCleared, this,
                [onBlockCleared]() { onBlockCleared(QStringLiteral("pgxl")); });
    }
    if (m_ag) {
        // Antenna Genius and ShackSwitch share the model; the device it is
        // serving owns the event.
        connect(m_ag, &AntennaGeniusModel::connected, this, [this, onLink]() {
            onLink(QStringLiteral("ag"));
            onLink(QStringLiteral("shackswitch"));
        });
        connect(m_ag, &AntennaGeniusModel::disconnected, this, [this, onLink]() {
            onLink(QStringLiteral("ag"));
            onLink(QStringLiteral("shackswitch"));
        });
        connect(m_ag, &AntennaGeniusModel::enteredAuthCodeDiscarded, this, [this, onDiscarded]() {
            // AG and ShackSwitch share this model. Only AG tracks a typed code.
            if (!AntennaGeniusModel::isShackSwitch(m_ag->connectedDevice())) {
                onDiscarded(QStringLiteral("ag"));
            }
        });
        connect(m_ag, &AntennaGeniusModel::authCodeAccepted, this,
                [onAccepted](const QString&) { onAccepted(QStringLiteral("ag")); });
        connect(m_ag, &AntennaGeniusModel::connectionError, this, [this, onFailure](const QString& error) {
            // m_device retains the attempted device even when TCP never connected.
            onFailure(AntennaGeniusModel::isShackSwitch(m_ag->connectedDevice())
                          ? QStringLiteral("shackswitch") : QStringLiteral("ag"),
                      error, m_ag->isAuthBlocked());
        });
        connect(m_ag, &AntennaGeniusModel::authCodeRequired, this, refresh);
        connect(m_ag, &AntennaGeniusModel::deviceInfoChanged, this, refresh);
        connect(m_ag, &AntennaGeniusModel::presenceChanged, this, refresh);
        connect(m_ag, &AntennaGeniusModel::deviceDiscovered, this, refresh);
        connect(m_ag, &AntennaGeniusModel::deviceLost, this, refresh);
    }
    connect(&m_model->tunerModel(), &TunerModel::presenceChanged, this, refresh);
    connect(&m_model->tunerModel(), &TunerModel::directConnectionChanged, this, refresh);
    connect(&m_model->amplifier(), &AmpModel::presenceChanged, this, refresh);
    connect(m_model, &RadioModel::connectionStateChanged, this, refresh);

    // The serial and VK3AMP devices: link changes, failures, and link-up with a
    // silent device.
    auto wireLinkDevice = [this, onLink, onFailure](const QString& id, auto* connection) {
        using Connection = std::remove_pointer_t<decltype(connection)>;
        connect(connection, &Connection::connected, this, [onLink, id]() { onLink(id); });
        connect(connection, &Connection::disconnected, this, [onLink, id]() { onLink(id); });
        connect(connection, &Connection::connectionFailed, this,
                [onFailure, id](const QString& error) { onFailure(id, error, false); });
    };
    if (m_acom) {
        wireLinkDevice(QStringLiteral("acom"), m_acom);
    }
    if (m_spe) {
        wireLinkDevice(QStringLiteral("spe"), m_spe);
    }
    if (m_vkamp) {
        wireLinkDevice(QStringLiteral("vkamp"), m_vkamp);
    }
    if (m_lpMeter) {
        wireLinkDevice(QStringLiteral("lp100a"), m_lpMeter);
        // Link up but the meter silent is its own state: the operator should
        // tell it apart from "not connected" here as well as in the applet.
        connect(m_lpMeter, &LpMeterConnection::dataFlowingChanged, this, [this, refresh](bool flowing) {
            if (PeripheralDeviceUi* ui = peripheralDevice(QStringLiteral("lp100a"))) {
                if (ui->isConnected()) {
                    ui->state.note = flowing ? QString() : tr("meter not answering");
                }
            }
            refresh();
        });
    }
    // The operator's own edits change the list line.
    for (const auto& ui : m_peripheralDevices) {
        if (ui->addressEdit) {
            connect(ui->addressEdit, &QLineEdit::textChanged, this, refresh);
        }
        if (ui->portSpin) {
            connect(ui->portSpin, &QSpinBox::valueChanged, this, refresh);
        }
        if (ui->addressStack) {
            for (QLineEdit* edit : ui->addressStack->findChildren<QLineEdit*>()) {
                connect(edit, &QLineEdit::textChanged, this, refresh);
            }
            for (QComboBox* combo : ui->addressStack->findChildren<QComboBox*>()) {
                connect(combo, &QComboBox::currentIndexChanged, this, refresh);
            }
        }
        if (ui->modeCombo) {
            connect(ui->modeCombo, &QComboBox::currentIndexChanged, this, refresh);
        }
    }
    connect(deviceList, &QListWidget::currentRowChanged, page, refresh);
    page->installEventFilter(new ShowNotifier(page, refresh));

    refreshList();
    updateAddMenu();
    if (deviceList->count() > 0) {
        deviceList->setCurrentRow(0);
    }
    refresh();

    vbox->addStretch();
    return page;
}

} // namespace AetherSDR
