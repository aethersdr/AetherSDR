#include "NativeMemoryReadWidget.h"
#include "ControlAvailabilityRegistry.h"
#include "models/RadioModel.h"

#include <QAbstractItemView>
#include <QAccessible>
#include <QAccessibleValueChangeEvent>
#include <QGridLayout>
#include <QHeaderView>
#include <QLabel>
#include <QList>
#include <QMetaType>
#include <QPlainTextEdit>
#include <QPair>
#include <QPushButton>
#include <QSet>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSizePolicy>
#include <QStringList>
#include <QTableWidget>
#include <QVBoxLayout>

namespace AetherSDR {
namespace {
const QStringList kVerbs{"memory.read.get","memory.read.refresh","memory.read.channel","memory.read.quick","memory.read.cancel"};
bool text(const QVariant& value,int maximum,bool empty=true) { return value.typeId()==QMetaType::QString && value.toString().size()<=maximum && (empty || !value.toString().isEmpty()); }
bool sequence(const QVariant& value,quint64& result) { if (!text(value,20,false)) { return false; } bool ok=false; result=value.toString().toULongLong(&ok); return ok && QString::number(result)==value.toString(); }
bool integer(const QVariant& value,int low,int high) { return value.typeId()==QMetaType::Int && value.toInt()>=low && value.toInt()<=high; }
bool hz(const QVariant& value) { return value.typeId()==QMetaType::LongLong && value.toLongLong()>=0 && value.toLongLong()<=99999999999LL; }
bool hexText(const QVariant& value,int maximum) { if (!text(value,maximum) || value.toString().size()%2) { return false; } for (QChar c : value.toString()) { if (!QStringLiteral("0123456789abcdef").contains(c)) { return false; } } return true; }
QString memoryNamespace(const RadioCapabilities& caps) {
    for (const QString& ns : caps.extensionNamespaces) { const QVariantMap record=caps.extensions.value(ns).toMap(); const QVariant version=record.value("nativeMemoryReadVersion"),verbs=record.value("nativeMemoryReadVerbs"); if (version.typeId()!=QMetaType::Int || version.toInt()!=1 || verbs.typeId()!=QMetaType::QStringList) { continue; } bool complete=true; for (const QString& verb : kVerbs) { complete=complete && verbs.toStringList().contains(verb); } if (complete) { return ns; } } return {};
}
QString memoryUnavailableReason(const RadioCapabilities& caps) {
    for (const QString& ns : caps.extensionNamespaces) { const QVariant value=caps.extensions.value(ns).toMap().value("nativeMemoryReadUnavailableReason"); if (text(value,2048,false)) { return value.toString(); } }
    return {};
}
bool fieldsValid(const QVariantMap& fields,bool quick) {
    const QSet<QString> normal{"frequency1Hz","frequency2Hz","frequency2Role","mode1","mode2","mode1Label","mode2Label","fmWidth1","fmWidth2","toneType","toneIndex","ctcssIndex","split","lockout","name","nameUtf8Hex"};
    const QSet<QString> quickKeys{"leftFrequencyHz","rightFrequencyHz","leftMode","rightMode","leftModeLabel","rightModeLabel","split"};
    for (auto it=fields.cbegin();it!=fields.cend();++it) {
        if (!(quick ? quickKeys : normal).contains(it.key())) { return false; }
        if (it.key()=="frequency2Role") { if (it.value().typeId()!=QMetaType::QString || it.value().toString()!=QLatin1String("unqualified-program-field")) { return false; } }
        else if (it.key().endsWith("Hz")) { if (!hz(it.value())) { return false; } }
        else if (it.key()=="name") { if (!text(it.value(),20) || it.value().toString().toUcs4().size()>10) { return false; } }
        else if (it.key()=="nameUtf8Hex") { if (!hexText(it.value(),80)) { return false; } }
        else if (it.key().contains("Mode") || it.key().startsWith("mode")) { if (!text(it.value(),32,false)) { return false; } }
        else { const int high=it.key()=="toneType" ? 3 : it.key()=="toneIndex" ? 50 : it.key()=="ctcssIndex" ? 49 : 1; if (!integer(it.value(),0,high)) { return false; } }
    } return true;
}
bool rowsValid(const QVariant& value,bool quick) {
    if (value.typeId()!=QMetaType::QVariantList) { return false; } const QVariantList rows=value.toList(); if (rows.size()!=(quick ? 10 : 120)) { return false; }
    const QSet<QString> states{"unread","empty","populated","partial","invalid","timeout","unavailable"};
    for (int index=0;index<rows.size();++index) { if (rows[index].typeId()!=QMetaType::QVariantMap) { return false; } const QVariantMap row=rows[index].toMap(); quint64 revision=0; const QString kind=quick ? QStringLiteral("quick") : index<100 ? QStringLiteral("standard") : index<110 ? QStringLiteral("program") : QStringLiteral("extended");
        if (row.size()!=7 || !integer(row.value("index"),index,index) || row.value("kind").typeId()!=QMetaType::QString || row.value("kind").toString()!=kind || !text(row.value("state"),32,false) || !states.contains(row.value("state").toString()) || !sequence(row.value("revision"),revision) || !text(row.value("reason"),2048) || !hexText(row.value("rawUtf8Hex"),512) || row.value("fields").typeId()!=QMetaType::QVariantMap || !fieldsValid(row.value("fields").toMap(),quick)) { return false; }
        if (kind!=QLatin1String("program") && row.value("fields").toMap().contains("frequency2Role")) { return false; }
    } return true;
}
bool metadataValid(const QVariantMap& metadata) {
    const QMap<QString,QPair<int,int>> bounds{{"selectedChannel",{0,119}},{"memoryMode",{0,1}},{"quickMode",{0,1}},{"quickChannel",{0,9}},{"configBank",{0,1}},{"quickCapacityRaw",{0,2}},{"scanActive",{0,1}},{"programSlowRange",{0,1}},{"scanSpeed",{1,9}},{"toneScan",{0,2}},{"scanType",{0,1}}};
    for (auto it=metadata.cbegin();it!=metadata.cend();++it) { if (it.key()=="temporaryLeftHz" || it.key()=="temporaryRightHz") { if (!hz(it.value())) { return false; } } else if (!bounds.contains(it.key()) || !integer(it.value(),bounds.value(it.key()).first,bounds.value(it.key()).second)) { return false; } } return true;
}
bool statusValid(const QVariantMap& status) {
    if (status.size()!=9 || !text(status.value("context"),512,false) || status.value("connected").typeId()!=QMetaType::Bool || status.value("readOnly").typeId()!=QMetaType::Bool || !status.value("readOnly").toBool() || !text(status.value("reason"),2048) || status.value("metadata").typeId()!=QMetaType::QVariantMap || !metadataValid(status.value("metadata").toMap()) || !rowsValid(status.value("channels"),false) || !rowsValid(status.value("quickChannels"),true) || status.value("refresh").typeId()!=QMetaType::QVariantMap) { return false; }
    const QVariantMap refresh=status.value("refresh").toMap(); if (refresh.size()!=9 || !text(refresh.value("token"),128) || refresh.value("busy").typeId()!=QMetaType::Bool || !text(refresh.value("pendingKey"),128) || !text(refresh.value("reason"),2048)) { return false; }
    quint64 token=0;
    if (!refresh.value("token").toString().isEmpty() && !sequence(refresh.value("token"),token)) { return false; }
    for (const QString& key : {QString("completed"),QString("total"),QString("observed"),QString("failed"),QString("skipped")}) { if (!integer(refresh.value(key),0,256)) { return false; } }
    return refresh.value("completed").toInt()<=refresh.value("total").toInt() && refresh.value("observed").toInt()+refresh.value("failed").toInt()+refresh.value("skipped").toInt()==refresh.value("completed").toInt() && (!refresh.value("busy").toBool() || (status.value("connected").toBool() && !refresh.value("token").toString().isEmpty()));
}
QString fieldsText(const QVariantMap& fields) { QStringList lines; for (auto it=fields.cbegin();it!=fields.cend();++it) { lines.append(it.key()+": "+it.value().toString()); } return lines.isEmpty() ? QObject::tr("No observed fields.") : lines.join('\n'); }
QString rowSummary(const QVariantMap& fields,bool quick) {
    QStringList parts;
    if (!fields.value("name").toString().isEmpty()) { parts.append(fields.value("name").toString()); }
    const QString first=quick ? QStringLiteral("leftFrequencyHz") : QStringLiteral("frequency1Hz");
    const QString mode=quick ? QStringLiteral("leftModeLabel") : QStringLiteral("mode1Label");
    if (fields.contains(first)) { parts.append(QObject::tr("%1 Hz %2").arg(QString::number(fields.value(first).toLongLong()),fields.contains(mode) ? fields.value(mode).toString() : QObject::tr("mode Unknown"))); }
    const QString second=quick ? QStringLiteral("rightFrequencyHz") : QStringLiteral("frequency2Hz");
    const QString secondMode=quick ? QStringLiteral("rightModeLabel") : QStringLiteral("mode2Label");
    if (fields.contains(second) && fields.value(second).toLongLong()>0) { parts.append(QObject::tr("%1: %2 Hz %3").arg(fields.contains("frequency2Role") ? QObject::tr("Second (unqualified)") : QObject::tr("Second"),QString::number(fields.value(second).toLongLong()),fields.contains(secondMode) ? fields.value(secondMode).toString() : QObject::tr("mode Unknown"))); }
    return parts.isEmpty() ? QObject::tr("No observed fields") : parts.join(QStringLiteral(" | "));
}
QString metadataSummary(const QVariantMap& metadata) {
    const auto value=[&metadata](const QString& key) { return metadata.contains(key) ? metadata.value(key).toString() : QObject::tr("Unknown"); };
    const auto choice=[&metadata](const QString& key,const QStringList& labels) { if (!metadata.contains(key)) { return QObject::tr("Unknown"); } const int index=metadata.value(key).toInt(); return index>=0 && index<labels.size() ? labels[index] : QObject::tr("Unknown"); };
    const auto frequency=[&metadata](const QString& key) { return metadata.contains(key) ? QObject::tr("%1 Hz").arg(QString::number(metadata.value(key).toLongLong())) : QObject::tr("Unknown"); };
    const QStringList offOn{QObject::tr("Off"),QObject::tr("On")};
    return QObject::tr("Channel: %1 | Mode: %2 | Environment: %3\nQuick memory: %4 | Slot: %5 | Capacity: %6\nTemporary frequencies — Left: %7 | Right: %8\nScan: %9 | Slow range: %10 | Speed: %11 | Type: %12 | Tone scan: %13")
        .arg(value("selectedChannel"),choice("memoryMode",{QObject::tr("VFO"),QObject::tr("Memory")}),choice("configBank",{QStringLiteral("A"),QStringLiteral("B")}),choice("quickMode",offOn),value("quickChannel"),choice("quickCapacityRaw",{QStringLiteral("3"),QStringLiteral("5"),QStringLiteral("10")}),frequency("temporaryLeftHz"),frequency("temporaryRightHz"),choice("scanActive",offOn),choice("programSlowRange",offOn),value("scanSpeed"),choice("scanType",{QObject::tr("Program"),QObject::tr("VFO")}),choice("toneScan",{QObject::tr("Off"),QObject::tr("Tone"),QObject::tr("CTCSS")}));
}
QString channelLabel(int index,bool quick) { return quick ? QString::number(index) : index<100 ? QString::number(index).rightJustified(2,'0') : (index<110 ? QStringLiteral("P") : QStringLiteral("E"))+QString::number(index%10); }
quint64 nextRequest() { static quint64 request=0x52584d5200000000ULL; return ++request; }
}

NativeMemoryReadWidget::NativeMemoryReadWidget(RadioModel& model,QWidget* parent)
    : QWidget(parent),m_model(model),m_availability(new ControlAvailabilityRegistry(model,this)),
      m_channels(new QTableWidget(120,4,this)),m_quick(new QTableWidget(10,4,this)),m_channelDraft(new QSpinBox(this)),m_quickDraft(new QSpinBox(this)),
      m_metadata(new QLabel(this)),m_progress(new QLabel(this)),m_status(new QLabel(this)),m_details(new QPlainTextEdit(this)),
      m_readAll(new QPushButton(tr("Read all memories"),this)),m_readChannel(new QPushButton(tr("Read selected channel"),this)),m_readQuick(new QPushButton(tr("Read selected quick memory"),this)),m_refresh(new QPushButton(tr("Refresh memory status"),this)),m_cancel(new QPushButton(tr("Cancel reads"),this))
{
    setObjectName("nativeMemoryReadSettings"); auto* layout=new QVBoxLayout(this);
    auto* explanation=new QLabel(tr("Read-only snapshots of the radio's stored memories. These are separate from the application's host memory bank. Reading never recalls, writes, imports or starts scanning. Records may be observed at different times. Program-channel second-frequency roles are not qualified by this view."),this); explanation->setWordWrap(true); layout->addWidget(explanation);
    auto* buttons=new QGridLayout; layout->addLayout(buttons); m_channelDraft->setRange(0,119); m_quickDraft->setRange(0,9); m_channelDraft->setObjectName("nativeMemoryChannelDraft"); m_quickDraft->setObjectName("nativeQuickMemoryDraft"); m_channelDraft->setAccessibleName(tr("Stored channel to read, 0 to 119")); m_quickDraft->setAccessibleName(tr("Quick memory to read, 0 to 9"));
    buttons->addWidget(m_channelDraft,0,0); buttons->addWidget(m_readChannel,0,1); buttons->addWidget(m_quickDraft,1,0); buttons->addWidget(m_readQuick,1,1); buttons->addWidget(m_readAll,2,0); buttons->addWidget(m_refresh,2,1); buttons->addWidget(m_cancel,2,2);
    const std::array<QString,5> names{"nativeMemoryReadAll","nativeMemoryReadChannel","nativeMemoryReadQuick","nativeMemoryRefreshStatus","nativeMemoryCancel"}; const std::array<QPushButton*,5> controls{m_readAll,m_readChannel,m_readQuick,m_refresh,m_cancel};
    for (int index=0;index<5;++index) { QPushButton* button=controls[index]; button->setObjectName(names[index]); button->setAutoDefault(false); m_availability->registerWidget(button,tr("Requires a connected supported radio and a current native memory read context."),[this,index](bool connected,const RadioCapabilities& caps) { return connected && !memoryNamespace(caps).isEmpty() && (index==4 ? canCancel() : index==3 && m_confirmed.isEmpty() ? m_requests.isEmpty() : canRead()); },{},false); }
    for (QSpinBox* selector : {m_channelDraft,m_quickDraft}) { m_availability->registerWidget(selector,tr("Requires a connected radio supporting native memory reads; selection alone never sends a command."),[this](bool connected,const RadioCapabilities& caps) { return connected && !memoryNamespace(caps).isEmpty(); },{},false); }
    m_metadata->setObjectName("nativeMemoryStatus"); m_metadata->setAccessibleName(tr("Current radio memory status")); m_progress->setObjectName("nativeMemoryProgress"); m_status->setObjectName("nativeMemoryRequestStatus"); for (QLabel* label : {m_metadata,m_progress,m_status}) { label->setTextFormat(Qt::PlainText); label->setWordWrap(true); label->setSizePolicy(QSizePolicy::Preferred,QSizePolicy::Minimum); layout->addWidget(label); }
    for (int group=0;group<2;++group) { QTableWidget* table=group ? m_quick : m_channels; table->setObjectName(group ? "nativeQuickMemories" : "nativeMemoryChannels"); table->setAccessibleName(group ? tr("Radio quick memory snapshots") : tr("Radio normal, program and extended memory snapshots")); table->setHorizontalHeaderLabels({tr("Channel"),tr("State"),tr("Last observed fields"),tr("Reason")}); table->setEditTriggers(QAbstractItemView::NoEditTriggers); table->setSelectionBehavior(QAbstractItemView::SelectRows); table->setSelectionMode(QAbstractItemView::SingleSelection); table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents); table->horizontalHeader()->setStretchLastSection(true); for (int row=0;row<table->rowCount();++row) { for (int col=0;col<4;++col) { table->setItem(row,col,new QTableWidgetItem(col==0 ? channelLabel(row,group) : col==1 ? tr("unread") : QString{})); } } layout->addWidget(table); connect(table,&QTableWidget::itemSelectionChanged,this,[this,group] { inspect(group!=0); }); }
    m_details->setObjectName("nativeMemoryDetails"); m_details->setAccessibleName(tr("Selected radio memory snapshot details")); m_details->setReadOnly(true); layout->addWidget(m_details);
    connect(m_readAll,&QPushButton::clicked,this,[this] { submit("memory.read.refresh",{{"context",m_confirmed.value("context")},{"scope","all"}}); });
    connect(m_refresh,&QPushButton::clicked,this,[this] { if (m_confirmed.isEmpty()) { m_refreshAfterGet=true; submit("memory.read.get"); } else { submit("memory.read.refresh",{{"context",m_confirmed.value("context")},{"scope","metadata"}}); } });
    connect(m_readChannel,&QPushButton::clicked,this,[this] { submit("memory.read.channel",{{"context",m_confirmed.value("context")},{"channel",m_channelDraft->value()}}); });
    connect(m_readQuick,&QPushButton::clicked,this,[this] { submit("memory.read.quick",{{"context",m_confirmed.value("context")},{"channel",m_quickDraft->value()}}); });
    connect(m_cancel,&QPushButton::clicked,this,[this] { submit("memory.read.cancel",{{"context",m_confirmed.value("context")},{"token",m_confirmed.value("refresh").toMap().value("token")}}); });
    connect(&model,&RadioModel::connectionStateChanged,this,&NativeMemoryReadWidget::bindBackend); connect(&model,&RadioModel::capabilitiesChanged,this,&NativeMemoryReadWidget::bindBackend); bindBackend();
}
NativeMemoryReadWidget::~NativeMemoryReadWidget() { for (QObject* child : findChildren<QObject*>()) { QObject::disconnect(child,nullptr,this,nullptr); } QObject::disconnect(&m_model,nullptr,this,nullptr); QObject::disconnect(&m_model,nullptr,m_availability,nullptr); for (const QMetaObject::Connection& connection : m_connections) { QObject::disconnect(connection); } }
bool NativeMemoryReadWidget::canRead() const { return m_requests.isEmpty() && m_confirmed.value("connected").toBool() && !m_confirmed.value("refresh").toMap().value("busy").toBool(); }
bool NativeMemoryReadWidget::canCancel() const { return m_requests.isEmpty() && m_confirmed.value("connected").toBool() && m_confirmed.value("refresh").toMap().value("busy").toBool(); }
void NativeMemoryReadWidget::bindBackend() {
    IRadioBackend* backend=m_model.isConnected() ? m_model.backend() : nullptr; const QString ns=backend ? memoryNamespace(m_model.backendCapabilities()) : QString{};
    if (backend==m_backend && ns==m_namespace && m_receiveSession==m_model.receiveControlSession()) { render(); return; }
    const quint64 generation=++m_bindingGeneration; m_receiveSession=m_model.receiveControlSession(); const RadioControlSession session=m_model.controlSession(); for (const QMetaObject::Connection& connection : m_connections) { disconnect(connection); }
    m_backend=backend; m_namespace=ns; m_confirmed.clear(); m_requests.clear(); m_refreshAfterGet=false; m_statusRevision=0; ++m_statusSerial;
    const QPointer<NativeMemoryReadWidget> alive(this); render(); if (!alive || generation!=m_bindingGeneration || !backend || ns.isEmpty()) { return; }
    const QPointer<IRadioBackend> source(backend); const auto current=[this,source,generation,session] { return source && generation==m_bindingGeneration && source==m_backend && source==m_model.backend() && m_model.isConnected() && m_model.controlSessionCurrent(session); };
    m_connections[0]=connect(backend,&IRadioBackend::extensionStatus,this,[this,current,ns](const QString& space,const QString& kind,const QVariantMap& status) { if (current() && space==ns && kind=="memoryRead") { acceptStatus(status); } });
    m_connections[1]=connect(backend,&IRadioBackend::extensionResult,this,[this,current](quint64 request,const QVariant& result) { if (!current() || !m_requests.contains(request)) { return; } const QString verb=m_requests.take(request); const QPointer<NativeMemoryReadWidget> alive(this);
        if (verb!="memory.read.get" || m_getSerial==m_statusSerial) { acceptStatus(result.toMap()); } else { render(); }
        if (!alive || !current()) { return; }
        if (verb=="memory.read.get" && m_refreshAfterGet) { m_refreshAfterGet=false; if (canRead()) { submit("memory.read.refresh",{{"context",m_confirmed.value("context")},{"scope","metadata"}}); } } });
    m_connections[2]=connect(backend,&IRadioBackend::extensionError,this,[this,current](quint64 request,const QString& error) { if (current() && m_requests.remove(request)) { m_refreshAfterGet=false; const QPointer<NativeMemoryReadWidget> alive(this); m_status->setText(error); if (alive) { render(); } } }); submit("memory.read.get");
}
void NativeMemoryReadWidget::acceptStatus(const QVariantMap& status) { quint64 revision=0; const bool ordered=sequence(status.value("statusRevision"),revision); if (ordered && revision<m_statusRevision) { return; } ++m_statusSerial; if (!ordered || !statusValid(status)) { m_confirmed.clear(); } else { m_statusRevision=revision; m_confirmed=status; } render(); }
void NativeMemoryReadWidget::render() {
    const QPointer<NativeMemoryReadWidget> alive(this); const quint64 generation=m_bindingGeneration,serial=m_statusSerial; const auto current=[this,alive,generation,serial] { return alive && generation==m_bindingGeneration && serial==m_statusSerial; };
    for (int group=0;group<2;++group) { const QPointer<QTableWidget> table(group ? m_quick : m_channels); QSignalBlocker blocker(table); const QVariantList rows=m_confirmed.value(group ? "quickChannels" : "channels").toList();
        for (int row=0;row<table->rowCount();++row) { const QVariantMap record=row<rows.size() ? rows[row].toMap() : QVariantMap{}; const QStringList values{channelLabel(row,group),record.isEmpty() ? tr("Unknown") : record.value("state").toString(),record.isEmpty() ? tr("Not observed") : rowSummary(record.value("fields").toMap(),group!=0),record.value("reason").toString()}; for (int col=0;col<4;++col) { table->item(row,col)->setText(values[col]); if (!table) { blocker.dismiss(); return; } if (!current()) { return; } } }
    }
    const QString unavailable=m_namespace.isEmpty() ? memoryUnavailableReason(m_model.backendCapabilities()) : QString{};
    m_metadata->setText(!unavailable.isEmpty() ? tr("Native memories unavailable: %1").arg(unavailable) : m_confirmed.isEmpty() ? tr("Current radio memory status: Unknown") : metadataSummary(m_confirmed.value("metadata").toMap())); if (!current()) { return; }
    const QVariantMap progress=m_confirmed.value("refresh").toMap(); m_progress->setText(m_confirmed.isEmpty() ? tr("Read progress: Unknown") : tr("Read progress: %1/%2; observed %3, failed %4, skipped %5. %6").arg(progress.value("completed").toInt()).arg(progress.value("total").toInt()).arg(progress.value("observed").toInt()).arg(progress.value("failed").toInt()).arg(progress.value("skipped").toInt()).arg(progress.value("reason").toString())); if (!current()) { return; }
    QAccessibleValueChangeEvent event(m_progress,m_progress->text()); QAccessible::updateAccessibility(&event); if (!current()) { return; }
    if ((m_inspectQuick ? m_quick : m_channels)->currentRow()>=0) { inspect(m_inspectQuick); } else { m_details->setPlainText(tr("Select a row to inspect its read-only snapshot.")); }
    if (current()) { refreshAvailability(); }
}
void NativeMemoryReadWidget::inspect(bool quick) { m_inspectQuick=quick; const int index=(quick ? m_quick : m_channels)->currentRow(); const QVariantList rows=m_confirmed.value(quick ? "quickChannels" : "channels").toList(); if (index<0 || index>=rows.size()) { m_details->setPlainText(tr("No observed record selected.")); return; } const QVariantMap row=rows[index].toMap(); m_details->setPlainText(tr("Channel %1 (%2) — %3\n%4\n%5\nRaw UTF-8 response (hex): %6").arg(channelLabel(index,quick),row.value("kind").toString(),row.value("state").toString(),row.value("reason").toString(),(row.value("kind")==QLatin1String("program") ? tr("Program-channel second-frequency role is unqualified; no TX or endpoint interpretation is assumed.\n") : QString{})+fieldsText(row.value("fields").toMap()),row.value("rawUtf8Hex").toString())); }
void NativeMemoryReadWidget::refreshAvailability() { const QPointer<NativeMemoryReadWidget> alive(this); const quint64 generation=m_bindingGeneration,serial=m_statusSerial; m_availability->refreshEngaged(); if (!alive || generation!=m_bindingGeneration || serial!=m_statusSerial) { return; } const QString unavailable=m_namespace.isEmpty() ? memoryUnavailableReason(m_model.backendCapabilities()) : QString{}; const QString reason=unavailable.isEmpty() ? m_confirmed.value("reason").toString() : unavailable; if (reason.isEmpty()) { return; } for (QWidget* control : {static_cast<QWidget*>(m_readAll),static_cast<QWidget*>(m_readChannel),static_cast<QWidget*>(m_readQuick),static_cast<QWidget*>(m_refresh),static_cast<QWidget*>(m_cancel),static_cast<QWidget*>(m_channelDraft),static_cast<QWidget*>(m_quickDraft)}) { const QPointer<QWidget> button(control); if (!button->isEnabled()) { button->setToolTip(reason); if (!button || !alive || generation!=m_bindingGeneration || serial!=m_statusSerial) { return; } button->setAccessibleDescription(reason); if (!button || !alive || generation!=m_bindingGeneration || serial!=m_statusSerial) { return; } } } }
void NativeMemoryReadWidget::submit(const QString& verb,const QVariantMap& arguments) { if (!m_backend || m_backend!=m_model.backend() || !m_model.isConnected() || m_namespace.isEmpty() || memoryNamespace(m_model.backendCapabilities())!=m_namespace || !m_requests.isEmpty() || (verb!="memory.read.get" && (verb=="memory.read.cancel" ? !canCancel() : !canRead()))) { return; } const quint64 request=nextRequest(); m_requests.insert(request,verb); if (verb=="memory.read.get") { m_getSerial=m_statusSerial; } const QPointer<NativeMemoryReadWidget> alive(this); const quint64 generation=m_bindingGeneration; render(); if (!alive || generation!=m_bindingGeneration || !m_requests.contains(request)) { return; } m_model.invokeBackendExtension(m_namespace,verb,request,arguments); }
} // namespace AetherSDR
