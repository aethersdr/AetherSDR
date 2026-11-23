#include "AntennaGeniusApplet.h"
#include "CanonIndicators.h"
#include "PeripheralConnectionSource.h"
#include "core/PeripheralSettings.h"
#include "GuardedSlider.h"
#include "models/AntennaGeniusModel.h"

#include <QPushButton>
#include <QCheckBox>
#include <QLabel>
#include <QComboBox>
#include <QLineEdit>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QSet>
#include <QSignalBlocker>
#include <QHostAddress>
#include <QDebug>
#include <QTimer>
#include "core/AppSettings.h"
#include "core/ThemeManager.h"

namespace AetherSDR {



// ── Styling constants ───────────────────────────────────────────────────────

static constexpr const char* kButtonBase =
    "QPushButton { background: #1a2a3a; border: 1px solid #203040; "
    "border-radius: 3px; padding: 2px 2px; font-size: 10px; color: #c8d8e8; }"
    "QPushButton:hover { background: #243848; }";

static const QString kGreenActive =
    "QPushButton:checked { background-color: #006040; color: #00ff88; "
    "border: 1px solid #00a060; }";

static const QString kBlueActive =
    "QPushButton:checked { background-color: #0070c0; color: #ffffff; "
    "border: 1px solid #0090e0; }";

static const QString kAmberActive =
    "QPushButton:checked { background-color: #8a6000; color: #ffe080; "
    "border: 1px solid #a07000; }";

static constexpr const char* kLabelStyle =
    "color: #8090a0; font-size: 10px; font-weight: bold;";

static constexpr const char* kValueStyle =
    "color: #00b4d8; font-size: 11px; font-weight: bold;";
static constexpr int kSelectionRefusalMessageMs = 2500;

// ── AntennaGeniusApplet ─────────────────────────────────────────────────────

AntennaGeniusApplet::AntennaGeniusApplet(QWidget* parent)
    : QWidget(parent)
{
    theme::setContainer(this, QStringLiteral("applet/antgen"));
    hide();
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    setMaximumWidth(260);
    buildUI();
}

void AntennaGeniusApplet::buildUI()
{
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);


    // Body
    auto* body = new QWidget;
    auto* vbox = new QVBoxLayout(body);
    vbox->setContentsMargins(4, 4, 4, 2);
    vbox->setSpacing(4);

    // ── Device selector + connect button ────────────────────────────────────
    {
        auto* row = new QHBoxLayout;
        row->setSpacing(4);

        m_deviceCombo = new GuardedComboBox;
        AetherSDR::ThemeManager::instance().applyStyleSheet(m_deviceCombo, "QComboBox { background: {{color.background.1}}; border: 1px solid {{color.background.1}}; "
            "border-radius: 3px; padding: 2px 4px; color: {{color.text.primary}}; font-size: 10px; }"
            "QComboBox::drop-down { border: none; }"
            "QComboBox QAbstractItemView { background: {{color.background.1}}; color: {{color.text.primary}}; "
            "selection-background-color: {{color.background.2}}; }");
        m_deviceCombo->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        m_deviceCombo->setMinimumWidth(0);
        row->addWidget(m_deviceCombo, 1);

        m_connectBtn = new QPushButton("Connect");
        m_connectBtn->setFixedWidth(72);
        m_connectBtn->setStyleSheet(
            QString(kButtonBase) +
            "QPushButton { font-size: 10px; font-weight: bold; }");
        row->addWidget(m_connectBtn);

        vbox->addLayout(row);
    }

    // Status label
    m_statusLabel = new QLabel("No device found");
    m_statusLabel->setObjectName(QStringLiteral("agConnectionStatus"));
    m_statusLabel->setStyleSheet("color: #606878; font-size: 10px;");
    m_statusLabel->setAlignment(Qt::AlignCenter);
    vbox->addWidget(m_statusLabel);

