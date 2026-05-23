#include "ConnectionPanel.h"
#include "core/AppSettings.h"
#include "core/NetworkPathResolver.h"
#include "FramelessResizer.h"
#include "FramelessWindowTitleBar.h"

#include <memory>

#include <QAbstractItemView>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QSignalBlocker>
#include <QTcpSocket>
#include <QTimer>
#include <QVBoxLayout>

namespace AetherSDR {

namespace {

constexpr int kSourceModeRole = Qt::UserRole + 10;
constexpr int kSourceInterfaceIdRole = Qt::UserRole + 11;
constexpr int kSourceInterfaceNameRole = Qt::UserRole + 12;
constexpr int kSourceAddressRole = Qt::UserRole + 13;
constexpr int kSourceStaleRole = Qt::UserRole + 14;
constexpr int kMaxRecentManualIps = 3;
constexpr const char* kRecentManualIpsKey = "RecentConnectByIpAddresses";

const char* kHintLabelStyle =
    "QLabel { color: #8aa8c0; font-size: 11px; background: transparent; border: none; }";
const char* kInfoLabelStyle =
    "QLabel { color: #9bd1ff; font-size: 11px; background: transparent; border: none; }";
const char* kErrorLabelStyle =
    "QLabel { color: #ff8f8f; font-size: 11px; background: transparent; border: none; }";

QJsonObject loadRoutedProfiles()
{
    const QByteArray json =
        AppSettings::instance().value("RoutedProfilesJson", "{}").toString().toUtf8();
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    return doc.isObject() ? doc.object() : QJsonObject{};
}

QString normalizeManualIp(const QString& ip)
{
    const QHostAddress address(ip.trimmed());
    if (address.isNull())
        return QString();

    return address.toString();
}

QStringList sanitizeRecentManualIps(const QStringList& ips)
{
    QStringList sanitized;
    for (const auto& ip : ips) {
        const QString normalized = normalizeManualIp(ip);
        if (normalized.isEmpty() || sanitized.contains(normalized))
            continue;

        sanitized.append(normalized);
        if (sanitized.size() >= kMaxRecentManualIps)
            break;
    }
    return sanitized;
}

QStringList loadRecentManualIpSettings()
{
    QStringList ips;
    const QByteArray json =
        AppSettings::instance().value(kRecentManualIpsKey, "[]").toString().toUtf8();
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (doc.isArray()) {
        const QJsonArray array = doc.array();
        for (const auto& item : array)
            ips.append(item.toString());
    }

    if (ips.isEmpty()) {
        const QString legacyLastIp =
            AppSettings::instance().value("LastRoutedRadioIp").toString();
        if (!legacyLastIp.isEmpty())
            ips.append(legacyLastIp);
    }

    return sanitizeRecentManualIps(ips);
}

void saveRecentManualIpSettings(const QStringList& ips)
{
    QJsonArray array;
    for (const auto& ip : sanitizeRecentManualIps(ips))
        array.append(ip);

    auto& settings = AppSettings::instance();
    settings.setValue(kRecentManualIpsKey,
                      QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact)));
    settings.save();
}

void saveRoutedProfiles(const QJsonObject& profiles)
{
    auto& settings = AppSettings::instance();
    settings.setValue("RoutedProfilesJson",
                      QString::fromUtf8(QJsonDocument(profiles).toJson(QJsonDocument::Compact)));
    settings.save();
}

RadioBindSettings bindSettingsFromProfile(const QJsonObject& profile)
{
    const QJsonObject bind = profile.value("bind").toObject();
    RadioBindSettings settings;
    settings.mode = bind.value("mode").toString() == "explicit"
        ? RadioBindMode::Explicit
        : RadioBindMode::Auto;
    settings.interfaceId = bind.value("interface_id").toString();
    settings.interfaceName = bind.value("interface_name").toString();
    settings.bindAddress = QHostAddress(bind.value("last_successful_ipv4").toString());
    return settings;
}

QString staleSelectionText(const RadioBindSettings& settings)
{
    QString iface = settings.interfaceName.trimmed();
    if (iface.isEmpty())
        iface = settings.interfaceId.trimmed();
    if (iface.isEmpty())
        iface = QStringLiteral("Saved source");
    const QString addr = settings.bindAddress.isNull()
        ? QStringLiteral("unknown IPv4")
        : settings.bindAddress.toString();
    return QStringLiteral("%1 (unavailable, last %2)").arg(iface, addr);
}

QLabel* makeWrappedLabel(const QString& text, const char* style = nullptr)
{
    auto* label = new QLabel(text);
    label->setWordWrap(true);
    if (style)
        label->setStyleSheet(style);
    return label;
}

QString smartLinkUserText(const SmartLinkClient* client)
{
    if (!client)
        return QStringLiteral("Sign in to see radios at remote stations.");

    if (!client->firstName().isEmpty()) {
        const QString call = client->callsign().trimmed();
        if (!call.isEmpty()) {
            return QStringLiteral("%1 %2 (%3)")
                .arg(client->firstName().trimmed(),
                     client->lastName().trimmed(),
                     call);
        }
        return QStringLiteral("%1 %2")
            .arg(client->firstName().trimmed(), client->lastName().trimmed());
    }

    if (!client->callsign().trimmed().isEmpty())
        return QStringLiteral("Signed in as %1").arg(client->callsign().trimmed());

    return QStringLiteral("Signed in to SmartLink");
}

QString normalizedStatus(QString status)
{
    status.replace('_', ' ');
    return status.trimmed();
}

}

