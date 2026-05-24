#include "CatControlApplet.h"
#include "SliceColorManager.h"
#include "core/RigctlServer.h"
#include "core/RigctlPty.h"
#include "core/AppSettings.h"
#include "models/RadioModel.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QLineEdit>

namespace AetherSDR {

namespace {

constexpr const char* kSectionStyle =
    "QWidget { background: transparent; }"
    "QLabel { color: #8090a0; font-size: 11px; }"
    "QPushButton { background: #1a2a3a; border: 1px solid #205070;"
    "  border-radius: 3px; padding: 2px 8px; font-size: 11px; font-weight: bold; color: #c8d8e8; }"
    "QPushButton:hover { background: #204060; }";

const QString kGreenToggle =
    "QPushButton { background: #1a2a3a; border: 1px solid #205070; border-radius: 3px;"
    " color: #c8d8e8; font-size: 11px; font-weight: bold; padding: 2px 8px; }"
    "QPushButton:hover { background: #204060; }"
    "QPushButton:checked { background: #006040; color: #00ff88; border: 1px solid #00a060; }";

constexpr const char* kDimLabel =
    "QLabel { color: #8090a0; font-size: 11px; }";

constexpr const char* kInsetStyle =
    "QLineEdit { font-size: 10px; background: #0a0a18; border: 1px solid #1e2e3e;"
    " border-radius: 3px; padding: 0px 2px; color: #c8d8e8; }";

} // namespace

CatControlApplet::CatControlApplet(QWidget* parent) : QWidget(parent)
{
    buildUI();
    hide();  // hidden by default, shown via CAT toggle button
}

void CatControlApplet::buildUI()
{
    setStyleSheet(kSectionStyle);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    auto* content = new QWidget;
    auto* root = new QVBoxLayout(content);
    root->setContentsMargins(4, 2, 4, 2);
    root->setSpacing(4);
    outer->addWidget(content);

    auto& settings = AppSettings::instance();

    // Enable row (created here, added to layout after channel rows)
    auto* enableRow = new QHBoxLayout;
    enableRow->setSpacing(4);

    m_tcpEnable = new QPushButton("Enable TCP");
    m_tcpEnable->setCheckable(true);
    m_tcpEnable->setStyleSheet(kGreenToggle);
    m_tcpEnable->setFixedSize(76, 22);
    {
        QSignalBlocker b(m_tcpEnable);
        m_tcpEnable->setChecked(
            settings.value("AutoStartRigctld", "False").toString() == "True");
    }
    enableRow->addWidget(m_tcpEnable);

    m_ptyEnable = new QPushButton("Enable TTY");
    m_ptyEnable->setCheckable(true);
    m_ptyEnable->setStyleSheet(kGreenToggle);
    m_ptyEnable->setFixedSize(76, 22);
    {
        QSignalBlocker b(m_ptyEnable);
        m_ptyEnable->setChecked(
            settings.value("AutoStartCAT", "False").toString() == "True");
    }
    enableRow->addWidget(m_ptyEnable);

    enableRow->addStretch();

    auto* portLabel = new QLabel("Base:");
    portLabel->setStyleSheet(kDimLabel);
    enableRow->addWidget(portLabel);

    m_basePort = new QLineEdit(settings.value("CatTcpPort", "4532").toString());
    m_basePort->setStyleSheet(kInsetStyle);
    m_basePort->setFixedWidth(40);
    m_basePort->setAlignment(Qt::AlignCenter);
    enableRow->addWidget(m_basePort);

    connect(m_basePort, &QLineEdit::editingFinished, this, [this]() {
        int port = m_basePort->text().toInt();
        if (port < 1024 || port > 65535) {
            port = 4532;
            m_basePort->setText("4532");
        }
        auto& ss = AppSettings::instance();
        ss.setValue("CatTcpPort", QString::number(port));
        ss.save();
        // If running, restart all servers with new base port
        if (m_tcpEnable->isChecked()) {
            for (int i = 0; i < kChannels; ++i) {
                if (m_servers[i]) {
                    m_servers[i]->stop();
                    m_servers[i]->start(static_cast<quint16>(port + i));
                }
            }
            updateAllChannelStatus();
        }
    });

    // TCP toggle: start/stop all 4 servers
    connect(m_tcpEnable, &QPushButton::toggled, this, [this](bool on) {
        int basePort = m_basePort->text().toInt();
        if (basePort < 1024 || basePort > 65535) {
            basePort = 4532;
        }
        auto& ss = AppSettings::instance();
        ss.setValue("CatTcpPort", QString::number(basePort));
        ss.setValue("AutoStartRigctld", on ? "True" : "False");
        ss.save();
        for (int i = 0; i < kChannels; ++i) {
            if (!m_servers[i]) {
                continue;
            }
            if (on) {
                m_servers[i]->start(static_cast<quint16>(basePort + i));
            } else {
                m_servers[i]->stop();
            }
        }
        updateAllChannelStatus();
    });

    // PTY toggle: start/stop all 4 PTYs
    connect(m_ptyEnable, &QPushButton::toggled, this, [this](bool on) {
        auto& ss = AppSettings::instance();
        ss.setValue("AutoStartCAT", on ? "True" : "False");
        ss.save();
        for (int i = 0; i < kChannels; ++i) {
            if (!m_ptys[i]) {
                continue;
            }
            if (on) {
                m_ptys[i]->start();
            } else {
                m_ptys[i]->stop();
            }
        }
        updateAllChannelStatus();
    });

    // Per-channel status rows
    static const char kLetters[] = "ABCD";

    for (int i = 0; i < kChannels; ++i) {
        auto* row = new QHBoxLayout;
        row->setSpacing(4);

        // Coloured badge
        m_rows[i].badge = new QLabel(QString(kLetters[i]));
        m_rows[i].badge->setFixedWidth(16);
        m_rows[i].badge->setAlignment(Qt::AlignCenter);
        m_rows[i].badge->setStyleSheet(
            QStringLiteral("QLabel { color: %1; font-size: 11px; font-weight: bold; }")
                .arg(SliceColorManager::instance().hexActive(i)));
        row->addWidget(m_rows[i].badge);

        // TCP status
        m_rows[i].tcpStatus = new QLabel("(stopped)");
        m_rows[i].tcpStatus->setStyleSheet("QLabel { color: #506070; font-size: 10px; }");
        m_rows[i].tcpStatus->setFixedWidth(100);
        row->addWidget(m_rows[i].tcpStatus);

        // PTY path — canonical per-user location, computed by
        // RigctlPty so the displayed path stays in sync with where
        // the actual symlink is created (#2940 / GHSA-qxhr-cwrc-pvrm).
        m_rows[i].ptyPath = new QLabel(RigctlPty::defaultSymlinkPath(i));
        m_rows[i].ptyPath->setStyleSheet("QLabel { color: #506070; font-size: 10px; }");
        row->addWidget(m_rows[i].ptyPath, 1);

        root->addLayout(row);
    }

    root->addLayout(enableRow);
}

void CatControlApplet::setRadioModel(RadioModel* model)
{
    m_model = model;
    if (model) {
        connect(model, &RadioModel::connectionStateChanged,
                this, [this](bool /*connected*/) {
            updateAllChannelStatus();
        });
    }
}

void CatControlApplet::setRigctlServers(RigctlServer** servers, int count)
{
    for (int i = 0; i < kChannels && i < count; ++i) {
        m_servers[i] = servers[i];
        if (servers[i]) {
            connect(servers[i], &RigctlServer::clientCountChanged,
                    this, [this, i]() { updateChannelStatus(i); });
        }
    }
}

void CatControlApplet::setRigctlPtys(RigctlPty** ptys, int count)
{
    for (int i = 0; i < kChannels && i < count; ++i) {
        m_ptys[i] = ptys[i];
        if (ptys[i]) {
            connect(ptys[i], &RigctlPty::started, this,
                    [this, i](const QString& path) {
                        m_rows[i].ptyPath->setText(path);
                    });
            connect(ptys[i], &RigctlPty::stopped, this,
                    [this, i]() {
                        m_rows[i].ptyPath->setText(RigctlPty::defaultSymlinkPath(i));
                    });
        }
    }
}

void CatControlApplet::updateChannelStatus(int ch)
{
    if (ch < 0 || ch >= kChannels) {
        return;
    }

    auto* srv = m_servers[ch];
    if (!srv || !srv->isRunning()) {
        m_rows[ch].tcpStatus->setText("(stopped)");
        m_rows[ch].tcpStatus->setStyleSheet("QLabel { color: #506070; font-size: 10px; }");
    } else {
        int n = srv->clientCount();
        m_rows[ch].tcpStatus->setText(
            QStringLiteral(":%1 (%2 client%3)")
                .arg(srv->port())
                .arg(n)
                .arg(n == 1 ? "" : "s"));
        m_rows[ch].tcpStatus->setStyleSheet(
            n > 0 ? QStringLiteral("QLabel { color: %1; font-size: 10px; }")
                         .arg(SliceColorManager::instance().hexActive(ch))
                   : "QLabel { color: #8090a0; font-size: 10px; }");
    }
}

void CatControlApplet::updateAllChannelStatus()
{
    for (int i = 0; i < kChannels; ++i) {
        updateChannelStatus(i);
    }
}

void CatControlApplet::setTcpEnabled(bool on)
{
    QSignalBlocker b(m_tcpEnable);
    m_tcpEnable->setChecked(on);
    updateAllChannelStatus();
}

void CatControlApplet::setPtyEnabled(bool on)
{
    QSignalBlocker b(m_ptyEnable);
    m_ptyEnable->setChecked(on);
    updateAllChannelStatus();
}

} // namespace AetherSDR