    // ── Manual IP entry (for remote connections) ───────────────────────────
    {
        auto* row = new QHBoxLayout;
        row->setSpacing(4);

        auto* label = new QLabel("Manual IP:");
        label->setStyleSheet(kLabelStyle);
        row->addWidget(label);

        m_manualIpEdit = new QLineEdit;
        AetherSDR::ThemeManager::instance().applyStyleSheet(m_manualIpEdit, "QLineEdit { background: {{color.background.1}}; border: 1px solid {{color.background.1}}; "
            "border-radius: 3px; padding: 2px 4px; color: {{color.text.primary}}; font-size: 10px; }");
        m_manualIpEdit->setPlaceholderText("IP address");
        m_manualIpEdit->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        m_manualIpEdit->setMinimumWidth(0);

        // Restore last-used manual IP from settings.
        auto& settings = AppSettings::instance();
        QString savedIp = settings.value("AG_ManualIp", "").toString();
        if (!savedIp.isEmpty())
            m_manualIpEdit->setText(savedIp);

        row->addWidget(m_manualIpEdit, 1);

        vbox->addLayout(row);
    }

    m_splitRxTxCheck = new QCheckBox(tr("Split RX/TX"));
    m_splitRxTxCheck->setObjectName(QStringLiteral("agSplitRxTxMode"));
    applyCanonSheet(m_splitRxTxCheck, [] {
        return QStringLiteral("QCheckBox { color: {{color.canon.inkSoft}}; font-size: 11px; }")
            + canonIndicatorRules();
    });
    vbox->addWidget(m_splitRxTxCheck);
    connect(m_splitRxTxCheck, &QCheckBox::toggled, this, [this](bool split) {
        if (m_model)
            m_model->setSplitRxTxMode(split);
        applyAntennaSelectionMode();
        updatePortDisplay(1);
        updatePortDisplay(2);
    });

    // ── Port A section ──────────────────────────────────────────────────────
    m_portASection = new QWidget;
    {
        auto* pv = new QVBoxLayout(m_portASection);
        pv->setContentsMargins(0, 2, 0, 2);
        pv->setSpacing(2);

        // Port A header row: label + band + antenna name
        auto* hdr = new QHBoxLayout;
        hdr->setSpacing(4);
        auto* portLabel = new QLabel("Port A");
        portLabel->setStyleSheet(kLabelStyle);
        hdr->addWidget(portLabel);

        m_portABandLabel = new QLabel("—");
        m_portABandLabel->setStyleSheet(kValueStyle);
        hdr->addWidget(m_portABandLabel);

        hdr->addStretch();

        m_portAAntLabel = new QLabel("—");
        m_portAAntLabel->setObjectName(QStringLiteral("agPortAAntennaStatus"));
        AetherSDR::ThemeManager::instance().applyStyleSheet(m_portAAntLabel, "color: {{color.accent.success}}; font-size: 11px; font-weight: bold;");
        m_portAAntLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_portAAntLabel->setMinimumWidth(0);
        hdr->addWidget(m_portAAntLabel, 1);

        pv->addLayout(hdr);

        auto* selections = new QHBoxLayout;
        selections->setContentsMargins(0, 0, 0, 0);
        selections->setSpacing(4);
        auto addSelectionGrid = [&](const QString& direction, QWidget*& grid, QLabel*& heading) {
            auto* column = new QWidget;
            auto* columnLayout = new QVBoxLayout(column);
            columnLayout->setContentsMargins(0, 0, 0, 0);
            columnLayout->setSpacing(1);
            heading = new QLabel(direction);
            ThemeManager::instance().applyStyleSheet(
                heading, "color: {{color.text.label}}; font-size: 10px; font-weight: bold;");
            heading->setAlignment(Qt::AlignCenter);
            columnLayout->addWidget(heading);
            grid = new QWidget;
            grid->setLayout(new QGridLayout);
            auto* gl = static_cast<QGridLayout*>(grid->layout());
            gl->setContentsMargins(0, 0, 0, 0);
            gl->setSpacing(2);
            columnLayout->addWidget(grid);
            selections->addWidget(column, 1);
        };
        addSelectionGrid(tr("RX"), m_portARxBtnGrid, m_portARxHeading);
        addSelectionGrid(tr("TX"), m_portATxBtnGrid, m_portATxHeading);
        pv->addLayout(selections);

        // AUTO button
        m_portAAutoBtn = new QPushButton("AUTO");
        m_portAAutoBtn->setCheckable(true);
        m_portAAutoBtn->setStyleSheet(QString(kButtonBase) + kGreenActive);
        pv->addWidget(m_portAAutoBtn);
    }
    vbox->addWidget(m_portASection);

