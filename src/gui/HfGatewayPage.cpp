#include "HfGatewayPage.h"

#include "core/ThemeManager.h"
#include "core/hfgateway/HfGatewaySettings.h"
#include "core/hfgateway/HostPortEngine.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFontDatabase>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QStyle>
#include <QTime>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace AetherSDR {

using HfGateway::HostPortEngine;
using ModemHost::Client;

namespace {

constexpr int kMaxLogBlocks = 4000;

constexpr const char* kPageStyle = R"(
QWidget#HfGatewayPage {
    background: transparent;
}
QFrame#HfGatewayHeaderFrame,
QFrame#HfGatewayEnginesFrame,
QFrame#HfGatewayHeardFrame,
QFrame#HfGatewayLogFrame {
    background: {{color.canon.raised}};
    border: 1px solid {{color.canon.line}};
    border-radius: 7px;
}
QFrame#HfGatewayEngineRow {
    background: {{color.canon.nested}};
    border: 1px solid {{color.canon.line}};
    border-radius: 5px;
}
QLabel#HfGatewayPageTitle {
    color: {{color.canon.ink}};
    background: transparent;
    font-size: 16px;
    font-weight: 700;
}
QLabel#HfGatewaySubtitle,
QLabel#HfGatewayHint {
    color: {{color.canon.muted}};
    background: transparent;
}
QLabel#HfGatewaySectionLabel {
    color: {{color.canon.muted}};
    background: transparent;
    font-size: 11px;
    font-weight: 700;
}
QLabel#HfGatewayEngineName {
    color: {{color.canon.ink}};
    background: transparent;
    font-weight: 600;
}
QLabel#HfGatewayEngineStatus {
    color: {{color.canon.inkSoft}};
    background: transparent;
}
QLabel#HfGatewayStateDot {
    background: {{color.canon.muted}};
    border-radius: 6px;
    min-width: 12px;
    max-width: 12px;
    min-height: 12px;
    max-height: 12px;
}
QLabel#HfGatewayStateDot[state="on"] {
    background: {{color.accent.success}};
}
QLabel#HfGatewayStateDot[state="blocked"] {
    background: {{color.accent.warning}};
}
QPlainTextEdit#hfGatewayLog,
QTreeWidget#hfGatewayHeard {
    color: {{color.canon.inkSoft}};
    background: {{color.canon.nested}};
    border: 1px solid {{color.canon.line}};
    border-radius: 5px;
}
)";

QLabel* sectionLabel(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("HfGatewaySectionLabel"));
    return label;
}

QFrame* card(const QString& objectName, QWidget* parent)
{
    auto* frame = new QFrame(parent);
    frame->setObjectName(objectName);
    return frame;
}

void setDot(QLabel* dot, const QString& state)
{
    if (dot->property("state").toString() == state) {
        return;
    }
    dot->setProperty("state", state);
    dot->style()->unpolish(dot);
    dot->style()->polish(dot);
}

QString profileName(HfGateway::Profile profile)
{
    switch (profile) {
    case HfGateway::Profile::Mercury: return HfGatewayPage::tr("Mercury");
    case HfGateway::Profile::Data2gHost: return HfGatewayPage::tr("Data2G (data2g-host)");
    case HfGateway::Profile::VaraCompatible: return HfGatewayPage::tr("VARA-compatible modem");
    }
    return {};
}

// A fresh engine id not already in use.
QString freshId(const HfGateway::Config& config, HfGateway::Profile profile)
{
    const QString base = HfGateway::toString(profile);
    for (int n = 1;; ++n) {
        const QString id = n == 1 ? base : QStringLiteral("%1-%2").arg(base).arg(n);
        bool used = false;
        for (const auto& e : config.engines) {
            used = used || e.id == id;
        }
        if (!used) {
            return id;
        }
    }
}

} // namespace

HfGatewayPage::HfGatewayPage(QWidget* parent)
    : QWidget(parent)
    , m_config(HfGateway::Settings::load())
{
    setObjectName(QStringLiteral("HfGatewayPage"));
    setAccessibleName(tr("HF Gateway modems"));
    ThemeManager::instance().applyStyleSheet(this, QString::fromLatin1(kPageStyle));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(10);

    buildHeader();
    buildEngines();
    buildHeard();
    buildLog();
    rebuildEngines();
}

HfGatewayPage::~HfGatewayPage() = default;

