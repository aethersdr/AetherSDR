#include "MqttSettingsDialog.h"

#include "core/AppSettings.h"
#include "core/LogManager.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTabWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

#ifdef HAVE_KEYCHAIN
#include <qt6keychain/keychain.h>
#endif

namespace AetherSDR {

namespace {

constexpr int kMaxPublishButtons = 12;

QTableWidgetItem* textItem(const QString& text = {})
{
    return new QTableWidgetItem(text);
}

QTableWidgetItem* checkItem(bool checked)
{
    auto* item = new QTableWidgetItem;
    item->setFlags((item->flags() | Qt::ItemIsUserCheckable) & ~Qt::ItemIsEditable);
    item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
    return item;
}

QList<int> selectedRowsDescending(QTableWidget* table)
{
    QList<int> rows;
    if (!table)
        return rows;

    const QModelIndexList selected = table->selectionModel()->selectedRows();
    rows.reserve(selected.size());
    for (const QModelIndex& index : selected) {
        rows.append(index.row());
    }
    std::sort(rows.begin(), rows.end(), std::greater<int>());
    return rows;
}

} // namespace

MqttSettingsDialog::MqttSettingsDialog(QWidget* parent)
    : PersistentDialog(tr("MQTT Settings"),
                       QStringLiteral("MqttSettingsDialogGeometry"),
                       parent)
{
    buildUi();
    loadSettings();
    loadPasswordFromKeychain();
}

void MqttSettingsDialog::buildUi()
{
    setMinimumSize(620, 520);

    auto* root = new QVBoxLayout(bodyWidget());
    root->setSpacing(8);

    auto* tabs = new QTabWidget;
    root->addWidget(tabs, 1);

    auto* brokerTab = new QWidget;
    auto* brokerLayout = new QVBoxLayout(brokerTab);
    auto* form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight);

    m_hostEdit = new QLineEdit;
    m_hostEdit->setObjectName(QStringLiteral("mqttHost"));
    m_hostEdit->setAccessibleName(tr("MQTT broker host"));
    form->addRow(tr("Host:"), m_hostEdit);

    m_portSpin = new QSpinBox;
    m_portSpin->setRange(1, 65535);
    m_portSpin->setObjectName(QStringLiteral("mqttPort"));
    m_portSpin->setAccessibleName(tr("MQTT broker port"));
    form->addRow(tr("Port:"), m_portSpin);

    m_userEdit = new QLineEdit;
    m_userEdit->setObjectName(QStringLiteral("mqttUsername"));
    m_userEdit->setAccessibleName(tr("MQTT broker username"));
    form->addRow(tr("User:"), m_userEdit);

    m_passEdit = new QLineEdit;
    m_passEdit->setEchoMode(QLineEdit::Password);
    m_passEdit->setObjectName(QStringLiteral("mqttPassword"));
    m_passEdit->setAccessibleName(tr("MQTT broker password"));
    form->addRow(tr("Password:"), m_passEdit);

    m_tlsCheck = new QCheckBox(tr("Use TLS"));
    form->addRow(tr("TLS:"), m_tlsCheck);

    auto* caRow = new QWidget;
    auto* caLayout = new QHBoxLayout(caRow);
    caLayout->setContentsMargins(0, 0, 0, 0);
    m_caFileEdit = new QLineEdit;
    m_caFileEdit->setPlaceholderText(tr("optional, blank = system CA bundle"));
    auto* browseCa = new QPushButton(tr("Browse..."));
    caLayout->addWidget(m_caFileEdit, 1);
    caLayout->addWidget(browseCa);
    form->addRow(tr("CA cert:"), caRow);

    brokerLayout->addLayout(form);
    brokerLayout->addStretch();
    tabs->addTab(brokerTab, tr("Broker"));