    // Separator
    auto* sep = new QWidget;
    sep->setFixedHeight(1);
    AetherSDR::ThemeManager::instance().applyStyleSheet(sep, "background: {{color.background.1}};");
    vbox->addWidget(sep);

    // ── Port B section ──────────────────────────────────────────────────────
    m_portBSection = new QWidget;
    {
        auto* pv = new QVBoxLayout(m_portBSection);
        pv->setContentsMargins(0, 2, 0, 2);
        pv->setSpacing(2);

        auto* hdr = new QHBoxLayout;
        hdr->setSpacing(4);
        auto* portLabel = new QLabel("Port B");
        portLabel->setStyleSheet(kLabelStyle);
        hdr->addWidget(portLabel);

        m_portBBandLabel = new QLabel("—");
        m_portBBandLabel->setStyleSheet(kValueStyle);
        hdr->addWidget(m_portBBandLabel);

        hdr->addStretch();

        m_portBAntLabel = new QLabel("—");
        m_portBAntLabel->setObjectName(QStringLiteral("agPortBAntennaStatus"));
        AetherSDR::ThemeManager::instance().applyStyleSheet(m_portBAntLabel, "color: {{color.accent.success}}; font-size: 11px; font-weight: bold;");
        m_portBAntLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_portBAntLabel->setMinimumWidth(0);
        hdr->addWidget(m_portBAntLabel, 1);

        pv->addLayout(hdr);

        auto* selections = new QHBoxLayout;
        selections->setContentsMargins(0, 0, 0, 0);
        selections->setSpacing(4);
        auto addSelectionGrid = [&](const QString& direction, QWidget*& grid, QLabel*& heading) {
            auto* column = new QWidget;
            auto* columnLayout = new QVBoxLayout(column);
            columnLayout->setContentsMargins(0, 0, 0, 0);
            columnLayout->setSpacing(1);
            heading = new QLabel(direction);
            ThemeManager::instance().applyStyleSheet(
                heading, "color: {{color.text.label}}; font-size: 10px; font-weight: bold;");
            heading->setAlignment(Qt::AlignCenter);
            columnLayout->addWidget(heading);
            grid = new QWidget;
            grid->setLayout(new QGridLayout);
            auto* gl = static_cast<QGridLayout*>(grid->layout());
            gl->setContentsMargins(0, 0, 0, 0);
            gl->setSpacing(2);
            columnLayout->addWidget(grid);
            selections->addWidget(column, 1);
        };
        addSelectionGrid(tr("RX"), m_portBRxBtnGrid, m_portBRxHeading);
        addSelectionGrid(tr("TX"), m_portBTxBtnGrid, m_portBTxHeading);
        pv->addLayout(selections);

        m_portBAutoBtn = new QPushButton("AUTO");
        m_portBAutoBtn->setCheckable(true);
        m_portBAutoBtn->setStyleSheet(QString(kButtonBase) + kGreenActive);
        pv->addWidget(m_portBAutoBtn);
    }
    vbox->addWidget(m_portBSection);

    vbox->addStretch(1);
    m_sourceLabel = new QLabel(QStringLiteral("● OFFLINE"), this);
    m_sourceLabel->setObjectName(QStringLiteral("agConnectionSource"));
    m_sourceLabel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    vbox->addWidget(m_sourceLabel, 0, Qt::AlignRight);
    setConnectionSource(false);
    applyAntennaSelectionMode();

    outer->addWidget(body);

    // ── Connect button logic (wired in setModel) ────────────────────────────
    connect(m_connectBtn, &QPushButton::clicked, this, [this]() {
        if (!m_model) return;
        if (m_model->isConnected()) {
            m_model->disconnectFromDevice();
        } else {
            int idx = m_deviceCombo->currentIndex();
            auto devices = m_model->discoveredDevices();
            if (idx >= 0 && idx < devices.size()) {
                attemptDirectConnection(devices[idx], true);
            } else {
                // No discovered device selected — try manual IP.
                tryManualConnect();
            }
        }
    });

