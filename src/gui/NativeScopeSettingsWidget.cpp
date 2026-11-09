#include "NativeScopeSettingsWidget.h"

#include "CanonIndicators.h"
#include "ControlAvailabilityRegistry.h"
#include "models/RadioModel.h"

#include <QAccessible>
#include <QAccessibleValueChangeEvent>
#include <QComboBox>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QList>
#include <QMetaType>
#include <QPainterPath>
#include <QPalette>
#include <QPair>
#include <functional>
#include <QPen>
#include <QPushButton>
#include <QSet>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QStringList>
#include <QVBoxLayout>

namespace AetherSDR {
namespace {
const QStringList kIds{"display", "display_type", "mode", "span", "fixed_range", "pause", "marker",
    "attenuation", "max_hold", "average", "waterfall_speed", "reference", "audio_attenuation", "audio_span",
    "oscilloscope_level", "oscilloscope_sweep", "shift", "audio_pause", "expand", "overflow", "bandscope_output", "filter_output"};
const QStringList kVerbs{"scope.controls.get", "scope.controls.set", "scope.controls.reset", "scope.controls.action",
    "scope.ranges.set", "scope.ranges.reset"};
QString scopeNamespace(const RadioCapabilities& caps)
{
    for (const QString& ns : caps.extensionNamespaces) {
        const QVariantMap extension = caps.extensions.value(ns).toMap();
        const QVariant version = extension.value("nativeScopeControlsVersion");
        const QVariant verbs = extension.value("nativeScopeControlsVerbs");
        if (version.typeId() != QMetaType::Int || version.toInt() != 1 || verbs.typeId() != QMetaType::QStringList) { continue; }
        bool complete = true;
        for (const QString& verb : kVerbs) { complete = complete && verbs.toStringList().contains(verb); }
        if (complete) { return ns; }
    }
    return {};
}
bool text(const QVariant& value, int maximum = 512, bool empty = true)
{
    return value.typeId() == QMetaType::QString && value.toString().size() <= maximum
        && (empty || !value.toString().isEmpty());
}
bool sequence(const QVariant& value, quint64& result)
{
    if (!text(value, 20, false)) { return false; }
    bool valid = false;
    result = value.toString().toULongLong(&valid);
    return valid && QString::number(result) == value.toString();
}
bool integer(const QVariant& value, int minimum, int maximum)
{
    return value.typeId() == QMetaType::Int && value.toInt() >= minimum && value.toInt() <= maximum;
}
bool frequency(const QVariant& value)
{
    const int type = value.typeId();
    if (type != QMetaType::Int && type != QMetaType::UInt && type != QMetaType::LongLong && type != QMetaType::ULongLong) { return false; }
    bool valid = false;
    const qint64 hz = value.toLongLong(&valid);
    return valid && hz >= 0 && hz <= 99999999;
}
bool pairValid(const QVariantMap& pair)
{
    if (pair.contains("lowHz") != pair.contains("highHz")) { return false; }
    return !pair.contains("lowHz") || (frequency(pair.value("lowHz")) && frequency(pair.value("highHz"))
        && pair.value("lowHz").toLongLong() < pair.value("highHz").toLongLong());
}
bool availability(const QVariantMap& state)
{
    return state.value("editable").typeId() == QMetaType::Bool && state.value("pending").typeId() == QMetaType::Bool
        && text(state.value("reason"), 2048) && !(state.value("editable").toBool() && state.value("pending").toBool());
}
QPair<int, int> domain(const QString& id)
{
    if (id == "display_type") { return {0, 4}; }
    if (id == "mode" || id == "filter_output") { return {0, 2}; }
    if (id == "span") { return {0, 7}; }
    if (id == "fixed_range" || id == "attenuation" || id == "average" || id == "audio_attenuation" || id == "oscilloscope_level") { return {0, 3}; }
    if (id == "waterfall_speed") { return {1, 4}; }
    if (id == "reference") { return {0, 60}; }
    if (id == "oscilloscope_sweep" || id == "bandscope_output") { return {0, 5}; }
    if (id == "shift") { return {0, 8}; }
    return {0, 1};
}
bool statusValid(const QVariantMap& status)
{
    if (!text(status.value("context"), 512, false) || !text(status.value("mode"), 32)
        || status.value("editable").typeId() != QMetaType::Bool || !text(status.value("reason"), 2048)
        || status.value("controls").typeId() != QMetaType::QVariantList || status.value("ranges").typeId() != QMetaType::QVariantList
        || status.value("currentBounds").typeId() != QMetaType::QVariantMap || status.value("actions").typeId() != QMetaType::QVariantMap
        || status.value("transport").typeId() != QMetaType::QVariantMap) { return false; }
    const QVariantList rows = status.value("controls").toList();
    if (rows.size() != kIds.size()) { return false; }
    QSet<QString> ids;
    for (const QVariant& raw : rows) {
        const QVariantMap row = raw.toMap(); const QString id = row.value("id").toString(); quint64 revision = 0;
        if (raw.typeId() != QMetaType::QVariantMap || !text(row.value("id"), 64, false) || !kIds.contains(id) || ids.contains(id)
            || !text(row.value("label"), 128, false) || !text(row.value("unit"), 128) || !availability(row)
            || !sequence(row.value("revision"), revision) || row.value("choices").typeId() != QMetaType::QVariantList) { return false; }
        ids.insert(id); const QPair<int, int> bounds = domain(id); const QVariantList choices = row.value("choices").toList();
        if (choices.size() != bounds.second - bounds.first + 1) { return false; }
        QSet<int> values;
        for (const QVariant& rawChoice : choices) {
            const QVariantMap choice = rawChoice.toMap();
            if (rawChoice.typeId() != QMetaType::QVariantMap || !integer(choice.value("value"), bounds.first, bounds.second)
                || values.contains(choice.value("value").toInt()) || !text(choice.value("label"), 128, false)
                || choice.value("editable").typeId() != QMetaType::Bool || !text(choice.value("reason"), 2048)) { return false; }
            const int value = choice.value("value").toInt(); values.insert(value);
            if (choice.value("editable").toBool() && ((id == "bandscope_output" && value >= 4)
                || (id == "filter_output" && value == 2))) { return false; }
        }
        if (row.contains("value") && !integer(row.value("value"), bounds.first, bounds.second)) { return false; }
        if (row.value("editable").toBool() && (!status.value("editable").toBool() || !row.contains("value") || id == "overflow")) { return false; }
    }
    const QVariantList ranges = status.value("ranges").toList(); if (ranges.size() != 3) { return false; }
    QSet<int> rangeSlots;
    for (const QVariant& raw : ranges) {
        const QVariantMap range = raw.toMap(); quint64 revision = 0;
        if (raw.typeId() != QMetaType::QVariantMap || !integer(range.value("slot"), 1, 3) || rangeSlots.contains(range.value("slot").toInt())
            || !sequence(range.value("revision"), revision) || !availability(range) || !pairValid(range)
            || range.value("resettable").typeId() != QMetaType::Bool
            || (range.value("editable").toBool() && !status.value("editable").toBool())
            || (range.value("resettable").toBool() && (!status.value("editable").toBool() || range.value("pending").toBool()))) { return false; }
        rangeSlots.insert(range.value("slot").toInt());
    }
    const QVariantMap current = status.value("currentBounds").toMap();
    if (!pairValid(current) || !text(current.value("reason"), 2048)) { return false; }
    const QVariantMap actions = status.value("actions").toMap();
    if (actions.size() != 3) { return false; }
    for (const QString& id : {QString("clear"), QString("marker_center"), QString("span_initial")}) {
        if (actions.value(id).typeId() != QMetaType::QVariantMap || !availability(actions.value(id).toMap())
            || (actions.value(id).toMap().value("editable").toBool() && !status.value("editable").toBool())) { return false; }
    }
    const QVariantMap transport = status.value("transport").toMap();
    return (!transport.contains("bandscopeOutput") || integer(transport.value("bandscopeOutput"), 0, 5))
        && (!transport.contains("filterOutput") || integer(transport.value("filterOutput"), 0, 2))
        && transport.value("rfProjectionAvailable").typeId() == QMetaType::Bool && text(transport.value("rfProjectionReason"), 2048)
        && transport.value("filterFrameAvailable").typeId() == QMetaType::Bool && text(transport.value("filterFrameReason"), 2048);
}
quint64 nextRequest()
{
    static quint64 request = 0x5258534300000000ULL;
    return ++request;
}
QString pairText(const QVariantMap& state)
{
    return state.contains("lowHz") ? QObject::tr("%1 to %2 Hz").arg(state.value("lowHz").toLongLong()).arg(state.value("highHz").toLongLong()) : QObject::tr("Unknown");
}
}

NativeScopeSettingsWidget::NativeScopeSettingsWidget(RadioModel& model, QWidget* parent)
    : QWidget(parent), m_model(model), m_availability(new ControlAvailabilityRegistry(model, this)),
      m_read(new QPushButton(tr("Read radio scope"), this)), m_context(new QLabel(this)), m_status(new QLabel(this)),
      m_currentBounds(new QLabel(this)), m_transport(new QLabel(this)), m_filterStatus(new QLabel(this)),
      m_plot(new QGraphicsView(this)), m_scene(new QGraphicsScene(this))
{
    setObjectName("nativeScopeSettings");
    m_transport->setObjectName("nativeScopeTransport");
    auto* layout = new QVBoxLayout(this);
    auto* explanation = new QLabel(tr("Radio scope controls use native radio units. Changes await radio readback. Stored fixed ranges require both endpoints; range and action requests have no verified acknowledgement. Serial output choices are shown but unavailable over this connection."), this);
    explanation->setWordWrap(true); layout->addWidget(explanation); layout->addWidget(m_context);
    auto* grid = new QGridLayout; layout->addLayout(grid);
    const QStringList labels{tr("Scope display"), tr("Display type"), tr("Scope mode"), tr("Span"), tr("Fixed range"), tr("Scope pause"), tr("Marker"), tr("RF attenuation"), tr("Maximum hold"), tr("Averaging"), tr("Waterfall speed"), tr("Reference level"), tr("Audio attenuation"), tr("Audio span"), tr("Oscilloscope level"), tr("Oscilloscope sweep"), tr("Shift position"), tr("Audio scope pause"), tr("Expanded display"), tr("Overflow indicator"), tr("Bandscope output"), tr("Filter spectrum output")};
    const auto gate = [this](QWidget* widget, std::function<bool()> available) {
        m_availability->registerWidget(widget, tr("Requires a current editable radio scope description and confirmed receive context."),
            [this, available](bool connected, const RadioCapabilities& caps) { return connected && !scopeNamespace(caps).isEmpty() && available(); }, {}, false);
    };
    for (int row = 0; row < kIds.size(); ++row) {
        const QString id = kIds[row]; Control control;
        auto* label = new QLabel(labels[row], this); grid->addWidget(label, row, 0);
        control.observed = new QLabel(tr("Unknown"), this); control.observed->setObjectName("nativeScope_" + id + "_Observed"); control.observed->setAccessibleName(labels[row] + tr(" confirmed value")); grid->addWidget(control.observed, row, 1);
        if (id == QLatin1String("reference")) {
            control.number = new QSpinBox(this); control.number->setRange(-1, 60); control.number->setSpecialValueText(tr("Unknown")); control.number->setValue(-1); control.number->setObjectName("nativeScope_" + id + "_Draft"); control.number->setAccessibleName(labels[row] + tr(" draft native index")); grid->addWidget(control.number, row, 2);
            connect(control.number, &QSpinBox::valueChanged, this, [this, id](int value) { editControl(id, value); }); connect(control.number, &QSpinBox::editingFinished, this, [this,id] { editControl(id,m_controls[id].number->value()); }); gate(control.number, [this,id] { return controlAvailable(id, false); });
        } else {
            control.choice = new QComboBox(this); control.choice->setObjectName("nativeScope_" + id + "_Choice"); control.choice->setAccessibleName(labels[row] + tr(" draft")); grid->addWidget(control.choice, row, 2);
            connect(control.choice, &QComboBox::activated, this, [this,id](int index) { editControl(id, m_controls[id].choice->itemData(index).toInt()); }); gate(control.choice, [this,id] { return controlAvailable(id, false); });
        }
        control.unit = new QLabel(this); grid->addWidget(control.unit, row, 3);
        control.apply = new QPushButton(tr("Apply"), this); control.apply->setAutoDefault(false); control.apply->setObjectName("nativeScope_" + id + "_Apply"); control.apply->setAccessibleName(tr("Apply %1").arg(labels[row])); grid->addWidget(control.apply, row, 4);
        connect(control.apply, &QPushButton::clicked, this, [this,id] { applyControl(id, false); }); gate(control.apply, [this,id] { return controlAvailable(id, true); });
        if (id == QLatin1String("span")) { control.reset = new QPushButton(tr("Reset span"), this); control.reset->setAutoDefault(false); control.reset->setObjectName("nativeScope_span_Reset"); grid->addWidget(control.reset, row, 5); connect(control.reset, &QPushButton::clicked, this, [this] { applyControl("span", true); }); gate(control.reset, [this] { return actionAvailable("span_initial"); }); }
        m_controls.insert(id, control);
    }
    for (int index = 0; index < 3; ++index) {
        Range& range = m_ranges[index]; const int slot = index + 1; const QString prefix = "nativeScopeRange" + QString::number(slot);
        auto* box = new QGroupBox(tr("Stored fixed range %1 (raw Hz)").arg(slot), this); auto* row = new QGridLayout(box);
        range.observed = new QLabel(tr("Unknown"), box); range.observed->setObjectName(prefix + "_Observed"); row->addWidget(range.observed, 0, 0, 1, 4);
        range.low = new QSpinBox(box); range.high = new QSpinBox(box);
        for (QSpinBox* spin : {range.low, range.high}) { spin->setRange(-1, 99999999); spin->setSpecialValueText(tr("Enter Hz")); spin->setValue(-1); connect(spin, &QSpinBox::valueChanged, this, [this,slot] { editRange(slot); }); connect(spin, &QSpinBox::editingFinished, this, [this,slot] { editRange(slot); }); gate(spin, [this,slot] { return m_requests.isEmpty() && rangeStatus(slot).value("editable").toBool(); }); }
        range.low->setObjectName(prefix + "_Low"); range.high->setObjectName(prefix + "_High"); range.low->setAccessibleName(tr("Range %1 lower endpoint Hz").arg(slot)); range.high->setAccessibleName(tr("Range %1 upper endpoint Hz").arg(slot)); row->addWidget(range.low, 1, 0); row->addWidget(range.high, 1, 1);
        range.apply = new QPushButton(tr("Apply both endpoints"), box); range.reset = new QPushButton(tr("Reset stored range"), box);
        range.apply->setObjectName(prefix + "_Apply"); range.reset->setObjectName(prefix + "_Reset");
        for (QPushButton* button : {range.apply, range.reset}) { button->setAutoDefault(false); }
        row->addWidget(range.apply, 1, 2); row->addWidget(range.reset, 1, 3); connect(range.apply, &QPushButton::clicked, this, [this,slot] { applyRange(slot,false); }); connect(range.reset, &QPushButton::clicked, this, [this,slot] { applyRange(slot,true); }); gate(range.apply, [this,slot] { return rangeAvailable(slot,false); }); gate(range.reset, [this,slot] { return rangeAvailable(slot,true); }); layout->addWidget(box);
    }
    auto* actions = new QGridLayout; layout->addLayout(actions);
    for (const QString& id : {QStringLiteral("clear"), QStringLiteral("marker_center")}) { auto* button = new QPushButton(id == QLatin1String("clear") ? tr("Clear scope") : tr("Center marker"), this); button->setAutoDefault(false); button->setObjectName("nativeScopeAction_" + id); m_actions.insert(id,button); actions->addWidget(button,0,m_actions.size()-1); gate(button,[this,id] { return actionAvailable(id); }); connect(button,&QPushButton::clicked,this,[this,id] { if (actionAvailable(id)) { submit("scope.controls.action", {{"context",m_confirmed.value("context")},{"id",id}}); } }); }
    m_read->setAutoDefault(false); m_read->setObjectName("nativeScopeRead"); gate(m_read,[this] { return m_requests.isEmpty(); }); connect(m_read,&QPushButton::clicked,this,[this] { submit("scope.controls.get"); }); layout->addWidget(m_read);
    for (QLabel* label : {m_context,m_status,m_currentBounds,m_transport,m_filterStatus}) { label->setWordWrap(true); label->setTextFormat(Qt::PlainText); }
    layout->addWidget(m_currentBounds); layout->addWidget(m_transport); layout->addWidget(m_status);
    m_plot->setObjectName("nativeScopeFilterPlot"); m_plot->setAccessibleName(tr("Last received native AF spectrum")); m_plot->setAccessibleDescription(tr("213 native sample positions, relative 0 to minus 50 dB. No frequency calibration or absolute signal level.")); m_plot->setScene(m_scene); m_plot->setMinimumHeight(120); m_filterStatus->setObjectName("nativeScopeFilterStatus"); layout->addWidget(m_filterStatus); layout->addWidget(m_plot);
    applyCanonSheet(m_plot, [] { return QStringLiteral("QGraphicsView { color: %1; background: %2; }").arg(detail::canonColour("cyan").name(), detail::canonColour("nested").name()); });
    connect(&model,&RadioModel::connectionStateChanged,this,&NativeScopeSettingsWidget::bindBackend); connect(&model,&RadioModel::capabilitiesChanged,this,&NativeScopeSettingsWidget::bindBackend); bindBackend();
}

NativeScopeSettingsWidget::~NativeScopeSettingsWidget()
{
    // QWidget teardown can finish an editor after derived members have gone.
    // Retire child callbacks while their captured draft state still exists.
    for (QObject* child : findChildren<QObject*>()) { QObject::disconnect(child, nullptr, this, nullptr); }
    QObject::disconnect(&m_model, nullptr, this, nullptr);
    QObject::disconnect(&m_model, nullptr, m_availability, nullptr);
    for (const QMetaObject::Connection& connection : m_connections) { QObject::disconnect(connection); }
}

QVariantMap NativeScopeSettingsWidget::controlStatus(const QString& id) const { for (const QVariant& item : m_confirmed.value("controls").toList()) { const QVariantMap row = item.toMap(); if (row.value("id").toString() == id) { return row; } } return {}; }
QVariantMap NativeScopeSettingsWidget::rangeStatus(int slot) const { for (const QVariant& item : m_confirmed.value("ranges").toList()) { const QVariantMap row = item.toMap(); if (row.value("slot").toInt() == slot) { return row; } } return {}; }
bool NativeScopeSettingsWidget::controlAvailable(const QString& id, bool apply) const
{
    const QVariantMap row = controlStatus(id); if (!m_requests.isEmpty() || !row.value("editable").toBool() || row.value("pending").toBool()) { return false; }
    if (!apply) { return true; } const Control& control = m_controls[id];
    if (control.draft < 0 || control.context != m_confirmed.value("context").toString() || control.revision != row.value("revision").toString()) { return false; }
    for (const QVariant& item : row.value("choices").toList()) { const QVariantMap choice = item.toMap(); if (choice.value("value").toInt() == control.draft) { return choice.value("editable").toBool(); } } return false;
}
bool NativeScopeSettingsWidget::rangeAvailable(int slot, bool reset) const
{
    const QVariantMap row = rangeStatus(slot); if (!m_requests.isEmpty() || !m_confirmed.value("editable").toBool() || row.value("pending").toBool()) { return false; }
    if (reset) { return row.value("resettable").toBool(); } const Range& range = m_ranges[slot-1];
    return row.value("editable").toBool() && range.context == m_confirmed.value("context").toString() && range.revision == row.value("revision").toString() && range.low->value() >= 0 && range.high->value() > range.low->value();
}
bool NativeScopeSettingsWidget::actionAvailable(const QString& id) const { return m_requests.isEmpty() && m_confirmed.value("actions").toMap().value(id).toMap().value("editable").toBool(); }
void NativeScopeSettingsWidget::editControl(const QString& id, int value) { if (m_confirmed.isEmpty() || !controlStatus(id).value("editable").toBool()) { return; } Control& control = m_controls[id]; control.draft=value; control.dirty=true; control.seeded=true; control.context=m_confirmed.value("context").toString(); control.revision=controlStatus(id).value("revision").toString(); refreshAvailability(); }
void NativeScopeSettingsWidget::editRange(int slot) { if (m_confirmed.isEmpty() || !rangeStatus(slot).value("editable").toBool()) { return; } Range& range=m_ranges[slot-1]; range.seeded=true; range.dirty=true; range.context=m_confirmed.value("context").toString(); range.revision=rangeStatus(slot).value("revision").toString(); refreshAvailability(); }
void NativeScopeSettingsWidget::applyControl(const QString& id, bool reset) { if (reset ? !actionAvailable("span_initial") : !controlAvailable(id,true)) { return; } QVariantMap args{{"context",m_confirmed.value("context")},{"id",id},{"revision",controlStatus(id).value("revision")}}; if (!reset) { args.insert("value",m_controls[id].draft); } submit(reset ? "scope.controls.reset" : "scope.controls.set",args); }
void NativeScopeSettingsWidget::applyRange(int slot, bool reset) { if (!rangeAvailable(slot,reset)) { return; } QVariantMap args{{"context",m_confirmed.value("context")},{"slot",slot},{"revision",rangeStatus(slot).value("revision")}}; if (!reset) { args.insert("lowHz",QVariant::fromValue<qint64>(m_ranges[slot-1].low->value())); args.insert("highHz",QVariant::fromValue<qint64>(m_ranges[slot-1].high->value())); } submit(reset ? "scope.ranges.reset" : "scope.ranges.set",args); }

void NativeScopeSettingsWidget::bindBackend()
{
    IRadioBackend* backend = m_model.isConnected() ? m_model.backend() : nullptr; const QString ns = backend ? scopeNamespace(m_model.backendCapabilities()) : QString{};
    if (backend == m_backend && ns == m_namespace && m_receiveSession == m_model.receiveControlSession()) { render(); return; }
    const quint64 generation=++m_bindingGeneration; m_receiveSession=m_model.receiveControlSession(); const RadioControlSession session=m_model.controlSession();
    for (const QMetaObject::Connection& connection : m_connections) { disconnect(connection); }
    m_backend=backend; m_namespace=ns; m_confirmed.clear(); m_requests.clear(); m_requestContexts.clear(); m_statusRevision=0; ++m_statusSerial; m_haveFrameSequence=false;
    for (Control& control : m_controls) { control.seeded=false; control.dirty=false; control.draft=-1; control.context.clear(); control.revision.clear(); }
    for (Range& range : m_ranges) { range.seeded=false; range.dirty=false; range.context.clear(); range.revision.clear(); }
    const QPointer<NativeScopeSettingsWidget> alive(this); parkFrame(tr("No confirmed spectrum in this receive context.")); if (!alive) { return; } render(); if (!alive || generation!=m_bindingGeneration || !backend || ns.isEmpty()) { return; }
    const QPointer<IRadioBackend> source(backend); const auto current=[this,source,generation,session] { return source && generation==m_bindingGeneration && source==m_backend && source==m_model.backend() && m_model.isConnected() && m_model.controlSessionCurrent(session); };
    m_connections[0]=connect(backend,&IRadioBackend::extensionStatus,this,[this,current,ns](const QString& space,const QString& kind,const QVariantMap& status) { if (!current() || space!=ns) { return; } if (kind==QLatin1String("scopeControls")) { acceptStatus(status); } else if (kind==QLatin1String("scopeFilterFrame")) { acceptFrame(status); } });
    m_connections[1]=connect(backend,&IRadioBackend::extensionResult,this,[this,current](quint64 request,const QVariant& result) {
        if (!current() || !m_requests.contains(request)) { return; } const QString verb=m_requests.take(request); const QString context=m_requestContexts.take(request);
        if (verb==QLatin1String("scope.controls.get")) { if (m_getSerial==m_statusSerial) { acceptStatus(result.toMap()); } else { render(); } return; }
        if (context!=m_confirmed.value("context").toString()) { render(); return; }
        const bool scalar=verb==QLatin1String("scope.controls.set") || verb==QLatin1String("scope.controls.reset"); const QVariantMap sent=result.toMap();
        const bool valid=scalar ? result.typeId()==QMetaType::Bool && result.toBool() : result.typeId()==QMetaType::QVariantMap && sent.value("sent").typeId()==QMetaType::Bool && sent.value("sent").toBool() && sent.value("verified").typeId()==QMetaType::Bool && !sent.value("verified").toBool();
        m_status->setText(valid ? tr("Request sent; radio confirmation is not implied.") : tr("Invalid request result; no confirmation inferred.")); render();
    });
    m_connections[2]=connect(backend,&IRadioBackend::extensionError,this,[this,current](quint64 request,const QString& error) { if (current() && m_requests.remove(request)) { m_requestContexts.remove(request); m_status->setText(error); render(); } });
    submit("scope.controls.get");
}
void NativeScopeSettingsWidget::submit(const QString& verb,const QVariantMap& arguments)
{
    if (!m_backend || m_backend!=m_model.backend() || !m_model.isConnected() || m_namespace.isEmpty() || !m_requests.isEmpty()) { return; }
    const quint64 request=nextRequest(); m_requests.insert(request,verb); m_requestContexts.insert(request,m_confirmed.value("context").toString()); if (verb==QLatin1String("scope.controls.get")) { m_getSerial=m_statusSerial; }
    const quint64 generation=m_bindingGeneration; const QPointer<NativeScopeSettingsWidget> alive(this); render(); if (!alive || generation!=m_bindingGeneration || !m_requests.contains(request)) { return; }
    m_model.invokeBackendExtension(m_namespace,verb,request,arguments);
}
void NativeScopeSettingsWidget::acceptStatus(const QVariantMap& status)
{
    quint64 revision=0; const bool ordered=sequence(status.value("statusRevision"),revision); if (ordered && revision<m_statusRevision) { return; } ++m_statusSerial;
    const QString oldContext=m_confirmed.value("context").toString();
    if (!ordered || !statusValid(status)) { m_confirmed.clear(); m_status->setText(tr("Invalid scope description. Read radio scope to retry.")); }
    else { m_statusRevision=revision; m_confirmed=status; }
    const QPointer<NativeScopeSettingsWidget> alive(this);
    if (oldContext!=m_confirmed.value("context").toString() || !m_confirmed.value("transport").toMap().value("filterFrameAvailable").toBool()) { parkFrame(tr("No confirmed spectrum in this receive context.")); }
    if (alive) { render(); }
}
void NativeScopeSettingsWidget::render()
{
    const QPointer<NativeScopeSettingsWidget> alive(this); const quint64 generation=m_bindingGeneration, serial=m_statusSerial;
    const auto current=[this,alive,generation,serial] { return alive && generation==m_bindingGeneration && serial==m_statusSerial; };
    m_context->setText(tr("Confirmed receive mode: %1").arg(m_confirmed.value("mode").toString().isEmpty() ? tr("Unknown") : m_confirmed.value("mode").toString()));
    for (const QString& id : kIds) {
        Control& control=m_controls[id]; const QVariantMap row=controlStatus(id); const bool known=row.contains("value");
        if (!control.seeded && known) { control.draft=row.value("value").toInt(); control.seeded=true; control.context=m_confirmed.value("context").toString(); control.revision=row.value("revision").toString(); }
        else if (known && control.context == m_confirmed.value("context").toString() && (!control.dirty || control.draft == row.value("value").toInt())) { control.revision=row.value("revision").toString(); }
        if (control.choice) {
            const QPointer<QComboBox> editor(control.choice); QSignalBlocker blocker(editor); const auto safe=[&] { if (!editor) { blocker.dismiss(); } return editor && current(); };
            editor->clear(); if (!safe()) { return; }
            for (const QVariant& entry : row.value("choices").toList()) {
                const QVariantMap choice=entry.toMap(); editor->addItem(choice.value("label").toString(),choice.value("value")); if (!safe()) { return; }
                editor->setItemData(editor->count()-1,choice.value("reason"),Qt::ToolTipRole); if (!safe()) { return; }
                auto* model=qobject_cast<QStandardItemModel*>(editor->model()); if (model && model->item(editor->count()-1)) { model->item(editor->count()-1)->setEnabled(choice.value("editable").toBool()); if (!safe()) { return; } }
            }
            editor->setCurrentIndex(editor->findData(control.draft)); if (!safe()) { return; }
        } else {
            const QPointer<QSpinBox> editor(control.number); QSignalBlocker blocker(editor); editor->setValue(control.draft); if (!editor) { blocker.dismiss(); return; } if (!current()) { return; }
        }
        QString observed=tr("Unknown"); if (known) { observed=QString::number(row.value("value").toInt()); for (const QVariant& entry : row.value("choices").toList()) { const QVariantMap choice=entry.toMap(); if (choice.value("value")==row.value("value")) { observed=choice.value("label").toString(); } } }
        control.observed->setText(observed); control.observed->setToolTip(row.value("reason").toString()); control.unit->setText(row.value("unit").toString());
        QAccessibleValueChangeEvent event(control.observed,observed); QAccessible::updateAccessibility(&event); if (!current()) { return; }
    }
    for (int index=0; index<3; ++index) {
        Range& range=m_ranges[index]; const QVariantMap row=rangeStatus(index+1); const bool seed=!range.seeded && row.contains("lowHz"); if (seed) { range.seeded=true; range.context=m_confirmed.value("context").toString(); range.revision=row.value("revision").toString(); }
        for (int edge=0; edge<2; ++edge) { QPointer<QSpinBox> editor(edge==0 ? range.low : range.high); QSignalBlocker blocker(editor); if (seed || !range.seeded) { editor->setValue(seed ? row.value(edge==0 ? "lowHz" : "highHz").toInt() : -1); } if (!editor) { blocker.dismiss(); return; } if (!current()) { return; } }
        range.observed->setText(pairText(row)); range.observed->setToolTip(row.value("reason").toString()); QAccessibleValueChangeEvent event(range.observed,range.observed->text()); QAccessible::updateAccessibility(&event); if (!current()) { return; }
    }
    m_currentBounds->setText(tr("Current scope bounds: %1").arg(pairText(m_confirmed.value("currentBounds").toMap())));
    const QVariantMap transport=m_confirmed.value("transport").toMap(); const auto transportState=[this,&transport](const QString& available,const QString& reason,bool af) {
        if (transport.value(available).typeId()!=QMetaType::Bool) { return tr("Unknown"); }
        if (transport.value(available).toBool()) { return af ? tr("Prerequisites available; received-frame status is shown below.") : tr("Available"); }
        const QString why=transport.value(reason).toString(); return why.isEmpty() ? tr("Unavailable") : tr("Unavailable: %1").arg(why);
    };
    m_transport->setText(tr("RF projection: %1\nAF spectrum: %2").arg(transportState("rfProjectionAvailable","rfProjectionReason",false),transportState("filterFrameAvailable","filterFrameReason",true)));
    refreshAvailability();
}
void NativeScopeSettingsWidget::refreshAvailability()
{
    const QPointer<NativeScopeSettingsWidget> alive(this); const quint64 generation=m_bindingGeneration, serial=m_statusSerial;
    const auto current=[this,alive,generation,serial] { return alive && generation==m_bindingGeneration && serial==m_statusSerial; };
    m_availability->refreshEngaged(); if (!current()) { return; }
    const auto explain=[&current](QWidget* widget,const QString& reason) {
        if (!widget || widget->isEnabled() || reason.isEmpty()) { return current(); }
        const QPointer<QWidget> target(widget); target->setToolTip(reason); if (!current() || !target) { return false; }
        target->setAccessibleDescription(reason); return current() && target;
    };
    for (const QString& id : kIds) {
        const Control& control=m_controls[id]; const QString reason=controlStatus(id).value("reason").toString();
        if (!explain(control.choice ? static_cast<QWidget*>(control.choice) : control.number,reason) || !explain(control.apply,reason)) { return; }
        if (control.reset && !explain(control.reset,m_confirmed.value("actions").toMap().value("span_initial").toMap().value("reason").toString())) { return; }
    }
    for (int slot=1; slot<=3; ++slot) { const Range& range=m_ranges[slot-1]; const QString reason=rangeStatus(slot).value("reason").toString(); for (QWidget* widget : {static_cast<QWidget*>(range.low),static_cast<QWidget*>(range.high),static_cast<QWidget*>(range.apply),static_cast<QWidget*>(range.reset)}) { if (!explain(widget,reason)) { return; } } }
    for (auto it=m_actions.cbegin(); it!=m_actions.cend(); ++it) { if (!explain(it.value(),m_confirmed.value("actions").toMap().value(it.key()).toMap().value("reason").toString())) { return; } }
}
void NativeScopeSettingsWidget::parkFrame(const QString& reason) { ++m_frameSerial; m_frameValues.clear(); m_frameReason=reason; renderFrame(); }
void NativeScopeSettingsWidget::acceptFrame(const QVariantMap& frame)
{
    quint64 sequenceValue=0; if (!sequence(frame.value("sequence"),sequenceValue) || !text(frame.value("context"),512,false) || frame.value("context")!=m_confirmed.value("context") || frame.value("known").typeId()!=QMetaType::Bool || frame.value("unit").typeId()!=QMetaType::QString || frame.value("unit").toString()!=QLatin1String("nativeInverseDisplayIndex") || !text(frame.value("reason"),2048) || frame.value("values").typeId()!=QMetaType::QVariantList) { return; }
    if (m_haveFrameSequence && sequenceValue<=m_frameSequence) { return; } const QVariantList values=frame.value("values").toList(); const bool known=frame.value("known").toBool();
    if (known ? values.size()!=213 : !values.isEmpty()) { return; } for (const QVariant& value : values) { if (!integer(value,0,50)) { return; } }
    m_frameSequence=sequenceValue; m_haveFrameSequence=true; m_frameContext=frame.value("context").toString(); ++m_frameSerial;
    m_frameValues=known && m_confirmed.value("transport").toMap().value("filterFrameAvailable").toBool() ? values : QVariantList{}; m_frameReason=frame.value("reason").toString(); renderFrame();
}
void NativeScopeSettingsWidget::renderFrame()
{
    const QPointer<NativeScopeSettingsWidget> alive(this); const quint64 generation=m_bindingGeneration, serial=m_frameSerial; const auto current=[this,alive,generation,serial] { return alive && generation==m_bindingGeneration && serial==m_frameSerial; };
    const QVariantList values=m_frameValues; m_scene->clear(); if (!current()) { return; } if (!values.isEmpty()) { QPainterPath path; path.moveTo(0,values[0].toInt()); for (int index=1; index<values.size(); ++index) { path.lineTo(index,values[index].toInt()); } m_scene->addPath(path,QPen(m_plot->palette().color(QPalette::Text))); if (!current()) { return; } }
    m_scene->setSceneRect(0,0,212,50); if (!current()) { return; } m_plot->fitInView(m_scene->sceneRect(),Qt::IgnoreAspectRatio); if (!current()) { return; } m_filterStatus->setText(tr("Last received native AF spectrum — 213 sample positions, relative 0 to −50 dB. %1").arg(m_frameValues.isEmpty() ? tr("Unknown. %1").arg(m_frameReason) : m_frameReason));
}
} // namespace AetherSDR
