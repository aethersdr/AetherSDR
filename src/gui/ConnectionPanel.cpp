#include "ConnectionPanel.h"
#include "core/AppSettings.h"
#include "core/backends/hl2/Hl2Discovery.h"   // shared nickname + MAC->serial helpers
#include "core/backends/icom/IcomCredentials.h"  // password -> OS keychain, never settings
#include "core/backends/icom/IcomSettings.h"     // host/user/ports (Principle V)
#include "core/backends/icom/IcomModels.h"       // knownModels() -> the chooser's items
#include "core/backends/hl2/MetisProtocol.h"  // discoveryRequest/parseDiscoveryReply, kMetisPort
#include "core/backends/sim/SimBackend.h"
#include "core/NetworkPathResolver.h"
#include "ComboStyle.h"   // shared themed combo look (painted arrow)
#include "FramelessResizer.h"
#include "FramelessWindowTitleBar.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <span>

#include <QAbstractItemView>
#include <QPointer>
#include <QLineEdit>
#include <QFormLayout>
#include <QGuiApplication>
#include <QInputDialog>
#include <QMenu>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QScrollArea>
#include <QScreen>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QStyle>
#include <QTcpSocket>
#include <QHostInfo>
#include <QUdpSocket>
#include <QNetworkDatagram>
#include <QDeadlineTimer>
#include <QTimer>
#include <QVBoxLayout>
#include <QWindow>
#include "core/ThemeManager.h"

namespace AetherSDR {

namespace {

constexpr int kSourceModeRole = Qt::UserRole + 10;
constexpr int kSourceInterfaceIdRole = Qt::UserRole + 11;
constexpr int kSourceInterfaceNameRole = Qt::UserRole + 12;
constexpr int kSourceAddressRole = Qt::UserRole + 13;
constexpr int kSourceStaleRole = Qt::UserRole + 14;
constexpr int kMaxRecentManualIps = 3;
constexpr const char* kRecentManualIpsKey = "RecentConnectByIpAddresses";
// Last radio type chosen on the "Connect by IP" page. Defaults to flex so an
// existing install keeps the behaviour it had before the selector existed.
constexpr const char* kManualRadioFamilyKey = "ConnectByIpRadioFamily";

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
    const QString trimmed = ip.trimmed();
    if (trimmed.isEmpty())
        return QString();

    // A numeric address is canonicalised, so "192.168.001.5" and
    // "192.168.1.5" are one entry rather than two.
    const QHostAddress address(trimmed);
    if (!address.isNull())
        return address.toString();

    // A HOST NAME is legitimate here and used to be dropped SILENTLY, because
    // QHostAddress parses numeric addresses only. That cost the recent list
    // every VPN radio reached by DNS name, and it bites hardest on an Icom:
    // the IC-705's documented default address is ic-705.local, so the one
    // address the manual explicitly tells the operator to use was the one the
    // field refused to remember.
    //
    // Conservative charset — letters, digits, dot, hyphen, underscore, with
    // alphanumeric ends — so this widens what we REMEMBER without widening
    // what we will hand to a resolver.
    static const QRegularExpression hostName(
        QStringLiteral("^[A-Za-z0-9]([A-Za-z0-9._-]*[A-Za-z0-9])?$"));
    if (trimmed.size() <= 253 && hostName.match(trimmed).hasMatch())
        return trimmed;