    // Manual IP: pressing Enter triggers connect.
    connect(m_manualIpEdit, &QLineEdit::returnPressed, this, [this]() {
        tryManualConnect();
    });

    // Port A AUTO toggle
    connect(m_portAAutoBtn, &QPushButton::toggled, this, [this](bool on) {
        if (!m_updatingFromModel && m_model && m_model->isConnected())
            m_model->setAutoMode(1, on);
    });

    // Port B AUTO toggle
    connect(m_portBAutoBtn, &QPushButton::toggled, this, [this](bool on) {
        if (!m_updatingFromModel && m_model && m_model->isConnected())
            m_model->setAutoMode(2, on);
    });

}


void AntennaGeniusApplet::tryManualConnect()
{
    if (!m_model || m_model->isConnected()) return;
    QString ip = m_manualIpEdit->text().trimmed();
    if (ip.isEmpty()) return;
    AppSettings::instance().setValue("AG_ManualIp", ip);
    const int configuredPort = AppSettings::instance().value("AG_ManualPort", "9007").toInt();
    AgDeviceInfo info;
    info.host = ip;
    info.ip = QHostAddress(ip);
    info.port = static_cast<quint16>(configuredPort > 0 && configuredPort <= 65535
        ? configuredPort : 9007);
    info.name = ip;
    info.serial = QString("manual-%1").arg(ip);
    attemptDirectConnection(info, true);
}

void AntennaGeniusApplet::attemptDirectConnection(const AgDeviceInfo& info, bool force)
{
    if (m_model && (force || (!m_model->isConnected() && !m_model->isConnecting()
                              && !m_model->isAuthBlockedFor(info)))) {
        if (force) {
            m_model->resetAuthBudgetFor(info);
        }
        m_model->connectToDevice(info);
    }
}

void AntennaGeniusApplet::setConnectionSource(bool connected)
{
    using PeripheralConnectionSource::Source;
    const auto text = PeripheralConnectionSource::describe(
        connected ? Source::Direct : Source::Offline, QStringLiteral("Antenna Genius"), true);
    m_sourceLabel->setText(text.indicator);
    m_sourceLabel->setAccessibleName(text.accessibleName);
    m_sourceLabel->setAccessibleDescription(text.description);
    AetherSDR::ThemeManager::instance().applyStyleSheet(m_sourceLabel,
        QStringLiteral("QLabel { color: {{%1}}; font-size: 9px; }")
            .arg(PeripheralConnectionSource::colorToken(
                connected ? Source::Direct : Source::Offline)));
}