ConnectionPanel::ConnectionPanel(QWidget* parent)
    : QWidget(parent)
{
    setStyleSheet(
        "ConnectionPanel { background: #0f0f1a; }"
        "QGroupBox { border: 1px solid #304050; border-radius: 7px; margin-top: 10px; "
        "color: #c8d8e8; font-weight: bold; }"
        "QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 4px; }"
        "QListWidget { background: #09111b; border: 1px solid #304050; border-radius: 4px; "
        "color: #d7e4f2; padding: 2px; }"
        "QPushButton { padding: 5px 12px; }");

    const QString editStyle =
        "QLineEdit { border: 1px solid #304050; border-radius: 4px; padding: 4px 6px; "
        "background: #09111b; color: #d7e4f2; }";
    const QString comboStyle =
        "QComboBox { border: 1px solid #304050; border-radius: 4px; padding: 0; "
        "background: #09111b; color: #d7e4f2; }"
        "QComboBox::drop-down { width: 24px; border-left: 1px solid #304050; }"
        "QComboBox::down-arrow { image: none; width: 0; height: 0; margin-right: 6px; "
        "border-left: 4px solid transparent; border-right: 4px solid transparent; "
        "border-top: 5px solid #8aa8c0; }"
        "QComboBox QLineEdit { border: none; padding: 4px 6px; "
        "background: #09111b; color: #d7e4f2; }"
        "QComboBox QAbstractItemView { background: #09111b; color: #d7e4f2; "
        "selection-background-color: #1a3046; border: 1px solid #304050; }";
    const QString modeCardStyle =
        "QCommandLinkButton { text-align: left; border: 1px solid #304050; border-radius: 8px; "
        "padding: 10px 12px; background: #121a25; color: #d7e4f2; }"
        "QCommandLinkButton:hover { border-color: #4e6a86; background: #172334; }"
        "QCommandLinkButton:checked { border-color: #66a8ff; background: #1a3046; }";
    const QString calloutStyle =
        "QFrame#connectionCallout { border: 1px solid #304050; border-radius: 8px; "
        "background: #121a25; }"
        "QFrame#connectionCallout QLabel { background: transparent; border: none; }"
        "QFrame#connectionCallout QCheckBox { background: transparent; border: none; }";
    const QString lowBandwidthCheckStyle =
        "QCheckBox { color: #d7e4f2; spacing: 8px; padding: 2px 0; "
        "background: transparent; border: none; }"
        "QCheckBox::indicator { width: 16px; height: 16px; "
        "border: 2px solid #5d748d; border-radius: 3px; background: #0b1520; }"
        "QCheckBox::indicator:hover { border-color: #81abd9; background: #142130; }"
        "QCheckBox::indicator:checked { border: 2px solid #8cc8ff; background: #2f71b6; }"
        "QCheckBox::indicator:disabled { border-color: #405262; background: #10161d; }";

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    auto* titleBar = new FramelessWindowTitleBar(QStringLiteral("Connect to Radio"), this);
    m_titleBar = titleBar;
    outer->addWidget(titleBar);

    auto* content = new QWidget(this);
    auto* root = new QVBoxLayout(content);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(10);
    m_rootLayout = root;
    outer->addWidget(content, 1);

    auto* titleLabel = new QLabel("Connect to a Radio", this);
    titleLabel->setStyleSheet(
        "QLabel { color: #e7f1fb; font-size: 18px; font-weight: bold; "
        "background: transparent; border: none; }");
    root->addWidget(titleLabel);

    auto* introLabel = makeWrappedLabel(
        "Pick the simplest path for your station. Most first-time users should start with "
        "\"On This Network\" and only use the IP path for VPN or routed connections.",
        kHintLabelStyle);
    root->addWidget(introLabel);

    m_modeButtons = new QButtonGroup(this);
    m_modeButtons->setExclusive(true);

    auto configureModeButton = [&](QCommandLinkButton* button,
                                   const QString& title,
                                   const QString& description,
                                   ConnectionMode mode) {
        button->setText(title);
        button->setDescription(description);
        button->setCheckable(true);
        button->setStyleSheet(modeCardStyle);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        button->setMinimumHeight(100);
        m_modeButtons->addButton(button, static_cast<int>(mode));
    };

    auto* modeRow = new QHBoxLayout;
    modeRow->setSpacing(8);

    m_localModeBtn = new QCommandLinkButton(this);
    configureModeButton(m_localModeBtn,
                        "On This Network",
                        "Recommended for new users when the radio and computer are on the same LAN.",
                        LocalMode);
    modeRow->addWidget(m_localModeBtn);

    m_smartLinkModeBtn = new QCommandLinkButton(this);
    configureModeButton(m_smartLinkModeBtn,
                        "Remote with SmartLink",
                        "Use FlexRadio SmartLink when the radio is away from this computer.",
                        SmartLinkMode);
    modeRow->addWidget(m_smartLinkModeBtn);

    m_manualModeBtn = new QCommandLinkButton(this);
    configureModeButton(m_manualModeBtn,
                        "Connect by IP",
                        "Best for VPN or routed station access when you already know the radio IP.",
                        ManualMode);
    modeRow->addWidget(m_manualModeBtn);

    root->addLayout(modeRow);

    m_modeStack = new QStackedWidget(this);
    root->addWidget(m_modeStack, 1);

    // ── Local page ────────────────────────────────────────────────────────
    auto* localPage = new QWidget(m_modeStack);
    auto* localLayout = new QVBoxLayout(localPage);
    localLayout->setContentsMargins(0, 0, 0, 0);
    localLayout->setSpacing(8);
    localLayout->addWidget(makeWrappedLabel(
        "Discovery finds radios automatically on your local network. If nothing appears, "
        "guest Wi-Fi isolation, VPN software, or firewall rules may be blocking discovery.",
        kHintLabelStyle));

    m_localStateStack = new QStackedWidget(localPage);
    localLayout->addWidget(m_localStateStack, 1);

    auto* localListPage = new QWidget(m_localStateStack);
    auto* localListLayout = new QVBoxLayout(localListPage);
    localListLayout->setContentsMargins(0, 0, 0, 0);
    localListLayout->setSpacing(8);
    auto* localGroup = new QGroupBox("Available radios", localListPage);
    auto* localGroupLayout = new QVBoxLayout(localGroup);
    m_radioList = new QListWidget(localGroup);
    m_radioList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_radioList->setWordWrap(true);
    m_radioList->setSpacing(2);
    m_radioList->setMinimumHeight(220);
    localGroupLayout->addWidget(m_radioList);
    localListLayout->addWidget(localGroup, 1);

    auto* localActionRow = new QHBoxLayout;
    localActionRow->addStretch();
    m_localConnectBtn = new QPushButton("Connect Selected Radio", localListPage);
    m_localConnectBtn->setEnabled(false);
    localActionRow->addWidget(m_localConnectBtn);
    localListLayout->addLayout(localActionRow);
    m_localStateStack->addWidget(localListPage);

    m_localEmptyState = new QWidget(m_localStateStack);
    auto* emptyLayout = new QVBoxLayout(m_localEmptyState);
    emptyLayout->setContentsMargins(0, 0, 0, 0);
    emptyLayout->setSpacing(8);
    auto* emptyCallout = new QFrame(m_localEmptyState);
    emptyCallout->setObjectName("connectionCallout");
    emptyCallout->setStyleSheet(calloutStyle);
    auto* emptyCalloutLayout = new QVBoxLayout(emptyCallout);
    emptyCalloutLayout->setContentsMargins(14, 14, 14, 14);
    emptyCalloutLayout->setSpacing(8);
    auto* emptyTitle = new QLabel("No local radios found yet", emptyCallout);
    emptyTitle->setStyleSheet(
        "QLabel { color: #e7f1fb; font-size: 15px; font-weight: bold; "
        "background: transparent; border: none; }");
    emptyCalloutLayout->addWidget(emptyTitle);
    emptyCalloutLayout->addWidget(makeWrappedLabel(
        "AetherSDR is still listening for discovery packets. If your station is on a VPN "
        "or another routed network, switch to \"Connect by IP\" instead.",
        kHintLabelStyle));

    auto* retryBtn = new QPushButton("Retry Discovery", emptyCallout);
    auto* useSmartLinkBtn = new QPushButton("Remote with SmartLink", emptyCallout);
    auto* connectByIpBtn = new QPushButton("Connect by IP", emptyCallout);
    auto* diagnosticsBtn = new QPushButton("Open Network Diagnostics", emptyCallout);
    emptyCalloutLayout->addWidget(retryBtn);
    emptyCalloutLayout->addWidget(connectByIpBtn);
    emptyCalloutLayout->addWidget(useSmartLinkBtn);
    emptyCalloutLayout->addWidget(diagnosticsBtn);
    emptyLayout->addWidget(emptyCallout);
    emptyLayout->addStretch();
    m_localStateStack->addWidget(m_localEmptyState);

    m_modeStack->addWidget(localPage);

    // ── SmartLink page ────────────────────────────────────────────────────
    auto* smartLinkPage = new QWidget(m_modeStack);
    auto* smartLinkLayout = new QVBoxLayout(smartLinkPage);
    smartLinkLayout->setContentsMargins(0, 0, 0, 0);
    smartLinkLayout->setSpacing(8);
    smartLinkLayout->addWidget(makeWrappedLabel(
        "Use SmartLink when the radio is at another location. Sign in, choose a remote radio, "
        "then connect over the internet.",
        kHintLabelStyle));

    auto* accountGroup = new QGroupBox("SmartLink account", smartLinkPage);
    auto* accountLayout = new QVBoxLayout(accountGroup);
    accountLayout->setSpacing(6);

    m_loginForm = new QWidget(accountGroup);
    auto* loginLayout = new QFormLayout(m_loginForm);
    loginLayout->setContentsMargins(0, 0, 0, 0);
    loginLayout->setHorizontalSpacing(8);
    loginLayout->setVerticalSpacing(6);
    // Accessibility + password-manager hints.  macOS Passwords, Windows
    // Authenticator, and KDE Wallet read the OS accessibility tree to
    // associate credential fields; the objectName + accessibleName +
    // accessibleDescription tuple is what they look for.  m_loginForm
    // is named so the password manager can scope the credential pair
    // ("SmartLink login form" vs "MQTT login form").
    m_loginForm->setObjectName(QStringLiteral("smartlinkLoginForm"));
    m_loginForm->setAccessibleName(tr("SmartLink account login"));

    m_emailEdit = new QLineEdit(m_loginForm);
    m_emailEdit->setStyleSheet(editStyle);
    m_emailEdit->setPlaceholderText("flexradio account email");
    m_emailEdit->setObjectName(QStringLiteral("smartlinkEmail"));
    m_emailEdit->setAccessibleName(tr("SmartLink account email"));
    m_emailEdit->setAccessibleDescription(
        tr("FlexRadio account email address used to sign in to SmartLink"));
    QString storedEmail = AppSettings::instance().value("SmartLinkEmail").toString();
    if (!storedEmail.isEmpty())
        m_emailEdit->setText(QString::fromUtf8(QByteArray::fromBase64(storedEmail.toUtf8())));
    m_passwordEdit = new QLineEdit(m_loginForm);
    m_passwordEdit->setStyleSheet(editStyle);
    m_passwordEdit->setEchoMode(QLineEdit::Password);
    m_passwordEdit->setPlaceholderText("password");
    m_passwordEdit->setObjectName(QStringLiteral("smartlinkPassword"));
    m_passwordEdit->setAccessibleName(tr("SmartLink account password"));
    m_passwordEdit->setAccessibleDescription(
        tr("FlexRadio account password used to sign in to SmartLink"));
    loginLayout->addRow("Email:", m_emailEdit);
    loginLayout->addRow("Password:", m_passwordEdit);
    m_loginBtn = new QPushButton("Sign In", m_loginForm);
    loginLayout->addRow(QString(), m_loginBtn);
    accountLayout->addWidget(m_loginForm);

    auto* accountActionBar = new QWidget(accountGroup);
    auto* accountActionRow = new QHBoxLayout(accountActionBar);
    accountActionRow->setContentsMargins(0, 0, 0, 0);
    accountActionRow->setSpacing(10);

    m_logoutBtn = new QPushButton("Sign Out", accountActionBar);
    m_logoutBtn->setVisible(false);
    accountActionRow->addWidget(m_logoutBtn);
    m_wanDisconnectClientsBtn = new QPushButton("Disconnect Remote Clients", accountActionBar);
    m_wanDisconnectClientsBtn->setVisible(false);
    m_wanDisconnectClientsBtn->setEnabled(false);
    accountActionRow->addWidget(m_wanDisconnectClientsBtn);
    accountActionRow->addStretch();
    accountLayout->addWidget(accountActionBar);

    m_slUserLabel = makeWrappedLabel("Sign in to see radios at remote stations.", kHintLabelStyle);
    accountLayout->addWidget(m_slUserLabel);
    smartLinkLayout->addWidget(accountGroup);

    auto* remoteGroup = new QGroupBox("Remote radios", smartLinkPage);
    auto* remoteLayout = new QVBoxLayout(remoteGroup);
    remoteLayout->setSpacing(10);
    m_wanList = new QListWidget(remoteGroup);
    m_wanList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_wanList->setWordWrap(true);
    m_wanList->setSpacing(2);
    m_wanList->setMinimumHeight(120);
    m_wanList->setMaximumHeight(160);
    m_wanList->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    remoteLayout->addWidget(m_wanList);
    m_smartLinkEmptyLabel = makeWrappedLabel(
        "Remote radios appear here after SmartLink sign-in.",
        kHintLabelStyle);
    remoteLayout->addWidget(m_smartLinkEmptyLabel);
    smartLinkLayout->addWidget(remoteGroup);

    auto* wanActionBar = new QWidget(smartLinkPage);
    auto* wanActionRow = new QHBoxLayout(wanActionBar);
    wanActionRow->setContentsMargins(0, 0, 0, 0);
    wanActionRow->setSpacing(10);
    wanActionRow->addStretch();
    m_wanConnectBtn = new QPushButton("Connect Remote Radio", wanActionBar);
    m_wanConnectBtn->setEnabled(false);
    m_wanConnectBtn->setMinimumWidth(190);
    wanActionRow->addWidget(m_wanConnectBtn);
    smartLinkLayout->addWidget(wanActionBar);
    smartLinkLayout->addStretch(1);

    m_modeStack->addWidget(smartLinkPage);

    // ── Manual / VPN page ────────────────────────────────────────────────
    auto* manualPage = new QWidget(m_modeStack);
    auto* manualLayout = new QVBoxLayout(manualPage);
    manualLayout->setContentsMargins(0, 0, 0, 0);
    manualLayout->setSpacing(8);
    manualLayout->addWidget(makeWrappedLabel(
        "Use this path for VPN or other routed networks where discovery broadcasts cannot reach "
        "the radio. Enter the radio IP address and AetherSDR will take care of the probe.",
        kHintLabelStyle));

    auto* manualGroup = new QGroupBox("Radio IP address", manualPage);
    auto* manualGroupLayout = new QVBoxLayout(manualGroup);
    manualGroupLayout->setSpacing(8);
    auto* manualForm = new QFormLayout;
    manualForm->setHorizontalSpacing(8);
    manualForm->setVerticalSpacing(6);
    m_manualIpCombo = new QComboBox(manualGroup);
    m_manualIpCombo->setEditable(true);
    m_manualIpCombo->setInsertPolicy(QComboBox::NoInsert);
    m_manualIpCombo->setMaxVisibleItems(kMaxRecentManualIps);
    m_manualIpCombo->setStyleSheet(comboStyle);
    m_manualIpEdit = m_manualIpCombo->lineEdit();
    m_manualIpEdit->setClearButtonEnabled(true);
    m_manualIpEdit->setPlaceholderText("Example: 10.0.0.25");
    manualForm->addRow("Radio IP:", m_manualIpCombo);
    manualGroupLayout->addLayout(manualForm);

    auto* manualActionRow = new QHBoxLayout;
    auto* manualDiagnosticsBtn = new QPushButton("Network Diagnostics", manualGroup);
    manualActionRow->addWidget(manualDiagnosticsBtn);
    manualActionRow->addStretch();
    m_manualConnectBtn = new QPushButton("Connect by IP", manualGroup);
    manualActionRow->addWidget(m_manualConnectBtn);
    manualGroupLayout->addLayout(manualActionRow);

    m_manualResultLabel = makeWrappedLabel(QString(), kHintLabelStyle);
    m_manualResultLabel->setVisible(false);
    manualGroupLayout->addWidget(m_manualResultLabel);

    m_manualAdvancedToggle = new QToolButton(manualGroup);
    m_manualAdvancedToggle->setText("Advanced: choose the VPN source path");
    m_manualAdvancedToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_manualAdvancedToggle->setCheckable(true);
    m_manualAdvancedToggle->setArrowType(Qt::RightArrow);
    m_manualAdvancedToggle->setVisible(false);
    manualGroupLayout->addWidget(m_manualAdvancedToggle, 0, Qt::AlignLeft);

    m_manualAdvancedWidget = new QWidget(manualGroup);
    auto* manualAdvancedLayout = new QVBoxLayout(m_manualAdvancedWidget);
    manualAdvancedLayout->setContentsMargins(8, 4, 8, 4);
    manualAdvancedLayout->setSpacing(6);
    manualAdvancedLayout->addWidget(makeWrappedLabel(
        "Most users can leave this on Auto. Pick a source path only if your VPN creates more "
        "than one active network adapter or a saved path is no longer available.",
        kHintLabelStyle));
    auto* sourceRow = new QHBoxLayout;
    sourceRow->setContentsMargins(0, 0, 0, 0);
    sourceRow->addWidget(new QLabel("Source path:", m_manualAdvancedWidget));
    m_manualSourceCombo = new QComboBox(m_manualAdvancedWidget);
    sourceRow->addWidget(m_manualSourceCombo, 1);
    manualAdvancedLayout->addLayout(sourceRow);
    m_manualSourceWarningLabel = makeWrappedLabel(QString(), kErrorLabelStyle);
    m_manualSourceWarningLabel->setVisible(false);
    manualAdvancedLayout->addWidget(m_manualSourceWarningLabel);
    m_manualAdvancedWidget->setVisible(false);
    manualGroupLayout->addWidget(m_manualAdvancedWidget);
    manualLayout->addWidget(manualGroup);
    manualLayout->addStretch();

    m_modeStack->addWidget(manualPage);

    // ── Contextual options ────────────────────────────────────────────────
    m_linkOptionsWidget = new QFrame(this);
    m_linkOptionsWidget->setObjectName("connectionCallout");
    m_linkOptionsWidget->setStyleSheet(calloutStyle);
    auto* optionsLayout = new QVBoxLayout(m_linkOptionsWidget);
    optionsLayout->setContentsMargins(12, 10, 12, 10);
    optionsLayout->setSpacing(6);
    auto* optionsTitle = new QLabel("Connection options for slower links", m_linkOptionsWidget);
    optionsTitle->setStyleSheet(
        "QLabel { color: #e7f1fb; font-weight: bold; background: transparent; border: none; }");
    optionsLayout->addWidget(optionsTitle);
    m_lowBwHintLabel = makeWrappedLabel(QString(), kHintLabelStyle);
    optionsLayout->addWidget(m_lowBwHintLabel);
    m_lowBwCheck = new QCheckBox("Use low bandwidth mode", m_linkOptionsWidget);
    const auto remoteLowBandwidth = AppSettings::instance()
        .value("LowBandwidthRemotePreferred",
               AppSettings::instance().value("LowBandwidthConnect", "False"))
        .toString();
    m_lowBwCheck->setChecked(remoteLowBandwidth == "True");
    m_lowBwCheck->setToolTip("Reduces FFT and waterfall traffic from the radio.");
    m_lowBwCheck->setStyleSheet(lowBandwidthCheckStyle);
    optionsLayout->addWidget(m_lowBwCheck);
    root->addWidget(m_linkOptionsWidget);

    m_autoConnectCheck = new QCheckBox("Connect to last radio on start up", this);
    m_autoConnectCheck->setChecked(
        AppSettings::instance().value("AutoConnectToLastRadio", "True").toString() == "True");
    m_autoConnectCheck->setStyleSheet(lowBandwidthCheckStyle);
    connect(m_autoConnectCheck, &QCheckBox::toggled, this, [](bool on) {
        auto& s = AppSettings::instance();
        s.setValue("AutoConnectToLastRadio", on ? "True" : "False");
        s.save();
    });
    root->addWidget(m_autoConnectCheck);

    // ── Footer ────────────────────────────────────────────────────────────
    auto* footerRow = new QHBoxLayout;
    footerRow->setSpacing(8);
    m_statusLabel = makeWrappedLabel("Choose how you want to connect.", kHintLabelStyle);
    footerRow->addWidget(m_statusLabel, 1);
    m_disconnectBtn = new QPushButton("Disconnect", this);
    m_disconnectBtn->setVisible(false);
    footerRow->addWidget(m_disconnectBtn, 0, Qt::AlignRight);
    root->addLayout(footerRow);

    loadRecentManualIps();
    applySavedSourceSelection(m_manualIpEdit->text().trimmed());

    connect(m_modeButtons, QOverload<int>::of(&QButtonGroup::idClicked),
            this, &ConnectionPanel::onConnectionModeClicked);

    connect(retryBtn, &QPushButton::clicked, this, [this] {
        setStatusText("Refreshing local discovery…");
        emit retryDiscoveryRequested();
    });
    connect(connectByIpBtn, &QPushButton::clicked, this, [this] {
        setCurrentMode(ManualMode);
        m_manualIpEdit->setFocus();
        m_manualIpEdit->selectAll();
    });
    connect(useSmartLinkBtn, &QPushButton::clicked, this, [this] {
        setCurrentMode(SmartLinkMode);
        if (m_loginForm->isVisible())
            m_emailEdit->setFocus();
    });
    connect(diagnosticsBtn, &QPushButton::clicked,
            this, &ConnectionPanel::networkDiagnosticsRequested);
    connect(manualDiagnosticsBtn, &QPushButton::clicked,
            this, &ConnectionPanel::networkDiagnosticsRequested);

    connect(m_radioList, &QListWidget::itemSelectionChanged,
            this, &ConnectionPanel::onListSelectionChanged);
    connect(m_wanList, &QListWidget::itemSelectionChanged,
            this, &ConnectionPanel::onWanSelectionChanged);
    connect(m_localConnectBtn, &QPushButton::clicked,
            this, &ConnectionPanel::onLocalConnectClicked);
    connect(m_wanConnectBtn, &QPushButton::clicked,
            this, &ConnectionPanel::onWanConnectClicked);
    connect(m_wanDisconnectClientsBtn, &QPushButton::clicked,
            this, &ConnectionPanel::onWanDisconnectClientsClicked);
    connect(m_disconnectBtn, &QPushButton::clicked,
            this, &ConnectionPanel::disconnectRequested);
    connect(m_radioList, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem*) {
        if (!m_connected)
            onLocalConnectClicked();
    });
    connect(m_wanList, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem*) {
        if (!m_connected)
            onWanConnectClicked();
    });

    connect(m_manualConnectBtn, &QPushButton::clicked,
            this, &ConnectionPanel::onManualConnectClicked);
    connect(m_manualIpEdit, &QLineEdit::returnPressed,
            this, &ConnectionPanel::onManualConnectClicked);
    connect(m_manualIpEdit, &QLineEdit::textChanged,
            this, &ConnectionPanel::onManualIpChanged);
    connect(m_manualAdvancedToggle, &QToolButton::toggled,
            this, &ConnectionPanel::onManualAdvancedToggled);
    connect(m_manualSourceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) {
                updateManualAdvancedVisibility();
                setManualMessage(QString());
            });

    const auto doLogin = [this] {
        const QString email = m_emailEdit->text().trimmed();
        const QString pass = m_passwordEdit->text();
        if (email.isEmpty() || pass.isEmpty())
            return;
        m_loginBtn->setEnabled(false);
        m_loginBtn->setText("Signing In...");
        emit smartLinkLoginRequested(email, pass);
    };
    connect(m_loginBtn, &QPushButton::clicked, this, doLogin);
    connect(m_passwordEdit, &QLineEdit::returnPressed, this, doLogin);
    connect(m_logoutBtn, &QPushButton::clicked, this, [this] {
        if (!m_smartLink)
            return;
        m_smartLink->logout();
        m_wanRadios.clear();
        m_wanList->clear();
        m_slUserLabel->setText("Signed out of SmartLink.");
        m_slUserLabel->setStyleSheet(kHintLabelStyle);
        updateSmartLinkUi();
    });

    setConnected(false);
    setCurrentMode(LocalMode);
    updateLocalPageState();
    updateSmartLinkUi();
    updateManualAdvancedVisibility();
    FramelessResizer::install(this);
    setFramelessMode(
        AppSettings::instance().value("FramelessWindow", "True").toString() == "True");
}

