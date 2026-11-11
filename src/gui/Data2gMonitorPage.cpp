#include "Data2gMonitorPage.h"

#include "core/ThemeManager.h"
#include "models/Data2gReceiveModel.h"

#include <QCheckBox>
#include <QFontDatabase>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>

namespace AetherSDR {

namespace {

// Decoded text comes off the air: it is shown as plain text only, and the log
// is bounded so a busy channel cannot grow it without limit.
constexpr int kMaxLogBlocks = 4000;

constexpr const char* kData2gPageStyle = R"(
QWidget#Data2gMonitorPage {
    background: transparent;
}
QFrame#Data2gHeaderFrame,
QFrame#Data2gLogFrame,
QFrame#Data2gStatusFrame {
    background: {{color.canon.raised}};
    border: 1px solid {{color.canon.line}};
    border-radius: 7px;
}
QLabel#Data2gPageTitle {
    color: {{color.canon.ink}};
    background: transparent;
    font-size: 16px;
    font-weight: 700;
}
QLabel#Data2gModeLabel,
QLabel#Data2gEmptyHint {
    color: {{color.canon.muted}};
    background: transparent;
}
QLabel#Data2gFooterLabel {
    color: {{color.canon.muted}};
    background: transparent;
    font-size: 11px;
    font-weight: 700;
}
QLabel[data2gValue="true"] {
    color: {{color.canon.ink}};
    background: transparent;
    font-weight: 600;
}
QLabel#Data2gStateDot {
    background: {{color.canon.muted}};
    border-radius: 6px;
    min-width: 12px;
    max-width: 12px;
    min-height: 12px;
    max-height: 12px;
}
QLabel#Data2gStateDot[state="on"] {
    background: {{color.accent.success}};
}
QLabel#Data2gStateDot[state="blocked"] {
    background: {{color.accent.warning}};
}
QPlainTextEdit#data2gMonitorLog {
    color: {{color.canon.inkSoft}};
    background: {{color.canon.nested}};
    border: 1px solid {{color.canon.line}};
    border-radius: 5px;
    padding: 8px;
    selection-background-color: {{color.canon.control}};
}
)";

QLabel* footerLabel(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("Data2gFooterLabel"));
    return label;
}

QLabel* footerValue(const QString& objectName, const QString& accessibleName, QWidget* parent)
{
    auto* label = new QLabel(QStringLiteral("0"), parent);
    label->setObjectName(objectName);
    label->setProperty("data2gValue", true);
    label->setAccessibleName(accessibleName);
    return label;
}

} // namespace

Data2gMonitorPage::Data2gMonitorPage(Data2gReceiveModel* model, QWidget* parent)
    : QWidget(parent)
    , m_model(model)
{
    setObjectName(QStringLiteral("Data2gMonitorPage"));
    setAccessibleName(tr("HF Data monitor"));
    ThemeManager::instance().applyStyleSheet(this, QString::fromLatin1(kData2gPageStyle));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(10);

    buildHeader();
    buildLog();
    buildFooter();

    if (m_model) {
        connect(m_model, &Data2gReceiveModel::burstHeard,
                this, &Data2gMonitorPage::appendBurst);
        connect(m_model, &Data2gReceiveModel::statsChanged,
                this, &Data2gMonitorPage::refreshStats);
        connect(m_model, &Data2gReceiveModel::enabledChanged,
                this, &Data2gMonitorPage::refreshState);
    }

    // "Monitoring" must mean audio is arriving, so the state follows the tap.
    m_audioWatch = new QTimer(this);
    m_audioWatch->setInterval(1000);
    connect(m_audioWatch, &QTimer::timeout, this, &Data2gMonitorPage::refreshState);

    refreshState();
    refreshStats();
}

