#include "TailnetShimDialog.h"

#include "core/TailnetShimTokenStore.h"
#include "core/ThemeManager.h"
#include "models/RadioModel.h"

#include <QClipboard>
#include <QFormLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

namespace AetherSDR {

namespace {

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

const char* stateColor(const QString& state)
{
    if (state == QLatin1String("running")) {
        return "QLabel { color: {{color.accent.success}}; font-weight: 600; }";
    }
    if (state == QLatin1String("error")) {
        return "QLabel { color: {{color.accent.danger}}; font-weight: 600; }";
    }
    if (state == QLatin1String("starting")) {
        return "QLabel { color: {{color.accent.warning}}; font-weight: 600; }";
    }
    return "QLabel { color: {{color.text.secondary}}; font-weight: 600; }";
}

}  // namespace

TailnetShimDialog::TailnetShimDialog(RadioModel* model, QWidget* parent)
    : PersistentDialog(tr("Remote Access (Tailscale)"),
                       QStringLiteral("TailnetShimDialogGeometry"), parent)
    , m_model(model)
{
    setMinimumWidth(560);
    auto* layout = new QVBoxLayout(bodyWidget());
    layout->setSpacing(12);

    auto* intro = new QLabel(
        tr("This radio container puts the radio on your Tailscale network, so AetherSDR "
           "can reach it from anywhere, including behind CGNAT. Set it up here while you "
           "are on the radio's local network, then connect remotely with "
           "Connect → Manual using the tailnet address below."),
        bodyWidget());
    intro->setWordWrap(true);
    intro->setAccessibleName(tr("About remote access over Tailscale"));
    ThemeManager::instance().applyStyleSheet(intro, "QLabel { color: {{color.text.secondary}}; }");
    layout->addWidget(intro);

    // Status
    auto* status = new QFormLayout;
    status->setHorizontalSpacing(14);
    m_stateLabel = new QLabel(tr("Checking…"), bodyWidget());
    m_stateLabel->setAccessibleName(tr("Remote access status"));
    status->addRow(tr("Status"), m_stateLabel);
    m_nameLabel = new QLabel(QStringLiteral("—"), bodyWidget());
    m_nameLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_nameLabel->setAccessibleName(tr("Tailnet name"));
    status->addRow(tr("Tailnet name"), m_nameLabel);
    auto* addressRow = new QHBoxLayout;
    m_addressLabel = new QLabel(QStringLiteral("—"), bodyWidget());
    m_addressLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_addressLabel->setAccessibleName(tr("Tailnet address"));
    m_copyButton = new QPushButton(tr("Copy"), bodyWidget());
    m_copyButton->setAccessibleName(tr("Copy tailnet address"));
    connect(m_copyButton, &QPushButton::clicked, this, [this] {
        if (m_status && !m_status->tailnetIp.isEmpty()) {
            QGuiApplication::clipboard()->setText(m_status->tailnetIp);
        }
    });
    addressRow->addWidget(m_addressLabel, 1);
    addressRow->addWidget(m_copyButton);
    status->addRow(tr("Tailnet address"), addressRow);
    m_sessionsLabel = new QLabel(QStringLiteral("—"), bodyWidget());
    m_sessionsLabel->setAccessibleName(tr("Remote sessions"));
    status->addRow(tr("Remote sessions"), m_sessionsLabel);
    m_versionLabel = new QLabel(QStringLiteral("—"), bodyWidget());
    m_versionLabel->setAccessibleName(tr("Container version"));
    status->addRow(tr("Container version"), m_versionLabel);
    layout->addLayout(status);

    m_messageLabel = new QLabel(bodyWidget());
    m_messageLabel->setWordWrap(true);
    m_messageLabel->setAccessibleName(tr("Remote access message"));
    m_messageLabel->hide();
    layout->addWidget(m_messageLabel);

    // Setup
    auto* form = new QFormLayout;
    form->setHorizontalSpacing(14);
    m_keyEdit = new QLineEdit(bodyWidget());
    m_keyEdit->setEchoMode(QLineEdit::Password);
    m_keyEdit->setPlaceholderText(QStringLiteral("tskey-auth-…"));
    m_keyEdit->setAccessibleName(tr("Tailscale auth key"));
    m_keyEdit->setAccessibleDescription(
        tr("A single-use key from the Tailscale admin console. It is sent to the radio "
           "once and is not stored."));
    connect(m_keyEdit, &QLineEdit::textChanged, this, &TailnetShimDialog::updateControls);
    form->addRow(tr("Auth key"), m_keyEdit);
    auto* keyHint = new QLabel(
        tr("Create a single-use key in the Tailscale admin console under Settings → Keys. "
           "AetherSDR sends it to the radio once and does not keep it."),
        bodyWidget());
    keyHint->setWordWrap(true);
    ThemeManager::instance().applyStyleSheet(keyHint, "QLabel { color: {{color.text.secondary}}; }");
    form->addRow(QString(), keyHint);
    m_hostnameEdit = new QLineEdit(bodyWidget());
    m_hostnameEdit->setAccessibleName(tr("Tailnet machine name"));
    m_hostnameEdit->setMaxLength(63);
    form->addRow(tr("Machine name"), m_hostnameEdit);
    m_allowEdit = new QLineEdit(bodyWidget());
    m_allowEdit->setPlaceholderText(tr("you@example.com, tag:operators (empty: anyone on your tailnet)"));
    m_allowEdit->setAccessibleName(tr("Who may connect"));
    m_allowEdit->setAccessibleDescription(
        tr("Tailnet logins or tags allowed to connect, separated by commas. Leave empty "
           "to allow anyone on your tailnet."));
    form->addRow(tr("Who may connect"), m_allowEdit);
    layout->addLayout(form);

    m_tokenNote = new QLabel(bodyWidget());
    m_tokenNote->setWordWrap(true);
    m_tokenNote->setAccessibleName(tr("Admin token note"));
    ThemeManager::instance().applyStyleSheet(m_tokenNote, "QLabel { color: {{color.accent.warning}}; }");
    m_tokenNote->hide();
    layout->addWidget(m_tokenNote);

    layout->addStretch(1);

    auto* buttons = new QHBoxLayout;
    m_refreshButton = new QPushButton(tr("Refresh"), bodyWidget());
    m_refreshButton->setAccessibleName(tr("Refresh remote access status"));
    m_signOutButton = new QPushButton(tr("Sign Out of Tailnet"), bodyWidget());
    m_signOutButton->setAccessibleName(tr("Sign the radio out of the tailnet"));
    m_saveAllowButton = new QPushButton(tr("Save Access List"), bodyWidget());
    m_saveAllowButton->setAccessibleName(tr("Save who may connect"));
    m_joinButton = new QPushButton(tr("Join Tailnet"), bodyWidget());
    m_joinButton->setAccessibleName(tr("Join the tailnet with this auth key"));
    m_joinButton->setDefault(true);
    buttons->addWidget(m_refreshButton);
    buttons->addWidget(m_signOutButton);
    buttons->addStretch(1);
    buttons->addWidget(m_saveAllowButton);
    buttons->addWidget(m_joinButton);
    layout->addLayout(buttons);

    connect(m_refreshButton, &QPushButton::clicked, this, &TailnetShimDialog::refresh);
    connect(m_joinButton, &QPushButton::clicked, this, &TailnetShimDialog::join);
    connect(m_saveAllowButton, &QPushButton::clicked, this, &TailnetShimDialog::saveAllowList);
    connect(m_signOutButton, &QPushButton::clicked, this, &TailnetShimDialog::signOut);

    connect(&m_client, &TailnetShimClient::statusReceived, this,
            [this](const TailnetShimStatus& st) {
        setBusy(false);
        showStatus(st);
    });
    connect(&m_client, &TailnetShimClient::provisioned, this,
            [this](const QString& token, const TailnetShimStatus& st) {
        m_adminToken = token;
        TailnetShimTokenStore::save(m_radioSerial, token);
        setBusy(false);
        showStatus(st);
        m_messageLabel->setText(
            tr("The radio joined your tailnet. Connect remotely with Connect → Manual "
               "and the address %1.").arg(st.tailnetIp));
        ThemeManager::instance().applyStyleSheet(m_messageLabel,
            "QLabel { color: {{color.accent.success}}; }");
        m_messageLabel->show();
    });
    connect(&m_client, &TailnetShimClient::requestFailed, this,
            [this](const QString& operation, const QString& message, bool unauthorized) {
        setBusy(false);
        if (unauthorized) {
            showError(tr("The container rejected this computer's admin token. To start "
                         "over, remove and reinstall the remote-access container from the "
                         "Waveforms list."));
            return;
        }
        if (operation == QLatin1String("signout") || operation == QLatin1String("status")) {
            showError(message);
            return;
        }
        showError(message);
        refresh();
    });

    m_pollTimer = new QTimer(this);
    m_pollTimer->setInterval(3000);
    connect(m_pollTimer, &QTimer::timeout, this, [this] {
        if (!m_busy) {
            m_client.fetchStatus();
        }
    });

    // Reachability: the provisioning API listens only on the radio's LAN
    // address. A radio reached over the tailnet itself (or any routed path
    // that isn't a private LAN address) can't be configured from here.
    const QHostAddress address = m_model ? m_model->radioAddress() : QHostAddress();
    const RadioInfo info = m_model ? m_model->lastRadioInfo() : RadioInfo{};
    m_lanReachable = m_model && m_model->isConnected() && !address.isNull()
        && address.protocol() == QAbstractSocket::IPv4Protocol
        && (address.isPrivateUse() || address.isLinkLocal());
    m_radioSerial = info.serial;
    m_hostnameEdit->setText(tailnetshim::suggestedHostname(
        m_model ? m_model->nickname() : QString()));

    if (!m_lanReachable) {
        m_stateLabel->setText(tr("Unavailable"));
        showError(tr("Connect to this radio over its local network to set up remote access. "
                     "The container's settings can't be changed over the tailnet."));
        updateControls();
        return;
    }

    m_client.setRadioAddress(address);
    TailnetShimTokenStore::load(m_radioSerial, this, [this](const QString& token) {
        m_adminToken = token;
        m_tokenLoaded = true;
        updateControls();
    });
    refresh();
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
        m_stateLabel->setText(what);
    }
    updateControls();
}

