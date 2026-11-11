#include "NativePskReceiveWidget.h"
#include "ControlAvailabilityRegistry.h"
#include "GuardedSlider.h"
#include "models/RadioModel.h"
#include <QAccessible>
#include <QAccessibleValueChangeEvent>
#include <QComboBox>
#include <QGridLayout>
#include <QLabel>
#include <QList>
#include <QMetaType>
#include <QPushButton>
#include <QSet>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QStringList>
#include <QVBoxLayout>
#include <QVariantList>
namespace AetherSDR {
namespace {
const QStringList kIds{"afcRange"};
const QStringList kVerbs{"psk.receive.get","psk.receive.read","psk.receive.set","psk.receive.reset","psk.receive.cancel"};
bool text(const QVariant& value,int maximum=512,bool empty=true) { return value.typeId()==QMetaType::QString && value.toString().size()<=maximum && (empty || !value.toString().isEmpty()); }
bool seq(const QVariant& value,bool empty=false,bool positive=false) { if (!text(value,20,empty)) { return false; } if (empty && value.toString().isEmpty()) { return true; } bool ok=false; const quint64 n=value.toString().toULongLong(&ok); return ok && QString::number(n)==value.toString() && (!positive || n>0); }
bool flag(const QVariant& value) { return value.typeId()==QMetaType::Bool; }
bool integer(const QVariant& value,int low,int high) { return value.typeId()==QMetaType::Int && value.toInt()>=low && value.toInt()<=high; }
bool map(const QVariant& value) { return value.typeId()==QMetaType::QVariantMap; }
bool keys(const QVariantMap& value,const QStringList& required,const QStringList& optional={}) { for (const QString& key : required) { if (!value.contains(key)) { return false; } } for (auto it=value.cbegin();it!=value.cend();++it) { if (!required.contains(it.key()) && !optional.contains(it.key())) { return false; } } return true; }
QString nativeNamespace(const RadioCapabilities& caps) { for (const QString& ns : caps.extensionNamespaces) { const QVariantMap entry=caps.extensions.value(ns).toMap(); if (!integer(entry.value("nativePskReceiveVersion"),1,1) || entry.value("nativePskReceiveVerbs").typeId()!=QMetaType::QStringList) { continue; } bool complete=true; for (const QString& verb : kVerbs) { complete=complete && entry.value("nativePskReceiveVerbs").toStringList().contains(verb); } if (complete) { return ns; } } return {}; }
bool validStatus(const QVariantMap& status) {
    if (!keys(status,{"statusRevision","context","connected","reason","bank","rows","refresh","lastWrite","notice"}) || !seq(status.value("statusRevision")) || !text(status.value("context"),512,false) || !flag(status.value("connected")) || !text(status.value("reason"),2048) || !text(status.value("notice"),4096) || status.value("rows").typeId()!=QMetaType::QVariantList || status.value("rows").toList().size()!=1) { return false; }
    for (const QString& key : {QString("bank"),QString("refresh"),QString("lastWrite")}) { if (!map(status.value(key))) { return false; } }
    const QVariantMap bank=status.value("bank").toMap(); if (!keys(bank,{"known","revision","reason"},{"value"}) || !flag(bank.value("known")) || !seq(bank.value("revision")) || !text(bank.value("reason"),2048) || (bank.contains("value") && !integer(bank.value("value"),0,1)) || (bank.value("known").toBool() && (!bank.contains("value") || !status.value("connected").toBool()))) { return false; }
    QSet<QString> ids; bool anyPending=false,anyWritable=false;
    for (const QVariant& raw : status.value("rows").toList()) {
        const QVariantMap row=raw.toMap(); const QString id=row.value("id").toString();
        if (!map(raw) || !keys(row,{"id","label","known","revision","choices","editable","resettable","pending","reason","applicability"},{"value"}) || !text(row.value("id")) || !kIds.contains(id) || ids.contains(id) || !text(row.value("label"),512,false) || !seq(row.value("revision")) || !flag(row.value("known")) || !flag(row.value("editable")) || !flag(row.value("resettable")) || !flag(row.value("pending")) || !text(row.value("reason"),2048) || !text(row.value("applicability"),2048) || (row.contains("value") && !integer(row.value("value"),0,1)) || (row.value("known").toBool() && (!row.contains("value") || !bank.value("known").toBool() || !status.value("connected").toBool())) || row.value("choices").typeId()!=QMetaType::QVariantList || row.value("choices").toList().size()!=2) { return false; }
        const bool writable=row.value("editable").toBool() || row.value("resettable").toBool(); if (writable && (!status.value("connected").toBool() || !bank.value("known").toBool() || !row.value("known").toBool() || row.value("pending").toBool())) { return false; } anyPending=anyPending || row.value("pending").toBool(); anyWritable=anyWritable || writable; ids.insert(id);
        QSet<int> values; for (const QVariant& choiceRaw : row.value("choices").toList()) { const QVariantMap choice=choiceRaw.toMap(); if (!map(choiceRaw) || !keys(choice,{"value","label"}) || !integer(choice.value("value"),0,1) || values.contains(choice.value("value").toInt()) || !text(choice.value("label"),512,false)) { return false; } values.insert(choice.value("value").toInt()); }
    }
    const QVariantMap refresh=status.value("refresh").toMap(); if (!keys(refresh,{"token","busy","total","completed","observed","failed","skipped","pendingKey","reason"}) || !seq(refresh.value("token"),true,true) || !flag(refresh.value("busy")) || !text(refresh.value("pendingKey"),16) || !text(refresh.value("reason"),2048) || !integer(refresh.value("skipped"),0,0)) { return false; }
    for (const QString& key : {QString("total"),QString("completed"),QString("observed"),QString("failed")}) { if (!integer(refresh.value(key),0,2)) { return false; } }
    if (refresh.value("completed").toInt()!=refresh.value("observed").toInt()+refresh.value("failed").toInt() || refresh.value("completed").toInt()>refresh.value("total").toInt() || (refresh.value("busy").toBool() && (refresh.value("token").toString().isEmpty() || !status.value("connected").toBool())) || (anyWritable && (anyPending || refresh.value("busy").toBool()))) { return false; }
    const QVariantMap write=status.value("lastWrite").toMap(); return keys(write,{"token","id","operation","state","reason"}) && seq(write.value("token"),true,true) && text(write.value("id")) && (write.value("id").toString().isEmpty() || kIds.contains(write.value("id").toString())) && text(write.value("operation")) && QStringList{"","set","reset"}.contains(write.value("operation").toString()) && text(write.value("state")) && QStringList{"none","pending","sent_unverified","observed_consistent","indeterminate","rejected"}.contains(write.value("state").toString()) && text(write.value("reason"),2048) && !(write.value("operation")==QLatin1String("reset") && write.value("state")==QLatin1String("observed_consistent"));
}
quint64 nextRequest() { static quint64 value=0x50534B5200000000ULL; return ++value; }
}
NativePskReceiveWidget::NativePskReceiveWidget(RadioModel& model,QWidget* parent)
    : QWidget(parent),m_model(model),m_availability(new ControlAvailabilityRegistry(model,this)),m_read(new QPushButton(tr("Read PSK receive settings"),this)),m_cancel(new QPushButton(tr("Cancel reads"),this)),m_bank(new QLabel(this)),m_notice(new QLabel(this)),m_progress(new QLabel(this)),m_write(new QLabel(this)),m_status(new QLabel(this))
{
    setObjectName("pskReceiveSettings"); auto* layout=new QVBoxLayout(this); auto* explanation=new QLabel(tr("Choose the PSK AFC tuning range stored in the radio: ±8 Hz or ±15 Hz. Changing this preference does not enable AFC or NET. Select a range and Apply explicitly. Reset asks the radio for its initial value; the displayed result follows radio readback."),this); explanation->setTextFormat(Qt::PlainText); explanation->setWordWrap(true); explanation->setSizePolicy(QSizePolicy::Preferred,QSizePolicy::Minimum); layout->addWidget(explanation); auto* grid=new QGridLayout; layout->addLayout(grid); int index=0;
    for (const QString& id : kIds) { Row row; row.observed=new QLabel(tr("Unknown"),this); row.observed->setObjectName("pskReceive_"+id+"Observed"); row.observed->setAccessibleName(tr("Observed PSK AFC tuning range")); row.observed->setTextFormat(Qt::PlainText); row.observed->setWordWrap(true); row.observed->setSizePolicy(QSizePolicy::Preferred,QSizePolicy::Minimum); row.draft=new GuardedComboBox(this); row.draft->setObjectName("pskReceive_"+id+"Draft"); row.draft->setAccessibleName(tr("PSK AFC tuning range draft")); row.apply=new QPushButton(tr("Apply"),this); row.apply->setObjectName("pskReceive_"+id+"Apply"); row.apply->setAccessibleName(tr("Apply PSK AFC tuning range")); row.reset=new QPushButton(tr("Reset to radio initial value"),this); row.reset->setObjectName("pskReceive_"+id+"Reset"); row.reset->setAccessibleName(tr("Reset PSK AFC tuning range to radio initial value")); row.apply->setAutoDefault(false); row.reset->setAutoDefault(false); grid->addWidget(row.observed,index,0); grid->addWidget(row.draft,index,1); grid->addWidget(row.apply,index,2); grid->addWidget(row.reset,index,3); ++index; m_rows.insert(id,row);
        connect(row.draft,&QComboBox::activated,this,[this,id](int index) { editDraft(id,index); }); connect(row.apply,&QPushButton::clicked,this,[this,id] { if (available(id,true)) { const Row& owner=m_rows[id]; submit("psk.receive.set",{{"context",owner.context},{"id",id},{"revision",owner.revision},{"value",owner.value}}); } }); connect(row.reset,&QPushButton::clicked,this,[this,id] { if (available(id,false,true)) { submit("psk.receive.reset",{{"context",m_confirmed.value("context")},{"id",id},{"revision",descriptor(id).value("revision")}}); } });
        for (QWidget* control : {static_cast<QWidget*>(row.draft),static_cast<QWidget*>(row.apply),static_cast<QWidget*>(row.reset)}) { m_availability->registerWidget(control,tr("Connect to a radio supporting native PSK receive settings."),[this,id,control](bool connected,const RadioCapabilities& caps) { return connected && !nativeNamespace(caps).isEmpty() && available(id,control==m_rows[id].apply,control==m_rows[id].reset); },{},false); }
    }
    m_read->setObjectName("pskReceiveRead"); m_cancel->setObjectName("pskReceiveCancel"); m_read->setAccessibleName(tr("Read PSK receive settings")); m_cancel->setAccessibleName(tr("Cancel PSK receive reads")); m_read->setAutoDefault(false); m_cancel->setAutoDefault(false); grid->addWidget(m_read,index,0,1,2); grid->addWidget(m_cancel,index,2,1,2);
    for (QPushButton* button : {m_read,m_cancel}) { m_availability->registerWidget(button,tr("Connect to a radio supporting native PSK receive settings."),[this,button](bool connected,const RadioCapabilities& caps) { if (!connected || nativeNamespace(caps).isEmpty() || !m_requests.isEmpty()) { return false; } const bool reading=m_confirmed.value("refresh").toMap().value("busy").toBool(); return button==m_read ? !reading : reading; },{},false); }
    const std::array<QLabel*,5> labels{m_bank,m_notice,m_progress,m_write,m_status}; const std::array<QString,5> names{"pskReceiveBank","pskReceiveNotice","pskReceiveProgress","pskReceiveWriteStatus","pskReceiveStatus"}; for (int label=0;label<5;++label) { labels[label]->setObjectName(names[label]); labels[label]->setAccessibleName(label==0 ? tr("Observed radio configuration") : label==1 ? tr("PSK receive notice") : label==2 ? tr("Read progress") : label==3 ? tr("Last PSK range request") : tr("PSK receive status")); labels[label]->setTextFormat(Qt::PlainText); labels[label]->setWordWrap(true); labels[label]->setSizePolicy(QSizePolicy::Preferred,QSizePolicy::Minimum); layout->addWidget(labels[label]); } layout->addStretch();
    connect(m_read,&QPushButton::clicked,this,&NativePskReceiveWidget::read); connect(m_cancel,&QPushButton::clicked,this,[this] { submit("psk.receive.cancel",{{"context",m_confirmed.value("context")},{"token",m_confirmed.value("refresh").toMap().value("token")}}); }); connect(&model,&RadioModel::connectionStateChanged,this,&NativePskReceiveWidget::bindBackend); connect(&model,&RadioModel::capabilitiesChanged,this,&NativePskReceiveWidget::bindBackend); bindBackend();
}
NativePskReceiveWidget::~NativePskReceiveWidget() { for (QObject* child : findChildren<QObject*>()) { QObject::disconnect(child,nullptr,this,nullptr); } QObject::disconnect(&m_model,nullptr,this,nullptr); QObject::disconnect(&m_model,nullptr,m_availability,nullptr); for (const QMetaObject::Connection& connection : m_connections) { QObject::disconnect(connection); } }
QVariantMap NativePskReceiveWidget::descriptor(const QString& id) const { for (const QVariant& raw : m_confirmed.value("rows").toList()) { if (raw.toMap().value("id")==id) { return raw.toMap(); } } return {}; }
bool NativePskReceiveWidget::available(const QString& id,bool apply,bool reset) const { const QVariantMap row=descriptor(id); if (!m_requests.isEmpty() || !row.value(reset ? "resettable" : "editable").toBool() || row.value("pending").toBool()) { return false; } if (!apply) { return true; } const Row& owner=m_rows[id]; return owner.seeded && !owner.retired && owner.context==m_confirmed.value("context").toString() && owner.revision==row.value("revision").toString(); }
void NativePskReceiveWidget::editDraft(const QString& id,int index) {
    Row& row=m_rows[id];
    if (!available(id) || index<0 || index>=row.draft->count() || index!=row.draft->currentIndex() || !integer(row.draft->itemData(index),0,1)) { return; }
    row.seeded=row.dirty=true; row.retired=false; row.value=row.draft->itemData(index).toInt();
    row.baseline=descriptor(id).value("value").toInt(); row.context=m_confirmed.value("context").toString(); row.revision=descriptor(id).value("revision").toString(); refreshAvailability();
}
void NativePskReceiveWidget::read() { if (m_confirmed.isEmpty()) { m_readAfterGet=true; submit("psk.receive.get"); } else { submit("psk.receive.read",{{"context",m_confirmed.value("context")}}); } }
void NativePskReceiveWidget::bindBackend() {
    IRadioBackend* backend=m_model.isConnected() ? m_model.backend() : nullptr; const QString ns=backend ? nativeNamespace(m_model.backendCapabilities()) : QString{}; if (backend==m_backend && ns==m_namespace) { render(); return; }
    const quint64 generation=++m_bindingGeneration; const RadioControlSession session=m_model.controlSession(); for (const QMetaObject::Connection& connection : m_connections) { disconnect(connection); }
    m_backend=backend; m_namespace=ns; m_confirmed.clear(); m_requests.clear(); m_statusRevision=0; ++m_statusSerial; m_readAfterGet=false; for (Row& row : m_rows) { row.seeded=row.dirty=row.retired=false; row.value=row.baseline=-1; row.context.clear(); row.revision.clear(); }
    const QPointer<NativePskReceiveWidget> alive(this); m_status->clear(); if (!alive || generation!=m_bindingGeneration) { return; } render(); if (!alive || generation!=m_bindingGeneration || !backend || ns.isEmpty()) { return; }
    const QPointer<IRadioBackend> source(backend); const auto current=[this,alive,source,generation,session] { return alive && source && generation==m_bindingGeneration && source==m_backend && source==m_model.backend() && m_model.isConnected() && m_model.controlSessionCurrent(session); };
    m_connections[0]=connect(backend,&IRadioBackend::extensionStatus,this,[this,current,ns](const QString& space,const QString& kind,const QVariantMap& status) { if (current() && space==ns && kind=="pskReceive") { acceptStatus(status); } });
    m_connections[1]=connect(backend,&IRadioBackend::extensionResult,this,[this,current](quint64 request,const QVariant& result) { if (!current() || !m_requests.contains(request)) { return; } const QString verb=m_requests.take(request); const QPointer<NativePskReceiveWidget> alive(this);
        if (verb!="psk.receive.set" && verb!="psk.receive.reset") { if (verb!="psk.receive.get" || m_getSerial==m_statusSerial) { acceptStatus(map(result) ? result.toMap() : QVariantMap{}); } else { render(); } if (!alive || !current()) { return; } if (verb=="psk.receive.get" && m_readAfterGet) { m_readAfterGet=false; if (!m_confirmed.isEmpty() && !m_confirmed.value("refresh").toMap().value("busy").toBool()) { submit("psk.receive.read",{{"context",m_confirmed.value("context")}}); } } return; }
        const QVariantMap reply=result.toMap(); const bool valid=map(result) && keys(reply,{"sent","verified","token"}) && flag(reply.value("sent")) && reply.value("sent").toBool() && flag(reply.value("verified")) && !reply.value("verified").toBool() && seq(reply.value("token"),false,true); m_status->setText(valid ? tr("Request sent; see Last request for the current radio observation. Reset does not predict the initial value.") : tr("Invalid handoff result; no confirmation inferred.")); if (alive && current()) { render(); }
    });
    m_connections[2]=connect(backend,&IRadioBackend::extensionError,this,[this,current](quint64 request,const QString& reason) { if (current() && m_requests.remove(request)) { m_readAfterGet=false; const QPointer<NativePskReceiveWidget> alive(this); m_status->setText(reason); if (alive && current()) { render(); } } }); submit("psk.receive.get");
}
void NativePskReceiveWidget::acceptStatus(const QVariantMap& status) {
    if (seq(status.value("statusRevision")) && status.value("statusRevision").toString().toULongLong()<m_statusRevision) { return; }
    ++m_statusSerial;
    if (!validStatus(status)) { m_confirmed.clear(); for (Row& row : m_rows) { row.retired=true; } }
    else { m_statusRevision=status.value("statusRevision").toString().toULongLong(); m_confirmed=status; }
    render();
}
void NativePskReceiveWidget::render() {
    const QPointer<NativePskReceiveWidget> alive(this); const quint64 generation=m_bindingGeneration,serial=m_statusSerial; const auto current=[this,alive,generation,serial] { return alive && generation==m_bindingGeneration && serial==m_statusSerial; };
    for (const QString& id : kIds) { Row& row=m_rows[id]; const QVariantMap value=descriptor(id); const bool known=value.value("known").toBool(); const int observed=value.value("value").toInt(); const QString context=m_confirmed.value("context").toString();
        if (!known) { row.retired=true; }
        else if (!row.seeded || !row.dirty) { row.seeded=true; row.retired=false; row.value=row.baseline=observed; row.context=context; row.revision=value.value("revision").toString(); }
        else if (row.context!=context || row.revision!=value.value("revision").toString()) { row.retired=true; }
        { const QPointer<QComboBox> editor(row.draft); QSignalBlocker blocker(editor); const auto safe=[&] { if (!editor) { blocker.dismiss(); } return editor && current(); }; editor->clear(); if (!safe()) { return; } const QVariantList choices=value.value("choices").toList(); if (choices.isEmpty()) { editor->addItem(tr("±8 Hz"),0); if (!safe()) { return; } editor->addItem(tr("±15 Hz"),1); if (!safe()) { return; } } else { for (const QVariant& raw : choices) { const QVariantMap choice=raw.toMap(); editor->addItem(choice.value("label").toString(),choice.value("value")); if (!safe()) { return; } } } editor->setCurrentIndex(editor->findData(row.value)); if (!safe()) { return; } }
        QString label=tr("Unknown"); if (value.contains("value")) { QString native=QString::number(observed); for (const QVariant& raw : value.value("choices").toList()) { if (raw.toMap().value("value").toInt()==observed) { native=raw.toMap().value("label").toString(); } } label=known ? native : tr("Unknown; last observed %1").arg(native); }
        row.observed->setText(tr("%1: %2%3").arg(value.value("label",tr("PSK AFC tuning range")).toString(),label,value.value("applicability").toString().isEmpty() ? QString{} : tr(" — %1").arg(value.value("applicability").toString()))); if (!current()) { return; } QAccessibleValueChangeEvent event(row.observed,label); QAccessible::updateAccessibility(&event); if (!current()) { return; }
    }
    const QVariantMap bank=m_confirmed.value("bank").toMap();
    const QString bankValue=bank.value("known").toBool() ? (bank.value("value").toInt()==0 ? tr("CONFIG A") : tr("CONFIG B")) : bank.contains("value") ? tr("Unknown; last observed %1").arg(bank.value("value").toInt()==0 ? tr("CONFIG A") : tr("CONFIG B")) : tr("Unknown");
    m_bank->setText(tr("Observed radio configuration: %1. %2").arg(bankValue,bank.value("reason").toString())); if (!current()) { return; }
    m_notice->setText(m_confirmed.isEmpty() ? tr("PSK AFC tuning range is stored by the radio. Read the current configuration and value; no initial value is assumed.") : m_confirmed.value("notice").toString()); if (!current()) { return; }
    const QVariantMap progress=m_confirmed.value("refresh").toMap(); m_progress->setText(m_confirmed.isEmpty() ? tr("Read progress: Unknown") : tr("Read progress %1/%2; observed %3, failed %4. %5").arg(progress.value("completed").toInt()).arg(progress.value("total").toInt()).arg(progress.value("observed").toInt()).arg(progress.value("failed").toInt()).arg(progress.value("reason").toString())); if (!current()) { return; }
    const QVariantMap write=m_confirmed.value("lastWrite").toMap();
    const QString state=write.value("state").toString();
    const QString stateText=state=="none" ? tr("None") : state=="pending" ? tr("Waiting to send")
        : state=="sent_unverified" ? tr("Sent; outcome unverified") : state=="observed_consistent" ? tr("Readback matches the request")
        : state=="indeterminate" ? tr("Outcome unknown") : state=="rejected" ? tr("Request rejected") : tr("Unknown");
    m_write->setText(state=="none" ? tr("Last request: None") : tr("Last request: PSK AFC tuning range — %1. %2").arg(stateText,write.value("reason").toString())); if (current()) { refreshAvailability(); }

}
void NativePskReceiveWidget::refreshAvailability() {
    const QPointer<NativePskReceiveWidget> alive(this); const quint64 generation=m_bindingGeneration,serial=m_statusSerial; const auto current=[this,alive,generation,serial] { return alive && generation==m_bindingGeneration && serial==m_statusSerial; }; m_availability->refreshEngaged(); if (!current()) { return; }
    QList<QWidget*> controls{m_read,m_cancel}; for (const Row& row : m_rows) { controls<<row.draft<<row.apply<<row.reset; }
    for (QWidget* control : controls) { if (control->isEnabled()) { continue; } QString reason; if (!m_model.isConnected()) { reason=tr("Connect to a radio supporting native PSK receive settings."); } else if (nativeNamespace(m_model.backendCapabilities()).isEmpty()) { reason=tr("This radio does not support native PSK receive settings."); } else if (m_confirmed.isEmpty()) { reason=tr("Read PSK receive settings to obtain a valid current bank and radio description."); } else { for (auto it=m_rows.cbegin();it!=m_rows.cend();++it) { if (control==it.value().draft || control==it.value().apply || control==it.value().reset) { reason=descriptor(it.key()).value("reason").toString(); } } }
        if (reason.isEmpty()) { if (!m_requests.isEmpty()) { reason=tr("Waiting for the current native request."); } else if (control==m_cancel) { reason=tr("No native read is running."); } else if (control==m_read) { reason=tr("Cancel the current read before starting another."); } else { reason=tr("Select the draft again to bind it to the current bank and native observation."); } }
        const QPointer<QWidget> target(control); target->setToolTip(reason); if (!target || !current()) { return; } target->setAccessibleDescription(reason); if (!current()) { return; }
    }
}
void NativePskReceiveWidget::submit(const QString& verb,const QVariantMap& arguments) {
    if (!m_backend || m_backend!=m_model.backend() || !m_model.isConnected() || m_namespace.isEmpty() || !m_requests.isEmpty()) { return; }
    const quint64 request=nextRequest(),generation=m_bindingGeneration,serial=m_statusSerial;
    const RadioControlSession session=m_model.controlSession(); m_requests.insert(request,verb);
    if (verb=="psk.receive.get") { m_getSerial=m_statusSerial; }
    const QPointer<NativePskReceiveWidget> alive(this); render();
    if (!alive || generation!=m_bindingGeneration || !m_requests.contains(request) || !m_model.controlSessionCurrent(session)) { return; }
    if (verb!="psk.receive.get" && serial!=m_statusSerial) { m_requests.remove(request); render(); return; }
    m_model.invokeBackendExtension(m_namespace,verb,request,arguments);
}
} // namespace AetherSDR