    return QString();
}

void setAutomationError(QString* error, const QString& text)
{
    if (error) {
        *error = text;
    }
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

// Radio family remembered for one manual address. Older profiles predate the
// selector and carry no identity.family — treat those as flex, which is what
// they were when they were written.
QString familyFromProfile(const QJsonObject& profile)
{
    const QString family =
        profile.value("identity").toObject().value("family").toString().trimmed().toLower();
    if (family == QLatin1String(ConnectionPanel::kFamilyHl2))
        return QString::fromLatin1(ConnectionPanel::kFamilyHl2);
    if (family == QLatin1String(ConnectionPanel::kFamilyIcom))
        return QString::fromLatin1(ConnectionPanel::kFamilyIcom);
    return QString::fromLatin1(ConnectionPanel::kFamilyFlex);
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
    setObjectName(QStringLiteral("connectionPanel"));
    setAccessibleName(tr("Connect to Radio"));

    theme::setContainer(this, QStringLiteral("panel/connection"));
    AetherSDR::ThemeManager::instance().applyStyleSheet(this, "ConnectionPanel { background: {{color.background.0}}; }"
        "QGroupBox { border: 1px solid {{color.background.2}}; border-radius: 7px; margin-top: 10px; "
        "color: {{color.text.primary}}; font-weight: bold; }"
        "QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 4px; }"
        "QListWidget { background: #09111b; border: 1px solid {{color.background.2}}; border-radius: 4px; "
        "color: {{color.text.primary}}; padding: 2px; }"
        "QListWidget QScrollBar:vertical { background: #09111b; width: 12px; margin: 0; }"
        "QListWidget QScrollBar::handle:vertical { background: #304050; border-radius: 5px; min-height: 24px; }"
        "QListWidget QScrollBar::add-line:vertical, QListWidget QScrollBar::sub-line:vertical { height: 0; }"
        "QPushButton { padding: 5px 12px; }");

    const QString editStyle =
        "QLineEdit { border: 1px solid #304050; border-radius: 4px; padding: 4px 6px; "
        "background: #09111b; color: #d7e4f2; }";
    // THE SHARED COMBO STYLE, not a hand-rolled one.
    //
    // This dialog used to carry its own: raw hex instead of theme tokens, and a
    // down-arrow built from the CSS zero-size-plus-borders triangle trick, which
    // Qt renders on macOS as a filled blob rather than an arrow. Meanwhile every
    // other combo in the app already used ComboStyle.h, which paints a real
    // arrow and follows the theme — so the one dialog a new operator sees first
    // was the one that looked wrong.
    //
    // The override is the field HEIGHT's business: these rows are 30 px, where
    // the compact applet combos the shared template was shaped for are 22 px, so
    // the text needs a bigger inset to sit off the frame. `padding: 0` was what
    // made it hug the border in the first place.
    const QString comboExtraRules =
        "QComboBox { padding: 4px 8px; }"
        // Pin the drop-down to the BORDER box. Without this it inherits the
        // padding above and floats inward, detached from the frame's right edge.
        "QComboBox::drop-down { subcontrol-origin: border;"
        " subcontrol-position: top right; width: 22px; border: none; }";

    // The Icom credential fields are bare QLineEdits, not combo boxes, so they
    // inherit none of the combo styling above. Without this they render as white
    // boxes on a dark panel — the same widget, two different looks, in one row.
    //
    // TOKENS, and applied through ThemeManager rather than setStyleSheet(). The
    // neighbouring style strings in this file are pre-existing raw hex and are
    // left alone, but nothing new should add to that pile: the same four values
    // already have canonical tokens, so hardcoding them here would have meant
    // two credential fields that stop following the theme the moment anyone
    // changes it.
    // 8 px to match the combos above, so the four fields in this column start
    // their text at the same x. They did not before: the combos had none at all.
    const QString lineEditStyle =
        "QLineEdit { border: 1px solid {{color.background.2}}; border-radius: 2px; "
        "padding: 4px 8px; background: {{color.background.1}}; "
        "color: {{color.text.primary}}; }"
        "QLineEdit:focus { border-color: {{color.accent.bright}}; }";
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
    content->setObjectName(QStringLiteral("connectionBodyContent"));
    auto* root = new QVBoxLayout(content);
    // No right margin: the 12 px band on that edge belongs to the scroll area
    // (see bodyContainer below), so adding one here would inset the body 24 px
    // from the right against 12 px on the left and 12 px on the footer.
    root->setContentsMargins(12, 12, 0, 10);
    root->setSpacing(10);
    m_rootLayout = root;
    m_bodyContent = content;

    auto* bodyScroll = new QScrollArea(this);
    bodyScroll->setObjectName(QStringLiteral("connectionBodyScrollArea"));
    bodyScroll->setAccessibleName(tr("Connection options"));
    bodyScroll->setWidgetResizable(true);
    bodyScroll->setFrameShape(QFrame::NoFrame);
    bodyScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    bodyScroll->setWidget(content);
    m_bodyScroll = bodyScroll;

    // Keep the scrollbars clear of FramelessResizer's edge-grab zone. The
    // fixed footer already carries its own inset; the scrolling body needs one
    // too because QScrollArea places its vertical bar at its outer edge.
    auto* bodyContainer = new QWidget(this);
    auto* bodyContainerLayout = new QVBoxLayout(bodyContainer);
    bodyContainerLayout->setContentsMargins(0, 0, 12, 0);
    bodyContainerLayout->setSpacing(0);
    bodyContainerLayout->addWidget(bodyScroll);
    outer->addWidget(bodyContainer, 1);

    auto* titleLabel = new QLabel("Connect to a Radio", this);
    AetherSDR::ThemeManager::instance().applyStyleSheet(titleLabel, "QLabel { color: {{color.text.primary}}; font-size: 18px; font-weight: bold; "
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
    m_localModeBtn->setObjectName(QStringLiteral("connectionLocalModeButton"));
    configureModeButton(m_localModeBtn,
                        "On This Network",
                        "Recommended for new users when the radio and computer are on the same LAN.",
                        LocalMode);
    modeRow->addWidget(m_localModeBtn);

    m_smartLinkModeBtn = new QCommandLinkButton(this);
    m_smartLinkModeBtn->setObjectName(QStringLiteral("connectionSmartLinkModeButton"));
    configureModeButton(m_smartLinkModeBtn,
                        "Remote with SmartLink",
                        "Use FlexRadio SmartLink when the radio is away from this computer.",
                        SmartLinkMode);
    modeRow->addWidget(m_smartLinkModeBtn);

    m_manualModeBtn = new QCommandLinkButton(this);
    m_manualModeBtn->setObjectName(QStringLiteral("connectionManualModeButton"));
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
    m_radioList->setObjectName(QStringLiteral("connectionLocalRadioList"));
    m_radioList->setAccessibleName(tr("Available local radios"));
    m_radioList->setAccessibleDescription(
        tr("Discovered FlexRadio radios on the local network"));
    m_radioList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_radioList->setWordWrap(true);
    m_radioList->setSpacing(2);
    // Bound the list height so it scrolls internally when more radios are
    // discovered than fit — otherwise on a small display (e.g. a 1024x600 Pi
    // panel) the list grew past the dialog and the Connect button and lower
    // radios became unreachable. Keep a modest minimum, cap the maximum, and
    // force the vertical scrollbar so overflow is always reachable (some
    // themes render an as-needed bar invisibly). Mirrors the WAN list below.
    m_radioList->setMinimumHeight(120);
    m_radioList->setMaximumHeight(240);
    m_radioList->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_radioList->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_radioList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    localGroupLayout->addWidget(m_radioList);

    // Right-click a discovered radio to set a custom nickname without connecting
    // first. Only radios without an on-radio name store (non-Flex: HL2, sim, …)
    // are stored client-side; Flex's own "radio name" is set from Radio Setup
    // while connected, so we don't offer it here for Flex to avoid two sources of
    // truth. The nickname is persisted keyed by serial and picked up on the next
    // discovery sweep.
    m_radioList->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_radioList, &QListWidget::customContextMenuRequested, this,
            [this](const QPoint& pos) { showRadioContextMenu(pos); });
    localListLayout->addWidget(localGroup, 1);

    auto* localActionRow = new QHBoxLayout;
    localActionRow->addStretch();
    m_localConnectBtn = new QPushButton("Connect Selected Radio", localListPage);
    m_localConnectBtn->setObjectName(QStringLiteral("connectionLocalConnectButton"));
    m_localConnectBtn->setAccessibleName(tr("Connect selected local radio"));
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
    AetherSDR::ThemeManager::instance().applyStyleSheet(emptyTitle, "QLabel { color: {{color.text.primary}}; font-size: 15px; font-weight: bold; "
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
    m_wanList->setObjectName(QStringLiteral("connectionSmartLinkRadioList"));
    m_wanList->setAccessibleName(tr("Available SmartLink radios"));
    m_wanList->setAccessibleDescription(
        tr("Remote FlexRadio radios available through SmartLink"));
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
    m_wanConnectBtn->setObjectName(QStringLiteral("connectionSmartLinkConnectButton"));
    m_wanConnectBtn->setAccessibleName(tr("Connect selected SmartLink radio"));
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
    m_manualHintLabel = makeWrappedLabel(QString(), kHintLabelStyle);
    m_manualHintLabel->setObjectName(QStringLiteral("connectionManualHintLabel"));
    manualLayout->addWidget(m_manualHintLabel);

    auto* manualGroup = new QGroupBox("Radio IP address", manualPage);
    // Baseline floor for the collapsed page. The rows and the result line carry
    // explicit minimums of their own, but a word-wrapped QLabel under-reports
    // its height, so without this the surrounding layout can still hand the
    // group less than it needs and Qt overlaps children. The taller states
    // (Advanced expanded) are covered by the enclosing QScrollArea, which never
    // gives the body less than qSmartMinSize() and scrolls the difference.
    manualGroup->setMinimumHeight(240);
    auto* manualGroupLayout = new QVBoxLayout(manualGroup);
    manualGroupLayout->setContentsMargins(12, 14, 12, 12);
    manualGroupLayout->setSpacing(12);
    // Explicit label+field rows rather than a QFormLayout. A QFormLayout nested
    // inside the group's QVBoxLayout squeezed both rows to a few pixels tall
    // once the wrapped error message appeared below them, so the labels ended up
    // overlapping the fields. Plain rows with a fixed-width label column give
    // the two entries a stable height and keep the labels aligned.
    auto* manualForm = new QVBoxLayout;
    manualForm->setContentsMargins(0, 0, 0, 0);
    manualForm->setSpacing(10);

    constexpr int kManualLabelWidth = 96;
    constexpr int kManualFieldHeight = 30;
    // Every row's label, so the column can be squared up once they all exist.
    //
    // setMinimumWidth() alone did NOT give a column. With a Fixed size policy Qt
    // caps a widget at its own sizeHint, so a label narrower than the minimum
    // sat at 96 while "Icom password:" — which is wider than 96 — sat at its own
    // hint, and the four fields started at three different x positions. The
    // width has to come from the WIDEST label, and that is not known until the
    // last row is added.
    QList<QLabel*> manualRowLabels;
    // Returns the row CONTAINER so a caller can hide the label and the field
    // together. Takes QWidget* rather than QComboBox* because the Icom
    // credential fields are QLineEdits; both calls below it are QWidget methods.
    const auto addManualRow = [&](const QString& labelText, QWidget* field) -> QWidget* {
        auto* rowWidget = new QWidget(manualGroup);
        auto* row = new QHBoxLayout(rowWidget);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(12);
        auto* label = new QLabel(labelText, rowWidget);
        label->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        label->setMinimumHeight(kManualFieldHeight);
        manualRowLabels.append(label);
        field->setMinimumHeight(kManualFieldHeight);
        field->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        row->addWidget(label);
        row->addWidget(field, 1);
        manualForm->addWidget(rowWidget);
        return rowWidget;
    };

    // Radio type. A FlexRadio answers a TCP/4992 command-plane probe; a
    // Hermes-Lite 2 answers an HPSDR Protocol 1 discovery datagram on UDP/1024
    // and will never answer the Flex probe (and vice versa). Probing both in
    // sequence made every Flex connect pay the HL2 timeout, so the operator
    // tells us which wire protocol to speak.
    m_manualRadioTypeCombo = new QComboBox(manualGroup);
    m_manualRadioTypeCombo->setObjectName(QStringLiteral("connectionManualRadioType"));
    m_manualRadioTypeCombo->setAccessibleName(tr("Radio type"));
    m_manualRadioTypeCombo->setAccessibleDescription(
        tr("Which radio family to look for at the address below"));
    AetherSDR::applyComboStyle(m_manualRadioTypeCombo, comboExtraRules);
    m_manualRadioTypeCombo->addItem(tr("FlexRadio"), QString::fromLatin1(kFamilyFlex));
    m_manualRadioTypeCombo->addItem(tr("Hermes-Lite 2"), QString::fromLatin1(kFamilyHl2));
    m_manualRadioTypeCombo->addItem(tr("Icom (network)"), QString::fromLatin1(kFamilyIcom));
    addManualRow(QStringLiteral("Radio type:"), m_manualRadioTypeCombo);

    m_manualIpCombo = new QComboBox(manualGroup);
    m_manualIpCombo->setObjectName(QStringLiteral("connectionManualIpCombo"));
    m_manualIpCombo->setAccessibleName(tr("Radio IP address"));
    m_manualIpCombo->setAccessibleDescription(
        tr("IP address or host name for a routed or VPN radio connection"));
    m_manualIpCombo->setEditable(true);
    m_manualIpCombo->setInsertPolicy(QComboBox::NoInsert);
    m_manualIpCombo->setMaxVisibleItems(kMaxRecentManualIps);
    AetherSDR::applyComboStyle(m_manualIpCombo, comboExtraRules);
    m_manualIpEdit = m_manualIpCombo->lineEdit();
    m_manualIpEdit->setObjectName(QStringLiteral("connectionManualIp"));
    m_manualIpEdit->setAccessibleName(tr("Radio IP address"));
    m_manualIpEdit->setAccessibleDescription(
        tr("IP address or host name for a routed or VPN radio connection"));
    m_manualIpEdit->setClearButtonEnabled(true);
    m_manualIpEdit->setPlaceholderText("Example: 10.0.0.25");
    // NO HEIGHT ON THE EDITOR. The combo around it is 30 px with 4 px of
    // vertical padding, which leaves a 22 px content box; a 26 px minimum on
    // the editor overflows that box and Qt resolves it downward, so the
    // address sat visibly below the centre of its own field — and below the
    // Radio type text in the row above it. The row's height is the combo's
    // to set (addManualRow does), and the editor fills what it is given.
    addManualRow(QStringLiteral("Radio IP:"), m_manualIpCombo);

    // Icom credentials. Hidden for every other family — see
    // updateManualFamilyHints(). An Icom will not answer the RS-BA1 handshake
    // without them, so unlike the Flex and HL2 paths this is not optional.
    m_manualIcomUserEdit = new QLineEdit(manualGroup);
    m_manualIcomUserEdit->setObjectName(QStringLiteral("connectionManualIcomUser"));
    m_manualIcomUserEdit->setAccessibleName(tr("Icom network username"));
    m_manualIcomUserEdit->setAccessibleDescription(
        tr("The network user name configured on the radio"));
    m_manualIcomUserEdit->setClearButtonEnabled(true);
    m_manualIcomUserEdit->setPlaceholderText(tr("Radio network user name"));
    ThemeManager::instance().applyStyleSheet(m_manualIcomUserEdit, lineEditStyle);
    m_manualIcomUserRow = addManualRow(QStringLiteral("Icom user:"), m_manualIcomUserEdit);

    m_manualIcomPassEdit = new QLineEdit(manualGroup);
    m_manualIcomPassEdit->setObjectName(QStringLiteral("connectionManualIcomPassword"));
    m_manualIcomPassEdit->setAccessibleName(tr("Icom network password"));
    m_manualIcomPassEdit->setAccessibleDescription(
        tr("The network password configured on the radio. Stored in the operating "
           "system keychain, never in the settings file."));
    m_manualIcomPassEdit->setEchoMode(QLineEdit::Password);
    m_manualIcomPassEdit->setPlaceholderText(tr("Radio network password"));
    ThemeManager::instance().applyStyleSheet(m_manualIcomPassEdit, lineEditStyle);
    m_manualIcomPassRow = addManualRow(QStringLiteral("Icom password:"), m_manualIcomPassEdit);

    // The CI-V address is the third thing the operator has to read off the
    // radio, alongside the two above — theirs live in the Network menu, this
    // one in Connectors > CI-V. Without it the address can only ever be the
    // IC-705 default, and every other model in kModels is unreachable: CI-V is
    // addressed, so an IC-9700 on 0xA2 silently ignores everything sent to
    // 0xA4. No ID reply, no model, and the conservative unknown fallback means
    // no scope and no transmit — which reads as "this backend has no
    // panadapter yet" rather than "wrong address".
    //
    // MOSTLY A DISPLAY, NOT AN INPUT — and that is the reframe that justifies a
    // chooser at all. The connect path now asks the radio for its own address
    // (a broadcast 0x19 0x00, which needs no model table and is right even when
    // the address was changed ON the radio), so the operator does not have to
    // know any of this. What the control buys is LEGIBILITY: it names the
    // models, so "A2" stops being a number to look up, and picking one is a
    // one-click shortcut for an operator who would rather be explicit.
    //
    // Non-editable, with a "Custom..." sentinel and a hidden hex row —
    // populateSerialPortCombo()'s shape (RadioSetupDialog.cpp), MIRRORED rather
    // than reused because that helper is serial-specific and behind
    // HAVE_SERIALPORT. It is the closer of the two in-repo precedents:
    // m_manualIpCombo above is an editable recent-values HISTORY, whereas this
    // enumerates a known set and offers an escape hatch. That helper's own
    // header records being factored out after two call sites reimplemented it
    // "with a subtly different isCustom computation"; this is deliberately not
    // the third.
    m_manualIcomCivCombo = new QComboBox(manualGroup);
    m_manualIcomCivCombo->setObjectName(QStringLiteral("connectionManualIcomCivCombo"));
    m_manualIcomCivCombo->setAccessibleName(tr("Icom radio model"));
    m_manualIcomCivCombo->setAccessibleDescription(
        tr("Which Icom model to address, or Auto-detect to ask the radio for its own "
           "CI-V address."));
    AetherSDR::applyComboStyle(m_manualIcomCivCombo, comboExtraRules);
    populateIcomCivCombo();
    m_manualIcomCivRow = addManualRow(QStringLiteral("Icom CI-V:"), m_manualIcomCivCombo);

    // The hex entry, kept for the radio whose address has been changed to
    // something no model uses, and for a shared CI-V bus where the operator is
    // selecting WHICH DEVICE rather than naming a model.
    //
    // The objectName is UNCHANGED and must stay so: the automation bridge and
    // any UI test address this field by that name, and renaming it would break
    // them silently rather than loudly.
    m_manualIcomCivEdit = new QLineEdit(manualGroup);
    m_manualIcomCivEdit->setObjectName(QStringLiteral("connectionManualIcomCivAddress"));
    m_manualIcomCivEdit->setAccessibleName(tr("Icom CI-V address"));
    m_manualIcomCivEdit->setAccessibleDescription(
        tr("The radio's CI-V address in hex, from MENU > SET > Connectors > CI-V. "
           "Only needed for a radio whose address has been changed, or to pick one "
           "device on a shared CI-V bus."));
    m_manualIcomCivEdit->setClearButtonEnabled(true);
    m_manualIcomCivEdit->setPlaceholderText(tr("e.g. A2"));
    ThemeManager::instance().applyStyleSheet(m_manualIcomCivEdit, lineEditStyle);
    m_manualIcomCivCustomRow =
        addManualRow(QStringLiteral("CI-V address:"), m_manualIcomCivEdit);

    // ⚠ AUTOMATION: THIS FIELD IS NOW HIDDEN UNTIL "Custom..." IS SELECTED, and
    // the bridge refuses to drive a hidden widget — `invoke
    // connectionManualIcomCivAddress setText A2` returns
    // "refused: 'connectionManualIcomCivAddress' is not visible". Verified
    // against a running build, not assumed.
    //
    // A script that sets this field must now select Custom first:
    //     invoke connectionManualIcomCivCombo setCurrentText "Custom..."
    //     invoke connectionManualIcomCivAddress setText A2
    // …or, better, name the model and skip the hex entirely:
    //     invoke connectionManualIcomCivCombo setCurrentText "IC-9700 — A2"
    //
    // The objectName is deliberately unchanged so that message names the field
    // the script already knows, and the failure is LOUD rather than a silent
    // no-op. An earlier revision of this tried to keep the old call working by
    // selecting Custom from the field's own textChanged — which cannot fire,
    // because the bridge's visibility check runs first. Removed rather than
    // left in as a comment promising something it does not do.
    connect(m_manualIcomCivCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { syncIcomCivCustomRow(); });

    // One column, set from the widest label. Rows that are hidden for a family
    // still count: the Icom rows appear and disappear as the operator changes
    // radio type, and a column that resized with them would move the Radio type
    // and Radio IP fields sideways every time.
    {
        int labelColumnWidth = kManualLabelWidth;
        for (QLabel* label : manualRowLabels)
            labelColumnWidth = std::max(labelColumnWidth, label->sizeHint().width());
        for (QLabel* label : manualRowLabels)
            label->setFixedWidth(labelColumnWidth);
    }

    manualGroupLayout->addLayout(manualForm);

    auto* manualActionRow = new QHBoxLayout;
    manualActionRow->setSpacing(10);
    auto* manualDiagnosticsBtn = new QPushButton("Network Diagnostics", manualGroup);
    manualActionRow->addWidget(manualDiagnosticsBtn);
    manualActionRow->addStretch();
    m_manualConnectBtn = new QPushButton("Connect by IP", manualGroup);
    m_manualConnectBtn->setObjectName(QStringLiteral("connectionManualConnectButton"));
    m_manualConnectBtn->setAccessibleName(tr("Connect by IP"));
    manualActionRow->addWidget(m_manualConnectBtn);
    manualGroupLayout->addLayout(manualActionRow);

    m_manualResultLabel = makeWrappedLabel(QString(), kHintLabelStyle);
    // Always laid out, even when empty. A word-wrapped QLabel reports a
    // one-line minimum height, so showing a message that wraps to two or three
    // lines used to grow the page past the dialog height and Qt resolved the
    // overflow by overlapping the rows above it — the "Radio type"/"Radio IP"
    // labels ended up on top of their own entry fields. Reserving the space up
    // front keeps the page height constant whether or not a message is showing.
    m_manualResultLabel->setMinimumHeight(36);
    m_manualResultLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    m_manualResultLabel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::MinimumExpanding);
    manualGroupLayout->addWidget(m_manualResultLabel);

    m_manualAdvancedToggle = new QToolButton(manualGroup);
    m_manualAdvancedToggle->setObjectName(QStringLiteral("connectionManualAdvancedToggle"));
    m_manualAdvancedToggle->setText("Advanced: choose the VPN source path");
    m_manualAdvancedToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_manualAdvancedToggle->setCheckable(true);
    m_manualAdvancedToggle->setArrowType(Qt::RightArrow);
    m_manualAdvancedToggle->setVisible(false);
    manualGroupLayout->addWidget(m_manualAdvancedToggle, 0, Qt::AlignLeft);

    m_manualAdvancedWidget = new QWidget(manualGroup);
    m_manualAdvancedWidget->setObjectName(QStringLiteral("connectionManualAdvancedSection"));
    auto* manualAdvancedLayout = new QVBoxLayout(m_manualAdvancedWidget);
    manualAdvancedLayout->setContentsMargins(8, 4, 8, 4);
    manualAdvancedLayout->setSpacing(6);
    auto* manualAdvancedHint = makeWrappedLabel(
        "Most users can leave this on Auto. Pick a source path only if your VPN creates more "
        "than one active network adapter or a saved path is no longer available.",
        kHintLabelStyle);
    manualAdvancedHint->setObjectName(QStringLiteral("connectionManualAdvancedHint"));
    manualAdvancedLayout->addWidget(manualAdvancedHint);
    auto* sourceRow = new QHBoxLayout;
    sourceRow->setContentsMargins(0, 0, 0, 0);
    sourceRow->addWidget(new QLabel("Source path:", m_manualAdvancedWidget));
    m_manualSourceCombo = new QComboBox(m_manualAdvancedWidget);
    m_manualSourceCombo->setObjectName(QStringLiteral("connectionManualSourcePath"));
    m_manualSourceCombo->setAccessibleName(tr("Manual connection source path"));
    m_manualSourceCombo->setAccessibleDescription(
        tr("Network interface or source address to use for the routed radio connection"));
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
    AetherSDR::ThemeManager::instance().applyStyleSheet(optionsTitle, "QLabel { color: {{color.text.primary}}; font-weight: bold; background: transparent; border: none; }");
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

    m_adaptiveThrottleCheck = new QCheckBox("Enable adaptive frame-rate throttle", this);
    const bool adaptiveEnabled = AppSettings::instance()
        .value("AdaptiveThrottleEnabled", "False").toString() == "True";
    m_adaptiveThrottleCheck->setChecked(adaptiveEnabled);
    m_adaptiveThrottleCheck->setToolTip(
        "Automatically reduces FFT/waterfall frame rate when network quality degrades, "
        "reducing the chance of a disconnect on congested links. "
        "Toggling while connected takes effect at the next quality update; "
        "reconnect to lift an already-applied cap immediately.");
    m_adaptiveThrottleCheck->setStyleSheet(lowBandwidthCheckStyle);
    connect(m_adaptiveThrottleCheck, &QCheckBox::toggled, this, [](bool on) {
        auto& s = AppSettings::instance();
        s.setValue("AdaptiveThrottleEnabled", on ? "True" : "False");
        s.save();
    });
    root->addWidget(m_adaptiveThrottleCheck);

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

    // Demo mode (RFC #4288): offer the synthetic "AetherSDR Demo — Simulator"
    // entry in the radio list. Default on for discoverability; the choice
    // persists. Toggling just writes the setting and shows/hides the entry — the
    // connection (and backend selection) happens only when the user connects to
    // it, exactly like a real radio.
    m_showDemoCheck = new QCheckBox("Show the AetherSDR demo simulator", this);
    m_showDemoCheck->setChecked(
        AppSettings::instance().value("ShowDemoRadio", "True").toString() == "True");
    m_showDemoCheck->setStyleSheet(lowBandwidthCheckStyle);
    connect(m_showDemoCheck, &QCheckBox::toggled, this, [this](bool on) {
        auto& s = AppSettings::instance();
        s.setValue("ShowDemoRadio", on ? "True" : "False");
        s.save();
        if (on) addDemoRadio();
        else    removeDemoRadio();
    });
    root->addWidget(m_showDemoCheck);

    // ── Footer ────────────────────────────────────────────────────────────
    auto* footerRow = new QHBoxLayout;
    footerRow->setSpacing(8);
    m_statusLabel = makeWrappedLabel("Choose how you want to connect.", kHintLabelStyle);
    footerRow->addWidget(m_statusLabel, 1);
    m_disconnectBtn = new QPushButton("Disconnect", this);
    m_disconnectBtn->setObjectName(QStringLiteral("connectionDisconnectButton"));
    m_disconnectBtn->setAccessibleName(tr("Disconnect from radio"));
    m_disconnectBtn->setVisible(false);
    footerRow->addWidget(m_disconnectBtn, 0, Qt::AlignRight);

    auto* footer = new QWidget(this);
    footer->setObjectName(QStringLiteral("connectionFooter"));
    footer->setLayout(footerRow);
    footerRow->setContentsMargins(12, 0, 12, 12);
    outer->addWidget(footer);

    // Family first: applySavedSourceSelection() may override it with the
    // per-address choice remembered for whichever recent IP we preselect.
    setManualFamily(AppSettings::instance()
                        .value(kManualRadioFamilyKey, QString::fromLatin1(kFamilyFlex))
                        .toString());
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
    // Enter connects from the credential fields too. Without this the Icom path
    // was the one family where the keyboard could not finish the job: the
    // operator types the address, tabs to user and password — the two fields
    // only an Icom shows — and Enter did nothing, because only the address
    // field was wired. Typing a password and pressing Enter is what everyone
    // does; it should not be the one gesture that requires the mouse.
    connect(m_manualIcomUserEdit, &QLineEdit::returnPressed,
            this, &ConnectionPanel::onManualConnectClicked);
    connect(m_manualIcomPassEdit, &QLineEdit::returnPressed,
            this, &ConnectionPanel::onManualConnectClicked);
    connect(m_manualIpEdit, &QLineEdit::textChanged,
            this, &ConnectionPanel::onManualIpChanged);
    // The address was CHOSEN, not typed — restore its remembered radio type.
    // `activated` fires only for a pick out of the popup, which is exactly the
    // gesture the per-address family restore was written for; the keystroke
    // path deliberately does not carry it (see applySavedSourceSelection).
    connect(m_manualIpCombo, &QComboBox::activated, this, [this](int) {
        applySavedSourceSelection(m_manualIpEdit->text().trimmed(),
                                  /*restoreFamily=*/true);
    });
    connect(m_manualRadioTypeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) {
                auto& settings = AppSettings::instance();
                settings.setValue(kManualRadioFamilyKey, currentManualFamily());
                settings.save();
                updateManualFamilyHints();
                setManualMessage(QString());
            });
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

    // Settle the body layout so preferredClientHeight() has a real answer the
    // first time fitToScreen() asks for one. The height floor this used to set
    // is gone: the manual page is still the tallest of the three, but overflow
    // now belongs to the scroll area rather than to the top-level minimum.
    refitToContent();
    setMinimumSize(kSafeMinimumWidth, kSafeMinimumHeight);
    resize(kPreferredWidth, kPreferredHeight);

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
        m_rootLayout->setContentsMargins(12, on ? 10 : 12, 0, 10);
    if (wasVisible) {
        // Turning decorations back on wraps a native frame — a title bar and
        // borders, ~24-31 px on Windows — around a client rect that was sized
        // for a frameless window. Without a re-fit that pushes the frame, and
        // the footer pinned at its bottom, straight back under the taskbar:
        // #4515 again, reached through View -> Frameless Window instead of
        // through a connect.
        fitAndClampToScreen();
        show();
    }
}

bool ConnectionPanel::event(QEvent* e)
{
    switch (e->type()) {
    // A fit is only valid for the screen, DPI and font metrics it was made
    // under. #4515 asks for it to be redone when any of those change beneath an
    // open window — dragged to a shorter monitor, scaling changed in Display
    // settings, UI font swapped. Deferred by one turn of the event loop because
    // the children have not re-laid-out by the time these arrive, so a fit
    // computed here would use the outgoing metrics.
    case QEvent::ScreenChangeInternal:
    case QEvent::ApplicationFontChange:
    case QEvent::FontChange:
    case QEvent::StyleChange:
        if (isVisible()) {
            QTimer::singleShot(0, this, [this] {
                if (isVisible())
                    fitAndClampToScreen();
            });
        }
        break;
    default:
        break;
    }
    return QWidget::event(e);
}

QScreen* ConnectionPanel::screenFitTarget(QScreen* preferredScreen) const
{
    QScreen* targetScreen = preferredScreen;
    if (!targetScreen && windowHandle()) {
        targetScreen = windowHandle()->screen();
    }
    if (!targetScreen && parentWidget()) {
        targetScreen = parentWidget()->screen();
    }
    if (!targetScreen) {
        targetScreen = QGuiApplication::primaryScreen();
    }
    return targetScreen;
}

int ConnectionPanel::preferredClientHeight() const
{
    if (!m_bodyContent || !m_bodyScroll) {
        return kPreferredHeight;
    }
    // Everything in `outer` except the scroll area — the title bar when it is
    // shown, the fixed footer always. Taking it as a difference rather than
    // summing the two widgets keeps this correct without duplicating which of
    // them is visible in which frameless mode.
    const int chromeHeight = sizeHint().height() - m_bodyScroll->sizeHint().height();
    return qMax(kPreferredHeight, chromeHeight + m_bodyContent->sizeHint().height());
}

void ConnectionPanel::fitToScreen(QScreen* preferredScreen)
{
    QScreen* targetScreen = screenFitTarget(preferredScreen);
    if (!targetScreen) {
        return;
    }

    // Realise the native window before measuring. QWindow::frameMargins() is
    // only populated once the platform window exists, so on the very first open
    // an un-created panel falls back to the style estimate — which on Windows,
    // the platform #4515 was reported on, is roughly a title bar short.
    if (!windowHandle()) {
        (void)winId();
    }

    const QRect available = targetScreen->availableGeometry();
    const QMargins frameMargins = screenFitFrameMargins();

    const int maximumClientWidth =
        qMax(1, available.width() - frameMargins.left() - frameMargins.right());
    const int maximumClientHeight =
        qMax(1, available.height() - frameMargins.top() - frameMargins.bottom());
    setMinimumSize(qMin(kSafeMinimumWidth, maximumClientWidth),
                   qMin(kSafeMinimumHeight, maximumClientHeight));

    // Shrinking is the invariant: the frame must fit the work area whatever the
    // operator wants. Growing is a courtesy, so it only applies to a height
    // this function chose — otherwise a dialog dragged short on a big screen
    // would spring back every time it was reopened.
    int targetHeight = qMin(height(), maximumClientHeight);
    if (height() == m_autoFitHeight) {
        targetHeight = qBound(qMin(kSafeMinimumHeight, maximumClientHeight),
                              preferredClientHeight(),
                              maximumClientHeight);
    }
    resize(qMin(width(), maximumClientWidth), targetHeight);
    m_autoFitHeight = height();
}

void ConnectionPanel::fitAndClampToScreen(QScreen* preferredScreen)
{
    QScreen* targetScreen = screenFitTarget(preferredScreen);
    if (!targetScreen) {
        return;
    }
    fitToScreen(targetScreen);
    // pos() on a window is its frame top-left, the same space
    // constrainedFrameTopLeft() works in, so this is a no-op when the window
    // already fits and a pull-back-inside when it does not.
    move(constrainedFrameTopLeft(pos(), targetScreen->availableGeometry()));
}

QMargins ConnectionPanel::screenFitFrameMargins() const
{
    QMargins frameMargins;
    if (windowHandle()) {
        frameMargins = windowHandle()->frameMargins();
    }
    if (frameMargins.isNull()
        && !windowFlags().testFlag(Qt::FramelessWindowHint)) {
        const int border =
            style()->pixelMetric(QStyle::PM_DefaultFrameWidth, nullptr, this);
        const int titleHeight =
            style()->pixelMetric(QStyle::PM_TitleBarHeight, nullptr, this);
        frameMargins = QMargins(border, titleHeight, border, border);
    }
    return frameMargins;
}

QSize ConnectionPanel::screenFitFrameSize() const
{
    const QMargins frameMargins = screenFitFrameMargins();
    return QSize(width() + frameMargins.left() + frameMargins.right(),
                 height() + frameMargins.top() + frameMargins.bottom());
}

QPoint ConnectionPanel::constrainedFrameTopLeft(
    const QPoint& preferredFrameTopLeft,
    const QRect& availableGeometry) const
{
    const QSize frameSize = screenFitFrameSize();
    const int maxX =
        availableGeometry.left() + availableGeometry.width() - frameSize.width();
    const int maxY =
        availableGeometry.top() + availableGeometry.height() - frameSize.height();
    return QPoint(
        qMax(availableGeometry.left(), qMin(preferredFrameTopLeft.x(), maxX)),
        qMax(availableGeometry.top(), qMin(preferredFrameTopLeft.y(), maxY)));
}

void ConnectionPanel::clearPendingIcomCredentials()
{
    m_pendingIcomPassword.clear();
    m_pendingIcomHost.clear();
    m_pendingIcomResolvedHost.clear();
    m_pendingIcomBindSettings = RadioBindSettings{};
    m_pendingIcomSessionBindAddress.clear();
}

void ConnectionPanel::setConnected(bool connected)
{
    // THE ONE MOMENT THE CREDENTIALS ARE PROVEN. Everything staged by
    // probeRadio() is written here and nowhere else, so a wrong password is
    // forgotten rather than persisted over a working one.
    // SCOPED TO THE FAMILY THAT WAS STAGED FOR. A bare "connected" is not
    // proof that THIS password was proven: stage an Icom attempt, have it fail
    // without driving a disconnected edge (the panel is already disconnected,
    // so a state-change-only caller emits nothing), then connect to a Flex, and
    // the unproven Icom password would be written over a working keychain entry
    // — the exact failure the staging exists to prevent, one step removed. The
    // clears at every other connect path are the belt to this brace.
    if (connected && !m_pendingIcomPassword.isEmpty()
        && currentManualFamily() == QLatin1String(kFamilyIcom)) {
        IcomCredentials::save(m_pendingIcomPassword);
        if (!m_pendingIcomHost.isEmpty()) {
            IcomSettings::setLastHost(m_pendingIcomHost);
            // The routed profile restores the family and source path for this
            // address at the next launch. Icom cannot be probed anonymously,
            // so commit it only after the authenticated session proves the
            // host and credentials together.
            saveManualProfile(m_pendingIcomHost,
                              m_pendingIcomBindSettings,
                              m_pendingIcomSessionBindAddress);
            if (m_pendingIcomResolvedHost != m_pendingIcomHost) {
                // MainWindow retains the resolved address in LastRoutedRadioIp.
                // Mirror the profile under that key as well so a hostname such
                // as ic-705.local still restores the Icom family at startup.
                saveManualProfile(m_pendingIcomResolvedHost,
                                  m_pendingIcomBindSettings,
                                  m_pendingIcomSessionBindAddress);
            }
        }
    }
    if (!connected || !m_pendingIcomPassword.isEmpty()) {
        // Cleared on BOTH edges: a failed attempt must not commit on the next
        // unrelated connect, and a committed one must not commit twice.
        m_pendingIcomPassword.clear();
        m_pendingIcomHost.clear();
    }

    m_connected = connected;
    m_disconnectBtn->setVisible(connected);
    updateActionState();
    // Showing the Disconnect button is the one moment the fixed footer changes
    // height, so it is the one moment a fit made earlier stops being right —
    // and it is exactly when #4515 was reported. Cheap: fitToScreen() only
    // grows from a height it chose itself, and the clamp is a no-op for a
    // window already inside the work area.
    if (isVisible()) {
        fitAndClampToScreen();
    }
}

void ConnectionPanel::setStatusText(const QString& text)
{
    m_statusLabel->setText(text);
}

QList<RadioInfo> ConnectionPanel::automationLocalRadios() const
{
    return m_radios;
}

bool ConnectionPanel::automationConnectLocalSerial(const QString& serial, QString* error)
{
    const QString wanted = serial.trimmed();
    if (wanted.isEmpty()) {
        setAutomationError(error, QStringLiteral("local serial selector is empty"));
        return false;
    }
    if (m_connected) {
        setAutomationError(error, QStringLiteral("already connected to a radio"));
        return false;
    }

    for (int i = 0; i < m_radios.size(); ++i) {
        if (m_radios[i].serial.compare(wanted, Qt::CaseInsensitive) == 0) {
            setCurrentMode(LocalMode);
            m_radioList->setCurrentRow(i);
            onLocalConnectClicked();
            return true;
        }
    }

    setAutomationError(
        error,
        QStringLiteral("no discovered local radio has serial '%1'").arg(wanted));
    return false;
}

bool ConnectionPanel::automationConnectByIp(const QString& hostOrIp,
                                            const QString& family,
                                            QString* error)
{
    const QString target = hostOrIp.trimmed();
    if (target.isEmpty()) {
        setAutomationError(error, QStringLiteral("connect ip requires an address"));
        return false;
    }
    if (m_connected) {
        setAutomationError(error, QStringLiteral("already connected to a radio"));
        return false;
    }

    const QString wantedFamily = family.trimmed().toLower();
    if (!wantedFamily.isEmpty()
        && wantedFamily != QLatin1String(kFamilyFlex)
        && wantedFamily != QLatin1String(kFamilyHl2)
        && wantedFamily != QLatin1String(kFamilyIcom)) {
        setAutomationError(
            error,
            QStringLiteral("unknown radio family '%1' (use flex, hl2 or icom)")
                .arg(family.trimmed()));
        return false;
    }

    setCurrentMode(ManualMode);
    m_manualIpCombo->setCurrentText(target);
    m_manualIpEdit->setText(target);
    // After the address, so the per-address profile restore inside
    // applySavedSourceSelection() cannot overwrite an explicit request.
    if (!wantedFamily.isEmpty())
        setManualFamily(wantedFamily);
    onManualConnectClicked();
    return true;
}

bool ConnectionPanel::automationDisconnect(QString* error)
{
    if (!m_connected) {
        setAutomationError(error, QStringLiteral("not connected to a radio"));
        return false;
    }

    emit disconnectRequested();
    return true;
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
        // Cleared, not hidden: the label keeps its reserved height so the rows
        // above it do not shift (and overlap) when a message comes and goes.
        m_manualResultLabel->clear();
        return;
    }