void TailnetShimDialog::showError(const QString& message)
{
    m_messageLabel->setText(message);
    ThemeManager::instance().applyStyleSheet(m_messageLabel,
        "QLabel { color: {{color.accent.danger}}; }");
    m_messageLabel->show();
}

void TailnetShimDialog::showStatus(const TailnetShimStatus& st)
{
    m_status = st;
    m_stateLabel->setText(stateText(st));
    ThemeManager::instance().applyStyleSheet(m_stateLabel, stateColor(st.state));
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
    m_signOutButton->setEnabled(provisioned && haveToken);
    m_copyButton->setEnabled(m_status && !m_status->tailnetIp.isEmpty());

    QString note;
    if (provisioned && m_tokenLoaded && !haveToken) {
        note = tr("This computer doesn't hold the admin token for this radio's remote-access "
                  "container, so it can't change the key or access list. To start over, "
                  "remove and reinstall the container from the Waveforms list.");
    } else if (haveToken && !TailnetShimTokenStore::persistentStoreAvailable()) {
        note = tr("This build of AetherSDR can't use the system keychain, so the admin "
                  "token is kept only until AetherSDR quits.");
    }
    m_tokenNote->setText(note);
    m_tokenNote->setVisible(!note.isEmpty());
}

void TailnetShimDialog::join()
{
    const QString key = m_keyEdit->text().trimmed();
    if (key.isEmpty()) {
        return;
    }
    m_messageLabel->hide();
    setBusy(true, tr("Joining the tailnet…"));
    m_client.provision(key, m_hostnameEdit->text().trimmed(),
                       tailnetshim::splitAllowList(m_allowEdit->text()), m_adminToken);
    // The key leaves this process in the request above and is kept nowhere.
    m_keyEdit->clear();
}

void TailnetShimDialog::saveAllowList()
{
    m_messageLabel->hide();
    setBusy(true, tr("Saving…"));
    m_client.setAllow(tailnetshim::splitAllowList(m_allowEdit->text()), m_adminToken);
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
