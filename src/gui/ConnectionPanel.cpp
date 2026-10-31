#include "ConnectionPanel.h"
#include "core/AppSettings.h"
#include "models/ConnectionWorkspace.h"
#include "core/backends/ConnectionSharingPolicy.h"  // in-use share gate (#4448), shared with MainWindow_Session
#include "core/backends/anan/AnanDiscovery.h" // shared nickname + MAC->serial helpers
#include "core/backends/anan/P2Protocol.h"  // buildDiscovery/parseDiscoveryReply, kRadioPort
#include "core/backends/hl2/Hl2Discovery.h"   // shared nickname + MAC->serial helpers
#include "core/backends/icom/IcomCredentials.h"  // password -> OS keychain, never settings
#include "core/backends/icom/IcomSettings.h"     // host/user/ports (Principle V)
#include "core/backends/icom/IcomModels.h"       // knownModels() -> the chooser's items
#include "core/backends/hl2/MetisProtocol.h"  // discoveryRequest/parseDiscoveryReply, kMetisPort
#include "core/backends/sim/SimBackend.h"
#include "core/NetworkPathResolver.h"
#include "ComboStyle.h"   // shared themed combo look (painted arrow)
#include "FramelessResizer.h"
#include "CanonIndicators.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <span>

#include <QAbstractItemView>
#include <QAbstractItemModel>
#include <QApplication>
#include <QStyledItemDelegate>
#include <QStyle>
#include <QFont>
#include <QFontMetrics>
#include <QPointer>
#include <QLineEdit>
#include <QFormLayout>
#include <QGuiApplication>
#include <QDialogButtonBox>
#include <QMenu>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QScrollArea>
#include <QScrollBar>
#include <QScreen>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSet>
#include <QSpinBox>
#include <QTcpSocket>
#include <QHostInfo>
#include <QUdpSocket>
#include <QNetworkDatagram>
#include <QDeadlineTimer>
#include <QTimer>
#include <QVBoxLayout>
#include <QWindow>
#include <QMediaDevices>
#include <QAudioDevice>
#include "core/ThemeManager.h"

namespace AetherSDR {

namespace {

class RadioListDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        QStyleOptionViewItem background(option);
        initStyleOption(&background, index);
        background.text.clear();
        const QStyle* style = option.widget ? option.widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &background, painter, option.widget);
        const QStringList lines = index.data().toString().split('\n');
        const QRect rect = option.rect.adjusted(10, 8, -10, -8);
        painter->save();
        QFont titleFont = option.font;
        titleFont.setBold(true);
        painter->setFont(titleFont);
        painter->setPen(ThemeManager::instance().color(option.widget, "color.canon.ink"));
        const QFontMetrics titleMetrics(titleFont);
        painter->drawText(rect, Qt::AlignLeft | Qt::AlignTop, titleMetrics.elidedText(lines.value(0), Qt::ElideRight, rect.width()));
        QFont detailFont = option.font;
        if (detailFont.pointSizeF() > 1) { detailFont.setPointSizeF(detailFont.pointSizeF() - 1); }
        painter->setFont(detailFont);
        painter->setPen(ThemeManager::instance().color(option.widget, "color.canon.muted"));
        painter->drawText(rect.adjusted(0, titleMetrics.height() + 4, 0, 0), Qt::AlignLeft | Qt::AlignTop,
            QFontMetrics(detailFont).elidedText(lines.value(1), Qt::ElideRight, rect.width()));
        painter->restore();
    }
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex&) const override
    { return {180, QFontMetrics(option.font).height() * 2 + 22}; }
};

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
    "QLabel { color: {{color.canon.muted}}; font-size: 11px; background: transparent; border: none; }";
const char* kInfoLabelStyle =
    "QLabel { color: {{color.canon.cyan}}; font-size: 11px; background: transparent; border: none; }";
const char* kErrorLabelStyle =
    "QLabel { color: {{color.accent.danger}}; font-size: 11px; background: transparent; border: none; }";

// The style guide's canon vocabulary (RFC #6226), as Network Diagnostics and
// AetherMap use it: transparent over CanonWindow's ground except the surfaces
// that hold content. Groups are raised cards, lists nested, fields on
// control; selection and focus are canon cyan.
constexpr const char* kConnectionStyle = R"(
QWidget {
    color: {{color.canon.inkSoft}};
    background: transparent;
}
QMenu,
QComboBoxPrivateContainer {
    background: {{color.canon.raised}};
}
QGroupBox {
    background: {{color.canon.raised}};
    border: 1px solid {{color.canon.line}};
    border-radius: 7px;
    margin-top: 10px;
    color: {{color.canon.ink}};
    font-weight: bold;
}
QGroupBox::title {
    subcontrol-origin: margin;
    left: 10px;
    padding: 0 4px;
    color: {{color.canon.muted}};
}
QListWidget {
    color: {{color.canon.inkSoft}};
    background: {{color.canon.nested}};
    border: 1px solid {{color.canon.line}};
    border-radius: 4px;
    padding: 2px;
    outline: none;
}
QListWidget::item:selected {
    color: {{color.canon.ink}};
    background: {{color.canon.control}};
}
QScrollBar:vertical {
    background: transparent;
    width: 12px;
    margin: 0;
}
QScrollBar::handle:vertical {
    background: {{color.canon.lineHi}};
    border-radius: 5px;
    min-height: 24px;
}
QScrollBar::add-line:vertical,
QScrollBar::sub-line:vertical {
    height: 0;
}
QPushButton {
    color: {{color.canon.cyan}};
    background: {{color.canon.control}};
    border: 1px solid {{color.canon.lineHi}};
    border-radius: 4px;
    padding: 5px 12px;
    font-weight: 600;
}
QPushButton:hover {
    background: {{color.canon.nested}};
    color: {{color.canon.aqua}};
}
QPushButton:focus {
    border-color: {{color.canon.aqua}};
}
QPushButton:disabled {
    color: {{color.canon.muted}};
    border-color: {{color.canon.line}};
    background: transparent;
}
QCheckBox {
    background: transparent;
    color: {{color.canon.inkSoft}};
}
)";

// Labels and cards here carry {{token}} placeholders, so they go through the
// theme rather than setStyleSheet(), which would resolve nothing.
void applyStyle(QWidget* widget, const QString& style)
{
    AetherSDR::ThemeManager::instance().applyStyleSheet(widget, style);
}

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

    // Host names are remembered too (QHostAddress parses numeric only); the
    // IC-705's documented default is ic-705.local. The charset is conservative
    // (alphanumeric ends; letters, digits, . - _) so this widens what is stored,
    // not what reaches a resolver.
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
    if (family == QLatin1String(ConnectionPanel::kFamilyAnan))
        return QString::fromLatin1(ConnectionPanel::kFamilyAnan);
    if (family == QLatin1String(ConnectionPanel::kFamilyIcom))
        return QString::fromLatin1(ConnectionPanel::kFamilyIcom);
    if (family == QLatin1String(ConnectionPanel::kFamilyRtl))
        return QString::fromLatin1(ConnectionPanel::kFamilyRtl);
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

bool icomBasePortFromProfile(const QJsonObject& profile, quint16* basePort)
{
    const int stored = profile.value("icom").toObject().value("base_port").toInt(0);
    if (stored <= 0 || stored > IcomSettings::maximumBasePort()) {
        return false;
    }
    if (basePort) {
        *basePort = static_cast<quint16>(stored);
    }
    return true;
}

QString staleSelectionText(const RadioBindSettings& settings)
{
    QString iface = settings.interfaceName.trimmed();
    if (iface.isEmpty())
        iface = settings.interfaceId.trimmed();
    if (iface.isEmpty())
        iface = QStringLiteral("Saved source");
    const QString addr = settings.bindAddress.isNull()
        ? QStringLiteral("unknown address")
        : settings.bindAddress.toString();
    return QStringLiteral("%1 (unavailable, last %2)").arg(iface, addr);
}