void AntennaGeniusApplet::setModel(AntennaGeniusModel* model)
{
    if (m_model == model) return;
    m_model = model;
    if (!m_model) return;

    {
        QSignalBlocker blocker(m_splitRxTxCheck);
        m_splitRxTxCheck->setChecked(m_model->splitRxTxMode());
    }
    applyAntennaSelectionMode();

    // Device discovery
    connect(m_model, &AntennaGeniusModel::deviceDiscovered, this,
            [this](const AgDeviceInfo& info) {
        // Add to combo if not already present.
        for (int i = 0; i < m_deviceCombo->count(); ++i) {
            if (m_deviceCombo->itemData(i).toString() == info.serial)
                return;  // already listed
        }
        QString label = QString("%1 (%2)").arg(info.name, info.ip.toString());
        m_deviceCombo->addItem(label, info.serial);
        m_statusLabel->setText("Device found");

        // Auto-connect to first discovered device — but not ShackSwitch (handled by SS applet).
        if (!AntennaGeniusModel::isShackSwitch(info) && !m_model->isConnected() && m_deviceCombo->count() == 1
            && PeripheralSettings::autoConnect(QStringLiteral("ag"))) {
            attemptDirectConnection(info);
        }
    });

    // Connection state
    connect(m_model, &AntennaGeniusModel::connected, this, [this]() {
        m_initializationWarning.clear();
        ++m_selectionRefusalGeneration;
        setConnectionSource(!AntennaGeniusModel::isShackSwitch(m_model->connectedDevice()));
        m_connectBtn->setText("Disconnect");
        m_statusLabel->setText(QString("Connected — %1 v%2")
            .arg(m_model->connectedDevice().name,
                 m_model->connectedDevice().version));
        AetherSDR::ThemeManager::instance().applyStyleSheet(m_statusLabel, "color: {{color.accent}}; font-size: 10px;");
        // Port B visibility is set by the deviceInfoChanged handler below,
        // which fires after either info get parses or a beacon arrives.
    });

    // Set Port B visibility once authoritative radioPorts is known --
    // either from the info get response (manual-IP path) or a UDP beacon.
    connect(m_model, &AntennaGeniusModel::deviceInfoChanged, this, [this]() {
        m_portBSection->setVisible(m_model->connectedDevice().radioPorts >= 2);
    });

    connect(m_model, &AntennaGeniusModel::disconnected, this, [this]() {
        m_initializationWarning.clear();
        ++m_selectionRefusalGeneration;
        setConnectionSource(false);
        m_connectBtn->setText("Connect");
        m_statusLabel->setText("Disconnected");
        m_statusLabel->setStyleSheet("color: #606878; font-size: 10px;");
        // Remove antenna button widgets from the grid and clear the lists so the
        // display and the model are consistent while disconnected.
        auto clearGrid = [](QWidget* gridWidget, QList<QPushButton*>& btns) {
            auto* gl = static_cast<QGridLayout*>(gridWidget->layout());
            while (gl->count() > 0) {
                auto* item = gl->takeAt(0);
                delete item->widget();
                delete item;
            }
            btns.clear();
        };
        clearGrid(m_portARxBtnGrid, m_portARxBtns);
        clearGrid(m_portATxBtnGrid, m_portATxBtns);
        clearGrid(m_portBRxBtnGrid, m_portBRxBtns);
        clearGrid(m_portBTxBtnGrid, m_portBTxBtns);
    });

    connect(m_model, &AntennaGeniusModel::connectionError, this,
            [this](const QString& msg) {
        ++m_selectionRefusalGeneration;
        setConnectionSource(false);
        m_statusLabel->setText("Error: " + msg);
        AetherSDR::ThemeManager::instance().applyStyleSheet(m_statusLabel, "color: {{color.accent.danger}}; font-size: 10px;");
    });

    connect(m_model, &AntennaGeniusModel::initializationWarning, this,
            [this](const QString& msg) {
        if (!m_model->isConnected()) return;
        m_initializationWarning = msg;
        ++m_selectionRefusalGeneration;
        if (msg.isEmpty()) {
            m_statusLabel->setText(QString("Connected — %1 v%2")
                .arg(m_model->connectedDevice().name, m_model->connectedDevice().version));
            ThemeManager::instance().applyStyleSheet(
                m_statusLabel, "color: {{color.accent}}; font-size: 10px;");
        } else {
            m_statusLabel->setText(msg);
            ThemeManager::instance().applyStyleSheet(
                m_statusLabel, "color: {{color.accent.warning}}; font-size: 10px;");
        }
    });

    connect(m_model, &AntennaGeniusModel::antennaSelectionRefused, this,
            [this](const QString& msg) {
        if (!m_model->isConnected()) return;
        const quint64 generation = ++m_selectionRefusalGeneration;
        m_statusLabel->setText(msg);
        ThemeManager::instance().applyStyleSheet(
            m_statusLabel, "color: {{color.accent.warning}}; font-size: 10px;");
        QTimer::singleShot(kSelectionRefusalMessageMs, this, [this, msg, generation] {
            if (generation != m_selectionRefusalGeneration
                || !m_model || !m_model->isConnected() || m_statusLabel->text() != msg)
                return;
            if (m_initializationWarning.isEmpty()) {
                m_statusLabel->setText(QString("Connected — %1 v%2")
                    .arg(m_model->connectedDevice().name,
                         m_model->connectedDevice().version));
                ThemeManager::instance().applyStyleSheet(
                    m_statusLabel, "color: {{color.accent}}; font-size: 10px;");
            } else {
                m_statusLabel->setText(m_initializationWarning);
                ThemeManager::instance().applyStyleSheet(
                    m_statusLabel, "color: {{color.accent.warning}}; font-size: 10px;");
            }
        });
    });

    // Antenna list loaded → rebuild button grids.
    connect(m_model, &AntennaGeniusModel::antennasChanged,
            this, &AntennaGeniusApplet::rebuildAntennaButtons);

    // Port status updates — refresh both ports so "in use" exclusion stays in sync.
    connect(m_model, &AntennaGeniusModel::portStatusChanged,
            this, [this](int /*portId*/) {
        updatePortDisplay(1);
        updatePortDisplay(2);
    });

    // Radio band changed (from frequency) → refresh button colours / permissions.
    connect(m_model, &AntennaGeniusModel::radioBandChanged, this, [this]() {
        updatePortDisplay(1);
        updatePortDisplay(2);
    });

    // Start listening for devices.
    setConnectionSource(m_model->isConnected()
        && !AntennaGeniusModel::isShackSwitch(m_model->connectedDevice()));
    m_model->startDiscovery();
}

