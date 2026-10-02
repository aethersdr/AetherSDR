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

QLabel* makeLabel(const QString& text, const QString& colorToken, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setWordWrap(true);
    ThemeManager::instance().applyStyleSheet(
        label, QStringLiteral("QLabel { color: {{%1}}; font-size: 10px; }").arg(colorToken));
    return label;
}

QString formatBytes(quint64 bytes)
{
    return QLocale::c().toString(bytes) + QStringLiteral(" B");
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

    auto* note = makeLabel(
        tr("Relays a CTR2 TCP connection to the radio unchanged. TCP only: no UDP, "
           "discovery or SmartLink. The CTR2 is its own radio client; its commands "
           "do not pass AetherSDR's transmit guards."),
        QStringLiteral("color.text.secondary"), this);
    note->setAccessibleName(tr("CTR2 proxy scope"));
    vbox->addWidget(note);

    auto* grid = new QGridLayout;
    grid->setHorizontalSpacing(4);
    grid->setVerticalSpacing(3);

    grid->addWidget(makeLabel(tr("Listen"), QStringLiteral("color.text.label"), this), 0, 0);
    m_listenCombo = new GuardedComboBox(this);
    m_listenCombo->setObjectName(QStringLiteral("ctr2ProxyListenAddress"));
    m_listenCombo->setAccessibleName(tr("CTR2 proxy listen address"));
    applyComboStyle(m_listenCombo, QStringLiteral("QComboBox { font-size: 10px; }"));
    m_listenCombo->setFixedHeight(20);
    grid->addWidget(m_listenCombo, 0, 1);

    m_listenPortEdit = new QLineEdit(this);
    m_listenPortEdit->setObjectName(QStringLiteral("ctr2ProxyListenPort"));
    m_listenPortEdit->setAccessibleName(tr("CTR2 proxy listen port"));
    m_listenPortEdit->setValidator(new QIntValidator(1, 65535, m_listenPortEdit));
    m_listenPortEdit->setFixedWidth(48);
    ThemeManager::instance().applyStyleSheet(m_listenPortEdit, kFieldStyle);
    grid->addWidget(m_listenPortEdit, 0, 2);

    m_refreshBtn = new QPushButton(tr("Rescan"), this);
    m_refreshBtn->setObjectName(QStringLiteral("ctr2ProxyRescan"));
    m_refreshBtn->setAccessibleName(tr("Rescan local addresses"));
    ThemeManager::instance().applyStyleSheet(m_refreshBtn, kButtonStyle);
    grid->addWidget(m_refreshBtn, 0, 3);

    grid->addWidget(makeLabel(tr("Radio"), QStringLiteral("color.text.label"), this), 1, 0);
    m_radioEdit = new QLineEdit(this);
    m_radioEdit->setObjectName(QStringLiteral("ctr2ProxyRadioAddress"));
    m_radioEdit->setAccessibleName(tr("Radio IPv4 address"));
    m_radioEdit->setPlaceholderText(tr("IPv4 address"));
    ThemeManager::instance().applyStyleSheet(m_radioEdit, kFieldStyle);
    grid->addWidget(m_radioEdit, 1, 1);

    m_radioPortEdit = new QLineEdit(this);
    m_radioPortEdit->setObjectName(QStringLiteral("ctr2ProxyRadioPort"));
    m_radioPortEdit->setAccessibleName(tr("Radio port"));
    m_radioPortEdit->setValidator(new QIntValidator(1, 65535, m_radioPortEdit));
    m_radioPortEdit->setFixedWidth(48);
    ThemeManager::instance().applyStyleSheet(m_radioPortEdit, kFieldStyle);
    grid->addWidget(m_radioPortEdit, 1, 2);

    m_startBtn = new QPushButton(tr("Start"), this);
    m_startBtn->setObjectName(QStringLiteral("ctr2ProxyStart"));
    m_startBtn->setAccessibleName(tr("Start CTR2 proxy"));
    ThemeManager::instance().applyStyleSheet(m_startBtn, kButtonStyle);
    grid->addWidget(m_startBtn, 1, 3);
    grid->setColumnStretch(1, 1);
    vbox->addLayout(grid);

    m_problemLabel = makeLabel(QString(), QStringLiteral("color.accent.warning"), this);
    m_problemLabel->setAccessibleName(tr("CTR2 proxy configuration"));
    vbox->addWidget(m_problemLabel);

    m_stateLabel = makeLabel(QString(), QStringLiteral("color.text.primary"), this);
    m_stateLabel->setObjectName(QStringLiteral("ctr2ProxyState"));
    vbox->addWidget(m_stateLabel);
    m_endpointsLabel = makeLabel(QString(), QStringLiteral("color.text.secondary"), this);
    m_endpointsLabel->setObjectName(QStringLiteral("ctr2ProxyEndpoints"));
    vbox->addWidget(m_endpointsLabel);
    m_trafficLabel = makeLabel(QString(), QStringLiteral("color.text.secondary"), this);
    m_trafficLabel->setObjectName(QStringLiteral("ctr2ProxyTraffic"));
    vbox->addWidget(m_trafficLabel);
    m_errorLabel = makeLabel(QString(), QStringLiteral("color.accent.danger"), this);
    m_errorLabel->setObjectName(QStringLiteral("ctr2ProxyError"));
    vbox->addWidget(m_errorLabel);

    connect(m_listenCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int idx) {
        if (m_model) {
            m_model->setListenAddress(idx > 0 ? m_listenCombo->itemText(idx) : QString());
        }
    });
    connect(m_listenPortEdit, &QLineEdit::textChanged, this, [this](const QString& text) {
        if (m_model) {
            m_model->setListenPortText(text);
        }
    });
    connect(m_radioEdit, &QLineEdit::textChanged, this, [this](const QString& text) {
        if (m_model) {
            m_model->setRadioAddress(text);
        }
    });
    connect(m_radioPortEdit, &QLineEdit::textChanged, this, [this](const QString& text) {
        if (m_model) {
            m_model->setRadioPortText(text);
        }
    });
    connect(m_refreshBtn, &QPushButton::clicked, this, [this] {
        if (m_model) {
            m_model->refreshListenAddresses();
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

void Ctr2ProxyApplet::syncConfiguration()
{
    const bool haveModel = m_model != nullptr;
    const bool running = haveModel && m_model->isRunning();
    const bool editable = haveModel && !running;
    const QString frozenReason = running ? tr("Stop the proxy to edit its endpoints")
                                         : (haveModel ? QString() : tr("Proxy unavailable"));

    for (QWidget* w : {static_cast<QWidget*>(m_listenCombo), static_cast<QWidget*>(m_listenPortEdit),
                       static_cast<QWidget*>(m_radioEdit), static_cast<QWidget*>(m_radioPortEdit),
                       static_cast<QWidget*>(m_refreshBtn)}) {
        w->setEnabled(editable);
        w->setAccessibleDescription(frozenReason);
    }

    if (haveModel) {
        if (m_listenPortEdit->text() != m_model->listenPortText()) {
            m_listenPortEdit->setText(m_model->listenPortText());
        }
        if (m_radioEdit->text() != m_model->radioAddress()) {
            m_radioEdit->setText(m_model->radioAddress());
        }
        if (m_radioPortEdit->text() != m_model->radioPortText()) {
            m_radioPortEdit->setText(m_model->radioPortText());
        }
    }

    const QString problem = editable ? m_model->configurationProblem() : QString();
    m_problemLabel->setText(problem);
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
        return;
    }
    m_stateLabel->setText(tr("State: %1").arg(m_model->stateText()));
    m_stateLabel->setAccessibleName(m_stateLabel->text());

    QStringList parts;
    if (!m_model->listenerEndpoint().isEmpty()) {
        parts << tr("Listening %1").arg(m_model->listenerEndpoint());
    }
    if (!m_model->peerEndpoint().isEmpty()) {
        parts << tr("CTR2 %1").arg(m_model->peerEndpoint());
    }
    if (m_model->isRunning() && !m_model->radioEndpoint().isEmpty()) {
        parts << tr("Radio %1").arg(m_model->radioEndpoint());
    }
    m_endpointsLabel->setText(parts.join(QStringLiteral("\n")));
    m_endpointsLabel->setAccessibleName(parts.join(QStringLiteral(", ")));

    const QString err = m_model->lastError();
    m_errorLabel->setText(err.isEmpty() ? QString() : tr("Last error: %1").arg(err));
    m_errorLabel->setAccessibleName(m_errorLabel->text());
}

void Ctr2ProxyApplet::syncStats()
{
    if (!m_model) {
        m_trafficLabel->clear();
        return;
    }
    const TcpByteProxy::Stats s = m_model->stats();
    QString text = tr("To radio %1 (queued %2)\nTo CTR2 %3 (queued %4)")
        .arg(formatBytes(s.toUpstream), formatBytes(static_cast<quint64>(s.queuedToUpstream)),
             formatBytes(s.toDownstream), formatBytes(static_cast<quint64>(s.queuedToDownstream)));
    if (s.rejectedClients > 0) {
        text += tr("\nRejected extra clients: %1").arg(s.rejectedClients);
    }
    m_trafficLabel->setText(text);
    m_trafficLabel->setAccessibleName(QString(text).replace(QLatin1Char('\n'), QStringLiteral(", ")));
}

} // namespace AetherSDR