QLabel* makeWrappedLabel(const QString& text, const char* style = nullptr)
{
    auto* label = new QLabel(text);
    label->setWordWrap(true);
    if (style)
        applyStyle(label, QString::fromLatin1(style));
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
    : CanonWindow(tr("Connect to a radio"), parent)
{
    setObjectName(QStringLiteral("connectionPanel"));
    setAccessibleName(tr("Connect to Radio"));
    // MainWindow anchors it above the status bar before showing it, and Return
    // in a field (an IP address, a SmartLink password) must not also press the
    // first button on the page.
    setOwnerPlaced(true);
    setReturnClicksDefault(false);

    theme::setContainer(this, QStringLiteral("canon/connection"));
    applyCanonSheet(bodyWidget(), [] {
        return QString::fromLatin1(kConnectionStyle) + canonIndicatorRules();
    });
    const QString editStyle =
        "QLineEdit { border: 1px solid {{color.canon.lineHi}}; border-radius: 4px; padding: 4px 6px; "
        "background: {{color.canon.control}}; color: {{color.canon.ink}}; }"
        "QLineEdit:focus { border-color: {{color.canon.aqua}}; }";
    // Uses the shared ComboStyle (themed, real painted arrow). The override is
    // for row height: these rows are 30 px vs the 22 px applet combos the
    // template was shaped for, so the text needs a larger inset.
    const QString comboExtraRules =
        "QComboBox { padding: 4px 8px; }"
        // Pin the drop-down to the BORDER box. Without this it inherits the
        // padding above and floats inward, detached from the frame's right edge.
        "QComboBox::drop-down { subcontrol-origin: border;"
        " subcontrol-position: top right; width: 22px; border: none; }";

    // The Icom credential fields are plain QLineEdits and inherit no combo
    // styling. Uses theme tokens through ThemeManager so they follow theme
    // changes. 8 px padding matches the combos so text in this column aligns.
    const QString lineEditStyle =
        "QLineEdit { border: 1px solid {{color.background.2}}; border-radius: 2px; "
        "padding: 4px 8px; background: {{color.background.1}}; "
        "color: {{color.text.primary}}; }"
        "QLineEdit:focus { border-color: {{color.accent.bright}}; }";
    const QString modeCardStyle =
        "QCommandLinkButton { text-align: left; border: 1px solid {{color.canon.line}}; border-radius: 8px; "
        "padding: 10px 12px; background: {{color.canon.raised}}; color: {{color.canon.ink}}; }"
        "QCommandLinkButton:hover { border-color: {{color.canon.lineHi}}; background: {{color.canon.nested}}; }"
        "QCommandLinkButton:checked { border-color: {{color.canon.cyan}}; background: {{color.canon.nested}}; }";
    const QString calloutStyle =
        "QFrame#connectionCallout { border: 1px solid {{color.canon.line}}; border-radius: 8px; "
        "background: {{color.canon.nested}}; }"
        "QFrame#connectionCallout QLabel { background: transparent; border: none; }"
        "QFrame#connectionCallout QCheckBox { background: transparent; border: none; }";
    // The indicator is the canon one, from the body sheet.
    const QString lowBandwidthCheckStyle =
        "QCheckBox { color: {{color.canon.inkSoft}}; spacing: 8px; padding: 2px 0; "
        "background: transparent; border: none; }";

    auto* outer = new QVBoxLayout(bodyWidget());
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    // Canon header in place of a title bar, fixed above the scrolling body.
    auto* header = makeCanonHeader(tr("Connect to a radio"));
    header->setContentsMargins(14, 12, 14, 4);
    outer->addWidget(header);

    auto* content = new QWidget(this);
    content->setObjectName(QStringLiteral("connectionBodyContent"));
    auto* root = new QVBoxLayout(content);
    // No right margin: the 12 px band on that edge belongs to the scroll area
    // (see bodyContainer below), so adding one here would inset the body 24 px
    // from the right against 12 px on the left and 12 px on the footer.
    root->setContentsMargins(12, 6, 0, 10);
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

    auto* introLabel = makeWrappedLabel(
        "Pick the simplest path for your station. Most first-time users should start with "
        "\"On This Network\" and only use the IP path for VPN or routed connections.",
        kHintLabelStyle);
    introLabel->hide();

    m_modeButtons = new QButtonGroup(this);
    m_modeButtons->setExclusive(true);

    auto configureModeButton = [&](QCommandLinkButton* button,
                                   const QString& title,
                                   const QString& description,
                                   ConnectionMode mode) {
        button->setText(title);
        button->setDescription(description);
        button->setCheckable(true);
        applyStyle(button, modeCardStyle);
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

    auto* setupModes = new QWidget(this);
    setupModes->setLayout(modeRow);
    setupModes->hide();

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
    applyStyle(emptyCallout, calloutStyle);
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
    applyStyle(m_emailEdit, editStyle);
    m_emailEdit->setPlaceholderText("flexradio account email");
    m_emailEdit->setObjectName(QStringLiteral("smartlinkEmail"));
    m_emailEdit->setAccessibleName(tr("SmartLink account email"));
    m_emailEdit->setAccessibleDescription(
        tr("FlexRadio account email address used to sign in to SmartLink"));
    QString storedEmail = AppSettings::instance().value("SmartLinkEmail").toString();
    if (!storedEmail.isEmpty())
        m_emailEdit->setText(QString::fromUtf8(QByteArray::fromBase64(storedEmail.toUtf8())));
    m_passwordEdit = new QLineEdit(m_loginForm);
    applyStyle(m_passwordEdit, editStyle);
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

    m_slUserLabel = makeWrappedLabel(QString());
    setAccountLine(QStringLiteral("Sign in to see radios at remote stations."), AccountTone::Hint);
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
    auto* manualNameRow = new QFormLayout;
    m_manualConnectionName = new QLineEdit(manualPage);
    m_manualConnectionName->setObjectName(QStringLiteral("connectionManualName"));
    m_manualConnectionName->setAccessibleName(tr("Saved connection name"));
    m_manualConnectionName->setMaxLength(128);
    m_manualConnectionName->setPlaceholderText(tr("Optional, for example IC-705 portable"));
    applyStyle(m_manualConnectionName, editStyle);
    manualNameRow->addRow(tr("Connection name:"), m_manualConnectionName);
    manualLayout->addLayout(manualNameRow);
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
    m_manualRadioTypeCombo->addItem(tr("ANAN-G2"), QString::fromLatin1(kFamilyAnan));
    m_manualRadioTypeCombo->addItem(tr("Icom (network)"), QString::fromLatin1(kFamilyIcom));
#ifdef AETHER_BACKEND_RTL
    m_manualRadioTypeCombo->addItem(tr("RTL-SDR (USB)"), QString::fromLatin1(kFamilyRtl));
#endif
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

    // The RS-BA1 transport is always three UDP ports: control, CI-V and audio.
    // NAT deployments commonly forward several radios through one public IP,
    // so the operator chooses only the first external port and the next two are
    // derived. The ordinary on-radio triplet remains the zero-effort default.
    m_manualIcomPortCombo = new QComboBox(manualGroup);
    m_manualIcomPortCombo->setObjectName(QStringLiteral("connectionManualIcomPortMode"));
    m_manualIcomPortCombo->setAccessibleName(tr("Icom network ports"));
    m_manualIcomPortCombo->setAccessibleDescription(
        tr("Use the standard Icom UDP ports, or choose a custom first port for NAT. "
           "The CI-V and audio ports are the next two sequential ports."));
    AetherSDR::applyComboStyle(m_manualIcomPortCombo, comboExtraRules);
    m_manualIcomPortCombo->addItem(
        tr("Standard (%1–%2)")
            .arg(IcomSettings::defaultBasePort())
            .arg(IcomSettings::defaultBasePort() + 2),
        QStringLiteral("__standard__"));
    m_manualIcomPortCombo->addItem(tr("Custom NAT ports..."),
                                   QStringLiteral("__custom__"));
    m_manualIcomPortRow =
        addManualRow(QStringLiteral("Icom ports:"), m_manualIcomPortCombo);

    m_manualIcomBasePortSpin = new QSpinBox(manualGroup);
    m_manualIcomBasePortSpin->setObjectName(QStringLiteral("connectionManualIcomBasePort"));
    m_manualIcomBasePortSpin->setAccessibleName(tr("Icom first UDP port"));
    m_manualIcomBasePortSpin->setAccessibleDescription(
        tr("First external UDP port forwarded to the radio. AetherSDR also uses the next "
           "two ports for CI-V and audio."));
    m_manualIcomBasePortSpin->setRange(1, IcomSettings::maximumBasePort());
    m_manualIcomBasePortSpin->setValue(IcomSettings::controlPort());
    m_manualIcomBasePortSpin->setToolTip(
        tr("First of three sequential UDP ports: control, CI-V, audio"));
    m_manualIcomPortCustomRow =
        addManualRow(QStringLiteral("First UDP port:"), m_manualIcomBasePortSpin);

    if (!IcomSettings::usesDefaultPorts()) {
        m_manualIcomPortCombo->setCurrentIndex(1);
    }
    connect(m_manualIcomPortCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { syncIcomPortCustomRow(); });

    // CI-V is addressed: a radio on 0xA2 ignores frames to 0xA4, which looks like
    // "no scope, no TX" rather than "wrong address". Mostly a display: connect
    // asks the radio for its address (broadcast 0x19 0x00), so this names the
    // models and gives an explicit override. Non-editable with a "Custom..."
    // sentinel and hidden hex row, mirroring populateSerialPortCombo()
    // (RadioSetupDialog.cpp), which is serial-only and behind HAVE_SERIALPORT.
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

    // Automation: the hex field is hidden until "Custom..." is selected, and the
    // bridge refuses hidden widgets. Select it first:
    //     invoke connectionManualIcomCivCombo setCurrentText "Custom..."
    //     invoke connectionManualIcomCivAddress setText A2
    // or pick the model directly ("IC-9700 — A2"). The objectName is kept so
    // the refusal names the field a script already uses.
    connect(m_manualIcomCivCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { syncIcomCivCustomRow(); });

    // ANAN-G2's front-end options (DDC0 rate, ADC select, dither,
    // randomization, the two filter-bank bypasses and the radio-speaker stream)
    // are set on Settings > Radio Setup > "ANAN Front End", which is reachable
    // with no radio connected: they configure the session a connect starts, so
    // they must be settable before the first one. The connect path reads them
    // from AnanSettings (RadioModel's populateFamilyParams()), never from this
    // form; this row only points there.
    //
    // Added straight to manualForm rather than through addManualRow(): that
    // helper gives its field a FIXED row height, which would clip a label that
    // wraps.
    auto* ananHint = makeWrappedLabel(
        QStringLiteral("Sample rate, ADC choice, dither, randomization, filter bypass and the "
                       "radio's own speaker are in Settings > Radio Setup > ANAN Front End. "
                       "They apply to the next connect."),
        kHintLabelStyle);
    ananHint->setObjectName(QStringLiteral("connectionManualAnanSetupHint"));
    ananHint->setParent(manualGroup);
    manualForm->addWidget(ananHint);
    m_manualAnanHintRow = ananHint;

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
    auto* saveManual = new QPushButton(tr("Save connection"), manualGroup);
    saveManual->setObjectName(QStringLiteral("connectionSaveManual"));
    manualActionRow->addWidget(saveManual);
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
    applyStyle(m_linkOptionsWidget, calloutStyle);
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
    applyStyle(m_lowBwCheck, lowBandwidthCheckStyle);
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
    applyStyle(m_adaptiveThrottleCheck, lowBandwidthCheckStyle);
    connect(m_adaptiveThrottleCheck, &QCheckBox::toggled, this, [](bool on) {
        auto& s = AppSettings::instance();
        s.setValue("AdaptiveThrottleEnabled", on ? "True" : "False");
        s.save();
    });
    auto* preferences = new QWidget(this);
    auto* preferencesLayout = new QVBoxLayout(preferences);
    preferencesLayout->setContentsMargins(0, 0, 0, 0);
    preferencesLayout->addWidget(m_adaptiveThrottleCheck);

    m_autoConnectCheck = new QCheckBox("Connect to last radio on start up", this);
    m_autoConnectCheck->setChecked(
        AppSettings::instance().value("AutoConnectToLastRadio", "True").toString() == "True");
    applyStyle(m_autoConnectCheck, lowBandwidthCheckStyle);
    connect(m_autoConnectCheck, &QCheckBox::toggled, this, [](bool on) {
        auto& s = AppSettings::instance();
        s.setValue("AutoConnectToLastRadio", on ? "True" : "False");
        s.save();
    });
    preferencesLayout->addWidget(m_autoConnectCheck);

    auto* wakeOnConnect = new QCheckBox(tr("Wake Icom on connect"), this);
    wakeOnConnect->setObjectName(QStringLiteral("connectionWakeOnConnect"));
    wakeOnConnect->setAccessibleName(tr("Wake Icom on connect"));
    wakeOnConnect->setAccessibleDescription(tr(
        "If Icom identity does not answer, wake the selected supported model once. "
        "Supports IC-705, IC-7300MK2 and IC-9700, including automatic detection."));
    wakeOnConnect->setToolTip(tr(
        "Wake a supported Icom from standby only if it does not answer identification. "
        "For a custom CI-V address, select the model in Connect by IP. "
        "Does not put the radio to sleep on disconnect."));
    wakeOnConnect->setChecked(IcomSettings::wakeOnConnect());
    AetherSDR::ThemeManager::instance().applyStyleSheet(wakeOnConnect, lowBandwidthCheckStyle);
    connect(wakeOnConnect, &QCheckBox::toggled, this, [](bool on) {
        IcomSettings::setWakeOnConnect(on);
    });
    preferencesLayout->addWidget(wakeOnConnect);

    // Demo mode (RFC #4288): offer the synthetic "AetherSDR Demo — Simulator"
    // entry in the radio list. Default on for discoverability; the choice
    // persists. Toggling just writes the setting and shows/hides the entry — the
    // connection (and backend selection) happens only when the user connects to
    // it, exactly like a real radio.
    m_showDemoCheck = new QCheckBox("Show the AetherSDR demo simulator", this);
    m_showDemoCheck->setChecked(
        AppSettings::instance().value("ShowDemoRadio", "True").toString() == "True");
    applyStyle(m_showDemoCheck, lowBandwidthCheckStyle);
    connect(m_showDemoCheck, &QCheckBox::toggled, this, [this](bool on) {
        auto& s = AppSettings::instance();
        s.setValue("ShowDemoRadio", on ? "True" : "False");
        s.save();
        if (on) addDemoRadio();
        else    removeDemoRadio();
    });
    preferencesLayout->addWidget(m_showDemoCheck);
    auto* preferencesToggle = new QToolButton(this);
    preferencesToggle->setText(tr("Startup and connection preferences"));
    preferencesToggle->setAccessibleName(tr("Startup and connection preferences"));
    preferencesToggle->setObjectName(QStringLiteral("connectionPreferencesToggle"));
    preferences->setObjectName(QStringLiteral("connectionPreferencesBody"));
    preferencesToggle->setCheckable(true);
    preferencesToggle->setArrowType(Qt::RightArrow);
    preferencesToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    root->addWidget(preferencesToggle);
    root->addWidget(preferences);
    preferences->hide();
    connect(preferencesToggle, &QToolButton::toggled, preferences, [this, preferences, preferencesToggle](bool checked) {
        preferences->setVisible(checked);
        preferencesToggle->setArrowType(checked ? Qt::DownArrow : Qt::RightArrow);
        refitToContent();
        if (checked && isVisible()) {
            QTimer::singleShot(0, this, [this, preferences] {
                resize(width(), qMax(height(), preferredClientHeight(width())));
                fitAndClampToScreen();
                m_bodyScroll->ensureWidgetVisible(preferences);
            });
        }
    });

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
    // Editing the route hands control back to the operator, including while
    // the asynchronous startup credential read is still pending.
    connect(m_manualIpEdit, &QLineEdit::textEdited, this, [this] {
        m_startupProbe = false;
    });
    connect(m_manualRadioTypeCombo, &QComboBox::activated, this, [this] {
        m_startupProbe = false;
    });
    // The address was CHOSEN, not typed — restore its remembered radio type.
    // `activated` fires only for a pick out of the popup, which is exactly the
    // gesture the per-address family restore was written for; the keystroke
    // path deliberately does not carry it (see applySavedSourceSelection).
    connect(m_manualIpCombo, &QComboBox::activated, this, [this](int) {
        m_startupProbe = false;
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
    connect(m_manualSourceCombo, &QComboBox::activated, this, [this](int) {
        m_manualSourcePick = currentManualBindSettings();
    });

    const auto doLogin = [this] {
        const QString email = m_emailEdit->text().trimmed();
        const QString pass = m_passwordEdit->text();
        if (email.isEmpty() || pass.isEmpty())
            return;
        if (!prepareWorkspaceLogin(email)) { return; }
        m_loginBtn->setEnabled(false);
        m_loginBtn->setText("Signing In...");
        emit smartLinkLoginRequested(email, pass);
    };
    connect(m_loginBtn, &QPushButton::clicked, this, doLogin);
    connect(m_passwordEdit, &QLineEdit::returnPressed, this, doLogin);
    connect(m_logoutBtn, &QPushButton::clicked, this, [this] {
        if (!m_smartLink)
            return;
        if (m_smartLink->accountLocked()) {
            setStatusText(tr("Disconnect the SmartLink radio before signing out.")); return;
        }
        m_smartLink->logout();
        m_pendingSmartLinkConnection = {};
        m_wanRadios.clear();
        m_wanList->clear();
        setAccountLine(QStringLiteral("Signed out of SmartLink."), AccountTone::Hint);
        updateSmartLinkUi();
        emit smartLinkSignedOut();
    });

    // Settle the body layout so preferredClientHeight() has a real answer the
    // first time fitToScreen() asks for one. The height floor this used to set
    // is gone: the manual page is still the tallest of the three, but overflow
    // now belongs to the scroll area rather than to the top-level minimum.
    refitToContent();
    setMinimumSize(kSafeMinimumWidth, kSafeMinimumHeight);
    resize(kPreferredWidth, kPreferredHeight);

    buildWorkspace(root);
    setConnected(false);
    setCurrentMode(WorkspaceMode);
    updateLocalPageState();
    updateSmartLinkUi();
    updateManualAdvancedVisibility();
    FramelessResizer::install(this);
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
    return CanonWindow::event(e);
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

int ConnectionPanel::preferredClientHeight(int clientWidth) const
{
    if (!m_bodyContent || !m_bodyScroll) {
        return kPreferredHeight;
    }
    // Everything in `outer` except the scroll area — the title bar when it is
    // shown, the fixed footer always. Taking it as a difference rather than
    // summing the two widgets keeps this correct without duplicating which of
    // them is visible in which frameless mode.
    const int chromeHeight = sizeHint().height() - m_bodyScroll->sizeHint().height();
    int bodyHeight = chromeHeight + m_bodyContent->sizeHint().height();

    // A body wider than the viewport shows a horizontal scrollbar, and that bar
    // takes its height out of the viewport. Left out, the fit comes up exactly
    // one bar short and the dialog opens with a vertical scrollbar anyway
    // (#5811). maximumViewportSize() is the viewport with no scrollbars, which
    // is the state this fit is aiming for; the horizontal chrome around it
    // carries over unchanged to the width the caller is about to apply.
    const int viewportWidth = clientWidth
        - (width() - m_bodyScroll->maximumViewportSize().width());
    const int contentMinWidth = m_bodyContent->minimumSizeHint()
                                    .expandedTo(m_bodyContent->minimumSize())
                                    .width();
    if (contentMinWidth > viewportWidth) {
        bodyHeight += m_bodyScroll->horizontalScrollBar()->sizeHint().height();
    }
    return qMax(kPreferredHeight, bodyHeight);
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
    const int targetWidth = qMin(width(), maximumClientWidth);
    int targetHeight = qMin(height(), maximumClientHeight);
    if (height() == m_autoFitHeight) {
        targetHeight = qBound(qMin(kSafeMinimumHeight, maximumClientHeight),
                              preferredClientHeight(targetWidth),
                              maximumClientHeight);
    }
    resize(targetWidth, targetHeight);
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

// A CanonWindow is always frameless, so this is whatever the platform reports
// once the window exists (nothing before), not a title-bar estimate.
QMargins ConnectionPanel::screenFitFrameMargins() const
{
    return windowHandle() ? windowHandle()->frameMargins() : QMargins();
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
    m_pendingIcomUsername.clear();
    m_pendingIcomHost.clear();
    m_pendingIcomResolvedHost.clear();
    m_pendingIcomBasePort = 0;
    m_pendingIcomBindSettings = RadioBindSettings{};
    m_pendingIcomSessionBindAddress.clear();
}

void ConnectionPanel::setConnected(bool connected)
{
    // Credentials staged by probeRadio() are persisted only here, so a wrong
    // password never overwrites a working one. Scoped to the staged family: a
    // failed Icom attempt followed by a Flex connect must not write the unproven
    // Icom password.
    if (connected && !m_pendingIcomPassword.isEmpty()
        && currentManualFamily() == QLatin1String(kFamilyIcom)) {
        IcomCredentials::save(m_pendingIcomPassword);
        const QString credentialId = ConnectionWorkspace::connectionCredentialId(
            m_pendingIcomHost + QLatin1Char(':') + QString::number(m_pendingIcomBasePort), m_pendingIcomUsername);
        IcomCredentials::saveForConnection(credentialId, m_pendingIcomPassword);
        m_activeIcomCredentialId = credentialId;
        if (!m_pendingIcomHost.isEmpty()) {
            IcomSettings::setLastHost(m_pendingIcomHost);
            // The routed profile restores the family and source path for this
            // address at the next launch. Icom cannot be probed anonymously,
            // so commit it only after the authenticated session proves the
            // host and credentials together.
            saveManualProfile(m_pendingIcomHost,
                              m_pendingIcomBindSettings,
                              m_pendingIcomSessionBindAddress,
                              m_pendingIcomBasePort);
            if (m_pendingIcomResolvedHost != m_pendingIcomHost) {
                // MainWindow retains the resolved address in LastRoutedRadioIp.
                // Mirror the profile under that key as well so a hostname such
                // as ic-705.local still restores the Icom family at startup.
                saveManualProfile(m_pendingIcomResolvedHost,
                                  m_pendingIcomBindSettings,
                                  m_pendingIcomSessionBindAddress,
                                  m_pendingIcomBasePort, false);
            }
        }
    }
    if (!connected || !m_pendingIcomPassword.isEmpty()) {
        // Cleared on BOTH edges: a failed attempt must not commit on the next
        // unrelated connect, and a committed one must not commit twice.
        clearPendingIcomCredentials();
    }

    if (connected) {
        m_startupProbe = false;
    }

    m_connected = connected;
    m_disconnectBtn->setVisible(connected);
    updateActionState();
    refreshWorkspaceDetails();
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

bool ConnectionPanel::selectRadio(const QString& serial)
{
    for (int index = 0; index < m_radios.size(); ++index) {
        if (m_radios[index].serial == serial) {
            setCurrentMode(LocalMode);
            m_radioList->setCurrentRow(index);
            return true;
        }
    }
    for (int index = 0; index < m_wanRadios.size(); ++index) {
        if (m_wanRadios[index].serial == serial) {
            setCurrentMode(SmartLinkMode);
            m_wanList->setCurrentRow(index);
            return true;
        }
    }
    return false;
}

void ConnectionPanel::selectManualConnection()
{
    setCurrentMode(ManualMode);
}

bool ConnectionPanel::canRenameRadio(const QString& serial) const
{
    for (const RadioInfo& radio : m_radios) {
        if (radio.serial == serial) {
            return canRenameRadio(radio);
        }
    }
    return false;
}

bool ConnectionPanel::canRenameRadio(const RadioInfo& radio) const
{
    // The demo's name IS its safety label ("not on the air"); it is not the
    // operator's to replace.
    if (radio.family == SimBackend::familyName()) {
        return false;
    }
    return !radio.serial.isEmpty() && !hl2::Hl2Discovery::nicknameLivesOnRadio(radio);
}

QString ConnectionPanel::radioDisplayName(const RadioInfo& radio, const QString& fallback) const
{
    return canRenameRadio(radio)
        ? hl2::Hl2Discovery::effectiveNickname(radio.family, radio.serial, fallback)
        : fallback;
}

void ConnectionPanel::renameRadio(const QString& serial)
{
    for (const RadioInfo& radio : m_radios) {
        if (radio.serial == serial) {
            renameRadio(radio);
            return;
        }
    }
}

void ConnectionPanel::renameRadio(const RadioInfo& radio)
{
    if (!canRenameRadio(radio)) {
        return;
    }
    // The dialog itself is the owner's to build (MainWindow, as a
    // PersistentDialog).  Keeping it out of this class keeps ConnectionPanel
    // free of the frameless-dialog stack, which the startup/auto-connect
    // tests link without.
    emit radioRenameRequested(radio, hl2::Hl2Discovery::effectiveNickname(
                                         radio.family, radio.serial, QString()));
}

void ConnectionPanel::setRadioNickname(const RadioInfo& radio, const QString& nickname)
{
    hl2::Hl2Discovery::setNickname(radio.family, radio.serial, nickname.trimmed());
    for (int current = 0; current < m_radios.size(); ++current) {
        if (m_radios[current].serial == radio.serial) {
            m_radios[current].nickname = hl2::Hl2Discovery::effectiveNickname(
                radio.family, radio.serial, radio.model);
            if (QListWidgetItem* item = m_radioList->item(current)) {
                item->setText(formatLocalRadioLabel(m_radios[current]));
            }
            break;
        }
    }
    emit radioNicknameChanged();
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
        && wantedFamily != QLatin1String(kFamilyAnan)
        && wantedFamily != QLatin1String(kFamilyIcom)) {
        setAutomationError(
            error,
            QStringLiteral("unknown radio family '%1' (use flex, hl2, anan or icom)")
                .arg(family.trimmed()));
        return false;
    }

    m_pendingWorkspaceConnection.clear();
    m_pendingWorkspaceStation.clear();
    m_manualConnectionName->clear();
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
    applyStyle(m_manualResultLabel, QString::fromLatin1(error ? kErrorLabelStyle : kInfoLabelStyle));
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
    refreshWorkspace();
}

void ConnectionPanel::setAccountLine(const QString& text, AccountTone tone)
{
    m_slUserTone = tone;
    m_slUserLabel->setText(text);
    const char* style = tone == AccountTone::Info  ? kInfoLabelStyle
                      : tone == AccountTone::Error ? kErrorLabelStyle
                                                   : kHintLabelStyle;
    applyStyle(m_slUserLabel, QString::fromLatin1(style));
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
        if (m_slUserLabel->text().trimmed().isEmpty() || m_slUserTone == AccountTone::Hint)
            setAccountLine(smartLinkUserText(m_smartLink), AccountTone::Info);
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
            setAccountLine(QStringLiteral("Sign in to see radios at remote stations."), AccountTone::Hint);
        m_smartLinkEmptyLabel->setText("Remote radios appear here after SmartLink sign-in.");
        m_smartLinkEmptyLabel->setVisible(true);
    }

    if (hasWanRadios && !m_wanList->currentItem())
        m_wanList->setCurrentRow(0);

    updateActionState();
    refreshWorkspace();
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
    const auto family = NetworkPathResolver::targetFamily(
        m_manualIpEdit ? m_manualIpEdit->text() : QString());
    const auto candidates = NetworkPathResolver::enumerateCandidates(family);
    // One IPv6 adapter normally has a GUA and a ULA; that is not a choice of
    // path, so IPv6 counts adapters. IPv4 keeps counting addresses.
    qsizetype paths = candidates.size();
    if (family == QAbstractSocket::IPv6Protocol) {
        QSet<QString> interfaces;
        for (const auto& candidate : candidates)
            interfaces.insert(candidate.interfaceId);
        paths = interfaces.size();
    }
    const RadioBindSettings settings = currentManualBindSettings();
    const bool hasExplicitSelection = settings.mode == RadioBindMode::Explicit;
    const bool showToggle = paths > 1
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
    for (int i = 0; i < m_modeStack->count(); ++i) {
        m_modeStack->widget(i)->setSizePolicy(i == static_cast<int>(mode) ? QSizePolicy::Preferred : QSizePolicy::Ignored,
            i == static_cast<int>(mode) ? QSizePolicy::Expanding : QSizePolicy::Ignored);
    }
    m_modeStack->updateGeometry();
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
    // The demo's name is its "not on the air" safety label (canRenameRadio).
    if (radio.family == SimBackend::familyName())
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
        renameRadio(radio.serial);
        return;
    } else if (clearNick && chosen == clearNick) {
        hl2::Hl2Discovery::setNickname(radio.family, radio.serial, QString());
    }

    // Reflect the change immediately: re-label this row from the saved setting
    // rather than waiting for the next discovery sweep.
    // Discovery may remove or reorder rows while either nested loop runs.
    // Resolve the radio again instead of retaining a QListWidgetItem pointer.
    for (int i = 0; i < m_radios.size(); ++i) {
        RadioInfo& updated = m_radios[i];
        if (updated.family != radio.family || updated.serial != radio.serial) {
            continue;
        }
        updated.nickname = hl2::Hl2Discovery::effectiveNickname(
            updated.family, updated.serial, updated.model);
        if (QListWidgetItem* currentItem = m_radioList->item(i)) {
            currentItem->setText(formatLocalRadioLabel(updated));
        }
        break;
    }
    emit radioNicknameChanged();
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
            refreshWorkspace();
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
    m_pendingSmartLinkConnection = {};
    m_pendingWorkspaceStation.clear();
    const int row = m_radioList->currentRow();
    if (m_connected || row < 0 || row >= m_radios.size())
        return;
    if (!applyWorkspaceOperator()) { return; }

    const RadioInfo& info = m_radios[row];
    // F5 (#4448): an in-use radio of a single-client family can't be shared,
    // and connecting would wedge both clients. Fail closed. The rule lives in
    // ConnectionSharingPolicy.h — one home, shared with the startup
    // auto-connect gate in MainWindow_Session.
    if (info.inUse && !AetherSDR::familySupportsSharedInUseConnect(info.family)) {
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
    rememberWorkspaceRadio(info, "local");
    emit connectRequested(info);
}

void ConnectionPanel::onWanConnectClicked()
{
    const int row = m_wanList->currentRow();
    if (m_connected || row < 0 || row >= m_wanRadios.size())
        return;
    if (!applyWorkspaceOperator()) { return; }

    saveLowBandwidthPreference(m_lowBwCheck->isChecked());
    rememberWorkspaceWan(m_wanRadios[row]);
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
        setAccountLine(smartLinkUserText(m_smartLink), AccountTone::Info);
        updateSmartLinkUi();
    });

    connect(client, &SmartLinkClient::serverConnected, this, [this] {
        QTimer::singleShot(500, this, [this] {
            if (m_smartLink && m_smartLink->isAuthenticated()) {
                setAccountLine(smartLinkUserText(m_smartLink), AccountTone::Info);
                updateSmartLinkUi();
            }
        });
    });

    connect(client, &SmartLinkClient::serverDisconnected, this, [this] {
        if (!m_smartLink || !m_smartLink->isAuthenticated()) {
            if (m_slUserLabel->text().trimmed().isEmpty())
                setAccountLine(QStringLiteral("Sign in to see radios at remote stations."), AccountTone::Hint);
        }
        updateSmartLinkUi();
    });

    connect(client, &SmartLinkClient::authFailed, this, [this](const QString& err) {
        m_pendingSmartLinkConnection = {};
        m_passwordEdit->clear();
        m_loginBtn->setText("Sign In");
        m_loginBtn->setEnabled(true);
        setAccountLine(QStringLiteral("SmartLink sign-in failed: ") + err, AccountTone::Error);
        updateSmartLinkUi();
    });

    connect(client, &SmartLinkClient::radioListReceived, this,
            [this](const QList<WanRadioInfo>& radios) {
        m_wanRadios.clear();
        m_wanList->clear();

        for (const auto& radio : radios) {
            m_wanRadios.append(radio);
            m_wanList->addItem(formatWanRadioLabel(radio));
        }

        if (m_wanList->count() > 0 && !m_wanList->currentItem())
            m_wanList->setCurrentRow(0);

        updateSmartLinkUi();
        resumeWorkspaceSmartLink();
    });

    client->setAccountId(m_workspace.value("selectedAccount").toString("legacy"));
    const QJsonObject account = ConnectionWorkspace::find(m_workspace.value("accounts").toArray(), client->accountId());
    if (!account.value("email").toString().isEmpty()) { m_emailEdit->setText(account.value("email").toString()); }
    client->tryAutoLogin();
    updateSmartLinkUi();
}

void ConnectionPanel::refreshManualSourceOptions(const RadioBindSettings* selected)
{
    const QSignalBlocker blocker(m_manualSourceCombo);
    m_manualSourceCombo->clear();

    m_manualSourceCombo->addItem("Auto");
    m_manualSourceCombo->setItemData(0, static_cast<int>(RadioBindMode::Auto), kSourceModeRole);
    m_manualSourceCombo->setItemData(0, false, kSourceStaleRole);

    int selectedIndex = 0;
    const auto family = NetworkPathResolver::targetFamily(m_manualIpEdit ? m_manualIpEdit->text() : QString());
    const auto candidates = m_manualSourceCandidates ? m_manualSourceCandidates(family)
        : NetworkPathResolver::enumerateCandidates(family);
    for (const auto& candidate : candidates) {
        const int idx = m_manualSourceCombo->count();
        m_manualSourceCombo->addItem(candidate.label());
        m_manualSourceCombo->setItemData(idx, static_cast<int>(RadioBindMode::Explicit), kSourceModeRole);
        m_manualSourceCombo->setItemData(idx, candidate.interfaceId, kSourceInterfaceIdRole);
        m_manualSourceCombo->setItemData(idx, candidate.interfaceName, kSourceInterfaceNameRole);
        m_manualSourceCombo->setItemData(idx, candidate.address.toString(), kSourceAddressRole);
        m_manualSourceCombo->setItemData(idx, false, kSourceStaleRole);
    }
    if (selected && selected->mode == RadioBindMode::Explicit) {
        const qsizetype match = NetworkPathResolver::matchSelection(
            candidates, selected->interfaceId, selected->interfaceName, selected->bindAddress,
            QHostAddress(m_manualIpEdit ? m_manualIpEdit->text().trimmed() : QString()));
        if (match >= 0)
            selectedIndex = 1 + static_cast<int>(match);   // row 0 is Auto
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

    // The external port triplet belongs to the routed endpoint, not to the
    // physical Icom model. Restore it only when the operator chose a saved
    // address (or startup restored one), never while an editable address is
    // being typed character by character.
    if (restoreFamily
        && familyFromProfile(profile) == QLatin1String(kFamilyIcom)
        && m_manualIcomPortCombo && m_manualIcomBasePortSpin) {
        quint16 basePort = IcomSettings::defaultBasePort();
        const bool hasSavedBasePort = icomBasePortFromProfile(profile, &basePort);
        const bool standard = !hasSavedBasePort
            || basePort == IcomSettings::defaultBasePort();
        m_manualIcomPortCombo->setCurrentIndex(standard ? 0 : 1);
        m_manualIcomBasePortSpin->setValue(
            standard ? IcomSettings::defaultBasePort() : basePort);
        syncIcomPortCustomRow();
    }

    RadioBindSettings settings = bindSettingsFromProfile(profile);
    if (settings.mode == RadioBindMode::Explicit) {
        const auto resolved = NetworkPathResolver::resolveExplicitSelection(
            settings.interfaceId, settings.interfaceName, settings.bindAddress,
            NetworkPathResolver::targetFamily(trimmedIp), QHostAddress(trimmedIp));
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
    if (restoreFamily)
        m_manualSourcePick = settings;   // a chosen address brings its own path
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
      : lowered == QLatin1String(kFamilyAnan) ? QString::fromLatin1(kFamilyAnan)
      : lowered == QLatin1String(kFamilyIcom) ? QString::fromLatin1(kFamilyIcom)
      : lowered == QLatin1String(kFamilyRtl)  ? QString::fromLatin1(kFamilyRtl)
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
        // Only radios this page can dial. hasNetwork == false means CI-V only
        // (serial or an RS-BA1 server front end); those stay reachable via
        // `Custom...`, where name auto-detect cannot help because the handshake
        // names the server, not the radio.
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

void ConnectionPanel::syncIcomPortCustomRow()
{
    if (!m_manualIcomPortCombo || !m_manualIcomPortCustomRow) {
        return;
    }
    const bool icom = currentManualFamily() == QLatin1String(kFamilyIcom);
    const bool custom =
        m_manualIcomPortCombo->currentData().toString() == QLatin1String("__custom__");
    m_manualIcomPortCustomRow->setVisible(icom && custom);
}

quint16 ConnectionPanel::selectedIcomBasePort() const
{
    if (m_manualIcomPortCombo && m_manualIcomBasePortSpin
        && m_manualIcomPortCombo->currentData().toString()
               == QLatin1String("__custom__")) {
        return static_cast<quint16>(m_manualIcomBasePortSpin->value());
    }
    return IcomSettings::defaultBasePort();
}

void ConnectionPanel::updateManualFamilyHints()
{
    const QString family = currentManualFamily();
    const bool hl2  = family == QLatin1String(kFamilyHl2);
    const bool anan = family == QLatin1String(kFamilyAnan);
    const bool icom = family == QLatin1String(kFamilyIcom);

    // The credentials and network selectors belong to Icom alone. Hiding the
    // row CONTAINERS rather than the fields keeps their labels from being left
    // behind.
    if (m_manualIcomUserRow)
        m_manualIcomUserRow->setVisible(icom);
    if (m_manualIcomPassRow)
        m_manualIcomPassRow->setVisible(icom);
    if (m_manualIcomPortRow) {
        m_manualIcomPortRow->setVisible(icom);
    }
    if (m_manualIcomCivRow)
        m_manualIcomCivRow->setVisible(icom);
    // The hex row has a second condition — "Custom..." — so it gets the shared
    // helper rather than a copy of the visibility rule.
    syncIcomCivCustomRow();
    syncIcomPortCustomRow();

    if (m_manualAnanHintRow)
        m_manualAnanHintRow->setVisible(anan);

    if (icom) {
        // Fill from settings, and read the password out of the keychain — which
        // is asynchronous, so the field populates a beat later. Guarded by the
        // widget pointer inside the callback because the panel can be closed
        // between the request and the answer.
        if (m_manualIcomUserEdit && m_manualIcomUserEdit->text().isEmpty())
            m_manualIcomUserEdit->setText(IcomSettings::username());
        // Rebuilt from settings so changes made elsewhere show and so the
        // selection decides the hex row's visibility — except while the Custom
        // hex field is open with text in it (reached via setManualFamily() from
        // applySavedSourceSelection()), which would discard the operator's entry.
        // Same rule as the isEmpty() guards on the fields below.
        const bool customEntryInFlight =
            m_manualIcomCivCombo
            && m_manualIcomCivCombo->currentData().toString()
                   == QLatin1String("__custom__")
            && m_manualIcomCivEdit && !m_manualIcomCivEdit->text().isEmpty();
        if (!customEntryInFlight)
            populateIcomCivCombo();
        if (m_manualIpEdit && m_manualIpEdit->text().isEmpty())
            m_manualIpEdit->setText(IcomSettings::lastHost());
        if (m_manualIcomPassEdit && m_manualIcomPassEdit->text().isEmpty()
            && (!m_activeIcomCredentialId.isEmpty() || m_allowLegacyIcomCredential)) {
            const QString credentialId = m_activeIcomCredentialId;
            const QString host = m_manualIpEdit->text().trimmed();
            const bool legacy = m_allowLegacyIcomCredential;
            IcomCredentials::loadForConnection(credentialId, this, [this, credentialId, host, legacy](const QString& password) {
                if (m_activeIcomCredentialId == credentialId && m_allowLegacyIcomCredential == legacy
                    && m_manualIpEdit->text().trimmed() == host && m_manualIcomPassEdit->text().isEmpty()) {
                    m_manualIcomPassEdit->setText(password);
                }
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
                      "configured for network control. Standard UDP ports are used unless "
                      "you choose a custom three-port NAT range. %1")
                      .arg(passwordStorageHint)
                : hl2
                ? QStringLiteral(
                      "Use this path when discovery broadcasts cannot reach the radio — a VPN, a "
                      "routed subnet, or a switch that drops broadcasts. AetherSDR sends a "
                      "Hermes-Lite 2 discovery request straight to the address you enter.")
                : anan
                ? QStringLiteral(
                      "Use this path when discovery broadcasts cannot reach the radio — a VPN, a "
                      "routed subnet, or a switch that drops broadcasts. AetherSDR sends an "
                      "openHPSDR Protocol 2 discovery request straight to the address you enter.")
                : QStringLiteral(
                      "Use this path for VPN or other routed networks where discovery broadcasts "
                      "cannot reach the radio. Enter the radio IP address and AetherSDR will take "
                      "care of the probe."));
    }
    if (m_manualIpEdit) {
        m_manualIpEdit->setPlaceholderText(
            icom ? QStringLiteral("Example: radio.local or 192.168.1.90")
          : hl2  ? QStringLiteral("Example: 192.168.1.21")
          : anan ? QStringLiteral("Example: 172.16.10.14")
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
                                        const QHostAddress& lastSuccessfulLocalIp,
                                       quint16 icomBasePort, bool rememberWorkspace)
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

    if (currentManualFamily() == QLatin1String(kFamilyIcom)) {
        QJsonObject icom;
        const quint16 basePort = icomBasePort != 0
            ? icomBasePort
            : selectedIcomBasePort();
        icom["base_port"] = basePort;
        profile["icom"] = icom;
        profile["schema_version"] = 2;
    }

    profiles[targetIp] = profile;
    saveRoutedProfiles(profiles);
    if (rememberWorkspace) { rememberWorkspaceManual(targetIp, profile); }
}

void ConnectionPanel::onManualIpChanged(const QString& ip)
{
    const QString trimmed = ip.trimmed();
    m_manualConnectPending = false;
    if (trimmed != m_manualProfileIp) {
        // Typing an address must not discard a source path the operator has
        // already selected. The saved profile is useful for startup and for
        // choosing a recent address, but it must not silently turn an
        // explicit interface back into Auto during an interactive edit.
        const RadioBindSettings selected = m_manualSourcePick.mode == RadioBindMode::Explicit
            ? m_manualSourcePick : currentManualBindSettings();
        applySavedSourceSelection(trimmed, /*restoreFamily=*/false);
        if (selected.mode == RadioBindMode::Explicit)
            refreshManualSourceOptions(&selected);
    }
    setManualMessage(QString());
    updateActionState();
}

void ConnectionPanel::onManualConnectClicked()
{
    m_pendingSmartLinkConnection = {};
    const QString ip = m_manualIpEdit->text().trimmed();
    if (m_connected || ip.isEmpty())
        return;
    if (!applyWorkspaceOperator()) { return; }

    m_pendingWorkspaceStation = m_workspace.value("selectedStation").toString();
    m_manualConnectPending = true;
    m_startupProbe = false;
    setManualMessage(QStringLiteral("Checking %1…").arg(ip));
    probeRadio(ip);
}

void ConnectionPanel::onManualAdvancedToggled(bool checked)
{
    m_manualAdvancedToggle->setArrowType(checked ? Qt::DownArrow : Qt::RightArrow);
    m_manualAdvancedWidget->setVisible(checked);
}

void ConnectionPanel::reportStartupProbeFailure(const QString& reason)
{
    // A startup probe is the one probe with nobody reading the manual page.
    // MainWindow has covered the window with "Looking for your radio…", and it
    // suppressed the no-saved-radio dialog popup precisely BECAUSE a radio is
    // saved — so setManualMessage() alone writes the reason onto a page behind a
    // dialog that will never open. The operator is left with a spinner and no
    // route back to the connection UI. Hand the reason up instead.
    if (!m_startupProbe) {
        return;
    }
    m_startupProbe = false;
    // Land the operator on the page that explains the failure: the full
    // guidance setManualMessage() wrote lives on the Connect by IP page, and
    // the footer line MainWindow sets carries only the short reason.
    setCurrentMode(ManualMode);
    emit startupConnectUnavailable(reason);
}

void ConnectionPanel::probeRadio(const QString& ip, bool restoreSavedFamily)
{
    const QString trimmedIp = ip.trimmed();
    if (trimmedIp.isEmpty())
        return;

    // Latched, not assigned: the Icom keychain read below re-enters probeRadio()
    // without restoreSavedFamily, and that second pass is still the same startup
    // attempt. Cleared on failure, probe dispatch, a proven connection, or
    // an operator route edit/manual connect.
    if (restoreSavedFamily) {
        m_startupProbe = true;
        if (!applyWorkspaceOperator()) {
            reportStartupProbeFailure(tr("The saved operator profile needs attention."));
            return;
        }
    }

    // Interactive and automation probes keep the family currently selected by
    // the operator. Startup is the exception: it has no current operator
    // choice, so restoreSavedFamily asks the retained route which wire protocol
    // belongs to the saved address.
    if (m_manualIpEdit->text().trimmed() != trimmedIp) {
        m_manualIpEdit->setText(trimmedIp);
        if (restoreSavedFamily)
            applySavedSourceSelection(trimmedIp, /*restoreFamily=*/true);
    } else if (restoreSavedFamily) {
        // Interactive connects keep the currently selected source path. Only
        // startup and an explicit recent-address selection restore profiles.
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
        reportStartupProbeFailure(
            QStringLiteral("The saved source path for this radio is unavailable."));
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
        if (pass.isEmpty() && m_activeIcomCredentialId.isEmpty() && m_allowLegacyIcomCredential)
            pass = IcomCredentials::sessionPassword();
        if (user.isEmpty()) {
            resetManualConnectButton();
            setManualMessage(
                QStringLiteral("An Icom needs the network user name and password set on the "
                               "radio. Check Network Control is ON in the radio's menu, then "
                               "enter the same credentials here."),
                true);
            reportStartupProbeFailure(
                QStringLiteral("This Icom needs its network user name and password."));
            return;
        }
        if (pass.isEmpty()) {
            if (m_activeIcomCredentialId.isEmpty() && !m_allowLegacyIcomCredential) {
                resetManualConnectButton();
                setManualMessage(tr("Enter this radio's password. It will be saved after a successful connection."), true);
                reportStartupProbeFailure(tr("This Icom needs its own saved password."));
                return;
            }
            // Keychain reads are asynchronous. Startup auto-connect reaches
            // this path on a fixed timer, so relying on the dialog's earlier
            // best-effort read races a locked or merely slow macOS Keychain.
            // Finish the read here and resume the same family-selected probe;
            // the synchronous connect path itself remains keychain-free.
            const QString requestedHost = trimmedIp;
            setManualMessage(QStringLiteral("Loading the saved Icom password…"));
            QPointer<ConnectionPanel> panel(this);
            const QString credentialId = m_activeIcomCredentialId;
            IcomCredentials::loadForConnection(credentialId, this, [panel, requestedHost, credentialId](const QString& password) {
                if (!panel) {
                    return;
                }
                if (panel->m_activeIcomCredentialId != credentialId
                    || panel->currentManualFamily() != QLatin1String(kFamilyIcom)
                    || !panel->m_manualIpEdit
                    || panel->m_manualIpEdit->text().trimmed() != requestedHost) {
                    panel->resetManualConnectButton();
                    panel->m_manualConnectPending = false;
                    // An operator edit has normally cleared the latch already,
                    // making this a no-op; if the route changed under the read
                    // any other way, the startup attempt still ends here and
                    // must not leave the overlay with no owner.
                    panel->reportStartupProbeFailure(
                        QStringLiteral("The saved Icom route changed before its password loaded."));
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
                    panel->reportStartupProbeFailure(
                        QStringLiteral("No saved password for this Icom."));
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
        // Hex, with or without 0x or a trailing h (the radio menu shows "A2h").
        // Unparseable or out-of-range input is ignored, not clamped: a wrong
        // CI-V address fails silently, so guessing would hide the typo. Branch on
        // currentData(), the raw value each item was built with.
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
                        const QString civError =
                            QStringLiteral("CI-V address \"%1\" is not a hex byte "
                                           "(try A2, 0xA2 or A2h) — not connecting.")
                                .arg(m_manualIcomCivEdit->text().trimmed());
                        setStatusText(civError);
                        m_manualIcomCivEdit->setFocus();
                        m_manualIcomCivEdit->selectAll();
                        reportStartupProbeFailure(civError);
                        return;
                    }
                }
            }
        }

        // One operator value describes the complete RS-BA1 forwarding rule.
        // The radio transport always opens control, CI-V and audio as three
        // independent UDP streams, in that order. Bound the base in the widget
        // and again in IcomSettings so base+2 can never wrap past 65535.
        const quint16 basePort = selectedIcomBasePort();
        IcomSettings::setBasePort(basePort);

        // Do not setLastHost or save the credential until the radio accepts us,
        // so a mistyped password never replaces a working keychain entry. The
        // port choice is kept now so retries and reconnects use the same
        // triplet. The session cache is primed now because connect reads it
        // synchronously; onIcomConnectSucceeded() writes the durable copy (see
        // setConnected()).
        IcomCredentials::setSessionPassword(pass);
        m_pendingIcomHost = trimmedIp;
        m_pendingIcomUsername = user;
        m_pendingIcomPassword = pass;
        m_pendingIcomBasePort = basePort;
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
                reportStartupProbeFailure(
                    QStringLiteral("Could not resolve \"%1\".").arg(trimmedIp));
                return;
            }
            resolved = hostInfo.addresses().first();
        }
        m_pendingIcomResolvedHost = resolved.toString();

        RadioInfo info;
        info.family   = QString::fromLatin1(kFamilyIcom);
        info.address  = resolved;
        info.port     = basePort;
        info.model    = QStringLiteral("Icom");
        info.name     = info.model;
        // No discovery means no MAC and no reported serial, so the host is the
        // only stable identity this radio has for us. It has to be SOMETHING:
        // the restore/persist scope keys off it.
        info.serial   = basePort == IcomSettings::defaultBasePort()
            ? QStringLiteral("icom:%1").arg(resolved.toString())
            : QStringLiteral("icom:%1:%2").arg(resolved.toString()).arg(basePort);
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
        finishManualProbe(info);
        return;
    }

    if (currentManualFamily() == QLatin1String(kFamilyHl2)) {
        // Only a genuine timeout earns the "check the radio" message. A bind
        // failure has already reported itself, and pointing the operator at the
        // radio would be actively wrong. (PR #4528 review.)
        const Hl2ProbeResult probe = probeHermesLite2(trimmedIp, bindSettings);
        handleHl2ProbeResult(probe, trimmedIp);
        return;
    }

    if (currentManualFamily() == QLatin1String(kFamilyAnan)) {
        const AnanProbeResult probe = probeAnan(trimmedIp, bindSettings);
        if (probe == AnanProbeResult::NoAnswer) {
            resetManualConnectButton();
            setManualMessage(
                QStringLiteral("No ANAN-G2 answered at %1. Check the address, and that the "
                               "radio is powered, idle, and reachable on UDP port 1024.")
                    .arg(trimmedIp),
                true);
            reportStartupProbeFailure(
                QStringLiteral("No ANAN-G2 answered at %1.").arg(trimmedIp));
        } else if (probe == AnanProbeResult::NotAttempted) {
            resetManualConnectButton();
        }
        return;
    }

    RadioBindSettings flexBind = bindSettings;
    if (!resolveNamedSourceFamily(trimmedIp, &flexBind))
        return;
    probeFlexRadio(trimmedIp, flexBind);
}

bool ConnectionPanel::resolveNamedSourceFamily(const QString& host, RadioBindSettings* bind)
{
    // The source list follows the typed text, so a host name lists IPv4
    // paths. A name with only IPv6 addresses reaches the radio over IPv6:
    // use the chosen interface's IPv6 address instead of failing on family.
    if (bind->mode != RadioBindMode::Explicit || !QHostAddress(host).isNull())
        return true;
    const QHostInfo info = QHostInfo::fromName(host);
    QHostAddress v6;
    for (const QHostAddress& address : info.addresses()) {
        bool isV4 = false;
        address.toIPv4Address(&isV4);
        if (isV4)
            return true;   // reachable over IPv4: the listed path applies
        if (v6.isNull() && address.protocol() == QAbstractSocket::IPv6Protocol)
            v6 = address;
    }
    if (v6.isNull())
        return true;   // unresolved: the probe reports it
    const NetworkPathCandidate path = NetworkPathResolver::resolveExplicitSelection(
        bind->interfaceId, bind->interfaceName, bind->bindAddress,
        QAbstractSocket::IPv6Protocol, v6);
    if (!path.isValid()) {
        resetManualConnectButton();
        setManualMessage(
            QStringLiteral("“%1” has only IPv6 addresses, and the selected source path has "
                           "no IPv6 address. Choose Auto or another path.").arg(host),
            true);
        reportStartupProbeFailure(
            QStringLiteral("The source path for “%1” has no IPv6 address.").arg(host));
        return false;
    }
    bind->interfaceId = path.interfaceId;
    bind->interfaceName = path.interfaceName;
    bind->bindAddress = path.address;
    return true;
}

void ConnectionPanel::handleHl2ProbeResult(Hl2ProbeResult probe, const QString& trimmedIp)
{
    if (probe == Hl2ProbeResult::NoAnswer) {
        resetManualConnectButton();
        setManualMessage(
            QStringLiteral("No Hermes-Lite 2 answered at %1. Check the address, and that the "
                           "radio is powered, idle, and reachable on UDP port 1024.")
                .arg(trimmedIp),
            true);
        reportStartupProbeFailure(
            QStringLiteral("No Hermes-Lite 2 answered at %1.").arg(trimmedIp));
    } else if (probe == Hl2ProbeResult::NotAttempted) {
        // probeHermesLite2() has already reported its own reason, upward
        // included; only the button needs restoring here.
        resetManualConnectButton();
    }
}

void ConnectionPanel::finishManualProbe(const RadioInfo& info, bool routedOnly)
{
    const QJsonObject station = ConnectionWorkspace::find(m_workspace.value("stations").toArray(),
        m_pendingWorkspaceStation.isEmpty() ? m_workspace.value("selectedStation").toString() : m_pendingWorkspaceStation);
    QJsonObject path = ConnectionWorkspace::find(station.value("connections").toArray(), m_pendingWorkspaceConnection);
    if (!path.isEmpty()) {
        path["serial"] = info.serial;
        path["family"] = info.family;
        path["model"] = info.model;
        storeWorkspaceConnection(path, station.value("id").toString());
    }
    // Discovery has handed off to the session layer. A later session failure
    // or interactive probe must not inherit this startup probe's ownership.
    m_startupProbe = false;
    if (routedOnly) {
        emit routedRadioFound(info);
    } else {
        emit connectRequested(info);
    }
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
    // Honour the Advanced source-path choice the same way the Flex probe does.
    // On a VPN that exposes more than one adapter, letting the OS pick can send
    // the request out the wrong interface and the reply never comes back.
    const bool explicitBind = bindSettings.mode == RadioBindMode::Explicit
                           && !bindSettings.bindAddress.isNull();
    std::vector<std::unique_ptr<QUdpSocket>> sockets;
    QString bindError;
    if (explicitBind) {
        auto socket = std::make_unique<QUdpSocket>();
        if (socket->bind(bindSettings.bindAddress, 0)) {
            sockets.push_back(std::move(socket));
        } else {
            bindError = socket->errorString();
        }
    } else {
        // Auto must not ask the route table to choose one adapter on a
        // multi-homed host. Send the directed probe from every active IPv4
        // address and keep the source address of the socket that gets the
        // valid reply. This is the manual-IP equivalent of Hl2Discovery's
        // per-interface sweep.
        for (const auto& candidate : NetworkPathResolver::enumerateIpv4Candidates()) {
            auto socket = std::make_unique<QUdpSocket>();
            if (socket->bind(candidate.address, 0))
                sockets.push_back(std::move(socket));
        }

        // Preserve the old wildcard behaviour on hosts where Qt exposes no
        // usable IPv4 candidate at all.
        if (sockets.empty()) {
            auto socket = std::make_unique<QUdpSocket>();
            if (socket->bind(QHostAddress::AnyIPv4, 0))
                sockets.push_back(std::move(socket));
        }
    }

    if (sockets.empty()) {
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
            const QString address = bindSettings.bindAddress.toString();
            const QString error = QStringLiteral("Failed to bind %1: %2")
                                      .arg(address, bindError);
            m_manualSourceWarningLabel->setText(
                error);
            m_manualSourceWarningLabel->setVisible(true);
            updateManualAdvancedVisibility();
            setManualMessage(
                QStringLiteral("AetherSDR could not use that VPN source path. "
                               "Try Auto or choose another path."),
                true);
            reportStartupProbeFailure(
                QStringLiteral("The saved source path for this radio is unavailable."));
        } else {
            setManualMessage(
                QStringLiteral("Could not open a UDP socket to probe for a "
                               "Hermes-Lite 2."),
                true);
            reportStartupProbeFailure(
                QStringLiteral("Could not open a UDP socket to reach the Hermes-Lite 2."));
        }
        return Hl2ProbeResult::NotAttempted;
    }

    // Resolve a name before probing: QHostAddress(ip) is null for non-literals
    // and writeDatagram() to a null address sends nothing. Names must work as on
    // the Flex path (`connect ip <host-or-ip> [flex|hl2]`, automation-bridge.md).
    // fromName() is synchronous, fine on a path that already blocks ~600 ms.
    // IPv4 only: Metis is IPv4-only, so an AAAA-only name is reported as such.
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
            reportStartupProbeFailure(
                QStringLiteral("Could not resolve “%1” to an IPv4 address.").arg(ip));
            return Hl2ProbeResult::NotAttempted;
        }
    }

    const auto request = hl2::discoveryRequest();
    bool sent = false;
    QString lastSendError;
    for (const auto& socket : sockets) {
        // A send that never left is not a radio that stayed silent. Without
        // this the two are indistinguishable and both surface as "check the
        // radio is powered".
        if (socket->writeDatagram(reinterpret_cast<const char*>(request.data()),
                                  qint64(request.size()),
                                  dest,
                                  hl2::kMetisPort) >= 0) {
            sent = true;
        } else {
            lastSendError = socket->errorString();
        }
    }
    if (!sent) {
        setManualMessage(
            QStringLiteral("Could not send a discovery request to %1: %2")
                .arg(dest.toString(), lastSendError),
            true);
        reportStartupProbeFailure(
            QStringLiteral("Could not send a discovery request to %1.").arg(dest.toString()));
        return Hl2ProbeResult::NotAttempted;
    }

    QDeadlineTimer deadline(600);
    while (!deadline.hasExpired()) {
        for (const auto& socket : sockets) {
            if (!socket->hasPendingDatagrams()) {
                const qint64 remaining = deadline.remainingTime();
                if (remaining <= 0)
                    break;
                socket->waitForReadyRead(static_cast<int>(std::min<qint64>(10, remaining)));
            }
            while (socket->hasPendingDatagrams()) {
                const QByteArray d = socket->receiveDatagram().data();
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
                info.sessionBindAddress = socket->localAddress();

                resetManualConnectButton();

                if (reply->streaming) {
                    // #4448: HPSDR Protocol 1 is single-client. Fail closed rather
                    // than wedging both clients; there is no takeover path.
                    setManualMessage(
                        QStringLiteral("The Hermes-Lite 2 at %1 is already in use by another client "
                                       "and can't be shared.").arg(ip),
                        true);
                    reportStartupProbeFailure(
                        QStringLiteral("The Hermes-Lite 2 at %1 is in use by another client.")
                            .arg(ip));
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
                finishManualProbe(info);
                return Hl2ProbeResult::Answered;
            }
        }
    }

    return Hl2ProbeResult::NoAnswer;
}

// Directed (unicast) openHPSDR Protocol 2 discovery against one host. Same
// shape as probeHermesLite2() and for the same reason -- a routed/VPN path
// never sees a broadcast sweep -- with the wire details swapped for
// Protocol 2 (P2Protocol::buildDiscovery/parseDiscoveryReply, kRadioPort,
// isSaturn() instead of isHermesLite2()).
ConnectionPanel::AnanProbeResult ConnectionPanel::probeAnan(
    const QString& ip, const RadioBindSettings& bindSettings)
{
    QUdpSocket hpsdr;
    const bool explicitBind = bindSettings.mode == RadioBindMode::Explicit
                           && !bindSettings.bindAddress.isNull();
    const bool bound = explicitBind
        ? hpsdr.bind(bindSettings.bindAddress, 0)
        : hpsdr.bind(QHostAddress(QHostAddress::AnyIPv4), 0);
    if (!bound) {
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
                QStringLiteral("Could not open a UDP socket to probe for an "
                               "ANAN-G2: %1").arg(hpsdr.errorString()),
                true);
        }
        reportStartupProbeFailure(explicitBind
            ? QStringLiteral("The saved source path for this radio is unavailable.")
            : QStringLiteral("Could not open a UDP socket to reach the ANAN-G2."));
        return AnanProbeResult::NotAttempted;
    }

    // RESOLVE FIRST -- see probeHermesLite2()'s comment; the same bug shape
    // applies verbatim to a hostname entered for an ANAN-G2.
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
                    : QStringLiteral("“%1” has no IPv4 address, and an ANAN-G2 "
                                     "is reachable over IPv4 only.").arg(ip),
                true);
            reportStartupProbeFailure(
                QStringLiteral("Could not resolve an IPv4 address for the ANAN-G2 at %1.").arg(ip));
            return AnanProbeResult::NotAttempted;
        }
    }

    const auto request = anan::buildDiscovery();
    if (hpsdr.writeDatagram(reinterpret_cast<const char*>(request.data()),
                            qint64(request.size()),
                            dest,
                            anan::kRadioPort) < 0) {
        setManualMessage(
            QStringLiteral("Could not send a discovery request to %1: %2")
                .arg(dest.toString(), hpsdr.errorString()),
            true);
        reportStartupProbeFailure(
            QStringLiteral("Could not send a discovery request to %1.").arg(dest.toString()));
        return AnanProbeResult::NotAttempted;
    }

    QDeadlineTimer deadline(600);
    while (!deadline.hasExpired()) {
        if (!hpsdr.waitForReadyRead(static_cast<int>(deadline.remainingTime())))
            break;
        while (hpsdr.hasPendingDatagrams()) {
            const QByteArray d = hpsdr.receiveDatagram().data();
            const auto reply = anan::parseDiscoveryReply(
                std::span<const std::uint8_t>(
                    reinterpret_cast<const std::uint8_t*>(d.constData()), std::size_t(d.size())));
            // Board type 10 (SATURN) only -- this project supports the G2
            // bring-up radio and no other Protocol 2 board (RFC §2.11), same
            // predicate AnanDiscovery applies to broadcast replies.
            if (!reply || !reply->isSaturn())
                continue;

            RadioInfo info;
            info.family   = QString::fromLatin1(kFamilyAnan);
            info.address  = dest;
            info.port     = anan::kRadioPort;
            info.model    = QStringLiteral("ANAN-G2");
            info.name     = info.model;
            info.serial   = anan::AnanDiscovery::macToSerial(reply->mac);
            info.nickname = anan::AnanDiscovery::effectiveNickname(info.family, info.serial,
                                                                    info.model);
            info.version  = QString::number(reply->firmwareVer);
            // A bare integer is a gateware bitstream number, not a software
            // version (discrepancy #1, see P2Protocol.h's DiscoveryReply) --
            // same label the broadcast path sets.
            info.versionLabel = QStringLiteral("Gateware");
            info.inUse    = reply->streaming;
            info.status   = reply->streaming ? QStringLiteral("In_Use")
                                             : QStringLiteral("Available");
            info.isRouted           = true;
            info.bindSettings       = bindSettings;
            info.sessionBindAddress = bindSettings.mode == RadioBindMode::Explicit
                ? bindSettings.bindAddress
                : QHostAddress();

            resetManualConnectButton();

            if (reply->streaming) {
                // openHPSDR Protocol 2 is single-client, same as Protocol 1.
                // Fail closed rather than wedging both clients.
                setManualMessage(
                    QStringLiteral("The ANAN-G2 at %1 is already in use by another client "
                                   "and can't be shared.").arg(ip),
                    true);
                reportStartupProbeFailure(
                    QStringLiteral("The ANAN-G2 at %1 is in use by another client.").arg(ip));
                return AnanProbeResult::Answered;
            }

            saveManualProfile(ip, bindSettings, info.sessionBindAddress);
            rememberManualIp(ip);
            setManualMessage(
                QStringLiteral("Found an ANAN-G2 at %1 — connecting.").arg(ip), false);
            clearPendingIcomCredentials();
            finishManualProbe(info);
            return AnanProbeResult::Answered;
        }
    }

    return AnanProbeResult::NoAnswer;
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
        reportStartupProbeFailure(
            QStringLiteral("The saved source path for this radio is unavailable."));
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
            reportStartupProbeFailure(
                QStringLiteral("No radio responded at %1.").arg(trimmedIp));
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
                finishManualProbe(info);
            } else {
                setManualMessage(
                    QStringLiteral("Found a radio at %1 and saved the path for later.")
                        .arg(trimmedIp));
                finishManualProbe(info, /*routedOnly=*/true);
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
        const QString reason =
            QStringLiteral("Could not reach %1: %2").arg(trimmedIp, sock->errorString());
        setManualMessage(reason, true);
        sock->deleteLater();
        m_manualConnectPending = false;
        m_manualConnectBtn->setText("Connect by IP");
        updateActionState();
        reportStartupProbeFailure(reason);
    });
}