    connect(m_tlsCheck, &QCheckBox::toggled, this, [this](bool checked) {
        if (checked && m_portSpin->value() == 1883) {
            m_portSpin->setValue(8883);
        } else if (!checked && m_portSpin->value() == 8883) {
            m_portSpin->setValue(1883);
        }
    });
    connect(browseCa, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(
            this, tr("Select MQTT CA Certificate"), m_caFileEdit->text());
        if (!path.isEmpty()) {
            m_caFileEdit->setText(path);
        }
    });

    auto* topicsTab = new QWidget;
    auto* topicsLayout = new QVBoxLayout(topicsTab);
    m_topicsTable = new QTableWidget(0, 2);
    m_topicsTable->setHorizontalHeaderLabels({tr("Topic"), tr("Display")});
    m_topicsTable->horizontalHeader()->setStretchLastSection(false);
    m_topicsTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_topicsTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_topicsTable->verticalHeader()->setVisible(false);
    m_topicsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_topicsTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    topicsLayout->addWidget(m_topicsTable, 1);

    auto* topicControls = new QHBoxLayout;
    auto* addTopic = new QPushButton(tr("Add"));
    auto* removeTopic = new QPushButton(tr("Remove"));
    topicControls->addWidget(addTopic);
    topicControls->addWidget(removeTopic);
    topicControls->addStretch();
    topicsLayout->addLayout(topicControls);

    auto* internalGroup = new QGroupBox(tr("Internal AetherSDR Topics"));
    auto* internalLayout = new QVBoxLayout(internalGroup);
    auto* internalText = new QLabel(
        tr("Subscribed automatically whenever MQTT connects; these topics are not removable:\n%1")
            .arg(internalMqttSubscriptionTopics().join(QStringLiteral("\n"))));
    internalText->setTextInteractionFlags(Qt::TextSelectableByMouse);
    internalLayout->addWidget(internalText);
    topicsLayout->addWidget(internalGroup);
    tabs->addTab(topicsTab, tr("Subscriptions"));

    connect(addTopic, &QPushButton::clicked, this, [this] { addTopicRow(); });
    connect(removeTopic, &QPushButton::clicked,
            this, &MqttSettingsDialog::removeSelectedTopicRows);

    auto* buttonsTab = new QWidget;
    auto* buttonsLayout = new QVBoxLayout(buttonsTab);
    m_buttonsTable = new QTableWidget(0, 3);
    m_buttonsTable->setHorizontalHeaderLabels({tr("Label"), tr("Topic"), tr("Payload")});
    m_buttonsTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_buttonsTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_buttonsTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_buttonsTable->verticalHeader()->setVisible(false);
    m_buttonsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_buttonsTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    buttonsLayout->addWidget(m_buttonsTable, 1);

    auto* buttonControls = new QHBoxLayout;
    m_addButtonRowBtn = new QPushButton(tr("Add"));
    auto* removeButton = new QPushButton(tr("Remove"));
    buttonControls->addWidget(m_addButtonRowBtn);
    buttonControls->addWidget(removeButton);
    buttonControls->addStretch();
    buttonsLayout->addLayout(buttonControls);
    tabs->addTab(buttonsTab, tr("Publish Buttons"));

    connect(m_addButtonRowBtn, &QPushButton::clicked, this, [this] { addButtonRow(); });
    connect(removeButton, &QPushButton::clicked,
            this, &MqttSettingsDialog::removeSelectedButtonRows);

    auto* dialogButtons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Apply | QDialogButtonBox::Cancel);
    root->addWidget(dialogButtons);

    connect(dialogButtons, &QDialogButtonBox::accepted, this, [this] {
        saveSettings();
        accept();
    });
    connect(dialogButtons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(dialogButtons->button(QDialogButtonBox::Apply), &QPushButton::clicked,
            this, &MqttSettingsDialog::saveSettings);
}

