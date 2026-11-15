#include "Kpa500Applet.h"
#include "CanonIndicators.h"
#include "GuardedSlider.h"
#include "HGauge.h"
#include "models/MeterModel.h"
#include "core/AppSettings.h"
#include "core/ThemeManager.h"

#include <QAccessible>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDateTime>
#include <QFileDialog>
#include <QEvent>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QTextDocument>
#include <QVBoxLayout>
#include <algorithm>
#include <array>
#include <cmath>

namespace AetherSDR {
namespace {
class HistoryGraphLabel : public QLabel {
public:
    using QLabel::QLabel;
    QSize sizeHint() const override { return QSize(220, 310); }
    QSize minimumSizeHint() const override { return sizeHint(); }
};

QVector<HGauge::Tick> meterTicks(float minimum, float maximum, int divisions, int majorEvery)
{
    QVector<HGauge::Tick> ticks;
    for (int step = 0; step <= divisions; ++step) {
        const float value = minimum + (maximum - minimum) * step / divisions;
        ticks.append({value, step % majorEvery == 0 ? QString::number(value, 'g', 4) : QString()});
    }
    return ticks;
}

void labelText(QLabel* label, const QString& text)
{
    if (label->text() == text) { return; }
    label->setText(text);
    QAccessibleEvent event(label, QAccessible::NameChanged);
    QAccessible::updateAccessibility(&event);
}
void available(QWidget* control, bool enabled, const QString& reason)
{
    control->setEnabled(enabled);
    control->setAccessibleDescription(enabled ? QString() : reason);
    control->setToolTip(enabled ? QString() : reason);
}
bool routineDiagnostic(const QString& line)
{
    if (line.startsWith("RX ") || line.startsWith("DECODE ")) { return true; }
    if (!line.startsWith("TX ")) { return false; }
    const QString bytes = line.mid(3);
    if (bytes == ";") { return true; }
    if (!bytes.startsWith('^') || !bytes.endsWith(';')) { return false; }
    const QString command = bytes.mid(1, bytes.size() - 2);
    return Kpa500::fastPollCommands().contains(command)
        || Kpa500::slowPollCommands().contains(command)
        || Kpa500::connectCommands().contains(command);
}

QString accessoryCheckRules()
{
    auto& theme = ThemeManager::instance();
    QString rules;
    for (bool enabled : {true, false}) {
        for (bool checked : {false, true}) {
            const bool active = enabled && checked;
            const QColor fill = theme.color(active ? "color.accessory.key.meffa.active.background" : "color.background.2");
            const QColor edge = theme.color(active ? "color.accessory.key.meffa.active.border" : enabled ? "color.canon.inkSoft" : "color.canon.muted");
            const QColor ink = theme.color(!enabled ? "color.text.secondary" : active ? "color.accessory.key.meffa.active.foreground" : "color.text.primary");
            const QString key = fill.name(QColor::HexArgb) + edge.name(QColor::HexArgb) + ink.name(QColor::HexArgb);
            const QString path = themedImagePath("kpa-accessory-checks", checked ? "checked" : "unchecked", key, QSize(18, 18), 3,
                [fill, edge, ink, checked](QPainter& painter) {
                    painter.setPen(QPen(edge, 1.5));
                    painter.setBrush(fill);
                    painter.drawRoundedRect(QRectF(1.5, 1.5, 15, 15), 3, 3);
                    if (checked) {
                        QPainterPath tick;
                        tick.moveTo(5, 9); tick.lineTo(8, 12); tick.lineTo(13, 6);
                        painter.setPen(QPen(ink, 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                        painter.drawPath(tick);
                    }
                });
            if (!path.isEmpty()) {
                rules += QStringLiteral("QCheckBox::indicator:%1:%2 { image: url(\"%3\"); }")
                    .arg(checked ? "checked" : "unchecked", enabled ? "enabled" : "disabled", path);
            }
        }
    }
    return rules;
}

QJsonObject preferences()
{
    return QJsonDocument::fromJson(AppSettings::instance().value("Kpa500Applet").toString().toUtf8()).object();
}
QString reading(std::optional<float> value, int decimals, const QString& suffix)
{
    return value ? QString::number(*value, 'f', decimals) + suffix : QStringLiteral("—");
}
}

Kpa500Applet::Kpa500Applet(QWidget* parent) : CanonApplet(parent)
{
    setObjectName("Kpa500Applet");
    setAccessibleName(tr("Elecraft KPA500 amplifier"));
    setCloseButtonVisible(false);
    theme::setContainer(this, QStringLiteral("canon/kpa500"));
    setMinimumWidth(238);
    m_clock.start();
    const QJsonObject prefs = preferences();
    m_mini = prefs.value("MiniMode").toBool(false);
    m_historyExpanded = prefs.value("HistoryVisible").toBool(false);
    m_fahrenheit = prefs.value("Fahrenheit").toBool(false);
    auto* body = new QVBoxLayout(bodyWidget());
    body->setContentsMargins(0, 0, 0, 0);
    m_scroll = new QScrollArea(bodyWidget());
    m_scroll->setObjectName("kpaScroll");
    m_scroll->setAccessibleName(tr("Amplifier controls and telemetry"));
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setWidgetResizable(true);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_scroll->viewport()->setAutoFillBackground(false);
    m_content = new QWidget;
    m_content->setObjectName("kpaContent");
    m_scroll->setWidget(m_content);
    m_content->setAutoFillBackground(false);
    body->addWidget(m_scroll);
    auto* root = new QVBoxLayout(m_content);
    root->setSizeConstraint(QLayout::SetMinAndMaxSize);
    root->setContentsMargins(4, 2, 4, 2);
    root->setSpacing(2);
    const auto button = [this](const QString& text, const char* name) {
        auto* b = new QPushButton(text, this);
        b->setObjectName(QLatin1String(name));
        b->setAccessibleName(text);
        b->setAutoDefault(false);
        b->setFixedHeight(22);
        return b;
    };
    auto* header = new QHBoxLayout;
    header->setSpacing(4);
    m_power = button(QStringLiteral("—"), "kpaPower");
    m_power->setFixedWidth(44);
    m_power->setCheckable(true);
    header->addWidget(m_power);
    m_tracking = new QLabel(this);
    m_tracking->setObjectName("kpaTracking");
    m_tracking->setProperty("kpaPill", true);
    m_tracking->setAlignment(Qt::AlignCenter);
    m_tracking->setFixedHeight(18);
    m_tracking->setMinimumWidth(0);
    m_tracking->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    m_tracking->setTextFormat(Qt::PlainText);
    header->addWidget(m_tracking, 1);
    m_band = new GuardedComboBox(this);
    m_band->setObjectName("kpaBand");
    m_band->setFixedHeight(22);
    m_band->setAccessibleName(tr("Confirmed amplifier band"));
    for (int band = 0; band <= 10; ++band) { m_band->addItem(Kpa500::bandName(band)); }
    m_band->setPlaceholderText(QStringLiteral("—"));
    m_band->setCurrentIndex(-1);
    header->addWidget(m_band, 1);
    m_miniButton = button(tr("Mini"), "kpaMini");
    m_miniButton->setCheckable(true);
    m_miniButton->setFixedWidth(38);
    m_miniButton->setChecked(m_mini);
    header->addWidget(m_miniButton);
    root->addLayout(header);
    m_modesPane = new QWidget(this);
    auto* modes = new QHBoxLayout(m_modesPane);
    modes->setContentsMargins(0, 0, 0, 0);
    modes->setSpacing(2);
    m_standby = button(tr("Standby"), "kpaStandby");
    m_operate = button(tr("Operate"), "kpaOperate");
    m_standby->setCheckable(true);
    m_operate->setCheckable(true);
    modes->addWidget(m_standby);
    modes->addWidget(m_operate);
    m_clear = button(tr("Clear fault"), "kpaClearFault");
    modes->addWidget(m_clear);
    root->addWidget(m_modesPane);
    m_state = new QLabel(this);
    m_state->setObjectName("kpaState");
    m_state->setAccessibleName(tr("Amplifier state"));
    m_state->setAlignment(Qt::AlignCenter);
    m_state->setFixedHeight(18);
    root->addWidget(m_state);
    m_fault = new QLabel(this);
    m_fault->setObjectName("kpaFault");
    m_fault->setWordWrap(true);
    ThemeManager::instance().applyStyleSheet(m_fault, "QLabel { color: {{color.accent.danger}}; font-weight: bold; }");
    root->addWidget(m_fault);
    m_detail = new QWidget(this);
    auto* details = new QVBoxLayout(m_detail);
    details->setContentsMargins(0, 0, 0, 0);
    details->setSpacing(2);
    auto* driveRow = new QHBoxLayout;
    driveRow->setSpacing(4);
    m_driveReadout = new QLabel(this);
    m_driveReadout->setObjectName("kpaDriveReading");
    m_driveReadout->setFixedSize(62, 18);
    m_driveReadout->setAlignment(Qt::AlignCenter);
    m_driveReadout->setProperty("kpaPill", true);
    m_driveReadout->setAccessibleName(tr("DRV radio forward power"));
    driveRow->addWidget(m_driveReadout);
    m_driveGauge = new HGauge(0, 100, 100, "", "", meterTicks(0, 100, 20, 5), this, 100);
    m_driveGauge->setObjectName("kpaDriveGauge");
    m_driveGauge->setAccessibleName(tr("DRV radio forward power"));
    driveRow->addWidget(m_driveGauge, 1);
    details->addLayout(driveRow);
    setRadioDrivePower(std::nullopt);
    const std::array<int, 5> divisions{12, 16, 15, 10, 15};
    const std::array<int, 5> majorEvery{4, 4, 5, 5, 5};
    const std::array<float, 5> maxima{600, 5, m_fahrenheit ? 302.0f : 150.0f, 100, 30};
    const QStringList names{tr("Power"), tr("SWR"), tr("Temperature"), tr("Voltage"), tr("Current")};
    for (int i = 0; i < 5; ++i) {
        auto* row = new QHBoxLayout;
        row->setSpacing(4);
        m_readouts[i] = new QLabel(this);
        m_readouts[i]->setFixedSize(62, 18);
        m_readouts[i]->setAlignment(Qt::AlignCenter);
        m_readouts[i]->setProperty("kpaPill", true);
        m_readouts[i]->setObjectName(QStringLiteral("kpaReading%1").arg(i));
        m_readouts[i]->setAccessibleName(names[i]);
        row->addWidget(m_readouts[i]);
        const float minimum = i == 1 ? 1.0f : (i == 2 && m_fahrenheit ? 32.0f : 0.0f);
        m_gauges[i] = new HGauge(minimum, maxima[i], i == 1 ? 2.5f : maxima[i], "", "",
            meterTicks(minimum, maxima[i], divisions[i], majorEvery[i]), this,
            i == 1 ? 2.0f : maxima[i]);
        m_gauges[i]->setObjectName(QStringLiteral("kpaGauge%1").arg(i));
        m_gauges[i]->setAccessibleName(names[i]);
        if (i == 0) { m_gauges[i]->setWindowPeakEnabled(true); }
        row->addWidget(m_gauges[i], 1);
        if (i < 2) { root->addLayout(row); } else { details->addLayout(row); }
    }
    auto* auxiliaries = new QHBoxLayout;
    auxiliaries->setSpacing(4);
    auxiliaries->addWidget(new QLabel(tr("Fan min"), this));
    m_fan = new GuardedComboBox(this);
    m_fan->setObjectName("kpaFan");
    m_fan->setFixedHeight(22);
    m_fan->setAccessibleName(tr("Minimum fan speed"));
    m_fan->addItem(tr("Auto"));
    for (int speed = 1; speed <= 6; ++speed) { m_fan->addItem(QString::number(speed)); }
    m_fan->setPlaceholderText(QStringLiteral("—"));
    auxiliaries->addWidget(m_fan);
    m_speaker = new QCheckBox(tr("Speaker alarm"), this);
    m_speaker->setObjectName("kpaSpeaker");
    m_speaker->setAccessibleName(tr("Amplifier fault speaker"));
    auxiliaries->addWidget(m_speaker);
    auxiliaries->addStretch();
    details->addLayout(auxiliaries);
    root->addWidget(m_detail);
    m_toolsPane = new QWidget(this);
    auto* tools = new QHBoxLayout(m_toolsPane);
    tools->setContentsMargins(0, 0, 0, 0);
    tools->setSpacing(2);
    m_historyButton = button(tr("▸ History"), "kpaHistory");
    m_historyButton->setCheckable(true);
    m_historyButton->setChecked(m_historyExpanded);
    m_settingsButton = button(tr("▸ Settings"), "kpaSettings");
    m_settingsButton->setCheckable(true);
    tools->addWidget(m_historyButton);
    tools->addWidget(m_settingsButton);
    root->addWidget(m_toolsPane);
    m_historyPane = new QWidget(this);
    auto* history = new QVBoxLayout(m_historyPane);
    history->setContentsMargins(0, 0, 0, 0);
    history->setSpacing(4);
    auto* rangeRow = new QHBoxLayout;
    rangeRow->addWidget(new QLabel(tr("Telemetry history"), this));
    m_range = new GuardedComboBox(this);
    m_range->setObjectName("kpaHistoryRange");
    m_range->setAccessibleName(tr("History time range"));
    m_range->addItem(tr("1 min"), 60);
    m_range->addItem(tr("5 min"), 300);
    m_range->addItem(tr("10 min"), 600);
    m_range->setCurrentIndex(1);
    rangeRow->addStretch();
    rangeRow->addWidget(m_range);
    history->addLayout(rangeRow);
    m_graph = new HistoryGraphLabel(this);
    m_graph->setObjectName("kpaHistoryGraph");
    m_graph->setAccessibleName(tr("Power, SWR, temperature, voltage and current history"));
    m_graph->setMinimumHeight(310);
    m_graph->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    history->addWidget(m_graph, 1);
    root->addWidget(m_historyPane, 1);
    m_settingsPane = new QWidget(this);
    auto* settings = new QVBoxLayout(m_settingsPane);
    settings->setContentsMargins(0, 2, 0, 0);
    settings->setSpacing(5);
    m_follow = new QCheckBox(tr("Follow TX flag"), this);
    m_follow->setObjectName("kpaFollowTx");
    m_follow->setAccessibleName(tr("Follow TX flag"));
    m_follow->setChecked(prefs.value("FollowTx").toBool(true));
    m_follow->setToolTip(tr("Follow the TX-assigned slice band, including XIT and split. Band changes wait until RX. Power and Operate remain manual."));
    m_follow->setAccessibleDescription(m_follow->toolTip());
    settings->addWidget(m_follow);
    m_trackingDetails = new QLabel(this);
    m_trackingDetails->setObjectName("kpaTrackingDetails");
    m_trackingDetails->setAccessibleName(tr("Amplifier band tracking details"));
    m_trackingDetails->setWordWrap(true);
    m_trackingDetails->setTextFormat(Qt::PlainText);
    settings->addWidget(m_trackingDetails);
    auto* explanation = new QLabel(tr("Disable to park the amp using the band dropdown in the header. The header band always shows confirmed amplifier readback. Hover over Follow TX for the source frequency and pending changes."), this);
    explanation->setWordWrap(true);
    settings->addWidget(explanation);
    m_units = new QCheckBox(tr("Temperature in °F"), this);
    m_units->setObjectName("kpaFahrenheit");
    m_units->setAccessibleName(tr("Temperature in degrees Fahrenheit"));
    m_units->setChecked(m_fahrenheit);
    settings->addWidget(m_units);
    m_link = new QLabel(this);
    m_link->setObjectName("kpaConnectionInfo");
    m_link->setWordWrap(true);
    m_link->setAccessibleName(tr("Amplifier connection information"));
    m_link->setTextFormat(Qt::PlainText);
    settings->addWidget(m_link);
    m_identity = new QLabel(this);
    m_identity->setObjectName("kpaIdentity");
    m_identity->setWordWrap(true);
    m_identity->setAccessibleName(tr("Amplifier firmware and serial number"));
    m_identity->setTextFormat(Qt::PlainText);
    settings->addWidget(m_identity);
    auto* advanced = button(tr("Advanced…"), "kpaAdvancedSettings");
    advanced->setToolTip(tr("Open Peripherals > Advanced for configuration backups, restore, stored fault history and the power-on banner."));
    advanced->setAccessibleDescription(advanced->toolTip());
    settings->addWidget(advanced, 0, Qt::AlignLeft);
    connect(advanced, &QPushButton::clicked, this, &Kpa500Applet::advancedSettingsRequested);
    auto* cadence = new QLabel(tr("On: fast 200 ms / slow 500 ms · off: power check 5 s\nStale: 3 s fast / 8 s slow · history: 10 min · debug: 2,000 lines per view"), this);
    cadence->setWordWrap(true);
    settings->addWidget(cadence);
    m_historyHelp = new QLabel(tr("History continues in Mini mode. Gaps mean no fresh measurement; SWR is unmeasured without RF. Scales clip at the displayed limits."), this);
    m_historyHelp->setObjectName("kpaHistoryHelp");
    m_historyHelp->setWordWrap(true);
    settings->addWidget(m_historyHelp);
    auto* rfHelp = new QLabel(tr("RF ACTIVE means the amplifier reports forward power above zero. RCA KEY IN/PTT is not reported by the serial protocol. Radio TX separately holds power, mode and band changes until RX."), this);
    rfHelp->setObjectName("kpaRfHelp");
    rfHelp->setWordWrap(true);
    settings->addWidget(rfHelp);
    m_debugButton = button(tr("▸ Debug"), "kpaDebug");
    m_debugButton->setCheckable(true);
    settings->addWidget(m_debugButton);
    m_debugPane = new QWidget(this);
    auto* debug = new QVBoxLayout(m_debugPane);
    debug->setContentsMargins(0, 0, 0, 0);
    m_verbose = new QCheckBox(tr("Verbose"), this);
    m_verbose->setObjectName("kpaDebugVerbose");
    m_verbose->setAccessibleName(tr("Verbose amplifier protocol debug output"));
    m_verbose->setAccessibleDescription(tr("Include routine serial queries, raw replies and decoded telemetry. TX here means serial writes, not RF."));
    m_verbose->setToolTip(m_verbose->accessibleDescription());
    m_verbose->setChecked(prefs.value("VerboseDebug").toBool(false));
    debug->addWidget(m_verbose);
    auto* debugActions = new QHBoxLayout;
    m_pause = button(tr("Pause"), "kpaDebugPause");
    m_pause->setCheckable(true);
    auto* copy = button(tr("Copy"), "kpaDebugCopy");
    auto* save = button(tr("Save…"), "kpaDebugSave");
    debugActions->addWidget(m_pause);
    debugActions->addWidget(copy);
    debugActions->addWidget(save);
    debugActions->addStretch();
    debug->addLayout(debugActions);
    m_debug = new QPlainTextEdit(this);
    m_debug->setObjectName("kpaDebugOutput");
    m_debug->setAccessibleName(tr("Timestamped amplifier serial diagnostics"));
    m_debug->setReadOnly(true);
    m_debug->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_debug->setMaximumBlockCount(kDebugLines);
    m_debug->setFixedHeight(160);
    debug->addWidget(m_debug);
    settings->addWidget(m_debugPane);
    root->addWidget(m_settingsPane);
    m_bottomSpace = new QSpacerItem(0, 0, QSizePolicy::Minimum, QSizePolicy::Expanding);
    root->addItem(m_bottomSpace);
    m_graph->installEventFilter(this);

    connect(m_power, &QPushButton::clicked, this, [this] { emit powerRequested(!m_status.powerOn.value_or(false)); refresh(); });
    connect(m_standby, &QPushButton::clicked, this, [this] { emit operateToggled(false); refresh(); });
    connect(m_operate, &QPushButton::clicked, this, [this] { emit operateToggled(true); refresh(); });
    connect(m_clear, &QPushButton::clicked, this, &Kpa500Applet::clearFaultClicked);
    connect(m_band, &QComboBox::activated, this, [this](int band) { emit bandRequested(band); refresh(); });
    connect(m_fan, &QComboBox::activated, this, [this](int speed) { emit fanSpeedChangeRequested(speed); refresh(); });
    connect(m_speaker, &QCheckBox::clicked, this, [this](bool on) { emit speakerRequested(on); refresh(); });
    connect(m_follow, &QCheckBox::toggled, this, [this](bool on) { savePreferences(); emit followTxChanged(on); refresh(); });
    connect(m_units, &QCheckBox::toggled, this, [this](bool on) { m_fahrenheit = on;
        const float minimum = on ? 32.0f : 0.0f;
        const float maximum = on ? 302.0f : 150.0f;
        m_gauges[2]->setRange(minimum, maximum, maximum,
            meterTicks(minimum, maximum, 15, 5));
        savePreferences();
        refresh();
        m_gauges[2]->setValueImmediate(m_gauges[2]->value());
        renderHistory(); });
    connect(m_miniButton, &QPushButton::toggled, this, [this](bool on) { m_mini = on; savePreferences(); updatePresentation(); });
    connect(m_historyButton, &QPushButton::toggled, this, [this](bool on) { m_historyExpanded = on; if (on && m_mini) { m_miniButton->setChecked(false); } savePreferences(); updatePresentation(); });
    connect(m_settingsButton, &QPushButton::toggled, this, &Kpa500Applet::updatePresentation);
    connect(m_debugButton, &QPushButton::toggled, this, &Kpa500Applet::updatePresentation);
    connect(m_range, &QComboBox::currentIndexChanged, this, &Kpa500Applet::renderHistory);
    connect(m_verbose, &QCheckBox::toggled, this, [this](bool) {
        savePreferences();
        if (!m_pause->isChecked()) { m_debug->setPlainText(debugText()); }
    });
    connect(m_pause, &QPushButton::toggled, this, [this](bool paused) {
        m_pause->setText(paused ? tr("Resume") : tr("Pause"));
        if (!paused) { m_debug->setPlainText(debugText()); }
    });
    connect(copy, &QPushButton::clicked, this, [this] { QApplication::clipboard()->setText(m_pause->isChecked() ? m_debug->toPlainText() : debugText()); });
    connect(save, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getSaveFileName(this, tr("Save amplifier debug log"), "kpa500-debug.txt", tr("Text files (*.txt)"));
        if (path.isEmpty()) { return; }
        QSaveFile file(path);
        const QByteArray data = (m_pause->isChecked() ? m_debug->toPlainText() : debugText()).toUtf8();
        if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
            appendDiagnostic(tr("ERROR Could not save debug log: %1").arg(file.errorString()));
        }
    });
    m_timer.setInterval(500);
    connect(&m_timer, &QTimer::timeout, this, [this] { refresh(); if (m_historyPane->isVisible()) { renderHistory(); } });
    m_timer.start();
    applyCanonSheet(bodyWidget(), [this] {
        m_driveGauge->setTickMarkColor(ThemeManager::instance().color(this,
            m_driveGauge->isEnabled() ? "color.canon.muted" : "color.control.unavailable"));
        for (HGauge* gauge : m_gauges) {
            gauge->setTickMarkColor(ThemeManager::instance().color(this, "color.canon.muted"));
        }
        return QStringLiteral(
            "QWidget#kpaContent, QScrollArea#kpaScroll { background: {{color.canon.ground}}; border: none; }"
            "QLabel[kpaPill=true] { font-size: 9px; font-weight: bold; color: {{color.canon.muted}}; "
            "background: {{color.canon.ground}}; border: 1px solid {{color.canon.line}}; border-radius: 3px; padding: 0 2px; }"
            "QLabel#kpaDriveReading:disabled { color: {{color.control.unavailable}}; }"
            "QScrollBar:vertical { background: {{color.background.0}}; width: 12px; }"
            "QScrollBar:horizontal { background: {{color.background.0}}; height: 12px; }"
            "QScrollBar::handle { background: {{color.background.1}}; border-radius: 4px; min-width: 20px; min-height: 20px; }"
            "QScrollBar::handle:hover { background: {{color.canon.lineHi}}; }"
            "QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }"
            "QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }"
            "QLabel { color: {{color.canon.muted}}; background: transparent; font-size: 10px; }"
            "QPushButton, QComboBox { background: {{color.canon.control}}; "
            "color: {{color.canon.inkSoft}}; border: 1px solid {{color.canon.lineHi}}; border-radius: 3px; padding: 0 4px; font-size: 10px; font-weight: bold; }"
            "QPushButton:hover { background: {{color.canon.nested}}; color: {{color.canon.aqua}}; }"
            "QPushButton:checked { background: {{color.accessory.key.meffa.active.background}}; color: {{color.accessory.key.meffa.active.foreground}}; border-color: {{color.accessory.key.meffa.active.border}}; }"
            "QPushButton:checked:hover { background: {{color.accessory.key.meffa.active.hover}}; }"
            "QPushButton#kpaStandby:checked, QPushButton#kpaStandby:checked:hover { background: {{color.accessory.key.standby.background}}; color: {{color.accessory.key.standby.foreground}}; border-color: {{color.accessory.key.standby.foreground}}; }"
            "QPushButton:disabled, QPushButton:checked:disabled, QPushButton#kpaStandby:checked:disabled, QComboBox:disabled { background: {{color.background.2}}; color: {{color.text.secondary}}; border-color: {{color.background.2}}; }"
            "QCheckBox { color: {{color.canon.muted}}; font-size: 10px; spacing: 4px; }"
            "QCheckBox:disabled { color: {{color.text.secondary}}; }"
            "QPlainTextEdit { background: {{color.canon.ground}}; color: {{color.canon.inkSoft}}; border: 1px solid {{color.canon.lineHi}}; font-family: monospace; font-size: 10px; }") + canonIndicatorRules() + accessoryCheckRules();
    });
    updatePresentation();
    refresh();
}

bool Kpa500Applet::followTx() const { return m_follow->isChecked(); }
void Kpa500Applet::savePreferences()
{
    QJsonObject doc = preferences();
    doc["FollowTx"] = followTx();
    doc["MiniMode"] = m_mini;
    doc["HistoryVisible"] = m_historyExpanded;
    doc["Fahrenheit"] = m_fahrenheit;
    doc["VerboseDebug"] = m_verbose->isChecked();
    auto& settings = AppSettings::instance();
    settings.setValue("Kpa500Applet", QString::fromUtf8(QJsonDocument(doc).toJson(QJsonDocument::Compact)));
    settings.save();
}
void Kpa500Applet::applyStatus(const Kpa500::Status& status) { m_status = status; refresh(); }
void Kpa500Applet::setConnected(bool connected)
{
    m_connected = connected;
    if (!connected) { setResponding(false); }
    refresh();
}
void Kpa500Applet::setResponding(bool responding)
{
    m_responding = responding;
    if (!responding) {
        m_status = {};
        for (auto& series : m_history) {
            series.push_back({m_clock.elapsed(), std::nullopt});
            while (series.size() > kHistorySamples) { series.pop_front(); }
        }
    }
    refresh();
}
void Kpa500Applet::setConnectionDescription(const QString& text) { m_connection = text; refresh(); }
void Kpa500Applet::setBandTrackingText(const QString& text)
{
    labelText(m_trackingDetails, text);
    m_tracking->setToolTip(text);
    m_tracking->setAccessibleDescription(text);
}
void Kpa500Applet::setRadioTransmitting(bool transmitting) { m_radioTransmitting = transmitting; refresh(); }
void Kpa500Applet::updateRadioDrive(const MeterModel& meters, bool radioConnected)
{
    // Radio FWDPWR has its own freshness stamp, independent of AMP meter routing.
    setRadioDrivePower(radioConnected ? meters.fwdPowerIfLive() : std::nullopt);
}

void Kpa500Applet::setRadioDrivePower(std::optional<float> watts)
{
    if (watts && (!std::isfinite(*watts) || *watts < 0.0f)) { watts.reset(); }
    const QString text = tr("DRV %1").arg(reading(watts, 1, " W"));
    const QString source = tr("Connected radio's measured forward power, before cable losses; "
                             "not measured at the KPA500 input. Scale 0–100 W; the pill retains readings above the scale.");
    const QString description = source + " " + (watts ? text : tr("Unavailable: no fresh radio forward-power measurement or radio disconnected."));
    m_driveReadout->setAccessibleName(tr("DRV radio forward power: %1").arg(reading(watts, 1, " W")));
    labelText(m_driveReadout, text);
    for (QWidget* widget : std::array<QWidget*, 2>{m_driveReadout, m_driveGauge}) {
        widget->setEnabled(watts.has_value());
        widget->setToolTip(description);
        widget->setAccessibleDescription(description);
    }
    m_driveGauge->setTickMarkColor(ThemeManager::instance().color(this,
        watts ? "color.canon.muted" : "color.control.unavailable"));
    if (watts) { m_driveGauge->setValue(*watts); }
    else { m_driveGauge->setValueImmediate(0); m_driveGauge->clearPeak(); }
}

void Kpa500Applet::setIdentification(const QString& firmware, const QString& serial)
{
    m_status.firmwareVersion = firmware;
    m_status.serialNumber = serial;
    refresh();
}
void Kpa500Applet::setFloating(bool floating)
{
    m_floating = floating;
    if (floating) {
        m_miniButton->setChecked(false);
        m_historyButton->setChecked(true);
    }
    updatePresentation();
}
QSize Kpa500Applet::sizeHint() const
{
    const int width = std::max(238, m_content->layout()->minimumSize().width() + 2);
    const int height = m_content->layout()->totalHeightForWidth(width - 2);
    return QSize(width, (height >= 0 ? height : m_content->sizeHint().height()) + 2);
}
QSize Kpa500Applet::minimumSizeHint() const
{
    if (!m_floating) { return sizeHint(); }
    return QSize(std::max(238, m_content->layout()->minimumSize().width()
        + (m_mini ? 0 : m_scroll->verticalScrollBar()->sizeHint().width()) + 2),
        m_mini ? sizeHint().height() : 160);
}
bool Kpa500Applet::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_graph && event->type() == QEvent::Resize) { renderHistory(); }
    return CanonApplet::eventFilter(watched, event);
}
void Kpa500Applet::updatePresentation()
{
    m_detail->setVisible(!m_mini);
    m_modesPane->setVisible(!m_mini);
    m_toolsPane->setVisible(!m_mini);
    m_miniButton->setText(m_mini ? tr("Full") : tr("Mini"));
    m_historyPane->setVisible(m_historyExpanded && !m_mini);
    m_historyButton->setText(m_historyExpanded && !m_mini ? tr("▾ History") : tr("▸ History"));
    m_settingsPane->setVisible(m_settingsButton->isChecked() && !m_mini);
    m_settingsButton->setText(m_settingsButton->isChecked() ? tr("▾ Settings") : tr("▸ Settings"));
    m_debugPane->setVisible(m_debugButton->isChecked());
    m_debugButton->setText(m_debugButton->isChecked() ? tr("▾ Debug") : tr("▸ Debug"));
    if (m_debugButton->isChecked() && !m_pause->isChecked()) { m_debug->setPlainText(debugText()); }
    if (m_historyPane->isVisible()) { renderHistory(); }
    m_bottomSpace->changeSize(0, 0, QSizePolicy::Minimum,
        m_historyExpanded && !m_mini ? QSizePolicy::Fixed : QSizePolicy::Expanding);
    m_content->layout()->invalidate();
    m_content->layout()->activate();
    setMinimumWidth(minimumSizeHint().width());
    updateGeometry();
    if (parentWidget()) { parentWidget()->updateGeometry(); }
}

