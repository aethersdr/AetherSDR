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
#include "Kpa500AdvancedWidget.h"
#include "Kxpa100AdvancedWidget.h"
#include "models/Kxpa100Model.h"
#include "models/Kxpa100RadioAssociation.h"
#include "Kat500AdvancedWidget.h"
#include "models/Kat500Model.h"
#include "PeripheralAuthConnectFlow.h"
#include "PeripheralAuthStore.h"
#include "PeripheralConnectionSource.h"
#include "ScopedChildWidget.h"
#include "core/AcomConnection.h"
#include "core/AppSettings.h"
#include "core/Kpa1500Connection.h"
#include "core/Kpa500Connection.h"
#include "core/LpMeterConnection.h"
#include "core/PeripheralRemovalGuard.h"
#include "core/PeripheralSettings.h"
#include "core/PgxlConnection.h"
#include "core/RfkitConnection.h"
#include "core/SpeConnection.h"
#include "core/TgxlConnection.h"
#include "core/ThemeManager.h"
#include "core/VkampConnection.h"
#include "models/AmpModel.h"
#include "models/AntennaGeniusModel.h"
#include "models/RadioModel.h"
#include "models/TunerModel.h"

#include <QAccessible>
#include <QApplication>
#include <QPainter>
#include <QRegion>
#include <QStyledItemDelegate>
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
#include <QScrollArea>
#include <QScrollBar>
#include <QTimer>
#include <QSpinBox>
#include <QStackedWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <QVector>

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
        Kpa1500,          // host + port
        Rfkit,            // host + port (HTTP)
        Kxpa100,          // KXPA100 amplifier with optional tuner
        Kat500,           // KAT500 serial or TCP tuner
        Kpa500,           // Serial with selectable baud, or raw TCP
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
    std::function<bool()> isNetworkConnection; // optional transport badge
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
    std::function<QString()> deviceInfo;
    QLabel* deviceInfoLabel{nullptr};
    QString fixedBaud;
    // Optional model picker for devices whose model decides the protocol and
    // line speed (SPE). Stored under settingsGroup/"Model"; empty = no picker.
    struct ModelChoice {
        QString label;
        QString key;
        QString baud;
    };
    QVector<ModelChoice> modelChoices;
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
    QComboBox* baudCombo{nullptr};
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

// The device list's line three starts with the connection-source dot. The item
// carries its source under kSourceRole (-1 while connecting, when there is no
// dot), and this delegate paints that dot in the source's color token; the
// rest of the line keeps the list's text color.
constexpr int kSourceRole = Qt::UserRole + 1;

class PeripheralListDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override
    {
        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index);
        const QString text = opt.text;
        const QVariant source = index.data(kSourceRole);
        const QString dot = QStringLiteral("● ");
        // initStyleOption() has turned the item's '\n' breaks into line
        // separators (U+2028), which is what the style lays out.
        auto isBreak = [](QChar c) {
            return c == QLatin1Char('\n') || c == QChar(QChar::LineSeparator);
        };
        int lineStart = 0;
        int lines = 1;
        for (int i = 0; i < text.size(); ++i) {
            if (isBreak(text.at(i))) {
                lineStart = i + 1;
                ++lines;
            }
        }
        if (!source.isValid() || source.toInt() < 0
            || text.mid(lineStart, dot.size()) != dot) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }
        // The item as usual, except in the dot's own cell, which keeps only
        // the item's fill and selection; the dot is drawn there in its color.
        // Each pixel is painted once, so a translucent selection is not
        // blended twice.
        const QWidget* widget = opt.widget;
        QStyle* style = widget ? widget->style() : QApplication::style();
        const QRect textRect = style->subElementRect(QStyle::SE_ItemViewItemText, &opt, widget);
        const QFontMetrics fm(opt.font);
        const int blockHeight = lines * fm.lineSpacing();
        int top = textRect.top();
        if (opt.displayAlignment & Qt::AlignVCenter) {
            top += (textRect.height() - blockHeight) / 2;
        }
        const int margin = style->pixelMetric(QStyle::PM_FocusFrameHMargin, nullptr, widget) + 1;
        const QPoint at(textRect.left() + margin,
                        top + (lines - 1) * fm.lineSpacing() + fm.ascent());
        const QRegion dotCell(at.x(), at.y() - fm.ascent(),
                              fm.horizontalAdvance(dot.left(1)), fm.height());

        painter->save();
        painter->setClipRegion(QRegion(opt.rect).subtracted(dotCell), Qt::IntersectClip);
        style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, widget);
        painter->restore();

        QStyleOptionViewItem ground = opt;
        ground.text.clear();
        painter->save();
        painter->setClipRegion(dotCell, Qt::IntersectClip);
        style->drawControl(QStyle::CE_ItemViewItem, &ground, painter, widget);
        painter->restore();

        using PeripheralConnectionSource::Source;
        const QColor color = ThemeManager::instance().color(
            PeripheralConnectionSource::colorToken(static_cast<Source>(source.toInt())));
        painter->save();
        painter->setFont(opt.font);
        painter->setPen(color);
        painter->drawText(at, QStringLiteral("●"));
        painter->restore();
    }
};

QString peripheralTr(const char* text)
{
    return RadioSetupDialog::tr(text);
}

bool effectivelyConnecting(const PeripheralDeviceUi& ui)
{
    return ui.state.connecting || (ui.isConnecting && ui.isConnecting());
}