    m_manualResultLabel->setText(text);
    m_manualResultLabel->setStyleSheet(error ? kErrorLabelStyle : kInfoLabelStyle);
    m_manualResultLabel->setVisible(true);
    // A long message wraps to more lines than the reserved height covers.
    refitToContent();
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
    const bool slowLinksVisible = mode == SmartLinkMode || mode == ManualMode;
    m_linkOptionsWidget->setVisible(slowLinksVisible);

    if (!slowLinksVisible)
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
    } else if (hasExplicitSelection || m_manualSourceWarningLabel->isVisible()) {
        const QSignalBlocker blocker(m_manualAdvancedToggle);
        m_manualAdvancedToggle->setChecked(true);
        m_manualAdvancedToggle->setArrowType(Qt::DownArrow);
        m_manualAdvancedWidget->setVisible(true);
    } else {
        m_manualAdvancedWidget->setVisible(m_manualAdvancedToggle->isChecked());
        m_manualAdvancedToggle->setArrowType(
            m_manualAdvancedToggle->isChecked() ? Qt::DownArrow : Qt::RightArrow);
    }

    // Expanding the Advanced section is the biggest single height change this
    // page has, and it happens on its own whenever a saved source path goes
    // stale — the exact VPN case this feature exists for. The extra height goes
    // into the scroll range now, not into the top-level minimum; this only
    // re-activates the body layout so the new hint is published immediately
    // rather than on the next spontaneous relayout.
    refitToContent();
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