void ConnectionPanel::setFramelessMode(bool on)
{
    const QRect geom = geometry();
    const bool wasVisible = isVisible();

    Qt::WindowFlags flags = (windowFlags() & ~Qt::WindowType_Mask) | Qt::Dialog;
    flags.setFlag(Qt::FramelessWindowHint, on);
    setWindowFlags(flags);
    if (wasVisible)
        setGeometry(geom);
    if (m_titleBar)
        m_titleBar->setVisible(on);
    if (m_rootLayout)
        m_rootLayout->setContentsMargins(12, on ? 10 : 12, 12, 12);
    if (wasVisible)
        show();
}

void ConnectionPanel::setConnected(bool connected)
{
    m_connected = connected;
    m_disconnectBtn->setVisible(connected);
    updateActionState();
}

void ConnectionPanel::setStatusText(const QString& text)
{
    m_statusLabel->setText(text);
}

void ConnectionPanel::saveLowBandwidthPreference(bool enabled)
{
    auto& settings = AppSettings::instance();
    const QString value = enabled ? QStringLiteral("True") : QStringLiteral("False");
    settings.setValue("LowBandwidthConnect", value);
    settings.setValue("LowBandwidthRemotePreferred", value);
    settings.save();
}

QString ConnectionPanel::formatLocalRadioLabel(const RadioInfo& radio) const
{
    QString title = radio.model.trimmed();
    if (!radio.nickname.trimmed().isEmpty())
        title += QStringLiteral("  %1").arg(radio.nickname.trimmed());
    if (!radio.callsign.trimmed().isEmpty())
        title += QStringLiteral("  %1").arg(radio.callsign.trimmed());
    if (title.trimmed().isEmpty())
        title = QStringLiteral("Radio");
    if (title == radio.model.trimmed() && !radio.address.isNull())
        title += QStringLiteral("  %1").arg(radio.address.toString());

    QString detail;
    if (!radio.guiClientStations.isEmpty()) {
        const QString station = radio.guiClientStations.first().trimmed();
        detail = station.isEmpty()
            ? QStringLiteral("Shared radio on your network via multiFLEX")
            : QStringLiteral("Shared radio on your network via multiFLEX at %1").arg(station);
    } else if (!radio.address.isNull()) {
        detail = QStringLiteral("Ready on your local network • %1").arg(radio.address.toString());
    } else {
        detail = QStringLiteral("Ready on your local network");
    }

    if (!radio.turfRegion.trimmed().isEmpty())
        detail += QStringLiteral(" • %1").arg(radio.turfRegion.trimmed());

    return title + QLatin1Char('\n') + detail;
}

