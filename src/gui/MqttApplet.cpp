#include "MqttApplet.h"

#include "core/AppSettings.h"
#include "core/LogManager.h"
#include "core/MqttAntennaAlias.h"
#include "core/MqttClient.h"

#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayoutItem>
#include <QPushButton>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextEdit>
#include <QVBoxLayout>

#ifdef HAVE_KEYCHAIN
#include <qt6keychain/keychain.h>
#endif

namespace AetherSDR {

static const QString kBtnOff =
    "QPushButton { background: #1a2a3a; color: #8090a0; "
    "border: 1px solid #205070; padding: 2px 8px; border-radius: 3px; font-size: 10px; }";
static const QString kBtnOn =
    "QPushButton { background: #00b4d8; color: #0f0f1a; font-weight: bold; "
    "border: 1px solid #008ba8; padding: 2px 8px; border-radius: 3px; font-size: 10px; }";
static const QString kPubBtn =
    "QPushButton { background: #1a2a3a; color: #c8d8e8; "
    "border: 1px solid #306080; padding: 4px 6px; border-radius: 3px; font-size: 10px; }"
    "QPushButton:hover { background: #203850; }"
    "QPushButton:pressed { background: #00b4d8; color: #0f0f1a; }";

MqttApplet::MqttApplet(QWidget* parent)
    : QWidget(parent)
{
    refreshSettings();
    buildUI();
    loadPasswordFromKeychain();
}

void MqttApplet::buildUI()
{
    auto* vbox = new QVBoxLayout(this);
    vbox->setContentsMargins(4, 4, 4, 4);
    vbox->setSpacing(5);

    auto* headerRow = new QHBoxLayout;
    auto* header = new QLabel("MQTT");
    header->setStyleSheet("QLabel { color: #c8d8e8; font-size: 11px; font-weight: bold; }");
    headerRow->addWidget(header);
    headerRow->addStretch();

    auto* settingsBtn = new QPushButton("Settings...");
    settingsBtn->setFixedHeight(18);
    settingsBtn->setStyleSheet(
        "QPushButton { background: transparent; color: #8090a0; "
        "border: none; font-size: 9px; padding: 0 4px; }"
        "QPushButton:hover { color: #c8d8e8; }");
    headerRow->addWidget(settingsBtn);
    vbox->addLayout(headerRow);

    connect(settingsBtn, &QPushButton::clicked, this, &MqttApplet::settingsRequested);

    auto* note = new QLabel("Antenna alias topics are subscribed automatically.");
    note->setWordWrap(true);
    note->setStyleSheet("QLabel { color: #8090a0; font-size: 10px; background: transparent; }");
    vbox->addWidget(note);

    auto* ctrlRow = new QHBoxLayout;
    ctrlRow->setSpacing(4);
    m_enableBtn = new QPushButton("Off");
    m_enableBtn->setFixedWidth(36);
    m_enableBtn->setStyleSheet(kBtnOff);
    ctrlRow->addWidget(m_enableBtn);

    m_statusLabel = new QLabel("Disconnected");
    m_statusLabel->setStyleSheet("QLabel { color: #8aa8c0; font-size: 10px; }");
    ctrlRow->addWidget(m_statusLabel, 1);
    vbox->addLayout(ctrlRow);

    auto* pubLbl = new QLabel("Publish");
    pubLbl->setStyleSheet("QLabel { color: #8090a0; font-size: 10px; font-weight: bold; }");
    vbox->addWidget(pubLbl);

    auto* btnContainer = new QWidget;
    m_buttonGrid = new QGridLayout(btnContainer);
    m_buttonGrid->setContentsMargins(0, 0, 0, 0);
    m_buttonGrid->setSpacing(2);
    vbox->addWidget(btnContainer);
    rebuildButtons();

    m_messageLog = new QTextEdit;
    m_messageLog->setReadOnly(true);
    m_messageLog->setMaximumHeight(95);
    m_messageLog->setStyleSheet(
        "QTextEdit { background: #0a0a14; color: #c8d8e8; border: 1px solid #203040; "
        "font-size: 10px; font-family: monospace; }");
    vbox->addWidget(m_messageLog);

    connect(m_enableBtn, &QPushButton::clicked, this, [this] {
        const bool wasOn = m_enableBtn->text() == QLatin1String("On");
        if (wasOn) {
            saveMqttConnectionEnabled(false);
            m_restoreConnectPending = false;
            emit disconnectRequested();
            m_enableBtn->setText("Off");
            m_enableBtn->setStyleSheet(kBtnOff);
            return;
        }

        saveMqttConnectionEnabled(true);
        if (!m_passwordLoaded) {
            m_restoreConnectPending = true;
            m_enableBtn->setText("On");
            m_enableBtn->setStyleSheet(kBtnOn);
            updateStatus("Waiting for keychain", false);
            return;
        }
        requestConnectFromSettings();
    });
}

void MqttApplet::setMqttClient(MqttClient* client)
{
    m_client = client;
    if (!client) return;

    connect(client, &MqttClient::connected, this, [this] {
        updateStatus("Connected", true);
    });
    connect(client, &MqttClient::disconnected, this, [this] {
        updateStatus("Disconnected", false);
        m_enableBtn->setText("Off");
        m_enableBtn->setStyleSheet(kBtnOff);
        emit displayCleared();
    });
    connect(client, &MqttClient::connectionError, this, [this](const QString& err) {
        updateStatus(err, false);
    });
    connect(client, &MqttClient::messageReceived, this, &MqttApplet::onMessageReceived);
}

void MqttApplet::refreshSettings()
{
    m_topicDefs = loadMqttTopicConfig();
    m_buttonDefs = loadMqttButtonConfig();
    if (m_buttonGrid) {
        rebuildButtons();
    }
}

void MqttApplet::setCachedPassword(const QString& password)
{
    m_password = password;
    m_passwordLoaded = true;
}

void MqttApplet::restoreConnectionState()
{
    if (!mqttConnectionEnabled()) {
        return;
    }

    if (!m_passwordLoaded) {
        m_restoreConnectPending = true;
        if (m_enableBtn) {
            m_enableBtn->setText("On");
            m_enableBtn->setStyleSheet(kBtnOn);
        }
        updateStatus("Waiting for keychain", false);
        return;
    }

    requestConnectFromSettings();
}

void MqttApplet::updateStatus(const QString& text, bool ok)
{
    m_statusLabel->setText(text);
    m_statusLabel->setStyleSheet(
        ok ? "QLabel { color: #00c040; font-size: 10px; }"
           : "QLabel { color: #8aa8c0; font-size: 10px; }");
}

void MqttApplet::onMessageReceived(const QString& topic, const QByteArray& payload)
{
    QString shortTopic = topic.section('/', -1);
    if (shortTopic.isEmpty()) { shortTopic = topic; }
    const QString value = QString::fromUtf8(payload).left(80);

    const QString line = QString("%1: %2").arg(shortTopic, value);
    m_messageLog->append(line);

    QTextDocument* doc = m_messageLog->document();
    while (doc->blockCount() > 50) {
        QTextCursor cursor(doc->begin());
        cursor.select(QTextCursor::BlockUnderCursor);
        cursor.removeSelectedText();
        cursor.deleteChar();
    }

    for (const auto& update : parseMqttAntennaAliasMessage(topic, payload)) {
        emit antennaAliasRequested(update.token, update.alias);
    }

    // Update panadapter overlay for display-enabled user topics only.
    for (const MqttTopicDef& td : m_topicDefs) {
        if (td.displayOnPan && td.topic == topic) {
            emit displayValueChanged(shortTopic, value);
            break;
        }
    }
}

void MqttApplet::rebuildButtons()
{
    QLayoutItem* item = nullptr;
    while ((item = m_buttonGrid->takeAt(0)) != nullptr) {
        delete item->widget();
        delete item;
    }
    m_buttons.clear();

    for (int i = 0; i < m_buttonDefs.size(); ++i) {
        auto* btn = new QPushButton(m_buttonDefs[i].label);
        btn->setStyleSheet(kPubBtn);
        btn->setToolTip(QString("%1 -> %2")
                            .arg(m_buttonDefs[i].topic, m_buttonDefs[i].payload));
        connect(btn, &QPushButton::clicked, this, [this, i] {
            if (m_client && m_client->isConnected()) {
                m_client->publish(m_buttonDefs[i].topic,
                                  m_buttonDefs[i].payload.toUtf8());
            }
        });
        m_buttons.append(btn);
        m_buttonGrid->addWidget(btn, i / 3, i % 3);
    }
}

void MqttApplet::requestConnectFromSettings()
{
    refreshSettings();
    const MqttConnectionConfig config = loadMqttConnectionConfig();
    emit connectRequested(config.host,
                          config.port,
                          config.username,
                          m_password,
                          mqttUserSubscriptionTopics(m_topicDefs),
                          config.useTls,
                          config.caFile);

    if (m_enableBtn) {
        m_enableBtn->setText("On");
        m_enableBtn->setStyleSheet(kBtnOn);
    }
    updateStatus("Connecting", false);
}

void MqttApplet::finishPasswordLoad()
{
    m_passwordLoaded = true;
    if (!m_restoreConnectPending) {
        return;
    }

    m_restoreConnectPending = false;
    requestConnectFromSettings();
}

// ── Keychain-backed password persistence (GHSA-mmqp-cm4w-cvpp) ──────────────

void MqttApplet::loadPasswordFromKeychain()
{
#ifdef HAVE_KEYCHAIN
    auto& settings = AppSettings::instance();
    const QString legacy = settings.value(legacyMqttPasswordSettingKey()).toString();
    if (!legacy.isEmpty()) {
        m_password = legacy;
        finishPasswordLoad();
        auto* job = new QKeychain::WritePasswordJob(mqttKeychainService());
        job->setAutoDelete(true);
        job->setKey(mqttKeychainKey());
        job->setTextData(legacy);
        connect(job, &QKeychain::Job::finished, this, [](QKeychain::Job* j) {
            if (j->error() != QKeychain::NoError) {
                qCWarning(lcMqtt) << "MqttApplet: keychain migration write failed:"
                                  << j->errorString()
                                  << "- legacy plaintext entry preserved for retry";
                return;
            }
            AppSettings::instance().remove(legacyMqttPasswordSettingKey());
            AppSettings::instance().save();
            qCInfo(lcMqtt) << "MqttApplet: migrated MQTT password to keychain";
        });
        job->start();
        return;
    }

    auto* job = new QKeychain::ReadPasswordJob(mqttKeychainService());
    job->setAutoDelete(true);
    job->setKey(mqttKeychainKey());
    connect(job, &QKeychain::Job::finished, this, [this](QKeychain::Job* j) {
        if (j->error() == QKeychain::NoError) {
            auto* read = static_cast<QKeychain::ReadPasswordJob*>(j);
            m_password = read->textData();
        } else if (j->error() != QKeychain::EntryNotFound) {
            qCWarning(lcMqtt) << "MqttApplet: keychain read failed:" << j->errorString();
        }
        finishPasswordLoad();
    });
    job->start();
#else
    const QString legacy = AppSettings::instance().value(legacyMqttPasswordSettingKey()).toString();
    if (!legacy.isEmpty()) {
        qCWarning(lcMqtt) << "MqttApplet: HAVE_KEYCHAIN not set - MQTT password "
                             "remains in plaintext AppSettings";
        m_password = legacy;
    }
    finishPasswordLoad();
#endif
}

} // namespace AetherSDR
