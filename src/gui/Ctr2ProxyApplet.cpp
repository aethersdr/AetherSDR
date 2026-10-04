#include "Ctr2ProxyApplet.h"

#include "ComboStyle.h"
#include "GuardedSlider.h"
#include "core/ThemeManager.h"
#include "models/Ctr2ProxyModel.h"

#include <QGridLayout>
#include <QHBoxLayout>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>

namespace AetherSDR {

namespace {

const QString kFieldStyle = QStringLiteral(
    "QLineEdit { background: {{color.background.1}}; "
    "border: 1px solid {{color.border.subtle}}; border-radius: 3px; "
    "padding: 2px 4px; color: {{color.text.primary}}; font-size: 10px; }"
    "QLineEdit:disabled { color: {{color.text.disabled}}; }");

const QString kButtonStyle = QStringLiteral(
    "QPushButton { background: {{color.toggle.background}}; "
    "border: 1px solid {{color.toggle.border}}; border-radius: 3px; "
    "padding: 2px 8px; font-size: 10px; font-weight: bold; "
    "color: {{color.toggle.foreground}}; }"
    "QPushButton:hover { background: {{color.background.2}}; }"
    "QPushButton:disabled { background: {{color.button.background.disabled}}; "
    "color: {{color.button.foreground.disabled}}; "
    "border: 1px solid {{color.button.border.disabled}}; }");

const QString kComboExtra = QStringLiteral("QComboBox { font-size: 10px; }");

QLabel* makeLabel(const QString& text, const QString& colorToken, QWidget* parent,
                  bool wrap = false)
{
    auto* label = new QLabel(text, parent);
    label->setWordWrap(wrap);
    ThemeManager::instance().applyStyleSheet(
        label, QStringLiteral("QLabel { color: {{%1}}; font-size: 10px; }").arg(colorToken));
    return label;
}

QString formatBytes(quint64 bytes)
{
    return QLocale::c().toString(bytes) + QStringLiteral(" B");
}

void setAvailability(QWidget* w, bool enabled, const QString& reason)
{
    w->setEnabled(enabled);
    w->setAccessibleDescription(enabled ? QString() : reason);
}

} // namespace

Ctr2ProxyApplet::Ctr2ProxyApplet(QWidget* parent)
    : QWidget(parent)
{
    theme::setContainer(this, QStringLiteral("applet/ctr2proxy"));
    setAccessibleName(tr("CTR2 Proxy"));
    buildUi();
    syncConfiguration();
    syncStatus();
    syncStats();
}

void Ctr2ProxyApplet::buildUi()
{
    auto* vbox = new QVBoxLayout(this);
    vbox->setContentsMargins(4, 4, 4, 4);
    vbox->setSpacing(4);

    auto* grid = new QGridLayout;
    grid->setHorizontalSpacing(4);
    grid->setVerticalSpacing(3);

    grid->addWidget(makeLabel(tr("Mode"), QStringLiteral("color.text.label"), this), 0, 0);
    m_modeCombo = new GuardedComboBox(this);
    m_modeCombo->setObjectName(QStringLiteral("ctr2ProxyMode"));
    m_modeCombo->setAccessibleName(tr("CTR2 connection mode"));
    m_modeCombo->addItem(tr("Wi-Fi (TCP)"));
    m_modeCombo->addItem(tr("USB"));
    applyComboStyle(m_modeCombo, kComboExtra);
    m_modeCombo->setFixedHeight(20);
    grid->addWidget(m_modeCombo, 0, 1, 1, 2);

    m_refreshBtn = new QPushButton(tr("Rescan"), this);
    m_refreshBtn->setObjectName(QStringLiteral("ctr2ProxyRescan"));
    m_refreshBtn->setAccessibleName(tr("Rescan local addresses and USB devices"));
    ThemeManager::instance().applyStyleSheet(m_refreshBtn, kButtonStyle);
    grid->addWidget(m_refreshBtn, 0, 3);

    grid->addWidget(makeLabel(tr("Listen"), QStringLiteral("color.text.label"), this), 1, 0);
    m_listenCombo = new GuardedComboBox(this);
    m_listenCombo->setObjectName(QStringLiteral("ctr2ProxyListenAddress"));
    m_listenCombo->setAccessibleName(tr("CTR2 proxy listen address"));
    applyComboStyle(m_listenCombo, kComboExtra);
    m_listenCombo->setFixedHeight(20);
    grid->addWidget(m_listenCombo, 1, 1);

    m_listenPortEdit = new QLineEdit(this);
    m_listenPortEdit->setObjectName(QStringLiteral("ctr2ProxyListenPort"));
    m_listenPortEdit->setAccessibleName(tr("CTR2 proxy listen port"));
    m_listenPortEdit->setValidator(new QIntValidator(1, 65535, m_listenPortEdit));
    m_listenPortEdit->setFixedWidth(48);
    ThemeManager::instance().applyStyleSheet(m_listenPortEdit, kFieldStyle);
    grid->addWidget(m_listenPortEdit, 1, 2);

    grid->addWidget(makeLabel(tr("USB"), QStringLiteral("color.text.label"), this), 2, 0);
    m_usbCombo = new GuardedComboBox(this);
    m_usbCombo->setObjectName(QStringLiteral("ctr2ProxyUsbDevice"));
    m_usbCombo->setAccessibleName(tr("CTR2 USB device"));
    applyComboStyle(m_usbCombo, kComboExtra);
    m_usbCombo->setFixedHeight(20);
    grid->addWidget(m_usbCombo, 2, 1, 1, 3);

    grid->addWidget(makeLabel(tr("Radio"), QStringLiteral("color.text.label"), this), 3, 0);
    // Always the radio AetherSDR is connected to; captured when Start is pressed.
    m_radioLabel = makeLabel(QString(), QStringLiteral("color.text.primary"), this);
    m_radioLabel->setObjectName(QStringLiteral("ctr2ProxyRadio"));
    grid->addWidget(m_radioLabel, 3, 1, 1, 2);

    m_startBtn = new QPushButton(tr("Start"), this);
    m_startBtn->setObjectName(QStringLiteral("ctr2ProxyStart"));
    m_startBtn->setAccessibleName(tr("Start CTR2 proxy"));
    ThemeManager::instance().applyStyleSheet(m_startBtn, kButtonStyle);
    grid->addWidget(m_startBtn, 3, 3);
    grid->setColumnStretch(1, 1);
    vbox->addLayout(grid);

    m_problemLabel = makeLabel(QString(), QStringLiteral("color.accent.warning"), this, true);
    m_problemLabel->setAccessibleName(tr("CTR2 proxy configuration"));
    vbox->addWidget(m_problemLabel);

    // Status readouts in their own pane with a 1 px minimum, so a window sized
    // below the default clips the status from the bottom before the layout
    // starts squeezing the controls above.
    auto* statusPane = new QWidget(this);
    statusPane->setMinimumHeight(1);
    auto* statusBox = new QVBoxLayout(statusPane);
    statusBox->setContentsMargins(0, 0, 0, 0);
    statusBox->setSpacing(4);

    m_stateLabel = makeLabel(QString(), QStringLiteral("color.text.primary"), statusPane);
    m_stateLabel->setObjectName(QStringLiteral("ctr2ProxyState"));
    statusBox->addWidget(m_stateLabel);
    // Endpoints and traffic side by side to save vertical space.
    auto* statusRow = new QHBoxLayout;
    statusRow->setSpacing(8);
    m_endpointsLabel = makeLabel(QString(), QStringLiteral("color.text.secondary"), statusPane);
    m_endpointsLabel->setObjectName(QStringLiteral("ctr2ProxyEndpoints"));
    m_endpointsLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    // Docked width is tight: let the endpoints clip rather than widen the panel.
    m_endpointsLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    statusRow->addWidget(m_endpointsLabel, 1);
    m_trafficLabel = makeLabel(QString(), QStringLiteral("color.text.secondary"), statusPane);
    m_trafficLabel->setObjectName(QStringLiteral("ctr2ProxyTraffic"));
    m_trafficLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    statusRow->addWidget(m_trafficLabel);
    statusBox->addLayout(statusRow);
    m_errorLabel = makeLabel(QString(), QStringLiteral("color.accent.danger"), statusPane, true);
    m_errorLabel->setObjectName(QStringLiteral("ctr2ProxyError"));
    statusBox->addWidget(m_errorLabel);
    statusBox->addStretch(1);
    vbox->addWidget(statusPane, 1);

    connect(m_modeCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int idx) {
        if (m_model) {
            m_model->setTransport(idx == 1 ? Ctr2ProxyModel::Transport::Usb
                                           : Ctr2ProxyModel::Transport::Wifi);
        }
    });
    connect(m_listenCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int idx) {
        if (m_model) {
            m_model->setListenAddress(idx > 0 ? m_listenCombo->itemText(idx) : QString());
        }
    });
    connect(m_usbCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int idx) {
        if (m_model) {
            m_model->setUsbDevicePath(idx > 0 ? m_usbCombo->itemData(idx).toString() : QString());
        }
    });
    connect(m_listenPortEdit, &QLineEdit::textChanged, this, [this](const QString& text) {
        if (m_model) {
            m_model->setListenPortText(text);
        }
    });
    connect(m_refreshBtn, &QPushButton::clicked, this, [this] {
        if (m_model) {
            m_model->refreshDevices();
        }
    });
    connect(m_startBtn, &QPushButton::clicked, this, [this] {
        if (!m_model) {
            return;
        }
        if (m_model->isRunning()) {
            m_model->stop();
        } else {
            m_model->start();
        }
    });
}

