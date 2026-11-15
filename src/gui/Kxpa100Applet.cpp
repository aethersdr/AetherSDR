#include "Kxpa100Applet.h"
#include "CanonIndicators.h"
#include "CanonWindow.h"
#include "HGauge.h"
#include "core/AppSettings.h"
#include "core/ThemeManager.h"
#include "models/Kxpa100Model.h"
#include <QAccessible>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
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
class Kxpa100DebugOutput : public QPlainTextEdit
{
  public:
    explicit Kxpa100DebugOutput(QWidget* parent) : QPlainTextEdit(parent)
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
void availability(QWidget* widget, const QString& reason)
{
    widget->setEnabled(reason.isEmpty());
    widget->setToolTip(reason);
    widget->setAccessibleDescription(reason);
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
    if (label->property("kxTone").toString() != state) {
        label->setProperty("kxTone", state);
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
                "kx-checks", checked ? "checked" : "unchecked", key, QSize(18, 18), 3,
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
    return QJsonDocument::fromJson(AppSettings::instance().value("Kxpa100Applet").toString().toUtf8())
        .object();
}
} // namespace
Kxpa100Applet::Kxpa100Applet(QWidget* parent) : CanonApplet(parent)
{
    setObjectName("Kxpa100Applet");
    setAccessibleName(tr("Elecraft KXPA100 amplifier and tuner"));
    setCloseButtonVisible(false);
    theme::setContainer(this, QStringLiteral("canon/kxpa100"));
    setMinimumWidth(238);
    m_clock.start();
    const QJsonObject prefs = preferences();
    auto* body = new QVBoxLayout(bodyWidget());
    body->setContentsMargins(0, 0, 0, 0);
    m_scroll = new QScrollArea(bodyWidget());
    m_scroll->setObjectName("kxScroll");
    m_scroll->setAccessibleName(tr("Tuner controls and telemetry"));
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_content = new QWidget;
    m_content->setObjectName("kxContent");
    m_scroll->viewport()->setObjectName("kxViewport");
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
        cell->setProperty("kxPill", true);
        cell->setWordWrap(false);
        cell->setAlignment(Qt::AlignCenter);
        cell->setFixedHeight(18);
        return cell;
    };
    auto* header = new QHBoxLayout;
    header->setSpacing(4);
    m_power = button(tr("—"), "kxPower", header);
    m_power->setCheckable(true);
    m_power->setFixedWidth(44);
    m_frequency = statusPill("kxFrequency", header);
    header->setStretch(1, 1);
    m_mini = button(tr("Mini"), "kxMini", header);
    m_mini->setCheckable(true);
    m_mini->setFixedWidth(38);
    root->addLayout(header);
    m_paPane = new QWidget(this);
    auto* pa = new QHBoxLayout(m_paPane);
    pa->setContentsMargins(0,0,0,0); pa->setSpacing(2);
    auto* standby = button(tr("Standby"), "kxStandby", pa);
    auto* operate = button(tr("Operate"), "kxOperate", pa);
    standby->setCheckable(true); operate->setCheckable(true);
    connect(standby,&QPushButton::clicked,this,[this] { if (m_model) { m_model->setOperate(false); } refresh(); });
    connect(operate,&QPushButton::clicked,this,[this] { if (m_model) { m_model->setOperate(true); } refresh(); });
    root->addWidget(m_paPane);
    auto* tuning = new QHBoxLayout;
    tuning->setSpacing(4);
    m_tune = button(tr("Tune"), "kxTune", tuning);
    m_tune->setProperty("aetherTxKeying", true);
    m_tune->setFixedWidth(60);
    m_fault = statusPill("kxFault", tuning);
    tuning->setStretch(1, 1);
    auto* clear = button(tr("Clear fault"), "kxClearFault", tuning);
    clear->setFixedWidth(72);
    root->addLayout(tuning);
    m_miniState = statusPill("kxMiniState", root);
    m_modesPane = new QWidget(this);
    auto* modes = new QHBoxLayout(m_modesPane);
    modes->setContentsMargins(0, 0, 0, 0);
    modes->setSpacing(2);
    const QStringList names{tr("Auto"), tr("Manual"), tr("Bypass")};
    for (int i = 0; i < 3; ++i) {
        m_modes[i] = button(names[i], "kx" + names[i], modes);
        m_modes[i]->setCheckable(true);
        connect(m_modes[i], &QPushButton::clicked, this, [this, i] {
            if (m_model) {
                m_model->setMode(static_cast<Kxpa100::Mode>(i));
            }
            refresh();
        });
    }
    root->addWidget(m_modesPane);
    m_antennasPane = new QWidget(this);
    auto* ants = new QHBoxLayout(m_antennasPane);
    ants->setContentsMargins(0, 0, 0, 0);
    ants->setSpacing(2);
    for (int i = 0; i < 2; ++i) {
        m_antennas[i] = button(tr("ANT %1").arg(i + 1), QStringLiteral("kxAntenna%1").arg(i + 1), ants);
        m_antennas[i]->setCheckable(true);
        connect(m_antennas[i], &QPushButton::clicked, this, [this, i] {
            if (m_model) {
                m_model->setAntenna(i + 1);
            }
            refresh();
        });
    }
    root->addWidget(m_antennasPane);
    const auto meter = [this, statusPill](int index, const QString& name, float maximum, QVBoxLayout* layout) {
        auto* row = new QHBoxLayout; row->setSpacing(4);
        m_meterLabels[index] = statusPill(QStringLiteral("kxMeterLabel%1").arg(index), row);
        m_meterLabels[index]->setFixedWidth(72);
        QVector<HGauge::Tick> ticks;
        for (int step = 0; step <= 20; ++step) {
            const float value = maximum * step / 20;
            ticks.append({value, step % 5 == 0 ? QString::number(value,'g',3) : QString()});
        }
        m_meters[index] = new HGauge(0,maximum,maximum*0.9f,"","",ticks,this,maximum*0.75f);
        m_meters[index]->setObjectName(name); m_meters[index]->setAccessibleName(name);
        m_meters[index]->setFixedHeight(24); row->addWidget(m_meters[index],1); layout->addLayout(row);
    };
    meter(0,"kxForwardGauge",100,root);
    auto* swrRow = new QHBoxLayout;
    m_swr = button(tr("SWR —"), "kxSwr", swrRow);
    m_swr->setProperty("kxPill", true);
    m_swr->setFixedHeight(18);
    m_swr->installEventFilter(this);
    connect(m_swr, &QPushButton::pressed, this, &Kxpa100Applet::refresh);
    connect(m_swr, &QPushButton::released, this, &Kxpa100Applet::refresh);
    m_swr->setFixedWidth(72);
    swrRow->setSpacing(4);
    QVector<HGauge::Tick> swrTicks;
    for (int step = 10; step <= 50; ++step) {
        const float value = step / 10.0f;
        swrTicks.append({value, step % 10 == 0 ? QString::number(value) : QString()});
    }
    m_swrGauge = new HGauge(1, 5, 3.0f, "", "", swrTicks, this, 2.0f);
    m_swrGauge->setFixedHeight(24);
    m_swrGauge->setObjectName("kxSwrGauge");
    swrRow->addWidget(m_swrGauge, 1);
    root->addLayout(swrRow);
    m_auxPane = new QWidget(this);
    auto* auxiliary = new QVBoxLayout(m_auxPane); auxiliary->setContentsMargins(0,0,0,0); auxiliary->setSpacing(2);
    meter(1,"kxInputGauge",10,auxiliary); meter(2,"kxReflectedGauge",20,auxiliary);
    meter(3,"kxDissipatedGauge",100,auxiliary); meter(4,"kxTemperatureGauge",100,auxiliary);
    meter(5,"kxSupplyGauge",16,auxiliary); meter(6,"kxCurrentGauge",25,auxiliary);
    root->addWidget(m_auxPane);
    m_physicalPane = new QWidget(this);
    m_physicalPane->setObjectName("kxPhysicalState");
    auto* states = new QHBoxLayout(m_physicalPane);
    states->setContentsMargins(0, 0, 0, 0);
    states->setSpacing(3);
    const QStringList physicalNames{"kxBypassedStatus", "kxAttenuatorStatus"};
    for (int i = 0; i < 2; ++i) {
        m_physical[i] = button(QString(), physicalNames[i], states);
        m_physical[i]->setProperty("kxPill", true);
        m_physical[i]->setProperty("kxSignalToggle", true);
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

                }
            }
            refresh();
        });
    }
    m_readiness = statusPill("kxReadiness", states);
    root->addWidget(m_physicalPane);
    auto* tools = new QHBoxLayout;
    tools->setSpacing(3);
    m_manualToggle = button(tr("▸ Manual"), "kxManualToggle", tools);
    m_manualToggle->setAccessibleName(tr("Manual controls"));
    m_manualToggle->setCheckable(true);
    m_manualPane = new QWidget(this);
    m_manualPane->setObjectName("kxManualPane");
    auto* manual = new QVBoxLayout(m_manualPane);
    manual->setContentsMargins(0, 0, 0, 0);
    auto* explanation = label("kxManualHelp", manual);
    explanation->setText(
        tr("Relay and frequency requests change the signal path. Controls show confirmed readback."));
    for (int kind = 0; kind < 2; ++kind) {
        auto* row = new QHBoxLayout;
        row->addWidget(new QLabel(kind == 0 ? tr("C (pF)") : tr("L (nH)"), this));
        auto* numeric = new QSpinBox(this);
        numeric->setObjectName(kind == 0 ? "kxCapacitance" : "kxInductance");
        numeric->setAccessibleName(kind == 0 ? tr("Capacitance in picofarads")
                                             : tr("Inductance in nanohenries"));
        numeric->setRange(0, Kxpa100::relayTotal(kind == 0, 255));
        row->addWidget(numeric, 1);
        if (kind == 0) {
            m_cap = numeric;
        } else {
            m_ind = numeric;
        }
        auto* request = button(tr("Set"), kind == 0 ? "kxSetCapacitance" : "kxSetInductance", row);
        manual->addLayout(row);
        connect(request, &QPushButton::clicked, this, [this, numeric, kind] {
            if (m_model) {
                const int mask = Kxpa100::relayMaskForValue(kind == 0, numeric->value());
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
            const int value = Kxpa100::relayTotal(kind == 0, 1 << bit);
            auto* check = new QCheckBox(QString::number(value), this);
            check->setProperty("kxRelay", true);
            check->setObjectName(QStringLiteral("kx%1Bit%2").arg(kind == 0 ? "C" : "L").arg(bit));
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
    m_frequencyRecall->setObjectName("kxRecallFrequency");
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
    memoryLabel->setObjectName("kxRecallFrequencyLabel");
    memoryLabel->setBuddy(m_frequencyRecall);
    memoryRow->addWidget(memoryLabel);
    memoryRow->addWidget(m_frequencyRecall, 1);
    manual->addLayout(memoryRow);
    auto* recall = button(tr("Recall"), "kxRecall", frequencyRow);
    auto* memorize = button(tr("Memorize"), "kxMemorize", frequencyRow);
    manual->addLayout(frequencyRow);
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
    m_historyToggle = button(tr("▸ History"), "kxHistory", tools);
    m_settings = button(tr("▸ Settings"), "kxSettings", tools);
    root->addLayout(tools);
    root->addWidget(m_manualPane);
    m_historyToggle->setCheckable(true);
    m_settings->setCheckable(true);
    m_historyPane = new QWidget(this);
    auto* history = new QVBoxLayout(m_historyPane);
    history->setContentsMargins(0, 0, 0, 0);
    m_historyWindow = new QComboBox(this);
    m_historyWindow->setAccessibleName(tr("History time window"));
    m_historyWindow->addItem(tr("Last minute"),60000); m_historyWindow->addItem(tr("Last 5 minutes"),300000);
    m_historyWindow->addItem(tr("Last 15 minutes"),900000); m_historyWindow->setCurrentIndex(1);
    history->addWidget(m_historyWindow);
    connect(m_historyWindow,&QComboBox::currentIndexChanged,this,&Kxpa100Applet::renderHistory);
    m_graph = new HistoryGraphLabel(this);
    m_graph->setObjectName("kxHistoryGraph");
    m_graph->setAccessibleName(tr("SWR and forward/reflected power history"));
    history->addWidget(m_graph);
    m_graph->setMinimumHeight(270);
    m_graph->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    m_graph->installEventFilter(this);
    root->addWidget(m_historyPane, 1);
    m_settingsPane = new QWidget(this);
    auto* settings = new QVBoxLayout(m_settingsPane);
    settings->setContentsMargins(0, 0, 0, 0);
    m_association = label("kxRadioAssociation",settings);
    m_association->setText(tr("No associated radio. Select a radio, then associate this amplifier."));
    m_associate = button(tr("Use with current radio"),"kxAssociateRadio",settings);
    availability(m_associate,tr("Select a radio first."));
    connect(m_associate,&QPushButton::clicked,this,&Kxpa100Applet::associateRadioRequested);
    auto* signalPathTitle = label("kxSignalPathTitle", manual);
    signalPathTitle->setText(tr("Signal path"));
    auto* signalPath = new QVBoxLayout;
    signalPath->setSpacing(2);
    manual->addLayout(signalPath);
    m_side = new QCheckBox(tr("Capacitor on antenna side"), this);
    m_side->setObjectName("kxCapacitorSide");
    signalPath->addWidget(m_side);
    connect(m_side, &QCheckBox::clicked, this, [this](bool on) {
        if (m_model) {
            m_model->setCapacitorSide(on ? Kxpa100::CapacitorSide::Antenna
                                         : Kxpa100::CapacitorSide::Transmitter);
        }
        refresh();
    });
    m_identity = label("kxIdentity", settings);
    m_observed = label("kxLastObserved", settings);
    m_availability = label("kxAvailability", settings);
    m_result = label("kxOperationResult", settings);
    auto* help = label("kxHistoryHelp", settings);
    help->setText(tr("History shows measured forward/reflected watts and last observed SWR. Gaps mean missing replies. "
        "Hold the SWR pill with the mouse to compare bypass SWR; release to return. "
        "Last SWR and RF-counter frequency can remain after RF stops. Debug TX means serial writes, not RF."));
    auto* advanced = button(tr("Open Advanced settings"), "kxAdvancedSettings", settings);
    connect(advanced, &QPushButton::clicked, this, &Kxpa100Applet::advancedSettingsRequested);
    m_debugToggle = button(tr("▸ Debug"), "kxDebug", settings);
    m_debugToggle->setCheckable(true);
    m_debugPane = new QWidget(this);
    auto* debug = new QVBoxLayout(m_debugPane);
    debug->setContentsMargins(0, 0, 0, 0);
    m_verbose = new QCheckBox(tr("Verbose"), this);
    m_verbose->setObjectName("kxVerbose");
    m_verbose->setChecked(prefs.value("Verbose").toBool(false));
    debug->addWidget(m_verbose);
    auto* debugActions = new QHBoxLayout;
    m_pause = new QCheckBox(tr("Pause display"), this);
    debug->addWidget(m_pause);
    auto* copy = button(tr("Copy"), "kxDebugCopy", debugActions);
    auto* save = button(tr("Save…"), "kxDebugSave", debugActions);
    auto* clearDebug = button(tr("Clear"), "kxDebugClear", debugActions);
    debug->addLayout(debugActions);
    m_debug = new Kxpa100DebugOutput(this);
    m_debug->setObjectName("kxDebugOutput");
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
        if (m_model && m_model->status().operate) {
            m_model->setOperate(!*m_model->status().operate);
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
    connect(save, &QPushButton::clicked, this, &Kxpa100Applet::saveDebug);
    connect(clearDebug, &QPushButton::clicked, this, [this] {
        if (m_model) {
            m_model->clearDebug();
        }
    });
    m_mini->setChecked(prefs.value("MiniMode").toBool(false));
    m_historyToggle->setChecked(prefs.value("HistoryVisible").toBool(false));
    m_timer.setInterval(500);
    connect(&m_timer, &QTimer::timeout, this, &Kxpa100Applet::refresh);
    m_timer.start();
    applyCanonSheet(bodyWidget(), [this] {
        for (auto* meter : m_meters) { meter->setTickMarkColor(ThemeManager::instance().color(this,"color.canon.muted")); }
        m_swrGauge->setTickMarkColor(ThemeManager::instance().color(this, "color.canon.muted"));
        return checkRules() + QStringLiteral(
            "QWidget#kxContent, QWidget#kxViewport, QScrollArea#kxScroll { background: {{color.canon.ground}}; border: none; }"
            "QLabel, QCheckBox { color: {{color.canon.muted}}; font-size: 10px; }"
            "QCheckBox[kxRelay=true] { spacing: 2px; }"
            "QLabel[kxPill=true] { font-size: 9px; font-weight: bold; background: {{color.canon.nested}}; "
            "border: 1px solid {{color.canon.lineHi}}; border-radius: 3px; padding: 0 2px; }"
            "QLabel[kxTone=ok] { color: {{color.accent.success}}; }"
            "QLabel[kxTone=warn] { color: {{color.accent.warning}}; }"
            "QLabel[kxTone=error] { color: {{color.accent.danger}}; }"
            "QPushButton { padding: 0 4px; font-size: 10px; font-weight: bold; "
            "color: {{color.canon.inkSoft}}; background: {{color.canon.control}}; "
            "border-radius: 3px; border: 1px solid {{color.canon.lineHi}}; } "
            "QPushButton:hover { background: {{color.canon.nested}}; color: {{color.canon.aqua}}; } "
            "QSpinBox { font-size: 10px; min-height: 18px; color: {{color.canon.inkSoft}}; "
            "background: {{color.canon.nested}}; border: 1px solid {{color.canon.inkSoft}}; border-radius: 3px; } "
            "QSpinBox:focus { border-color: {{color.canon.cyan}}; } "
            "QSpinBox:disabled { color: {{color.canon.muted}}; border-color: {{color.canon.muted}}; } "
            "QPushButton[kxPill=true] { font-size: 9px; padding: 0 2px; color: {{color.canon.muted}}; "
            "background: {{color.canon.ground}}; border-color: {{color.canon.line}}; } "
            "QPushButton[kxPill=true]:focus { border-color: {{color.canon.cyan}}; } "
            "QPushButton[kxTone=warn] { color: {{color.accent.warning}}; } "
            "QPushButton[kxTone=live] { color: {{color.canon.inkSoft}}; } "
            "QPlainTextEdit { font-size: 10px; font-family: monospace; color: {{color.canon.inkSoft}}; } "
            "QPushButton:checked:enabled { background: "
            "{{color.accessory.key.meffa.active.background}}; color: "
            "{{color.accessory.key.meffa.active.foreground}}; } QPushButton#kxBypass:checked:enabled { "
            "background: {{color.accessory.key.standby.background}}; color: "
            "{{color.accessory.key.standby.foreground}}; } QPushButton:disabled { color: "
            "{{color.text.secondary}}; background: {{color.background.2}}; } "
            "QPushButton:checked:disabled { background: {{color.accessory.key.meffa.active.background}}; "
            "color: {{color.text.secondary}}; border-color: {{color.accessory.key.meffa.active.border}}; } "
            "QPushButton#kxBypass:checked:disabled { background: {{color.accessory.key.standby.background}}; "
            "color: {{color.text.secondary}}; border-color: {{color.accessory.key.standby.foreground}}; }"
            "QPushButton[kxSignalToggle=true] { background: {{color.canon.nested}}; border-color: {{color.canon.lineHi}}; }"
            "QPushButton[kxSignalToggle=true]:checked { background: {{color.canon.nested}}; "
            "color: {{color.accent.warning}}; border-color: {{color.canon.lineHi}}; }"
            "QPushButton[kxSignalToggle=true]:hover:enabled { border-color: {{color.canon.cyan}}; }"
            "QPushButton[kxSignalToggle=true]:disabled { color: {{color.canon.muted}}; "
            "background: {{color.canon.nested}}; border-color: {{color.canon.line}}; }");
    });
    presentation();
    refresh();
}
Kxpa100Applet::~Kxpa100Applet()
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
void Kxpa100Applet::setModel(Kxpa100Model* model)
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
        model->setVerbose(m_verbose->isChecked());
        connect(model, &Kxpa100Model::statusUpdated, this, &Kxpa100Applet::refresh);
        connect(model, &Kxpa100Model::operationFinished, this,
                [this](const QString& message) { text(m_result, message); });
        connect(model, &Kxpa100Model::operationFailed, this,
                [this](const QString& message) { text(m_result, message); });
        connect(model, &Kxpa100Model::connectionFailed, this,
                [this](const QString& message) { text(m_result, message); });
        connect(model, &Kxpa100Model::availabilityChanged, this, &Kxpa100Applet::refresh);
        connect(model, &Kxpa100Model::configurationRead, this, &Kxpa100Applet::refresh);
        connect(model, &Kxpa100Model::diagnostic, this, [this](const QString& line) {
            if (!m_pause->isChecked()) {
                m_debug->appendDiagnostic(line);
            }
        });
        connect(model, &Kxpa100Model::debugChanged, this, [this] {
            if (m_model && !m_pause->isChecked() && m_model->debugText().isEmpty()) {
                m_debug->showLatest(QString());
            }
        });
        connect(model, &Kxpa100Model::telemetryReceived, this,
                [this](const QString& command, const Kxpa100::Status& state) {
                    const int i = command == "^SW" ? 0 : command == "^PF" ? 1 : command == "^PV" ? 2 : -1;
                    if (i < 0) {
                        return;
                    }
                    std::optional<double> value;
                    if (i == 0) {
                        value = state.swr;
                    } else {
                        const auto count = i == 1 ? state.forwardWatts : state.reflectedWatts;
                        if (count) {
                            value = *count;
                        }
                    }
                    m_history[i].push_back({m_clock.elapsed(), value});
                    while (m_history[i].size() > 4096) {
                        m_history[i].pop_front();
                    }
                });
        connect(model, &Kxpa100Model::disconnected, this, [this] {
            for (auto& series : m_history) {
                series.push_back({m_clock.elapsed(), std::nullopt});
            }
            refresh();
        });
    }
    refresh();
}
void Kxpa100Applet::setRadioContext(const QString& radioName, int txFrequencyKhz, bool associated)
{
    m_txFrequencyKhz = txFrequencyKhz; m_associated = associated;
    text(m_association,radioName.isEmpty() ? tr("No radio selected.") : associated
        ? tr("Associated with %1. VU/PWR metering and frequency context apply only to this radio.").arg(radioName)
        : tr("Selected radio: %1. Associate it for VU/PWR metering and frequency context.").arg(radioName));
    availability(m_associate,radioName.isEmpty() ? tr("Select a radio first.") : QString());
    m_associate->setText(associated ? tr("Remove radio association") : tr("Use with current radio"));
    refresh();
}
void Kxpa100Applet::setAssociationResult(bool saved)
{
    text(m_result, saved ? tr("Radio association updated.")
        : tr("Radio association was not saved. Connect an identified radio and KXPA100, and check that settings are writable. The previous association is unchanged."));
}
void Kxpa100Applet::refresh()
{
    const Kxpa100::Status s = m_model ? m_model->status() : Kxpa100::Status{};
    const QString reason = m_model ? m_model->unavailableReason(true) : tr("Connect a KXPA100 first.");
    const QString tunerReason = m_model ? m_model->unavailableReason(true,true) : reason;
    const bool responding = m_model && m_model->isResponding();
    const bool tuning = s.tuning == true;
    const auto fresh = [this](const char* key) { const auto age=m_model ? m_model->fieldAgeMs(key) : -1; return age>=0 && age<=5000; };
    const bool rf = responding && s.forwardWatts.value_or(0)>0 && fresh("^PF");
    const int frequency = m_associated && m_txFrequencyKhz>0 ? m_txFrequencyKhz : s.frequencyKhz.value_or(0);
    text(m_frequency,tr("%1 · %2").arg(s.band ? Kxpa100::bandName(*s.band) : tr("Band —"),
        frequency>0 ? tr("%1 kHz").arg(frequency) : tr("Freq —")));
    m_frequency->setToolTip(m_associated ? tr("Associated radio TX frequency; band is device readback.") : tr("Last RF-counter frequency; not an indication of current transmission."));
    m_frequency->setAccessibleDescription(m_frequency->toolTip());
    text(m_power,s.operate ? (*s.operate ? tr("OPR") : tr("STBY")) : tr("PA ?"));
    m_power->setChecked(s.operate==true); availability(m_power,reason);
    m_power->setAccessibleName(tr("PA operate/standby, confirmed %1").arg(m_power->text()));
    for (const bool operate : {false,true}) {
        auto* control=findChild<QPushButton*>(operate ? "kxOperate" : "kxStandby");
        control->setChecked(s.operate && *s.operate==operate); availability(control,reason);
    }
    text(m_tune,tuning ? tr("Cancel") : tr("Tune"));
    availability(m_tune,tuning && responding && !m_model->operationBusy() ? QString() : tunerReason);
    for (int i=0;i<3;++i) {
        m_modes[i]->setChecked(s.mode && static_cast<int>(*s.mode)==i); availability(m_modes[i],tunerReason);
    }
    for (int i=0;i<2;++i) {
        m_antennas[i]->setChecked(s.antenna==i+1);
        QString blocked=tunerReason;
        if (blocked.isEmpty() && m_model->configuration() && s.band && !((*m_model->configuration())[*s.band] & (1<<i))) {
            blocked=tr("This antenna is disabled on the current band.");
        }
        availability(m_antennas[i],blocked);
    }
    availability(findChild<QPushButton*>("kxClearFault"),reason);
    const QString fault=s.faultCode ? Kxpa100::currentFaultName(*s.faultCode) : tr("Fault unknown");
    pill(m_fault,s.faultCode ? (*s.faultCode=='N' ? tr("NO FAULT") : tr("FAULT %1").arg(QChar(*s.faultCode))) : tr("FAULT ?"),
        fault,s.faultCode && *s.faultCode=='N' ? "ok" : "error");
    const std::array<std::optional<double>,7> values{s.forwardWatts,s.inputWatts,s.reflectedWatts,s.dissipatedWatts,s.temperatureC,s.supplyVolts,s.drainAmps};
    const QStringList names{tr("OUT"),tr("DRV"),tr("REFL"),tr("DISS"),tr("TEMP"),tr("SUPPLY"),tr("DRAIN")};
    const QStringList units{tr("W"),tr("W"),tr("W"),tr("W"),tr("°C"),tr("V"),tr("A")};
    for (int i=0;i<7;++i) {
        const QString value=values[i] ? QString::number(*values[i],'f',1) : QStringLiteral("—");
        const QString label=tr("%1 %2%3").arg(names[i],value,units[i]);
        pill(m_meterLabels[i],label,tr("KXPA100 %1: %2 %3. %4").arg(names[i],value,units[i],responding ? QString() : tr("Readback is stale.")),"muted");
        m_meters[i]->setValue(values[i].value_or(0)); m_meters[i]->setAccessibleName(label);
        if (i == 1) {
            const QString source = tr("DRV: input power measured by the KXPA100, in watts. Scale 0–10 W.");
            const QString description = source + " " + m_meterLabels[i]->accessibleDescription();
            m_meterLabels[i]->setToolTip(description);
            m_meterLabels[i]->setAccessibleDescription(description);
            m_meters[i]->setToolTip(description);
            m_meters[i]->setAccessibleDescription(description);
        }
    }
    const bool preview=m_swr->isDown(); const auto swr=preview ? s.bypassSwr : s.swr;
    const QString swrText=tr("%1 %2").arg(preview ? tr("BYP") : tr("SWR"),reading(swr));
    const QString swrHelp=tr("Last observed %1 SWR. Hold with the mouse to preview bypass SWR; release to return. "
        "This changes the display only. SWR may remain after RF stops. Scale 1–5; the pill preserves values above 5.").arg(preview ? tr("bypass") : tr("adjusted"));
    pill(m_swr,swrText,swrHelp,preview ? "warn" : rf ? "live" : "muted");
    if (preview!=m_showingBypass) { m_swrGauge->setValueImmediate(swr.value_or(1)); }
    else { m_swrGauge->setValue(swr.value_or(1)); }
    m_showingBypass=preview; m_swrGauge->setAccessibleName(swrText); m_swrGauge->setAccessibleDescription(swrHelp);
    const std::array<std::optional<bool>,2> physical{s.bypassed,s.attenuator ? std::optional<bool>(*s.attenuator!=0) : std::nullopt};
    const QStringList descriptions{
        tr("ATU matching-network bypass relay. Auto/Manual mode stays unchanged; a recall or tune may engage it again. Click to toggle."),
        tr("Input attenuator. Firmware may engage it for protection. The rear-panel switch overrides software control. Click to toggle.")};
    for (int i=0;i<2;++i) {
        QString blocked=i==0 ? tunerReason : reason;
        if (blocked.isEmpty() && !physical[i]) { blocked=tr("Wait for confirmed state."); }
        if (i==1 && s.attenuator==2) { blocked=tr("The rear-panel switch holds the attenuator on."); }
        m_physical[i]->setChecked(physical[i]==true); availability(m_physical[i],blocked);
        pill(m_physical[i],tr("%1 %2").arg(i==0 ? tr("ATU BYP") : tr("ATT"),flag(physical[i])),
            descriptions[i]+QStringLiteral("\n")+blocked,physical[i]==true ? "warn" : "muted");
    }
    const QString readiness=!m_model || !m_model->isDetected() ? tr("OFFLINE") : !responding ? tr("STALE") : tuning ? tr("TUNING") : rf ? tr("RF") : tr("IDLE");
    pill(m_readiness,readiness,tr("RF is based on amplifier power readback, not radio PTT."),tuning || !responding ? "warn" : "ok");
    const QStringList modes{tr("AUTO"),tr("MANUAL"),tr("BYPASS")};
    const QString mode=s.tunerPresent==false ? tr("NO ATU") : s.mode ? modes[int(*s.mode)] : tr("ATU ?");
    pill(m_miniState,tr("%1 · ANT %2 · %3").arg(mode,s.antenna ? QString::number(*s.antenna) : QStringLiteral("?"),
        s.faultCode && *s.faultCode!='N' ? fault : readiness),tr("Tuner mode and antenna are readbacks. Physical ATU bypass: %1.").arg(flag(s.bypassed)),
        s.mode==Kxpa100::Mode::Bypass || s.bypassed==true || !responding ? "warn" : "ok");
    text(m_identity,m_model ? tr("%1\n%2 · Firmware %3 · S/N %4").arg(m_model->description(),s.identity,s.firmware,s.serial) : tr("No amplifier connected."));
    text(m_availability,tr("%1\nPower / SWR reply age: %2 / %3 ms").arg(reason.isEmpty() ? tr("Ready for an operator request.") : reason)
        .arg(m_model ? m_model->fieldAgeMs("^PF") : -1).arg(m_model ? m_model->fieldAgeMs("^SW") : -1));
    text(m_observed,tr("Last RF frequency: %1\nAdjusted / bypass SWR: %2 / %3\nOptional tuner: %4")
        .arg(s.frequencyKhz.value_or(0)>0 ? tr("%1 kHz").arg(*s.frequencyKhz) : tr("Unavailable"),reading(s.swr),reading(s.bypassSwr),flag(s.tunerPresent)));
    for (auto* control : m_manualPane->findChildren<QWidget*>()) {
        if (qobject_cast<QPushButton*>(control) || qobject_cast<QSpinBox*>(control) || qobject_cast<QCheckBox*>(control)) { availability(control,tunerReason); }
    }
    if (!m_frequencyEdited) {
        const QSignalBlocker blocker(m_frequencyRecall);
        m_frequencyRecall->setValue(Kxpa100::isSupportedFrequency(frequency) ? frequency : 0);
    }
    const QString frequencyReason=!tunerReason.isEmpty() ? tunerReason : Kxpa100::isSupportedFrequency(m_frequencyRecall->value()) ? QString() : tr("Select 1800–54000 kHz before recall or save.");
    availability(findChild<QPushButton*>("kxRecall"),frequencyReason); availability(findChild<QPushButton*>("kxMemorize"),frequencyReason);
    for (int kind=0;kind<2;++kind) {
        const auto mask=kind==0 ? s.capacitorMask : s.inductorMask; auto* numeric=kind==0 ? m_cap : m_ind;
        const QSignalBlocker blocker(numeric);
        if (mask && mask!=m_lastRelayMasks[kind]) { numeric->setValue(Kxpa100::relayTotal(kind==0,*mask)); }
        m_lastRelayMasks[kind]=mask;
        for (int bit=0;bit<8;++bit) {
            auto* check=(kind==0 ? m_capBits : m_indBits)[bit]; const QSignalBlocker blocked(check);
            check->setChecked(mask && (*mask & (1<<bit))); if (!mask) { availability(check,tr("Wait for confirmed relay state.")); }
        }
    }
    { const QSignalBlocker blocker(m_side); m_side->setChecked(s.side==Kxpa100::CapacitorSide::Antenna); }
    if (m_historyPane->isVisible()) { renderHistory(); }
}
void Kxpa100Applet::presentation()
{
    const QStringList titles{tr("Manual"), tr("History"), tr("Settings"), tr("Debug")};
    const std::array<QPushButton*, 4> drawers{m_manualToggle, m_historyToggle, m_settings, m_debugToggle};
    for (int i = 0; i < 4; ++i) {
        drawers[i]->setText(QStringLiteral("%1 %2").arg(drawers[i]->isChecked() ? "▾" : "▸", titles[i]));
    }
    const bool mini = m_mini->isChecked();
    m_mini->setText(mini ? tr("Full") : tr("Mini"));
    m_mini->setAccessibleName(mini ? tr("Show full tuner controls") : tr("Show mini tuner controls"));
    m_paPane->setVisible(!mini);
    m_power->setVisible(mini);
    m_auxPane->setVisible(!mini);
    m_modesPane->setVisible(!mini);
    m_antennasPane->setVisible(!mini);
    m_physicalPane->setVisible(!mini);
    findChild<QPushButton*>("kxClearFault")->setVisible(!mini);
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
void Kxpa100Applet::savePreferences()
{
    QJsonObject p;
    p["MiniMode"] = m_mini->isChecked();
    p["HistoryVisible"] = m_historyToggle->isChecked();
    p["Verbose"] = m_verbose->isChecked();
    AppSettings::instance().setValue("Kxpa100Applet",
                                     QString::fromUtf8(QJsonDocument(p).toJson(QJsonDocument::Compact)));
    AppSettings::instance().save();
}
void Kxpa100Applet::setFloating(bool floating)
{
    m_floating = floating;
    updateGeometry();
}
QSize Kxpa100Applet::sizeHint() const
{
    return QSize(300, std::min(700, m_content->sizeHint().height() + 2));
}
QSize Kxpa100Applet::minimumSizeHint() const
{
    return QSize(238, m_mini && m_mini->isChecked() ? 124 : 376);
}
bool Kxpa100Applet::eventFilter(QObject* watched, QEvent* event)
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
void Kxpa100Applet::renderHistory()
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
    const QStringList names{tr("SWR · last observed"), tr("Forward · W"), tr("Reflected · W")};
    const qint64 now = m_clock.elapsed();
    const int window = m_historyWindow->currentData().toInt();
    QStringList description{tr("Telemetry history in watts. "
                               "SWR is last observed, not proof of RF. Gaps mean missing replies.")};
    for (int i = 0; i < 3; ++i) {
        const qreal top = i * size.height() / 3.0;
        const QRectF area(4, top + 20, size.width() - 38, size.height() / 3.0 - 37);
        const double minimum = i == 0 ? 1 : 0;
        double maximum = i == 0 ? 5 : 1;
        std::optional<double> latest;
        for (const Point& point : m_history[i]) {
            if (point.time >= now - window) {
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
        p.drawText(QRectF(area.left(), area.bottom() + 2, area.width(), 12), Qt::AlignLeft, tr("−%1 min").arg(window/60000));
        p.drawText(QRectF(area.left(), area.bottom() + 2, area.width(), 12), Qt::AlignRight, tr("now"));
        QPainterPath path;
        bool segment = false;
        qint64 prior = -10000;
        p.setPen(QPen(trace, 1.5));
        for (const Point& point : m_history[i]) {
            if (point.time < now - window) {
                continue;
            }
            if (!point.value) {
                segment = false;
                continue;
            }
            const QPointF pos(area.left() + area.width() * (point.time - (now - window)) / double(window),
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
void Kxpa100Applet::saveDebug()
{
    if (m_fileDialog) {
        m_fileDialog->raise();
        return;
    }
    auto* dialog = new QFileDialog(this);
    m_fileDialog = dialog;
    dialog->setObjectName("kxDebugFileDialog");
    dialog->setWindowTitle(tr("Save KXPA100 debug log"));
    dialog->setNameFilter(tr("Text files (*.txt)"));
    dialog->setAcceptMode(QFileDialog::AcceptSave);
    dialog->selectFile("kxpa100-debug.txt");
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
