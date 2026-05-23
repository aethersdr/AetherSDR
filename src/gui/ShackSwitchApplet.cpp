#include "ShackSwitchApplet.h"
#include "models/AntennaGeniusModel.h"
#include "core/AppSettings.h"

#include <QPushButton>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFrame>
#include <QMenu>
#include <QDesktopServices>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QUrl>

namespace MasterSDR {

// ── Styling ─────────────────────────────────────────────────────────────────

static constexpr const char* kCardStyle =
    "QWidget { background: #0d1e30; border: 1px solid #1c2a40; border-radius: 4px; }";

static constexpr const char* kLabelMuted =
    "QLabel { color: #6b8099; font-size: 10px; font-weight: bold; letter-spacing: 0.06em; }";

static constexpr const char* kStatusStyle =
    "QLabel { color: #6b8099; font-size: 10px; }";

static const QString kABtnNeutral =
    "QPushButton { background: #0b1220; border: 1px solid #1c2a40; border-radius: 3px; "
    "padding: 3px 6px; font-size: 10px; font-weight: bold; color: #6b8099; min-width: 18px; }"
    "QPushButton:hover { background: #0e1a2e; }";

static const QString kABtnActive =
    "QPushButton { background: #041e2a; border: 1px solid #00d8ef; border-radius: 3px; "
    "padding: 3px 6px; font-size: 10px; font-weight: bold; color: #00d8ef; min-width: 18px; }";

static const QString kBBtnNeutral =
    "QPushButton { background: #0b1220; border: 1px solid #1c2a40; border-radius: 3px; "
    "padding: 3px 6px; font-size: 10px; font-weight: bold; color: #6b8099; min-width: 18px; }"
    "QPushButton:hover { background: #0e1a2e; }";

static const QString kBBtnActive =
    "QPushButton { background: #1e0e00; border: 1px solid #f97316; border-radius: 3px; "
    "padding: 3px 6px; font-size: 10px; font-weight: bold; color: #f97316; min-width: 18px; }";

// Amber: B button blinking on the antenna where a conflict exists (B "wants" to go there)
static const QString kBBtnConflict =
    "QPushButton { background: #1e1200; border: 1px solid #ffaa00; border-radius: 3px; "
    "padding: 3px 6px; font-size: 10px; font-weight: bold; color: #ffaa00; min-width: 18px; }";

// Dim orange: dummy load row B button when B is parked there (blink between this and active orange)
static const QString kBBtnParked =
    "QPushButton { background: #0b1220; border: 1px solid #c05000; border-radius: 3px; "
    "padding: 3px 6px; font-size: 10px; font-weight: bold; color: #c05000; min-width: 18px; }";

static constexpr const char* kSettingsBtnStyle =
    "QPushButton { background: #0f1e30; border: 1px solid #008fa0; border-radius: 3px; "
    "padding: 4px 10px; font-size: 10px; font-weight: bold; color: #00d8ef; }"
    "QPushButton:hover { background: #041e2a; }";

static constexpr const char* kDummyLoadBtnStyle =
    "QPushButton { background: #0b1220; border: 1px solid #1c2a40; border-radius: 3px; "
    "padding: 4px 8px; font-size: 10px; color: #6b8099; text-align: left; }"
    "QPushButton:hover { background: #0e1a2e; color: #dde6f0; }";

static constexpr const char* kDummyLoadBtnActiveStyle =
    "QPushButton { background: #0b1220; border: 1px solid #c05000; border-radius: 3px; "
    "padding: 4px 8px; font-size: 10px; color: #f97316; text-align: left; }"
    "QPushButton:hover { background: #1e0e00; }";

// ── ShackSwitchApplet ────────────────────────────────────────────────────────

ShackSwitchApplet::ShackSwitchApplet(QWidget* parent)
    : QWidget(parent)
{
    hide();
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    setMaximumWidth(260);

    connect(&m_blinkTimer, &QTimer::timeout, this, &ShackSwitchApplet::onBlinkTick);

    buildUI();
}

void ShackSwitchApplet::buildUI()
{
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    auto* body = new QWidget;
    auto* vbox = new QVBoxLayout(body);
    vbox->setContentsMargins(4, 4, 4, 4);
    vbox->setSpacing(4);

    // ── Status row ───────────────────────────────────────────────────────────
    m_statusLabel = new QLabel("Not connected");
    m_statusLabel->setStyleSheet(kStatusStyle);
    vbox->addWidget(m_statusLabel);

    // ── Separator ────────────────────────────────────────────────────────────
    auto* sep1 = new QFrame;
    sep1->setFrameShape(QFrame::HLine);
    sep1->setStyleSheet("color: #1c2a40;");
    vbox->addWidget(sep1);

    // ── Input A card ─────────────────────────────────────────────────────────
    {
        auto* card = new QWidget;
        card->setStyleSheet(kCardStyle);
        auto* hl = new QHBoxLayout(card);
        hl->setContentsMargins(6, 4, 6, 4);
        hl->setSpacing(6);

        auto* hdr = new QLabel("INPUT A");
        hdr->setStyleSheet(
            "QLabel { color: #00d8ef; font-size: 10px; font-weight: bold; "
            "letter-spacing: 0.08em; min-width: 54px; }");
        hl->addWidget(hdr);

        m_inputABandLabel = new QLabel("—");
        m_inputABandLabel->setStyleSheet(
            "QLabel { color: #00d8ef; font-size: 11px; font-weight: bold; min-width: 36px; }");
        hl->addWidget(m_inputABandLabel);

        m_inputAAntLabel = new QLabel("—");
        m_inputAAntLabel->setStyleSheet(
            "QLabel { color: #dde6f0; font-size: 10px; }");
        m_inputAAntLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        hl->addWidget(m_inputAAntLabel, 1);

        vbox->addWidget(card);
    }

    // ── Input B card ─────────────────────────────────────────────────────────
    {
        m_inputBCard = new QWidget;
        m_inputBCard->setStyleSheet(kCardStyle);
        auto* hl = new QHBoxLayout(m_inputBCard);
        hl->setContentsMargins(6, 4, 6, 4);
        hl->setSpacing(6);

        auto* hdr = new QLabel("INPUT B");
        hdr->setStyleSheet(
            "QLabel { color: #f97316; font-size: 10px; font-weight: bold; "
            "letter-spacing: 0.08em; min-width: 54px; }");
        hl->addWidget(hdr);

        m_inputBBandLabel = new QLabel("—");
        m_inputBBandLabel->setStyleSheet(
            "QLabel { color: #f97316; font-size: 11px; font-weight: bold; min-width: 36px; }");
        hl->addWidget(m_inputBBandLabel);

        m_inputBAntLabel = new QLabel("—");
        m_inputBAntLabel->setStyleSheet(
            "QLabel { color: #dde6f0; font-size: 10px; }");
        m_inputBAntLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        hl->addWidget(m_inputBAntLabel, 1);

        vbox->addWidget(m_inputBCard);
        m_inputBCard->hide();
    }

    // ── Separator ────────────────────────────────────────────────────────────
    auto* sep2 = new QFrame;
    sep2->setFrameShape(QFrame::HLine);
    sep2->setStyleSheet("color: #1c2a40;");
    vbox->addWidget(sep2);

    // ── Column headers ───────────────────────────────────────────────────────
    {
        auto* hl = new QHBoxLayout;
        hl->setContentsMargins(0, 0, 0, 0);
        hl->setSpacing(2);
        auto* antHdr = new QLabel("ANTENNA");
        antHdr->setStyleSheet(kLabelMuted);
        hl->addWidget(antHdr, 1);
        auto* aHdr = new QLabel("A");
        aHdr->setStyleSheet(
            "QLabel { color: #00d8ef; font-size: 10px; font-weight: bold; "
            "min-width: 18px; text-align: center; }");
        aHdr->setAlignment(Qt::AlignCenter);
        hl->addWidget(aHdr);
        m_bColumnHeader = new QLabel("B");
        m_bColumnHeader->setStyleSheet(
            "QLabel { color: #f97316; font-size: 10px; font-weight: bold; "
            "min-width: 18px; text-align: center; }");
        m_bColumnHeader->setAlignment(Qt::AlignCenter);
        m_bColumnHeader->hide();
        hl->addWidget(m_bColumnHeader);
        vbox->addLayout(hl);
    }

    // ── Antenna rows (populated by rebuildAntennaRows) ───────────────────────
    m_antennaContainer = new QWidget;
    auto* antVbox = new QVBoxLayout(m_antennaContainer);
    antVbox->setContentsMargins(0, 0, 0, 0);
    antVbox->setSpacing(2);
    vbox->addWidget(m_antennaContainer);

    // ── Separator ────────────────────────────────────────────────────────────
    auto* sep3 = new QFrame;
    sep3->setFrameShape(QFrame::HLine);
    sep3->setStyleSheet("color: #1c2a40;");
    vbox->addWidget(sep3);

    // ── Dummy load selector ──────────────────────────────────────────────────
    {
        m_dummyLoadBtn = new QPushButton("Dummy Load: None");
        m_dummyLoadBtn->setStyleSheet(kDummyLoadBtnStyle);
        m_dummyLoadBtn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        connect(m_dummyLoadBtn, &QPushButton::clicked, this, [this]() {
            auto* menu = new QMenu(this);

            auto* noneAct = menu->addAction("None");
            noneAct->setCheckable(true);
            noneAct->setChecked(m_dummyLoadAntId < 0);
            connect(noneAct, &QAction::triggered, this, [this]() {
                m_dummyLoadAntId = -1;
                AppSettings::instance().setValue("SS_DummyLoadAnt", -1);
                updateDummyLoadBtn();
            });

            if (m_model && !m_model->antennas().isEmpty()) {
                menu->addSeparator();
                for (const auto& ant : m_model->antennas()) {
                    auto* act = menu->addAction(ant.name);
                    act->setCheckable(true);
                    act->setChecked(ant.id == m_dummyLoadAntId);
                    const int antId = ant.id;
                    connect(act, &QAction::triggered, this, [this, antId]() {
                        m_dummyLoadAntId = antId;
                        AppSettings::instance().setValue("SS_DummyLoadAnt", antId);
                        updateDummyLoadBtn();
                    });
                }
            }

            menu->exec(m_dummyLoadBtn->mapToGlobal(QPoint(0, m_dummyLoadBtn->height())));
            menu->deleteLater();
        });
        vbox->addWidget(m_dummyLoadBtn);
    }

    // ── Settings button ──────────────────────────────────────────────────────
    {
        auto* hl = new QHBoxLayout;
        hl->setContentsMargins(0, 0, 0, 0);
        hl->addStretch();
        auto* btn = new QPushButton("Settings ⚙");
        btn->setStyleSheet(kSettingsBtnStyle);
        connect(btn, &QPushButton::clicked, this, [this]() {
            auto& settings = AppSettings::instance();
            // Get IP from the live connected device; port from beacon webPort
            // (populated by UDP enrichment) if it looks valid (>1024), otherwise
            // fall back to SS_ManualPort or the Uno Q default of 5000.
            // We never trust port ≤1024 from the beacon — the firmware may
            // broadcast 80 as a placeholder before enrichment is complete.
            QString ip;
            int port = 0;
            if (m_model && m_model->isConnected()) {
                const auto& dev = m_model->connectedDevice();
                ip = dev.ip.toString();
                if (ip.isEmpty())
                    ip = m_model->peerAddress();
                if (dev.webPort > 1024)
                    port = dev.webPort;
            }
            if (ip.isEmpty())
                ip = settings.value("SS_ManualIp", "").toString();
            if (port <= 1024)
                port = settings.value("SS_WebPort", "5000").toInt();
            if (port <= 1024) port = 5000;
            if (!ip.isEmpty()) {
                QString url = "http://" + ip + ":" + QString::number(port) + "/";
                QDesktopServices::openUrl(QUrl(url));
            }
        });
        hl->addWidget(btn);
        vbox->addLayout(hl);
    }

    outer->addWidget(body);
}

void ShackSwitchApplet::setModel(AntennaGeniusModel* model)
{
    if (m_model == model) return;
    m_model = model;
    if (!m_model) return;

    m_dummyLoadAntId = AppSettings::instance().value("SS_DummyLoadAnt", -1).toInt();
    updateDummyLoadBtn();

    connect(m_model, &AntennaGeniusModel::deviceDiscovered, this,
            [this](const AgDeviceInfo& info) {
        const bool isShackSwitch = info.name.contains("ShackSwitch", Qt::CaseInsensitive);
        if (isShackSwitch && !m_model->isConnected() && !m_model->isConnecting())
            m_model->connectToDevice(info);
    });

    connect(m_model, &AntennaGeniusModel::connected, this, [this]() {
        const auto& dev = m_model->connectedDevice();
        bool isShackSwitch = dev.name.contains("ShackSwitch", Qt::CaseInsensitive);
        if (!isShackSwitch) return;

        QString ip = dev.ip.toString();
        if (ip.isEmpty()) ip = m_model->peerAddress();
        m_statusLabel->setText(ip + " • v" + dev.version);

        const bool dual = dev.radioPorts >= 2;
        m_inputBCard->setVisible(dual);
        m_bColumnHeader->setVisible(dual);
    });

    connect(m_model, &AntennaGeniusModel::disconnected, this, [this]() {
        m_blinkTimer.stop();
        m_conflictAntId    = -1;
        m_intendedBAntId   = -1;
        m_autoRoutedToDummy = false;
        m_blinkState       = false;
        m_statusLabel->setText("Not connected");
        m_inputABandLabel->setText("—");
        m_inputAAntLabel->setText("—");
        m_inputBBandLabel->setText("—");
        m_inputBAntLabel->setText("—");
    });

    connect(m_model, &AntennaGeniusModel::antennasChanged, this,
            &ShackSwitchApplet::rebuildAntennaRows);

    connect(m_model, &AntennaGeniusModel::portStatusChanged, this, [this](int) {
        updateInputHeaders();
    });

    connect(m_model, &AntennaGeniusModel::radioBandChanged, this, [this](int) {
        updateInputHeaders();
    });
}

void ShackSwitchApplet::rebuildAntennaRows()
{
    if (!m_model) return;

    m_antRows.clear();
    auto* layout = qobject_cast<QVBoxLayout*>(m_antennaContainer->layout());
    while (layout->count() > 0) {
        auto* item = layout->takeAt(0);
        if (item->widget()) item->widget()->deleteLater();
        delete item;
    }

    const auto antennas = m_model->antennas();
    const int rxA = m_model->portA().rxAntenna;
    const int rxB = m_model->portB().rxAntenna;
    const auto& dev = m_model->connectedDevice();
    const bool dualPort = dev.radioPorts >= 2;

    m_inputBCard->setVisible(dualPort);
    m_bColumnHeader->setVisible(dualPort);

    for (const auto& ant : antennas) {
        auto* row = new QWidget;
        auto* hl  = new QHBoxLayout(row);
        hl->setContentsMargins(0, 0, 0, 0);
        hl->setSpacing(2);

        auto* nameLbl = new QLabel(ant.name);
        nameLbl->setStyleSheet(
            "QLabel { background: #0b1220; border: 1px solid #1c2a40; border-radius: 3px; "
            "padding: 4px 6px; font-size: 11px; color: #dde6f0; }");
        nameLbl->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        hl->addWidget(nameLbl, 1);

        auto* aBtn = new QPushButton("A");
        aBtn->setFixedWidth(22);
        hl->addWidget(aBtn);

        auto* bBtn = new QPushButton("B");
        bBtn->setFixedWidth(22);
        bBtn->setVisible(dualPort);
        hl->addWidget(bBtn);

        const int antId = ant.id;
        connect(aBtn, &QPushButton::clicked, this, [this, antId]() {
            if (!m_model) return;
            m_model->selectAntenna(1, antId);
        });
        connect(bBtn, &QPushButton::clicked, this, [this, antId]() {
            if (!m_model) return;
            // Manual B selection clears any auto-reroute state
            m_autoRoutedToDummy = false;
            m_conflictAntId    = -1;
            m_intendedBAntId   = -1;
            m_model->selectAntenna(2, antId);
        });

        layout->addWidget(row);
        m_antRows.append({antId, aBtn, bBtn});
    }

    updateDummyLoadBtn();
    updateInputHeaders();
}

void ShackSwitchApplet::updateInputHeaders()
{
    if (!m_model || m_updatingFromModel) return;
    m_updatingFromModel = true;

    const auto portA = m_model->portA();
    const auto portB = m_model->portB();

    const auto& dev = m_model->connectedDevice();
    const bool isShackSwitch = dev.name.contains("ShackSwitch", Qt::CaseInsensitive);
    const bool singlePort = isShackSwitch && dev.radioPorts < 2;
    const int  activeAnt  = singlePort
                          ? (portA.rxAntenna > 0 ? portA.rxAntenna : portB.rxAntenna)
                          : portA.rxAntenna;

    int effBand = m_model->lastRadioBand();
    if (effBand <= 0) effBand = m_model->effectiveBand(1);
    if (effBand <= 0 && singlePort) effBand = portB.band;
    QString bandA = effBand > 0 ? m_model->bandName(effBand) : "—";
    QString antA  = activeAnt > 0 ? m_model->antennaName(activeAnt) : "—";
    m_inputABandLabel->setText(bandA);
    m_inputAAntLabel->setText(antA);

    QString bandB = portB.band > 0 ? m_model->bandName(portB.band) : "—";
    QString antB  = portB.rxAntenna > 0 ? m_model->antennaName(portB.rxAntenna) : "—";
    m_inputBBandLabel->setText(bandB);
    m_inputBAntLabel->setText(antB);

    const int rxA = singlePort ? activeAnt : portA.rxAntenna;
    const int rxB = portB.rxAntenna;

    // Must clear the guard before checkConflict, which may call selectAntenna (async, but safe)
    m_updatingFromModel = false;

    checkConflict(rxA, rxB);
    applyButtonStyles(rxA, rxB, singlePort);
}

void ShackSwitchApplet::checkConflict(int rxA, int rxB)
{
    // Single-port: no B port, nothing to conflict
    if (!m_model) return;
    const auto& dev = m_model->connectedDevice();
    const bool singlePort = dev.name.contains("ShackSwitch", Qt::CaseInsensitive)
                            && dev.radioPorts < 2;
    if (singlePort) {
        m_blinkTimer.stop();
        m_conflictAntId    = -1;
        m_intendedBAntId   = -1;
        m_autoRoutedToDummy = false;
        m_blinkState       = false;
        return;
    }

    // If we auto-routed B to dummy and user has since manually moved B elsewhere, clear state
    if (m_autoRoutedToDummy && rxB != m_dummyLoadAntId) {
        m_blinkTimer.stop();
        m_conflictAntId    = -1;
        m_intendedBAntId   = -1;
        m_autoRoutedToDummy = false;
        m_blinkState       = false;
        return;
    }

    if (rxA > 0 && rxA == rxB) {
        // Direct conflict: both ports on the same antenna
        m_conflictAntId = rxA;
        if (m_dummyLoadAntId > 0 && !m_autoRoutedToDummy) {
            // Auto-route B to dummy load; guard prevents re-entry on the resulting update
            m_intendedBAntId    = rxA;
            m_autoRoutedToDummy = true;
            m_model->selectAntenna(2, m_dummyLoadAntId);
        }
        if (!m_blinkTimer.isActive())
            m_blinkTimer.start(600);
    } else if (m_autoRoutedToDummy && rxB == m_dummyLoadAntId && rxA != rxB) {
        // B is safely parked on dummy; keep blinking to show the situation
        m_conflictAntId = m_intendedBAntId > 0 ? m_intendedBAntId : rxA;
        if (!m_blinkTimer.isActive())
            m_blinkTimer.start(600);
    } else {
        // No conflict
        m_blinkTimer.stop();
        m_conflictAntId    = -1;
        m_intendedBAntId   = -1;
        m_autoRoutedToDummy = false;
        m_blinkState       = false;
    }
}

void ShackSwitchApplet::applyButtonStyles(int rxA, int rxB, bool singlePort)
{
    for (auto& r : m_antRows) {
        if (singlePort) {
            const bool aActive = (r.antennaId == rxA);
            r.aBtn->setStyleSheet(aActive ? kABtnActive : kABtnNeutral);
            r.bBtn->setStyleSheet(kBBtnNeutral);
            continue;
        }

        // A button: always straightforward
        r.aBtn->setStyleSheet(r.antennaId == rxA ? kABtnActive : kABtnNeutral);

        // B button: depends on conflict state
        if (m_conflictAntId >= 0) {
            if (m_autoRoutedToDummy) {
                // B is parked on dummy load
                if (r.antennaId == m_conflictAntId) {
                    // This is where B wants to be — blink amber/neutral
                    r.bBtn->setStyleSheet(m_blinkState ? kBBtnConflict : kBBtnNeutral);
                } else if (r.antennaId == rxB) {
                    // This is the dummy load row — blink orange/dim
                    r.bBtn->setStyleSheet(m_blinkState ? kBBtnActive : kBBtnParked);
                } else {
                    r.bBtn->setStyleSheet(kBBtnNeutral);
                }
            } else {
                // Direct conflict, no dummy load configured — B button blinks amber on the shared antenna
                if (r.antennaId == m_conflictAntId) {
                    r.bBtn->setStyleSheet(m_blinkState ? kBBtnConflict : kBBtnNeutral);
                } else {
                    r.bBtn->setStyleSheet(kBBtnNeutral);
                }
            }
        } else {
            r.bBtn->setStyleSheet(r.antennaId == rxB ? kBBtnActive : kBBtnNeutral);
        }
    }
}

void ShackSwitchApplet::onBlinkTick()
{
    m_blinkState = !m_blinkState;
    if (!m_model) return;
    const auto& dev = m_model->connectedDevice();
    const bool singlePort = dev.name.contains("ShackSwitch", Qt::CaseInsensitive)
                            && dev.radioPorts < 2;
    const auto portA = m_model->portA();
    const auto portB = m_model->portB();
    const int rxA = singlePort
                  ? (portA.rxAntenna > 0 ? portA.rxAntenna : portB.rxAntenna)
                  : portA.rxAntenna;
    applyButtonStyles(rxA, portB.rxAntenna, singlePort);
}

void ShackSwitchApplet::updateDummyLoadBtn()
{
    if (!m_dummyLoadBtn) return;
    if (m_dummyLoadAntId < 0) {
        m_dummyLoadBtn->setText("Dummy Load: None");
        m_dummyLoadBtn->setStyleSheet(kDummyLoadBtnStyle);
    } else {
        QString name = (m_model && m_dummyLoadAntId > 0)
                     ? m_model->antennaName(m_dummyLoadAntId)
                     : QString::number(m_dummyLoadAntId);
        m_dummyLoadBtn->setText("Dummy Load: " + name);
        m_dummyLoadBtn->setStyleSheet(kDummyLoadBtnActiveStyle);
    }
}

} // namespace MasterSDR