void Kpa500Applet::setMaintenanceBusy(bool busy)
{
    m_maintenanceBusy = busy;
    refresh();
}

void Kpa500Applet::refresh()
{
    const bool live = m_connected && m_responding;
    const bool powered = live && m_status.powerOn == true;
    const bool ampRfActive = powered && m_status.forwardPowerW.value_or(0.0f) > 0.0f;
    const bool busy = m_maintenanceBusy || m_radioTransmitting || m_status.forwardPowerW.value_or(0.0f) > 0.0f;
    const QString busyReason = m_maintenanceBusy ? tr("Unavailable during amplifier maintenance")
                                                  : tr("Unavailable while transmitting");
    const QString unavailable = m_maintenanceBusy ? busyReason : tr("Waiting for amplifier readback");
    available(m_power, m_connected && !busy, m_connected ? busyReason : tr("Connect the amplifier first"));
    m_power->setText(m_status.powerOn.has_value()
        ? (*m_status.powerOn ? tr("ON") : tr("OFF")) : QStringLiteral("—"));
    m_power->setChecked(powered);
    m_power->setAccessibleName(m_status.powerOn.has_value()
        ? tr("Amplifier power %1").arg(m_power->text()) : tr("Amplifier power unknown"));
    if (m_power->isEnabled()) {
        const QString action = m_status.powerOn == true
            ? tr("Power is ON. Click to turn the amplifier off.")
            : m_status.powerOn == false
                ? tr("Power is OFF. Click to wake the amplifier (rear power switch must be on).")
                : tr("Power state is unknown. Click to request amplifier power on (rear power switch must be on).");
        m_power->setToolTip(action);
        m_power->setAccessibleDescription(action);
    }
    available(m_standby, powered && m_status.operate.has_value() && !busy, busy ? busyReason : unavailable);
    available(m_operate, powered && m_status.operate.has_value() && !busy, busy ? busyReason : unavailable);
    m_standby->setChecked(powered && m_status.operate == false);
    m_operate->setChecked(powered && m_status.operate == true);
    available(m_band, powered && m_status.band.has_value() && !followTx() && !m_maintenanceBusy, m_maintenanceBusy ? busyReason : followTx() ? tr("Disable Follow TX flag in Settings to select a manual band") : unavailable);
    { const QSignalBlocker blocker(m_band); m_band->setCurrentIndex(m_status.band.value_or(-1)); }
    m_tracking->setVisible(followTx());
    m_band->setVisible(!followTx());
    labelText(m_tracking, tr("%1 · Follow TX").arg(m_status.band ? Kpa500::bandName(*m_status.band) : QStringLiteral("—")));
    available(m_fan, powered && m_status.fanMinSpeed.has_value() && !m_maintenanceBusy, unavailable);
    { const QSignalBlocker blocker(m_fan); m_fan->setCurrentIndex(m_status.fanMinSpeed.value_or(-1)); }
    available(m_speaker, live && m_status.speakerOn.has_value() && m_status.powerOn.value_or(false) && !m_maintenanceBusy, unavailable);
    { const QSignalBlocker blocker(m_speaker); m_speaker->setChecked(m_status.speakerOn.value_or(false)); }
    available(m_clear, powered && m_status.faultCode.value_or(0) > 0 && !busy, busy ? busyReason : tr("No confirmed fault to clear"));
    QString state;
    if (!m_connected) { state = tr("DISCONNECTED"); }
    else if (!m_responding) { state = tr("NO RESPONSE · power unknown"); }
    else {
        state = m_status.powerOn.has_value() ? (*m_status.powerOn ? tr("AMP ON") : tr("AMP OFF")) : tr("POWER UNKNOWN");
        if (m_status.powerOn != false) { state += m_status.operate.has_value() ? (*m_status.operate ? tr(" · OPERATE") : tr(" · STANDBY")) : tr(" · MODE UNKNOWN"); }
        if (ampRfActive) { state += tr(" · RF ACTIVE"); }
    }
    if (m_maintenanceBusy) { state += tr(" · MAINTENANCE"); }
    labelText(m_state, state);
    const QString stateToken = ampRfActive ? "color.accent.danger" : powered && m_status.operate == false ? "color.accessory.key.standby.foreground" : powered ? "color.accent.success" : "color.text.secondary";
    ThemeManager::instance().applyStyleSheet(m_state,
        "QLabel { background: {{color.canon.nested}}; color: {{" + stateToken + "}}; border: 1px solid {{color.canon.lineHi}}; border-radius: 3px; font-size: 9px; font-weight: bold; }");
    labelText(m_fault, m_status.faultCode.value_or(0) > 0 ? tr("FAULT %1 · amplifier reports an active fault").arg(*m_status.faultCode, 2, 10, QLatin1Char('0')) : QString());
    m_fault->setVisible(!m_fault->text().isEmpty());
    std::optional<float> temperature = m_status.paTemperatureC;
    if (temperature && m_fahrenheit) { temperature = *temperature * 1.8f + 32.0f; }
    labelText(m_readouts[0], tr("PWR %1").arg(reading(m_status.forwardPowerW, 0, " W")));
    labelText(m_readouts[1], tr("SWR %1").arg(m_status.swr ? reading(m_status.swr, 1, "") : m_status.forwardPowerW == 0.0f ? tr("NO RF") : QStringLiteral("—")));
    labelText(m_readouts[2], reading(temperature, 0, m_fahrenheit ? " °F" : " °C"));
    labelText(m_readouts[3], reading(m_status.paVoltageV, 1, " V"));
    labelText(m_readouts[4], reading(m_status.paCurrentA, 1, " A"));
    const std::array<std::optional<float>, 5> values{m_status.forwardPowerW, m_status.swr, temperature, m_status.paVoltageV, m_status.paCurrentA};
    for (int i = 0; i < 5; ++i) {
        if (values[i]) {
            m_gauges[i]->setValue(*values[i]);
        } else {
            m_gauges[i]->setValueImmediate(i == 1 ? 1.0f : 0.0f);
            m_gauges[i]->clearPeak();
        }
        m_gauges[i]->setAccessibleDescription(values[i] ? m_readouts[i]->text() : tr("No fresh measurement"));
        m_readouts[i]->setToolTip(tr("%1: %2").arg(m_gauges[i]->accessibleName(), m_readouts[i]->text()));
    }
    labelText(m_link, tr("%1 · %2\nLast valid reply: %3").arg(m_connection.isEmpty() ? tr("Connection not configured") : m_connection,
        live ? tr("Responding") : m_connected ? tr("Link open / no response") : tr("Disconnected"),
        m_lastReply < 0 ? tr("none") : tr("%1 s ago").arg((m_clock.elapsed() - m_lastReply) / 1000.0, 0, 'f', 1)));
    labelText(m_identity, tr("Firmware: %1 · Serial: %2").arg(m_status.firmwareVersion.value_or("—"), m_status.serialNumber.value_or("—")));
}

