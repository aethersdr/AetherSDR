#include "AntennaGeniusApplet.h"
#include "GuardedSlider.h"
#include "models/AntennaGeniusModel.h"

#include <QPushButton>
#include <QLabel>
#include <QComboBox>
#include <QLineEdit>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QSignalBlocker>
#include <QHostAddress>
#include <QDebug>
#include "core/AppSettings.h"

namespace MasterSDR {



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

// ── AntennaGeniusApplet ─────────────────────────────────────────────────────

AntennaGeniusApplet::AntennaGeniusApplet(QWidget* parent)
    : QWidget(parent)
{
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
    vbox->setContentsMargins(4, 4, 4, 4);
    vbox->setSpacing(4);

    // ── Device selector + connect button ────────────────────────────────────
    {
        auto* row = new QHBoxLayout;
        row->setSpacing(4);

        m_deviceCombo = new GuardedComboBox;
        m_deviceCombo->setStyleSheet(
            "QComboBox { background: #1a2a3a; border: 1px solid #203040; "
            "border-radius: 3px; padding: 2px 4px; color: #c8d8e8; font-size: 10px; }"
            "QComboBox::drop-down { border: none; }"
            "QComboBox QAbstractItemView { background: #1a2a3a; color: #c8d8e8; "
            "selection-background-color: #0070c0; }");
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
        m_manualIpEdit->setStyleSheet(
            "QLineEdit { background: #1a2a3a; border: 1px solid #203040; "
            "border-radius: 3px; padding: 2px 4px; color: #c8d8e8; font-size: 10px; }");
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
        m_portAAntLabel->setStyleSheet(
            "color: #00ff88; font-size: 11px; font-weight: bold;");
        m_portAAntLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_portAAntLabel->setMinimumWidth(0);
        hdr->addWidget(m_portAAntLabel, 1);

        pv->addLayout(hdr);

        // Antenna button grid (populated when antennas are loaded)
        m_portABtnGrid = new QWidget;
        m_portABtnGrid->setLayout(new QGridLayout);
        auto* gl = static_cast<QGridLayout*>(m_portABtnGrid->layout());
        gl->setContentsMargins(0, 0, 0, 0);
        gl->setSpacing(2);
        pv->addWidget(m_portABtnGrid);

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
    sep->setStyleSheet("background: #203040;");
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
        m_portBAntLabel->setStyleSheet(
            "color: #00ff88; font-size: 11px; font-weight: bold;");
        m_portBAntLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_portBAntLabel->setMinimumWidth(0);
        hdr->addWidget(m_portBAntLabel, 1);

        pv->addLayout(hdr);

        m_portBBtnGrid = new QWidget;
        m_portBBtnGrid->setLayout(new QGridLayout);
        auto* gl = static_cast<QGridLayout*>(m_portBBtnGrid->layout());
        gl->setContentsMargins(0, 0, 0, 0);
        gl->setSpacing(2);
        pv->addWidget(m_portBBtnGrid);

        m_portBAutoBtn = new QPushButton("AUTO");
        m_portBAutoBtn->setCheckable(true);
        m_portBAutoBtn->setStyleSheet(QString(kButtonBase) + kGreenActive);
        pv->addWidget(m_portBAutoBtn);
    }
    vbox->addWidget(m_portBSection);

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
                m_model->connectToDevice(devices[idx]);
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
    QHostAddress addr(ip);
    if (addr.isNull()) {
        m_statusLabel->setText("Invalid IP address");
        m_statusLabel->setStyleSheet("QLabel { color: #ff4444; font-size: 10px; }");
        return;
    }
    AppSettings::instance().setValue("AG_ManualIp", ip);
    m_model->connectToAddress(addr, 9007);
}

void AntennaGeniusApplet::setModel(AntennaGeniusModel* model)
{
    if (m_model == model) return;
    m_model = model;
    if (!m_model) return;

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
        if (!AntennaGeniusModel::isShackSwitch(info) && !m_model->isConnected() && m_deviceCombo->count() == 1) {
            m_model->connectToDevice(info);
        }
    });

    // Connection state
    connect(m_model, &AntennaGeniusModel::connected, this, [this]() {
        m_connectBtn->setText("Disconnect");
        m_statusLabel->setText(QString("Connected — %1 v%2")
            .arg(m_model->connectedDevice().name,
                 m_model->connectedDevice().version));
        m_statusLabel->setStyleSheet("color: #00b4d8; font-size: 10px;");

        // Hide Port B if device has only 1 radio port.
        m_portBSection->setVisible(m_model->connectedDevice().radioPorts >= 2);
    });

    connect(m_model, &AntennaGeniusModel::disconnected, this, [this]() {
        m_connectBtn->setText("Connect");
        m_statusLabel->setText("Disconnected");
        m_statusLabel->setStyleSheet("color: #606878; font-size: 10px;");
        // Clear antenna buttons.
        m_portABtns.clear();
        m_portBBtns.clear();
    });

    connect(m_model, &AntennaGeniusModel::connectionError, this,
            [this](const QString& msg) {
        m_statusLabel->setText("Error: " + msg);
        m_statusLabel->setStyleSheet("color: #ff4444; font-size: 10px;");
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
    m_model->startDiscovery();
}

void AntennaGeniusApplet::rebuildAntennaButtons()
{
    if (!m_model) return;

    auto antennas = m_model->antennas();

    // Helper: build a grid of antenna buttons for a given port.
    auto buildGrid = [&](QWidget* gridWidget, QList<QPushButton*>& btns, int portId) {
        // Remove old buttons.
        auto* gl = static_cast<QGridLayout*>(gridWidget->layout());
        while (gl->count() > 0) {
            auto* item = gl->takeAt(0);
            delete item->widget();
            delete item;
        }
        btns.clear();

        // Create a button per antenna, 4 columns.
        int col = 0, row = 0;
        for (const auto& ant : antennas) {
            auto* btn = new QPushButton(ant.name);
            btn->setCheckable(true);
            btn->setStyleSheet(QString(kButtonBase) + kBlueActive);
            btn->setToolTip(QString("Antenna %1: %2").arg(ant.id).arg(ant.name));
            btn->setFixedHeight(24);
            btn->setMinimumWidth(0);
            btn->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);

            int antId = ant.id;
            connect(btn, &QPushButton::clicked, this, [this, portId, antId]() {
                if (!m_model || !m_model->isConnected() || m_updatingFromModel)
                    return;
                // Click on already-active antenna → deselect (set to 0).
                const auto& ps = (portId == 1) ? m_model->portA() : m_model->portB();
                if (ps.rxAntenna == antId)
                    m_model->selectAntenna(portId, 0);
                else
                    m_model->selectAntenna(portId, antId);
            });

            gl->addWidget(btn, row, col);
            btns.append(btn);

            if (++col >= 4) { col = 0; ++row; }
        }
    };

    buildGrid(m_portABtnGrid, m_portABtns, 1);
    buildGrid(m_portBBtnGrid, m_portBBtns, 2);

    // Refresh display with current port state.
    updatePortDisplay(1);
    updatePortDisplay(2);
}