Source sourceOf(const PeripheralDeviceUi& ui)
{
    if (ui.isConnected()) {
        return ui.isNetworkConnection && ui.isNetworkConnection()
            ? Source::Network : Source::Direct;
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
    } else if (state.source == Source::Direct || state.source == Source::Network) {
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

Tone statusTone(const PeripheralDeviceUi& ui)
{
    switch (ui.state.attention) {
    case Attention::InvalidCode:
    case Attention::AuthBlocked:
    case Attention::ConnectError:
    case Attention::CredentialError:
        return Tone::Error;
    case Attention::CredentialWarning:
    case Attention::RemovalPending:
        return Tone::Warn;
    case Attention::None:
        break;
    }
    if (!ui.state.note.isEmpty() || ui.state.discardedAuthCode || ui.state.codeNotSaved) {
        return Tone::Warn;
    }
    return ui.state.source == Source::Direct || ui.state.source == Source::Network
        ? Tone::Ok : Tone::Hint;
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
    const bool textChanged = label->text() != text;
    const bool nameChanged = label->accessibleName() != name;
    const bool descriptionChanged = label->accessibleDescription() != text;
    setTone(label, statusTone(ui));
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
    detail.layout = new QVBoxLayout(detail.page);
    detail.layout->setContentsMargins(8, 12, 0, 0);
    detail.layout->setSpacing(10);

    auto* heading = new QHBoxLayout;
    auto* title = new QLabel(ui.label, detail.page);
    setTone(title, Tone::Title);
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

// The automatic-connection toggle, the Connect button row and the stretch.
// `extraButtons` follow the edit hint.
void finishDetailPage(DetailPage& detail, PeripheralDeviceUi& ui,
                      const QList<QWidget*>& extraButtons)
{
    detail.group->setMaximumWidth(760);
    detail.layout->addWidget(detail.group, 0, Qt::AlignTop);

    if (ui.kind == PeripheralDeviceUi::Kind::AuthNetwork
        || (ui.kind == PeripheralDeviceUi::Kind::Kpa500 || ui.kind == PeripheralDeviceUi::Kind::Kat500 || ui.kind == PeripheralDeviceUi::Kind::Kxpa100)) {
        // TGXL and PGXL can also be reached through the radio, so for them the
        // toggle chooses the direct link over the radio's relay.
        const bool relayed = ui.id == QLatin1String("tgxl") || ui.id == QLatin1String("pgxl");
        auto* autoConnect = new QCheckBox(relayed
            ? RadioSetupDialog::tr("Connect directly when available")
            : RadioSetupDialog::tr("Connect automatically"), detail.page);
        autoConnect->setObjectName(QStringLiteral("peripheralAutoConnect_%1").arg(ui.id));
        autoConnect->setAccessibleName(relayed
            ? RadioSetupDialog::tr("%1 connect directly when available").arg(ui.label)
            : RadioSetupDialog::tr("%1 connect automatically").arg(ui.label));
        autoConnect->setAccessibleDescription(relayed
            ? RadioSetupDialog::tr(
                  "Connect directly to %1 on startup, when the radio reports it, and after a "
                  "drop. Turn off to use the radio's relay; Connect still connects directly "
                  "until that session drops.").arg(ui.label)
            : RadioSetupDialog::tr(
                  "Connect to %1 on startup, when discovery reports it, and after a drop. "
                  "Turn off to connect only when you click Connect.").arg(ui.label));
        if ((ui.kind == PeripheralDeviceUi::Kind::Kpa500 || ui.kind == PeripheralDeviceUi::Kind::Kat500 || ui.kind == PeripheralDeviceUi::Kind::Kxpa100)) {
            autoConnect->setAccessibleDescription(RadioSetupDialog::tr(
                "Connect to %1 on startup. Turn off to connect only when you click Connect.")
                .arg(ui.label));
        }
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
    setTone(ui.editHint, Tone::Hint);

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
                           static_cast<QWidget*>(ui.modeCombo), static_cast<QWidget*>(ui.baudCombo)}) {
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
    return button;
}

QSpinBox* makePortSpin(const PeripheralDeviceUi& ui)
{
    auto* spin = new QSpinBox;
    spin->setObjectName(QStringLiteral("peripheralPort_%1").arg(ui.id));
    spin->setRange(1, 65535);
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
    if (ui.baudCombo) {
        const QSignalBlocker blocked(ui.baudCombo);
        ui.baudCombo->setCurrentIndex(0);
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
        // visibility handlers see the cleared settings. A reconnect loop still
        // aimed at the cleared address is stopped too, or it would keep
        // dialing it until restart.
        settings.remove(ui->ipKey);
        settings.remove(ui->portKey);
        settings.save();
        if (ui->isConnected()
            || (ui->description && ui->description().startsWith(savedIp + ":"))) {
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
        // Only disconnect if the connection is actually the network target
        // being cleared, whether connected or still retrying it; it may be on
        // Serial, which this saved IP has nothing to do with.
        if (ui->description && ui->description().startsWith(savedIp + ":")) {
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

RadioSetupDialog::PeripheralLinkProbes RadioSetupDialog::swapPeripheralLinkForTest(
    const QString& id, PeripheralLinkProbes probes)
{
    PeripheralDeviceUi* ui = peripheralDevice(id);
    if (!ui) {
        return probes;
    }
    std::swap(ui->isConnected, probes.isConnected);
    std::swap(ui->hasRadioRelay, probes.hasRadioRelay);
    return probes;
}

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

        auto* show = new QPushButton(tr("Show"));
        ui.showButton = show;
        show->setObjectName(QStringLiteral("peripheralAuth_%1_5").arg(ui.id));
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

        auto* clear = new QPushButton(tr("Clear Code"));
        ui.clearButton = clear;
        clear->setObjectName(QStringLiteral("peripheralAuth_%1_6").arg(ui.id));
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
        ui.webUiButton->setToolTip(tr("Open ShackSwitch web interface"));
        connect(ui.webUiButton, &QPushButton::clicked, this, [u]() { u->openWebUi(); });
        extras.append(ui.webUiButton);
    }

    closeSaverForNetworkField(u, m_peripheralRowSavers);
    finishDetailPage(detail, ui, extras);
}

void RadioSetupDialog::buildSerialNetworkDevice(PeripheralDeviceUi& ui, QWidget* stackParent,
                                                const std::function<void()>& refresh,
                                                const std::shared_ptr<QVector<std::function<void()>>>& pageReseeds)
{
    PeripheralDeviceUi* const u = &ui;
    const QString group = ui.settingsGroup;
    DetailPage detail = beginDetailPage(ui, stackParent);

    ui.modeCombo = new QComboBox;
    ui.modeCombo->setObjectName(QStringLiteral("peripheralMode_%1").arg(ui.id));
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
        serialCombo->setAccessibleName(tr("%1 serial port").arg(ui.label));
        // A port's description can be long ("ttyACM1 — USB JTAG serial debug
        // unit"); sized by its content, one plugged-in device would widen the
        // page past the window. The open list still shows the whole name.
        serialCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        serialCombo->setMinimumContentsLength(14);
        serialCombo->view()->setTextElideMode(Qt::ElideNone);
        serialCustomEdit = new QLineEdit;
        serialCustomEdit->setPlaceholderText("/dev/ttyUSB0");
        serialCustomEdit->setAccessibleName(tr("%1 custom serial port").arg(ui.label));
        populateSerialPortCombo(serialCombo, serialCustomEdit,
                                PeripheralSettings::deviceString(group, "SerialPort"));
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
        // Show only after parenting: a visible parentless widget is a transient window.
        serialCustomEdit->setVisible(serialCombo->currentData().toString() == "__custom__");
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
    QLabel* fixedBaudLbl = nullptr;
#ifdef HAVE_SERIALPORT
    {
        if (ui.baudCombo) {
            serialBaudIdx = ui.portStack->addWidget(ui.baudCombo);
        } else {
            fixedBaudLbl = new QLabel(ui.fixedBaud);
            setTone(fixedBaudLbl, Tone::Hint);
            serialBaudIdx = ui.portStack->addWidget(fixedBaudLbl);
        }
    }
#endif
    auto* netPortSpin = new QSpinBox;
    netPortSpin->setRange(1, 65535);
    netPortSpin->setValue(PeripheralSettings::deviceInt(group, "ManualPort", ui.defaultPort));
    netPortSpin->setAccessibleName(tr("%1 network port").arg(ui.label));
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
        if (u->isConnected() || (u->isConnecting && u->isConnecting())) {
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

    if (!ui.modelChoices.isEmpty()) {
        // The model fixes the protocol and the serial speed, so the speed
        // label follows it. A change applies on the next Connect.
        ui.modelCombo = new QComboBox;
        ui.modelCombo->setObjectName(QStringLiteral("peripheralModel_%1").arg(ui.id));
        ui.modelCombo->setAccessibleName(tr("%1 model").arg(ui.label));
        ui.modelCombo->setAccessibleDescription(
            tr("Selects the amplifier's protocol and serial speed. Takes effect on the next connect."));
        for (const auto& choice : ui.modelChoices) {
            ui.modelCombo->addItem(choice.label, choice.key);
        }
        const int idx = ui.modelCombo->findData(PeripheralSettings::deviceString(group, "Model"));
        ui.modelCombo->setCurrentIndex(idx >= 0 ? idx : 0);
        auto applyModel = [u, fixedBaudLbl](int i) {
            if (fixedBaudLbl && i >= 0 && i < u->modelChoices.size()) {
                fixedBaudLbl->setText(u->modelChoices.at(i).baud);
            }
        };
        applyModel(ui.modelCombo->currentIndex());
        connect(ui.modelCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [group, combo = ui.modelCombo, applyModel](int i) {
            PeripheralSettings::setDeviceString(group, "Model", combo->itemData(i).toString());
            applyModel(i);
        });
        addFormRow(detail, tr("Amplifier Model"), ui.modelCombo);
    }
    addFormRow(detail, tr("Connection Type"), ui.modeCombo);
    addFormRow(detail, tr("Address/Serial Port"), ui.addressStack);
    addFormRow(detail, tr("TCP Port/Speed"), ui.portStack);
    if (ui.deviceInfo) {
        ui.deviceInfoLabel = new QLabel(ui.deviceInfo());
        ui.deviceInfoLabel->setObjectName(QStringLiteral("peripheralDeviceInfo_%1").arg(ui.id));
        ui.deviceInfoLabel->setAccessibleName(tr("%1 device information").arg(ui.label));
        ui.deviceInfoLabel->setWordWrap(true);
        ui.deviceInfoLabel->setTextFormat(Qt::PlainText);
        setTone(ui.deviceInfoLabel, Tone::Value);
        addFormRow(detail, tr("Device Information"), ui.deviceInfoLabel);
    }
    if ((ui.kind == PeripheralDeviceUi::Kind::Kpa500 || ui.kind == PeripheralDeviceUi::Kind::Kat500 || ui.kind == PeripheralDeviceUi::Kind::Kxpa100)) {
        auto* help = new QLabel(ui.networkTooltip);
        help->setWordWrap(true);
        setTone(help, Tone::Hint);
        addFormRow(detail, tr("Network Setup"), help);
    }

    QList<QWidget*> extras;
#ifdef HAVE_SERIALPORT
    ui.serialRefreshButton = new QPushButton(tr("Refresh Serial Ports"), detail.page);
    ui.serialRefreshButton->setObjectName(QStringLiteral("peripheralSerialRefresh_%1").arg(ui.id));
    ui.serialRefreshButton->setAutoDefault(false);
    ui.serialRefreshButton->setAccessibleName(tr("%1 refresh serial ports").arg(ui.label));
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

void RadioSetupDialog::buildVkampDevice(PeripheralDeviceUi& ui, QWidget* stackParent,
                                        const std::function<void()>& refresh)
{
    PeripheralDeviceUi* const u = &ui;
    DetailPage detail = beginDetailPage(ui, stackParent);

    ui.addressEdit = new QLineEdit;
    ui.addressEdit->setObjectName(QStringLiteral("peripheralAddress_%1").arg(ui.id));
    ui.addressEdit->setPlaceholderText(QStringLiteral("e.g. 192.168.1.50"));
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
        // Connected or still retrying: a reconnect loop aimed at the cleared
        // address would otherwise keep dialing it until restart.
        if (u->description().startsWith(savedIp + ":")) {
            u->disconnectNow();
        }
    });
    finishDetailPage(detail, ui, {});
}

void RadioSetupDialog::buildKpa500Device(PeripheralDeviceUi& ui, QWidget* stackParent,
                                        const std::function<void()>& refresh,
                                        const std::shared_ptr<QVector<std::function<void()>>>& pageReseeds)
{
#ifdef HAVE_SERIALPORT
    ui.baudCombo = new QComboBox;
    ui.baudCombo->setObjectName(QStringLiteral("peripheralBaud_%1").arg(ui.id));
    ui.baudCombo->setAccessibleName(tr("KPA500 baud rate"));
    ui.baudCombo->setAccessibleDescription(tr("Local serial speed; must match the amplifier. 8 data bits, no parity, 1 stop bit."));
    for (int baud : {4800, 9600, 19200, 38400}) {
        ui.baudCombo->addItem(QString::number(baud), baud);
    }
    const int savedBaud = PeripheralSettings::deviceInt(ui.settingsGroup, "BaudRate", 4800);
    const int baudIndex = ui.baudCombo->findData(savedBaud);
    ui.baudCombo->setCurrentIndex(baudIndex >= 0 ? baudIndex : 0);
    ui.connectSerial = [this, u = &ui](const QString& port) {
        const int baud = u->baudCombo->currentData().toInt();
        PeripheralSettings::setDeviceInt(u->settingsGroup, "BaudRate", baud);
        m_kpa500->connectSerial(port, baud);
    };
#endif
    ui.networkPlaceholder = tr("ser2net host, raw TCP mode");
    ui.networkDescription = tr("IP address or host name of the raw TCP serial proxy");
    ui.networkTooltip = tr("Use a raw TCP ser2net endpoint, with 8N1 and the amplifier's baud rate configured on the server. "
                           "Telnet/RFC 2217 and Elecraft Remote Server are not supported by this connection.");
    buildSerialNetworkDevice(ui, stackParent, refresh, pageReseeds);
    auto* advancedToggle = new QPushButton(tr("Advanced"), ui.page);
    advancedToggle->setObjectName(QStringLiteral("peripheralAdvanced_kpa500"));
    advancedToggle->setAccessibleName(tr("KPA500 advanced maintenance"));
    advancedToggle->setAccessibleDescription(tr("Configuration backup and restore, stored fault history and power-on banner."));
    advancedToggle->setCheckable(true);
    auto* advanced = new Kpa500AdvancedWidget(m_kpa500, ui.page);
    advanced->hide();
    auto* layout = qobject_cast<QVBoxLayout*>(ui.page->layout());
    layout->insertWidget(layout->count() - 1, advancedToggle, 0, Qt::AlignLeft);
    layout->insertWidget(layout->count() - 1, advanced);
    connect(advancedToggle, &QPushButton::toggled, advanced, &QWidget::setVisible);
    connect(ui.autoConnectCheck, &QCheckBox::toggled, this, [this](bool on) {
        m_kpa500->setAutoReconnect(on && PeripheralSettings::autoReconnect());
    });
}

void RadioSetupDialog::revealKpa500AdvancedSettings()
{
    selectTab(QStringLiteral("Peripherals"));
    if (QListWidget* devices = findChild<QListWidget*>(QStringLiteral("peripheralDeviceList"))) {
        for (int row = 0; row < devices->count(); ++row) {
            if (devices->item(row)->data(Qt::UserRole).toString() == QStringLiteral("kpa500")) {
                devices->setCurrentRow(row);
                break;
            }
        }
    }
    QPointer<QPushButton> toggle = findChild<QPushButton*>(QStringLiteral("peripheralAdvanced_kpa500"));
    if (!toggle) {
        return;
    }
    toggle->setChecked(true);
    toggle->setFocus(Qt::OtherFocusReason);
    QTimer::singleShot(0, this, [toggle] {
        if (!toggle) {
            return;
        }
        for (QWidget* parent = toggle->parentWidget(); parent; parent = parent->parentWidget()) {
            if (auto* area = qobject_cast<QScrollArea*>(parent); area && area->widget()) {
                const int y = toggle->mapTo(area->widget(), QPoint(0, 0)).y();
                area->verticalScrollBar()->setValue(qMax(0, y - 8));
                return;
            }
        }
    });
}

void RadioSetupDialog::buildKat500Device(PeripheralDeviceUi& ui, QWidget* stackParent,
                                        const std::function<void()>& refresh,
                                        const std::shared_ptr<QVector<std::function<void()>>>& pageReseeds)
{
#ifdef HAVE_SERIALPORT
    ui.baudCombo = new QComboBox;
    ui.baudCombo->setObjectName(QStringLiteral("peripheralBaud_%1").arg(ui.id));
    ui.baudCombo->setAccessibleName(tr("KAT500 baud rate"));
    ui.baudCombo->setAccessibleDescription(tr("Local serial speed; must match the tuner. 8 data bits, no parity, 1 stop bit."));
    for (int baud : {4800, 9600, 19200, 38400}) {
        ui.baudCombo->addItem(QString::number(baud), baud);
    }
    const int savedBaud = PeripheralSettings::deviceInt(ui.settingsGroup, "BaudRate", 38400);
    const int baudIndex = ui.baudCombo->findData(savedBaud);
    ui.baudCombo->setCurrentIndex(baudIndex >= 0 ? baudIndex : 0);
    ui.connectSerial = [this, u = &ui](const QString& port) {
        const int baud = u->baudCombo->currentData().toInt();
        PeripheralSettings::setDeviceInt(u->settingsGroup, "BaudRate", baud);
        m_kat500->connectSerial(port, baud);
    };
#endif
    ui.networkPlaceholder = tr("ser2net host, raw TCP mode");
    ui.networkDescription = tr("IP address or host name of the raw TCP serial proxy");
    ui.networkTooltip = tr("Use a raw TCP ser2net endpoint, with 8N1 and the tuner's baud rate configured on the server. "
                           "Telnet/RFC 2217 and Elecraft Remote Server are not supported by this connection.");
    buildSerialNetworkDevice(ui, stackParent, refresh, pageReseeds);
    auto* advancedToggle = new QPushButton(tr("Advanced"), ui.page);
    advancedToggle->setObjectName(QStringLiteral("peripheralAdvanced_kat500"));
    advancedToggle->setAccessibleName(tr("KAT500 advanced maintenance"));
    advancedToggle->setAccessibleDescription(tr("Configuration backup and restore, stored fault history and all seven configuration categories and stored matches."));
    advancedToggle->setCheckable(true);
    auto* advanced = new Kat500AdvancedWidget(m_kat500, ui.page);
    advanced->hide();
    auto* layout = qobject_cast<QVBoxLayout*>(ui.page->layout());
    layout->insertWidget(layout->count() - 1, advancedToggle, 0, Qt::AlignLeft);
    layout->insertWidget(layout->count() - 1, advanced);
    connect(advancedToggle, &QPushButton::toggled, advanced, &QWidget::setVisible);
    connect(ui.autoConnectCheck, &QCheckBox::toggled, this, [this](bool on) {
        m_kat500->setAutoReconnect(on && PeripheralSettings::autoReconnect());
    });
}

void RadioSetupDialog::revealKat500AdvancedSettings()
{
    selectTab(QStringLiteral("Peripherals"));
    if (QListWidget* devices = findChild<QListWidget*>(QStringLiteral("peripheralDeviceList"))) {
        for (int row = 0; row < devices->count(); ++row) {
            if (devices->item(row)->data(Qt::UserRole).toString() == QStringLiteral("kat500")) {
                devices->setCurrentRow(row);
                break;
            }
        }
    }
    QPointer<QPushButton> toggle = findChild<QPushButton*>(QStringLiteral("peripheralAdvanced_kat500"));
    if (!toggle) {
        return;
    }
    toggle->setChecked(true);
    toggle->setFocus(Qt::OtherFocusReason);
    QTimer::singleShot(0, this, [toggle] {
        if (!toggle) {
            return;
        }
        for (QWidget* parent = toggle->parentWidget(); parent; parent = parent->parentWidget()) {
            if (auto* area = qobject_cast<QScrollArea*>(parent); area && area->widget()) {
                const int y = toggle->mapTo(area->widget(), QPoint(0, 0)).y();
                area->verticalScrollBar()->setValue(qMax(0, y - 8));
                return;
            }
        }
    });
}

void RadioSetupDialog::buildKxpa100Device(PeripheralDeviceUi& ui, QWidget* stackParent,
                                        const std::function<void()>& refresh,
                                        const std::shared_ptr<QVector<std::function<void()>>>& pageReseeds)
{
#ifdef HAVE_SERIALPORT
    ui.baudCombo = new QComboBox;
    ui.baudCombo->setObjectName(QStringLiteral("peripheralBaud_%1").arg(ui.id));
    ui.baudCombo->setAccessibleName(tr("KXPA100 baud rate"));
    ui.baudCombo->setAccessibleDescription(tr("Local serial speed; must match the tuner. 8 data bits, no parity, 1 stop bit."));
    for (int baud : {4800, 9600, 19200, 38400}) {
        ui.baudCombo->addItem(QString::number(baud), baud);
    }
    const int savedBaud = PeripheralSettings::deviceInt(ui.settingsGroup, "BaudRate", 38400);
    const int baudIndex = ui.baudCombo->findData(savedBaud);
    ui.baudCombo->setCurrentIndex(baudIndex >= 0 ? baudIndex : 0);
    ui.connectSerial = [this, u = &ui](const QString& port) {
        const int baud = u->baudCombo->currentData().toInt();
        PeripheralSettings::setDeviceInt(u->settingsGroup, "BaudRate", baud);
        m_kxpa100->connectSerial(port, baud);
    };
#endif
    ui.networkPlaceholder = tr("ser2net host, raw TCP mode");
    ui.networkDescription = tr("IP address or host name of the raw TCP serial proxy");
    ui.networkTooltip = tr("Use a raw TCP ser2net endpoint, with 8N1 and the amplifier's baud rate configured on the server. "
                           "Telnet/RFC 2217 and Elecraft Remote Server are not supported by this connection.");
    buildSerialNetworkDevice(ui, stackParent, refresh, pageReseeds);
    auto* advancedToggle = new QPushButton(tr("Advanced"), ui.page);
    advancedToggle->setObjectName(QStringLiteral("peripheralAdvanced_kxpa100"));
    advancedToggle->setAccessibleName(tr("KXPA100 advanced maintenance"));
    advancedToggle->setAccessibleDescription(tr("Antenna configuration, backup and restore, stored fault history and tuner memories."));
    advancedToggle->setCheckable(true);
    auto* advanced = new Kxpa100AdvancedWidget(m_kxpa100, ui.page);
    advanced->hide();
    auto* layout = qobject_cast<QVBoxLayout*>(ui.page->layout());
    auto* associate = new QPushButton(tr("Use with current radio"), ui.page);
    associate->setObjectName("kxAssociateRadio");
    associate->setAccessibleName(tr("Associate KXPA100 with the connected radio"));
    associate->setToolTip(tr("Bind this KXPA100 serial to this radio for VU/PWR metering and applet frequency context."));
    layout->insertWidget(layout->count() - 1, associate);
    connect(associate, &QPushButton::clicked, this, [this, associate] {
        const bool saved = Kxpa100RadioAssociation::associate(m_model, m_kxpa100);
        associate->setToolTip(saved ? tr("Association saved for the current radio.") : tr("Connect an identified radio and KXPA100 before associating them. Settings must be writable."));
    });
    layout->insertWidget(layout->count() - 1, advancedToggle, 0, Qt::AlignLeft);
    layout->insertWidget(layout->count() - 1, advanced);
    connect(advancedToggle, &QPushButton::toggled, advanced, &QWidget::setVisible);
    connect(ui.autoConnectCheck, &QCheckBox::toggled, this, [this](bool on) {
        m_kxpa100->setAutoReconnect(on && PeripheralSettings::autoReconnect());
    });
}

void RadioSetupDialog::revealKxpa100AdvancedSettings()
{
    selectTab(QStringLiteral("Peripherals"));
    if (QListWidget* devices = findChild<QListWidget*>(QStringLiteral("peripheralDeviceList"))) {
        for (int row = 0; row < devices->count(); ++row) {
            if (devices->item(row)->data(Qt::UserRole).toString() == QStringLiteral("kxpa100")) {
                devices->setCurrentRow(row);
                break;
            }
        }
    }
    QPointer<QPushButton> toggle = findChild<QPushButton*>(QStringLiteral("peripheralAdvanced_kxpa100"));
    if (!toggle) {
        return;
    }
    toggle->setChecked(true);
    toggle->setFocus(Qt::OtherFocusReason);
    QTimer::singleShot(0, this, [toggle] {
        if (!toggle) {
            return;
        }
        for (QWidget* parent = toggle->parentWidget(); parent; parent = parent->parentWidget()) {
            if (auto* area = qobject_cast<QScrollArea*>(parent); area && area->widget()) {
                const int y = toggle->mapTo(area->widget(), QPoint(0, 0)).y();
                area->verticalScrollBar()->setValue(qMax(0, y - 8));
                return;
            }
        }
    });
}

void RadioSetupDialog::buildRfkitDevice(PeripheralDeviceUi& ui, QWidget* stackParent,
                                        const std::function<void()>& refresh)
{
    PeripheralDeviceUi* const u = &ui;
    DetailPage detail = beginDetailPage(ui, stackParent);

    ui.addressEdit = new QLineEdit;
    ui.addressEdit->setObjectName(QStringLiteral("peripheralAddress_%1").arg(ui.id));
    ui.addressEdit->setPlaceholderText(QStringLiteral("e.g. 192.168.1.70"));
    ui.addressEdit->setText(PeripheralSettings::deviceString("Rfkit", "ManualIp"));
    ui.addressEdit->setAccessibleName(tr("RF-KIT address"));
    ui.addressEdit->setAccessibleDescription(
        tr("IP address or host name of the RF-KIT amplifier"));

    // The REST API's port is a device setting, so it is editable rather than fixed.
    ui.portSpin = makePortSpin(ui);
    ui.portSpin->setValue(PeripheralSettings::deviceInt("Rfkit", "ManualPort", ui.defaultPort));
    ui.portSpin->setAccessibleName(tr("RF-KIT control port"));
    ui.portSpin->setAccessibleDescription(tr("HTTP port, 1 to 65535, default 8080"));

    ui.connectButton = makeConnectButton(ui);
    ui.connectButton->setAccessibleName(tr("Connect or disconnect the RF-KIT amplifier"));
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
        PeripheralSettings::setDeviceString("Rfkit", "ManualIp", ip);
        PeripheralSettings::setDeviceInt("Rfkit", "ManualPort", port);
        u->state.connecting = true;
        refresh();
        u->connectNetwork(ip, static_cast<quint16>(port));
        refresh();
    });

    addFormRow(detail, tr("IP Address"), ui.addressEdit);
    addFormRow(detail, tr("Port"), ui.portSpin);

    m_peripheralRowSavers.append([u]() {
        if (!u->addressEdit || !u->addressEdit->text().trimmed().isEmpty()) {
            return;
        }
        const QString savedIp = PeripheralSettings::deviceString("Rfkit", "ManualIp");
        if (savedIp.isEmpty()) {
            return;
        }
        PeripheralSettings::clearDeviceField("Rfkit", "ManualIp");
        PeripheralSettings::clearDeviceField("Rfkit", "ManualPort");
        // Connected or still retrying: a reconnect loop aimed at the cleared
        // address would otherwise keep dialing it until restart.
        if (u->description().startsWith(savedIp + ":")) {
            u->disconnectNow();
        }
    });
    finishDetailPage(detail, ui, {});
}

void RadioSetupDialog::buildKpa1500Device(PeripheralDeviceUi& ui, QWidget* stackParent,
                                          const std::function<void()>& refresh)
{
    PeripheralDeviceUi* const u = &ui;
    DetailPage detail = beginDetailPage(ui, stackParent);

    ui.addressEdit = new QLineEdit;
    ui.addressEdit->setObjectName(QStringLiteral("peripheralAddress_%1").arg(ui.id));
    ui.addressEdit->setPlaceholderText(QStringLiteral("e.g. 192.168.1.60"));
    ui.addressEdit->setText(PeripheralSettings::deviceString("Kpa1500", "ManualIp"));
    ui.addressEdit->setAccessibleName(tr("KPA1500 address"));
    ui.addressEdit->setAccessibleDescription(
        tr("IP address or host name of the Elecraft KPA1500 amplifier"));

    // The amp's port is movable with ^CP, so it is editable rather than fixed.
    ui.portSpin = makePortSpin(ui);
    ui.portSpin->setValue(PeripheralSettings::deviceInt("Kpa1500", "ManualPort", ui.defaultPort));
    ui.portSpin->setAccessibleName(tr("KPA1500 control port"));
    ui.portSpin->setAccessibleDescription(tr("TCP control port, 1 to 65535, default 1500"));

    ui.connectButton = makeConnectButton(ui);
    ui.connectButton->setAccessibleName(tr("Connect or disconnect the Elecraft KPA1500"));
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
        PeripheralSettings::setDeviceString("Kpa1500", "ManualIp", ip);
        PeripheralSettings::setDeviceInt("Kpa1500", "ManualPort", port);
        u->state.connecting = true;
        refresh();
        u->connectNetwork(ip, static_cast<quint16>(port));
        refresh();
    });

    addFormRow(detail, tr("IP Address"), ui.addressEdit);
    addFormRow(detail, tr("Port"), ui.portSpin);

    m_peripheralRowSavers.append([u]() {
        if (!u->addressEdit || !u->addressEdit->text().trimmed().isEmpty()) {
            return;
        }
        const QString savedIp = PeripheralSettings::deviceString("Kpa1500", "ManualIp");
        if (savedIp.isEmpty()) {
            return;
        }
        PeripheralSettings::clearDeviceField("Kpa1500", "ManualIp");
        PeripheralSettings::clearDeviceField("Kpa1500", "ManualPort");
        // Connected or still retrying: a reconnect loop aimed at the cleared
        // address would otherwise keep dialing it until restart.
        if (u->description().startsWith(savedIp + ":")) {
            u->disconnectNow();
        }
    });
    finishDetailPage(detail, ui, {});
}

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
    m_peripheralRowSavers.clear();   // they point into m_peripheralDevices

    auto* content = new QWidget(page);
    content->setObjectName(QStringLiteral("peripheralContent"));
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
    setTone(emptyText, Tone::Hint);
    emptyLayout->addWidget(emptyText);
    detailStack->addWidget(emptyPage);

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
        // The target reconnects aim at, so clearing its address can stop them.
        ui->description = [this]() {
            const QString host = m_tgxl->reconnectHost();
            return host.isEmpty() ? QString()
                                  : QStringLiteral("%1:%2").arg(host).arg(m_tgxl->reconnectPort());
        };
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
        // The target reconnects aim at, so clearing its address can stop them.
        ui->description = [this]() {
            const QString host = m_pgxl->reconnectHost();
            return host.isEmpty() ? QString()
                                  : QStringLiteral("%1:%2").arg(host).arg(m_pgxl->reconnectPort());
        };
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
            "              /dev/ttyUSB0\n"
            "\n"
            "Plain serial-to-Ethernet converters (e.g. Waveshare in\n"
            "TCP-server mode) work for monitoring and control, but cannot\n"
            "power the amplifier ON — they have no RFC 2217 support.\n"
            "\n"
            "Original 1K-FA: set the proxy's serial side to 9600 baud;\n"
            "raw mode is preferred (its binary status suffers in telnet mode).");
        auto ui = serialDevice(QStringLiteral("spe"), tr("SPE Expert Amplifier"),
            QStringLiteral("SPE Expert amplifier"), QStringLiteral("SpeExpert"), 7000,
            QStringLiteral("115200 8N1"), QStringLiteral("ser2net host — e.g. 192.168.1.52"),
            tr("IP address or host name of the serial proxy"), ser2netTip);
        ui->modelChoices = {
            {tr("1.3K / 1.5K / 2K-FA"), Spe::variantKey(Spe::Variant::Expert),
             QStringLiteral("115200 8N1")},
            {tr("1K-FA (original, RS-232)"), Spe::variantKey(Spe::Variant::Legacy1k),
             QStringLiteral("9600 8N1")},
        };
        // The variant is read at connect time so a model change made since
        // the last connect is honoured.
        auto applySpeVariant = [this]() {
            m_spe->setVariant(Spe::variantFromKey(
                PeripheralSettings::deviceString("SpeExpert", "Model")));
        };
        ui->isConnected = [this]() { return m_spe->isConnected(); };
        ui->disconnectNow = [this]() { m_spe->disconnect(); };
        ui->connectNetwork = [this, applySpeVariant](const QString& h, quint16 p) {
            applySpeVariant();
            m_spe->connectNetwork(h, p);
        };
        ui->connectSerial = [this, applySpeVariant](const QString& port) {
            applySpeVariant();
            m_spe->connectSerial(port);
        };
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
    if (m_kpa1500) {
        // TCP only: the amp's UDP server takes the same commands, but control
        // writes here need delivery confirmation (kpa1500-amplifier-design.md).
        auto ui = addDevice(QStringLiteral("kpa1500"), tr("Elecraft KPA1500"),
                            QStringLiteral("KPA1500 amplifier"),
                            PeripheralDeviceUi::Kind::Kpa1500, Kpa1500::kDefaultPort);
        ui->settingsGroup = QStringLiteral("Kpa1500");
        ui->isConnected = [this]() { return m_kpa1500->isConnected(); };
        ui->disconnectNow = [this]() { m_kpa1500->disconnect(); };
        ui->connectNetwork = [this](const QString& h, quint16 p) { m_kpa1500->connectNetwork(h, p); };
        ui->description = [this]() { return m_kpa1500->description(); };
        ui->afterRemoval = [this]() { m_kpa1500->disconnect(); };
        buildKpa1500Device(*ui, detailStack, refresh);
    }
    if (m_rfkit) {
        // REST over HTTP (docs/architecture/rfkit-amplifier-design.md): a plain
        // host/port pair, no Serial/Network toggle.
        auto ui = addDevice(QStringLiteral("rfkit"), tr("RF-KIT Amplifier"),
                            QStringLiteral("RF-KIT amplifier"),
                            PeripheralDeviceUi::Kind::Rfkit, Rfkit::kDefaultPort);
        ui->settingsGroup = QStringLiteral("Rfkit");
        ui->isConnected = [this]() { return m_rfkit->isConnected(); };
        ui->disconnectNow = [this]() { m_rfkit->disconnect(); };
        ui->connectNetwork = [this](const QString& h, quint16 p) { m_rfkit->connectNetwork(h, p); };
        ui->description = [this]() { return m_rfkit->description(); };
        ui->afterRemoval = [this]() { m_rfkit->disconnect(); };
        buildRfkitDevice(*ui, detailStack, refresh);
    }
    if (m_kpa500) {
        auto ui = addDevice(QStringLiteral("kpa500"), tr("Elecraft KPA500"),
                            QStringLiteral("KPA500 amplifier"),
                            PeripheralDeviceUi::Kind::Kpa500, 2000);
        ui->settingsGroup = QStringLiteral("Kpa500");
        ui->isConnected = [this]() { return m_kpa500->isConnected(); };
        ui->isConnecting = [this]() { return m_kpa500->isConnecting(); };
        ui->isNetworkConnection = [this]() { return m_kpa500->isNetworkConnection(); };
        ui->connectNetwork = [this](const QString& host, quint16 port) { m_kpa500->connectToHost(host, port); };
        ui->deviceInfo = [this]() {
            const Kpa500::Status& status = m_kpa500->lastStatus();
            return tr("%1\nFirmware: %2 · Serial: %3")
                .arg(m_kpa500->isConnected() ? m_kpa500->description() : tr("Disconnected"),
                     status.firmwareVersion.value_or(tr("Not reported")),
                     status.serialNumber.value_or(tr("Not reported")));
        };
        ui->disconnectNow = [this]() { m_kpa500->disconnect(); };
        ui->description = [this]() { return m_kpa500->description(); };
        ui->afterRemoval = [this]() {
            m_kpa500->disconnect();
            m_kpa500->setAutoReconnect(PeripheralSettings::autoReconnect()
                                      && PeripheralSettings::autoConnect("kpa500"));
        };
        ui->state.note = m_kpa500->isConnected() && !m_kpa500->isResponding()
            ? tr("amplifier not answering") : QString();
        buildKpa500Device(*ui, detailStack, refresh, serialReseeds);
    }
    if (m_kat500) {
        auto ui = addDevice(QStringLiteral("kat500"), tr("Elecraft KAT500"),
                            QStringLiteral("KAT500 tuner"),
                            PeripheralDeviceUi::Kind::Kat500, 2000);
        ui->settingsGroup = QStringLiteral("Kat500");
        ui->isConnected = [this]() { return m_kat500->isConnected(); };
        ui->isConnecting = [this]() { return m_kat500->isConnecting(); };
        ui->isNetworkConnection = [this]() { return m_kat500->isNetworkConnection(); };
        ui->connectNetwork = [this](const QString& host, quint16 port) { m_kat500->connectToHost(host, port); };
        ui->deviceInfo = [this]() {
            const Kat500::Status& status = m_kat500->status();
            return tr("%1\nFirmware: %2 · Serial: %3\n%4")
                .arg(m_kat500->isConnected() ? m_kat500->description() : tr("Disconnected"),
                     status.firmware.isEmpty() ? tr("Not reported") : status.firmware,
                     status.serial.isEmpty() ? tr("Not reported") : status.serial,
                     m_kat500->unavailableReason(false));
        };
        ui->disconnectNow = [this]() { m_kat500->disconnectDevice(); };
        ui->description = [this]() { return m_kat500->description(); };
        ui->afterRemoval = [this]() {
            m_kat500->disconnectDevice();
            m_kat500->setAutoReconnect(PeripheralSettings::autoReconnect()
                                      && PeripheralSettings::autoConnect("kat500"));
        };
        ui->state.note = m_kat500->isConnected() && !m_kat500->isResponding()
            ? tr("tuner not answering") : QString();
        buildKat500Device(*ui, detailStack, refresh, serialReseeds);
    }
    if (m_kxpa100) {
        auto ui = addDevice(QStringLiteral("kxpa100"), tr("Elecraft KXPA100"),
                            QStringLiteral("KXPA100 amplifier / tuner"),
                            PeripheralDeviceUi::Kind::Kxpa100, 2000);
        ui->settingsGroup = QStringLiteral("Kxpa100");
        ui->isConnected = [this]() { return m_kxpa100->isConnected(); };
        ui->isConnecting = [this]() { return m_kxpa100->isConnecting(); };
        ui->isNetworkConnection = [this]() { return m_kxpa100->isNetworkConnection(); };
        ui->connectNetwork = [this](const QString& host, quint16 port) { m_kxpa100->connectToHost(host, port); };
        ui->deviceInfo = [this]() {
            const Kxpa100::Status& status = m_kxpa100->status();
            return tr("%1\nFirmware: %2 · Serial: %3\n%4")
                .arg(m_kxpa100->isConnected() ? m_kxpa100->description() : tr("Disconnected"),
                     status.firmware.isEmpty() ? tr("Not reported") : status.firmware,
                     status.serial.isEmpty() ? tr("Not reported") : status.serial,
                     m_kxpa100->unavailableReason(false));
        };
        ui->disconnectNow = [this]() { m_kxpa100->disconnectDevice(); };
        ui->description = [this]() { return m_kxpa100->description(); };
        ui->afterRemoval = [this]() {
            m_kxpa100->disconnectDevice();
            m_kxpa100->setAutoReconnect(PeripheralSettings::autoReconnect()
                                      && PeripheralSettings::autoConnect("kxpa100"));
        };
        ui->state.note = m_kxpa100->isConnected() && !m_kxpa100->isResponding()
            ? tr("amplifier not answering") : QString();
        buildKxpa100Device(*ui, detailStack, refresh, serialReseeds);
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
    auto* reconnectCheck = new QCheckBox(tr("Reconnect automatically"));
    reconnectCheck->setObjectName(QStringLiteral("peripheralAutoReconnect"));
    reconnectCheck->setAccessibleDescription(tr("Reconnect peripherals after a connection drops."));
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
        if (m_kpa1500) {
            m_kpa1500->setAutoReconnect(on);
        }
        if (m_rfkit) {
            m_rfkit->setAutoReconnect(on);
        }
        if (m_kpa500) {
            m_kpa500->setAutoReconnect(on && PeripheralSettings::autoConnect("kpa500"));
        }
        if (m_kat500) {
            m_kat500->setAutoReconnect(on && PeripheralSettings::autoConnect("kat500"));
        }
        if (m_kxpa100) {
            m_kxpa100->setAutoReconnect(on && PeripheralSettings::autoConnect("kxpa100"));
        }
        // VK3AMP takes this setting only at startup (#4919).
    });

    auto* listGroup = new QGroupBox(tr("Devices"), content);
    listGroup->setMinimumWidth(250);
    listGroup->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    auto* listLayout = new QVBoxLayout(listGroup);
    auto* deviceList = new QListWidget(listGroup);
    deviceList->setObjectName(QStringLiteral("peripheralDeviceList"));
    deviceList->setAccessibleName(tr("Configured peripherals"));
    deviceList->setAlternatingRowColors(true);
    deviceList->setSpacing(3);
    deviceList->setItemDelegate(new PeripheralListDelegate(deviceList));
    deviceList->setMinimumHeight(200);
    deviceList->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    listLayout->addWidget(deviceList);

    auto* listActions = new QHBoxLayout;
    auto* addButton = new QPushButton(tr("Add"), listGroup);
    addButton->setObjectName(QStringLiteral("peripheralAddButton"));
    addButton->setAccessibleName(tr("Add peripheral device"));
    addButton->setAccessibleDescription(tr("Choose a device type to configure"));
    auto* addMenu = new QMenu(addButton);
    addButton->setMenu(addMenu);
    auto* removeButton = new QPushButton(tr("Remove"), listGroup);
    removeButton->setObjectName(QStringLiteral("peripheralRemoveButton"));
    removeButton->setAccessibleName(tr("Remove selected peripheral device"));
    removeButton->setAccessibleDescription(tr("Select a configured peripheral to remove it"));
    removeButton->setToolTip(tr("Remove this device and clear its saved connection details"));
    removeButton->setEnabled(false);
    listActions->addWidget(addButton);
    listActions->addWidget(removeButton);
    listActions->addStretch(1);
    listLayout->addLayout(listActions);
    auto* removalNotice = new QLabel(listGroup);
    removalNotice->setObjectName(QStringLiteral("peripheralRemovalNotice"));
    removalNotice->setAccessibleName(tr("Peripheral removal status"));
    removalNotice->setWordWrap(true);
    removalNotice->hide();
    setTone(removalNotice, Tone::Warn);
    listLayout->addWidget(removalNotice);
    listLayout->addSpacing(8);
    listLayout->addWidget(reconnectCheck);
    listLayout->addSpacing(8);
    auto* helpButton = new QPushButton(tr("Connection Help"), listGroup);
    helpButton->setObjectName(QStringLiteral("peripheralConnectionHelp"));
    helpButton->setAutoDefault(false);
    helpButton->setAccessibleDescription(tr("Explain local and remote peripheral connections."));
    connect(helpButton, &QPushButton::clicked, this, [this]() {
        QMessageBox::information(this, tr("Peripheral connections"),
            tr("Add a device and enter its address for a direct connection. "
               "Use this for devices that are not discovered on your local network, "
               "including remote, VPN, and SmartLink setups.\n\n"
               "Configured devices connect when the radio connects. "
               "Reconnect automatically also retries after a connection drops. "
               "On a TGXL or PGXL, turn off Connect directly when available to use the "
               "radio's relay instead. On an Antenna Genius or ShackSwitch, turn off "
               "Connect automatically to connect it only when you click Connect.\n\n"
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
                          &settings](PeripheralDeviceUi& ui,
                                     const std::optional<QPair<QString, QString>>& savedAtClick = std::nullopt) {
        // An address saved from elsewhere (the AG applet) while the credential
        // deletion was pending is newer than this Remove: keep it.
        // An empty value is not a target: MainWindow clears AG_ManualIp when a
        // ShackSwitch is discovered.
        const QString currentHost = settings.value(ui.ipKey, QString()).toString();
        const bool newerEndpointSaved = savedAtClick
            && ui.kind == PeripheralDeviceUi::Kind::AuthNetwork
            && !currentHost.isEmpty()
            && (currentHost != savedAtClick->first
                || settings.value(ui.portKey, QString()).toString() != savedAtClick->second);
        if (ui.kind == PeripheralDeviceUi::Kind::AuthNetwork) {
            // Remove resets the device to its defaults, "Connect automatically"
            // included; a hidden row never keeps a silent "do not connect".
            PeripheralSettings::resetAutoConnect(ui.id);
            if (ui.autoConnectCheck) {
                const QSignalBlocker blocked(ui.autoConnectCheck);
                ui.autoConnectCheck->setChecked(true);
            }
            if (!newerEndpointSaved) {
                settings.remove(ui.ipKey);
                settings.remove(ui.portKey);
                settings.save();
            }
        } else {
            PeripheralSettings::clearDeviceConnection(ui.settingsGroup);
            if ((ui.kind == PeripheralDeviceUi::Kind::Kpa500 || ui.kind == PeripheralDeviceUi::Kind::Kat500 || ui.kind == PeripheralDeviceUi::Kind::Kxpa100)) {
                PeripheralSettings::clearDeviceField(ui.settingsGroup, "BaudRate");
                PeripheralSettings::resetAutoConnect(ui.id);
                const QSignalBlocker blocked(ui.autoConnectCheck);
                ui.autoConnectCheck->setChecked(true);
            }
        }
        if (ui.afterRemoval) {
            ui.afterRemoval();
        }
        resetFields(ui);
        if (newerEndpointSaved) {
            // Show the newer target rather than the defaults.
            const QString host = settings.value(ui.ipKey, QString()).toString();
            const int port = settings.value(ui.portKey, QString()).toInt();
            ui.addressEdit->setText(host);
            ui.portSpin->setValue(port > 0 ? port : ui.defaultPort);
            ui.addressEdit->setProperty("peripheralSavedHost", host.trimmed());
            ui.addressEdit->setProperty("peripheralSavedPort",
                settings.value(ui.portKey, QString()).toString());
            ui.addressEdit->setProperty("peripheralPrefillPort", ui.portSpin->value());
            ui.addressEdit->setModified(false);
        }
        if (!newerEndpointSaved) {
            activeIds->removeAll(ui.id);
            configuredIds->removeAll(ui.id);
            PeripheralSettings::setVisibleDeviceIds(*configuredIds);
        }
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
        if (!confirmPeripheralRemoval(ui->label, ui->autoConnectCheck
                                                       ? ui->autoConnectCheck->text()
                                                       : QString())) {
            return;
        }
        // Acquire before disconnect: model signals can synchronously request
        // another manual or discovered connection during teardown.
        auto removal = std::make_shared<PeripheralRemovalGuard>(ui->authDevice);
        m_peripheralRemovalPending = true;
        QString removalEndpoint;
        const QPair<QString, QString> savedAtClick{
            AppSettings::instance().value(ui->ipKey, QString()).toString(),
            AppSettings::instance().value(ui->portKey, QString()).toString()};
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
                                savedAtClick,
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
                finishRemoval(*ui, savedAtClick);
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
        if (ui->endpointScopedCredential && removalEndpoint.isEmpty()) {
            // This device has no endpoint to match a code against. If the shared
            // slot holds the other device's code, there is nothing of this
            // device's to delete; only when the owner is unknown does Remove stop.
            const bool otherLive = m_ag->isConnected()
                && AntennaGeniusModel::isShackSwitch(m_ag->connectedDevice()) != ui->isShackSwitch;
            // The other device's live session: its code was saved under the host
            // the attempt asked for, which configuredEndpoint() rebuilds when the
            // socket has no peer to report.
            const QString otherLiveEndpoint = !otherLive ? QString()
                : !m_ag->peerAddress().isEmpty()
                ? PeripheralAuthStore::endpoint(m_ag->attemptHost(), m_ag->peerAddress(),
                                                m_ag->peerPort())
                : PeripheralAuthStore::configuredEndpoint(m_ag->attemptHost(),
                                                          m_ag->connectedDevice().port);
            const QString otherEndpoint = otherLive
                ? otherLiveEndpoint
                : ui->isShackSwitch
                ? (AntennaGeniusModel::isShackSwitch(m_ag->connectedDevice())
                       ? QString()
                       : PeripheralAuthStore::configuredEndpoint(
                             AppSettings::instance().value("AG_ManualIp").toString(),
                             static_cast<quint16>(AppSettings::instance().value("AG_ManualPort", 9007).toUInt())))
                : PeripheralAuthStore::configuredEndpoint(
                      AppSettings::instance().value("SS_ManualIp").toString(), kShackSwitchControlPort);
            const auto device = ui->authDevice;
            const auto endpointScoped = [device, completed, removalEndpoint]() {
                PeripheralAuthStore::clearForEndpoint(device, removalEndpoint, qApp, completed);
            };
            if (otherEndpoint.isEmpty()) {
                endpointScoped();
            } else {
                PeripheralAuthStore::load(device, otherEndpoint, qApp,
                    [completed, endpointScoped](const PeripheralAuthStore::LoadResult& result) {
                        if (result.status == PeripheralAuthStore::LoadStatus::Found) {
                            completed(PeripheralAuthStore::ClearResult::Cleared);
                        } else {
                            endpointScoped();
                        }
                    });
            }
        } else if (ui->endpointScopedCredential) {
            PeripheralAuthStore::clearForEndpoint(ui->authDevice, removalEndpoint, qApp, completed);
        } else {
            PeripheralAuthStore::clear(ui->authDevice, qApp, completed);
        }
    });

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
            // The endpoint that refused the code. An alternate attempt keeps the
            // reconnect target on the manual host, so it is not that endpoint.
            const QString host = tgxl ? m_tgxl->attemptHost() : m_pgxl->attemptHost();
            if (!blocked || host.isEmpty()) {
                continue;
            }
            if (!activeIds->contains(ui->id)) {
                activeIds->append(ui->id);
                addedRow = true;
            }
            if (!ui->addressEdit->isModified()) {
                const quint16 port = tgxl ? m_tgxl->attemptPort() : m_pgxl->attemptPort();
                if (ui->addressEdit->text() != host || ui->portSpin->value() != port) {
                    ui->addressEdit->setText(host);
                    ui->portSpin->setValue(port);
                }
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
            const bool kpaConnecting = (ui.kind == PeripheralDeviceUi::Kind::Kpa500 || ui.kind == PeripheralDeviceUi::Kind::Kat500 || ui.kind == PeripheralDeviceUi::Kind::Kxpa100) && connecting;
            ui.connectButton->setText(connected ? tr("Disconnect")
                : kpaConnecting ? tr("Cancel") : tr("Connect"));
            applyStatusLabel(ui);
            if (ui.deviceInfoLabel && ui.deviceInfo) {
                const QString info = ui.deviceInfo();
                if (ui.deviceInfoLabel->text() != info) {
                    ui.deviceInfoLabel->setText(info);
                    ui.deviceInfoLabel->setAccessibleDescription(info);
                    QAccessibleEvent event(ui.deviceInfoLabel, QAccessible::DescriptionChanged);
                    QAccessible::updateAccessibility(&event);
                }
            }

            const bool locked = connected || kpaConnecting;
            const QString reason = locked
                ? tr("Disconnect or cancel to change connection settings.") : QString();
            updateFieldAvailability(ui, ui.addressEdit, locked, reason);
            updateFieldAvailability(ui, ui.addressStack, locked, reason);
            updateFieldAvailability(ui, ui.portSpin, locked, reason);
            updateFieldAvailability(ui, ui.portStack, locked, reason);
            updateFieldAvailability(ui, ui.modeCombo, locked, reason);
            updateFieldAvailability(ui, ui.baudCombo, locked, reason);
            if (ui.editHint) {
                ui.editHint->setText(reason);
                ui.editHint->setVisible(locked);
            }
            if (ui.serialRefreshButton) {
                ui.serialRefreshButton->setEnabled(!locked);
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
                item->setData(kSourceRole, connecting ? -1 : static_cast<int>(ui.state.source));
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
                ? tr("%1 hide authorization code").arg(ui.shortName)
                : tr("%1 show authorization code").arg(ui.shortName));
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
        // An automatic attempt starts without any other signal; show Connecting….
        connect(m_tgxl, &TgxlConnection::attemptStarted, this, [refresh]() { refresh(); });
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
        connect(m_pgxl, &PgxlConnection::attemptStarted, this, [refresh]() { refresh(); });
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
        connect(m_ag, &AntennaGeniusModel::attemptStarted, this, [refresh]() { refresh(); });
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

    // The serial, VK3AMP, KPA1500 and RF-KIT devices: link changes, failures, and link-up with a
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
    if (m_kpa1500) {
        wireLinkDevice(QStringLiteral("kpa1500"), m_kpa1500);
    }
    if (m_rfkit) {
        wireLinkDevice(QStringLiteral("rfkit"), m_rfkit);
    }
    if (m_kat500) {
        wireLinkDevice(QStringLiteral("kat500"), m_kat500);
        connect(m_kat500, &Kat500Model::statusUpdated, this, refresh);
        connect(m_kat500, &Kat500Model::availabilityChanged, this, refresh);
    }
    if (m_kxpa100) {
        wireLinkDevice(QStringLiteral("kxpa100"), m_kxpa100);
        connect(m_kxpa100, &Kxpa100Model::statusUpdated, this, refresh);
        connect(m_kxpa100, &Kxpa100Model::availabilityChanged, this, refresh);
    }
    if (m_kpa500) {
        wireLinkDevice(QStringLiteral("kpa500"), m_kpa500);
        connect(m_kpa500, &Kpa500Connection::statusUpdated, this, refresh);
        connect(m_kpa500, &Kpa500Connection::respondingChanged, this, [this, refresh](bool responding) {
            if (PeripheralDeviceUi* ui = peripheralDevice(QStringLiteral("kpa500"))) {
                ui->state.note = ui->isConnected() && !responding
                    ? tr("amplifier not answering") : QString();
            }
            refresh();
        });
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