void Ctr2ProxyApplet::setModel(Ctr2ProxyModel* model)
{
    if (m_model) {
        disconnect(m_model, nullptr, this, nullptr);
    }
    m_model = model;
    if (m_model) {
        connect(m_model, &Ctr2ProxyModel::listenAddressesChanged, this, &Ctr2ProxyApplet::syncAddresses);
        connect(m_model, &Ctr2ProxyModel::configurationChanged, this, &Ctr2ProxyApplet::syncConfiguration);
        connect(m_model, &Ctr2ProxyModel::stateChanged, this, &Ctr2ProxyApplet::syncConfiguration);
        connect(m_model, &Ctr2ProxyModel::stateChanged, this, &Ctr2ProxyApplet::syncStatus);
        connect(m_model, &Ctr2ProxyModel::endpointsChanged, this, &Ctr2ProxyApplet::syncStatus);
        connect(m_model, &Ctr2ProxyModel::lastErrorChanged, this, &Ctr2ProxyApplet::syncStatus);
        connect(m_model, &Ctr2ProxyModel::statsChanged, this, &Ctr2ProxyApplet::syncStats);
    }
    syncAddresses();
    syncConfiguration();
    syncStatus();
    syncStats();
}

void Ctr2ProxyApplet::syncAddresses()
{
    {
        const QSignalBlocker block(m_listenCombo);
        m_listenCombo->clear();
        // Placeholder first: the listen address is always an explicit choice.
        m_listenCombo->addItem(tr("Select address"));
        if (m_model) {
            m_listenCombo->addItems(m_model->availableListenAddresses());
            const int idx = m_listenCombo->findText(m_model->listenAddress());
            m_listenCombo->setCurrentIndex(idx > 0 ? idx : 0);
            if (idx <= 0 && !m_model->listenAddress().isEmpty() && !m_model->isRunning()) {
                m_model->setListenAddress(QString());
            }
        }
    }
    {
        const QSignalBlocker block(m_usbCombo);
        m_usbCombo->clear();
        m_usbCombo->addItem(tr("Select CTR2 USB device"));
        if (m_model) {
            for (const Ctr2HidPort::DeviceInfo& d : m_model->availableUsbDevices()) {
                m_usbCombo->addItem(d.label(), d.path);
            }
            const int idx = m_usbCombo->findData(m_model->usbDevicePath());
            m_usbCombo->setCurrentIndex(idx > 0 ? idx : 0);
            if (idx <= 0 && !m_model->usbDevicePath().isEmpty() && !m_model->isRunning()) {
                m_model->setUsbDevicePath(QString());
            }
        }
    }
}