void ConnectionPanel::showRadioContextMenu(const QPoint& pos)
{
    QListWidgetItem* item = m_radioList->itemAt(pos);
    if (!item)
        return;
    const int row = m_radioList->row(item);
    if (row < 0 || row >= m_radios.size())
        return;
    const RadioInfo radio = m_radios[row];

    // Flex stores its name on the radio itself (set from Radio Setup while
    // connected); offering a client-side override here would create two sources
    // of truth. So the client-side nickname is only for families without an
    // on-radio store (HL2, sim, any future non-Flex backend).
    if (hl2::Hl2Discovery::nicknameLivesOnRadio(radio))
        return;

    QMenu menu(this);
    QAction* setNick = menu.addAction(tr("Set Nickname…"));
    const bool hasCustom =
        hl2::Hl2Discovery::hasCustomNickname(radio.family, radio.serial);
    QAction* clearNick = hasCustom ? menu.addAction(tr("Clear Nickname")) : nullptr;

    QAction* chosen = menu.exec(m_radioList->mapToGlobal(pos));
    if (!chosen)
        return;

    if (chosen == setNick) {
        bool ok = false;
        const QString current = hl2::Hl2Discovery::effectiveNickname(
            radio.family, radio.serial, QString());
        const QString name = QInputDialog::getText(
            this, tr("Set Nickname"),
            tr("Nickname for %1:").arg(radio.model),
            QLineEdit::Normal, current, &ok);
        if (ok) {
            // setNickname commits eagerly — a naming the operator just
            // confirmed shouldn't be lost to a crash or a kill.
            hl2::Hl2Discovery::setNickname(radio.family, radio.serial,
                                           name.trimmed());
        }
    } else if (clearNick && chosen == clearNick) {
        hl2::Hl2Discovery::setNickname(radio.family, radio.serial, QString());
    }

    // Reflect the change immediately: re-label this row from the saved setting
    // rather than waiting for the next discovery sweep.
    RadioInfo updated = radio;
    updated.nickname =
        hl2::Hl2Discovery::effectiveNickname(radio.family, radio.serial,
                                             radio.model);
    m_radios[row] = updated;
    item->setText(formatLocalRadioLabel(updated));
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

    // Demo mode: keep the synthetic entry sorted LAST so a real radio always
    // takes precedence in the list and the demo never gets in the way (RFC
    // #4288). If a real radio was just added while the demo is present, move the
    // demo to the bottom. (No-op when the entry being added IS the demo.)
    if (radio.serial != SimBackend::demoSerial()) {
        moveDemoRadioToBottom();
    }
    updateLocalPageState();
}