void AntennaGeniusApplet::updatePortDisplay(int portId)
{
    if (!m_model) return;

    m_updatingFromModel = true;

    const auto& ps = (portId == 1) ? m_model->portA() : m_model->portB();
    auto& bandLabel = (portId == 1) ? m_portABandLabel : m_portBBandLabel;
    auto& antLabel  = (portId == 1) ? m_portAAntLabel  : m_portBAntLabel;
    auto& btns      = (portId == 1) ? m_portABtns      : m_portBBtns;
    auto* autoBtn   = (portId == 1) ? m_portAAutoBtn    : m_portBAutoBtn;

    // Effective band: AG-reported or radio-frequency-derived.
    int band = m_model->effectiveBand(portId);

    // Band name
    bandLabel->setText(m_model->bandName(band));

    // Active antenna name
    QString antName = m_model->antennaName(ps.rxAntenna);
    antLabel->setText(antName);

    // TX indicator — highlight antenna label red when transmitting.
    if (ps.transmitting) {
        antLabel->setStyleSheet(
            "color: #ff4444; font-size: 11px; font-weight: bold;");
    } else {
        antLabel->setStyleSheet(
            "color: #00ff88; font-size: 11px; font-weight: bold;");
    }

    // Get the other port's selected antenna so we can block duplicates.
    const auto& otherPs = (portId == 1) ? m_model->portB() : m_model->portA();
    int otherPortAnt = otherPs.rxAntenna;

    // Highlight buttons and colour-code by TX/RX permission for current band.
    auto antennas = m_model->antennas();
    for (int i = 0; i < btns.size() && i < antennas.size(); ++i) {
        QSignalBlocker b(btns[i]);
        int antId = antennas[i].id;
        bool isActive = (antId == ps.rxAntenna);

        // Antenna in use by the other port — disable, dim, and force unchecked.
        bool inUseByOther = (antId == otherPortAnt && otherPortAnt > 0);
        btns[i]->setEnabled(!inUseByOther);
        btns[i]->setChecked(inUseByOther ? false : isActive);

        if (inUseByOther) {
            btns[i]->setStyleSheet(
                "QPushButton { background: #101820; border: 1px solid #182028; "
                "border-radius: 3px; padding: 2px 2px; font-size: 10px; color: #404858; }"
                "QPushButton:checked { background: #202830; color: #606878; "
                "border: 1px solid #303848; }");
            QString otherLabel = (portId == 1) ? "Port B" : "Port A";
            btns[i]->setToolTip(QString("%1 — in use by %2")
                .arg(antennas[i].name, otherLabel));
            continue;
        }

        bool canTx = m_model->canTxOnBand(antId, band);
        bool canRx = m_model->canRxOnBand(antId, band);

        // Style based on TX/RX permissions:
        //   TX+RX → blue active (normal)
        //   RX only → amber active + "RX" suffix
        //   No RX → dim/disabled appearance
        if (band <= 0) {
            // No band known — show all buttons normally
            btns[i]->setStyleSheet(QString(kButtonBase) + kBlueActive);
            btns[i]->setToolTip(antennas[i].name);
        } else if (!canRx && !canTx) {
            // No permission on this band — dim the button
            btns[i]->setStyleSheet(
                "QPushButton { background: #101820; border: 1px solid #182028; "
                "border-radius: 3px; padding: 2px 2px; font-size: 10px; color: #404858; }"
                "QPushButton:checked { background: #202830; color: #606878; "
                "border: 1px solid #303848; }");
            btns[i]->setToolTip(QString("%1 — no RX/TX on %2")
                .arg(antennas[i].name, m_model->bandName(band)));
        } else if (canRx && !canTx) {
            // RX only — amber highlight when active
            btns[i]->setStyleSheet(QString(kButtonBase) + kAmberActive);
            btns[i]->setToolTip(QString("%1 — RX only on %2 (no TX)")
                .arg(antennas[i].name, m_model->bandName(band)));
        } else {
            // Full TX+RX — normal blue
            btns[i]->setStyleSheet(QString(kButtonBase) + kBlueActive);
            btns[i]->setToolTip(QString("%1 — TX+RX on %2")
                .arg(antennas[i].name, m_model->bandName(band)));
        }
    }

    // Show TX antenna info alongside RX antenna.
    bool rxCanTx = m_model->canTxOnBand(ps.rxAntenna, band);
    if (!rxCanTx && ps.rxAntenna > 0 && band > 0) {
        // RX antenna can't TX — show TX antenna separately
        QString txName = m_model->antennaName(ps.txAntenna);
        antLabel->setText(antName + "  TX:" + txName);
        antLabel->setStyleSheet(
            "color: #ff8800; font-size: 10px; font-weight: bold;");
    }

    // AUTO button state.
    {
        QSignalBlocker b(autoBtn);
        autoBtn->setChecked(ps.autoMode);
    }

    // Inhibit indicator
    if (ps.inhibited) {
        antLabel->setText(antName + " [INHIBIT]");
        antLabel->setStyleSheet(
            "color: #ff8800; font-size: 11px; font-weight: bold;");
    }

    m_updatingFromModel = false;
}

void AntennaGeniusApplet::syncFromModel()
{
    if (!m_model) return;
    updatePortDisplay(1);
    updatePortDisplay(2);
}

} // namespace MasterSDR
