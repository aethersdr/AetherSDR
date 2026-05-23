#include "MqttApplet.h"
#include "core/AppSettings.h"
#include "core/LogManager.h"
#include "core/MqttAntennaAlias.h"
#include "core/MqttClient.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QCheckBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTextEdit>
#include <QTextBlock>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDialog>
#include <QDialogButtonBox>
#include <QMenu>

#ifdef HAVE_KEYCHAIN
#include <qt6keychain/keychain.h>
#endif

namespace AetherSDR {

// Keychain identifiers for the MQTT broker password.  See
// GHSA-mmqp-cm4w-cvpp — previously stored as plaintext in
// ~/.config/AetherSDR/AetherSDR.settings (default umask 0644, world-
// readable on most Linux systems).
static constexpr const char* kKeychainService = "AetherSDR";
static constexpr const char* kKeychainKey     = "mqtt_password";
static constexpr const char* kLegacySetting   = "MqttPass";

static const QString kLabelStyle =
    "QLabel { color: #8090a0; font-size: 10px; background: transparent; }";
static const QString kEditStyle =
    "QLineEdit { background: #0a0a14; color: #c8d8e8; border: 1px solid #203040; "
    "padding: 2px 4px; font-size: 10px; }";
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
static const QString kPubBtnEdit =
    "QPushButton { background: #2a1a1a; color: #d8a080; "
    "border: 1px dashed #805030; padding: 4px 6px; border-radius: 3px; font-size: 10px; }";

MqttApplet::MqttApplet(QWidget* parent)
    : QWidget(parent)
{
    loadButtons();
    buildUI();
}

void MqttApplet::buildUI()
{
    auto* vbox = new QVBoxLayout(this);
    vbox->setContentsMargins(4, 4, 4, 4);
    vbox->setSpacing(3);

    // Header
    auto* header = new QLabel("MQTT");
    header->setStyleSheet("QLabel { color: #c8d8e8; font-size: 11px; font-weight: bold; }");
    vbox->addWidget(header);

    // Broker settings grid.  Accessibility hints (objectName +
    // accessibleName + accessibleDescription) help macOS Passwords,
    // Windows Authenticator, and KDE Wallet associate user/pass fields
    // when offering autofill.  No-op on platforms / password managers
    // that don't read the accessibility tree.
    auto* grid = new QGridLayout;
    grid->setSpacing(2);
    grid->setContentsMargins(0, 0, 0, 0);

    setObjectName(QStringLiteral("mqttLoginForm"));
    setAccessibleName(tr("MQTT broker connection"));

    auto& s = AppSettings::instance();

    auto addRow = [&](int row, const QString& label, QLineEdit*& edit,
                      const QString& key, const QString& def,
                      const QString& objName, const QString& a11yName,
                      const QString& a11yDesc, bool password = false) {
        auto* lbl = new QLabel(label);
        lbl->setStyleSheet(kLabelStyle);
        grid->addWidget(lbl, row, 0);
        edit = new QLineEdit(s.value(key, def).toString());
        edit->setStyleSheet(kEditStyle);
        if (password) { edit->setEchoMode(QLineEdit::Password); }
        if (!objName.isEmpty())  edit->setObjectName(objName);
        if (!a11yName.isEmpty()) edit->setAccessibleName(a11yName);
        if (!a11yDesc.isEmpty()) edit->setAccessibleDescription(a11yDesc);
        grid->addWidget(edit, row, 1);
    };

    addRow(0, "Host:", m_hostEdit, "MqttHost", "localhost",
           QStringLiteral("mqttHost"), tr("MQTT broker host"),
           tr("Hostname or IP address of the MQTT broker"));
    addRow(1, "Port:", m_portEdit, "MqttPort", "1883",
           QStringLiteral("mqttPort"), tr("MQTT broker port"),
           tr("TCP port for the MQTT broker (1883 plaintext, 8883 TLS)"));
    addRow(2, "User:", m_userEdit, "MqttUser", "",
           QStringLiteral("mqttUsername"), tr("MQTT broker username"),
           tr("Username for MQTT broker authentication"));
    // Password is not stored in AppSettings (see GHSA-mmqp-cm4w-cvpp).
    // addRow with an empty default + sentinel key leaves the QLineEdit
    // empty; we populate it asynchronously from the keychain below, and
    // migrate any legacy plaintext value found in AppSettings on first
    // launch.
    addRow(3, "Pass:", m_passEdit, QStringLiteral("MqttPass_unused"), QString(),
           QStringLiteral("mqttPassword"), tr("MQTT broker password"),
           tr("Password for MQTT broker authentication"), true);
    loadPasswordFromKeychain();

    auto* topicLbl = new QLabel("Topics:");
    topicLbl->setStyleSheet(kLabelStyle);
    grid->addWidget(topicLbl, 4, 0, Qt::AlignTop);
    m_topicsEdit = new QLineEdit(s.value("MqttTopics", "").toString());
    m_topicsEdit->setStyleSheet(kEditStyle);
    m_topicsEdit->setPlaceholderText("topic1, topic2, ...");
    m_topicsEdit->setToolTip("Comma-separated MQTT topics to subscribe to.\n"
                             "Prefix with * to display on panadapter overlay.\n"
                             "Antenna names: aethersdr/antenna/name/+, aethersdr/antenna/names\n"
                             "Example: *rotator/pos, *ant/selected, station/log");
    grid->addWidget(m_topicsEdit, 4, 1);

    // TLS row
    auto* tlsLbl = new QLabel("TLS:");
    tlsLbl->setStyleSheet(kLabelStyle);
    grid->addWidget(tlsLbl, 5, 0);
    m_tlsCheck = new QCheckBox;
    m_tlsCheck->setChecked(s.value("MqttTls", "False").toString() == "True");
    m_tlsCheck->setStyleSheet("QCheckBox { color: #8090a0; font-size: 10px; }");
    m_tlsCheck->setToolTip("Enable TLS encryption (requires broker on port 8883)");
    grid->addWidget(m_tlsCheck, 5, 1);

    // CA certificate row (shown only when TLS is checked)
    auto* caLbl = new QLabel("CA cert:");
    caLbl->setStyleSheet(kLabelStyle);
    grid->addWidget(caLbl, 6, 0);
    m_caFileEdit = new QLineEdit(s.value("MqttCaFile", "").toString());
    m_caFileEdit->setStyleSheet(kEditStyle);
    m_caFileEdit->setPlaceholderText("optional, blank = system CA bundle");
    m_caFileEdit->setToolTip("Path to CA certificate file.\nLeave blank to use the system CA bundle.");
    grid->addWidget(m_caFileEdit, 6, 1);

    // Show/hide CA row based on TLS checkbox state
    auto updateCaVisibility = [caLbl, this](bool checked) {
        caLbl->setVisible(checked);
        m_caFileEdit->setVisible(checked);
        // Auto-switch port between 1883 and 8883
        QString port = m_portEdit->text().trimmed();
        if (checked && port == "1883") {
            m_portEdit->setText("8883");
        } else if (!checked && port == "8883") {
            m_portEdit->setText("1883");
        }
    };
    updateCaVisibility(m_tlsCheck->isChecked());
    connect(m_tlsCheck, &QCheckBox::toggled, this, updateCaVisibility);

    vbox->addLayout(grid);

    // Enable button + status
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

    // Publish buttons section
    {
        auto* pubHeader = new QHBoxLayout;
        auto* pubLbl = new QLabel("Publish");
        pubLbl->setStyleSheet("QLabel { color: #8090a0; font-size: 10px; font-weight: bold; }");
        pubHeader->addWidget(pubLbl);
        pubHeader->addStretch();

        m_editBtn = new QPushButton("Edit");
        m_editBtn->setFixedHeight(16);
        m_editBtn->setStyleSheet(
            "QPushButton { background: transparent; color: #607080; "
            "border: none; font-size: 9px; padding: 0 4px; }"
            "QPushButton:hover { color: #c8d8e8; }");
        pubHeader->addWidget(m_editBtn);
        vbox->addLayout(pubHeader);

        auto* btnContainer = new QWidget;
        m_buttonGrid = new QGridLayout(btnContainer);
        m_buttonGrid->setContentsMargins(0, 0, 0, 0);
        m_buttonGrid->setSpacing(2);
        vbox->addWidget(btnContainer);

        rebuildButtons();

        connect(m_editBtn, &QPushButton::clicked, this, [this] {
            m_editMode = !m_editMode;
            m_editBtn->setText(m_editMode ? "Done" : "Edit");
            rebuildButtons();
        });
    }

    // Message log
    m_messageLog = new QTextEdit;
    m_messageLog->setReadOnly(true);
    m_messageLog->setMaximumHeight(80);
    m_messageLog->setStyleSheet(
        "QTextEdit { background: #0a0a14; color: #c8d8e8; border: 1px solid #203040; "
        "font-size: 10px; font-family: monospace; }");
    vbox->addWidget(m_messageLog);

    // Enable toggle
    connect(m_enableBtn, &QPushButton::clicked, this, [this] {
        bool wasOn = m_enableBtn->text() == "On";
        if (wasOn) {
            emit disconnectRequested();
            m_enableBtn->setText("Off");
            m_enableBtn->setStyleSheet(kBtnOff);
        } else {
            auto& ss = AppSettings::instance();
            ss.setValue("MqttHost",   m_hostEdit->text().trimmed());
            ss.setValue("MqttPort",   m_portEdit->text().trimmed());
            ss.setValue("MqttUser",   m_userEdit->text().trimmed());
            // Password lives in QKeychain — see GHSA-mmqp-cm4w-cvpp.
            // Belt-and-braces: ensure no legacy plaintext entry survives.
            ss.remove(kLegacySetting);
            savePasswordToKeychain(m_passEdit->text().trimmed());
            ss.setValue("MqttTopics", m_topicsEdit->text().trimmed());
            ss.setValue("MqttTls",    m_tlsCheck->isChecked() ? "True" : "False");
            ss.setValue("MqttCaFile", m_caFileEdit->text().trimmed());
            ss.save();

            // Parse topics — * prefix means display on panadapter
            m_topicDefs.clear();
            QStringList topicNames;
            for (const QString& raw : m_topicsEdit->text().split(',', Qt::SkipEmptyParts)) {
                QString t = raw.trimmed();
                bool display = t.startsWith('*');
                if (display) { t = t.mid(1).trimmed(); }
                if (!t.isEmpty()) {
                    m_topicDefs.append({t, display});
                    topicNames.append(t);
                }
            }

            emit connectRequested(
                m_hostEdit->text().trimmed(),
                m_portEdit->text().trimmed().toUShort(),
                m_userEdit->text().trimmed(),
                m_passEdit->text().trimmed(),
                topicNames,
                m_tlsCheck->isChecked(),
                m_caFileEdit->text().trimmed());

            m_enableBtn->setText("On");
            m_enableBtn->setStyleSheet(kBtnOn);
        }
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
    QString value = QString::fromUtf8(payload).left(80);

    QString line = QString("%1: %2").arg(shortTopic, value);
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

    // Update panadapter overlay for display-enabled topics
    for (const auto& td : m_topicDefs) {
        if (td.displayOnPan && td.topic == topic) {
            emit displayValueChanged(shortTopic, value);
            break;
        }
    }
}

// ── Publish Buttons ──────────────────────────────────────────────────────────

void MqttApplet::rebuildButtons()
{
    // Clear existing
    for (auto* btn : m_buttons) { delete btn; }
    m_buttons.clear();

    // Remove the "+" button if it exists
    QLayoutItem* item;
    while ((item = m_buttonGrid->takeAt(0)) != nullptr) {
        delete item->widget();
        delete item;
    }

    // Add buttons from defs
    for (int i = 0; i < m_buttonDefs.size(); ++i) {
        auto* btn = new QPushButton(m_buttonDefs[i].label);
        btn->setStyleSheet(m_editMode ? kPubBtnEdit : kPubBtn);
        btn->setToolTip(m_editMode
            ? QString("Click to edit\nTopic: %1\nPayload: %2")
                .arg(m_buttonDefs[i].topic, m_buttonDefs[i].payload)
            : QString("%1 → %2")
                .arg(m_buttonDefs[i].topic, m_buttonDefs[i].payload));

        if (m_editMode) {
            btn->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(btn, &QPushButton::clicked, this, [this, i] { editButton(i); });
            connect(btn, &QPushButton::customContextMenuRequested, this, [this, i](const QPoint& pos) {
                QMenu menu;
                menu.addAction("Remove", this, [this, i] { removeButton(i); });
                menu.exec(m_buttons[i]->mapToGlobal(pos));
            });
        } else {
            connect(btn, &QPushButton::clicked, this, [this, i] {
                if (m_client && m_client->isConnected()) {
                    m_client->publish(m_buttonDefs[i].topic,
                                      m_buttonDefs[i].payload.toUtf8());
                }
            });
        }

        m_buttons.append(btn);
        m_buttonGrid->addWidget(btn, i / 3, i % 3);
    }

    // Add "+" button in edit mode
    if (m_editMode && m_buttonDefs.size() < 12) {
        auto* addBtn = new QPushButton("+");
        addBtn->setStyleSheet(
            "QPushButton { background: #1a2a1a; color: #60a060; "
            "border: 1px dashed #305030; padding: 4px 6px; border-radius: 3px; font-size: 14px; }");
        connect(addBtn, &QPushButton::clicked, this, &MqttApplet::addButton);
        int idx = m_buttonDefs.size();
        m_buttonGrid->addWidget(addBtn, idx / 3, idx % 3);
    }
}

void MqttApplet::editButton(int index)
{
    if (index < 0 || index >= m_buttonDefs.size()) return;

    QDialog dlg(this);
    dlg.setWindowTitle("Edit MQTT Button");
    dlg.setFixedWidth(280);
    auto* vb = new QVBoxLayout(&dlg);

    auto* labelEdit = new QLineEdit(m_buttonDefs[index].label);
    auto* topicEdit = new QLineEdit(m_buttonDefs[index].topic);
    auto* payloadEdit = new QLineEdit(m_buttonDefs[index].payload);

    auto addField = [&](const QString& name, QLineEdit* edit) {
        auto* row = new QHBoxLayout;
        auto* lbl = new QLabel(name);
        lbl->setFixedWidth(55);
        row->addWidget(lbl);
        row->addWidget(edit);
        vb->addLayout(row);
    };

    addField("Label:", labelEdit);
    addField("Topic:", topicEdit);
    addField("Payload:", payloadEdit);

    auto* btns = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(btns, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(btns, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    vb->addWidget(btns);

    if (dlg.exec() == QDialog::Accepted) {
        m_buttonDefs[index].label = labelEdit->text().trimmed();
        m_buttonDefs[index].topic = topicEdit->text().trimmed();
        m_buttonDefs[index].payload = payloadEdit->text().trimmed();
        saveButtons();
        rebuildButtons();
    }
}

void MqttApplet::addButton()
{
    if (m_buttonDefs.size() >= 12) return;
    m_buttonDefs.append({"New", "topic", "payload"});
    saveButtons();
    editButton(m_buttonDefs.size() - 1);
}

void MqttApplet::removeButton(int index)
{
    if (index < 0 || index >= m_buttonDefs.size()) return;
    m_buttonDefs.remove(index);
    saveButtons();
    rebuildButtons();
}

void MqttApplet::saveButtons()
{
    QJsonArray arr;
    for (const auto& def : m_buttonDefs) {
        QJsonObject obj;
        obj["label"] = def.label;
        obj["topic"] = def.topic;
        obj["payload"] = def.payload;
        arr.append(obj);
    }
    auto& s = AppSettings::instance();
    s.setValue("MqttButtons", QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
    s.save();
}

void MqttApplet::loadButtons()
{
    auto& s = AppSettings::instance();
    QString json = s.value("MqttButtons", "").toString();
    if (json.isEmpty()) return;

    QJsonArray arr = QJsonDocument::fromJson(json.toUtf8()).array();
    for (const auto& val : arr) {
        QJsonObject obj = val.toObject();
        m_buttonDefs.append({
            obj["label"].toString(),
            obj["topic"].toString(),
            obj["payload"].toString()
        });
    }
}

// ── Keychain-backed password persistence (GHSA-mmqp-cm4w-cvpp) ──────────────

void MqttApplet::loadPasswordFromKeychain()
{
#ifdef HAVE_KEYCHAIN
    auto& s = AppSettings::instance();
    const QString legacy = s.value(kLegacySetting).toString();
    if (!legacy.isEmpty()) {
        // First-launch migration on an upgraded install.  Populate the
        // UI immediately so the user doesn't see an empty field, write
        // the value to the keychain, then strip the plaintext entry
        // from AppSettings.  Keychain failure leaves the plaintext in
        // place — better that than losing the user's credentials.
        m_passEdit->setText(legacy);
        auto* job = new QKeychain::WritePasswordJob(QLatin1String(kKeychainService));
        job->setAutoDelete(true);
        job->setKey(QLatin1String(kKeychainKey));
        job->setTextData(legacy);
        connect(job, &QKeychain::Job::finished, this, [](QKeychain::Job* j) {
            if (j->error() != QKeychain::NoError) {
                qCWarning(lcMqtt) << "MqttApplet: keychain migration write failed:"
                                  << j->errorString()
                                  << "— legacy plaintext entry preserved for retry";
                return;
            }
            AppSettings::instance().remove(kLegacySetting);
            AppSettings::instance().save();
            qCInfo(lcMqtt) << "MqttApplet: migrated MQTT password to keychain"
                           << "(legacy plaintext entry removed)";
        });
        job->start();
        return;
    }

    auto* job = new QKeychain::ReadPasswordJob(QLatin1String(kKeychainService));
    job->setAutoDelete(true);
    job->setKey(QLatin1String(kKeychainKey));
    connect(job, &QKeychain::Job::finished, this, [this](QKeychain::Job* j) {
        if (!m_passEdit) return;  // applet may have been destroyed mid-flight
        if (j->error() == QKeychain::NoError) {
            auto* read = static_cast<QKeychain::ReadPasswordJob*>(j);
            m_passEdit->setText(read->textData());
        } else if (j->error() != QKeychain::EntryNotFound) {
            qCWarning(lcMqtt) << "MqttApplet: keychain read failed:" << j->errorString();
        }
    });
    job->start();
#else
    // Keychain not available at build time.  Fall back to reading the
    // legacy plaintext setting if it exists, with a warning.  This keeps
    // the applet usable on builds that opt out of Qt6Keychain at the
    // cost of leaving the password readable on disk (the bug this fix
    // is meant to address).
    auto& s = AppSettings::instance();
    const QString legacy = s.value(kLegacySetting).toString();
    if (!legacy.isEmpty()) {
        qCWarning(lcMqtt) << "MqttApplet: HAVE_KEYCHAIN not set — MQTT password "
                             "remains in plaintext AppSettings";
        m_passEdit->setText(legacy);
    }
#endif
}

void MqttApplet::savePasswordToKeychain(const QString& password)
{
#ifdef HAVE_KEYCHAIN
    if (password.isEmpty()) {
        // Empty password = user cleared the field.  Drop any keychain
        // entry so the next load doesn't surprise them with a stale value.
        auto* job = new QKeychain::DeletePasswordJob(QLatin1String(kKeychainService));
        job->setAutoDelete(true);
        job->setKey(QLatin1String(kKeychainKey));
        connect(job, &QKeychain::Job::finished, this, [](QKeychain::Job* j) {
            if (j->error() != QKeychain::NoError
                && j->error() != QKeychain::EntryNotFound) {
                qCWarning(lcMqtt) << "MqttApplet: keychain delete failed:" << j->errorString();
            }
        });
        job->start();
        return;
    }
    auto* job = new QKeychain::WritePasswordJob(QLatin1String(kKeychainService));
    job->setAutoDelete(true);
    job->setKey(QLatin1String(kKeychainKey));
    job->setTextData(password);
    connect(job, &QKeychain::Job::finished, this, [](QKeychain::Job* j) {
        if (j->error() != QKeychain::NoError)
            qCWarning(lcMqtt) << "MqttApplet: keychain save failed:" << j->errorString();
    });
    job->start();
#else
    qCWarning(lcMqtt) << "MqttApplet: HAVE_KEYCHAIN not set — falling back to "
                         "plaintext AppSettings for MQTT password";
    AppSettings::instance().setValue(kLegacySetting, password);
    AppSettings::instance().save();
#endif
}

} // namespace AetherSDR