QJsonObject ConnectionPanel::workspaceSnapshot() const
{
    return m_workspace;
}

bool ConnectionPanel::persistWorkspace(const QJsonObject& document)
{
    QString error;
    if (!ConnectionWorkspace::save(document, &error)) {
        setStatusText(error);
        return false;
    }
    m_workspace = document;
    refreshWorkspace();
    emit workspaceChanged();
    return true;
}

void ConnectionPanel::buildWorkspace(QVBoxLayout* root)
{
    m_workspace = ConnectionWorkspace::load();
    auto* page = new QWidget(m_modeStack);
    page->setObjectName(QStringLiteral("connectionWorkspace"));
    auto* columns = new QHBoxLayout(page);
    columns->setContentsMargins(0, 0, 0, 0);
    columns->setSpacing(16);
    auto* stationColumn = new QVBoxLayout;
    stationColumn->addWidget(makeWrappedLabel(tr("LOCATIONS"), kHintLabelStyle));
    m_stationList = new QListWidget(page);
    m_stationList->setObjectName(QStringLiteral("connectionStationList"));
    m_stationList->setAccessibleName(tr("Saved locations"));
    m_stationList->setAccessibleDescription(tr("Drag locations up or down to reorder them, or use Location options, Move."));
    m_stationList->setDragDropMode(QAbstractItemView::InternalMove);
    m_stationList->setDefaultDropAction(Qt::MoveAction);
    m_stationList->setDropIndicatorShown(true);
    m_stationList->setMinimumWidth(110);
    m_stationList->setMaximumWidth(160);
    stationColumn->addWidget(m_stationList, 1);
    auto* addStation = new QPushButton(tr("+ Add location"), page);
    addStation->setObjectName(QStringLiteral("connectionAddStation"));
    stationColumn->addWidget(addStation);
    auto* editStation = new QPushButton(tr("Edit location"), page);
    editStation->setObjectName(QStringLiteral("connectionEditStation"));
    editStation->hide();
    auto* removeStation = new QPushButton(tr("Remove location"), page);
    removeStation->setObjectName(QStringLiteral("connectionRemoveStation"));
    removeStation->hide();
    auto* locationOptions = new QPushButton(tr("Location options…"), page);
    locationOptions->setObjectName(QStringLiteral("connectionLocationOptions"));
    stationColumn->addWidget(locationOptions);
    connect(locationOptions, &QPushButton::clicked, this, [this, locationOptions] {
        showWorkspaceMenu(true, locationOptions->mapToGlobal(QPoint(0, locationOptions->height())));
    });
    m_stationList->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_stationList, &QListWidget::customContextMenuRequested, this, [this](const QPoint& position) {
        if (QListWidgetItem* item = m_stationList->itemAt(position)) { m_stationList->setCurrentItem(item); }
        showWorkspaceMenu(true, m_stationList->viewport()->mapToGlobal(position));
    });
    columns->addLayout(stationColumn);

    auto* radioColumn = new QVBoxLayout;
    m_workspaceTitle = makeWrappedLabel(QString(), kInfoLabelStyle);
    m_workspaceTitle->setTextFormat(Qt::PlainText);
    m_workspaceTitle->setObjectName(QStringLiteral("connectionLocationHeading"));
    QFont locationFont = m_workspaceTitle->font();
    locationFont.setBold(true);
    m_workspaceTitle->setFont(locationFont);
    auto* radioRow = new QHBoxLayout;
    auto* radioListColumn = new QVBoxLayout;
    radioListColumn->addWidget(m_workspaceTitle);
    m_workspaceRadioList = new QListWidget(page);
    m_workspaceRadioList->setObjectName(QStringLiteral("connectionWorkspaceRadios"));
    m_workspaceRadioList->setAccessibleName(tr("Radios at selected location"));
    m_workspaceRadioList->setWordWrap(true);
    m_workspaceRadioList->setMinimumHeight(140);
    m_workspaceRadioList->setItemDelegate(new RadioListDelegate(m_workspaceRadioList));
    m_workspaceRadioList->setSpacing(4);
    m_workspaceRadioList->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_workspaceRadioList, &QListWidget::customContextMenuRequested, this, [this](const QPoint& position) {
        if (QListWidgetItem* item = m_workspaceRadioList->itemAt(position)) { m_workspaceRadioList->setCurrentItem(item); }
        showWorkspaceMenu(false, m_workspaceRadioList->viewport()->mapToGlobal(position));
    });
    radioListColumn->addWidget(m_workspaceRadioList, 1);
    radioRow->addLayout(radioListColumn, 1);
    auto* detailColumn = new QVBoxLayout;
    auto* detailHeading = makeWrappedLabel(tr("Selected radio"), kHintLabelStyle);
    detailHeading->setObjectName(QStringLiteral("connectionSelectedRadioHeading"));
    detailColumn->addWidget(detailHeading);
    auto* detail = new QGroupBox(page);
    detail->setObjectName(QStringLiteral("connectionSelectedRadioGroup"));
    detail->setAccessibleName(tr("Selected radio"));
    applyStyle(detail, "QGroupBox { margin-top: 0; }");
    auto* detailLayout = new QVBoxLayout(detail);
    m_workspaceDetail = makeWrappedLabel(tr("Choose a saved radio or add one from discovery."));
    m_workspaceDetail->setTextFormat(Qt::PlainText);
    m_workspaceDetail->setAccessibleName(tr("Selected connection details"));
    detailLayout->addWidget(m_workspaceDetail);
    m_workspacePath = new QComboBox(detail);
    m_workspacePath->setObjectName(QStringLiteral("connectionWorkspacePath"));
    m_workspacePath->setAccessibleName(tr("Saved connection path"));
    applyComboStyle(m_workspacePath);
    detailLayout->addWidget(m_workspacePath);
    m_workspaceSave = new QPushButton(tr("Save to location"), detail);
    m_workspaceSave->setObjectName(QStringLiteral("connectionSaveToStation"));
    auto* connectionSettings = new QToolButton(detail);
    connectionSettings->setText(tr("Connection settings"));
    connectionSettings->setObjectName(QStringLiteral("connectionSettingsToggle"));
    connectionSettings->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    connectionSettings->setArrowType(Qt::RightArrow);
    connectionSettings->setCheckable(true);
    detailLayout->addWidget(connectionSettings);
    auto* settingsBody = new QWidget(detail);
    auto* settingsLayout = new QVBoxLayout(settingsBody);
    settingsLayout->setContentsMargins(0, 0, 0, 0);
    settingsLayout->addWidget(m_workspaceSave);
    auto* editConnection = new QPushButton(tr("Edit connection"), detail);
    editConnection->setObjectName(QStringLiteral("connectionEditSaved"));
    settingsLayout->addWidget(editConnection);
    auto* removeConnection = new QPushButton(tr("Remove connection"), detail);
    removeConnection->setObjectName(QStringLiteral("connectionRemoveSaved"));
    settingsLayout->addWidget(removeConnection);
    detailLayout->addWidget(settingsBody);
    settingsBody->hide();
    connect(connectionSettings, &QToolButton::toggled, this, [this, connectionSettings, settingsBody](bool open) {
        connectionSettings->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
        settingsBody->setVisible(open);
        refitToContent();
    });
    detailLayout->addStretch();
    m_workspaceConnect = new QPushButton(tr("Connect"), detail);
    m_workspaceConnect->setObjectName(QStringLiteral("connectionWorkspaceConnect"));
    m_workspaceConnect->setAccessibleName(tr("Connect to selected radio"));
    applyStyle(m_workspaceConnect,
        "QPushButton { background: {{color.canon.cyan}}; color: {{color.canon.onAccent}};"
        " border: 1px solid {{color.canon.lineHi}}; border-radius: 6px; padding: 8px 14px; }"
        "QPushButton:disabled { background: {{color.canon.control}}; color: {{color.canon.muted}}; }");
    detailLayout->addWidget(m_workspaceConnect);
    auto* radioOptions = new QPushButton(tr("Radio options…"), detail);
    radioOptions->setObjectName(QStringLiteral("connectionRadioOptions"));
    detailLayout->addWidget(radioOptions);
    connect(radioOptions, &QPushButton::clicked, this, [this, radioOptions] {
        showWorkspaceMenu(false, radioOptions->mapToGlobal(QPoint(0, radioOptions->height())));
    });
    detailColumn->addWidget(detail, 1);
    radioRow->addLayout(detailColumn, 1);
    radioColumn->addLayout(radioRow, 1);
    auto* tools = new QHBoxLayout;
    auto* nearby = new QPushButton(tr("Nearby radios"), page);
    nearby->setObjectName(QStringLiteral("connectionNearbyRadios"));
    auto* addManual = new QPushButton(tr("+ Add radio by IP"), page);
    addManual->setObjectName(QStringLiteral("connectionAddRadio"));
    tools->addWidget(nearby);
    tools->addWidget(addManual);
    radioColumn->addLayout(tools);
    auto* profiles = new QHBoxLayout;
    profiles->addWidget(new QLabel(tr("Operating as"), page));
    m_operatorCombo = new QComboBox(page);
    m_operatorCombo->setObjectName(QStringLiteral("connectionOperatorProfile"));
    m_operatorCombo->setAccessibleName(tr("Operator profile for next connection"));
    applyComboStyle(m_operatorCombo);
    profiles->addWidget(m_operatorCombo, 1);
    auto* manageProfiles = new QPushButton(tr("Profiles…"), page);
    manageProfiles->setObjectName(QStringLiteral("connectionManageProfiles"));
    profiles->addWidget(manageProfiles);
    auto* accounts = new QPushButton(tr("SmartLink accounts…"), page);
    accounts->setObjectName(QStringLiteral("connectionManageAccounts"));
    profiles->addWidget(accounts);
    radioColumn->addLayout(profiles);
    columns->addLayout(radioColumn, 1);
    m_modeStack->addWidget(page);

    auto* back = new QPushButton(tr("‹ Back to location"), this);
    back->setObjectName(QStringLiteral("connectionBackToStation"));
    root->insertWidget(0, back, 0, Qt::AlignLeft);
    connect(back, &QPushButton::clicked, this, [this] {
        m_pendingSmartLinkConnection = {};
        setCurrentMode(WorkspaceMode);
        refreshWorkspace();
    });
    connect(m_modeStack, &QStackedWidget::currentChanged, back,
        [back](int index) { back->setVisible(index != WorkspaceMode); });
    back->hide();
    connect(addStation, &QPushButton::clicked, this, [this] { editWorkspaceProfile("stations"); });
    connect(editStation, &QPushButton::clicked, this, [this] {
        editWorkspaceProfile("stations", m_workspace.value("selectedStation").toString());
    });
    connect(removeStation, &QPushButton::clicked, this, [this] {
        removeWorkspaceProfile("stations", m_workspace.value("selectedStation").toString());
    });
    connect(manageProfiles, &QPushButton::clicked, this, &ConnectionPanel::manageOperatorProfiles);
    connect(nearby, &QPushButton::clicked, this, [this] { setCurrentMode(LocalMode); });
    connect(addManual, &QPushButton::clicked, this, [this] {
        m_pendingWorkspaceConnection.clear();
        m_pendingWorkspaceStation.clear();
        m_activeIcomCredentialId.clear();
        m_allowLegacyIcomCredential = false;
        m_manualIcomPassEdit->clear();
        m_manualConnectionName->clear();
        setCurrentMode(ManualMode);
    });
    connect(m_manualIpEdit, &QLineEdit::textEdited, this, [this] {
        m_activeIcomCredentialId.clear();
        m_allowLegacyIcomCredential = false;
        m_manualIcomPassEdit->clear();
    });
    connect(m_manualIcomUserEdit, &QLineEdit::textEdited, this, [this] {
        m_activeIcomCredentialId.clear();
        m_allowLegacyIcomCredential = false;
        m_manualIcomPassEdit->clear();
    });
    connect(accounts, &QPushButton::clicked, this, [this] { setCurrentMode(SmartLinkMode); });
    connect(m_workspaceRadioList, &QListWidget::currentRowChanged, this, [this](int) {
        m_pendingSmartLinkConnection = {};
        refreshWorkspaceDetails();
    });
    connect(m_workspacePath, &QComboBox::currentIndexChanged, this, [this] {
        m_pendingSmartLinkConnection = {};
        refreshWorkspaceDetails();
    });
    connect(m_workspaceConnect, &QPushButton::clicked, this, &ConnectionPanel::connectWorkspaceSelection);
    connect(m_workspaceSave, &QPushButton::clicked, this, &ConnectionPanel::saveWorkspaceSelection);
    connect(m_stationList, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row < 0) { return; }
        const QString id = m_stationList->item(row)->data(Qt::UserRole).toString();
        if (id != m_workspace.value("selectedStation").toString()) { selectWorkspaceProfile("stations", id); }
    });
    connect(m_stationList->model(), &QAbstractItemModel::rowsMoved, this, [this] {
        QJsonArray ordered;
        const QJsonArray stations = m_workspace.value("stations").toArray();
        for (int row = 0; row < m_stationList->count(); ++row) {
            const QJsonObject station = ConnectionWorkspace::find(stations,
                m_stationList->item(row)->data(Qt::UserRole).toString());
            if (station.isEmpty()) { refreshWorkspace(); return; }
            ordered.append(station);
        }
        if (ordered == stations) { return; }
        QJsonObject next = m_workspace;
        next["stations"] = ordered;
        if (!persistWorkspace(next)) { refreshWorkspace(); }
    }, Qt::QueuedConnection);
    connect(m_operatorCombo, &QComboBox::currentIndexChanged, this, [this](int) {
        selectWorkspaceProfile("operators", m_operatorCombo->currentData().toString());
    });
    connect(editConnection, &QPushButton::clicked, this, [this] {
        const QJsonObject connection = m_workspacePath->currentData().toJsonObject();
        if (connection.value("kind").toString() == QLatin1String("manual")) {
            restoreWorkspaceManual(connection);
            setCurrentMode(ManualMode);
        } else {
            editWorkspaceProfile("connections", connection.value("id").toString());
        }
    });
    connect(removeConnection, &QPushButton::clicked, this, [this] {
        removeWorkspaceProfile("connections", m_workspacePath->currentData().toJsonObject().value("id").toString());
    });

    // The existing pages are the setup tools; saving never requires a live
    // radio. Discovery and their established protocol paths remain intact.
    for (int index : {LocalMode, SmartLinkMode}) {
        auto* save = new QPushButton(tr("Save selected radio to location"), this);
        save->setObjectName(index == LocalMode ? QStringLiteral("connectionSaveLocal") : QStringLiteral("connectionSaveRemote"));
        qobject_cast<QVBoxLayout*>(m_modeStack->widget(index)->layout())->addWidget(save);
        connect(save, &QPushButton::clicked, this, [this, index] {
            if (index == LocalMode && m_radioList->currentRow() >= 0) {
                rememberWorkspaceRadio(m_radios.at(m_radioList->currentRow()), "local");
            } else if (index == SmartLinkMode && m_wanList->currentRow() >= 0) {
                rememberWorkspaceWan(m_wanRadios.at(m_wanList->currentRow()));
            }
            setCurrentMode(WorkspaceMode);
        });
    }
    auto* saveManual = findChild<QPushButton*>(QStringLiteral("connectionSaveManual"));
    connect(saveManual, &QPushButton::clicked, this, [this] {
        const QString host = normalizeManualIp(m_manualIpEdit->text());
        if (host.isEmpty()) { setStatusText(tr("Enter a valid radio host name or IP address.")); return; }
        bool stale = false;
        const RadioBindSettings bind = currentManualBindSettings(&stale);
        if (stale) { setStatusText(tr("Choose an available source interface before saving.")); return; }
        saveManualProfile(host, bind, bind.bindAddress);
        setCurrentMode(WorkspaceMode);
    });

    auto* accountRow = new QHBoxLayout;
    m_accountCombo = new QComboBox(this);
    m_accountCombo->setObjectName(QStringLiteral("connectionSmartLinkAccount"));
    m_accountCombo->setAccessibleName(tr("Saved SmartLink account"));
    applyComboStyle(m_accountCombo);
    accountRow->addWidget(m_accountCombo, 1);
    for (const QString& action : {QStringLiteral("Add"), QStringLiteral("Rename"), QStringLiteral("Remove")}) {
        auto* button = new QPushButton(action, this);
        button->setObjectName(QStringLiteral("connectionAccount") + action);
        accountRow->addWidget(button);
        connect(button, &QPushButton::clicked, this, [this, action] {
            const QString id = m_accountCombo->currentData().toString();
            if (action == QLatin1String("Remove")) { removeWorkspaceProfile("accounts", id); }
            else { editWorkspaceProfile("accounts", action == QLatin1String("Rename") ? id : QString()); }
        });
    }
    qobject_cast<QVBoxLayout*>(m_modeStack->widget(SmartLinkMode)->layout())->insertLayout(0, accountRow);
    if (!SmartLinkClient::persistentStoreAvailable()) {
        qobject_cast<QVBoxLayout*>(m_modeStack->widget(SmartLinkMode)->layout())->insertWidget(1,
            makeWrappedLabel(tr("This build keeps SmartLink credentials for this session only. Account names remain saved."), kHintLabelStyle));
    }
    connect(m_accountCombo, &QComboBox::currentIndexChanged, this, [this](int) {
        m_pendingSmartLinkConnection = {};
        activateWorkspaceAccount(m_accountCombo->currentData().toString());
    });
    refreshWorkspace();
}