QString ConnectionPanel::formatWanRadioLabel(const WanRadioInfo& radio) const
{
    QString title = radio.model.trimmed();
    if (!radio.nickname.trimmed().isEmpty())
        title += QStringLiteral("  %1").arg(radio.nickname.trimmed());
    if (!radio.callsign.trimmed().isEmpty())
        title += QStringLiteral("  %1").arg(radio.callsign.trimmed());
    if (title.trimmed().isEmpty())
        title = QStringLiteral("SmartLink radio");

    QString detail = QStringLiteral("Remote via SmartLink");
    const QString status = normalizedStatus(radio.status);
    if (!status.isEmpty() && status.compare(QStringLiteral("Available"), Qt::CaseInsensitive) != 0)
        detail += QStringLiteral(" • %1").arg(status);
    else
        detail += QStringLiteral(" • Ready to connect");

    if (!radio.guiClientStations.trimmed().isEmpty())
        detail += QStringLiteral(" • station %1").arg(radio.guiClientStations.trimmed());

    return title + QLatin1Char('\n') + detail;
}

void ConnectionPanel::setManualMessage(const QString& text, bool error)
{
    if (text.trimmed().isEmpty()) {
        m_manualResultLabel->clear();
        m_manualResultLabel->setVisible(false);
        return;
    }

    m_manualResultLabel->setText(text);
    m_manualResultLabel->setStyleSheet(error ? kErrorLabelStyle : kInfoLabelStyle);
    m_manualResultLabel->setVisible(true);
}