void Kpa500Applet::recordTelemetry(const QString& command, const Kpa500::Status& status)
{
    const qint64 now = m_clock.elapsed();
    m_lastReply = now;
    const auto append = [this, now](int index, std::optional<float> value) {
        auto& series = m_history[index];
        series.push_back({now, value});
        while (!series.empty() && (series.size() > kHistorySamples || now - series.front().time > kHistorySeconds * 1000)) { series.pop_front(); }
    };
    if (command == "WS") { append(0, status.forwardPowerW); append(1, status.swr); }
    if (command == "TM") { append(2, status.paTemperatureC); }
    if (command == "VI") { append(3, status.paVoltageV); append(4, status.paCurrentA); }
}
QString Kpa500Applet::debugText() const
{
    QStringList lines;
    const auto& buffer = m_verbose->isChecked() ? m_verboseDebugLines : m_debugLines;
    for (const QString& line : buffer) { lines.append(line); }
    return lines.join('\n');
}
void Kpa500Applet::appendDiagnostic(const QString& line)
{
    const QString stamped = QDateTime::currentDateTime().toString("HH:mm:ss.zzz ") + line.left(2048);
    const bool routine = routineDiagnostic(line);
    m_verboseDebugLines.push_back(stamped);
    while (m_verboseDebugLines.size() > kDebugLines) { m_verboseDebugLines.pop_front(); }
    if (!routine) {
        m_debugLines.push_back(stamped);
        while (m_debugLines.size() > kDebugLines) { m_debugLines.pop_front(); }
    }
    if (m_debugPane->isVisible() && !m_pause->isChecked() && (!routine || m_verbose->isChecked())) {
        m_debug->appendPlainText(stamped);
    }
}