void AntennaGeniusApplet::rebuildAntennaButtons()
{
    if (!m_model) return;

    auto antennas = m_model->antennas();
    // Don't wipe the existing buttons if the model hasn't loaded the list yet —
    // that would leave the grid blank until the response arrives.
    if (antennas.isEmpty()) return;

    // Helper: build a grid of antenna buttons for a given port.
    auto buildGrid = [&](QWidget* gridWidget, QList<QPushButton*>& btns,
                         int portId, bool transmit) {
        // Remove old buttons.
        auto* gl = static_cast<QGridLayout*>(gridWidget->layout());
        while (gl->count() > 0) {
            auto* item = gl->takeAt(0);
            delete item->widget();
            delete item;
        }
        btns.clear();

        // The default RX selector retains the original four-column layout;
        // split selectors use two columns each.
        int col = 0, row = 0;
        const int columnCount = !transmit && !m_splitRxTxCheck->isChecked() ? 4 : 2;
        for (const auto& ant : antennas) {
            auto* btn = new QPushButton(ant.name);
            btn->setCheckable(true);
            btn->setStyleSheet(QString(kButtonBase) + kBlueActive);
            const QString direction = transmit ? tr("transmit") : tr("receive");
            btn->setAccessibleName(tr("Port %1 %2 antenna %3")
                .arg(portId).arg(direction, ant.name));
            btn->setToolTip(tr("%1 antenna %2: %3")
                .arg(transmit ? tr("TX") : tr("RX"))
                .arg(ant.id).arg(ant.name));
            btn->setObjectName(QString("agPort%1%2Antenna%3")
                .arg(portId).arg(transmit ? QStringLiteral("Tx") : QStringLiteral("Rx"))
                .arg(ant.id));
            btn->setFixedHeight(24);
            btn->setMinimumWidth(0);
            btn->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);

            int antId = ant.id;
            connect(btn, &QPushButton::clicked, this, [this, portId, antId, transmit]() {
                if (!m_model || !m_model->isConnected() || m_updatingFromModel)
                    return;
                const auto& ps = (portId == 1) ? m_model->portA() : m_model->portB();
                if (!m_splitRxTxCheck->isChecked()) {
                    if (ps.rxAntenna == antId) {
                        m_model->selectAntenna(portId, 0);
                    } else {
                        m_model->selectAntenna(portId, antId);
                    }
                    updatePortDisplay(1);
                    updatePortDisplay(2);
                    return;
                }

                const int selected = transmit ? ps.txAntenna : ps.rxAntenna;
                const int next = selected == antId ? 0 : antId;
                transmit ? m_model->selectTxAntenna(portId, next)
                         : m_model->selectRxAntenna(portId, next);
                updatePortDisplay(1);
                updatePortDisplay(2);
            });

            gl->addWidget(btn, row, col);
            btns.append(btn);

            if (++col >= columnCount) { col = 0; ++row; }
        }
    };

    buildGrid(m_portARxBtnGrid, m_portARxBtns, 1, false);
    buildGrid(m_portATxBtnGrid, m_portATxBtns, 1, true);
    buildGrid(m_portBRxBtnGrid, m_portBRxBtns, 2, false);
    buildGrid(m_portBTxBtnGrid, m_portBTxBtns, 2, true);
    applyAntennaSelectionMode();

    // Refresh display with current port state.
    updatePortDisplay(1);
    updatePortDisplay(2);
}