void ConnectionPanel::updateLocalPageState()
{
    const bool hasRadios = !m_radios.isEmpty();
    m_localStateStack->setCurrentIndex(hasRadios ? 0 : 1);

    if (hasRadios && !m_radioList->currentItem())
        m_radioList->setCurrentRow(0);

    updateActionState();
}

void ConnectionPanel::updateSmartLinkUi()
{
    const bool authed = m_smartLink && m_smartLink->isAuthenticated();
    const bool hasWanRadios = authed && !m_wanRadios.isEmpty();

    m_loginForm->setVisible(!authed);
    m_logoutBtn->setVisible(authed);
    m_wanDisconnectClientsBtn->setVisible(authed);
    m_wanList->setVisible(hasWanRadios);
    m_wanConnectBtn->setVisible(authed);

    if (authed) {
        if (m_slUserLabel->text().trimmed().isEmpty()
            || m_slUserLabel->styleSheet() == QString::fromLatin1(kHintLabelStyle)) {
            m_slUserLabel->setText(smartLinkUserText(m_smartLink));
            m_slUserLabel->setStyleSheet(kInfoLabelStyle);
        }
        if (hasWanRadios) {
            m_smartLinkEmptyLabel->setVisible(false);
        } else {
            m_smartLinkEmptyLabel->setText(
                "No SmartLink radios are available right now. If the station is on your current "
                "LAN, it will appear on the local page instead.");
            m_smartLinkEmptyLabel->setVisible(true);
        }
    } else {
        if (m_slUserLabel->text().trimmed().isEmpty())
            m_slUserLabel->setText("Sign in to see radios at remote stations.");
        if (m_slUserLabel->styleSheet().isEmpty())
            m_slUserLabel->setStyleSheet(kHintLabelStyle);
        m_smartLinkEmptyLabel->setText("Remote radios appear here after SmartLink sign-in.");
        m_smartLinkEmptyLabel->setVisible(true);
    }

    if (hasWanRadios && !m_wanList->currentItem())
        m_wanList->setCurrentRow(0);

    updateActionState();
}

void ConnectionPanel::updateActionState()
{
    m_localConnectBtn->setEnabled(!m_connected && m_radioList->currentItem() != nullptr);

    const bool smartLinkReady = !m_connected
        && m_smartLink
        && m_smartLink->isAuthenticated()
        && m_wanList->currentItem() != nullptr;
    m_wanConnectBtn->setEnabled(smartLinkReady);

    const int wanRow = m_wanList->currentRow();
    const bool remoteClientsAvailable = wanRow >= 0
        && wanRow < m_wanRadios.size()
        && !m_wanRadios[wanRow].guiClientHandles.trimmed().isEmpty();
    const bool smartLinkDisconnectReady = m_smartLink
        && m_smartLink->isAuthenticated()
        && m_smartLink->isConnected()
        && remoteClientsAvailable;
    m_wanDisconnectClientsBtn->setEnabled(smartLinkDisconnectReady);

    const bool manualReady = !m_connected && !m_manualIpEdit->text().trimmed().isEmpty();
    m_manualConnectBtn->setEnabled(manualReady);
}

void ConnectionPanel::updateLowBandwidthVisibility()
{
    const auto mode = static_cast<ConnectionMode>(m_modeStack->currentIndex());
    const bool visible = mode == SmartLinkMode || mode == ManualMode;
    m_linkOptionsWidget->setVisible(visible);

    if (!visible)
        return;

    if (mode == SmartLinkMode) {
        m_lowBwHintLabel->setText(
            "Recommended for SmartLink, hotel Wi-Fi, LTE, or other internet paths where "
            "waterfall and FFT traffic may feel heavy.");
    } else {
        m_lowBwHintLabel->setText(
            "Helpful on VPN or routed links when the radio is reachable by IP but the network "
            "feels slower than a normal local LAN.");
    }
}