void ConnectionPanel::refreshWorkspace()
{
    if (!m_stationList) { return; }
    const QSignalBlocker stationsBlock(m_stationList);
    const QSignalBlocker operatorsBlock(m_operatorCombo);
    const QSignalBlocker radiosBlock(m_workspaceRadioList);
    const QString previous = m_workspaceRadioList->currentItem()
        ? m_workspaceRadioList->currentItem()->data(Qt::UserRole).toString() : QString();
    m_stationList->clear();
    const QString stationId = m_workspace.value("selectedStation").toString();
    for (const QJsonValue& value : m_workspace.value("stations").toArray()) {
        const QJsonObject station = value.toObject();
        auto* item = new QListWidgetItem(station.value("name").toString(), m_stationList);
        item->setData(Qt::UserRole, station.value("id"));
        item->setFlags(item->flags() & ~Qt::ItemIsDropEnabled);
        if (station.value("id").toString() == stationId) {
            QFont font = m_stationList->font();
            font.setBold(true);
            item->setFont(font);
            m_stationList->setCurrentItem(item);
        }
    }
    m_operatorCombo->clear();
    for (const QJsonValue& value : m_workspace.value("operators").toArray()) {
        const QJsonObject profile = value.toObject();
        m_operatorCombo->addItem(profile.value("name").toString(), profile.value("id").toString());
    }
    m_operatorCombo->setCurrentIndex(m_operatorCombo->findData(m_workspace.value("selectedOperator").toString()));
    const QJsonObject station = ConnectionWorkspace::find(m_workspace.value("stations").toArray(), stationId);
    const QString radioHeading = station.isEmpty() ? tr("Connection profiles unavailable")
        : tr("Radios at %1").arg(station.value("name").toString());
    m_workspaceTitle->setText(radioHeading);
    m_workspaceRadioList->setAccessibleName(radioHeading);
    m_workspaceRadioList->clear();
    QJsonArray connections = station.value("connections").toArray();
    // The imported station also offers nearby radios until saved groups have
    // been populated. These sightings are not silently persisted or moved.
    if (connections.isEmpty()) {
        for (const RadioInfo& info : m_radios) {
            connections.append(QJsonObject{{"id", "nearby:" + info.family + ":" + info.serial},
                {"kind", "local"}, {"family", info.family}, {"serial", info.serial},
                {"model", info.model}, {"name", info.nickname.isEmpty() ? info.model : info.nickname}, {"nearby", true}});
        }
    }
    QMap<QString, QJsonArray> groups;
    QStringList order;
    for (const QJsonValue& value : connections) {
        const QJsonObject connection = value.toObject();
        const QString serial = connection.value("serial").toString();
        const QString key = serial.isEmpty() ? connection.value("id").toString()
            : connection.value("family").toString() + ":" + serial;
        if (!groups.contains(key)) { order.append(key); }
        groups[key].append(connection);
    }
    const auto hasSavedName = [](const QJsonObject& connection) {
        const QString name = connection.value("name").toString().trimmed();
        return !name.isEmpty() && name.compare(connection.value("host").toString(), Qt::CaseInsensitive) != 0
            && QHostAddress(name).isNull();
    };
    for (auto it = groups.begin(); it != groups.end(); ++it) {
        QJsonArray sorted;
        for (bool named : {true, false}) {
            for (const QJsonValue& path : it.value()) {
                if (hasSavedName(path.toObject()) == named) { sorted.append(path); }
            }
        }
        it.value() = sorted;
    }
    std::stable_sort(order.begin(), order.end(), [&groups, &hasSavedName](const QString& left, const QString& right) {
        const QJsonObject a = groups.value(left).first().toObject();
        const QJsonObject b = groups.value(right).first().toObject();
        if (hasSavedName(a) != hasSavedName(b)) { return hasSavedName(a); }
        return QString::compare(a.value("name").toString(), b.value("name").toString(), Qt::CaseInsensitive) < 0;
    });
    for (const QString& key : order) {
        const QJsonArray paths = groups.value(key);
        const QJsonObject connection = paths.first().toObject();
        QString status = tr("Saved connection");
        for (const RadioInfo& info : m_radios) {
            if (info.serial == connection.value("serial").toString()
                && (info.family == connection.value("family").toString() || connection.value("family").toString().isEmpty())) {
                status = info.inUse ? tr("In use") : tr("Discovered nearby");
                break;
            }
        }
        const QString title = hasSavedName(connection) ? connection.value("name").toString()
            : connection.value("model").toString(tr("Unnamed radio"));
        const QString secondary = hasSavedName(connection) ? connection.value("model").toString()
            : connection.value("host").toString();
        const QString description = secondary.isEmpty() ? status : secondary + QStringLiteral(" · ") + status;
        auto* item = new QListWidgetItem(title + QLatin1Char('\n') + description, m_workspaceRadioList);
        item->setData(Qt::AccessibleTextRole, title + QStringLiteral(", ") + description);
        item->setToolTip(description);
        item->setData(Qt::UserRole, key);
        item->setData(Qt::UserRole + 1, paths);
        if (key == previous) { m_workspaceRadioList->setCurrentItem(item); }
    }
    if (!m_workspaceRadioList->currentItem() && m_workspaceRadioList->count()) { m_workspaceRadioList->setCurrentRow(0); }
    refreshWorkspaceAccounts();
    refreshWorkspaceDetails();
}