void Ctr2ProxyApplet::syncConfiguration()
{
    const bool haveModel = m_model != nullptr;
    const bool running = haveModel && m_model->isRunning();
    const bool editable = haveModel && !running;
    const bool usb = haveModel && m_model->transport() == Ctr2ProxyModel::Transport::Usb;
    const QString frozen = !haveModel ? tr("Proxy unavailable")
                                      : tr("Stop the proxy to change its settings");

    if (haveModel) {
        const QSignalBlocker block(m_modeCombo);
        m_modeCombo->setCurrentIndex(usb ? 1 : 0);
    }
    setAvailability(m_modeCombo, editable, frozen);
    setAvailability(m_refreshBtn, editable, frozen);
    const QString wifiOnly = tr("Used in Wi-Fi mode only");
    setAvailability(m_listenCombo, editable && !usb, editable ? wifiOnly : frozen);
    setAvailability(m_listenPortEdit, editable && !usb, editable ? wifiOnly : frozen);
    const bool usbAvailable = haveModel && m_model->usbAvailable();
    const QString usbReason = !usbAvailable
        ? tr("This build has no USB HID support (hidapi)")
        : tr("Used in USB mode only");
    const bool usbEditable = editable && usb && usbAvailable;
    setAvailability(m_usbCombo, usbEditable, editable ? usbReason : frozen);

    if (haveModel) {
        if (m_listenPortEdit->text() != m_model->listenPortText()) {
            m_listenPortEdit->setText(m_model->listenPortText());
        }
    }

    if (haveModel) {
        const QString radio = m_model->aetherRadioLabel();
        m_radioLabel->setText(radio.isEmpty() ? tr("Not connected") : radio);
        m_radioLabel->setAccessibleName(tr("Relay radio: %1").arg(m_radioLabel->text()));
    }

    const QString problem = editable ? m_model->configurationProblem() : QString();
    m_problemLabel->setText(problem);
    m_problemLabel->setVisible(!problem.isEmpty());
    m_startBtn->setText(running ? tr("Stop") : tr("Start"));
    m_startBtn->setAccessibleName(running ? tr("Stop CTR2 proxy") : tr("Start CTR2 proxy"));
    m_startBtn->setEnabled(haveModel && (running || problem.isEmpty()));
    m_startBtn->setAccessibleDescription(
        !haveModel ? tr("Proxy unavailable") : (running ? QString() : problem));
}