void ConnectionPanel::updateManualAdvancedVisibility()
{
    const auto candidates = NetworkPathResolver::enumerateIpv4Candidates();
    const RadioBindSettings settings = currentManualBindSettings();
    const bool hasExplicitSelection = settings.mode == RadioBindMode::Explicit;
    const bool showToggle = candidates.size() > 1
        || hasExplicitSelection
        || m_manualSourceWarningLabel->isVisible();

    m_manualAdvancedToggle->setVisible(showToggle);
    if (!showToggle) {
        if (m_manualAdvancedToggle->isChecked()) {
            const QSignalBlocker blocker(m_manualAdvancedToggle);
            m_manualAdvancedToggle->setChecked(false);
        }
        m_manualAdvancedToggle->setArrowType(Qt::RightArrow);
        m_manualAdvancedWidget->setVisible(false);
        return;
    }

    if (hasExplicitSelection || m_manualSourceWarningLabel->isVisible()) {
        const QSignalBlocker blocker(m_manualAdvancedToggle);
        m_manualAdvancedToggle->setChecked(true);
        m_manualAdvancedToggle->setArrowType(Qt::DownArrow);
        m_manualAdvancedWidget->setVisible(true);
        return;
    }

    m_manualAdvancedWidget->setVisible(m_manualAdvancedToggle->isChecked());
    m_manualAdvancedToggle->setArrowType(
        m_manualAdvancedToggle->isChecked() ? Qt::DownArrow : Qt::RightArrow);
}

void ConnectionPanel::setCurrentMode(ConnectionMode mode)
{
    if (m_modeButtons->checkedId() != static_cast<int>(mode)) {
        const QSignalBlocker blocker(m_modeButtons);
        if (auto* button = m_modeButtons->button(static_cast<int>(mode)))
            button->setChecked(true);
    }

    m_modeStack->setCurrentIndex(static_cast<int>(mode));
    updateLowBandwidthVisibility();
    updateActionState();
}

void ConnectionPanel::onConnectionModeClicked(int id)
{
    setCurrentMode(static_cast<ConnectionMode>(id));
}

void ConnectionPanel::onRadioDiscovered(const RadioInfo& radio)
{
    for (int i = 0; i < m_radios.size(); ++i) {
        if (m_radios[i].serial == radio.serial) {
            m_radios[i] = radio;
            if (auto* item = m_radioList->item(i))
                item->setText(formatLocalRadioLabel(radio));
            updateLocalPageState();
            return;
        }
    }

    m_radios.append(radio);
    m_radioList->addItem(formatLocalRadioLabel(radio));
    if (m_radioList->count() == 1)
        m_radioList->setCurrentRow(0);
    updateLocalPageState();
}

void ConnectionPanel::onRadioUpdated(const RadioInfo& radio)
{
    for (int i = 0; i < m_radios.size(); ++i) {
        if (m_radios[i].serial == radio.serial) {
            m_radios[i] = radio;
            if (auto* item = m_radioList->item(i))
                item->setText(formatLocalRadioLabel(radio));
            return;
        }
    }
}

void ConnectionPanel::onRadioLost(const QString& serial)
{
    for (int i = 0; i < m_radios.size(); ++i) {
        if (m_radios[i].serial == serial) {
            delete m_radioList->takeItem(i);
            m_radios.removeAt(i);
            break;
        }
    }

    updateLocalPageState();
}

void ConnectionPanel::onListSelectionChanged()
{
    updateActionState();
}

void ConnectionPanel::onWanSelectionChanged()
{
    updateActionState();
}

void ConnectionPanel::onLocalConnectClicked()
{
    const int row = m_radioList->currentRow();
    if (m_connected || row < 0 || row >= m_radios.size())
        return;

    auto& settings = AppSettings::instance();
    settings.setValue("LowBandwidthConnect", "False");
    settings.save();

    emit connectRequested(m_radios[row]);
}

void ConnectionPanel::onWanConnectClicked()
{
    const int row = m_wanList->currentRow();
    if (m_connected || row < 0 || row >= m_wanRadios.size())
        return;

    saveLowBandwidthPreference(m_lowBwCheck->isChecked());
    emit wanConnectRequested(m_wanRadios[row]);
}

void ConnectionPanel::onWanDisconnectClientsClicked()
{
    const int row = m_wanList->currentRow();
    if (row < 0 || row >= m_wanRadios.size())
        return;

    emit wanDisconnectClientsRequested(m_wanRadios[row]);
}

void ConnectionPanel::setSmartLinkClient(SmartLinkClient* client)
{
    m_smartLink = client;
    if (!client)
        return;

    connect(client, &SmartLinkClient::authenticated, this, [this] {
        m_passwordEdit->clear();
        m_loginBtn->setEnabled(true);
        m_loginBtn->setText("Sign In");
        m_slUserLabel->setText(smartLinkUserText(m_smartLink));
        m_slUserLabel->setStyleSheet(kInfoLabelStyle);
        updateSmartLinkUi();
    });

    connect(client, &SmartLinkClient::serverConnected, this, [this] {
        QTimer::singleShot(500, this, [this] {
            if (m_smartLink && m_smartLink->isAuthenticated()) {
                m_slUserLabel->setText(smartLinkUserText(m_smartLink));
                m_slUserLabel->setStyleSheet(kInfoLabelStyle);
                updateSmartLinkUi();
            }
        });
    });

    connect(client, &SmartLinkClient::serverDisconnected, this, [this] {
        if (!m_smartLink || !m_smartLink->isAuthenticated()) {
            if (m_slUserLabel->text().trimmed().isEmpty()) {
                m_slUserLabel->setText("Sign in to see radios at remote stations.");
                m_slUserLabel->setStyleSheet(kHintLabelStyle);
            }
        }
        updateSmartLinkUi();
    });

    connect(client, &SmartLinkClient::authFailed, this, [this](const QString& err) {
        m_passwordEdit->clear();
        m_loginBtn->setText("Sign In");
        m_loginBtn->setEnabled(true);
        m_slUserLabel->setText("SmartLink sign-in failed: " + err);
        m_slUserLabel->setStyleSheet(kErrorLabelStyle);
        updateSmartLinkUi();
    });

    connect(client, &SmartLinkClient::radioListReceived, this,
            [this](const QList<WanRadioInfo>& radios) {
        m_wanRadios.clear();
        m_wanList->clear();

        for (const auto& radio : radios) {
            bool isLanRadio = false;
            for (const auto& lan : m_radios) {
                if (lan.serial == radio.serial) {
                    isLanRadio = true;
                    break;
                }
            }
            if (isLanRadio)
                continue;

            m_wanRadios.append(radio);
            m_wanList->addItem(formatWanRadioLabel(radio));
        }

        if (m_wanList->count() > 0 && !m_wanList->currentItem())
            m_wanList->setCurrentRow(0);

        updateSmartLinkUi();
    });

    client->tryAutoLogin();
    updateSmartLinkUi();
}

bool ConnectionPanel::event(QEvent* e)
{
    return QWidget::event(e);
}

void ConnectionPanel::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), QColor(15, 15, 26));
}