void ConnectionPanel::refreshWorkspaceDetails()
{
    if (!m_workspaceRadioList) { return; }
    const QListWidgetItem* item = m_workspaceRadioList->currentItem();
    const QString previousPath = m_workspacePath->currentData().toJsonObject().value("id").toString();
    if (item && item->data(Qt::UserRole).toString() != m_workspaceSelection) {
        m_workspaceSelection = item->data(Qt::UserRole).toString();
        const QSignalBlocker blocker(m_workspacePath);
        m_workspacePath->clear();
    }
    if (item) {
        const QSignalBlocker blocker(m_workspacePath);
        const QJsonArray paths = item->data(Qt::UserRole + 1).toJsonArray();
        m_workspacePath->clear();
        for (const QJsonValue& value : paths) {
            const QJsonObject path = value.toObject();
            const QString kind = path.value("kind").toString();
            const QString account = ConnectionWorkspace::find(m_workspace.value("accounts").toArray(), path.value("accountId").toString()).value("name").toString();
            const QString label = kind == QLatin1String("local") ? tr("Discovery / USB")
                : kind == QLatin1String("smartlink") ? tr("SmartLink · %1").arg(account)
                : tr("Saved address · %1").arg(path.value("host").toString());
            m_workspacePath->addItem(label, path);
            if (path.value("id").toString() == previousPath) { m_workspacePath->setCurrentIndex(m_workspacePath->count() - 1); }
        }
    } else {
        const QSignalBlocker blocker(m_workspacePath);
        m_workspacePath->clear();
    }
    const QJsonObject path = m_workspacePath->currentData().toJsonObject();
    m_workspacePath->setVisible(m_workspacePath->count() > 1);
    const QJsonObject profile = ConnectionWorkspace::find(m_workspace.value("operators").toArray(), m_workspace.value("selectedOperator").toString());
    m_workspaceDetail->setText(path.isEmpty() ? tr("Choose a saved radio or add one from Nearby radios.")
        : tr("%1\n%2\n\nOperator: %3\n%4")
            .arg(path.value("name").toString(), path.value("model").toString(), profile.value("name").toString(), m_workspacePath->currentText()));
    const bool allowed = !m_connected && !path.isEmpty() && !m_workspace.isEmpty();
    m_workspaceConnect->setEnabled(allowed);
    m_workspaceConnect->setAccessibleDescription(m_connected ? tr("Disconnect the current radio before connecting another.")
        : path.isEmpty() ? tr("Choose a radio first.") : tr("Uses the selected saved connection and operator preferences."));
    m_workspaceSave->setEnabled(!path.isEmpty());
    m_workspaceSave->setText(path.value("nearby").toBool() ? tr("Save to location") : tr("Copy to another location…"));
}