void Ctr2ProxyApplet::syncStatus()
{
    if (!m_model) {
        m_stateLabel->setText(tr("State: Stopped"));
        m_endpointsLabel->clear();
        m_errorLabel->clear();
        m_errorLabel->hide();
        return;
    }
    m_stateLabel->setText(tr("State: %1").arg(m_model->stateText()));
    m_stateLabel->setAccessibleName(m_stateLabel->text());

    const bool usb = m_model->transport() == Ctr2ProxyModel::Transport::Usb;
    QStringList parts;
    if (!m_model->listenerEndpoint().isEmpty()) {
        parts << tr("Listen %1").arg(m_model->listenerEndpoint());
    }
    if (!m_model->peerEndpoint().isEmpty()) {
        parts << (usb ? tr("USB %1") : tr("CTR2 %1")).arg(m_model->peerEndpoint());
    }
    if (m_model->isRunning() && !m_model->radioEndpoint().isEmpty()) {
        parts << tr("Radio %1").arg(m_model->radioEndpoint());
    }
    m_endpointsLabel->setText(parts.join(QStringLiteral("\n")));
    m_endpointsLabel->setAccessibleName(parts.join(QStringLiteral(", ")));

    const QString err = m_model->lastError();
    m_errorLabel->setText(err.isEmpty() ? QString() : tr("Last error: %1").arg(err));
    m_errorLabel->setVisible(!err.isEmpty());
    m_errorLabel->setAccessibleName(m_errorLabel->text());
}

void Ctr2ProxyApplet::syncStats()
{
    if (!m_model) {
        m_trafficLabel->clear();
        return;
    }
    const TcpByteProxy::Stats s = m_model->stats();
    // Compact text for the narrow column; the accessible name spells it out.
    auto direction = [](const QString& label, quint64 sent, qint64 queued) {
        QString line = label.arg(formatBytes(sent));
        if (queued > 0) {
            line += tr(" (+%1 queued)").arg(formatBytes(static_cast<quint64>(queued)));
        }
        return line;
    };
    QStringList lines{direction(tr("\u2191 Radio %1"), s.toUpstream, s.queuedToUpstream),
                      direction(tr("\u2193 CTR2 %1"), s.toDownstream, s.queuedToDownstream)};
    QStringList spoken{
        tr("To radio %1, queued %2").arg(formatBytes(s.toUpstream),
                                         formatBytes(static_cast<quint64>(s.queuedToUpstream))),
        tr("To CTR2 %1, queued %2").arg(formatBytes(s.toDownstream),
                                        formatBytes(static_cast<quint64>(s.queuedToDownstream)))};
    if (s.rejectedClients > 0) {
        lines << tr("Rejected %1").arg(s.rejectedClients);
        spoken << tr("Rejected extra clients: %1").arg(s.rejectedClients);
    }
    if (m_model->transport() == Ctr2ProxyModel::Transport::Usb) {
        lines << tr("UDP \u2191%1 \u2193%2 \u2715%3")
                     .arg(s.datagramsToRadio).arg(s.datagramsToDevice).arg(s.datagramsDropped);
        spoken << tr("UDP: %1 to radio, %2 to CTR2, %3 dropped")
                      .arg(s.datagramsToRadio).arg(s.datagramsToDevice).arg(s.datagramsDropped);
    }
    const QString text = lines.join(QLatin1Char('\n'));
    m_trafficLabel->setText(text);
    m_trafficLabel->setAccessibleName(spoken.join(QStringLiteral(", ")));
}

} // namespace AetherSDR