void ConnectionPanel::moveDemoRadioToBottom()
{
    const QString demoSerial = SimBackend::demoSerial();
    for (int i = 0; i < m_radios.size(); ++i) {
        if (m_radios[i].serial != demoSerial) {
            continue;
        }
        if (i == m_radios.size() - 1) {
            return;   // already last
        }
        const bool wasSelected = m_radioList->currentRow() == i;
        const RadioInfo demo = m_radios.takeAt(i);
        delete m_radioList->takeItem(i);
        m_radios.append(demo);
        m_radioList->addItem(formatLocalRadioLabel(demo));
        // A real radio outranks the demo: never leave the demo auto-selected
        // once something real is present.
        if (wasSelected && m_radioList->count() > 1) {
            m_radioList->setCurrentRow(0);
        }
        return;
    }
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

void ConnectionPanel::addDemoRadio()
{
    RadioInfo demo;
    // Impersonate a FLEX so AE's model/band logic treats it like a normal radio;
    // the nickname carries the unmistakable "not on the air" label. The serial is
    // SimBackend::demoSerial() so the backend factory can recognize this target.
    demo.name = QStringLiteral("FLEX-6700");
    demo.model = SimBackend::demoModelName();
    demo.serial = SimBackend::demoSerial();
    // The demo is its own BACKEND FAMILY, even though it impersonates a Flex in
    // the picker. RadioModel::connectToRadio switches backends on this field, so
    // leaving it empty (defaulting to "flex") made the family switch build a
    // FlexBackend for the demo target — and the demo then ran on the wrong
    // backend entirely. One selector, and this is it.
    demo.family = SimBackend::familyName();
    demo.version = QStringLiteral("0.0.0.0");
    demo.nickname = QStringLiteral("Simulator (not on the air)");
    demo.callsign = QStringLiteral("DEMO");
    demo.address = QHostAddress(QHostAddress::LocalHost);   // synthetic; never dialed
    demo.port = 4992;
    demo.status = QStringLiteral("Available");
    demo.isSystemModel = false;
    demo.multiFlexEnabled = false;

    // Reuse the normal discovery ingest path (dedupes by serial), so the demo
    // entry behaves exactly like a discovered radio in the list.
    onRadioDiscovered(demo);
}

void ConnectionPanel::removeDemoRadio()
{
    onRadioLost(SimBackend::demoSerial());
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

    const RadioInfo& info = m_radios[row];
    // F5 (#4448): a non-Flex family (HL2) is single-client under HPSDR Protocol 1
    // — an in-use radio can't be shared, and connecting would wedge both clients.
    // Fail closed. Flex multiFlex sharing is a separate, Flex-only path.
    if (info.inUse && info.family != QLatin1String("flex")) {
        setStatusText(QStringLiteral(
            "%1 is already in use by another client and can't be shared.")
            .arg(info.model));
        return;
    }

    // Low bandwidth mode is a slow-link concession, and for a Flex on the LAN it
    // is meaningless — so a local Flex connect clears it. Do NOT clear it for
    // other families: on the HL2 the same preference caps the panadapter span
    // (there the span IS the data rate), and unconditionally writing False here
    // meant that ceiling could never engage on the HL2's only discovery path.
    // (#4470)
    if (info.family.compare(QLatin1String("flex"), Qt::CaseInsensitive) == 0) {
        auto& settings = AppSettings::instance();
        settings.setValue("LowBandwidthConnect", "False");
        settings.save();
    }

    // A staged Icom credential belongs to the attempt it was staged for.
    clearPendingIcomCredentials();
    emit connectRequested(info);
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

void ConnectionPanel::applySavedSourceSelection(const QString& ip, bool restoreFamily)
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

    // Restore the radio type this address was last reached with, so picking a
    // recent HL2 address out of the dropdown does not silently probe it as a
    // Flex (and time out on TCP/4992).
    //
    // ONLY when the address was CHOSEN, never while it is being typed — see the
    // parameter's note in the header. The bind settings below are restored on
    // both paths: they are not a control the operator is looking at, and an
    // address's VPN source path is a property of the address either way.
    if (restoreFamily)
        setManualFamily(familyFromProfile(profile));

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

QString ConnectionPanel::currentManualFamily() const
{
    const QString family = m_manualRadioTypeCombo
        ? m_manualRadioTypeCombo->currentData().toString()
        : QString();
    return family.isEmpty() ? QString::fromLatin1(kFamilyFlex) : family;
}

void ConnectionPanel::setManualFamily(const QString& family)
{
    if (!m_manualRadioTypeCombo)
        return;

    const QString lowered = family.trimmed().toLower();
    const QString wanted =
        lowered == QLatin1String(kFamilyHl2)  ? QString::fromLatin1(kFamilyHl2)
      : lowered == QLatin1String(kFamilyIcom) ? QString::fromLatin1(kFamilyIcom)
                                              : QString::fromLatin1(kFamilyFlex);
    const int index = m_manualRadioTypeCombo->findData(wanted);
    if (index < 0 || index == m_manualRadioTypeCombo->currentIndex()) {
        updateManualFamilyHints();
        return;
    }

    m_manualRadioTypeCombo->setCurrentIndex(index);   // persists via currentIndexChanged
    // Render the hint here too, rather than relying on currentIndexChanged.
    //
    // The constructor calls this BEFORE that signal is connected, so
    // setCurrentIndex() fires into nothing and the hint paragraph — constructed
    // empty — stayed blank with the Flex placeholder still showing. Only `hl2`
    // reached this line at construction time: `flex` is already the current
    // index and returns through the branch above, which is why the default
    // looked correct and a saved HL2 did not. Idempotent, so the signal doing it
    // again once connected is harmless. (PR #4528 review.)
    updateManualFamilyHints();
}

// Build the model list, and select what the settings say.
//
// FROM knownModels(), never hand-typed, so the list cannot drift from the table
// the backend decodes against. A model added there appears here for free —
// which is the point, given the table is where the IC-7760 row is missing.
void ConnectionPanel::populateIcomCivCombo()
{
    if (!m_manualIcomCivCombo)
        return;
    const QSignalBlocker block(m_manualIcomCivCombo);
    m_manualIcomCivCombo->clear();

    // FIRST, and the default. Auto-detect is measured working on both lab
    // radios, needs no table, and is correct when the address was changed on the
    // radio — so it is the right thing for an operator who has never heard of
    // CI-V to land on without touching anything.
    m_manualIcomCivCombo->addItem(tr("Auto-detect (recommended)"),
                                  QStringLiteral("__auto__"));
    for (const auto& model : AetherSDR::icom::knownModels()) {
        // ONLY RADIOS THIS PAGE CAN ACTUALLY DIAL.
        //
        // `hasNetwork` false means CI-V only — a serial port, or Icom's own
        // RS-BA1 *server* software on a PC acting as a front end. The IC-7300 is
        // the one such row today, and offering it here would invite an operator
        // with a USB-only IC-7300 to pick it and get a connect timeout on a page
        // whose whole premise is "you already know the radio's IP".
        //
        // The server-fronted case is not lost: that session's address is still
        // the radio's 0x94, reachable through `Custom...`. It is the rarer path
        // and the one where auto-detect by NAME cannot help anyway, because the
        // handshake names the server rather than the radio behind it.
        if (!model.hasNetwork)
            continue;
        const QString name = QString::fromUtf8(model.name.data(),
                                               static_cast<int>(model.name.size()));
        const QString hex =
            QStringLiteral("%1").arg(model.civAddress, 2, 16, QLatin1Char('0')).toUpper();
        // "%1 — %2" is populateSerialPortCombo's own label idiom, with the raw
        // value in userData so the read-back never has to re-parse the label.
        m_manualIcomCivCombo->addItem(QStringLiteral("%1 — %2").arg(name, hex), hex);
    }
    // TRAILING sentinel, and the read-back below relies on it being last.
    m_manualIcomCivCombo->addItem(tr("Custom..."), QStringLiteral("__custom__"));

    const QSignalBlocker blockEdit(m_manualIcomCivEdit);
    switch (IcomSettings::civSelection()) {
    case IcomSettings::CivSelection::Auto:
        m_manualIcomCivCombo->setCurrentIndex(0);
        break;
    case IcomSettings::CivSelection::Model:
    case IcomSettings::CivSelection::Custom: {
        const QString hex = QStringLiteral("%1")
                                .arg(IcomSettings::civAddress(), 2, 16, QLatin1Char('0'))
                                .toUpper();
        int found = -1;
        for (int i = 1; i < m_manualIcomCivCombo->count() - 1; ++i) {
            if (m_manualIcomCivCombo->itemData(i).toString() == hex) {
                found = i;
                break;
            }
        }
        // A SAVED VALUE THAT MATCHES NO ITEM FALLS BACK TO Custom... WITH THE
        // EDIT PRE-FILLED, rather than being silently dropped — which is exactly
        // the changed-CI-V-address case this field exists for, and the one a
        // naive "select it or give up" would throw away on every restart.
        if (found >= 0 && IcomSettings::civSelection()
                              == IcomSettings::CivSelection::Model) {
            m_manualIcomCivCombo->setCurrentIndex(found);
        } else {
            m_manualIcomCivCombo->setCurrentIndex(m_manualIcomCivCombo->count() - 1);
            if (m_manualIcomCivEdit)
                m_manualIcomCivEdit->setText(hex);
        }
        break;
    }
    }
    syncIcomCivCustomRow();
}

void ConnectionPanel::syncIcomCivCustomRow()
{
    if (!m_manualIcomCivCombo || !m_manualIcomCivCustomRow)
        return;
    const bool icom = currentManualFamily() == QLatin1String(kFamilyIcom);
    const bool custom =
        m_manualIcomCivCombo->currentData().toString() == QLatin1String("__custom__");
    m_manualIcomCivCustomRow->setVisible(icom && custom);
}

void ConnectionPanel::updateManualFamilyHints()
{
    const QString family = currentManualFamily();
    const bool hl2  = family == QLatin1String(kFamilyHl2);
    const bool icom = family == QLatin1String(kFamilyIcom);

    // The credential pair belongs to Icom alone. Hiding the row CONTAINERS
    // rather than the fields keeps their labels from being left behind.
    if (m_manualIcomUserRow)
        m_manualIcomUserRow->setVisible(icom);
    if (m_manualIcomPassRow)
        m_manualIcomPassRow->setVisible(icom);
    if (m_manualIcomCivRow)
        m_manualIcomCivRow->setVisible(icom);
    // The hex row has a second condition — "Custom..." — so it gets the shared
    // helper rather than a copy of the visibility rule.
    syncIcomCivCustomRow();

    if (icom) {
        // Fill from settings, and read the password out of the keychain — which
        // is asynchronous, so the field populates a beat later. Guarded by the
        // widget pointer inside the callback because the panel can be closed
        // between the request and the answer.
        if (m_manualIcomUserEdit && m_manualIcomUserEdit->text().isEmpty())
            m_manualIcomUserEdit->setText(IcomSettings::username());
        // The chooser answers this now. Rebuilt rather than left alone so a
        // settings change made elsewhere in the session is reflected, and
        // because the selection is what decides whether the hex row is showing.
        //
        // EXCEPT over an address the operator is still typing. This function is
        // reached from setManualFamily(), which applySavedSourceSelection()
        // calls when a recent host is picked from the dropdown — so selecting
        // "Custom...", typing an address and then choosing an IP rebuilt the
        // chooser from settings, reset it to Auto, hid the row and discarded the
        // entry with nothing said. The sibling user / password / IP fills below
        // have always guarded on isEmpty() for exactly this reason; the chooser
        // is a combo rather than a line edit, so its "unsaved work in progress"
        // is the Custom hex field standing open with something in it.
        const bool customEntryInFlight =
            m_manualIcomCivCombo
            && m_manualIcomCivCombo->currentData().toString()
                   == QLatin1String("__custom__")
            && m_manualIcomCivEdit && !m_manualIcomCivEdit->text().isEmpty();
        if (!customEntryInFlight)
            populateIcomCivCombo();
        if (m_manualIpEdit && m_manualIpEdit->text().isEmpty())
            m_manualIpEdit->setText(IcomSettings::lastHost());
        if (m_manualIcomPassEdit && m_manualIcomPassEdit->text().isEmpty()) {
            QPointer<QLineEdit> field(m_manualIcomPassEdit);
            IcomCredentials::load(this, [field](const QString& password) {
                if (field && field->text().isEmpty())
                    field->setText(password);
            });
        }
    }

    if (m_manualHintLabel) {
        const QString passwordStorageHint = IcomCredentials::persistentStoreAvailable()
            ? QStringLiteral(
                  "The password is stored in your operating system keychain, never in the "
                  "settings file.")
            : QStringLiteral(
                  "This build has no QtKeychain support, so the password is kept for this "
                  "session only.");
        m_manualHintLabel->setText(
            icom
                ? QStringLiteral(
                      "Enter the radio address and the network user name and password "
                      "configured for network control. %1")
                      .arg(passwordStorageHint)
                : hl2
                ? QStringLiteral(
                      "Use this path when discovery broadcasts cannot reach the radio — a VPN, a "
                      "routed subnet, or a switch that drops broadcasts. AetherSDR sends a "
                      "Hermes-Lite 2 discovery request straight to the address you enter.")
                : QStringLiteral(
                      "Use this path for VPN or other routed networks where discovery broadcasts "
                      "cannot reach the radio. Enter the radio IP address and AetherSDR will take "
                      "care of the probe."));
    }
    if (m_manualIpEdit) {
        m_manualIpEdit->setPlaceholderText(
            icom ? QStringLiteral("Example: radio.local or 192.168.1.90")
          : hl2  ? QStringLiteral("Example: 192.168.1.21")
                 : QStringLiteral("Example: 10.0.0.25"));
    }
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
    identity["family"] = currentManualFamily();
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
        applySavedSourceSelection(trimmed, /*restoreFamily=*/false);
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

void ConnectionPanel::probeRadio(const QString& ip, bool restoreSavedFamily)
{
    const QString trimmedIp = ip.trimmed();
    if (trimmedIp.isEmpty())
        return;

    // Interactive and automation probes keep the family currently selected by
    // the operator. Startup is the exception: it has no current operator
    // choice, so restoreSavedFamily asks the retained route which wire protocol
    // belongs to the saved address.
    if (m_manualIpEdit->text().trimmed() != trimmedIp) {
        m_manualIpEdit->setText(trimmedIp);
        applySavedSourceSelection(trimmedIp, restoreSavedFamily);
    } else if (m_manualProfileIp != trimmedIp) {
        applySavedSourceSelection(trimmedIp, restoreSavedFamily);
    } else if (restoreSavedFamily) {
        applySavedSourceSelection(trimmedIp, /*restoreFamily=*/true);
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

    // Probe exactly the family the operator selected. The two wire protocols
    // are disjoint — a Hermes-Lite 2 speaks HPSDR Protocol 1 on UDP/1024 and
    // never answers the Flex TCP/4992 command plane, and a Flex never answers a
    // Metis discovery datagram — so there is nothing to gain from trying both,
    // and trying HL2 first (as this used to) charged every Flex connect the HL2
    // timeout before it started.
    if (currentManualFamily() == QLatin1String(kFamilyIcom)) {
        // NO ANONYMOUS PROBE. A Flex answers TCP/4992 and an HL2 answers a
        // Metis datagram without credentials, so both can be probed before
        // committing. An Icom will not answer usefully until the RS-BA1 login
        // has succeeded — so the connect IS the probe, and a wrong password
        // surfaces as a session failure rather than as "nothing there".
        QString user = m_manualIcomUserEdit ? m_manualIcomUserEdit->text().trimmed()
                                            : QString();
        QString pass = m_manualIcomPassEdit ? m_manualIcomPassEdit->text() : QString();
        // Fall back to stored credentials when the fields are empty. This is
        // what makes the bridge's `connect ip <host> icom` work at all: an
        // automation launch has no dialog to type into, so without it the verb
        // could only ever report "needs a user name and password".
        if (user.isEmpty())
            user = IcomSettings::username();
        if (pass.isEmpty())
            pass = IcomCredentials::sessionPassword();
        if (user.isEmpty()) {
            resetManualConnectButton();
            setManualMessage(
                QStringLiteral("An Icom needs the network user name and password set on the "
                               "radio. Check Network Control is ON in the radio's menu, then "
                               "enter the same credentials here."),
                true);
            return;
        }
        if (pass.isEmpty()) {
            // Keychain reads are asynchronous. Startup auto-connect reaches
            // this path on a fixed timer, so relying on the dialog's earlier
            // best-effort read races a locked or merely slow macOS Keychain.
            // Finish the read here and resume the same family-selected probe;
            // the synchronous connect path itself remains keychain-free.
            const QString requestedHost = trimmedIp;
            setManualMessage(QStringLiteral("Loading the saved Icom password…"));
            QPointer<ConnectionPanel> panel(this);
            IcomCredentials::load(this, [panel, requestedHost](const QString& password) {
                if (!panel) {
                    return;
                }
                if (panel->currentManualFamily() != QLatin1String(kFamilyIcom)
                    || !panel->m_manualIpEdit
                    || panel->m_manualIpEdit->text().trimmed() != requestedHost) {
                    panel->resetManualConnectButton();
                    panel->m_manualConnectPending = false;
                    return;
                }
                if (password.isEmpty()) {
                    panel->resetManualConnectButton();
                    panel->setManualMessage(
                        QStringLiteral(
                            "No saved Icom password is available. Enter the network password "
                            "set on the radio, then connect once to remember it."),
                        true);
                    panel->m_manualConnectPending = false;
                    return;
                }
                if (panel->m_manualIcomPassEdit
                    && panel->m_manualIcomPassEdit->text().isEmpty()) {
                    panel->m_manualIcomPassEdit->setText(password);
                }
                panel->probeRadio(requestedHost);
            });
            return;
        }

        IcomSettings::setUsername(user);
        // Hex, with or without an 0x prefix or a trailing h — the radio's own
        // menu writes it as "A2h", so accept what the operator is looking at.
        // An unparseable or out-of-range entry is IGNORED rather than clamped:
        // a wrong CI-V address is silent (the radio simply never answers), so
        // guessing on the operator's behalf would hide their typo behind the
        // exact symptom this field exists to cure.
        // WITH A NON-EDITABLE COMBO THE READ-BACK IS UNAMBIGUOUS: branch on
        // currentData(), which is the raw value the item was built with. The old
        // single-field form had to infer intent from an empty string, and could
        // not tell "the operator chose A4" from "nobody chose anything" at all.
        if (m_manualIcomCivCombo) {
            const QString sel = m_manualIcomCivCombo->currentData().toString();
            if (sel == QLatin1String("__auto__")) {
                // NOTHING IS WRITTEN AS AN ADDRESS. Detected is not chosen: an
                // auto-detected value persisted as though it had been typed
                // would turn this session's radio into next session's pin, and
                // "auto" would survive exactly one connect.
                IcomSettings::setCivAddressAuto();
            } else if (sel != QLatin1String("__custom__")) {
                // A PICKED MODEL is a shortcut for an address, so the radio's
                // own 0x19 0x00 reply may correct it — see IcomSettings.h.
                bool okPick = false;
                const uint picked = sel.toUInt(&okPick, 16);
                if (okPick && picked > 0 && picked <= 0xFF)
                    IcomSettings::setCivAddressFromModel(static_cast<std::uint8_t>(picked));
            } else if (m_manualIcomCivEdit) {
                QString civ = m_manualIcomCivEdit->text().trimmed();
                if (civ.isEmpty()) {
                    // "Custom..." with nothing in it is not a choice. Fall back to
                    // auto rather than to a stale value the operator just cleared.
                    IcomSettings::setCivAddressAuto();
                } else {
                    if (civ.startsWith(QLatin1String("0x"), Qt::CaseInsensitive))
                        civ = civ.mid(2);
                    if (civ.endsWith(QLatin1Char('h'), Qt::CaseInsensitive))
                        civ.chop(1);
                    bool ok = false;
                    const uint addr = civ.toUInt(&ok, 16);
                    if (ok && addr > 0 && addr <= 0xFF) {
                        IcomSettings::setCivAddress(static_cast<std::uint8_t>(addr));
                    } else {
                        // SAY SO rather than connecting with something else. This
                        // field exists because a wrong CI-V address fails SILENTLY
                        // — the radio just never answers — so silently ignoring bad
                        // input reproduces the exact symptom the field is here to
                        // cure, and the operator would be left reading a "no reply"
                        // that their typo caused.
                        setStatusText(tr("CI-V address \"%1\" is not a hex byte "
                                         "(try A2, 0xA2 or A2h) — not connecting.")
                                          .arg(m_manualIcomCivEdit->text().trimmed()));
                        m_manualIcomCivEdit->setFocus();
                        m_manualIcomCivEdit->selectAll();
                        return;
                    }
                }
            }
        }

        // NOT setLastHost, and NOT save(), until the radio has actually
        // accepted us.
        //
        // Both used to run here, before the connect was even attempted. One
        // mistyped password therefore replaced a working keychain entry with a
        // broken one — permanently, with no way to recover it — and the
        // last-host setting was overwritten with an address that never
        // answered. The operator's only symptom is that the NEXT connect fails
        // for a reason they did not cause.
        //
        // The session cache is still primed immediately, because the connect
        // below reads it synchronously and must not race a keyring write. What
        // is deferred is the DURABLE copy: onIcomConnectSucceeded() commits it
        // once the radio has answered. See setConnected().
        IcomCredentials::setSessionPassword(pass);
        m_pendingIcomHost = trimmedIp;
        m_pendingIcomPassword = pass;
        m_pendingIcomBindSettings = bindSettings;
        m_pendingIcomSessionBindAddress =
            bindSettings.mode == RadioBindMode::Explicit
                ? bindSettings.bindAddress
                : QHostAddress();

        // RESOLVE FIRST. QHostAddress parses NUMERIC addresses only — given a
        // host name it yields a null address SILENTLY, and the connect then
        // goes to "" and times out with a message about the radio's network
        // settings. That bites hardest on exactly the documented case: the
        // IC-705's default name is ic-705.local, which is what this field's own
        // placeholder suggests.
        QHostAddress resolved(trimmedIp);
        if (resolved.isNull()) {
            const QHostInfo hostInfo = QHostInfo::fromName(trimmedIp);
            if (hostInfo.addresses().isEmpty()) {
                resetManualConnectButton();
                setManualMessage(
                    QStringLiteral("Could not resolve \"%1\". Check the name, or enter the "
                                   "radio's IP address instead.").arg(trimmedIp),
                    true);
                return;
            }
            resolved = hostInfo.addresses().first();
        }
        m_pendingIcomResolvedHost = resolved.toString();

        RadioInfo info;
        info.family   = QString::fromLatin1(kFamilyIcom);
        info.address  = resolved;
        info.port     = IcomSettings::controlPort();
        info.model    = QStringLiteral("Icom");
        info.name     = info.model;
        // No discovery means no MAC and no reported serial, so the host is the
        // only stable identity this radio has for us. It has to be SOMETHING:
        // the restore/persist scope keys off it.
        info.serial   = QStringLiteral("icom:%1").arg(resolved.toString());
        info.nickname = info.model;
        // Manual Icom sessions use the same retention path as routed Flex and
        // HL2 sessions. Without this marker MainWindow removes
        // LastRoutedRadioIp immediately, so the startup checkbox has no host to
        // reconnect to even though LastConnectedRadioSerial was retained.
        info.isRouted           = true;
        info.bindSettings       = bindSettings;
        info.sessionBindAddress = m_pendingIcomSessionBindAddress;
        rememberManualIp(trimmedIp);
        resetManualConnectButton();
        emit connectRequested(info);
        return;
    }

    if (currentManualFamily() == QLatin1String(kFamilyHl2)) {
        // Only a genuine timeout earns the "check the radio" message. A bind
        // failure has already reported itself, and pointing the operator at the
        // radio would be actively wrong. (PR #4528 review.)
        const Hl2ProbeResult probe = probeHermesLite2(trimmedIp, bindSettings);
        if (probe == Hl2ProbeResult::NoAnswer) {
            resetManualConnectButton();
            setManualMessage(
                QStringLiteral("No Hermes-Lite 2 answered at %1. Check the address, and that the "
                               "radio is powered, idle, and reachable on UDP port 1024.")
                    .arg(trimmedIp),
                true);
        } else if (probe == Hl2ProbeResult::NotAttempted) {
            resetManualConnectButton();
        }
        return;
    }

    probeFlexRadio(trimmedIp, bindSettings);
}

void ConnectionPanel::refitToContent()
{
    if (!m_rootLayout)
        return;

    // The body owns overflow now. Refresh its geometry so expanding Advanced or
    // wrapping a result message extends the scroll range instead of increasing
    // the top-level minimum past the screen's available height.
    m_rootLayout->invalidate();
    m_rootLayout->activate();
}

void ConnectionPanel::resetManualConnectButton()
{
    m_manualConnectPending = false;
    m_manualConnectBtn->setText(QStringLiteral("Connect by IP"));
    m_manualConnectBtn->setEnabled(true);
    updateActionState();
}

// Directed Metis discovery: the same EF FE 02 request Hl2Discovery broadcasts,
// sent unicast to one host. This is the whole reason connect-by-IP works for an
// HL2 on a routed/VPN path — the broadcast sweep never leaves the local subnet.
// Bounded (~600 ms) blocking wait on a path that is already a modal
// "Checking..." step, matching the Flex probe's synchronous feel.
ConnectionPanel::Hl2ProbeResult ConnectionPanel::probeHermesLite2(
    const QString& ip, const RadioBindSettings& bindSettings)
{
    QUdpSocket hpsdr;
    // Honour the Advanced source-path choice the same way the Flex probe does.
    // On a VPN that exposes more than one adapter, letting the OS pick can send
    // the request out the wrong interface and the reply never comes back.
    const bool explicitBind = bindSettings.mode == RadioBindMode::Explicit
                           && !bindSettings.bindAddress.isNull();
    const bool bound = explicitBind
        ? hpsdr.bind(bindSettings.bindAddress, 0)
        : hpsdr.bind(QHostAddress(QHostAddress::AnyIPv4), 0);
    if (!bound) {
        // REPORT THE BIND FAILURE AS ITSELF, not as silence from the radio.
        //
        // Returning a bare false here made this indistinguishable from "nothing
        // answered", and the caller renders that as "check the radio is powered,
        // idle, and reachable" — sending the operator to power-cycle a radio
        // that was never contacted. The Explicit path exists because a VPN can
        // expose several adapters, so the likeliest cause of a bind failure is
        // an Advanced source path naming an adapter that has since gone away:
        // precisely the case where pointing at the radio is wrong.
        //
        // probeFlexRadio() already reports this properly; the two paths had
        // drifted apart. (PR #4528 review.)
        if (explicitBind) {
            m_manualSourceWarningLabel->setText(
                QStringLiteral("Failed to bind %1: %2")
                    .arg(bindSettings.bindAddress.toString(), hpsdr.errorString()));
            m_manualSourceWarningLabel->setVisible(true);
            updateManualAdvancedVisibility();
            setManualMessage(
                QStringLiteral("AetherSDR could not use that VPN source path. "
                               "Try Auto or choose another path."),
                true);
        } else {
            setManualMessage(
                QStringLiteral("Could not open a UDP socket to probe for a "
                               "Hermes-Lite 2: %1").arg(hpsdr.errorString()),
                true);
        }
        return Hl2ProbeResult::NotAttempted;
    }

    // RESOLVE A NAME BEFORE PROBING IT.
    //
    // QHostAddress(ip) is null for anything that is not a literal address, and a
    // writeDatagram() to a null destination sends nothing — so a hostname used to
    // fail as a 600 ms silence and then get reported as "No Hermes-Lite 2 answered
    // … check the radio is powered", blaming the radio for an input this path
    // never tried to send to.
    //
    // Names have to work here because the Flex path accepts them (connectToHost()
    // resolves internally) and docs/automation-bridge.md documents the verb as
    // `connect ip <host-or-ip> [flex|hl2]`. QHostInfo::fromName() is synchronous,
    // which suits a path that already blocks ~600 ms on the reply.
    //
    // IPv4 only, and not an arbitrary pick from the list: Metis is IPv4-only, so a
    // AAAA-only name has nothing this protocol can talk to and should say so
    // rather than fail as silence. (PR #4528 review.)
    QHostAddress dest(ip);
    if (dest.isNull()) {
        const QHostInfo resolved = QHostInfo::fromName(ip);
        for (const QHostAddress& a : resolved.addresses()) {
            if (a.protocol() == QAbstractSocket::IPv4Protocol) {
                dest = a;
                break;
            }
        }
        if (dest.isNull()) {
            setManualMessage(
                resolved.error() != QHostInfo::NoError
                    ? QStringLiteral("Could not resolve “%1”: %2")
                          .arg(ip, resolved.errorString())
                    : QStringLiteral("“%1” has no IPv4 address, and a Hermes-Lite 2 "
                                     "is reachable over IPv4 only.").arg(ip),
                true);
            return Hl2ProbeResult::NotAttempted;
        }
    }

    const auto request = hl2::discoveryRequest();
    // A send that never left is not a radio that stayed silent. Without this the
    // two are indistinguishable and both surface as "check the radio is powered".
    if (hpsdr.writeDatagram(reinterpret_cast<const char*>(request.data()),
                            qint64(request.size()),
                            dest,
                            hl2::kMetisPort) < 0) {
        setManualMessage(
            QStringLiteral("Could not send a discovery request to %1: %2")
                .arg(dest.toString(), hpsdr.errorString()),
            true);
        return Hl2ProbeResult::NotAttempted;
    }

    QDeadlineTimer deadline(600);
    while (!deadline.hasExpired()) {
        if (!hpsdr.waitForReadyRead(static_cast<int>(deadline.remainingTime())))
            break;
        while (hpsdr.hasPendingDatagrams()) {
            const QByteArray d = hpsdr.receiveDatagram().data();
            const auto reply = hl2::parseDiscoveryReply(
                std::span<const std::uint8_t>(
                    reinterpret_cast<const std::uint8_t*>(d.constData()), std::size_t(d.size())));
            // A bare 0xEFFE reply is any openHPSDR board (Hermes, Mercury,
            // Red Pitaya, …). Only board id 0x06 is a Hermes-Lite; gate on it
            // so we never drive a foreign board through Hl2Backend. Same
            // predicate Hl2Discovery applies to broadcast replies.
            if (!reply || !reply->isHermesLite2())
                continue;

            RadioInfo info;
            info.family   = QString::fromLatin1(kFamilyHl2);
            info.address  = dest;
            info.port     = hl2::kMetisPort;            // Metis, not Flex 4992
            info.model    = QStringLiteral("Hermes-Lite 2");
            info.name     = info.model;
            info.serial   = hl2::Hl2Discovery::macToSerial(reply->mac);
            // Same nickname the broadcast sweep shows for this MAC. An HL2 has no
            // on-radio name store, so the operator's custom name lives client-side
            // keyed by serial — and hard-coding the model here meant a radio named
            // in Radio Setup showed that name when found locally and
            // "Hermes-Lite 2" when reached over the VPN. Needs the serial first.
            info.nickname = hl2::Hl2Discovery::effectiveNickname(info.family, info.serial, info.model);
            info.version  = QString::number(reply->gatewareVersion);
            // Same label Hl2Discovery sets on the broadcast path — this
            // is the SECOND place an HL2 RadioInfo is built, and a field
            // set in only one of them is not set at all.
            info.versionLabel = QStringLiteral("Gateware");
            // Streaming (status byte 0x03) means another client already owns
            // the radio. Reflect it rather than hard-coding Available.
            info.inUse    = reply->streaming;
            info.status   = reply->streaming ? QStringLiteral("In_Use")
                                             : QStringLiteral("Available");
            // Reached over a routed path, not a discovery broadcast — the same
            // flag the Flex manual probe sets, so MainWindow remembers the
            // address and the UI treats the link as remote.
            info.isRouted           = true;
            info.bindSettings       = bindSettings;
            info.sessionBindAddress = bindSettings.mode == RadioBindMode::Explicit
                ? bindSettings.bindAddress
                : QHostAddress();

            resetManualConnectButton();

            if (reply->streaming) {
                // #4448: HPSDR Protocol 1 is single-client. Fail closed rather
                // than wedging both clients; there is no takeover path.
                setManualMessage(
                    QStringLiteral("The Hermes-Lite 2 at %1 is already in use by another client "
                                   "and can't be shared.").arg(ip),
                    true);
                return Hl2ProbeResult::Answered;
            }

            saveManualProfile(ip, bindSettings, info.sessionBindAddress);
            rememberManualIp(ip);
            // #4470: the low-bandwidth checkbox is what caps the HL2 panadapter
            // span, and this page is the one place it is on screen. Save it
            // before we hand off, or ticking it does nothing.
            saveLowBandwidthPreference(m_lowBwCheck->isChecked());
            setManualMessage(
                QStringLiteral("Found a Hermes-Lite 2 at %1 — connecting.").arg(ip), false);
            // A staged Icom credential belongs to the attempt it was staged for.
            clearPendingIcomCredentials();
            emit connectRequested(info);
            return Hl2ProbeResult::Answered;
        }
    }

    return Hl2ProbeResult::NoAnswer;
}

void ConnectionPanel::probeFlexRadio(const QString& trimmedIp, const RadioBindSettings& bindSettings)
{
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
            QHostAddress targetAddress(trimmedIp);
            if (targetAddress.isNull()) {
                targetAddress = sock->peerAddress();
            }

            sock->disconnectFromHost();
            sock->deleteLater();

            RadioInfo info;
            info.address  = targetAddress;
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
                // A staged Icom credential belongs to the attempt it was staged for.
                clearPendingIcomCredentials();
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