void HfGatewayPage::buildHeader()
{
    auto* frame = card(QStringLiteral("HfGatewayHeaderFrame"), this);
    auto* column = new QVBoxLayout(frame);
    column->setContentsMargins(16, 12, 16, 12);
    column->setSpacing(4);

    auto* title = new QLabel(tr("HF Gateway"), frame);
    title->setObjectName(QStringLiteral("HfGatewayPageTitle"));
    column->addWidget(title);
    auto* subtitle = new QLabel(tr("Receive only · Stage 1"), frame);
    subtitle->setObjectName(QStringLiteral("HfGatewaySubtitle"));
    column->addWidget(subtitle);
    auto* hint = new QLabel(
        tr("Connect to the HF data modems this station runs: Mercury, data2g-host, or a "
           "VARA-compatible modem you installed yourself. Each modem's VARA-style host port "
           "is shown here. Nothing on this page can transmit, and no modem may answer a "
           "call through it. Some modems accept one host program per port, so connecting "
           "here keeps other programs out of that modem until you disconnect."),
        frame);
    hint->setObjectName(QStringLiteral("HfGatewayHint"));
    hint->setWordWrap(true);
    column->addWidget(hint);

    layout()->addWidget(frame);
}

void HfGatewayPage::buildEngines()
{
    auto* frame = card(QStringLiteral("HfGatewayEnginesFrame"), this);
    auto* column = new QVBoxLayout(frame);
    column->setContentsMargins(12, 10, 12, 12);
    column->setSpacing(8);
    column->addWidget(sectionLabel(tr("MODEMS"), frame));

    m_noEnginesHint = new QLabel(tr("No modems yet. Add one below."), frame);
    m_noEnginesHint->setObjectName(QStringLiteral("HfGatewayHint"));
    column->addWidget(m_noEnginesHint);

    m_engineRows = new QVBoxLayout;
    m_engineRows->setSpacing(6);
    column->addLayout(m_engineRows);

    auto* add = new QHBoxLayout;
    add->setSpacing(6);
    m_addProfile = new QComboBox(frame);
    m_addProfile->setAccessibleName(tr("Kind of modem to add"));
    for (const auto p : {HfGateway::Profile::Mercury, HfGateway::Profile::Data2gHost,
                         HfGateway::Profile::VaraCompatible}) {
        m_addProfile->addItem(profileName(p), static_cast<int>(p));
    }
    add->addWidget(m_addProfile);
    m_addLabel = new QLineEdit(frame);
    m_addLabel->setPlaceholderText(tr("Name (optional)"));
    m_addLabel->setAccessibleName(tr("Name for the modem"));
    m_addLabel->setMaxLength(64);
    add->addWidget(m_addLabel, 1);
    m_addHost = new QLineEdit(QStringLiteral("127.0.0.1"), frame);
    m_addHost->setAccessibleName(tr("Host the modem runs on"));
    m_addHost->setMaxLength(253);
    add->addWidget(m_addHost, 1);
    m_addPort = new QSpinBox(frame);
    m_addPort->setRange(1024, 65534);
    m_addPort->setValue(8300);
    m_addPort->setAccessibleName(tr("Modem command port; the data port is the next one"));
    m_addPort->setToolTip(tr("Command port. The data port is the next one up."));
    add->addWidget(m_addPort);
    auto* addButton = new QPushButton(tr("Add"), frame);
    addButton->setAccessibleName(tr("Add this modem"));
    connect(addButton, &QPushButton::clicked, this, &HfGatewayPage::addEngine);
    add->addWidget(addButton);
    column->addLayout(add);

    layout()->addWidget(frame);
}

void HfGatewayPage::buildHeard()
{
    auto* frame = card(QStringLiteral("HfGatewayHeardFrame"), this);
    auto* column = new QVBoxLayout(frame);
    column->setContentsMargins(12, 10, 12, 12);
    column->setSpacing(6);
    column->addWidget(sectionLabel(tr("HEARD (MERCURY MONITOR, CQ FRAMES)"), frame));

    m_heard = new QTreeWidget(frame);
    m_heard->setObjectName(QStringLiteral("hfGatewayHeard"));
    m_heard->setAccessibleName(tr("Stations heard"));
    m_heard->setRootIsDecorated(false);
    m_heard->setHeaderLabels({tr("Callsign"), tr("Frame"), tr("Bandwidth"), tr("SNR"), tr("Last heard (UTC)"),
                              tr("Count"), tr("Modem")});
    m_heard->header()->setStretchLastSection(true);
    m_heard->setMinimumHeight(120);
    column->addWidget(m_heard);

    static_cast<QVBoxLayout*>(layout())->addWidget(frame, 1);
}

void HfGatewayPage::buildLog()
{
    auto* frame = card(QStringLiteral("HfGatewayLogFrame"), this);
    auto* column = new QVBoxLayout(frame);
    column->setContentsMargins(12, 10, 12, 12);
    column->setSpacing(6);
    column->addWidget(sectionLabel(tr("EVENTS"), frame));

    m_log = new QPlainTextEdit(frame);
    m_log->setObjectName(QStringLiteral("hfGatewayLog"));
    m_log->setAccessibleName(tr("HF Gateway events"));
    m_log->setReadOnly(true);
    m_log->setUndoRedoEnabled(false);
    m_log->setMaximumBlockCount(kMaxLogBlocks);
    m_log->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_log->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_log->setMinimumHeight(100);
    column->addWidget(m_log);

    static_cast<QVBoxLayout*>(layout())->addWidget(frame, 1);
}