void MqttSettingsDialog::loadSettings()
{
    const MqttConnectionConfig config = loadMqttConnectionConfig();
    m_hostEdit->setText(config.host);
    m_portSpin->setValue(config.port);
    m_userEdit->setText(config.username);
    m_tlsCheck->setChecked(config.useTls);
    m_caFileEdit->setText(config.caFile);

    m_topicsTable->setRowCount(0);
    for (const MqttTopicDef& def : loadMqttTopicConfig()) {
        addTopicRow(def);
    }

    m_buttonsTable->setRowCount(0);
    for (const MqttButtonDef& def : loadMqttButtonConfig()) {
        addButtonRow(def);
    }
    updateButtonControls();
}

void MqttSettingsDialog::saveSettings()
{
    const MqttConnectionConfig config{
        m_hostEdit->text().trimmed(),
        static_cast<quint16>(m_portSpin->value()),
        m_userEdit->text().trimmed(),
        m_tlsCheck->isChecked(),
        m_caFileEdit->text().trimmed(),
    };
    saveMqttConnectionConfig(config);
    saveMqttTopicConfig(topicRows());
    saveMqttButtonConfig(buttonRows());
    savePasswordToKeychain(m_passEdit->text());
    emit settingsSaved(m_passEdit->text());
}

void MqttSettingsDialog::addTopicRow(const MqttTopicDef& def)
{
    const int row = m_topicsTable->rowCount();
    m_topicsTable->insertRow(row);
    m_topicsTable->setItem(row, 0, textItem(def.topic));
    m_topicsTable->setItem(row, 1, checkItem(def.displayOnPan));
}

void MqttSettingsDialog::removeSelectedTopicRows()
{
    for (const int row : selectedRowsDescending(m_topicsTable)) {
        m_topicsTable->removeRow(row);
    }
}

QVector<MqttTopicDef> MqttSettingsDialog::topicRows() const
{
    QVector<MqttTopicDef> topics;
    topics.reserve(m_topicsTable->rowCount());
    for (int row = 0; row < m_topicsTable->rowCount(); ++row) {
        QTableWidgetItem* topicItem = m_topicsTable->item(row, 0);
        QTableWidgetItem* displayItem = m_topicsTable->item(row, 1);
        const QString topic = topicItem ? topicItem->text().trimmed() : QString();
        if (!topic.isEmpty()) {
            topics.append({topic, displayItem && displayItem->checkState() == Qt::Checked});
        }
    }
    return topics;
}

void MqttSettingsDialog::addButtonRow(const MqttButtonDef& def)
{
    if (m_buttonsTable->rowCount() >= kMaxPublishButtons) {
        updateButtonControls();
        return;
    }

    const int row = m_buttonsTable->rowCount();
    m_buttonsTable->insertRow(row);
    m_buttonsTable->setItem(row, 0, textItem(def.label));
    m_buttonsTable->setItem(row, 1, textItem(def.topic));
    m_buttonsTable->setItem(row, 2, textItem(def.payload));
    updateButtonControls();
}

void MqttSettingsDialog::removeSelectedButtonRows()
{
    for (const int row : selectedRowsDescending(m_buttonsTable)) {
        m_buttonsTable->removeRow(row);
    }
    updateButtonControls();
}

QVector<MqttButtonDef> MqttSettingsDialog::buttonRows() const
{
    QVector<MqttButtonDef> buttons;
    buttons.reserve(m_buttonsTable->rowCount());
    for (int row = 0; row < m_buttonsTable->rowCount(); ++row) {
        QTableWidgetItem* labelItem = m_buttonsTable->item(row, 0);
        QTableWidgetItem* topicItem = m_buttonsTable->item(row, 1);
        QTableWidgetItem* payloadItem = m_buttonsTable->item(row, 2);
        MqttButtonDef def{
            labelItem ? labelItem->text().trimmed() : QString(),
            topicItem ? topicItem->text().trimmed() : QString(),
            payloadItem ? payloadItem->text() : QString(),
        };
        if (!def.label.isEmpty() || !def.topic.isEmpty() || !def.payload.isEmpty()) {
            buttons.append(def);
        }
    }
    return buttons;
}

void MqttSettingsDialog::updateButtonControls()
{
    if (m_addButtonRowBtn) {
        m_addButtonRowBtn->setEnabled(m_buttonsTable->rowCount() < kMaxPublishButtons);
    }
}