void AntennaGeniusApplet::applyAntennaSelectionMode()
{
    const bool split = m_splitRxTxCheck && m_splitRxTxCheck->isChecked();
    const QString rxHeading = split ? tr("RX") : tr("RX + TX");
    if (m_portARxHeading) m_portARxHeading->setText(rxHeading);
    if (m_portBRxHeading) m_portBRxHeading->setText(rxHeading);
    if (m_portATxBtnGrid) m_portATxBtnGrid->parentWidget()->setVisible(split);
    if (m_portBTxBtnGrid) m_portBTxBtnGrid->parentWidget()->setVisible(split);
    if (m_portATxHeading) m_portATxHeading->setText(tr("TX"));
    if (m_portBTxHeading) m_portBTxHeading->setText(tr("TX"));

    auto setColumnCount = [](QWidget* grid, int columnCount) {
        if (!grid) return;
        auto* layout = static_cast<QGridLayout*>(grid->layout());
        QList<QWidget*> buttons;
        while (layout->count() > 0) {
            auto* item = layout->takeAt(0);
            if (item->widget())
                buttons.append(item->widget());
            delete item;
        }
        int row = 0;
        int column = 0;
        for (auto* button : buttons) {
            layout->addWidget(button, row, column);
            if (++column >= columnCount) {
                column = 0;
                ++row;
            }
        }
    };
    setColumnCount(m_portARxBtnGrid, split ? 2 : 4);
    setColumnCount(m_portBRxBtnGrid, split ? 2 : 4);
}