bool ConnectionPanel::selectWorkspaceProfile(const QString& collection, const QString& id, QString* error)
{
    if (collection != QLatin1String("stations") && collection != QLatin1String("operators")) {
        setAutomationError(error, tr("Unknown profile collection.")); return false;
    }
    if (ConnectionWorkspace::find(m_workspace.value(collection).toArray(), id).isEmpty()) {
        setAutomationError(error, tr("Profile not found.")); return false;
    }
    QJsonObject next = m_workspace;
    m_pendingSmartLinkConnection = {};
    next[collection == QLatin1String("stations") ? "selectedStation" : "selectedOperator"] = id;
    return persistWorkspace(next);
}

void ConnectionPanel::storeWorkspaceConnection(const QJsonObject& connection, const QString& stationId, bool preserveName)
{
    QJsonObject next = m_workspace;
    QJsonArray stations = next.value("stations").toArray();
    for (qsizetype i = 0; i < stations.size(); ++i) {
        QJsonObject station = stations[i].toObject();
        if (station.value("id").toString() != (stationId.isEmpty() ? next.value("selectedStation").toString() : stationId)) { continue; }
        QJsonArray connections = station.value("connections").toArray();
        bool replaced = false;
        for (qsizetype j = 0; j < connections.size(); ++j) {
            const QJsonObject old = connections[j].toObject();
            if ((!connection.value("id").toString().isEmpty() && old.value("id") == connection.value("id"))
                || (old.value("kind") == connection.value("kind") && old.value("family") == connection.value("family")
                    && old.value("serial") == connection.value("serial") && old.value("host") == connection.value("host")
                    && old.value("accountId") == connection.value("accountId")
                    && old.value("route") == connection.value("route")
                    && old.value("username") == connection.value("username"))) {
                QJsonObject replacement = old;
                for (auto it = connection.begin(); it != connection.end(); ++it) { replacement[it.key()] = it.value(); }
                replacement["id"] = old.value("id");
                if (preserveName && old.contains("name")) { replacement["name"] = old.value("name"); }
                connections[j] = replacement;
                replaced = true;
                break;
            }
        }
        if (!replaced) { connections.append(connection); }
        station["connections"] = connections;
        stations[i] = station;
    }
    next["stations"] = stations;
    persistWorkspace(next);
}

void ConnectionPanel::rememberWorkspaceRadio(const RadioInfo& info, const QString& kind)
{
    if (m_workspace.isEmpty()) { return; }
    storeWorkspaceConnection({{"id", ConnectionWorkspace::newId()}, {"name", info.nickname.isEmpty() ? info.model : info.nickname},
        {"model", info.model}, {"serial", info.serial}, {"family", info.family}, {"kind", kind}});
}

void ConnectionPanel::rememberWorkspaceWan(const WanRadioInfo& info)
{
    if (!m_smartLink) { return; }
    storeWorkspaceConnection({{"id", ConnectionWorkspace::newId()}, {"name", info.nickname.isEmpty() ? info.model : info.nickname},
        {"model", info.model}, {"serial", info.serial}, {"family", "flex"}, {"kind", "smartlink"},
        {"accountId", m_smartLink->accountId()}, {"lowBandwidth", m_lowBwCheck->isChecked()}});
}

void ConnectionPanel::rememberWorkspaceManual(const QString& host, const QJsonObject& route)
{
    if (m_workspace.isEmpty()) { return; }
    if (m_pendingWorkspaceConnection.isEmpty()) { m_pendingWorkspaceConnection = ConnectionWorkspace::newId(); }
    storeWorkspaceConnection({{"id", m_pendingWorkspaceConnection},
        {"name", m_manualConnectionName->text().trimmed().isEmpty() ? host : m_manualConnectionName->text().trimmed()},
        {"model", m_manualRadioTypeCombo->currentText()},
        {"kind", "manual"}, {"family", currentManualFamily()}, {"host", host}, {"route", route},
        {"username", m_manualIcomUserEdit->text().trimmed()}, {"civ", m_manualIcomCivCombo->currentData().toString()},
        {"credentialId", m_activeIcomCredentialId},
        {"customCiv", m_manualIcomCivEdit->text()}, {"lowBandwidth", m_lowBwCheck->isChecked()}}, m_pendingWorkspaceStation, false);
}