void ConnectionPanel::refreshManualSourceOptions(const RadioBindSettings* selected)
{
    const QSignalBlocker blocker(m_manualSourceCombo);
    m_manualSourceCombo->clear();

    m_manualSourceCombo->addItem("Auto");
    m_manualSourceCombo->setItemData(0, static_cast<int>(RadioBindMode::Auto), kSourceModeRole);
    m_manualSourceCombo->setItemData(0, false, kSourceStaleRole);

    int selectedIndex = 0;
    const auto candidates = NetworkPathResolver::enumerateIpv4Candidates();
    for (const auto& candidate : candidates) {
        const int idx = m_manualSourceCombo->count();
        m_manualSourceCombo->addItem(candidate.label());
        m_manualSourceCombo->setItemData(idx, static_cast<int>(RadioBindMode::Explicit), kSourceModeRole);
        m_manualSourceCombo->setItemData(idx, candidate.interfaceId, kSourceInterfaceIdRole);
        m_manualSourceCombo->setItemData(idx, candidate.interfaceName, kSourceInterfaceNameRole);
        m_manualSourceCombo->setItemData(idx, candidate.address.toString(), kSourceAddressRole);
        m_manualSourceCombo->setItemData(idx, false, kSourceStaleRole);

        if (selected
            && selected->mode == RadioBindMode::Explicit
            && ((!selected->interfaceId.isEmpty() && selected->interfaceId == candidate.interfaceId)
                || (!selected->bindAddress.isNull() && selected->bindAddress == candidate.address))) {
            selectedIndex = idx;
        }
    }

    if (selected
        && selected->mode == RadioBindMode::Explicit
        && selectedIndex == 0) {
        selectedIndex = m_manualSourceCombo->count();
        m_manualSourceCombo->addItem(staleSelectionText(*selected));
        m_manualSourceCombo->setItemData(
            selectedIndex, static_cast<int>(RadioBindMode::Explicit), kSourceModeRole);
        m_manualSourceCombo->setItemData(selectedIndex, selected->interfaceId, kSourceInterfaceIdRole);
        m_manualSourceCombo->setItemData(selectedIndex, selected->interfaceName, kSourceInterfaceNameRole);
        m_manualSourceCombo->setItemData(selectedIndex, selected->bindAddress.toString(), kSourceAddressRole);
        m_manualSourceCombo->setItemData(selectedIndex, true, kSourceStaleRole);
    }

    m_manualSourceCombo->setCurrentIndex(selectedIndex);
    updateManualAdvancedVisibility();
}

void ConnectionPanel::applySavedSourceSelection(const QString& ip)
{
    const QString trimmedIp = ip.trimmed();
    m_manualProfileIp = trimmedIp;
    m_manualSourceWarningLabel->clear();
    m_manualSourceWarningLabel->setVisible(false);

    if (trimmedIp.isEmpty()) {
        refreshManualSourceOptions();
        updateManualAdvancedVisibility();
        return;
    }

    const QJsonObject profiles = loadRoutedProfiles();
    const QJsonObject profile = profiles.value(trimmedIp).toObject();
    if (profile.isEmpty()) {
        refreshManualSourceOptions();
        updateManualAdvancedVisibility();
        return;
    }

    RadioBindSettings settings = bindSettingsFromProfile(profile);
    if (settings.mode == RadioBindMode::Explicit) {
        const auto resolved = NetworkPathResolver::resolveExplicitSelection(
            settings.interfaceId, settings.interfaceName, settings.bindAddress);
        if (resolved.isValid()) {
            settings.interfaceId = resolved.interfaceId;
            settings.interfaceName = resolved.interfaceName;
            settings.bindAddress = resolved.address;
        } else {
            m_manualSourceWarningLabel->setText(
                QStringLiteral("Saved VPN source path for %1 is unavailable. Pick a live path "
                               "below before connecting.")
                    .arg(trimmedIp));
            m_manualSourceWarningLabel->setVisible(true);
        }
    }

    refreshManualSourceOptions(&settings);
    updateManualAdvancedVisibility();
}

RadioBindSettings ConnectionPanel::currentManualBindSettings(bool* staleSelection) const
{
    RadioBindSettings settings;
    const int index = m_manualSourceCombo->currentIndex();
    settings.mode = static_cast<RadioBindMode>(
        m_manualSourceCombo->itemData(index, kSourceModeRole).toInt());
    settings.interfaceId = m_manualSourceCombo->itemData(index, kSourceInterfaceIdRole).toString();
    settings.interfaceName = m_manualSourceCombo->itemData(index, kSourceInterfaceNameRole).toString();
    settings.bindAddress = QHostAddress(m_manualSourceCombo->itemData(index, kSourceAddressRole).toString());
    if (staleSelection)
        *staleSelection = m_manualSourceCombo->itemData(index, kSourceStaleRole).toBool();
    return settings;
}

void ConnectionPanel::loadRecentManualIps()
{
    const QStringList ips = loadRecentManualIpSettings();
    const QSignalBlocker comboBlocker(m_manualIpCombo);
    const QSignalBlocker editBlocker(m_manualIpEdit);

    m_manualIpCombo->clear();
    for (const auto& ip : ips)
        m_manualIpCombo->addItem(ip);

    if (!ips.isEmpty())
        m_manualIpCombo->setCurrentText(ips.first());
    else
        m_manualIpEdit->clear();
}

void ConnectionPanel::rememberManualIp(const QString& ip)
{
    const QString normalized = normalizeManualIp(ip);
    if (normalized.isEmpty())
        return;

    QStringList ips = loadRecentManualIpSettings();
    ips.removeAll(normalized);
    ips.prepend(normalized);
    const QStringList sanitized = sanitizeRecentManualIps(ips);
    saveRecentManualIpSettings(sanitized);

    const QSignalBlocker comboBlocker(m_manualIpCombo);
    const QSignalBlocker editBlocker(m_manualIpEdit);
    m_manualIpCombo->clear();
    for (const auto& recentIp : sanitized)
        m_manualIpCombo->addItem(recentIp);
    m_manualIpCombo->setCurrentText(normalized);
}

void ConnectionPanel::saveManualProfile(const QString& targetIp,
                                        const RadioBindSettings& settings,
                                        const QHostAddress& lastSuccessfulLocalIp)
{
    if (targetIp.trimmed().isEmpty())
        return;

    QJsonObject profiles = loadRoutedProfiles();
    QJsonObject profile;
    profile["schema_version"] = 1;

    QJsonObject identity;
    identity["target_address"] = targetIp;
    profile["identity"] = identity;

    QJsonObject bind;
    bind["mode"] = settings.mode == RadioBindMode::Explicit ? "explicit" : "auto";
    bind["interface_id"] = settings.interfaceId;
    bind["interface_name"] = settings.interfaceName;
    bind["last_successful_ipv4"] = lastSuccessfulLocalIp.toString();
    profile["bind"] = bind;

    profiles[targetIp] = profile;
    saveRoutedProfiles(profiles);
}

void ConnectionPanel::onManualIpChanged(const QString& ip)
{
    const QString trimmed = ip.trimmed();
    m_manualConnectPending = false;
    if (trimmed != m_manualProfileIp)
        applySavedSourceSelection(trimmed);
    setManualMessage(QString());
    updateActionState();
}

void ConnectionPanel::onManualConnectClicked()
{
    const QString ip = m_manualIpEdit->text().trimmed();
    if (m_connected || ip.isEmpty())
        return;

    m_manualConnectPending = true;
    setManualMessage(QStringLiteral("Checking %1…").arg(ip));
    probeRadio(ip);
}

void ConnectionPanel::onManualAdvancedToggled(bool checked)
{
    m_manualAdvancedToggle->setArrowType(checked ? Qt::DownArrow : Qt::RightArrow);
    m_manualAdvancedWidget->setVisible(checked);
}