void Data2gMonitorPage::buildHeader()
{
    auto* frame = new QFrame(this);
    frame->setObjectName(QStringLiteral("Data2gHeaderFrame"));
    auto* row = new QHBoxLayout(frame);
    row->setContentsMargins(16, 12, 16, 12);
    row->setSpacing(14);

    auto* titles = new QVBoxLayout;
    titles->setSpacing(2);
    auto* title = new QLabel(tr("HF Data"), frame);
    title->setObjectName(QStringLiteral("Data2gPageTitle"));
    titles->addWidget(title);
    const QString version = Data2gReceiveModel::upstreamVersion();
    auto* mode = new QLabel(
        tr("Data2G %1 · Beta · Receive only").arg(version.isEmpty() ? tr("unavailable") : version),
        frame);
    mode->setObjectName(QStringLiteral("Data2gModeLabel"));
    mode->setAccessibleName(
        tr("Data2G version %1, beta, receive only").arg(version.isEmpty() ? tr("unavailable") : version));
    titles->addWidget(mode);
    row->addLayout(titles);
    row->addStretch(1);

    m_stateDot = new QLabel(frame);
    m_stateDot->setObjectName(QStringLiteral("Data2gStateDot"));
    m_stateDot->setAccessibleName(tr("HF Data monitor status"));
    row->addWidget(m_stateDot, 0, Qt::AlignVCenter);

    m_stateLabel = new QLabel(frame);
    m_stateLabel->setObjectName(QStringLiteral("data2gMonitorState"));
    m_stateLabel->setProperty("data2gValue", true);
    m_stateLabel->setAccessibleName(tr("HF Data monitor state"));
    row->addWidget(m_stateLabel, 0, Qt::AlignVCenter);

    m_monitorCheck = new QCheckBox(tr("Monitor"), frame);
    m_monitorCheck->setObjectName(QStringLiteral("data2gMonitorEnable"));
    m_monitorCheck->setAccessibleName(tr("Monitor Data2G on the modem's receive audio"));
    connect(m_monitorCheck, &QCheckBox::toggled, this, [this](bool on) {
        if (m_model) {
            m_model->setEnabled(on && m_modemEnabled);
        }
        refreshState();
    });
    row->addWidget(m_monitorCheck, 0, Qt::AlignVCenter);

    m_clearButton = new QPushButton(tr("Clear"), frame);
    m_clearButton->setObjectName(QStringLiteral("data2gMonitorClear"));
    m_clearButton->setAccessibleName(tr("Clear the HF Data monitor log"));
    connect(m_clearButton, &QPushButton::clicked, this, [this] {
        m_log->clear();
        m_emptyHint->setVisible(true);
    });
    row->addWidget(m_clearButton, 0, Qt::AlignVCenter);

    layout()->addWidget(frame);
}

void Data2gMonitorPage::buildLog()
{
    auto* frame = new QFrame(this);
    frame->setObjectName(QStringLiteral("Data2gLogFrame"));
    auto* column = new QVBoxLayout(frame);
    column->setContentsMargins(12, 12, 12, 12);
    column->setSpacing(8);

    m_emptyHint = new QLabel(
        tr("Nothing heard yet. Tune a slice in DIGU or USB to a Data2G frequency, "
           "enable the modem, then switch Monitor on. Every burst heard is listed "
           "here, whoever it is addressed to."),
        frame);
    m_emptyHint->setObjectName(QStringLiteral("Data2gEmptyHint"));
    m_emptyHint->setWordWrap(true);
    column->addWidget(m_emptyHint);

    m_log = new QPlainTextEdit(frame);
    m_log->setObjectName(QStringLiteral("data2gMonitorLog"));
    m_log->setAccessibleName(tr("HF Data bursts heard"));
    m_log->setReadOnly(true);
    m_log->setUndoRedoEnabled(false);
    m_log->setMaximumBlockCount(kMaxLogBlocks);
    m_log->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_log->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    column->addWidget(m_log, 1);

    static_cast<QVBoxLayout*>(layout())->addWidget(frame, 1);
}

