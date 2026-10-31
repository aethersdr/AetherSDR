#include "Kat500Applet.h"
#include "CanonIndicators.h"
#include "CanonWindow.h"
#include "HGauge.h"
#include "core/AppSettings.h"
#include "core/ThemeManager.h"
#include "models/Kat500Model.h"
#include <QAccessible>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QColor>
#include <QFileDialog>
#include <QFont>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QScopedValueRollback>
#include <QResizeEvent>
#include <QShowEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStyle>
#include <QVBoxLayout>
#include <QVector>
#include <algorithm>
namespace AetherSDR
{
class Kat500DebugOutput : public QPlainTextEdit
{
  public:
    explicit Kat500DebugOutput(QWidget* parent) : QPlainTextEdit(parent)
    {
        connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int value) {
            if (!m_updating) {
                m_followLatest = value == verticalScrollBar()->maximum();
            }
        });
    }
    void appendDiagnostic(const QString& line)
    {
        const QScopedValueRollback<bool> guard(m_updating, true);
        appendPlainText(line);
        followLatest();
    }
    void showLatest(const QString& content)
    {
        const QScopedValueRollback<bool> guard(m_updating, true);
        m_followLatest = true;
        setPlainText(content);
        followLatest();
    }

  protected:
    void resizeEvent(QResizeEvent* event) override
    {
        const QScopedValueRollback<bool> guard(m_updating, true);
        QPlainTextEdit::resizeEvent(event);
        followLatest();
    }
    void showEvent(QShowEvent* event) override
    {
        const QScopedValueRollback<bool> guard(m_updating, true);
        QPlainTextEdit::showEvent(event);
        followLatest();
    }

  private:
    void followLatest()
    {
        if (m_followLatest) {
            verticalScrollBar()->setValue(verticalScrollBar()->maximum());
        }
    }
    bool m_followLatest = true;
    bool m_updating = false;
};
namespace
{
class HistoryGraphLabel : public QLabel
{
  public:
    using QLabel::QLabel;
    // The rendered pixmap must not become the layout's preferred size.
    QSize sizeHint() const override { return QSize(220, 270); }
    QSize minimumSizeHint() const override { return sizeHint(); }
};
void availability(QWidget* widget, const QString& reason, const QString& help = {})
{
    widget->setEnabled(reason.isEmpty());
    const QString detail = help.isEmpty() ? reason
        : reason.isEmpty() ? help : help + QStringLiteral("\n") + reason;
    widget->setToolTip(detail);
    widget->setAccessibleDescription(detail);
}
QString flag(std::optional<bool> value)
{
    return value ? (*value ? QStringLiteral("ON") : QStringLiteral("OFF")) : QStringLiteral("unknown");
}
QString reading(std::optional<double> value)
{
    return value ? QString::number(*value, 'f', 2) : QStringLiteral("—");
}
template <typename TextWidget>
void text(TextWidget* label, const QString& value)
{
    if (label->text() == value) {
        return;
    }
    label->setText(value);
    label->setAccessibleName(value);
    QAccessibleEvent event(label, QAccessible::NameChanged);
    QAccessible::updateAccessibility(&event);
}
template <typename TextWidget>
void pill(TextWidget* label, const QString& value, const QString& detail, const char* tone)
{
    text(label, value);
    label->setToolTip(detail);
    label->setAccessibleDescription(detail);
    const QString state = QString::fromLatin1(tone);
    if (label->property("katTone").toString() != state) {
        label->setProperty("katTone", state);
        label->style()->unpolish(label);
        label->style()->polish(label);
        label->update();
    }
}
QString checkRules()
{
    auto& theme = ThemeManager::instance();
    QString rules = canonIndicatorRules();
    for (bool enabled : {true, false}) {
        for (bool checked : {false, true}) {
            const bool active = enabled && checked;
            const QColor fill = theme.color(active ? "color.accessory.key.meffa.active.background"
                                                   : "color.canon.control");
            const QColor edge = theme.color(enabled ? "color.canon.inkSoft" : "color.canon.muted");
            const QColor ink = theme.color(active ? "color.accessory.key.meffa.active.foreground"
                                                  : "color.canon.muted");
            const QString key = fill.name(QColor::HexArgb) + edge.name(QColor::HexArgb) +
                                ink.name(QColor::HexArgb);
            const QString path = themedImagePath(
                "kat-checks", checked ? "checked" : "unchecked", key, QSize(18, 18), 3,
                [fill, edge, ink, checked](QPainter& painter) {
                    painter.setPen(QPen(edge, 1.5));
                    painter.setBrush(fill);
                    painter.drawRoundedRect(QRectF(1.5, 1.5, 15, 15), 3, 3);
                    if (checked) {
                        QPainterPath tick;
                        tick.moveTo(5, 9);
                        tick.lineTo(8, 12);
                        tick.lineTo(13, 6);
                        painter.setBrush(Qt::NoBrush);
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
    return QJsonDocument::fromJson(AppSettings::instance().value("Kat500Applet").toString().toUtf8())
        .object();
}
} // namespace
Kat500Applet::Kat500Applet(QWidget* parent) : CanonApplet(parent)
{
    setObjectName("Kat500Applet");
    setAccessibleName(tr("Elecraft KAT500 tuner"));
    setCloseButtonVisible(false);
    theme::setContainer(this, QStringLiteral("canon/kat500"));
    setMinimumWidth(238);
    m_clock.start();
    const QJsonObject prefs = preferences();
    auto* body = new QVBoxLayout(bodyWidget());
    body->setContentsMargins(0, 0, 0, 0);
    m_scroll = new QScrollArea(bodyWidget());
    m_scroll->setObjectName("katScroll");
    m_scroll->setAccessibleName(tr("Tuner controls and telemetry"));
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_content = new QWidget;
    m_content->setObjectName("katContent");
    m_scroll->viewport()->setObjectName("katViewport");
    m_scroll->setWidget(m_content);
    body->addWidget(m_scroll);
    auto* root = new QVBoxLayout(m_content);
    root->setContentsMargins(4, 2, 4, 2);
    root->setSpacing(2);
    root->setSizeConstraint(QLayout::SetMinAndMaxSize);
    const auto button = [this](const QString& label, const QString& name, QLayout* layout) {
        auto* b = new QPushButton(label, this);
        b->setObjectName(name);
        b->setAccessibleName(label);
        b->setAutoDefault(false);
        b->setFixedHeight(22);
        layout->addWidget(b);
        return b;
    };
    const auto label = [this](const QString& name, QLayout* layout) {
        auto* l = new QLabel(this);
        l->setObjectName(name);
        l->setWordWrap(true);
        l->setTextFormat(Qt::PlainText);
        l->setAccessibleName(name);
        layout->addWidget(l);
        return l;
    };
    const auto statusPill = [label](const QString& name, QLayout* layout) {
        QLabel* cell = label(name, layout);
        cell->setProperty("katPill", true);
        cell->setWordWrap(false);
        cell->setAlignment(Qt::AlignCenter);
        cell->setFixedHeight(18);
        return cell;
    };
    auto* header = new QHBoxLayout;
    header->setSpacing(4);
    m_power = button(tr("—"), "katPower", header);
    m_power->setCheckable(true);
    m_power->setFixedWidth(44);
    m_frequency = statusPill("katFrequency", header);
    header->setStretch(1, 1);
    m_mini = button(tr("Mini"), "katMini", header);
    m_mini->setCheckable(true);
    m_mini->setFixedWidth(38);
    root->addLayout(header);
    auto* tuning = new QHBoxLayout;
    tuning->setSpacing(4);
    m_tune = button(tr("Tune"), "katTune", tuning);
    m_tune->setProperty("aetherTxKeying", true);
    m_tune->setFixedWidth(60);
    m_fault = statusPill("katFault", tuning);
    tuning->setStretch(1, 1);
    auto* clear = button(tr("Clear fault"), "katClearFault", tuning);
    clear->setFixedWidth(72);
    root->addLayout(tuning);
    m_miniState = statusPill("katMiniState", root);
    m_modesPane = new QWidget(this);
    auto* modes = new QHBoxLayout(m_modesPane);
    modes->setContentsMargins(0, 0, 0, 0);
    modes->setSpacing(2);
    const QStringList names{tr("Auto"), tr("Manual"), tr("Bypass")};
    for (int i = 0; i < 3; ++i) {
        m_modes[i] = button(names[i], "kat" + names[i], modes);
        m_modes[i]->setCheckable(true);
        connect(m_modes[i], &QPushButton::clicked, this, [this, i] {
            if (m_model) {
                m_model->setMode(static_cast<Kat500::Mode>(i));
            }
            refresh();
        });
    }
    root->addWidget(m_modesPane);
    m_antennasPane = new QWidget(this);
    auto* ants = new QHBoxLayout(m_antennasPane);
    ants->setContentsMargins(0, 0, 0, 0);
    ants->setSpacing(2);
    for (int i = 0; i < 3; ++i) {
        m_antennas[i] = button(tr("ANT %1").arg(i + 1), QStringLiteral("katAntenna%1").arg(i + 1), ants);
        m_antennas[i]->setCheckable(true);
        connect(m_antennas[i], &QPushButton::clicked, this, [this, i] {
            if (m_model) {
                m_model->setAntenna(i + 1);
            }
            refresh();
        });
    }
    root->addWidget(m_antennasPane);
    auto* swrRow = new QHBoxLayout;
    m_swr = button(tr("SWR —"), "katSwr", swrRow);
    m_swr->setProperty("katPill", true);
    m_swr->setFixedHeight(18);
    m_swr->installEventFilter(this);
    connect(m_swr, &QPushButton::pressed, this, &Kat500Applet::refresh);
    connect(m_swr, &QPushButton::released, this, &Kat500Applet::refresh);
    m_swr->setFixedWidth(62);
    swrRow->setSpacing(4);
    QVector<HGauge::Tick> swrTicks;
    for (int step = 10; step <= 30; ++step) {
        const float value = step / 10.0f;
        swrTicks.append({value, step % 5 == 0 ? QString::number(value) : QString()});
    }
    m_swrGauge = new HGauge(1, 3, 2.5f, "", "", swrTicks, this, 2.0f);
    m_swrGauge->setFixedHeight(24);
    m_swrGauge->setObjectName("katSwrGauge");
    swrRow->addWidget(m_swrGauge, 1);
    root->addLayout(swrRow);
    m_physicalPane = new QWidget(this);
    m_physicalPane->setObjectName("katPhysicalState");
    auto* states = new QHBoxLayout(m_physicalPane);
    states->setContentsMargins(0, 0, 0, 0);
    states->setSpacing(3);
    const QStringList physicalNames{"katBypassedStatus", "katAttenuatorStatus", "katAmpInterruptStatus"};
    for (int i = 0; i < 3; ++i) {
        m_physical[i] = button(QString(), physicalNames[i], states);
        m_physical[i]->setProperty("katPill", true);
        m_physical[i]->setProperty("katSignalToggle", true);
        m_physical[i]->setCheckable(true);
        m_physical[i]->setFixedHeight(18);
        m_physical[i]->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        states->setStretch(i, 1);
        connect(m_physical[i], &QPushButton::clicked, this, [this, i] {
            if (m_model) {
                const auto& status = m_model->status();
                if (i == 0 && status.bypassed) {
                    m_model->setBypassed(!*status.bypassed);
                } else if (i == 1 && status.attenuator) {
                    m_model->setAttenuator(!*status.attenuator);
                } else if (i == 2 && status.ampKeyInterrupted) {
                    m_model->setAmpKeyInterrupted(!*status.ampKeyInterrupted);
                }
            }
            refresh();
        });
    }
    m_readiness = statusPill("katReadiness", states);
    root->addWidget(m_physicalPane);
    auto* tools = new QHBoxLayout;
    tools->setSpacing(3);
    m_manualToggle = button(tr("▸ Manual"), "katManualToggle", tools);
    m_manualToggle->setAccessibleName(tr("Manual controls"));
    m_manualToggle->setCheckable(true);
    m_manualPane = new QWidget(this);
    m_manualPane->setObjectName("katManualPane");
    auto* manual = new QVBoxLayout(m_manualPane);
    manual->setContentsMargins(0, 0, 0, 0);
    auto* explanation = label("katManualHelp", manual);
    explanation->setText(
        tr("Relay and frequency requests change the signal path. Controls show confirmed readback."));
    for (int kind = 0; kind < 2; ++kind) {
        auto* row = new QHBoxLayout;
        row->addWidget(new QLabel(kind == 0 ? tr("C (pF)") : tr("L (nH)"), this));
        auto* numeric = new QSpinBox(this);
        numeric->setObjectName(kind == 0 ? "katCapacitance" : "katInductance");
        numeric->setAccessibleName(kind == 0 ? tr("Capacitance in picofarads")
                                             : tr("Inductance in nanohenries"));
        numeric->setRange(0, Kat500::relayTotal(kind == 0, 255));
        row->addWidget(numeric, 1);
        if (kind == 0) {
            m_cap = numeric;
        } else {
            m_ind = numeric;
        }
        auto* request = button(tr("Set"), kind == 0 ? "katSetCapacitance" : "katSetInductance", row);
        manual->addLayout(row);
        connect(request, &QPushButton::clicked, this, [this, numeric, kind] {
            if (m_model) {
                const int mask = Kat500::relayMaskForValue(kind == 0, numeric->value());
                if (kind == 0) {
                    m_model->setCapacitorMask(mask);
                } else {
                    m_model->setInductorMask(mask);
                }
            }
            refresh();
        });
        auto* bits = new QGridLayout;
        bits->setHorizontalSpacing(2);
        for (int position = 0; position < 8; ++position) {
            const int bit = 7 - position;
            const int value = Kat500::relayTotal(kind == 0, 1 << bit);
            auto* check = new QCheckBox(QString::number(value), this);
            check->setProperty("katRelay", true);
            check->setObjectName(QStringLiteral("kat%1Bit%2").arg(kind == 0 ? "C" : "L").arg(bit));
            check->setAccessibleName(
                (kind == 0 ? tr("Capacitor %1 picofarads, relay bit %2")
                           : tr("Inductor %1 nanohenries, relay bit %2")).arg(value).arg(bit));
            bits->addWidget(check, position / 4, position % 4);
            (kind == 0 ? m_capBits : m_indBits)[bit] = check;
            connect(check, &QCheckBox::clicked, this, [this, kind, bit](bool on) {
                if (m_model) {
                    const auto value =
                        kind == 0 ? m_model->status().capacitorMask : m_model->status().inductorMask;
                    if (value) {
                        const int mask = on ? (*value | (1 << bit)) : (*value & ~(1 << bit));
                        if (kind == 0) {
                            m_model->setCapacitorMask(mask);
                        } else {
                            m_model->setInductorMask(mask);
                        }
                    }
                }
                refresh();
            });
        }
        manual->addLayout(bits);
    }
    auto* frequencyRow = new QHBoxLayout;
    m_frequencyRecall = new QSpinBox(this);
    m_frequencyRecall->setObjectName("katRecallFrequency");
    m_frequencyRecall->setAccessibleName(tr("Memory frequency in kHz"));
    m_frequencyRecall->setRange(0, 99999);
    m_frequencyRecall->setSpecialValueText(tr("Select frequency"));
    m_frequencyRecall->setToolTip(tr("Starts at the confirmed tuner frequency. Enter 1800–54000 kHz to choose a different memory; edits are preserved."));
    m_frequencyRecall->setAccessibleDescription(m_frequencyRecall->toolTip());
    connect(m_frequencyRecall, &QSpinBox::valueChanged, this, [this] {
        m_frequencyEdited = true;
        refresh();
    });
    m_frequencyRecall->setSuffix(tr(" kHz"));
    auto* memoryRow = new QHBoxLayout;
    auto* memoryLabel = new QLabel(tr("Match frequency"), this);
    memoryLabel->setObjectName("katRecallFrequencyLabel");
    memoryLabel->setBuddy(m_frequencyRecall);
    memoryRow->addWidget(memoryLabel);
    memoryRow->addWidget(m_frequencyRecall, 1);
    manual->addLayout(memoryRow);
    auto* recall = button(tr("Recall"), "katRecall", frequencyRow);
    auto* memorize = button(tr("Memorize"), "katMemorize", frequencyRow);
    manual->addLayout(frequencyRow);
    auto* ftns = button(tr("Full tune without saving"), "katFullTuneNoSave", manual);
    ftns->setProperty("aetherTxKeying", true);
    connect(recall, &QPushButton::clicked, this, [this] {
        if (m_model) {
            m_model->recallFrequency(m_frequencyRecall->value());
        }
    });
    connect(memorize, &QPushButton::clicked, this, [this] {
        if (m_model) {
            m_model->memorize(m_frequencyRecall->value());
        }
    });
    connect(ftns, &QPushButton::clicked, this, [this] {
        if (m_model) {
            m_model->startTune(false);
        }
    });
    m_historyToggle = button(tr("▸ History"), "katHistory", tools);
    m_settings = button(tr("▸ Settings"), "katSettings", tools);
    root->addLayout(tools);
    root->addWidget(m_manualPane);
    m_historyToggle->setCheckable(true);
    m_settings->setCheckable(true);
    m_historyPane = new QWidget(this);
    auto* history = new QVBoxLayout(m_historyPane);
    history->setContentsMargins(0, 0, 0, 0);
    m_graph = new HistoryGraphLabel(this);
    m_graph->setObjectName("katHistoryGraph");
    m_graph->setAccessibleName(tr("SWR and forward/reflected ADC history"));
    history->addWidget(m_graph);
    m_graph->setMinimumHeight(270);
    m_graph->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    m_graph->installEventFilter(this);
    root->addWidget(m_historyPane, 1);
    m_settingsPane = new QWidget(this);
    auto* settings = new QVBoxLayout(m_settingsPane);
    settings->setContentsMargins(0, 0, 0, 0);
    m_follow = new QCheckBox(tr("Follow TX frequency"), this);
    m_follow->setObjectName("katFollowTx");
    m_follow->setAccessibleDescription(
        tr("Follow the effective TX slice frequency including split and XIT. This sends frequency requests "
           "only; it does not key a radio. Default off."));
    m_follow->setChecked(prefs.value("FollowTx").toBool(false));
    settings->addWidget(m_follow);
    auto* preTune = button(tr("Pre-tune bands…"), "katPreTuneBands", settings);
    preTune->setAccessibleName(tr("KAT500 pre-tune bands"));
    preTune->setToolTip(tr("Open the KAT500 band sweep. Starting a sweep transmits at the radio's Tune Pwr; opening it does not transmit or change the radio tuner."));
    preTune->setAccessibleDescription(preTune->toolTip());
    connect(preTune, &QPushButton::clicked, this, &Kat500Applet::preTuneRequested);
    auto* signalPathTitle = label("katSignalPathTitle", settings);
    signalPathTitle->setText(tr("Signal path"));
    auto* signalPath = new QVBoxLayout;
    signalPath->setSpacing(2);
    settings->addLayout(signalPath);
    m_side = new QCheckBox(tr("Capacitor on antenna side"), this);
    m_side->setObjectName("katCapacitorSide");
    signalPath->addWidget(m_side);
    connect(m_side, &QCheckBox::clicked, this, [this](bool on) {
        if (m_model) {
            m_model->setCapacitorSide(on ? Kat500::CapacitorSide::Antenna
                                         : Kat500::CapacitorSide::Transmitter);
        }
        refresh();
    });
    m_identity = label("katIdentity", settings);
    m_observed = label("katLastObserved", settings);
    m_availability = label("katAvailability", settings);
    m_result = label("katOperationResult", settings);
    auto* help = label("katHistoryHelp", settings);
    help->setText(tr("History: SWR and forward/reflected ADC counts, last observed, 5 minutes. Gaps mean "
                     "missing replies; ADC counts are not watts. SWR may describe an earlier RF measurement. "
                     "Press and hold the SWR pill with the mouse to compare bypass SWR; release to return. Readings remain "
                     "after RF stops. Debug TX means serial writes, not RF."));
    auto* advanced = button(tr("Open Advanced settings"), "katAdvancedSettings", settings);
    connect(advanced, &QPushButton::clicked, this, &Kat500Applet::advancedSettingsRequested);
    m_debugToggle = button(tr("▸ Debug"), "katDebug", settings);
    m_debugToggle->setCheckable(true);
    m_debugPane = new QWidget(this);
    auto* debug = new QVBoxLayout(m_debugPane);
    debug->setContentsMargins(0, 0, 0, 0);
    m_verbose = new QCheckBox(tr("Verbose"), this);
    m_verbose->setObjectName("katVerbose");
    m_verbose->setChecked(prefs.value("Verbose").toBool(false));
    debug->addWidget(m_verbose);
    auto* debugActions = new QHBoxLayout;
    m_pause = new QCheckBox(tr("Pause display"), this);
    debug->addWidget(m_pause);
    auto* copy = button(tr("Copy"), "katDebugCopy", debugActions);
    auto* save = button(tr("Save…"), "katDebugSave", debugActions);
    auto* clearDebug = button(tr("Clear"), "katDebugClear", debugActions);
    debug->addLayout(debugActions);
    m_debug = new Kat500DebugOutput(this);
    m_debug->setObjectName("katDebugOutput");
    m_debug->setAccessibleName(tr("Tuner protocol diagnostics"));
    m_debug->setReadOnly(true);
    m_debug->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_debug->setMaximumBlockCount(1000);
    m_debug->setFixedHeight(160);
    debug->addWidget(m_debug);
    settings->addWidget(m_debugPane);
    root->addWidget(m_settingsPane);
    root->addStretch();
    connect(m_power, &QPushButton::clicked, this, [this] {
        if (m_model && m_model->status().powerOn) {
            m_model->setPower(!*m_model->status().powerOn);
        }
        refresh();
    });
    connect(m_tune, &QPushButton::clicked, this, [this] {
        if (m_model) {
            if (m_model->status().tuning.value_or(false)) {
                m_model->cancelTune();
            } else {
                m_model->startTune();
            }
        }
        refresh();
    });
    connect(clear, &QPushButton::clicked, this, [this] {
        if (m_model) {
            m_model->clearFault();
        }
    });
    for (auto* b : {m_mini, m_manualToggle, m_settings, m_historyToggle, m_debugToggle}) {
        connect(b, &QPushButton::toggled, this, [this] {
            savePreferences();
            presentation();
        });
    }
    connect(m_follow, &QCheckBox::toggled, this, [this](bool on) {
        if (m_model) {
            m_model->setFollowTxFrequency(on);
        }
        savePreferences();
        emit followTxChanged(on);
    });
    connect(m_verbose, &QCheckBox::toggled, this, [this](bool on) {
        if (m_model) {
            m_model->setVerbose(on);
        }
        savePreferences();
    });
    connect(m_pause, &QCheckBox::toggled, this, [this](bool paused) {
        if (!paused && m_model) {
            m_debug->showLatest(m_model->debugText());
        }
    });
    connect(copy, &QPushButton::clicked, this,
            [this] { QApplication::clipboard()->setText(m_debug->toPlainText()); });
    connect(save, &QPushButton::clicked, this, &Kat500Applet::saveDebug);
    connect(clearDebug, &QPushButton::clicked, this, [this] {
        if (m_model) {
            m_model->clearDebug();
        }
    });
    m_mini->setChecked(prefs.value("MiniMode").toBool(false));
    m_historyToggle->setChecked(prefs.value("HistoryVisible").toBool(false));
    m_timer.setInterval(500);
    connect(&m_timer, &QTimer::timeout, this, &Kat500Applet::refresh);
    m_timer.start();
    applyCanonSheet(bodyWidget(), [this] {
        m_swrGauge->setTickMarkColor(ThemeManager::instance().color(this, "color.canon.muted"));
        return checkRules() + QStringLiteral(
            "QWidget#katContent, QWidget#katViewport, QScrollArea#katScroll { background: {{color.canon.ground}}; border: none; }"
            "QLabel, QCheckBox { color: {{color.canon.muted}}; font-size: 10px; }"
            "QCheckBox[katRelay=true] { spacing: 2px; }"
            "QLabel[katPill=true] { font-size: 9px; font-weight: bold; background: {{color.canon.nested}}; "
            "border: 1px solid {{color.canon.lineHi}}; border-radius: 3px; padding: 0 2px; }"
            "QLabel[katTone=ok] { color: {{color.accent.success}}; }"
            "QLabel[katTone=warn] { color: {{color.accent.warning}}; }"
            "QLabel[katTone=error] { color: {{color.accent.danger}}; }"
            "QPushButton { padding: 0 4px; font-size: 10px; font-weight: bold; "
            "color: {{color.canon.inkSoft}}; background: {{color.canon.control}}; "
            "border-radius: 3px; border: 1px solid {{color.canon.lineHi}}; } "
            "QPushButton:hover { background: {{color.canon.nested}}; color: {{color.canon.aqua}}; } "
            "QSpinBox { font-size: 10px; min-height: 18px; color: {{color.canon.inkSoft}}; "
            "background: {{color.canon.nested}}; border: 1px solid {{color.canon.inkSoft}}; border-radius: 3px; } "
            "QSpinBox:focus { border-color: {{color.canon.cyan}}; } "
            "QSpinBox:disabled { color: {{color.canon.muted}}; border-color: {{color.canon.muted}}; } "
            "QPushButton[katPill=true] { font-size: 9px; padding: 0 2px; color: {{color.canon.muted}}; "
            "background: {{color.canon.ground}}; border-color: {{color.canon.line}}; } "
            "QPushButton[katPill=true]:focus { border-color: {{color.canon.cyan}}; } "
            "QPushButton[katTone=warn] { color: {{color.accent.warning}}; } "
            "QPushButton[katTone=live] { color: {{color.canon.inkSoft}}; } "
            "QPlainTextEdit { font-size: 10px; font-family: monospace; color: {{color.canon.inkSoft}}; } "
            "QPushButton:checked:enabled { background: "
            "{{color.accessory.key.meffa.active.background}}; color: "
            "{{color.accessory.key.meffa.active.foreground}}; } QPushButton#katBypass:checked:enabled { "
            "background: {{color.accessory.key.standby.background}}; color: "
            "{{color.accessory.key.standby.foreground}}; } QPushButton:disabled { color: "
            "{{color.text.secondary}}; background: {{color.background.2}}; } "
            "QPushButton:checked:disabled { background: {{color.accessory.key.meffa.active.background}}; "
            "color: {{color.text.secondary}}; border-color: {{color.accessory.key.meffa.active.border}}; } "
            "QPushButton#katBypass:checked:disabled { background: {{color.accessory.key.standby.background}}; "
            "color: {{color.text.secondary}}; border-color: {{color.accessory.key.standby.foreground}}; }"
            "QPushButton[katSignalToggle=true] { background: {{color.canon.nested}}; border-color: {{color.canon.lineHi}}; }"
            "QPushButton[katSignalToggle=true]:checked { background: {{color.canon.nested}}; "
            "color: {{color.accent.warning}}; border-color: {{color.canon.lineHi}}; }"
            "QPushButton[katSignalToggle=true]:hover:enabled { border-color: {{color.canon.cyan}}; }"
            "QPushButton[katSignalToggle=true]:disabled { color: {{color.canon.muted}}; "
            "background: {{color.canon.nested}}; border-color: {{color.canon.line}}; }");
    });
    presentation();
    refresh();
}
Kat500Applet::~Kat500Applet()
{
    if (m_fileDialog) {
        QFileDialog* dialog = m_fileDialog.data();
        m_fileDialog.clear();
        disconnect(dialog, nullptr, this, nullptr);
        dialog->reject();
        // Let the native sheet finish closing before QObject destruction.
        dialog->setParent(nullptr);
        dialog->deleteLater();
    }
}
void Kat500Applet::setModel(Kat500Model* model)
{
    if (m_model) {
        disconnect(m_model, nullptr, this, nullptr);
    }
    m_model = model;
    if (!m_pause->isChecked()) {
        m_debug->showLatest(model ? model->debugText() : QString());
    }
    m_frequencyEdited = false;
    { const QSignalBlocker blocker(m_frequencyRecall); m_frequencyRecall->setValue(0); }
    if (model) {
        model->setFollowTxFrequency(m_follow->isChecked());
        model->setVerbose(m_verbose->isChecked());
        connect(model, &Kat500Model::statusUpdated, this, &Kat500Applet::refresh);
        connect(model, &Kat500Model::operationFinished, this,
                [this](const QString& message) { text(m_result, message); });
        connect(model, &Kat500Model::operationFailed, this,
                [this](const QString& message) { text(m_result, message); });
        connect(model, &Kat500Model::connectionFailed, this,
                [this](const QString& message) { text(m_result, message); });
        connect(model, &Kat500Model::availabilityChanged, this, &Kat500Applet::refresh);
        connect(model, &Kat500Model::configurationRead, this, &Kat500Applet::refresh);
        connect(model, &Kat500Model::diagnostic, this, [this](const QString& line) {
            if (!m_pause->isChecked()) {
                m_debug->appendDiagnostic(line);
            }
        });
        connect(model, &Kat500Model::debugChanged, this, [this] {
            if (m_model && !m_pause->isChecked() && m_model->debugText().isEmpty()) {
                m_debug->showLatest(QString());
            }
        });
        connect(model, &Kat500Model::telemetryReceived, this,
                [this](const QString& command, const Kat500::Status& state) {
                    const int i = command == "VSWR" ? 0 : command == "VFWD" ? 1 : command == "VRFL" ? 2 : -1;
                    if (i < 0) {
                        return;
                    }
                    std::optional<double> value;
                    if (i == 0) {
                        value = state.swr;
                    } else {
                        const auto count = i == 1 ? state.forwardAdc : state.reflectedAdc;
                        if (count) {
                            value = *count;
                        }
                    }
                    m_history[i].push_back({m_clock.elapsed(), value});
                    while (m_history[i].size() > 4096) {
                        m_history[i].pop_front();
                    }
                });
        connect(model, &Kat500Model::disconnected, this, [this] {
            for (auto& series : m_history) {
                series.push_back({m_clock.elapsed(), std::nullopt});
            }
            refresh();
        });
    }
    refresh();
}
void Kat500Applet::refresh()
{
    const Kat500::Status s = m_model ? m_model->status() : Kat500::Status{};
    const QString reason = m_model ? m_model->unavailableReason(true) : tr("Connect a KAT500 first.");
    const bool ready = reason.isEmpty();
    const bool tuning = s.tuning.value_or(false);
    text(m_frequency, tr("%1 · %2 kHz")
                          .arg(s.band ? Kat500::bandName(*s.band) : tr("unknown band"),
                               s.frequencyKhz ? QString::number(*s.frequencyKhz) : QStringLiteral("—")));
    m_power->setText(s.powerOn ? (*s.powerOn ? tr("ON") : tr("OFF")) : QStringLiteral("—"));
    m_power->setChecked(s.powerOn.value_or(false));
    m_power->setAccessibleName(tr("Tuner power, confirmed %1").arg(m_power->text()));
    availability(m_power, m_model && m_model->isDetected() && m_model->isResponding() &&
                                  !m_model->operationBusy() && s.powerOn
                              ? QString()
                              : tr("Wait for confirmed tuner identity, power and an idle operation."),
                 tr("Turn the KAT500 on or off. The button shows confirmed tuner power, not radio power."));
    m_tune->setText(tuning ? tr("Cancel") : tr("Tune"));
    m_tune->setAccessibleName(m_tune->text());
    availability(m_tune, tuning && m_model && m_model->isResponding() ? QString() : reason,
                 tuning ? tr("Cancel the KAT500's current tuning search. This does not stop a radio carrier.")
                        : tr("Start a full KAT500 tuning search and save the match. Supply RF separately; "
                             "this button does not start a radio carrier."));
    const QStringList modeHelp{
        tr("Auto mode: recall stored matches as frequency or antenna changes, and search for a new "
           "match when SWR exceeds the auto-tune threshold."),
        tr("Manual mode: recall stored matches as frequency or antenna changes. SWR changes do not "
           "start a new search; use Tune when needed."),
        tr("Bypass mode: bypass the matching network and disable tuning. Unlike the BYP pill, "
           "this changes the operating mode.")};
    for (int i = 0; i < 3; ++i) {
        m_modes[i]->setChecked(s.mode && static_cast<int>(*s.mode) == i);
        availability(m_modes[i], reason, modeHelp[i]);
        m_antennas[i]->setChecked(s.antenna == i + 1);
        QString antennaReason = reason;
        if (antennaReason.isEmpty()) {
            if (!s.band ||
                !m_model->configuration().contains({Kat500::Setting::EnabledAntenna, *s.band, i + 1})) {
                antennaReason =
                    tr("Reading antenna availability for this band from the tuner.");
            } else if (m_model->configuration().value({Kat500::Setting::EnabledAntenna, *s.band, i + 1}) ==
                       0) {
                antennaReason = tr("This antenna is disabled on the confirmed band.");
            }
        }
        availability(m_antennas[i], antennaReason,
                     tr("Select KAT500 antenna connector %1. The antenna must be enabled for the "
                        "current band; the selection changes after tuner readback.").arg(i + 1));
    }
    availability(findChild<QPushButton*>("katClearFault"), reason,
                 tr("Clear the current KAT500 fault indication. Stored fault history is retained."));
    pill(m_fault, s.faultCode ? (*s.faultCode == 0 ? tr("NO FAULT") : tr("FAULT %1").arg(*s.faultCode))
                             : tr("FAULT ?"),
         s.faultCode ? Kat500::currentFaultName(*s.faultCode) : tr("Fault unknown"),
         s.faultCode ? (*s.faultCode == 0 ? "ok" : "error") : "muted");
    const bool preview = m_swr->isDown();
    const auto swr = preview ? s.bypassSwr : s.swr;
    const auto fresh = [this](const char* command) {
        const qint64 age = m_model ? m_model->fieldAgeMs(command) : -1;
        return age >= 0 && age <= 5000;
    };
    const bool rf = m_model && m_model->isResponding() && s.powerOn == true &&
                    s.forwardAdc.value_or(0) > 0 && fresh("VFWD") && fresh("VSWR");
    const QString swrText = tr("%1 %2").arg(preview ? tr("BYP") : tr("SWR"), reading(swr));
    const QString swrHelp =
        tr("%1. Press and hold this pill with the mouse to view the last bypass SWR; release to return to adjusted SWR. "
           "This only changes the display, not the tuner. Readings can remain after RF stops. "
           "The meter spans 1–3, amber from 2 and red from 2.5; values above 3 remain exact in the pill.")
            .arg(preview ? tr("Last bypass SWR: %1").arg(reading(swr))
                         : rf ? tr("Adjusted SWR: %1").arg(reading(swr))
                              : tr("Last observed adjusted SWR: %1 (no fresh RF indication)").arg(reading(swr)));
    pill(m_swr, swrText, swrHelp, preview ? "warn" : rf ? "live" : "muted");
    if (preview != m_showingBypass) {
        m_swrGauge->setValueImmediate(swr.value_or(1));
    } else {
        m_swrGauge->setValue(swr.value_or(1));
    }
    m_showingBypass = preview;
    m_swrGauge->setAccessibleName(swrText);
    m_swrGauge->setAccessibleDescription(swrHelp);
    const std::array<std::optional<bool>, 3> physical{s.bypassed, s.attenuator, s.ampKeyInterrupted};
    const QStringList physicalText{
        tr("BYP %1").arg(s.bypassed ? flag(s.bypassed) : tr("?")),
        tr("ATT %1").arg(s.attenuator ? flag(s.attenuator) : tr("?")),
        s.ampKeyInterrupted ? (*s.ampKeyInterrupted ? tr("KEY BLOCK") : tr("KEY PASS")) : tr("KEY ?")};
    const QStringList physicalHelp{
        tr("Bypass network (relays only): %1. Auto/Manual mode stays unchanged; recall or tuning may "
           "engage the network again. A good antenna match can also leave BYP ON in Auto or Manual. "
           "Use the main Bypass button to disable tuning. %2")
            .arg(flag(s.bypassed), s.bypassed ? (*s.bypassed ? tr("Click to engage the matching network.")
                                                          : tr("Click to bypass the matching network."))
                                             : QString()),
        tr("Attenuator: %1. %2")
            .arg(flag(s.attenuator), s.attenuator ? (*s.attenuator ? tr("Click to turn the attenuator off.")
                                                               : tr("Click to turn the attenuator on."))
                                                : QString()),
        tr("Amplifier key-line relay: %1. This affects the amplifier only when its key line passes "
           "through the tuner; it does not indicate amplifier transmission. %2")
            .arg(s.ampKeyInterrupted ? (*s.ampKeyInterrupted ? tr("blocked") : tr("connected"))
                                    : tr("unknown"),
                 s.ampKeyInterrupted ? (*s.ampKeyInterrupted ? tr("Click to allow amplifier keying.")
                                                           : tr("Click to block amplifier keying."))
                                     : QString())};
    for (int i = 0; i < 3; ++i) {
        const QSignalBlocker blocker(m_physical[i]);
        m_physical[i]->setChecked(physical[i].value_or(false));
        const QString blocked = !reason.isEmpty() ? reason
            : !physical[i] ? tr("Wait for confirmed state.") : QString();
        availability(m_physical[i], blocked);
        const QString detail = blocked.isEmpty() ? physicalHelp[i]
                                                : physicalHelp[i] + QStringLiteral("\n") + blocked;
        pill(m_physical[i], physicalText[i], detail, physical[i].value_or(false) ? "warn" : "muted");
    }
    const bool responding = m_model && m_model->isResponding();
    const QString readiness = !m_model || !m_model->isDetected() ? tr("OFFLINE")
                              : !responding ? tr("STALE")
                              : s.powerOn == false ? tr("OFF")
                              : tuning ? tr("TUNING")
                              : ready ? tr("READY") : tr("WAIT");
    pill(m_readiness, readiness, ready ? tr("Tuner is ready for an operator request.") : reason,
         ready ? "ok" : "warn");
    const QStringList modes{tr("AUTO"), tr("MANUAL"), tr("BYPASS")};
    const QString mode = s.mode ? modes.value(static_cast<int>(*s.mode)) : tr("MODE ?");
    const QString antenna = s.antenna ? tr("ANT %1").arg(*s.antenna) : tr("ANT ?");
    const bool bypassed = s.mode == Kat500::Mode::Bypass || s.bypassed == true;
    pill(m_miniState, tr("%1 · %2 · %3").arg(mode, antenna, readiness),
         tr("Confirmed mode: %1. Antenna: %2. Physical bypass: %3. %4")
             .arg(mode, antenna, flag(s.bypassed), m_readiness->accessibleDescription()),
         ready && !bypassed ? "ok" : "warn");
    text(m_availability, tr("%1\nLast observed SWR: %2 ms since reply.")
                             .arg(ready ? tr("Tuner ready.") : reason)
                             .arg(m_model ? m_model->fieldAgeMs("VSWR") : -1));
    text(m_identity, m_model
                         ? tr("%1\n%2 · Firmware %3 · S/N %4\nReply %5 · forward/reflected ADC %6 / %7")
                               .arg(m_model->description(), s.identity, s.firmware, s.serial,
                                    m_model->isResponding() ? tr("fresh") : tr("stale"),
                                    s.forwardAdc ? QString::number(*s.forwardAdc) : QStringLiteral("—"),
                                    s.reflectedAdc ? QString::number(*s.reflectedAdc) : QStringLiteral("—"))
                         : tr("No tuner connected."));
    const auto frequency = [](std::optional<int> khz) {
        return khz ? QString::number(*khz) + QStringLiteral(" kHz") : QStringLiteral("—");
    };
    text(m_observed, tr("Last observed\nATU selection (F): %1\nXCVR / CAT (FX): %2\n"
                        "RF counter (FC): %3\nATU bin (FY): %4\nAdjusted / bypass SWR: %5 / %6")
                         .arg(frequency(s.frequencyKhz), frequency(s.catFrequencyKhz),
                              frequency(s.countedFrequencyKhz),
                              s.frequencyBin.isEmpty() ? QStringLiteral("—")
                                                       : s.frequencyBin + QStringLiteral(" kHz"),
                              reading(s.swr), reading(s.bypassSwr)));
    m_observed->setToolTip(tr("F selects the stored match. FX is the last frequency received by the "
                             "tuner through CAT or AUXBUS. FC is the last measured RF frequency, "
                             "in 8 kHz steps. FY is the selected memory bin's frequency range. "
                             "Zero is a raw device value, not a usable frequency. These values may "
                             "remain after RF stops."));
    m_observed->setAccessibleDescription(m_observed->toolTip());
    for (auto* control : m_manualPane->findChildren<QWidget*>()) {
        if (qobject_cast<QPushButton*>(control) || qobject_cast<QSpinBox*>(control) ||
            qobject_cast<QCheckBox*>(control)) {
            availability(control, reason);
        }
    }
    if (!m_frequencyEdited && m_model && m_model->isResponding()) {
        const QSignalBlocker blocker(m_frequencyRecall);
        m_frequencyRecall->setValue(Kat500::isSupportedFrequency(s.frequencyKhz.value_or(0))
                                       ? *s.frequencyKhz : 0);
    }
    const QString frequencyReason = !reason.isEmpty() ? reason
        : Kat500::isSupportedFrequency(m_frequencyRecall->value()) ? QString()
        : tr("Select a frequency from 1800 to 54000 kHz before recalling or memorizing a match.");
    availability(findChild<QPushButton*>("katRecall"), frequencyReason);
    availability(findChild<QPushButton*>("katMemorize"), frequencyReason);
    for (int kind = 0; kind < 2; ++kind) {
        const auto mask = kind == 0 ? s.capacitorMask : s.inductorMask;
        auto* numeric = kind == 0 ? m_cap : m_ind;
        const QSignalBlocker blocked(numeric);
        if (mask && mask != m_lastRelayMasks[kind]) {
            numeric->setValue(Kat500::relayTotal(kind == 0, *mask));
        }
        m_lastRelayMasks[kind] = mask;
        for (int bit = 0; bit < 8; ++bit) {
            auto* check = (kind == 0 ? m_capBits : m_indBits)[bit];
            const QSignalBlocker b(check);
            check->setChecked(mask && (*mask & (1 << bit)));
            if (!mask) {
                availability(check, tr("Wait for a confirmed relay mask."));
            }
        }
    }
    {
        const QSignalBlocker b(m_side);
        availability(m_side, reason);
        m_side->setChecked(s.side == Kat500::CapacitorSide::Antenna);
        if (!s.side) {
            availability(m_side, tr("Wait for confirmed capacitor side."));
        }
    }
    if (reason.isEmpty() && s.firmware != QStringLiteral("02.16")) {
        availability(findChild<QPushButton*>("katMemorize"),
                     tr("Stored match verification supports firmware 02.16 only."));
    }
    if (m_historyPane->isVisible()) {
        renderHistory();
    }
}
void Kat500Applet::presentation()
{
    const QStringList titles{tr("Manual"), tr("History"), tr("Settings"), tr("Debug")};
    const QStringList drawerHelp{
        tr("Show or hide manual relay and stored-match controls. This does not change the tuner mode."),
        tr("Show or hide recent SWR and forward/reflected ADC history."),
        tr("Show or hide tuner connection details, signal-path settings and diagnostics."),
        tr("Show or hide tuner protocol diagnostics.")};
    const std::array<QPushButton*, 4> drawers{m_manualToggle, m_historyToggle, m_settings, m_debugToggle};
    for (int i = 0; i < 4; ++i) {
        drawers[i]->setText(QStringLiteral("%1 %2").arg(drawers[i]->isChecked() ? "▾" : "▸", titles[i]));
        drawers[i]->setToolTip(drawerHelp[i]);
        drawers[i]->setAccessibleDescription(drawerHelp[i]);
    }
    const bool mini = m_mini->isChecked();
    m_mini->setText(mini ? tr("Full") : tr("Mini"));
    m_mini->setAccessibleName(mini ? tr("Show full tuner controls") : tr("Show mini tuner controls"));
    m_mini->setToolTip(mini ? tr("Show all tuner controls and expandable drawers.")
                          : tr("Show compact power, Tune/Cancel and status controls."));
    m_mini->setAccessibleDescription(m_mini->toolTip());
    m_modesPane->setVisible(!mini);
    m_antennasPane->setVisible(!mini);
    m_physicalPane->setVisible(!mini);
    findChild<QPushButton*>("katClearFault")->setVisible(!mini);
    m_miniState->setVisible(mini);
    m_manualToggle->setVisible(!mini);
    m_manualPane->setVisible(!mini && m_manualToggle->isChecked());
    m_settings->setVisible(!mini);
    m_historyToggle->setVisible(!mini);
    m_settingsPane->setVisible(!mini && m_settings->isChecked());
    m_historyPane->setVisible(!mini && m_historyToggle->isChecked());
    m_debugPane->setVisible(m_debugToggle->isChecked());
    updateGeometry();
}
void Kat500Applet::savePreferences()
{
    QJsonObject p;
    p["MiniMode"] = m_mini->isChecked();
    p["HistoryVisible"] = m_historyToggle->isChecked();
    p["FollowTx"] = m_follow->isChecked();
    p["Verbose"] = m_verbose->isChecked();
    AppSettings::instance().setValue("Kat500Applet",
                                     QString::fromUtf8(QJsonDocument(p).toJson(QJsonDocument::Compact)));
    AppSettings::instance().save();
}
void Kat500Applet::setFloating(bool floating)
{
    m_floating = floating;
    updateGeometry();
}
QSize Kat500Applet::sizeHint() const
{
    return QSize(300, std::min(700, m_content->sizeHint().height() + 2));
}
QSize Kat500Applet::minimumSizeHint() const
{
    return QSize(238, m_mini && m_mini->isChecked() ? 98 : 170);
}
bool Kat500Applet::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_swr && (event->type() == QEvent::FocusOut || event->type() == QEvent::Hide ||
                             event->type() == QEvent::WindowDeactivate) && m_swr->isDown()) {
        m_swr->setDown(false);
        refresh();
    }
    if (watched == m_graph && event->type() == QEvent::Resize) {
        renderHistory();
    }
    return CanonApplet::eventFilter(watched, event);
}
void Kat500Applet::renderHistory()
{
    const QSize size = m_graph->size();
    if (size.isEmpty()) {
        return;
    }
    const qreal dpr = m_graph->devicePixelRatioF();
    QPixmap pixmap(size * dpr);
    pixmap.setDevicePixelRatio(dpr);
    auto& theme = ThemeManager::instance();
    pixmap.fill(theme.color(this, "color.canon.ground"));
    QPainter p(&pixmap);
    QFont font = m_graph->font();
    font.setPixelSize(9);
    p.setFont(font);
    const QStringList names{tr("SWR · last observed"), tr("Forward · ADC counts"), tr("Reflected · ADC counts")};
    const qint64 now = m_clock.elapsed();
    QStringList description{tr("Telemetry history, last 5 minutes. ADC counts are not watts. "
                               "SWR is last observed, not proof of RF. Gaps mean missing replies.")};
    for (int i = 0; i < 3; ++i) {
        const qreal top = i * size.height() / 3.0;
        const QRectF area(4, top + 20, size.width() - 38, size.height() / 3.0 - 37);
        const double minimum = i == 0 ? 1 : 0;
        double maximum = i == 0 ? 5 : 1;
        std::optional<double> latest;
        for (const Point& point : m_history[i]) {
            if (point.time >= now - 300000) {
                latest = point.value;
                if (i != 0 && point.value) {
                    maximum = std::max(maximum, *point.value);
                }
            }
        }
        const QColor trace = theme.color(this, i == 0 ? "color.canon.cyan" : "color.canon.aqua");
        const QString current = latest ? QString::number(*latest, 'f', i == 0 ? 2 : 0) : QStringLiteral("—");
        p.setPen(QPen(trace, 2));
        p.drawLine(QPointF(4, top + 10), QPointF(13, top + 10));
        p.drawText(QRectF(18, top, size.width() - 65, 18), Qt::AlignVCenter, names[i]);
        p.drawText(QRectF(size.width() - 46, top, 42, 18), Qt::AlignVCenter | Qt::AlignRight, current);
        p.setPen(theme.color(this, "color.canon.lineHi"));
        p.drawRect(area);
        p.drawLine(QPointF(area.center().x(), area.top()), QPointF(area.center().x(), area.bottom()));
        p.drawLine(QPointF(area.left(), area.center().y()), QPointF(area.right(), area.center().y()));
        p.setPen(theme.color(this, "color.canon.muted"));
        p.drawText(QRectF(area.right() + 3, area.top() - 5, 30, 12), Qt::AlignLeft, QString::number(maximum));
        p.drawText(QRectF(area.right() + 3, area.bottom() - 8, 30, 12), Qt::AlignLeft, QString::number(minimum));
        p.drawText(QRectF(area.left(), area.bottom() + 2, area.width(), 12), Qt::AlignLeft, tr("−5 min"));
        p.drawText(QRectF(area.left(), area.bottom() + 2, area.width(), 12), Qt::AlignRight, tr("now"));
        QPainterPath path;
        bool segment = false;
        qint64 prior = -10000;
        p.setPen(QPen(trace, 1.5));
        for (const Point& point : m_history[i]) {
            if (point.time < now - 300000) {
                continue;
            }
            if (!point.value) {
                segment = false;
                continue;
            }
            const QPointF pos(area.left() + area.width() * (point.time - (now - 300000)) / 300000.0,
                              area.bottom() - area.height() *
                                  std::clamp((*point.value - minimum) / (maximum - minimum), 0.0, 1.0));
            if (!segment || point.time - prior > 5000) {
                path.moveTo(pos);
                p.drawPoint(pos);
            } else {
                path.lineTo(pos);
            }
            segment = true;
            prior = point.time;
        }
        p.drawPath(path);
        description.append(tr("%1: latest %2; scale %3 to %4.")
                               .arg(names[i], current, QString::number(minimum), QString::number(maximum)));
    }
    m_graph->setAccessibleDescription(description.join(QLatin1Char(' ')));
    m_graph->setPixmap(pixmap);
}
void Kat500Applet::saveDebug()
{
    if (m_fileDialog) {
        m_fileDialog->raise();
        return;
    }
    auto* dialog = new QFileDialog(this);
    m_fileDialog = dialog;
    dialog->setObjectName("katDebugFileDialog");
    dialog->setWindowTitle(tr("Save KAT500 debug log"));
    dialog->setNameFilter(tr("Text files (*.txt)"));
    dialog->setAcceptMode(QFileDialog::AcceptSave);
    dialog->selectFile("kat500-debug.txt");
    connect(dialog, &QDialog::finished, this, [this, dialog](int result) {
        const QStringList paths = dialog->selectedFiles();
        m_fileDialog.clear();
        dialog->deleteLater();
        if (result != QDialog::Accepted || paths.isEmpty()) {
            return;
        }
        QSaveFile file(paths.first());
        const QByteArray data = m_debug->toPlainText().toUtf8();
        if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
            text(m_availability, tr("Debug save failed: %1").arg(file.errorString()));
        }
    });
    dialog->open();
}
} // namespace AetherSDR