// ── Keychain-backed password persistence (GHSA-mmqp-cm4w-cvpp) ──────────────

void MqttSettingsDialog::loadPasswordFromKeychain()
{
#ifdef HAVE_KEYCHAIN
    auto& settings = AppSettings::instance();
    const QString legacy = settings.value(legacyMqttPasswordSettingKey()).toString();
    if (!legacy.isEmpty()) {
        m_passEdit->setText(legacy);
        auto* job = new QKeychain::WritePasswordJob(mqttKeychainService());
        job->setAutoDelete(true);
        job->setKey(mqttKeychainKey());
        job->setTextData(legacy);
        connect(job, &QKeychain::Job::finished, this, [](QKeychain::Job* j) {
            if (j->error() != QKeychain::NoError) {
                qCWarning(lcMqtt) << "MqttSettingsDialog: keychain migration write failed:"
                                  << j->errorString()
                                  << "- legacy plaintext entry preserved for retry";
                return;
            }
            AppSettings::instance().remove(legacyMqttPasswordSettingKey());
            AppSettings::instance().save();
            qCInfo(lcMqtt) << "MqttSettingsDialog: migrated MQTT password to keychain";
        });
        job->start();
        return;
    }

    auto* job = new QKeychain::ReadPasswordJob(mqttKeychainService());
    job->setAutoDelete(true);
    job->setKey(mqttKeychainKey());
    connect(job, &QKeychain::Job::finished, this, [this](QKeychain::Job* j) {
        if (!m_passEdit)
            return;
        if (j->error() == QKeychain::NoError) {
            auto* read = static_cast<QKeychain::ReadPasswordJob*>(j);
            m_passEdit->setText(read->textData());
        } else if (j->error() != QKeychain::EntryNotFound) {
            qCWarning(lcMqtt) << "MqttSettingsDialog: keychain read failed:"
                              << j->errorString();
        }
    });
    job->start();
#else
    const QString legacy = AppSettings::instance().value(legacyMqttPasswordSettingKey()).toString();
    if (!legacy.isEmpty()) {
        qCWarning(lcMqtt) << "MqttSettingsDialog: HAVE_KEYCHAIN not set - MQTT password "
                             "remains in plaintext AppSettings";
        m_passEdit->setText(legacy);
    }
#endif
}

void MqttSettingsDialog::savePasswordToKeychain(const QString& password)
{
#ifdef HAVE_KEYCHAIN
    AppSettings::instance().remove(legacyMqttPasswordSettingKey());
    AppSettings::instance().save();

    if (password.isEmpty()) {
        auto* job = new QKeychain::DeletePasswordJob(mqttKeychainService());
        job->setAutoDelete(true);
        job->setKey(mqttKeychainKey());
        connect(job, &QKeychain::Job::finished, this, [](QKeychain::Job* j) {
            if (j->error() != QKeychain::NoError
                && j->error() != QKeychain::EntryNotFound) {
                qCWarning(lcMqtt) << "MqttSettingsDialog: keychain delete failed:"
                                  << j->errorString();
            }
        });
        job->start();
        return;
    }

    auto* job = new QKeychain::WritePasswordJob(mqttKeychainService());
    job->setAutoDelete(true);
    job->setKey(mqttKeychainKey());
    job->setTextData(password);
    connect(job, &QKeychain::Job::finished, this, [](QKeychain::Job* j) {
        if (j->error() != QKeychain::NoError) {
            qCWarning(lcMqtt) << "MqttSettingsDialog: keychain save failed:"
                              << j->errorString();
        }
    });
    job->start();
#else
    qCWarning(lcMqtt) << "MqttSettingsDialog: HAVE_KEYCHAIN not set - falling back to "
                         "plaintext AppSettings for MQTT password";
    AppSettings::instance().setValue(legacyMqttPasswordSettingKey(), password);
    AppSettings::instance().save();
#endif
}

} // namespace AetherSDR