void HfGatewayPage::rebuildEngines()
{
    while (!m_engines.isEmpty()) {
        removeEngineRow(static_cast<int>(m_engines.size()) - 1);
    }
    for (const HfGateway::EngineConfig& c : m_config.engines) {
        addEngineRow(c);
    }
}

void HfGatewayPage::addEngineRow(const HfGateway::EngineConfig& config)
{
    auto* engine = new HostPortEngine(config, this);
    m_engines.append(engine);

    auto* row = card(QStringLiteral("HfGatewayEngineRow"), this);
    auto* h = new QHBoxLayout(row);
    h->setContentsMargins(10, 6, 10, 6);
    h->setSpacing(10);
    auto* dot = new QLabel(row);
    dot->setObjectName(QStringLiteral("HfGatewayStateDot"));
    h->addWidget(dot, 0, Qt::AlignVCenter);
    auto* texts = new QVBoxLayout;
    texts->setSpacing(1);
    auto* name = new QLabel(row);
    name->setObjectName(QStringLiteral("HfGatewayEngineName"));
    texts->addWidget(name);
    auto* status = new QLabel(row);
    status->setObjectName(QStringLiteral("HfGatewayEngineStatus"));
    texts->addWidget(status);
    h->addLayout(texts, 1);

    if (engine->monitorSupported()) {
        auto* monitor = new QCheckBox(tr("Monitor"), row);
        monitor->setObjectName(QStringLiteral("HfGatewayMonitor"));
        monitor->setToolTip(tr("Mercury's passive monitor: lists every connect and CQ it hears. Read-only, "
                               "never transmits. Uses several times the modem's CPU."));
        monitor->setAccessibleName(tr("Mercury monitor for %1").arg(engineLabel(engine)));
        connect(monitor, &QCheckBox::toggled, engine, &HostPortEngine::setMonitor);
        h->addWidget(monitor);
    }
    auto* connectButton = new QPushButton(row);
    connectButton->setObjectName(QStringLiteral("HfGatewayConnect"));
    connect(connectButton, &QPushButton::clicked, engine, [engine] {
        if (engine->state() == Client::State::Disconnected) {
            engine->connectToModem();
        } else {
            engine->disconnectFromModem();
        }
    });
    h->addWidget(connectButton);
    auto* remove = new QPushButton(tr("Remove"), row);
    remove->setAccessibleName(tr("Remove %1").arg(engineLabel(engine)));
    connect(remove, &QPushButton::clicked, this, [this, engine] {
        const int index = static_cast<int>(m_engines.indexOf(engine));
        if (index >= 0) {
            removeEngine(index);
        }
    });
    h->addWidget(remove);
    m_engineRows->addWidget(row);

    connect(engine, &HostPortEngine::changed, this, [this, engine] {
        refreshEngineRow(static_cast<int>(m_engines.indexOf(engine)));
    });
    connect(engine, &HostPortEngine::heardChanged, this, &HfGatewayPage::refreshHeard);
    connect(engine, &HostPortEngine::logLine, this, [this, engine](const QString& text, bool warning) {
        appendLog(engineLabel(engine), text, warning);
    });
    refreshEngineRow(static_cast<int>(m_engines.size()) - 1);
    m_noEnginesHint->setVisible(false);
}

void HfGatewayPage::removeEngineRow(int index)
{
    if (index < 0 || index >= m_engines.size()) {
        return;
    }
    HostPortEngine* engine = m_engines.takeAt(index);
    engine->disconnect(this);
    engine->disconnectFromModem();
    engine->deleteLater();
    if (QLayoutItem* item = m_engineRows->takeAt(index)) {
        if (QWidget* w = item->widget()) {
            w->deleteLater();
        }
        delete item;
    }
    m_noEnginesHint->setVisible(m_engines.isEmpty());
    refreshHeard();
}

