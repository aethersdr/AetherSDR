#include "TailnetShimDialog.h"

#include "core/TailnetShimTokenStore.h"
#include "core/ThemeManager.h"
#include "models/RadioModel.h"

#include <QCheckBox>
#include <QClipboard>
#include <QFrame>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>

namespace AetherSDR {

namespace {

// AetherSDR style guide (docs/style/aethersdr-style-guide.md, RFC #6226),
// modelled on the About window: a CanonWindow (ambient ground, rounded
// corners, corner close), nested 12 px cards with a hairline, keys in muted,
// values in ink-soft, canon secondary controls, and Done as the brand-gradient
// primary like About's OK. State colours always travel with a word. Rules are
// scoped by object name or the tsRole property so the card rule never reaches
// the QLabels inside it (QLabel is a QFrame).
constexpr const char* kTailnetDialogStyle =
    "QWidget#tailnetBody { background: transparent; }"
    "QLabel { color: {{color.canon.inkSoft}}; background: transparent; border: none; font-size: 12px; }"
    "QLabel#tsTitle { color: {{color.canon.ink}}; font-size: 20px; font-weight: bold; }"
    "QLabel#tsStatusLine { color: {{color.canon.muted}}; font-size: 13px; }"
    "QLabel#tsStatusDot { background: {{color.canon.muted}}; border-radius: 4px;"
    " min-width: 8px; max-width: 8px; min-height: 8px; max-height: 8px; }"
    "QLabel#tsStatusDot[state=\"running\"] { background: {{color.canon.cyan}}; }"
    "QLabel#tsStatusDot[state=\"starting\"] { background: {{color.accent.warning}}; }"
    "QLabel#tsStatusDot[state=\"error\"] { background: {{color.accent.danger}}; }"
    "QFrame#tsRule { background: {{color.canon.line}}; border: none; min-height: 1px; max-height: 1px; }"
    "QFrame[tsRole=\"card\"] { background: {{color.canon.nested}}; border: 1px solid {{color.canon.line}};"
    " border-radius: 12px; }"
    "QLabel[tsRole=\"cardTitle\"] { color: {{color.canon.ink}}; font-size: 13px; font-weight: bold; }"
    "QLabel[tsRole=\"key\"] { color: {{color.canon.muted}}; font-size: 10px; font-weight: bold; }"
    "QLabel[tsRole=\"value\"] { color: {{color.canon.inkSoft}}; font-size: 12px; }"
    "QLabel[tsRole=\"hint\"] { color: {{color.canon.muted}}; font-size: 11px; }"
    "QLabel[tsRole=\"message\"][tone=\"error\"] { color: {{color.accent.danger}}; }"
    "QLabel[tsRole=\"message\"][tone=\"ok\"] { color: {{color.accent.success}}; }"
    "QLabel[tsRole=\"message\"][tone=\"warn\"] { color: {{color.accent.warning}}; }"
    "QLineEdit { background: {{color.canon.control}}; color: {{color.canon.ink}};"
    " border: 1px solid {{color.canon.lineHi}}; border-radius: 4px; padding: 5px 8px; font-size: 12px;"
    " selection-background-color: {{color.canon.cyan}}; selection-color: {{color.canon.onAccent}}; }"
    "QLineEdit:focus { border-color: {{color.canon.aqua}}; }"
    "QLineEdit:disabled { color: {{color.canon.muted}}; border-color: {{color.canon.line}}; }"
    "QCheckBox { color: {{color.canon.inkSoft}}; background: transparent; font-size: 12px; spacing: 8px; }"
    "QCheckBox:disabled { color: {{color.canon.muted}}; }"
    "QPushButton { background: {{color.canon.control}}; color: {{color.canon.cyan}};"
    " border: 1px solid {{color.canon.lineHi}}; border-radius: 4px; padding: 6px 16px;"
    " font-size: 12px; font-weight: bold; }"
    "QPushButton:hover { background: {{color.canon.nested}}; color: {{color.canon.aqua}}; }"
    "QPushButton:focus { border-color: {{color.canon.aqua}}; }"
    "QPushButton:disabled { background: transparent; color: {{color.canon.muted}};"
    " border-color: {{color.canon.line}}; }"
    // Done: the primary action, in the brand gradient, exactly as About's OK.
    "QPushButton#tsDone { background: {{color.brand.gradient}}; color: {{color.canon.onAccent}};"
    " border: 1px solid transparent; border-radius: 4px; padding: 6px 30px; }"
    "QPushButton#tsDone:hover { border-color: {{color.canon.aqua}}; }"
    "QPushButton#tsDone:focus { border: 2px solid {{color.canon.aqua}}; padding: 5px 29px; }";

QString stateText(const TailnetShimStatus& st)
{
    const QString state = st.state;
    if (state == QLatin1String("running")) {
        return QObject::tr("On your tailnet");
    }
    if (state == QLatin1String("starting")) {
        return QObject::tr("Joining the tailnet…");
    }
    if (state == QLatin1String("unprovisioned")) {
        return QObject::tr("Not set up");
    }
    if (state == QLatin1String("error")) {
        return QObject::tr("Problem");
    }
    return state;
}

void repolish(QWidget* w)
{
    w->style()->unpolish(w);
    w->style()->polish(w);
}

QLabel* roleLabel(const QString& text, const char* role, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setProperty("tsRole", QString::fromLatin1(role));
    return label;
}

// A canon card: nested surface, hairline border, 12 px radius, with a title.
QFrame* makeCard(const QString& title, QWidget* parent, QVBoxLayout** contents)
{
    auto* card = new QFrame(parent);
    card->setProperty("tsRole", QStringLiteral("card"));
    card->setAccessibleName(title);
    auto* v = new QVBoxLayout(card);
    v->setContentsMargins(16, 14, 16, 16);
    v->setSpacing(10);
    v->addWidget(roleLabel(title, "cardTitle", card));
    *contents = v;
    return card;
}

}  // namespace

TailnetShimDialog::TailnetShimDialog(RadioModel* model, QWidget* parent)
    : CanonWindow(tr("Remote Access (Tailscale)"), parent)
    , m_model(model)
{
    constexpr int kWidth = 600;
    constexpr int kSide = 28;          // body side margins, as About's 24 plus room for cards
    constexpr int kCardPad = 16;       // makeCard() side padding
    setFixedWidth(kWidth);
    m_bodyTextWidth = kWidth - 2 * CanonWindow::kInset - 2 * kSide;
    m_cardTextWidth = m_bodyTextWidth - 2 * kCardPad - 2;   // less the card's hairline
    bodyWidget()->setObjectName(QStringLiteral("tailnetBody"));
    ThemeManager::instance().applyStyleSheet(bodyWidget(), kTailnetDialogStyle);

    auto* layout = new QVBoxLayout(bodyWidget());
    layout->setContentsMargins(kSide, 26, kSide, 22);
    layout->setSpacing(14);
    QWidget* body = bodyWidget();

    // Header: title, then a status line led by a dot (the dot never stands
    // alone: the line always names the state), then a hairline.
    auto* title = new QLabel(tr("Remote Access"), body);
    title->setObjectName(QStringLiteral("tsTitle"));
    title->setAlignment(Qt::AlignCenter);
    title->setAccessibleName(tr("Remote access over Tailscale"));
    layout->addWidget(title);
    auto* statusRow = new QHBoxLayout;
    statusRow->setSpacing(8);
    statusRow->addStretch(1);
    m_statusDot = new QLabel(body);
    m_statusDot->setObjectName(QStringLiteral("tsStatusDot"));
    m_statusDot->setAccessibleName(tr("Remote access status indicator"));
    m_stateLabel = new QLabel(tr("Checking…"), body);
    m_stateLabel->setObjectName(QStringLiteral("tsStatusLine"));
    m_stateLabel->setAccessibleName(tr("Remote access status"));
    statusRow->addWidget(m_statusDot, 0, Qt::AlignVCenter);
    statusRow->addWidget(m_stateLabel, 0);
    statusRow->addStretch(1);
    layout->addLayout(statusRow);
    auto* rule = new QFrame(body);
    rule->setObjectName(QStringLiteral("tsRule"));
    layout->addWidget(rule);

    auto* intro = roleLabel(QString(), "value", body);
    intro->setAlignment(Qt::AlignCenter);
    intro->setAccessibleName(tr("About remote access over Tailscale"));
    setWrapped(intro,
        tr("This radio container puts the radio on your Tailscale network, so AetherSDR can "
           "reach it from anywhere, including behind CGNAT. Set it up here while you are on the "
           "radio's local network, then connect remotely with Connect → Manual and the tailnet "
           "address below."),
        m_bodyTextWidth);
    layout->addWidget(intro);

    // Card: the node on the tailnet.
    QVBoxLayout* tailnet = nullptr;
    layout->addWidget(makeCard(tr("Tailnet"), body, &tailnet));
    auto* grid = new QGridLayout;
    grid->setHorizontalSpacing(16);
    grid->setVerticalSpacing(8);
    grid->setColumnStretch(1, 1);
    auto addRow = [&](int row, const QString& key, QWidget* value, QWidget* trailing = nullptr) {
        grid->addWidget(roleLabel(key, "key", body), row, 0, Qt::AlignLeft | Qt::AlignVCenter);
        grid->addWidget(value, row, 1);
        if (trailing) {
            grid->addWidget(trailing, row, 2);
        }
    };
    m_nameLabel = roleLabel(QStringLiteral("—"), "value", body);
    m_nameLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_nameLabel->setAccessibleName(tr("Tailnet name"));
    m_addressLabel = roleLabel(QStringLiteral("—"), "value", body);
    m_addressLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_addressLabel->setAccessibleName(tr("Tailnet address"));
    m_copyButton = new QPushButton(tr("Copy"), body);
    m_copyButton->setAccessibleName(tr("Copy tailnet address"));
    connect(m_copyButton, &QPushButton::clicked, this, [this] {
        if (m_status && !m_status->tailnetIp.isEmpty()) {
            QGuiApplication::clipboard()->setText(m_status->tailnetIp);
        }
    });
    m_sessionsLabel = roleLabel(QStringLiteral("—"), "value", body);
    m_sessionsLabel->setAccessibleName(tr("Remote sessions"));
    m_versionLabel = roleLabel(QStringLiteral("—"), "value", body);
    m_versionLabel->setAccessibleName(tr("Container version"));
    addRow(0, tr("TAILNET NAME"), m_nameLabel);
    addRow(1, tr("TAILNET ADDRESS"), m_addressLabel, m_copyButton);
    addRow(2, tr("REMOTE SESSIONS"), m_sessionsLabel);
    addRow(3, tr("CONTAINER VERSION"), m_versionLabel);
    tailnet->addLayout(grid);

    // Card: joining, or changing the key, and who may connect.
    QVBoxLayout* join = nullptr;
    layout->addWidget(makeCard(tr("Join your tailnet"), body, &join));
    auto* joinGrid = new QGridLayout;
    joinGrid->setHorizontalSpacing(16);
    joinGrid->setVerticalSpacing(8);
    joinGrid->setColumnStretch(1, 1);
    m_keyEdit = new QLineEdit(body);
    m_keyEdit->setEchoMode(QLineEdit::Password);
    m_keyEdit->setPlaceholderText(QStringLiteral("tskey-auth-…"));
    m_keyEdit->setAccessibleName(tr("Tailscale auth key"));
    m_keyEdit->setAccessibleDescription(
        tr("A single-use key from the Tailscale admin console. It is sent to the radio once "
           "and is not stored."));
    connect(m_keyEdit, &QLineEdit::textChanged, this, &TailnetShimDialog::updateControls);
    auto* keyHint = roleLabel(QString(), "hint", body);
    setWrapped(keyHint,
        tr("Create a single-use key in the Tailscale admin console under Settings → Keys. "
           "AetherSDR sends it to the radio once and does not keep it. Leave Who may connect "
           "empty to allow anyone on your tailnet."),
        m_cardTextWidth);
    m_hostnameEdit = new QLineEdit(body);
    m_hostnameEdit->setAccessibleName(tr("Tailnet machine name"));
    m_hostnameEdit->setMaxLength(63);
    m_allowEdit = new QLineEdit(body);
    m_allowEdit->setPlaceholderText(tr("you@example.com, tag:operators"));
    m_allowEdit->setAccessibleName(tr("Who may connect"));
    m_allowEdit->setAccessibleDescription(
        tr("Tailnet logins or tags allowed to connect, separated by commas. Leave empty to "
           "allow anyone on your tailnet."));
    joinGrid->addWidget(roleLabel(tr("AUTH KEY"), "key", body), 0, 0);
    joinGrid->addWidget(m_keyEdit, 0, 1);
    joinGrid->addWidget(roleLabel(tr("MACHINE NAME"), "key", body), 1, 0);
    joinGrid->addWidget(m_hostnameEdit, 1, 1);
    joinGrid->addWidget(roleLabel(tr("WHO MAY CONNECT"), "key", body), 2, 0);
    joinGrid->addWidget(m_allowEdit, 2, 1);
    join->addLayout(joinGrid);
    join->addWidget(keyHint);
    auto* joinButtons = new QHBoxLayout;
    joinButtons->addStretch(1);
    m_saveAllowButton = new QPushButton(tr("Save Access List"), body);
    m_saveAllowButton->setAccessibleName(tr("Save who may connect"));
    m_joinButton = new QPushButton(tr("Join Tailnet"), body);
    m_joinButton->setAccessibleName(tr("Join the tailnet with this auth key"));
    m_joinButton->setDefault(true);
    joinButtons->addWidget(m_saveAllowButton);
    joinButtons->addWidget(m_joinButton);
    join->addLayout(joinButtons);

    // Card: station devices (4O3A accessories) shared as subnet routes.
    QVBoxLayout* devices = nullptr;
    layout->addWidget(makeCard(tr("Station devices"), body, &devices));
    m_devicesLabel = roleLabel(QString(), "value", body);
    setWrapped(m_devicesLabel,
               tr("Looking for Antenna Genius, Power Genius XL and Tuner Genius XL…"),
               m_cardTextWidth);
    m_devicesLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_devicesLabel->setAccessibleName(tr("Station devices found on the radio's network"));
    devices->addWidget(m_devicesLabel);
    m_shareDiscoveredCheck = new QCheckBox(tr("Share the devices found here over the tailnet"), body);
    m_shareDiscoveredCheck->setChecked(true);
    m_shareDiscoveredCheck->setAccessibleName(tr("Share discovered station devices over the tailnet"));
    connect(m_shareDiscoveredCheck, &QCheckBox::toggled, this, &TailnetShimDialog::updateControls);
    devices->addWidget(m_shareDiscoveredCheck);
    auto* otherRow = new QGridLayout;
    otherRow->setHorizontalSpacing(16);
    otherRow->setColumnStretch(1, 1);
    m_extraDevicesEdit = new QLineEdit(body);
    m_extraDevicesEdit->setPlaceholderText(tr("192.168.1.40, 192.168.1.41"));
    m_extraDevicesEdit->setAccessibleName(tr("Other LAN devices to share"));
    m_extraDevicesEdit->setAccessibleDescription(
        tr("Local network addresses of other devices to reach over the tailnet, separated by commas."));
    otherRow->addWidget(roleLabel(tr("OTHER DEVICES"), "key", body), 0, 0);
    otherRow->addWidget(m_extraDevicesEdit, 0, 1);
    devices->addLayout(otherRow);
    m_sharingNote = roleLabel(QString(), "hint", body);
    m_sharingNote->setAccessibleName(tr("Device sharing status"));
    devices->addWidget(m_sharingNote);
    auto* sharingButtons = new QHBoxLayout;
    sharingButtons->addStretch(1);
    m_saveSharingButton = new QPushButton(tr("Save Sharing"), body);
    m_saveSharingButton->setAccessibleName(tr("Save which station devices are shared"));
    sharingButtons->addWidget(m_saveSharingButton);
    devices->addLayout(sharingButtons);

    // Messages (always worded; colour only reinforces).
    m_messageLabel = roleLabel(QString(), "message", body);
    m_messageLabel->setAccessibleName(tr("Remote access message"));
    m_messageLabel->hide();
    layout->addWidget(m_messageLabel);
    m_tokenNote = roleLabel(QString(), "message", body);
    m_tokenNote->setProperty("tone", QStringLiteral("warn"));
    m_tokenNote->setAccessibleName(tr("Admin token note"));
    m_tokenNote->hide();
    layout->addWidget(m_tokenNote);

    auto* footer = new QHBoxLayout;
    m_refreshButton = new QPushButton(tr("Refresh"), body);
    m_refreshButton->setAccessibleName(tr("Refresh remote access status"));
    m_signOutButton = new QPushButton(tr("Sign Out of Tailnet"), body);
    m_signOutButton->setAccessibleName(tr("Sign the radio out of the tailnet"));
    auto* doneButton = new QPushButton(tr("Done"), body);
    doneButton->setObjectName(QStringLiteral("tsDone"));
    doneButton->setAccessibleName(tr("Close remote access settings"));
    footer->addWidget(m_refreshButton);
    footer->addWidget(m_signOutButton);
    footer->addStretch(1);
    footer->addWidget(doneButton);
    layout->addSpacing(4);
    layout->addLayout(footer);

    connect(doneButton, &QPushButton::clicked, this, &QDialog::close);
    connect(m_refreshButton, &QPushButton::clicked, this, &TailnetShimDialog::refresh);
    connect(m_joinButton, &QPushButton::clicked, this, &TailnetShimDialog::join);
    connect(m_saveAllowButton, &QPushButton::clicked, this, &TailnetShimDialog::saveAllowList);
    connect(m_saveSharingButton, &QPushButton::clicked, this, &TailnetShimDialog::saveSharing);
    connect(m_signOutButton, &QPushButton::clicked, this, &TailnetShimDialog::signOut);

    connect(&m_client, &TailnetShimClient::statusReceived, this,
            [this](const TailnetShimStatus& st) {
        setBusy(false);
        showStatus(st);
    });
    connect(&m_client, &TailnetShimClient::requestFailed,
            this, &TailnetShimDialog::onRequestFailed);

    m_pollTimer = new QTimer(this);
    m_pollTimer->setInterval(3000);
    connect(m_pollTimer, &QTimer::timeout, this, [this] {
        if (!m_busy) {
            m_client.fetchStatus();
        }
    });

    // Status, errors and device names come from whatever answers on the
    // radio's LAN address: never let Qt read them as rich text.
    for (QLabel* label : findChildren<QLabel*>()) {
        label->setTextFormat(Qt::PlainText);
    }

    // Reachability: the provisioning API listens only on the radio's LAN
    // address. A radio reached over the tailnet itself (or any routed path
    // that isn't a private LAN address) can't be configured from here.
    const QHostAddress address = m_model ? m_model->radioAddress() : QHostAddress();
    const RadioInfo info = m_model ? m_model->lastRadioInfo() : RadioInfo{};
    m_lanReachable = m_model && m_model->isConnected() && !address.isNull()
        && address.protocol() == QAbstractSocket::IPv4Protocol
        && (address.isPrivateUse() || address.isLinkLocal());
    // Key the admin token by the radio's own serial (from its `info` reply):
    // the discovery serial is the IP address for a manual/routed connection,
    // which would make one radio look like two.
    m_radioSerial = m_model && !m_model->chassisSerial().trimmed().isEmpty()
        ? m_model->chassisSerial().trimmed()
        : info.serial;
    m_hostnameEdit->setText(tailnetshim::suggestedHostname(
        m_model ? m_model->nickname() : QString()));

    if (!m_lanReachable) {
        setStatusLine(tr("Unavailable"), QStringLiteral("idle"));
        showError(tr("Connect to this radio over its local network to set up remote access. "
                     "The container's settings can't be changed over the tailnet."));
        updateControls();
        return;
    }

    m_apiAddress = address;
    m_client.setRadioAddress(address);
    // The window is for the radio it opened on. If that connection goes
    // (or is replaced by another radio, which disconnects first), stop
    // offering changes rather than send them to whatever is there now. A
    // join already under way still finishes and saves its token.
    connect(m_model, &RadioModel::connectionStateChanged, this, [this](bool connected) {
        if (connected || !m_lanReachable) {
            return;
        }
        m_lanReachable = false;
        m_pollTimer->stop();
        setStatusLine(tr("Unavailable"), QStringLiteral("idle"));
        showError(tr("The radio disconnected. Reopen this window once it is connected again."));
        updateControls();
    });
    TailnetShimTokenStore::load(m_radioSerial, this, [this](const QString& token) {
        m_adminToken = token;
        m_tokenLoaded = true;
        updateControls();
    });
    refresh();
}

void TailnetShimDialog::setWrapped(QLabel* label, const QString& text, int width)
{
    label->setWordWrap(true);
    label->setText(text);
    label->ensurePolished();
    label->setMinimumHeight(text.isEmpty() ? 0 : label->heightForWidth(width));
    if (isVisible()) {
        adjustSize();
    }
}

void TailnetShimDialog::setStatusLine(const QString& text, const QString& state)
{
    m_stateLabel->setText(text);
    m_statusDot->setProperty("state", state);
    repolish(m_statusDot);
}

void TailnetShimDialog::showMessage(const QString& message, const QString& tone)
{
    m_messageLabel->setProperty("tone", tone);
    repolish(m_messageLabel);
    m_messageLabel->show();
    setWrapped(m_messageLabel, message, m_bodyTextWidth);
}

void TailnetShimDialog::refresh()
{
    if (!m_lanReachable) {
        return;
    }
    setBusy(true, tr("Checking…"));
    m_client.fetchStatus();
}

void TailnetShimDialog::setBusy(bool busy, const QString& what)
{
    m_busy = busy;
    if (busy && !what.isEmpty()) {
        setStatusLine(what, QStringLiteral("starting"));
    }
    updateControls();
}

void TailnetShimDialog::showError(const QString& message)
{
    showMessage(tr("Problem: %1").arg(message), QStringLiteral("error"));
}

void TailnetShimDialog::showStatus(const TailnetShimStatus& st)
{
    m_status = st;
    QString line = stateText(st);
    if (!st.version.isEmpty()) {
        line += QStringLiteral(" · ") + tr("container %1").arg(st.version);
    }
    if (st.state == QLatin1String("running")) {
        line += QStringLiteral(" · ") + tr("%n remote session(s)", "", st.sessions);
    }
    setStatusLine(line, st.state);
    m_nameLabel->setText(st.dnsName.isEmpty() ? QStringLiteral("—") : st.dnsName);
    m_addressLabel->setText(st.tailnetIp.isEmpty() ? QStringLiteral("—") : st.tailnetIp);
    m_sessionsLabel->setText(QString::number(st.sessions));
    m_versionLabel->setText(st.version);
    if (!st.hostname.isEmpty()) {
        m_hostnameEdit->setText(st.hostname);
    }
    if (!m_allowEdit->hasFocus()) {
        m_allowEdit->setText(st.allow.join(QStringLiteral(", ")));
    }
    QStringList found;
    for (const TailnetShimDevice& d : st.discovered) {
        found << tr("%1 “%2” at %3").arg(d.kind, d.name, d.ip);
    }
    setWrapped(m_devicesLabel, found.isEmpty()
        ? tr("No Antenna Genius, Power Genius XL or Tuner Genius XL has announced itself on "
             "the radio's network yet.")
        : found.join(QStringLiteral("\n")), m_cardTextWidth);
    if (!m_shareDiscoveredCheck->hasFocus()) {
        m_shareDiscoveredCheck->setChecked(st.shareDiscovered);
    }
    if (!m_extraDevicesEdit->hasFocus()) {
        m_extraDevicesEdit->setText(st.routes.join(QStringLiteral(", ")));
    }
    if (st.state == QLatin1String("running")) {
        if (st.advertisedRoutes.isEmpty()) {
            setWrapped(m_sharingNote, tr("No devices are shared."), m_cardTextWidth);
        } else {
            QStringList approved, waiting;
            for (const QString& r : st.advertisedRoutes) {
                (st.approvedRoutes.contains(r) ? approved : waiting) << r;
            }
            QStringList lines;
            if (!approved.isEmpty()) {
                lines << tr("Confirmed in use over the tailnet: %1.")
                             .arg(approved.join(QStringLiteral(", ")));
            }
            if (!waiting.isEmpty()) {
                lines << tr("Offered, not yet confirmed: %1. If you haven't already, approve them "
                            "once in the Tailscale admin console (Machines \u2192 this radio "
                            "\u2192 Edit route settings). A route is confirmed the first time a "
                            "remote device connects through it.")
                             .arg(waiting.join(QStringLiteral(", ")));
            }
            lines << tr("Remotely, set each device's applet to its LAN address.");
            setWrapped(m_sharingNote, lines.join(QStringLiteral("\n")), m_cardTextWidth);
        }
    } else {
        setWrapped(m_sharingNote, tr("Devices are shared once the radio is on the tailnet."),
                   m_cardTextWidth);
    }
    if (st.state == QLatin1String("error") && !st.lastError.isEmpty()) {
        showError(st.lastError);
    }
    if (st.state == QLatin1String("starting")) {
        m_pollTimer->start();
    } else {
        m_pollTimer->stop();
    }
    updateControls();
}

void TailnetShimDialog::updateControls()
{
    const bool known = m_lanReachable && m_status.has_value() && !m_busy;
    const bool provisioned = known && m_status->provisioned;
    const bool haveToken = !m_adminToken.isEmpty();
    // A provisioned container accepts changes only with its admin token.
    const bool mayChange = known && (!provisioned || haveToken);

    m_refreshButton->setEnabled(m_lanReachable && !m_busy);
    m_keyEdit->setEnabled(mayChange);
    m_hostnameEdit->setEnabled(mayChange);
    m_allowEdit->setEnabled(mayChange);
    m_joinButton->setText(provisioned ? tr("Change Key") : tr("Join Tailnet"));
    m_joinButton->setAccessibleName(provisioned
        ? tr("Rejoin the tailnet with a new auth key")
        : tr("Join the tailnet with this auth key"));
    m_joinButton->setEnabled(mayChange && !m_keyEdit->text().trimmed().isEmpty());
    m_saveAllowButton->setEnabled(provisioned && haveToken);
    m_saveSharingButton->setEnabled(provisioned && haveToken);
    m_signOutButton->setEnabled(provisioned && haveToken);
    m_shareDiscoveredCheck->setEnabled(mayChange);
    m_extraDevicesEdit->setEnabled(mayChange);
    m_copyButton->setEnabled(m_status && !m_status->tailnetIp.isEmpty());

    QString note;
    if (provisioned && m_tokenLoaded && !haveToken) {
        note = tr("Note: this computer doesn't hold the admin token for this radio's "
                  "remote-access container, so it can't change the key, access list or "
                  "shared devices. To start over, remove and reinstall the container from the "
                  "Waveforms list.");
    } else if (haveToken && !m_tokenSaveError.isEmpty()) {
        note = tr("Note: the admin token couldn't be saved to the system keychain (%1), so it "
                  "is kept only until AetherSDR quits. Change the key or sharing settings now, or "
                  "fix the keychain and rejoin with a new key.").arg(m_tokenSaveError);
    } else if (haveToken && !TailnetShimTokenStore::persistentStoreAvailable()) {
        note = tr("Note: this build of AetherSDR can't use the system keychain, so the admin "
                  "token is kept only until AetherSDR quits.");
    }
    m_tokenNote->setVisible(!note.isEmpty());
    if (m_tokenNote->text() != note) {
        setWrapped(m_tokenNote, note, m_bodyTextWidth);
    }
}

void TailnetShimDialog::join()
{
    const QString key = m_keyEdit->text().trimmed();
    if (key.isEmpty()) {
        return;
    }
    m_messageLabel->hide();
    setBusy(true, tr("Joining the tailnet…"));
    // The join can take a minute and rotates the admin token, so it must
    // not depend on this window: closing it (or the Waveforms window that
    // owns it) mid-join would otherwise drop the only copy of the new token
    // and leave the container unchangeable until it is reinstalled. The
    // request and the keychain save belong to the application; the window
    // hears the outcome only if it is still open.
    auto* transaction = new TailnetShimClient(qApp);
    transaction->setRadioAddress(m_apiAddress);
    const QPointer<TailnetShimDialog> window(this);
    const QString serial = m_radioSerial;
    connect(transaction, &TailnetShimClient::provisioned, qApp,
            [transaction, window, serial](const QString& token, const TailnetShimStatus& st) {
        TailnetShimTokenStore::save(serial, token, qApp,
                                    [window](bool persisted, const QString& error) {
            if (!persisted) {
                qWarning("Remote access: the new admin token was not saved to the keychain: %s",
                         qPrintable(error));
            }
            if (window) {
                window->m_tokenSaveError = persisted ? QString() : error;
                window->updateControls();
            }
        });
        if (window) {
            window->onProvisioned(token, st);
        }
        transaction->deleteLater();
    });
    connect(transaction, &TailnetShimClient::requestFailed, qApp,
            [transaction, window](const QString& operation, const QString& message,
                                  bool unauthorized) {
        if (window) {
            window->onRequestFailed(operation, message, unauthorized);
        }
        transaction->deleteLater();
    });
    transaction->provision(key, m_hostnameEdit->text().trimmed(),
                           tailnetshim::splitAllowList(m_allowEdit->text()),
                           tailnetshim::splitAllowList(m_extraDevicesEdit->text()),
                           m_shareDiscoveredCheck->isChecked(), m_adminToken);
    // The key leaves this process in the request above and is kept nowhere.
    m_keyEdit->clear();
}

void TailnetShimDialog::onProvisioned(const QString& token, const TailnetShimStatus& st)
{
    m_adminToken = token;
    setBusy(false);
    showStatus(st);
    showMessage(tr("Joined. Connect remotely with Connect → Manual and the address %1.")
                    .arg(st.tailnetIp),
                QStringLiteral("ok"));
}

void TailnetShimDialog::onRequestFailed(const QString& operation, const QString& message,
                                        bool unauthorized)
{
    setBusy(false);
    if (unauthorized) {
        showError(tr("The container rejected this computer's admin token. To start over, "
                     "remove and reinstall the remote-access container from the Waveforms "
                     "list."));
        return;
    }
    showError(message);
    // After a failed sign-out the status says whether anything changed:
    // still provisioned means the container kept the token, so this
    // computer keeps it too (see signOut()).
    if (operation != QLatin1String("status")) {
        refresh();
    }
}

void TailnetShimDialog::saveAllowList()
{
    m_messageLabel->hide();
    setBusy(true, tr("Saving…"));
    m_client.setAllow(tailnetshim::splitAllowList(m_allowEdit->text()), m_adminToken);
}

void TailnetShimDialog::saveSharing()
{
    m_messageLabel->hide();
    setBusy(true, tr("Saving…"));
    m_client.setSharing(tailnetshim::splitAllowList(m_extraDevicesEdit->text()),
                        m_shareDiscoveredCheck->isChecked(), m_adminToken);
}

void TailnetShimDialog::signOut()
{
    const auto answer = QMessageBox::question(
        this, tr("Sign Out of Tailnet"),
        tr("Sign this radio out of your tailnet? Remote sessions end now, and you will "
           "need a new auth key to set remote access up again."),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (answer != QMessageBox::Yes) {
        return;
    }
    m_messageLabel->hide();
    setBusy(true, tr("Signing out…"));
    connect(&m_client, &TailnetShimClient::statusReceived, this,
            [this](const TailnetShimStatus& st) {
        if (!st.provisioned) {
            m_adminToken.clear();
            TailnetShimTokenStore::save(m_radioSerial, QString());
            updateControls();
        }
    }, Qt::SingleShotConnection);
    m_client.signOut(m_adminToken);
}

}  // namespace AetherSDR