void Data2gMonitorPage::buildFooter()
{
    auto* frame = new QFrame(this);
    frame->setObjectName(QStringLiteral("Data2gStatusFrame"));
    auto* row = new QHBoxLayout(frame);
    row->setContentsMargins(16, 8, 16, 8);
    row->setSpacing(10);

    row->addWidget(footerLabel(tr("BURSTS HEARD"), frame));
    m_burstsValue = footerValue(QStringLiteral("data2gMonitorBursts"),
                                tr("Data2G bursts heard"), frame);
    row->addWidget(m_burstsValue);
    row->addSpacing(18);
    row->addWidget(footerLabel(tr("AUDIO DROPPED"), frame));
    m_overflowsValue = footerValue(QStringLiteral("data2gMonitorOverflows"),
                                   tr("Times the monitor dropped audio it could not keep up with"),
                                   frame);
    row->addWidget(m_overflowsValue);
    row->addSpacing(18);
    row->addWidget(footerLabel(tr("DECODE FAULTS"), frame));
    m_faultsValue = footerValue(QStringLiteral("data2gMonitorFaults"),
                                tr("Data2G bursts the decoder rejected"), frame);
    row->addWidget(m_faultsValue);
    row->addStretch(1);

    layout()->addWidget(frame);
}

void Data2gMonitorPage::setModemEnabled(bool enabled)
{
    if (m_modemEnabled == enabled) {
        return;
    }
    m_modemEnabled = enabled;
    if (m_model) {
        m_model->setEnabled(enabled && m_monitorCheck->isChecked());
    }
    refreshState();
}

void Data2gMonitorPage::appendBurst(const Data2gHeardBurst& burst)
{
    // appendPlainText, never appendHtml: this text came off the air.
    QString text = burst.text;
    while (text.endsWith(QLatin1Char('\n'))) {
        text.chop(1);
    }
    m_log->appendPlainText(text);
    m_emptyHint->setVisible(false);
}

void Data2gMonitorPage::refreshState()
{
    const bool available = m_model && Data2gReceiveModel::available();
    const bool wanted = m_monitorCheck->isChecked();
    const bool running = available && m_model->isEnabled();

    QString reason;
    if (!available) {
        reason = tr("This build does not include the Data2G decoder.");
    } else if (!m_modemEnabled) {
        reason = tr("Enable the modem on the APRS tab first. "
                    "The monitor listens to the modem's receive audio.");
    }
    // A control that cannot work is dimmed with its reason, never hidden.
    m_monitorCheck->setEnabled(reason.isEmpty());
    m_monitorCheck->setToolTip(reason.isEmpty()
        ? tr("List every Data2G burst heard on the modem's receive audio. Receive only.")
        : reason);
    // The reason goes on the accessible channel too, not only the tooltip.
    m_monitorCheck->setAccessibleDescription(reason);

    if (running != m_audioWatch->isActive()) {
        running ? m_audioWatch->start() : m_audioWatch->stop();
    }

    QString state;
    QString dot;
    if (running && !m_model->audioFlowing()) {
        state = tr("Waiting for audio");
        dot = QStringLiteral("blocked");
        reason = tr("No receive audio is reaching the modem. "
                    "Connect to a radio and select a slice.");
    } else if (running) {
        state = tr("Monitoring");
        dot = QStringLiteral("on");
    } else if (!reason.isEmpty()) {
        state = !available ? tr("Unavailable") : tr("Modem is off");
        dot = wanted ? QStringLiteral("blocked") : QStringLiteral("off");
    } else {
        state = tr("Off");
        dot = QStringLiteral("off");
    }
    m_stateLabel->setText(state);
    m_stateLabel->setToolTip(reason);
    m_stateLabel->setAccessibleDescription(reason);
    m_stateDot->setToolTip(reason);
    if (m_stateDot->property("state").toString() != dot) {
        m_stateDot->setProperty("state", dot);
        // A property selector only re-evaluates when the widget is repolished.
        m_stateDot->style()->unpolish(m_stateDot);
        m_stateDot->style()->polish(m_stateDot);
    }
}

void Data2gMonitorPage::refreshStats()
{
    if (!m_model) {
        return;
    }
    const Data2gReceiveModel::Stats stats = m_model->stats();
    const QLocale locale;
    m_burstsValue->setText(locale.toString(stats.bursts));
    m_overflowsValue->setText(locale.toString(stats.overflows));
    m_faultsValue->setText(locale.toString(stats.faults));
}

} // namespace AetherSDR