void HfGatewayPage::refreshEngineRow(int index)
{
    if (index < 0 || index >= m_engines.size() || index >= m_engineRows->count()) {
        return;
    }
    const HostPortEngine* engine = m_engines.at(index);
    QWidget* row = m_engineRows->itemAt(index)->widget();
    if (!row) {
        return;
    }
    auto* dot = row->findChild<QLabel*>(QStringLiteral("HfGatewayStateDot"));
    auto* name = row->findChild<QLabel*>(QStringLiteral("HfGatewayEngineName"));
    auto* status = row->findChild<QLabel*>(QStringLiteral("HfGatewayEngineStatus"));
    auto* connectButton = row->findChild<QPushButton*>(QStringLiteral("HfGatewayConnect"));

    const HfGateway::EngineConfig& c = engine->config();
    name->setText(QStringLiteral("%1 — %2:%3").arg(engineLabel(engine), c.host).arg(c.commandPort));

    QStringList parts;
    QString dotState = QStringLiteral("off");
    switch (engine->state()) {
    case Client::State::Disconnected:
        parts << tr("Not connected");
        connectButton->setText(tr("Connect"));
        break;
    case Client::State::Connecting:
        parts << tr("Connecting…");
        dotState = QStringLiteral("blocked");
        connectButton->setText(tr("Cancel"));
        break;
    case Client::State::Connected:
        parts << tr("Connected");
        dotState = QStringLiteral("on");
        connectButton->setText(tr("Disconnect"));
        if (!engine->version().isEmpty()) {
            parts << engine->version();
        }
        parts << (engine->busy() ? tr("Channel busy") : tr("Channel clear"));
        if (engine->hasSn()) {
            parts << tr("SN %1 dB").arg(engine->snDb(), 0, 'f', 1);
        }
        if (engine->monitor()) {
            parts << tr("Monitoring");
        }
        break;
    }
    if (!HfGateway::isLoopbackHost(c.host)) {
        parts << tr("Remote host");
    }
    const QString text = parts.join(QStringLiteral(" · "));
    status->setText(text);
    status->setAccessibleName(tr("%1: %2").arg(engineLabel(engine), text));
    connectButton->setAccessibleName(tr("%1 %2").arg(connectButton->text(), engineLabel(engine)));
    setDot(dot, dotState);
}

void HfGatewayPage::refreshHeard()
{
    m_heard->clear();
    for (const HostPortEngine* engine : m_engines) {
        for (const HfGateway::HeardStation& h : engine->heard()) {
            auto* item = new QTreeWidgetItem(m_heard);
            item->setText(0, h.callsign);
            item->setText(1, h.kind);
            item->setText(2, h.bandwidthHz > 0 ? tr("%1 Hz").arg(h.bandwidthHz) : QString());
            item->setText(3, h.snDb != 0.0 ? QString::number(h.snDb, 'f', 1) : QString());
            item->setText(4, h.lastHeardUtc.toString(QStringLiteral("HH:mm:ss")));
            item->setText(5, QString::number(h.count));
            item->setText(6, engineLabel(engine));
        }
    }
    m_heard->sortItems(4, Qt::DescendingOrder);
}

void HfGatewayPage::addEngine()
{
    if (m_config.engines.size() >= HfGateway::Config::kMaxEngines) {
        appendLog(tr("Gateway"), tr("At most %1 modems.").arg(HfGateway::Config::kMaxEngines), true);
        return;
    }
    const QString host = m_addHost->text().trimmed();
    if (!HfGateway::isValidHost(host)) {
        appendLog(tr("Gateway"), tr("Enter the host the modem runs on."), true);
        return;
    }
    HfGateway::EngineConfig e;
    e.profile = static_cast<HfGateway::Profile>(m_addProfile->currentData().toInt());
    e.id = freshId(m_config, e.profile);
    e.label = m_addLabel->text().trimmed();
    e.host = host;
    e.commandPort = static_cast<quint16>(m_addPort->value());
    e.dataPort = static_cast<quint16>(m_addPort->value() + 1);
    m_config.engines.append(e);
    saveConfig();
    m_addLabel->clear();
    if (m_config.engines.size() == m_engines.size() + 1) {
        addEngineRow(m_config.engines.last());
    } else {
        rebuildEngines();
    }
}

void HfGatewayPage::removeEngine(int index)
{
    if (index < 0 || index >= m_config.engines.size()) {
        return;
    }
    m_config.engines.removeAt(index);
    saveConfig();
    removeEngineRow(index);
    if (m_config.engines.size() != m_engines.size()) {
        rebuildEngines();
    }
}

void HfGatewayPage::saveConfig()
{
    HfGateway::Settings::save(m_config);
    // Read back through the validating boundary, so the page shows exactly
    // what is stored.
    m_config = HfGateway::Settings::load();
}

void HfGatewayPage::appendLog(const QString& engineLabel, const QString& text, bool warning)
{
    const QString stamp = QTime::currentTime().toString(QStringLiteral("HH:mm:ss"));
    // appendPlainText, never appendHtml: modem text is untrusted.
    m_log->appendPlainText(QStringLiteral("%1  %2%3: %4")
                               .arg(stamp, warning ? QStringLiteral("! ") : QString(), engineLabel, text));
}

QString HfGatewayPage::engineLabel(const HostPortEngine* engine) const
{
    const HfGateway::EngineConfig& c = engine->config();
    return c.label.isEmpty() ? profileName(c.profile) : c.label;
}

} // namespace AetherSDR