void AntennaGeniusApplet::updatePortDisplay(int portId)
{
    if (!m_model) return;

    m_updatingFromModel = true;

    const auto& ps = (portId == 1) ? m_model->portA() : m_model->portB();
    auto& bandLabel = (portId == 1) ? m_portABandLabel : m_portBBandLabel;
    auto& antLabel  = (portId == 1) ? m_portAAntLabel  : m_portBAntLabel;
    auto& rxBtns    = (portId == 1) ? m_portARxBtns    : m_portBRxBtns;
    auto& txBtns    = (portId == 1) ? m_portATxBtns    : m_portBTxBtns;
    auto* autoBtn   = (portId == 1) ? m_portAAutoBtn    : m_portBAutoBtn;

    // Effective band: AG-reported or radio-frequency-derived.
    int band = m_model->effectiveBand(portId);

    // Band name
    bandLabel->setText(m_model->bandName(band));

    // Active antenna name
    const QString rxName = m_model->antennaName(ps.rxAntenna);
    const QString txName = m_model->antennaName(ps.txAntenna);
    antLabel->setText(ps.rxAntenna == ps.txAntenna
        ? rxName
        : tr("RX:%1  TX:%2").arg(rxName, txName));

    // TX indicator — highlight antenna label red when transmitting.
    if (ps.transmitting) {
        AetherSDR::ThemeManager::instance().applyStyleSheet(antLabel, "color: {{color.accent.danger}}; font-size: 11px; font-weight: bold;");
    } else {
        AetherSDR::ThemeManager::instance().applyStyleSheet(antLabel, "color: {{color.accent.success}}; font-size: 11px; font-weight: bold;");
    }

    // Get the other port's selected antenna so we can block duplicates.
    const auto& otherPs = (portId == 1) ? m_model->portB() : m_model->portA();
    const QSet<int> otherPortAntennas{otherPs.rxAntenna, otherPs.txAntenna};

    // Highlight buttons and colour-code each direction by its permission.
    auto antennas = m_model->antennas();
    auto updateButtons = [&](QList<QPushButton*>& btns, bool transmit) {
        for (int i = 0; i < btns.size() && i < antennas.size(); ++i) {
            QSignalBlocker b(btns[i]);
            const int antId = antennas[i].id;
            const bool split = m_splitRxTxCheck->isChecked();
            const bool isActive = split
                ? antId == (transmit ? ps.txAntenna : ps.rxAntenna)
                : ps.rxAntenna == antId;
            const bool canTx = m_model->canTxOnBand(antId, band);
            const bool canRx = m_model->canRxOnBand(antId, band);
            const bool permitted = split ? (transmit ? canTx : canRx) : canRx;

            // Antenna in use by the other port — disable, dim, and force unchecked.
            const bool inUseByOther = antId > 0 && otherPortAntennas.contains(antId);
            // An active but newly disallowed antenna remains selectable so the
            // operator can clear it, while new invalid selections are blocked.
            const bool unknownBandTx = split && transmit && band <= 0;
            const bool enabled = (isActive || !inUseByOther)
                && (!split || isActive || (!unknownBandTx && (band <= 0 || permitted)));
            btns[i]->setEnabled(enabled);
            btns[i]->setChecked(isActive);
            btns[i]->setAccessibleDescription(QString());

            if (inUseByOther) {
                AetherSDR::ThemeManager::instance().applyStyleSheet(btns[i], "QPushButton { background: #101820; border: 1px solid #182028; "
                    "border-radius: 3px; padding: 2px 2px; font-size: 10px; color: {{color.background.2}}; }"
                    "QPushButton:checked { background: #202830; color: #606878; "
                    "border: 1px solid #303848; }");
                const QString otherLabel = (portId == 1) ? tr("Port B") : tr("Port A");
                btns[i]->setToolTip(tr("%1 — in use by %2")
                    .arg(antennas[i].name, otherLabel));
                continue;
            }

            if (band <= 0) {
                btns[i]->setStyleSheet(QString(kButtonBase) + kBlueActive);
                btns[i]->setToolTip(unknownBandTx
                    ? tr("%1 — TX selection requires a known band").arg(antennas[i].name)
                    : tr("%1 — %2 antenna").arg(antennas[i].name, transmit ? tr("TX") : tr("RX")));
                btns[i]->setAccessibleDescription(btns[i]->toolTip());
            } else if (!permitted) {
                AetherSDR::ThemeManager::instance().applyStyleSheet(btns[i], "QPushButton { background: #101820; border: 1px solid #182028; "
                    "border-radius: 3px; padding: 2px 2px; font-size: 10px; color: {{color.background.2}}; }"
                    "QPushButton:checked { background: #202830; color: #606878; "
                    "border: 1px solid #303848; }");
                if (split) {
                    btns[i]->setToolTip(tr("%1 — not permitted for %2 on %3")
                        .arg(antennas[i].name,
                             transmit ? tr("TX") : tr("RX"),
                             m_model->bandName(band)));
                } else {
                    btns[i]->setToolTip(tr("%1 — not permitted for RX on %2")
                        .arg(antennas[i].name, m_model->bandName(band)));
                }
            } else if (!transmit
                       && !m_model->canTxOnBand(antId, band)) {
                btns[i]->setStyleSheet(QString(kButtonBase) + kAmberActive);
                btns[i]->setToolTip(tr("%1 — RX only on %2")
                    .arg(antennas[i].name, m_model->bandName(band)));
            } else {
                btns[i]->setStyleSheet(QString(kButtonBase) + kBlueActive);
                btns[i]->setToolTip(tr("%1 — %2 on %3")
                    .arg(antennas[i].name,
                         transmit ? tr("TX") : tr("RX"),
                         m_model->bandName(band)));
            }
        }
    };
    updateButtons(rxBtns, false);
    updateButtons(txBtns, true);

    if (!ps.transmitting && ps.rxAntenna > 0 && ps.txAntenna > 0
        && ps.rxAntenna != ps.txAntenna)
        AetherSDR::ThemeManager::instance().applyStyleSheet(
            antLabel, "color: {{color.accent.warning}}; font-size: 10px; font-weight: bold;");

    // AUTO button state.
    {
        QSignalBlocker b(autoBtn);
        autoBtn->setChecked(ps.autoMode);
    }

    // Inhibit indicator
    if (ps.inhibited) {
        antLabel->setText(antLabel->text() + " [INHIBIT]");
        AetherSDR::ThemeManager::instance().applyStyleSheet(antLabel, "color: {{color.accent.warning}}; font-size: 11px; font-weight: bold;");
    }

    m_updatingFromModel = false;
}

void AntennaGeniusApplet::syncFromModel()
{
    if (!m_model) return;
    updatePortDisplay(1);
    updatePortDisplay(2);
}

} // namespace AetherSDR