void ConnectionPanel::saveWorkspaceSelection()
{
    const QJsonObject path = m_workspacePath->currentData().toJsonObject();
    if (path.isEmpty()) { return; }
    if (path.value("nearby").toBool()) {
        for (const RadioInfo& info : m_radios) {
            if (info.serial == path.value("serial").toString() && info.family == path.value("family").toString()) {
                rememberWorkspaceRadio(info, "local"); return;
            }
        }
        return;
    }
    auto* dialog = new CanonWindow(tr("Copy connection to location"), this);
    dialog->setObjectName(QStringLiteral("connectionCopyRadio"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    auto* layout = new QVBoxLayout(dialog->bodyWidget());
    layout->addWidget(makeCanonHeader(tr("Copy connection to location")));
    auto* stations = new QComboBox(dialog);
    stations->setObjectName(QStringLiteral("connectionCopyDestination"));
    stations->setAccessibleName(tr("Destination location"));
    for (const QJsonValue& value : m_workspace.value("stations").toArray()) {
        const QJsonObject station = value.toObject();
        stations->addItem(station.value("name").toString(), station.value("id").toString());
    }
    layout->addWidget(stations);
    auto* save = new QPushButton(tr("Copy connection"), dialog);
    save->setObjectName(QStringLiteral("connectionConfirmCopy"));
    save->setDefault(true);
    layout->addWidget(save);
    connect(save, &QPushButton::clicked, dialog, [this, dialog, stations, path] {
        QJsonObject next = m_workspace;
        QJsonArray records = next.value("stations").toArray();
        for (qsizetype i = 0; i < records.size(); ++i) {
            QJsonObject record = records[i].toObject();
            if (record.value("id").toString() != stations->currentData().toString()) { continue; }
            QJsonArray connections = record.value("connections").toArray();
            QJsonObject copy = path;
            copy["id"] = ConnectionWorkspace::newId();
            connections.append(copy);
            record["connections"] = connections;
            records[i] = record;
        }
        next["stations"] = records;
        if (persistWorkspace(next)) { dialog->accept(); }
    });
    dialog->resize(420, 200);
    dialog->show();
}

bool ConnectionPanel::applyWorkspaceOperator()
{
    if (m_workspace.isEmpty()) { return true; }
    const QJsonObject profile = ConnectionWorkspace::find(m_workspace.value("operators").toArray(), m_workspace.value("selectedOperator").toString());
    const QJsonObject preferences = profile.value("preferences").toObject();
    for (const QString& key : {QStringLiteral("inputDeviceId"), QStringLiteral("outputDeviceId")}) {
        const QByteArray id = QByteArray::fromBase64(preferences.value(key).toString().toLatin1());
        const QList<QAudioDevice> devices = key == QLatin1String("inputDeviceId") ? QMediaDevices::audioInputs() : QMediaDevices::audioOutputs();
        const bool found = id.isEmpty() || std::any_of(devices.cbegin(), devices.cend(), [&id](const QAudioDevice& device) { return device.id() == id; });
        if (!found) {
            setStatusText(tr("The operator profile's %1 is unavailable. Edit Profiles to choose a device or System default before connecting.")
                .arg(key == QLatin1String("inputDeviceId") ? tr("microphone") : tr("output device")));
            return false;
        }
    }
    emit operatorPreferencesRequested(preferences);
    return true;
}

void ConnectionPanel::connectWorkspaceSelection()
{
    if (m_connected) { setStatusText(tr("Disconnect the current radio first.")); return; }
    const QJsonObject path = m_workspacePath->currentData().toJsonObject();
    if (path.isEmpty()) { return; }
    const QString kind = path.value("kind").toString();
    if (kind == QLatin1String("local")) {
        for (int i = 0; i < m_radios.size(); ++i) {
            if (m_radios[i].serial == path.value("serial").toString()
                && (path.value("family").toString().isEmpty() || m_radios[i].family == path.value("family").toString())) {
                m_radioList->setCurrentRow(i);
                onLocalConnectClicked();
                return;
            }
        }
        setStatusText(tr("This radio is not currently discovered. Power it on, retry discovery or add a saved IP path."));
        return;
    }
    if (kind == QLatin1String("smartlink")) {
        const QString id = path.value("accountId").toString();
        if (ConnectionWorkspace::find(m_workspace.value("accounts").toArray(), id).isEmpty()) {
            setStatusText(tr("This connection's SmartLink account was removed. Add that account again in SmartLink accounts."));
            return;
        }
        m_pendingSmartLinkConnection = path;
        if (!m_smartLink || m_smartLink->accountId() != id || !m_smartLink->isAuthenticated() || !m_smartLink->isConnected()) {
            if (!activateWorkspaceAccount(id)) { m_pendingSmartLinkConnection = {}; return; }
            setCurrentMode(SmartLinkMode);
            setStatusText(tr("Looking up this radio with its saved account. Sign in if requested; the connection will continue automatically."));
            return;
        }
        resumeWorkspaceSmartLink();
        return;
    }
    if (kind == QLatin1String("manual")) {
        if (m_manualRadioTypeCombo->findData(path.value("family").toString()) < 0) {
            setStatusText(tr("This build cannot connect to that radio family.")); return;
        }
        restoreWorkspaceManual(path);
        setCurrentMode(ManualMode);
        onManualConnectClicked();
    }
}

void ConnectionPanel::restoreWorkspaceManual(const QJsonObject& path)
{
    m_manualConnectionName->setText(path.value("name").toString());
    m_pendingWorkspaceConnection = path.value("id").toString();
    m_pendingWorkspaceStation = m_workspace.value("selectedStation").toString();
    m_activeIcomCredentialId = path.value("credentialId").toString();
    m_allowLegacyIcomCredential = path.value("legacyCredentials").toBool();
    m_manualIcomPassEdit->clear();
    IcomCredentials::clearSession();
    m_manualIcomUserEdit->setText(path.value("username").toString());
    setManualFamily(path.value("family").toString());
    m_manualIpEdit->setText(path.value("host").toString());
    QJsonObject routes = loadRoutedProfiles();
    routes[path.value("host").toString()] = path.value("route").toObject();
    saveRoutedProfiles(routes);
    applySavedSourceSelection(path.value("host").toString(), true);
    setManualFamily(path.value("family").toString());
    updateManualFamilyHints();
    const int civIndex = m_manualIcomCivCombo->findData(path.value("civ").toString());
    if (civIndex >= 0) { m_manualIcomCivCombo->setCurrentIndex(civIndex); }
    m_manualIcomCivEdit->setText(path.value("customCiv").toString());
    m_lowBwCheck->setChecked(path.value("lowBandwidth").toBool());
}

void ConnectionPanel::resumeWorkspaceSmartLink()
{
    if (m_pendingSmartLinkConnection.isEmpty() || m_connected || !m_smartLink
        || !m_smartLink->isAuthenticated()
        || m_smartLink->accountId() != m_pendingSmartLinkConnection.value("accountId").toString()) { return; }
    for (int i = 0; i < m_wanRadios.size(); ++i) {
        if (m_wanRadios[i].serial == m_pendingSmartLinkConnection.value("serial").toString()) {
            m_lowBwCheck->setChecked(m_pendingSmartLinkConnection.value("lowBandwidth").toBool());
            m_pendingSmartLinkConnection = {};
            m_wanList->setCurrentRow(i);
            onWanConnectClicked();
            return;
        }
    }
    setStatusText(tr("This radio is offline or unavailable to the selected SmartLink account. Waiting for an updated list."));
}

void ConnectionPanel::refreshWorkspaceAccounts()
{
    if (!m_accountCombo) { return; }
    const QSignalBlocker blocker(m_accountCombo);
    m_accountCombo->clear();
    for (const QJsonValue& value : m_workspace.value("accounts").toArray()) {
        const QJsonObject account = value.toObject();
        m_accountCombo->addItem(account.value("name").toString(), account.value("id").toString());
    }
    m_accountCombo->setCurrentIndex(m_accountCombo->findData(m_workspace.value("selectedAccount").toString()));
}

bool ConnectionPanel::activateWorkspaceAccount(const QString& id)
{
    const QJsonObject account = ConnectionWorkspace::find(m_workspace.value("accounts").toArray(), id);
    if (!m_smartLink || account.isEmpty()) { return false; }
    if (m_smartLink->accountLocked()) {
        setStatusText(tr("Disconnect the current SmartLink radio before changing accounts."));
        refreshWorkspaceAccounts();
        return false;
    }
    QJsonObject next = m_workspace;
    next["selectedAccount"] = id;
    if (!persistWorkspace(next)) { return false; }
    if (m_smartLink->accountId() == id && m_smartLink->isAuthenticated()) {
        m_smartLink->reconnect();
        return true;
    }
    m_smartLink->setAccountId(id);
    m_wanRadios.clear();
    m_wanList->clear();
    m_emailEdit->setText(account.value("email").toString());
    m_passwordEdit->clear();
    setAccountLine(tr("Loading the selected account. Sign in if its saved credential is unavailable."), AccountTone::Hint);
    emit smartLinkSignedOut();
    m_smartLink->tryAutoLogin();
    updateSmartLinkUi();
    return true;
}

bool ConnectionPanel::prepareWorkspaceLogin(const QString& email)
{
    if (!m_smartLink || m_smartLink->accountLocked()) {
        setStatusText(tr("Disconnect the current SmartLink radio before signing in.")); return false;
    }
    QJsonObject next = m_workspace;
    QJsonArray accounts = next.value("accounts").toArray();
    QString id;
    for (const QJsonValue& value : accounts) {
        const QJsonObject account = value.toObject();
        if (account.value("email").toString().compare(email, Qt::CaseInsensitive) == 0) { id = account.value("id").toString(); break; }
    }
    if (id.isEmpty()) {
        id = ConnectionWorkspace::accountId(email);
        accounts.append(QJsonObject{{"id", id}, {"name", email}, {"email", email}});
    }
    next["accounts"] = accounts;
    next["selectedAccount"] = id;
    if (!persistWorkspace(next)) { return false; }
    m_smartLink->setAccountId(id);
    return true;
}

void ConnectionPanel::editWorkspaceProfile(const QString& collection, const QString& id, bool duplicate, bool renameOnly)
{
    if (collection == QLatin1String("accounts") && id.isEmpty() && m_smartLink && m_smartLink->accountLocked()) {
        setStatusText(tr("Disconnect the SmartLink radio before adding another account."));
        return;
    }
    QJsonArray records = m_workspace.value(collection).toArray();
    if (collection == QLatin1String("connections")) {
        records = ConnectionWorkspace::find(m_workspace.value("stations").toArray(),
            m_workspace.value("selectedStation").toString()).value("connections").toArray();
    }
    const QJsonObject original = ConnectionWorkspace::find(records, id);
    if (!id.isEmpty() && original.isEmpty()) { setStatusText(tr("Save this connection to the location first.")); return; }
    const bool newRecord = id.isEmpty() || duplicate;
    const QString title = collection == QLatin1String("stations") ? tr("Location profile")
        : collection == QLatin1String("operators") ? tr("Operator profile")
        : collection == QLatin1String("accounts") ? tr("SmartLink account") : tr("Saved connection");
    auto* dialog = new CanonWindow((renameOnly ? tr("Rename %1") : newRecord ? tr("Create %1") : tr("Edit %1")).arg(title), this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setObjectName(QStringLiteral("connectionProfileEditor"));
    dialog->setAccessibleName(dialog->windowTitle());
    dialog->setReturnClicksDefault(true);
    theme::setContainer(dialog, QStringLiteral("canon/connectionProfile"));
    applyCanonSheet(dialog->bodyWidget(), [] { return QString::fromLatin1(kConnectionStyle) + canonIndicatorRules(); });
    auto* layout = new QVBoxLayout(dialog->bodyWidget());
    layout->addWidget(makeCanonHeader(dialog->windowTitle()));
    auto* form = new QFormLayout;
    auto* name = new QLineEdit(dialog);
    name->setObjectName(QStringLiteral("connectionProfileName"));
    name->setAccessibleName(tr("Profile name"));
    name->setMaxLength(128);
    name->setText(original.value("name").toString() + (duplicate ? tr(" copy") : QString()));
    form->addRow(tr("Name:"), name);
    auto* email = new QLineEdit(dialog);
    email->setObjectName(QStringLiteral("connectionAccountEmail"));
    email->setAccessibleName(tr("SmartLink account email"));
    email->setText(original.value("email").toString());
    email->setReadOnly(!newRecord);
    if (collection == QLatin1String("accounts")) { form->addRow(tr("Email:"), email); }
    else { email->hide(); }

    auto* copyFrom = new QComboBox(dialog);
    copyFrom->setAccessibleName(tr("Start from operator profile"));
    auto* input = new QComboBox(dialog);
    auto* output = new QComboBox(dialog);
    input->setObjectName(QStringLiteral("connectionProfileInput"));
    output->setObjectName(QStringLiteral("connectionProfileOutput"));
    input->setAccessibleName(tr("Operator microphone"));
    output->setAccessibleName(tr("Operator audio output"));
    auto* panLayout = new QComboBox(dialog);
    panLayout->setObjectName(QStringLiteral("connectionProfileLayout"));
    panLayout->setAccessibleName(tr("Operator panadapter arrangement"));
    auto* lowBandwidth = new QCheckBox(tr("Use low bandwidth mode for this connection"), dialog);
    lowBandwidth->setChecked(original.value("lowBandwidth").toBool());
    if (collection == QLatin1String("connections") && !renameOnly) { form->addRow(lowBandwidth); }
    else { lowBandwidth->hide(); }
    if (collection == QLatin1String("operators")) {
        copyFrom->addItem(tr("App defaults"), ConnectionWorkspace::defaultOperatorPreferences());
        copyFrom->addItem(tr("Current audio and layout preferences"), ConnectionWorkspace::captureOperatorPreferences());
        for (const QJsonValue& value : m_workspace.value("operators").toArray()) {
            const QJsonObject profile = value.toObject();
            copyFrom->addItem(profile.value("name").toString(), profile.value("preferences").toObject());
        }
        auto populateDevices = [](QComboBox* combo, const QList<QAudioDevice>& devices) {
            combo->addItem(QObject::tr("System default"), QString());
            for (const QAudioDevice& device : devices) {
                combo->addItem(device.description(), QString::fromLatin1(device.id().toBase64()));
            }
        };
        populateDevices(input, QMediaDevices::audioInputs());
        populateDevices(output, QMediaDevices::audioOutputs());
        for (const QString& arrangement : {QStringLiteral("1"), QStringLiteral("2v"), QStringLiteral("2h"),
                QStringLiteral("2h1"), QStringLiteral("12h"), QStringLiteral("3v"), QStringLiteral("2x2"),
                QStringLiteral("4v"), QStringLiteral("3h2"), QStringLiteral("2x3"), QStringLiteral("4h3"), QStringLiteral("2x4")}) {
            panLayout->addItem(arrangement, arrangement);
        }
        const auto setPreferences = [input, output, panLayout](const QJsonObject& preferences) {
            for (const auto& pair : {qMakePair(input, QStringLiteral("inputDeviceId")), qMakePair(output, QStringLiteral("outputDeviceId"))}) {
                const QString deviceId = preferences.value(pair.second).toString();
                int index = pair.first->findData(deviceId);
                if (index < 0) {
                    pair.first->addItem(QObject::tr("Unavailable saved device — choose a replacement"), deviceId);
                    index = pair.first->count() - 1;
                }
                pair.first->setCurrentIndex(index);
            }
            panLayout->setCurrentIndex(qMax(0, panLayout->findData(preferences.value("layout").toString("1"))));
        };
        setPreferences(original.value("preferences").toObject().isEmpty()
            ? ConnectionWorkspace::captureOperatorPreferences() : original.value("preferences").toObject());
        copyFrom->setCurrentIndex(1);
        connect(copyFrom, &QComboBox::currentIndexChanged, dialog, [copyFrom, setPreferences](int) {
            setPreferences(copyFrom->currentData().toJsonObject());
        });
        if (newRecord) { form->addRow(tr("Start from:"), copyFrom); }
        else { copyFrom->hide(); }
        form->addRow(tr("Microphone:"), input);
        form->addRow(tr("Audio output:"), output);
        form->addRow(tr("Pan arrangement:"), panLayout);
    } else {
        for (QComboBox* widget : {copyFrom, input, output, panLayout}) { widget->hide(); }
    }
    for (QComboBox* combo : {copyFrom, input, output, panLayout}) { applyComboStyle(combo); }
    applyStyle(name, "QLineEdit { color: {{color.canon.ink}}; background: {{color.canon.control}};"
        " border: 1px solid {{color.canon.lineHi}}; border-radius: 4px; padding: 6px; }");
    applyStyle(email, "QLineEdit { color: {{color.canon.ink}}; background: {{color.canon.control}};"
        " border: 1px solid {{color.canon.lineHi}}; border-radius: 4px; padding: 6px; }");
    layout->addLayout(form);
    layout->addWidget(makeWrappedLabel(tr("Profiles save connection details and client preferences. Radio-owned operating state and account credentials are not copied."), kHintLabelStyle));
    auto* error = makeWrappedLabel(QString(), kErrorLabelStyle);
    error->setAccessibleName(tr("Profile validation error"));
    layout->addWidget(error);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, dialog);
    buttons->button(QDialogButtonBox::Save)->setObjectName(QStringLiteral("connectionProfileSave"));
    buttons->button(QDialogButtonBox::Save)->setAccessibleName(tr("Save connection profile"));
    buttons->button(QDialogButtonBox::Save)->setDefault(true);
    if (newRecord && collection == QLatin1String("operators")) {
        buttons->button(QDialogButtonBox::Save)->setText(tr("Create and use profile"));
    }
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    const QString stationId = m_workspace.value("selectedStation").toString();
    connect(buttons, &QDialogButtonBox::accepted, dialog,
        [this, dialog, collection, original, newRecord, duplicate, stationId, name, email, input, output, panLayout, lowBandwidth, error, renameOnly] {
        if (collection == QLatin1String("accounts") && newRecord && m_smartLink && m_smartLink->accountLocked()) {
            error->setText(tr("Disconnect the SmartLink radio before adding another account."));
            return;
        }
        QJsonObject next = m_workspace;
        QJsonArray current = next.value(collection).toArray();
        if (collection == QLatin1String("connections")) {
            current = ConnectionWorkspace::find(next.value("stations").toArray(), stationId).value("connections").toArray();
        }
        const QString existingId = newRecord ? QString() : original.value("id").toString();
        QJsonArray namePeers;
        for (const QJsonValue& value : current) {
            const QJsonObject peer = value.toObject();
            if (collection == QLatin1String("connections") && !original.value("serial").toString().isEmpty()
                && peer.value("serial") == original.value("serial") && peer.value("family") == original.value("family")) { continue; }
            namePeers.append(peer);
        }
        if (!ConnectionWorkspace::uniqueName(namePeers, name->text(), existingId)) {
            error->setText(tr("Choose a unique, nonempty name (up to 128 characters).")); return;
        }
        QJsonObject record = original;
        record["id"] = newRecord ? ConnectionWorkspace::newId() : existingId;
        record["name"] = name->text().trimmed();
        if (collection == QLatin1String("stations") && newRecord && !duplicate) { record["connections"] = QJsonArray(); }
        if (collection == QLatin1String("accounts")) {
            const QString address = email->text().trimmed();
            if ((newRecord || !address.isEmpty()) && (!address.contains('@') || address.size() > 254)) {
                error->setText(tr("Enter the account's email address.")); return;
            }
            record["email"] = address;
            if (newRecord) { record["id"] = ConnectionWorkspace::accountId(address); }
        }
        if (collection == QLatin1String("operators")) {
            record["preferences"] = QJsonObject{{"inputDeviceId", input->currentData().toString()},
                {"outputDeviceId", output->currentData().toString()}, {"layout", panLayout->currentData().toString()}};
        }
        if (collection == QLatin1String("connections")) { record["lowBandwidth"] = lowBandwidth->isChecked(); }
        bool replaced = false;
        for (qsizetype i = 0; i < current.size(); ++i) {
            if (!newRecord && current[i].toObject().value("id").toString() == existingId) { current[i] = record; replaced = true; }
            else if (renameOnly && collection == QLatin1String("connections") && !original.value("serial").toString().isEmpty()
                && current[i].toObject().value("serial") == original.value("serial")
                && current[i].toObject().value("family") == original.value("family")) {
                QJsonObject path = current[i].toObject(); path["name"] = record.value("name"); current[i] = path;
            }
        }
        if (!replaced) { current.append(record); }
        if (collection == QLatin1String("connections")) {
            QJsonArray stations = next.value("stations").toArray();
            for (qsizetype i = 0; i < stations.size(); ++i) {
                QJsonObject station = stations[i].toObject();
                if (station.value("id").toString() == stationId) { station["connections"] = current; stations[i] = station; }
            }
            next["stations"] = stations;
        } else {
            next[collection] = current;
            if (collection != QLatin1String("accounts") || newRecord) {
                next[collection == QLatin1String("stations") ? "selectedStation" : collection == QLatin1String("operators") ? "selectedOperator" : "selectedAccount"] = record.value("id");
            }
        }
        if (persistWorkspace(next)) {
            if (collection == QLatin1String("accounts") && newRecord) { activateWorkspaceAccount(record.value("id").toString()); }
            dialog->accept();
        } else { error->setText(tr("The profile could not be saved. See the connection status.")); }
    });
    dialog->resize(530, collection == QLatin1String("operators") ? 410 : 250);
    dialog->show();
    name->setFocus();
}

void ConnectionPanel::showWorkspaceMenu(bool location, const QPoint& position)
{
    const QString id = location ? m_workspace.value("selectedStation").toString()
        : m_workspacePath->currentData().toJsonObject().value("id").toString();
    if (id.isEmpty()) { return; }
    auto* menu = new QMenu(this);
    menu->setObjectName(location ? QStringLiteral("connectionLocationMenu") : QStringLiteral("connectionRadioMenu"));
    applyCanonSheet(menu, [] { return QString::fromLatin1(kConnectionStyle); });
    connect(menu, &QMenu::aboutToHide, menu, &QObject::deleteLater);
    const QString collection = location ? QStringLiteral("stations") : QStringLiteral("connections");
    const bool saved = location || !m_workspacePath->currentData().toJsonObject().value("nearby").toBool();
    auto* edit = menu->addAction(tr("Edit"));
    edit->setObjectName(location ? QStringLiteral("connectionLocationEdit") : QStringLiteral("connectionRadioEdit"));
    edit->setEnabled(saved);
    edit->setStatusTip(saved ? tr("Edit saved settings.") : tr("Save this radio to a location first."));
    connect(edit, &QAction::triggered, this, [this, location, id] {
        if (location) { editWorkspaceProfile("stations", id); }
        else { findChild<QPushButton*>("connectionEditSaved")->click(); }
    });
    if (location) {
        auto* move = menu->addMenu(tr("Move"));
        const QJsonArray locations = m_workspace.value("stations").toArray();
        int current = -1;
        for (qsizetype i = 0; i < locations.size(); ++i) {
            if (locations[i].toObject().value("id").toString() == id) { current = static_cast<int>(i); }
        }
        for (int offset : {-1, 1}) {
            auto* action = move->addAction(offset < 0 ? tr("Move up") : tr("Move down"));
            action->setEnabled(current + offset >= 0 && current + offset < locations.size());
            action->setStatusTip(tr("Reorder locations in the list."));
            connect(action, &QAction::triggered, this, [this, id, offset] {
                QJsonObject next = m_workspace;
                QJsonArray records = next.value("stations").toArray();
                for (qsizetype i = 0; i < records.size(); ++i) {
                    if (records[i].toObject().value("id").toString() == id && i + offset >= 0 && i + offset < records.size()) {
                        const QJsonValue record = records[i];
                        records.removeAt(i);
                        records.insert(i + offset, record);
                        next["stations"] = records;
                        persistWorkspace(next);
                        break;
                    }
                }
            });
        }
    } else {
        auto* move = menu->addAction(tr("Move"));
        move->setObjectName(QStringLiteral("connectionRadioMove"));
        move->setEnabled(saved && m_workspace.value("stations").toArray().size() > 1);
        move->setStatusTip(tr("Move this radio and its saved paths to another location."));
        connect(move, &QAction::triggered, this, &ConnectionPanel::moveWorkspaceRadio);
    }
    auto* rename = menu->addAction(tr("Rename"));
    rename->setObjectName(location ? QStringLiteral("connectionLocationRename") : QStringLiteral("connectionRadioRename"));
    rename->setEnabled(saved);
    rename->setStatusTip(saved ? tr("Change the saved display name.") : tr("Save this radio to a location first."));
    connect(rename, &QAction::triggered, this, [this, collection, id] { editWorkspaceProfile(collection, id, false, true); });
    menu->addSeparator();
    auto* remove = menu->addAction(tr("Remove"));
    remove->setObjectName(location ? QStringLiteral("connectionLocationRemove") : QStringLiteral("connectionRadioRemove"));
    remove->setEnabled(saved);
    remove->setStatusTip(tr("Ask for confirmation before removing saved entries."));
    connect(remove, &QAction::triggered, this, [this, collection, id] { removeWorkspaceProfile(collection, id); });
    menu->popup(position);
}

void ConnectionPanel::moveWorkspaceRadio()
{
    const QString sourceId = m_workspace.value("selectedStation").toString();
    const QJsonArray paths = m_workspaceRadioList->currentItem()
        ? m_workspaceRadioList->currentItem()->data(Qt::UserRole + 1).toJsonArray() : QJsonArray();
    if (paths.isEmpty()) { return; }
    auto* dialog = new CanonWindow(tr("Move radio"), this);
    dialog->setObjectName(QStringLiteral("connectionMoveRadio"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    applyCanonSheet(dialog->bodyWidget(), [] { return QString::fromLatin1(kConnectionStyle); });
    auto* layout = new QVBoxLayout(dialog->bodyWidget());
    layout->addWidget(makeCanonHeader(tr("Move radio")));
    layout->addWidget(makeWrappedLabel(tr("Move this radio and its saved connection paths to another location.")));
    auto* destination = new QComboBox(dialog);
    destination->setObjectName(QStringLiteral("connectionMoveDestination"));
    destination->setAccessibleName(tr("Destination location"));
    applyComboStyle(destination);
    for (const QJsonValue& value : m_workspace.value("stations").toArray()) {
        const QJsonObject record = value.toObject();
        if (record.value("id").toString() != sourceId) { destination->addItem(record.value("name").toString(), record.value("id")); }
    }
    layout->addWidget(destination);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Move"));
    buttons->button(QDialogButtonBox::Ok)->setObjectName(QStringLiteral("connectionConfirmMove"));
    buttons->button(QDialogButtonBox::Ok)->setDefault(true);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, dialog, [this, dialog, sourceId, paths, destination] {
        const QString targetId = destination->currentData().toString();
        if (targetId.isEmpty() || ConnectionWorkspace::find(m_workspace.value("stations").toArray(), targetId).isEmpty()) { return; }
        QSet<QString> ids;
        for (const QJsonValue& value : paths) { ids.insert(value.toObject().value("id").toString()); }
        QJsonObject next = m_workspace;
        QJsonArray records = next.value("stations").toArray();
        QJsonArray moved;
        for (qsizetype i = 0; i < records.size(); ++i) {
            QJsonObject record = records[i].toObject();
            if (record.value("id").toString() != sourceId) { continue; }
            QJsonArray remaining;
            for (const QJsonValue& value : record.value("connections").toArray()) {
                if (ids.contains(value.toObject().value("id").toString())) { moved.append(value); }
                else { remaining.append(value); }
            }
            record["connections"] = remaining;
            records[i] = record;
        }
        if (moved.isEmpty()) { dialog->reject(); return; }
        for (qsizetype i = 0; i < records.size(); ++i) {
            QJsonObject record = records[i].toObject();
            if (record.value("id").toString() != targetId) { continue; }
            QJsonArray connections = record.value("connections").toArray();
            for (const QJsonValue& value : moved) { connections.append(value); }
            record["connections"] = connections;
            records[i] = record;
        }
        next["stations"] = records;
        next["selectedStation"] = targetId;
        if (persistWorkspace(next)) { dialog->accept(); }
    });
    dialog->resize(440, 230);
    dialog->show();
}

void ConnectionPanel::manageOperatorProfiles()
{
    auto* dialog = new CanonWindow(tr("Operator profiles"), this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setObjectName(QStringLiteral("connectionOperatorProfiles"));
    theme::setContainer(dialog, QStringLiteral("canon/connectionProfiles"));
    applyCanonSheet(dialog->bodyWidget(), [] { return QString::fromLatin1(kConnectionStyle) + canonIndicatorRules(); });
    auto* layout = new QVBoxLayout(dialog->bodyWidget());
    layout->addWidget(makeCanonHeader(tr("Operator profiles")));
    layout->addWidget(makeWrappedLabel(tr("Select audio and panadapter arrangement for the next connection. This does not change a live radio."), kHintLabelStyle));
    auto* list = new QListWidget(dialog);
    list->setObjectName(QStringLiteral("connectionOperatorProfileList"));
    list->setAccessibleName(tr("Saved operator profiles"));
    layout->addWidget(list);
    const auto refill = [this, list] {
        list->clear();
        for (const QJsonValue& value : m_workspace.value("operators").toArray()) {
            const QJsonObject profile = value.toObject();
            const QJsonObject preferences = profile.value("preferences").toObject();
            const QString current = profile.value("id") == m_workspace.value("selectedOperator") ? tr(" · Selected") : QString();
            const QString audio = preferences.value("inputDeviceId").toString().isEmpty()
                && preferences.value("outputDeviceId").toString().isEmpty() ? tr("System audio") : tr("Saved audio devices");
            auto* item = new QListWidgetItem(tr("%1%2\n%3 · Pan arrangement %4").arg(profile.value("name").toString(), current, audio,
                preferences.value("layout").toString()), list);
            item->setData(Qt::UserRole, profile.value("id").toString());
            if (profile.value("id") == m_workspace.value("selectedOperator")) { list->setCurrentItem(item); }
        }
    };
    connect(this, &ConnectionPanel::workspaceChanged, dialog, refill);
    refill();
    auto* actions = new QHBoxLayout;
    for (const QString& action : {QStringLiteral("Create"), QStringLiteral("Edit"), QStringLiteral("Duplicate"), QStringLiteral("Remove"), QStringLiteral("Use profile")}) {
        auto* button = new QPushButton(action, dialog);
        button->setObjectName(QStringLiteral("connectionOperator") + QString(action).remove(' '));
        actions->addWidget(button);
        connect(button, &QPushButton::clicked, dialog, [this, dialog, list, action] {
            const QString id = list->currentItem() ? list->currentItem()->data(Qt::UserRole).toString() : QString();
            if (action == QLatin1String("Create")) { editWorkspaceProfile("operators"); }
            else if (id.isEmpty()) { return; }
            else if (action == QLatin1String("Remove")) { removeWorkspaceProfile("operators", id); }
            else if (action == QLatin1String("Use profile")) { if (selectWorkspaceProfile("operators", id)) { dialog->accept(); } }
            else { editWorkspaceProfile("operators", id, action == QLatin1String("Duplicate")); }
        });
    }
    layout->addLayout(actions);
    dialog->resize(610, 360);
    dialog->show();
}

void ConnectionPanel::removeWorkspaceProfile(const QString& collection, const QString& id, bool confirmed)
{
    if (id.isEmpty()) { return; }
    if (collection == QLatin1String("accounts") && m_smartLink && m_smartLink->accountLocked()) {
        setStatusText(tr("Disconnect the SmartLink radio before removing an account.")); return;
    }
    QJsonObject next = m_workspace;
    QJsonArray records = next.value(collection).toArray();
    const QString stationId = next.value("selectedStation").toString();
    if (collection == QLatin1String("connections")) {
        records = ConnectionWorkspace::find(next.value("stations").toArray(), stationId).value("connections").toArray();
    }
    if (collection != QLatin1String("connections") && collection != QLatin1String("accounts") && records.size() <= 1) {
        setStatusText(tr("Keep at least one location and one operator profile.")); return;
    }
    const QJsonObject original = ConnectionWorkspace::find(records, id);
    if (original.isEmpty()) { return; }
    if (!confirmed) {
        auto* dialog = new CanonWindow(tr("Confirm removal"), this);
        dialog->setObjectName(QStringLiteral("connectionRemoveConfirmation"));
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        applyCanonSheet(dialog->bodyWidget(), [] { return QString::fromLatin1(kConnectionStyle); });
        auto* layout = new QVBoxLayout(dialog->bodyWidget());
        layout->addWidget(makeCanonHeader(tr("Confirm removal")));
        const QString explanation = collection == QLatin1String("stations")
            ? tr("Remove “%1” and its saved connections? Physical radios and shared account credentials are kept.")
            : collection == QLatin1String("connections")
                ? tr("Remove “%1” and its saved paths from this location? The physical radio and account credentials are kept.")
                : tr("Remove “%1”? This cannot be undone.");
        layout->addWidget(makeWrappedLabel(explanation.arg(original.value("name").toString())));
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, dialog);
        auto* remove = buttons->addButton(tr("Remove"), QDialogButtonBox::DestructiveRole);
        remove->setObjectName(QStringLiteral("connectionConfirmRemove"));
        remove->setAutoDefault(false);
        buttons->button(QDialogButtonBox::Cancel)->setDefault(true);
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
        connect(remove, &QPushButton::clicked, dialog, [this, dialog, collection, id, stationId] {
            if (collection == QLatin1String("connections") && m_workspace.value("selectedStation").toString() != stationId) {
                setStatusText(tr("The selected location changed. Choose the radio again before removing it."));
            } else { removeWorkspaceProfile(collection, id, true); }
            dialog->accept();
        });
        dialog->resize(450, 220);
        dialog->show();
        return;
    }
    QJsonArray updated;
    const QString serial = original.value("serial").toString();
    for (const QJsonValue& value : records) {
        const QJsonObject record = value.toObject();
        const bool sameRadio = collection == QLatin1String("connections") && !serial.isEmpty()
            && record.value("serial").toString() == serial && record.value("family") == original.value("family");
        if (record.value("id").toString() != id && !sameRadio) { updated.append(value); }
    }
    if (updated.size() == records.size()) { return; }
    const QString selectedKey = collection == QLatin1String("stations") ? "selectedStation"
        : collection == QLatin1String("operators") ? "selectedOperator" : "selectedAccount";
    if (collection == QLatin1String("operators") && next.value(selectedKey).toString() == id) {
        setStatusText(tr("Select another operator profile before removing the current one.")); return;
    }
    if (collection == QLatin1String("connections")) {
        QJsonArray stations = next.value("stations").toArray();
        for (qsizetype i = 0; i < stations.size(); ++i) {
            QJsonObject station = stations[i].toObject();
            if (station.value("id").toString() == stationId) { station["connections"] = updated; stations[i] = station; }
        }
        next["stations"] = stations;
    } else {
        next[collection] = updated;
        if (next.value(selectedKey).toString() == id) { next[selectedKey] = updated.isEmpty() ? QString() : updated.first().toObject().value("id").toString(); }
    }
    if (!persistWorkspace(next)) { return; }
    if (collection == QLatin1String("accounts") && m_smartLink) {
        m_smartLink->setAccountId(id);
        m_smartLink->logout();
        m_wanRadios.clear();
        m_wanList->clear();
        emit smartLinkSignedOut();
        activateWorkspaceAccount(next.value("selectedAccount").toString());
        updateSmartLinkUi();
    }
}

} // namespace AetherSDR