void ConnectionPanel::probeRadio(const QString& ip)
{
    const QString trimmedIp = ip.trimmed();
    if (trimmedIp.isEmpty())
        return;

    if (m_manualIpEdit->text().trimmed() != trimmedIp) {
        m_manualIpEdit->setText(trimmedIp);
        applySavedSourceSelection(trimmedIp);
    } else if (m_manualProfileIp != trimmedIp) {
        applySavedSourceSelection(trimmedIp);
    }

    bool staleSelection = false;
    const RadioBindSettings bindSettings = currentManualBindSettings(&staleSelection);
    if (bindSettings.mode == RadioBindMode::Explicit && staleSelection) {
        m_manualSourceWarningLabel->setText(
            QStringLiteral("The selected VPN source path is unavailable. Choose a live path "
                           "before connecting."));
        m_manualSourceWarningLabel->setVisible(true);
        updateManualAdvancedVisibility();
        setManualMessage("Choose a live source path before trying again.", true);
        m_manualConnectPending = false;
        return;
    }

    const QString busyText = m_manualConnectPending
        ? QStringLiteral("Connecting...")
        : QStringLiteral("Checking...");
    m_manualConnectBtn->setEnabled(false);
    m_manualConnectBtn->setText(busyText);
    m_manualSourceWarningLabel->setVisible(false);
    updateManualAdvancedVisibility();

    auto* sock = new QTcpSocket(this);
    if (bindSettings.mode == RadioBindMode::Explicit
        && !sock->bind(bindSettings.bindAddress, 0)) {
        m_manualSourceWarningLabel->setText(
            QStringLiteral("Failed to bind %1: %2")
                .arg(bindSettings.bindAddress.toString(), sock->errorString()));
        m_manualSourceWarningLabel->setVisible(true);
        updateManualAdvancedVisibility();
        setManualMessage("AetherSDR could not use that VPN source path. Try Auto or choose another path.", true);
        sock->deleteLater();
        m_manualConnectPending = false;
        m_manualConnectBtn->setText("Connect by IP");
        updateActionState();
        return;
    }

    sock->connectToHost(trimmedIp, 4992);

    QTimer::singleShot(3000, sock, [this, sock, trimmedIp] {
        if (sock->state() != QAbstractSocket::ConnectedState) {
            sock->abort();
            sock->deleteLater();
            m_manualConnectPending = false;
            m_manualConnectBtn->setText("Connect by IP");
            updateActionState();
            setManualMessage(
                QStringLiteral("No radio responded at %1. If this is a VPN path, confirm the IP "
                               "address and try Advanced only if your VPN exposes multiple adapters.")
                    .arg(trimmedIp),
                true);
        }
    });

    connect(sock, &QTcpSocket::connected, this, [this, sock, trimmedIp, bindSettings] {
        // Shared state across readyRead calls and the peek timer.
        auto buffer      = std::make_shared<QByteArray>();
        auto version     = std::make_shared<QString>();
        auto statusLines = std::make_shared<QStringList>();
        auto localSrc    = std::make_shared<QHostAddress>();
        auto seenHandle  = std::make_shared<bool>(false);

        // Fired 400 ms after H is received; by then the radio has sent
        // its full radio + client status burst in response to our subs.
        auto* peekTimer = new QTimer(sock);
        peekTimer->setSingleShot(true);
        peekTimer->setInterval(400);

        // Build RadioInfo from collected status and emit the connect signal.
        auto finishProbe = [this, sock, trimmedIp, bindSettings, version, statusLines, localSrc] {
            sock->disconnectFromHost();
            sock->deleteLater();

            RadioInfo info;
            info.address  = QHostAddress(trimmedIp);
            info.port     = 4992;
            info.version  = *version;
            info.status   = QStringLiteral("Available");
            info.model    = QStringLiteral("FLEX");
            info.name     = QStringLiteral("FLEX");
            info.serial   = trimmedIp;
            info.isRouted = true;
            info.bindSettings       = bindSettings;
            info.sessionBindAddress = *localSrc;

            // Parse S-type status lines collected during the peek window.
            for (const QString& line : *statusLines) {
                // S<handle>|<object> [key=val ...]
                const int pipeIdx = line.indexOf('|');
                if (pipeIdx < 0)
                    continue;
                const QString body = line.mid(pipeIdx + 1);

                if (body.startsWith(QStringLiteral("radio "))) {
                    const QStringList parts = body.mid(6).split(' ', Qt::SkipEmptyParts);
                    for (const QString& part : parts) {
                        const int eq = part.indexOf('=');
                        if (eq < 0)
                            continue;
                        const QString key = part.left(eq);
                        const QString val = part.mid(eq + 1);
                        if (key == QStringLiteral("mf_enable")) {
                            info.multiFlexEnabled = (val != QStringLiteral("0"));
                        } else if (key == QStringLiteral("model") && !val.isEmpty()) {
                            info.model = val;
                            info.name  = val;
                        } else if (key == QStringLiteral("nickname") && !val.isEmpty()) {
                            info.nickname = val;
                        } else if (key == QStringLiteral("callsign") && !val.isEmpty()) {
                            info.callsign = val;
                        }
                    }
                } else if (body.startsWith(QStringLiteral("client 0x"))
                           && body.contains(QStringLiteral("connected"))) {
                    // "client 0x<handle> connected program=... station=..."
                    const QStringList tokens = body.split(' ', Qt::SkipEmptyParts);
                    if (tokens.size() >= 3 && tokens[2] == QStringLiteral("connected")) {
                        bool ok = false;
                        const quint32 handle = tokens[1].toUInt(&ok, 16);
                        if (!ok || handle == 0)
                            continue;
                        QString program;
                        QString station;
                        for (int i = 3; i < tokens.size(); ++i) {
                            const int eq = tokens[i].indexOf('=');
                            if (eq < 0)
                                continue;
                            const QString key = tokens[i].left(eq);
                            const QString val = tokens[i].mid(eq + 1);
                            if (key == QStringLiteral("program"))
                                program = val;
                            else if (key == QStringLiteral("station"))
                                station = val;
                        }
                        info.guiClientHandles.append(QString::number(handle, 16).toUpper());
                        info.guiClientPrograms.append(program);
                        info.guiClientStations.append(station);
                    }
                }
            }

            saveManualProfile(trimmedIp, bindSettings, *localSrc);
            rememberManualIp(trimmedIp);

            m_manualConnectBtn->setText(QStringLiteral("Connect by IP"));
            updateActionState();

            if (m_manualConnectPending) {
                saveLowBandwidthPreference(m_lowBwCheck->isChecked());
                setManualMessage(
                    QStringLiteral("Found a radio at %1. Connecting now…").arg(trimmedIp));
                m_manualConnectPending = false;
                emit connectRequested(info);
            } else {
                setManualMessage(
                    QStringLiteral("Found a radio at %1 and saved the path for later.")
                        .arg(trimmedIp));
                emit routedRadioFound(info);
            }
        };

        connect(peekTimer, &QTimer::timeout, this, [finishProbe] { finishProbe(); });

        connect(sock, &QTcpSocket::readyRead, this,
                [sock, buffer, version, statusLines, localSrc, seenHandle, peekTimer] {
            buffer->append(sock->readAll());

            while (buffer->contains('\n')) {
                const int idx = buffer->indexOf('\n');
                const QString line = QString::fromUtf8(buffer->left(idx)).trimmed();
                buffer->remove(0, idx + 1);

                if (line.startsWith('V')) {
                    *version = line.mid(1);
                } else if (line.startsWith('H') && !*seenHandle) {
                    *seenHandle = true;
                    *localSrc   = sock->localAddress();
                    // Ask the radio for its current state before we commit
                    // to a real connection. This lets us populate mf_enable
                    // and connected client info even when there's no discovery
                    // broadcast (direct IP / VPN / routed path).
                    sock->write("C1|sub radio all\n");
                    sock->write("C2|sub client all\n");
                    sock->flush();
                    peekTimer->start();
                } else if (line.startsWith('S') && *seenHandle) {
                    statusLines->append(line);
                }
            }
        });
    });

    connect(sock, &QTcpSocket::errorOccurred, this,
            [this, sock, trimmedIp](QAbstractSocket::SocketError) {
        setManualMessage(
            QStringLiteral("Could not reach %1: %2").arg(trimmedIp, sock->errorString()),
            true);
        sock->deleteLater();
        m_manualConnectPending = false;
        m_manualConnectBtn->setText("Connect by IP");
        updateActionState();
    });
}

} // namespace AetherSDR
