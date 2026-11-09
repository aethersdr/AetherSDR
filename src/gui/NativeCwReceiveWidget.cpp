#include "NativeCwReceiveWidget.h"
#include "ControlAvailabilityRegistry.h"
#include "models/RadioModel.h"
#include <climits>
#include <QByteArray>
#include <QAccessible>
#include <QAccessibleValueChangeEvent>
#include <QComboBox>
#include <QGridLayout>
#include <QLabel>
#include <QList>
#include <QMetaType>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSet>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QStringList>
#include <QVBoxLayout>

namespace AetherSDR {
namespace {
const QStringList kIds{"screen","threshold","filter","enabled"};
const QStringList kVerbs{"cw.receive.get","cw.receive.read","cw.receive.set","cw.receive.cancel","cw.receive.clear"};
bool text(const QVariant& value,int maximum=512,bool empty=true) { return value.typeId()==QMetaType::QString && value.toString().size()<=maximum && (empty || !value.toString().isEmpty()); }
bool seq(const QVariant& value,bool empty=false,bool positive=false) { if (!text(value,20,empty)) { return false; } if (empty && value.toString().isEmpty()) { return true; } bool ok=false; const quint64 number=value.toString().toULongLong(&ok); return ok && QString::number(number)==value.toString() && (!positive || number>0); }
bool integer(const QVariant& value,int low,int high) { return value.typeId()==QMetaType::Int && value.toInt()>=low && value.toInt()<=high; }
bool flag(const QVariant& value) { return value.typeId()==QMetaType::Bool; }
bool keys(const QVariantMap& map,const QStringList& required,const QStringList& optional={}) { for (const QString& key : required) { if (!map.contains(key)) { return false; } } for (auto it=map.cbegin();it!=map.cend();++it) { if (!required.contains(it.key()) && !optional.contains(it.key())) { return false; } } return true; }
bool map(const QVariant& value) { return value.typeId()==QMetaType::QVariantMap; }
QString nativeNamespace(const RadioCapabilities& caps) { for (const QString& ns : caps.extensionNamespaces) { const QVariantMap value=caps.extensions.value(ns).toMap(); if (!integer(value.value("nativeCwReceiveVersion"),1,1) || value.value("nativeCwReceiveVerbs").typeId()!=QMetaType::QStringList) { continue; } bool complete=true; for (const QString& verb : kVerbs) { complete=complete && value.value("nativeCwReceiveVerbs").toStringList().contains(verb); } if (complete) { return ns; } } return {}; }
bool validText(const QVariantMap& stream,const QVariantMap& transcript) {
    if (!keys(stream,{"available","reason","aiKnown","nativeDecoderObserved","context","revision"},{"ai","lastEventElapsedMs"}) || !flag(stream.value("available")) || !flag(stream.value("aiKnown")) || !flag(stream.value("nativeDecoderObserved")) || !text(stream.value("reason"),2048) || !text(stream.value("context"),512,false) || !seq(stream.value("revision"))) { return false; }
    if ((stream.contains("ai") && (!integer(stream.value("ai"),0,4) || !QList<int>{0,2,4}.contains(stream.value("ai").toInt()))) || (stream.value("aiKnown").toBool() && !stream.contains("ai")) || (stream.value("available").toBool() && (!stream.value("aiKnown").toBool() || !QList<int>{2,4}.contains(stream.value("ai").toInt())))) { return false; }
    if (stream.contains("lastEventElapsedMs") && (stream.value("lastEventElapsedMs").typeId()!=QMetaType::LongLong || stream.value("lastEventElapsedMs").toLongLong()<0)) { return false; }
    if (!keys(transcript,{"revision","events","dropped","invalid","reason"}) || !seq(transcript.value("revision")) || transcript.value("revision")!=stream.value("revision") || !integer(transcript.value("dropped"),0,INT_MAX) || !integer(transcript.value("invalid"),0,INT_MAX) || !text(transcript.value("reason"),2048) || transcript.value("events").typeId()!=QMetaType::QVariantList || transcript.value("events").toList().size()>1024) { return false; }
    quint64 previous=0; int characters=0;
    for (const QVariant& raw : transcript.value("events").toList()) {
        const QVariantMap event=raw.toMap(); if (!map(raw) || !keys(event,{"sequence","context","label","kind","text","rawAsciiHex","elapsedMs"}) || !seq(event.value("sequence"),false,true) || event.value("sequence").toString().toULongLong()<=previous || !text(event.value("context"),512,false) || !text(event.value("label")) || !text(event.value("kind")) || !text(event.value("text"),4,false) || !text(event.value("rawAsciiHex"),4,false) || event.value("elapsedMs").typeId()!=QMetaType::LongLong || event.value("elapsedMs").toLongLong()<0) { return false; }
        previous=event.value("sequence").toString().toULongLong(); const QString hex=event.value("rawAsciiHex").toString(); const QByteArray bytes=QByteArray::fromHex(hex.toLatin1()); if ((hex.size()!=2 && hex.size()!=4) || bytes.toHex()!=hex.toLatin1().toLower()) { return false; }
        const QString value=QString::fromLatin1(bytes); const QString kind=event.value("kind").toString();
        if (kind=="prosign") { if (!QStringList{"BT","AR","AS","HH","SK","KN","BK","SN"}.contains(value) || event.value("text").toString()!=QString("<%1>").arg(value)) { return false; } }
        else if (kind=="character" || kind=="undecodable") { if (bytes.size()!=1 || static_cast<unsigned char>(bytes[0])<0x20 || static_cast<unsigned char>(bytes[0])>0x7e || bytes[0]==';' || value!=event.value("text").toString() || (kind=="undecodable")!=(value=="_")) { return false; } }
        else { return false; } characters+=event.value("text").toString().size(); if (characters>8192) { return false; }
    }
    return true;
}
bool validStatus(const QVariantMap& status) {
    if (!keys(status,{"statusRevision","context","connected","reason","controls","refresh","lastWrite","stream","transcript","restrictionNotice","autoZero"}) || !seq(status.value("statusRevision")) || !text(status.value("context"),512,false) || !flag(status.value("connected")) || !text(status.value("reason"),2048) || !text(status.value("restrictionNotice"),4096) || status.value("controls").typeId()!=QMetaType::QVariantList || status.value("controls").toList().size()!=4) { return false; }
    QSet<QString> ids;
    for (const QVariant& raw : status.value("controls").toList()) {
        const QVariantMap row=raw.toMap(); const QString id=row.value("id").toString(); const int low=id=="threshold" ? 1 : 0,high=id=="threshold" ? 30 : id=="filter" ? 2 : 1;
        if (!map(raw) || !keys(row,{"id","label","unit","minimum","maximum","step","known","revision","editable","pending","reason","choices"},{"value"}) || !text(row.value("id")) || !kIds.contains(id) || ids.contains(id) || !text(row.value("label"),512,false) || !text(row.value("unit")) || !integer(row.value("minimum"),low,low) || !integer(row.value("maximum"),high,high) || !integer(row.value("step"),1,1) || !flag(row.value("known")) || !seq(row.value("revision")) || !flag(row.value("editable")) || !flag(row.value("pending")) || !text(row.value("reason"),2048) || (row.contains("value") && !integer(row.value("value"),low,high)) || (row.value("known").toBool() && !row.contains("value")) || (row.value("editable").toBool() && (!status.value("connected").toBool() || !row.value("known").toBool() || row.value("pending").toBool())) || row.value("choices").typeId()!=QMetaType::QVariantList) { return false; }
        ids.insert(id); const QVariantList choices=row.value("choices").toList(); if (choices.size()!=(id=="threshold" ? 0 : high+1)) { return false; } QSet<int> values; bool any=false;
        for (const QVariant& choiceRaw : choices) { const QVariantMap choice=choiceRaw.toMap(); if (!map(choiceRaw) || !keys(choice,{"value","label","editable","reason"}) || !integer(choice.value("value"),low,high) || values.contains(choice.value("value").toInt()) || !text(choice.value("label"),512,false) || !flag(choice.value("editable")) || !text(choice.value("reason"),2048) || (choice.value("editable").toBool() && !row.value("editable").toBool())) { return false; } values.insert(choice.value("value").toInt()); any=any || choice.value("editable").toBool(); }
        if (id!="threshold" && any!=row.value("editable").toBool()) { return false; }
    }
    const QVariantMap autoZero=status.value("autoZero").toMap();
    if (!map(status.value("autoZero")) || !keys(autoZero,{"known","reason"},{"active"}) || !flag(autoZero.value("known")) || !text(autoZero.value("reason"),2048) || (autoZero.contains("active") && !flag(autoZero.value("active"))) || (autoZero.value("known").toBool() && (!autoZero.contains("active") || !status.value("connected").toBool()))) { return false; }
    for (const QString& key : {QString("refresh"),QString("lastWrite"),QString("stream"),QString("transcript")}) { if (!map(status.value(key))) { return false; } }
    const QVariantMap refresh=status.value("refresh").toMap(); if (!keys(refresh,{"token","busy","total","completed","observed","failed","pendingKey","reason"}) || !seq(refresh.value("token"),true,true) || !flag(refresh.value("busy")) || !text(refresh.value("pendingKey"),16) || !QStringList{"","AI","CD0","CD1","CD3","CD5","DS0","CA"}.contains(refresh.value("pendingKey").toString()) || !text(refresh.value("reason"),2048)) { return false; }
    for (const QString& key : {QString("total"),QString("completed"),QString("observed"),QString("failed")}) { if (!integer(refresh.value(key),0,7)) { return false; } }
    if (refresh.value("completed").toInt()!=refresh.value("observed").toInt()+refresh.value("failed").toInt() || refresh.value("completed").toInt()>refresh.value("total").toInt() || (refresh.value("busy").toBool() && (refresh.value("token").toString().isEmpty() || !status.value("connected").toBool()))) { return false; }
    const QVariantMap write=status.value("lastWrite").toMap(); if (!keys(write,{"token","id","state","reason"}) || !seq(write.value("token"),true,true) || !text(write.value("id")) || (!write.value("id").toString().isEmpty() && !kIds.contains(write.value("id").toString())) || !text(write.value("state")) || !QStringList{"none","pending","sent_unverified","observed_consistent","indeterminate","rejected"}.contains(write.value("state").toString()) || !text(write.value("reason"),2048)) { return false; }
    return validText(status.value("stream").toMap(),status.value("transcript").toMap());
}
quint64 nextRequest() { static quint64 value=0x4357525800000000ULL; return ++value; }
}

NativeCwReceiveWidget::NativeCwReceiveWidget(RadioModel& model,QWidget* parent)
    : QWidget(parent),m_model(model),m_availability(new ControlAvailabilityRegistry(model,this)),
      m_read(new QPushButton(tr("Read native CW controls"),this)),m_cancel(new QPushButton(tr("Cancel reads"),this)),m_clear(new QPushButton(tr("Clear local history"),this)),m_autoZero(new QLabel(this)),m_stream(new QLabel(this)),m_progress(new QLabel(this)),m_write(new QLabel(this)),m_status(new QLabel(this)),m_restrictions(new QLabel(this)),m_transcript(new QPlainTextEdit(this))
{
    setObjectName("nativeCwReceiveSettings"); auto* layout=new QVBoxLayout(this); auto* explanation=new QLabel(tr("These are the radio's CW receive decoder controls. Communication screen and decoder enable are separate. The transcript contains last received native CW characters, not a host audio decoder. Clear local history changes only this page; it sends no radio command."),this); explanation->setTextFormat(Qt::PlainText); explanation->setWordWrap(true); explanation->setSizePolicy(QSizePolicy::Preferred,QSizePolicy::Minimum); layout->addWidget(explanation);
    auto* grid=new QGridLayout; layout->addLayout(grid); int index=0;
    for (const QString& id : kIds) {
        Row row; row.observed=new QLabel(tr("Unknown"),this); row.observed->setObjectName("nativeCwReceive_"+id+"_Observed"); row.observed->setTextFormat(Qt::PlainText); row.observed->setWordWrap(true); row.observed->setSizePolicy(QSizePolicy::Preferred,QSizePolicy::Minimum);
        row.apply=new QPushButton(tr("Apply"),this); row.apply->setObjectName("nativeCwReceive_"+id+"_Apply"); row.apply->setAutoDefault(false);
        QWidget* editor=nullptr; if (id=="threshold") { row.number=new QSpinBox(this); row.number->setRange(1,30); editor=row.number; connect(row.number,&QSpinBox::valueChanged,this,[this,id] { editDraft(id); }); }
        else { row.choice=new QComboBox(this); editor=row.choice; connect(row.choice,&QComboBox::activated,this,[this,id] { editDraft(id); }); }
        editor->setObjectName("nativeCwReceive_"+id+"_Draft"); editor->setAccessibleName(tr("Native CW %1 draft").arg(id)); row.apply->setAccessibleName(tr("Apply native CW %1").arg(id));
        grid->addWidget(row.observed,index,0); grid->addWidget(editor,index,1); grid->addWidget(row.apply,index,2); ++index; m_rows.insert(id,row);
        connect(row.apply,&QPushButton::clicked,this,[this,id] { if (rowAvailable(id,true)) { const Row& current=m_rows[id]; submit("cw.receive.set",{{"context",current.context},{"id",id},{"revision",current.revision},{"value",current.draft}}); } });
        for (QWidget* target : {editor,static_cast<QWidget*>(row.apply)}) { m_availability->registerWidget(target,tr("Connect to a radio supporting native CW receive controls."),[this,id,target](bool connected,const RadioCapabilities& caps) { return connected && !nativeNamespace(caps).isEmpty() && rowAvailable(id,target==m_rows[id].apply); },{},false); }
    }
    auto* buttons=new QGridLayout; layout->addLayout(buttons); buttons->addWidget(m_read,0,0); buttons->addWidget(m_cancel,0,1); buttons->addWidget(m_clear,0,2);
    m_read->setObjectName("nativeCwReceiveRead"); m_cancel->setObjectName("nativeCwReceiveCancel"); m_clear->setObjectName("nativeCwReceiveClear");
    m_clear->setAutoDefault(false);
    for (QPushButton* button : {m_read,m_cancel}) { button->setAutoDefault(false); m_availability->registerWidget(button,tr("Connect to a radio supporting native CW receive controls."),[this,button](bool connected,const RadioCapabilities& caps) { if (!connected || nativeNamespace(caps).isEmpty()) { return false; } if (button==m_clear) { return !m_transcriptState.isEmpty(); } if (button==m_cancel) { return m_confirmed.value("refresh").toMap().value("busy").toBool(); } return m_requests.isEmpty() && !m_confirmed.value("refresh").toMap().value("busy").toBool(); },{},false); }
    const std::array<QLabel*,6> labels{m_autoZero,m_stream,m_progress,m_write,m_status,m_restrictions}; const std::array<QString,6> names{"nativeCwReceiveAutoZero","nativeCwReceiveStream","nativeCwReceiveProgress","nativeCwReceiveWriteStatus","nativeCwReceiveStatus","nativeCwReceiveRestrictions"};
    for (int labelIndex=0;labelIndex<6;++labelIndex) { labels[labelIndex]->setObjectName(names[labelIndex]); labels[labelIndex]->setTextFormat(Qt::PlainText); labels[labelIndex]->setWordWrap(true); labels[labelIndex]->setSizePolicy(QSizePolicy::Preferred,QSizePolicy::Minimum); layout->addWidget(labels[labelIndex]); }
    m_transcript->setObjectName("nativeCwReceiveTranscript"); m_transcript->setAccessibleName(tr("Last received native CW history")); m_transcript->setReadOnly(true); layout->addWidget(m_transcript,1);
    connect(m_read,&QPushButton::clicked,this,&NativeCwReceiveWidget::read);
    connect(m_cancel,&QPushButton::clicked,this,[this] { submit("cw.receive.cancel",{{"context",m_confirmed.value("context")},{"token",m_confirmed.value("refresh").toMap().value("token")}}); });
    connect(m_clear,&QPushButton::clicked,this,[this] { if (!m_transcriptState.isEmpty()) { submit("cw.receive.clear",{{"context",m_confirmed.value("context")},{"transcriptRevision",m_transcriptState.value("revision")}}); } });
    connect(&model,&RadioModel::connectionStateChanged,this,&NativeCwReceiveWidget::bindBackend); connect(&model,&RadioModel::capabilitiesChanged,this,&NativeCwReceiveWidget::bindBackend); bindBackend();
}
NativeCwReceiveWidget::~NativeCwReceiveWidget() { for (QObject* child : findChildren<QObject*>()) { QObject::disconnect(child,nullptr,this,nullptr); } QObject::disconnect(&m_model,nullptr,this,nullptr); QObject::disconnect(&m_model,nullptr,m_availability,nullptr); for (const QMetaObject::Connection& connection : m_connections) { QObject::disconnect(connection); } }
QVariantMap NativeCwReceiveWidget::control(const QString& id) const { for (const QVariant& raw : m_confirmed.value("controls").toList()) { if (raw.toMap().value("id")==id) { return raw.toMap(); } } return {}; }
bool NativeCwReceiveWidget::rowAvailable(const QString& id,bool apply) const {
    const QVariantMap descriptor=control(id); if (!m_requests.isEmpty() || !descriptor.value("editable").toBool() || descriptor.value("pending").toBool()) { return false; } if (!apply) { return true; }
    const Row& row=m_rows[id]; if (!row.seeded || row.retired || row.context!=m_confirmed.value("context").toString() || row.revision!=descriptor.value("revision").toString()) { return false; }
    if (id=="threshold") { return true; } for (const QVariant& raw : descriptor.value("choices").toList()) { if (raw.toMap().value("value").toInt()==row.draft) { return raw.toMap().value("editable").toBool(); } } return false;
}
void NativeCwReceiveWidget::editDraft(const QString& id) { if (!rowAvailable(id)) { return; } Row& row=m_rows[id]; const QVariantMap descriptor=control(id); row.seeded=row.dirty=true; row.retired=false; row.draft=row.number ? row.number->value() : row.choice->currentData().toInt(); row.baseline=descriptor.value("value").toInt(); row.context=m_confirmed.value("context").toString(); row.revision=descriptor.value("revision").toString(); refreshAvailability(); }
void NativeCwReceiveWidget::read() { if (m_confirmed.isEmpty()) { m_readAfterGet=true; submit("cw.receive.get"); } else { submit("cw.receive.read",{{"context",m_confirmed.value("context")}}); } }
void NativeCwReceiveWidget::bindBackend() {
    IRadioBackend* backend=m_model.backend(); const bool connected=m_model.isConnected(); const QString ns=backend ? nativeNamespace(m_model.backendCapabilities()) : QString{};
    if (backend==m_backend && ns==m_namespace && m_receiveSession==m_model.receiveControlSession() && connected==m_boundConnected) { render(); return; }
    const bool sameCache=backend && backend==m_backend && ns==m_namespace;
    const quint64 generation=++m_bindingGeneration; m_receiveSession=m_model.receiveControlSession(); m_boundConnected=connected; const quint64 receiveSession=m_receiveSession; for (const QMetaObject::Connection& connection : m_connections) { disconnect(connection); }
    m_backend=backend; m_namespace=ns; m_confirmed.clear(); m_requests.clear(); if (!sameCache) { m_streamState.clear(); m_transcriptState.clear(); m_statusRevision=m_textRevision=0; } ++m_statusSerial; ++m_textSerial; m_readAfterGet=false;
    for (Row& row : m_rows) { row.seeded=row.dirty=row.retired=false; row.context.clear(); row.revision.clear(); }
    const QPointer<NativeCwReceiveWidget> alive(this); m_status->clear(); if (!alive || generation!=m_bindingGeneration) { return; } render(); if (!alive || generation!=m_bindingGeneration) { return; } renderText(); if (!alive || generation!=m_bindingGeneration || !backend || ns.isEmpty()) { return; }
    const QPointer<IRadioBackend> source(backend); const auto current=[this,source,generation,receiveSession] { return source && generation==m_bindingGeneration && source==m_backend && source==m_model.backend() && receiveSession==m_model.receiveControlSession(); };
    m_connections[0]=connect(backend,&IRadioBackend::extensionStatus,this,[this,current,ns](const QString& space,const QString& kind,const QVariantMap& status) { if (!current() || space!=ns) { return; } if (kind=="cwReceive") { acceptStatus(status); } else if (kind=="cwReceiveText") { acceptText(status); } });
    m_connections[1]=connect(backend,&IRadioBackend::extensionResult,this,[this,current](quint64 request,const QVariant& result) {
        if (!current() || !m_requests.contains(request)) { return; } const QString verb=m_requests.take(request); const QPointer<NativeCwReceiveWidget> alive(this);
        if (verb!="cw.receive.set") { acceptStatus(map(result) ? result.toMap() : QVariantMap{}); if (!alive || !current()) { return; } if (verb=="cw.receive.get" && m_readAfterGet) { m_readAfterGet=false; if (m_model.isConnected() && !m_confirmed.isEmpty() && !m_confirmed.value("refresh").toMap().value("busy").toBool()) { submit("cw.receive.read",{{"context",m_confirmed.value("context")}}); } } return; }
        const QVariantMap reply=result.toMap(); const bool valid=map(result) && keys(reply,{"sent","verified","token"}) && flag(reply.value("sent")) && reply.value("sent").toBool() && flag(reply.value("verified")) && !reply.value("verified").toBool() && seq(reply.value("token"),false,true);
        m_status->setText(valid ? tr("Request sent; awaiting native radio readback.") : tr("Invalid handoff result; no confirmation inferred.")); if (alive && current()) { render(); }
    });
    m_connections[2]=connect(backend,&IRadioBackend::extensionError,this,[this,current](quint64 request,const QString& reason) { if (current() && m_requests.remove(request)) { m_readAfterGet=false; const QPointer<NativeCwReceiveWidget> alive(this); m_status->setText(reason); if (alive && current()) { render(); } } }); submit("cw.receive.get");
}
void NativeCwReceiveWidget::acceptStatus(const QVariantMap& status) {
    const bool older=seq(status.value("statusRevision")) && status.value("statusRevision").toString().toULongLong()<m_statusRevision;
    if (!validStatus(status)) { if (older) { return; } ++m_statusSerial; ++m_textSerial; m_confirmed.clear(); m_streamState.clear(); m_transcriptState.clear(); const QPointer<NativeCwReceiveWidget> alive(this); const quint64 generation=m_bindingGeneration; render(); if (alive && generation==m_bindingGeneration) { renderText(); } return; }
    const QPointer<NativeCwReceiveWidget> alive(this); const quint64 generation=m_bindingGeneration;
    if (!older) { ++m_statusSerial; m_statusRevision=status.value("statusRevision").toString().toULongLong(); m_confirmed=status; render(); }
    if (!alive || generation!=m_bindingGeneration) { return; }
    mergeText(status.value("stream").toMap(),status.value("transcript").toMap());
}
void NativeCwReceiveWidget::acceptText(const QVariantMap& status) {
    if (seq(status.value("statusRevision")) && status.value("statusRevision").toString().toULongLong()<m_textRevision) { return; }
    if (!keys(status,{"statusRevision","stream","transcript"}) || !seq(status.value("statusRevision")) || !map(status.value("stream")) || !map(status.value("transcript")) || !validText(status.value("stream").toMap(),status.value("transcript").toMap()) || status.value("statusRevision")!=status.value("stream").toMap().value("revision")) { ++m_textSerial; m_streamState.clear(); m_transcriptState.clear(); renderText(); return; }
    mergeText(status.value("stream").toMap(),status.value("transcript").toMap());
}
void NativeCwReceiveWidget::mergeText(const QVariantMap& stream,const QVariantMap& transcript) { const quint64 revision=stream.value("revision").toString().toULongLong(); if (revision<m_textRevision) { return; } m_textRevision=revision; ++m_textSerial; m_streamState=stream; m_transcriptState=transcript; renderText(); }
void NativeCwReceiveWidget::render() {
    const QPointer<NativeCwReceiveWidget> alive(this); const quint64 generation=m_bindingGeneration,serial=m_statusSerial; const auto current=[this,alive,generation,serial] { return alive && generation==m_bindingGeneration && serial==m_statusSerial; };
    for (const QString& id : kIds) {
        Row& row=m_rows[id]; const QVariantMap descriptor=control(id); const bool known=descriptor.value("known").toBool(); const int value=descriptor.value("value").toInt(); const QString context=m_confirmed.value("context").toString();
        if (known && (!row.seeded || !row.dirty)) { row.seeded=true; row.draft=row.baseline=value; row.context=context; row.revision=descriptor.value("revision").toString(); row.retired=false; }
        else if (known && !row.retired && row.context==context) { if (value==row.baseline || value==row.draft) { row.baseline=value; row.revision=descriptor.value("revision").toString(); } else { row.retired=true; } }
        if (row.choice) {
            const QPointer<QComboBox> editor(row.choice); QSignalBlocker blocker(editor); const auto safe=[&] { if (!editor) { blocker.dismiss(); } return editor && current(); }; editor->clear(); if (!safe()) { return; }
            for (const QVariant& raw : descriptor.value("choices").toList()) { const QVariantMap choice=raw.toMap(); editor->addItem(choice.value("label").toString(),choice.value("value")); if (!safe()) { return; } editor->setItemData(editor->count()-1,choice.value("reason"),Qt::ToolTipRole); if (!safe()) { return; } auto* items=qobject_cast<QStandardItemModel*>(editor->model()); if (items && items->item(editor->count()-1)) { items->item(editor->count()-1)->setEnabled(choice.value("editable").toBool()); if (!safe()) { return; } } }
            editor->setCurrentIndex(editor->findData(row.draft)); if (!safe()) { return; }
        } else { const QPointer<QSpinBox> editor(row.number); QSignalBlocker blocker(editor); editor->setValue(row.seeded ? row.draft : 1); if (!editor) { blocker.dismiss(); return; } if (!current()) { return; } }
        QString observed=tr("Unknown"); if (descriptor.contains("value")) { QString label=QString::number(value); for (const QVariant& raw : descriptor.value("choices").toList()) { if (raw.toMap().value("value").toInt()==value) { label=raw.toMap().value("label").toString(); } } observed=known ? label : tr("Unknown; last observed %1").arg(label); }
        const QString title=descriptor.value("label",id).toString(); row.observed->setText(tr("%1: %2").arg(title,observed)); if (!current()) { return; } QAccessibleValueChangeEvent event(row.observed,observed); QAccessible::updateAccessibility(&event); if (!current()) { return; }
    }
    const QVariantMap autoZero=m_confirmed.value("autoZero").toMap();
    QString activity=tr("Unknown");
    if (autoZero.contains("active")) { const QString observed=autoZero.value("active").toBool() ? tr("Active") : tr("Inactive"); activity=autoZero.value("known").toBool() ? observed : tr("Unknown; last observed %1").arg(observed); }
    const QString activityText=tr("Last reported CW auto zero-in: %1. Read native CW controls to refresh. Start and Stop are not implemented; inactivity does not confirm successful alignment.").arg(activity);
    if (m_autoZero->text()!=activityText) {
        m_autoZero->setText(activityText); if (!current()) { return; }
        QAccessibleValueChangeEvent activityEvent(m_autoZero, activity); QAccessible::updateAccessibility(&activityEvent); if (!current()) { return; }
    }
    m_restrictions->setText(m_confirmed.isEmpty() ? tr("Native CW restrictions: Unknown. Host audio DSP is independent.") : m_confirmed.value("restrictionNotice").toString()); if (!current()) { return; }
    const QVariantMap progress=m_confirmed.value("refresh").toMap(); m_progress->setText(m_confirmed.isEmpty() ? tr("Read progress: Unknown") : tr("Read progress: %1/%2; observed %3, failed %4. %5").arg(progress.value("completed").toInt()).arg(progress.value("total").toInt()).arg(progress.value("observed").toInt()).arg(progress.value("failed").toInt()).arg(progress.value("reason").toString())); if (!current()) { return; }
    const QVariantMap write=m_confirmed.value("lastWrite").toMap(); m_write->setText(tr("Last request: %1 %2 — %3").arg(write.value("id").toString(),write.value("state",tr("Unknown")).toString(),write.value("reason").toString())); if (current()) { refreshAvailability(); }
}
void NativeCwReceiveWidget::renderText() {
    const QPointer<NativeCwReceiveWidget> alive(this); const quint64 generation=m_bindingGeneration,serial=m_textSerial; const auto current=[this,alive,generation,serial] { return alive && generation==m_bindingGeneration && serial==m_textSerial; };
    QString history; for (const QVariant& raw : m_transcriptState.value("events").toList()) { history+=raw.toMap().value("text").toString(); }
    m_transcript->setPlainText(history); if (!current()) { return; }
    QString readiness=m_streamState.isEmpty() ? tr("Unknown; read native CW controls.") : m_streamState.value("available").toBool() ? tr("Native receive prerequisites available.") : tr("Unavailable: %1").arg(m_streamState.value("reason").toString());
    if (!m_streamState.isEmpty() && !m_streamState.value("aiKnown").toBool()) { readiness+=tr(" AI state is unknown; received native events do not confirm AI On."); }
    m_stream->setText(tr("Last received native CW — %1 Dropped history events: %2; invalid events: %3.").arg(readiness).arg(m_transcriptState.value("dropped").toInt()).arg(m_transcriptState.value("invalid").toInt())); if (!current()) { return; }
    QAccessibleValueChangeEvent event(m_stream,m_stream->text()); QAccessible::updateAccessibility(&event); if (current()) { refreshAvailability(); }
}
void NativeCwReceiveWidget::refreshAvailability() {
    const QPointer<NativeCwReceiveWidget> alive(this); const quint64 generation=m_bindingGeneration,serial=m_statusSerial,textSerial=m_textSerial; const auto current=[this,alive,generation,serial,textSerial] { return alive && generation==m_bindingGeneration && serial==m_statusSerial && textSerial==m_textSerial; }; m_availability->refreshEngaged(); if (!current()) { return; }
    // Local history belongs to the retained backend cache, including offline.
    const bool canClear=m_backend && m_backend==m_model.backend() && !m_namespace.isEmpty() && !m_confirmed.isEmpty() && !m_transcriptState.isEmpty();
    m_clear->setEnabled(canClear); if (!current()) { return; }
    if (canClear) { m_clear->setToolTip(tr("Clear this local native CW history; no radio command is sent.")); if (!current()) { return; } m_clear->setAccessibleDescription(tr("Clear this local native CW history; available without a radio connection.")); if (!current()) { return; } }
    QList<QWidget*> controls{m_read,m_cancel,m_clear}; for (const Row& row : m_rows) { controls.append(row.apply); controls.append(row.choice ? static_cast<QWidget*>(row.choice) : static_cast<QWidget*>(row.number)); }
    for (QWidget* widget : controls) {
        if (widget->isEnabled()) { continue; } QString reason;
        if (!m_model.isConnected()) { reason=tr("Connect to a radio supporting native CW receive controls."); }
        else if (nativeNamespace(m_model.backendCapabilities()).isEmpty()) { reason=tr("This radio does not support native CW receive controls."); }
        else if (m_confirmed.isEmpty()) { reason=tr("Read native CW controls to obtain a valid radio description."); }
        else {
            for (auto it=m_rows.cbegin();it!=m_rows.cend();++it) { const Row& row=it.value(); if (widget==row.apply || widget==row.choice || widget==row.number) { const QVariantMap descriptor=control(it.key()); reason=descriptor.value("reason").toString(); if (reason.isEmpty() && widget==row.apply) { for (const QVariant& raw : descriptor.value("choices").toList()) { if (raw.toMap().value("value").toInt()==row.draft && !raw.toMap().value("editable").toBool()) { reason=raw.toMap().value("reason").toString(); } } } if (reason.isEmpty()) { reason=tr("Edit this draft again to bind it to the current native observation."); } } }
        }
        if (reason.isEmpty()) { if (widget==m_cancel) { reason=tr("No native read is running."); } else if (widget==m_clear) { reason=tr("Read native CW controls to obtain the current local history revision."); } else { reason=tr("Waiting for the current native request to finish."); } }
        const QPointer<QWidget> target(widget); target->setToolTip(reason); if (!target || !current()) { return; } target->setAccessibleDescription(reason); if (!current()) { return; }
    }
}
void NativeCwReceiveWidget::submit(const QString& verb,const QVariantMap& arguments) {
    const bool local=verb=="cw.receive.clear" || verb=="cw.receive.get";
    if (!m_backend || m_backend!=m_model.backend() || m_namespace.isEmpty() || (!local && !m_model.isConnected())) { return; }
    const bool independent=verb=="cw.receive.clear" || verb=="cw.receive.cancel"; if (!independent && !m_requests.isEmpty()) { return; }
    const RadioControlSession session=m_model.controlSession(); const quint64 receiveSession=m_receiveSession;
    const quint64 request=nextRequest(),generation=m_bindingGeneration; m_requests.insert(request,verb); const QPointer<NativeCwReceiveWidget> alive(this); render();
    if (!alive || generation!=m_bindingGeneration || !m_requests.contains(request) || m_backend!=m_model.backend() || receiveSession!=m_model.receiveControlSession() || (!local && !m_model.controlSessionCurrent(session))) { return; }
    m_model.invokeBackendExtension(m_namespace,verb,request,arguments);
}
} // namespace AetherSDR