void Kpa500Applet::renderHistory()
{
    const int width = std::max(220, m_graph->width());
    const int height = std::max(310, m_graph->height());
    const qreal rowHeight = (height - 20) / 5.0;
    const qreal dpr = devicePixelRatioF();
    QPixmap pixmap(QSize(width, height) * dpr);
    pixmap.setDevicePixelRatio(dpr);
    auto& theme = ThemeManager::instance();
    pixmap.fill(theme.color(this, "color.canon.nested"));
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    QFont font = painter.font(); font.setPixelSize(10); painter.setFont(font);
    const qint64 now = m_clock.elapsed();
    const qint64 span = m_range->currentData().toInt() * 1000;
    const std::array<float, 5> low{0, 1, m_fahrenheit ? 32.0f : 0.0f, 0, 0};
    const std::array<float, 5> high{600, 5, m_fahrenheit ? 302.0f : 150.0f, 100, 30};
    const QStringList labels{"Power · W", "SWR", m_fahrenheit ? "Temp · °F" : "Temp · °C", "Voltage · V", "Current · A"};
    QStringList summaries;
    for (int index = 0; index < 5; ++index) {
        const QRectF plot(8, index * rowHeight + 19, width - 48, rowHeight - 25);
        painter.setPen(theme.color(this, "color.canon.lineHi"));
        painter.drawRect(plot);
        painter.drawLine(QPointF(plot.center().x(), plot.top()), QPointF(plot.center().x(), plot.bottom()));
        painter.setPen(theme.color(this, "color.canon.muted"));
        painter.drawText(QPointF(8, index * rowHeight + 13), labels[index]);
        painter.drawText(QPointF(width - 35, plot.top() + 8), QString::number(high[index], 'f', 0));
        painter.drawText(QPointF(width - 35, plot.bottom()), QString::number(low[index], 'f', 0));
        QPainterPath path;
        bool connected = false;
        qint64 previous = -1;
        std::optional<float> peak;
        for (const Point& point : m_history[index]) {
            if (point.time < now - span) { continue; }
            if (!point.value) { connected = false; continue; }
            float value = *point.value;
            if (index == 2 && m_fahrenheit) { value = value * 1.8f + 32.0f; }
            peak = peak ? std::max(*peak, value) : value;
            const QPointF p(plot.left() + double(point.time - (now - span)) / span * plot.width(),
                plot.bottom() - std::clamp((value - low[index]) / (high[index] - low[index]), 0.0f, 1.0f) * plot.height());
            if (!connected || point.time - previous > (index < 2 ? 3000 : 8000)) { path.moveTo(p); }
            else { path.lineTo(p); }
            connected = true;
            previous = point.time;
        }
        painter.setPen(QPen(theme.color(this, index < 2 ? "color.canon.cyan" : "color.canon.aqua"), 1.5));
        painter.drawPath(path);
        if (peak) {
            const QString maximum = tr("peak %1").arg(*peak, 0, 'f', index == 0 || index == 2 ? 0 : 1);
            painter.drawText(QRectF(95, index * rowHeight + 1, width - 104, 16), Qt::AlignRight, maximum);
            summaries.append(labels[index] + ": " + maximum);
        }
    }
    painter.setPen(theme.color(this, "color.canon.muted"));
    painter.drawText(QPointF(8, height - 5), tr("−%1 min").arg(span / 60000));
    painter.drawText(QRectF(width - 45, height - 18, 38, 15), Qt::AlignRight, tr("now"));
    painter.end();
    m_graph->setPixmap(pixmap);
    m_graph->setAccessibleDescription(summaries.isEmpty() ? tr("No telemetry history yet") : summaries.join("; "));
}
} // namespace AetherSDR
