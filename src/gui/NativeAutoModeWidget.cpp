#include "NativeAutoModeWidget.h"
#include "ControlAvailabilityRegistry.h"
#include "models/RadioModel.h"
#include <QAbstractItemView>
#include <QAccessible>
#include <QAccessibleValueChangeEvent>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QHeaderView>
#include <QLabel>
#include <QList>
#include <QMetaType>
#include <QPushButton>
#include <QSet>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QStringList>
#include <QTableWidget>
#include <QVBoxLayout>
#include <cmath>

namespace AetherSDR {
namespace {
const QStringList kVerbs{"auto.mode.get","auto.mode.read","auto.mode.cancel","auto.mode.enabled.set","auto.mode.division.register","auto.mode.division.delete"};
bool text(const QVariant& value,int maximum,bool empty=true) { return value.typeId()==QMetaType::QString && value.toString().size()<=maximum && (empty || !value.toString().isEmpty()); }
bool sequence(const QVariant& value,bool empty=false) { if (!text(value,20,empty)) { return false; } if (empty && value.toString().isEmpty()) { return true; } bool ok=false; const quint64 number=value.toString().toULongLong(&ok); return ok && QString::number(number)==value.toString(); }
bool token(const QVariant& value,bool empty=true) { return sequence(value,empty) && (value.toString().isEmpty() || value.toString()!=QLatin1String("0")); }
bool integer(const QVariant& value,int minimum,int maximum) { return value.typeId()==QMetaType::Int && value.toInt()>=minimum && value.toInt()<=maximum; }
bool hz(const QVariant& value) { return value.typeId()==QMetaType::LongLong && value.toLongLong()>=0 && value.toLongLong()<=99999999999LL; }
bool hex(const QVariant& value) { if (!text(value,128) || value.toString().size()%2) { return false; } for (QChar c : value.toString()) { if (!QStringLiteral("0123456789abcdef").contains(c)) { return false; } } return true; }
QString autoNamespace(const RadioCapabilities& caps) { for (const QString& ns : caps.extensionNamespaces) { const QVariantMap entry=caps.extensions.value(ns).toMap(); if (entry.value("nativeAutoModeVersion").typeId()!=QMetaType::Int || entry.value("nativeAutoModeVersion").toInt()!=1 || entry.value("nativeAutoModeVerbs").typeId()!=QMetaType::QStringList) { continue; } bool complete=true; for (const QString& verb : kVerbs) { complete=complete && entry.value("nativeAutoModeVerbs").toStringList().contains(verb); } if (complete) { return ns; } } return {}; }
bool validStatus(const QVariantMap& status) {
    const QSet<QString> required{"statusRevision","context","connected","reason","bankKnown","enabledKnown","enabledRevision","countKnown","rows","modeChoices","hzDomain","snapshot","refresh","operations","lastWrite"};
    if (status.size()<required.size() || status.size()>required.size()+3) { return false; }
    for (const QString& key : required) { if (!status.contains(key)) { return false; } }
    for (auto it=status.cbegin();it!=status.cend();++it) { if (!required.contains(it.key()) && it.key()!=QLatin1String("bank") && it.key()!=QLatin1String("enabled") && it.key()!=QLatin1String("count")) { return false; } }
    if (!sequence(status.value("statusRevision")) || !sequence(status.value("enabledRevision")) || !text(status.value("context"),512,false) || !text(status.value("reason"),2048)) { return false; }
    for (const QString& flag : {QString("connected"),QString("bankKnown"),QString("enabledKnown"),QString("countKnown")}) { if (status.value(flag).typeId()!=QMetaType::Bool) { return false; } }
    if ((status.contains("bank") && !integer(status.value("bank"),0,1)) || (status.contains("enabled") && status.value("enabled").typeId()!=QMetaType::Bool) || (status.contains("count") && !integer(status.value("count"),0,32))) { return false; }
    for (const QString& key : {QString("bank"),QString("enabled"),QString("count")}) { if (status.value(key+"Known").toBool() && !status.contains(key)) { return false; } }
    if (status.value("modeChoices").typeId()!=QMetaType::QVariantList || status.value("modeChoices").toList().size()!=14) { return false; } QSet<QString> modes;
    for (const QVariant& raw : status.value("modeChoices").toList()) { const QVariantMap choice=raw.toMap(); const QString code=choice.value("value").toString(); if (raw.typeId()!=QMetaType::QVariantMap || choice.size()!=2 || !text(choice.value("value"),1,false) || code==QLatin1String("0") || modes.contains(code) || !text(choice.value("label"),32,false)) { return false; } modes.insert(code); }
    if (status.value("hzDomain").typeId()!=QMetaType::QVariantMap) { return false; } const QVariantMap domain=status.value("hzDomain").toMap(); if (domain.size()!=4 || !hz(domain.value("minimum")) || domain.value("minimum").toLongLong()!=0 || !hz(domain.value("maximum")) || domain.value("maximum").toLongLong()!=99999999999LL || !hz(domain.value("step")) || domain.value("step").toLongLong()!=1 || domain.value("unit").typeId()!=QMetaType::QString || domain.value("unit")!=QLatin1String("Hz")) { return false; }
    if (status.value("rows").typeId()!=QMetaType::QVariantList || status.value("rows").toList().size()!=32) { return false; } int populated=0; bool allValid=true;
    const QSet<QString> states{"unread","empty","populated","invalid","timeout"}; const QVariantList rows=status.value("rows").toList();
    for (int index=0;index<32;++index) { const QVariantMap row=rows[index].toMap(); const QString state=row.value("state").toString(); if (rows[index].typeId()!=QMetaType::QVariantMap || (row.size()!=5 && row.size()!=8) || !integer(row.value("index"),index,index) || !text(row.value("state"),32,false) || !states.contains(state) || !sequence(row.value("revision")) || !text(row.value("reason"),2048) || !hex(row.value("rawUtf8Hex"))) { return false; }
        if (row.size()==8 && (!hz(row.value("upperHz")) || !text(row.value("mode"),1,false) || !text(row.value("modeLabel"),32,false) || (!modes.contains(row.value("mode").toString()) && row.value("mode")!=QLatin1String("0")))) { return false; }
        if (state==QLatin1String("populated")) { if (row.size()!=8 || !modes.contains(row.value("mode").toString())) { return false; } ++populated; }
        else if (state==QLatin1String("empty")) { if (row.size()!=8 || row.value("upperHz").toLongLong()!=0 || row.value("mode")!=QLatin1String("0")) { return false; } }
        else { allValid=false; if (state==QLatin1String("unread") && row.size()!=5) { return false; } }
    }
    for (const QString& key : {QString("snapshot"),QString("refresh"),QString("operations"),QString("lastWrite")}) { if (status.value(key).typeId()!=QMetaType::QVariantMap) { return false; } }
    const QVariantMap snapshot=status.value("snapshot").toMap(); if (snapshot.size()!=4 || !token(snapshot.value("token")) || snapshot.value("complete").typeId()!=QMetaType::Bool || snapshot.value("coherent").typeId()!=QMetaType::Bool || !text(snapshot.value("reason"),2048)) { return false; }
    if (snapshot.value("complete").toBool() && (!status.value("connected").toBool() || !status.value("bankKnown").toBool() || !status.value("enabledKnown").toBool() || !status.value("countKnown").toBool() || !allValid)) { return false; }
    if (snapshot.value("coherent").toBool() && (!snapshot.value("complete").toBool() || snapshot.value("token").toString().isEmpty() || populated!=status.value("count").toInt())) { return false; }
    const QVariantMap refresh=status.value("refresh").toMap(); if (refresh.size()!=8 || !token(refresh.value("token")) || refresh.value("busy").typeId()!=QMetaType::Bool || !text(refresh.value("pendingKey"),16) || !text(refresh.value("reason"),2048)) { return false; }
    for (const QString& key : {QString("total"),QString("completed"),QString("observed"),QString("failed")}) { if (!integer(refresh.value(key),0,35)) { return false; } }
    if (refresh.value("completed").toInt()!=refresh.value("observed").toInt()+refresh.value("failed").toInt() || refresh.value("completed").toInt()>refresh.value("total").toInt() || (refresh.value("busy").toBool() && (refresh.value("token").toString().isEmpty() || !status.value("connected").toBool()))) { return false; }
    const QVariantMap operations=status.value("operations").toMap(); if (operations.size()!=3) { return false; }
    for (const QString& key : {QString("enabled"),QString("register"),QString("delete")}) { const QVariantMap operation=operations.value(key).toMap(); if (operations.value(key).typeId()!=QMetaType::QVariantMap || operation.size()!=3 || operation.value("editable").typeId()!=QMetaType::Bool || operation.value("pending").typeId()!=QMetaType::Bool || !text(operation.value("reason"),2048)) { return false; }
        if (operation.value("editable").toBool() && (operation.value("pending").toBool() || !status.value("connected").toBool() || !status.value("bankKnown").toBool() || !status.value("enabledKnown").toBool() || (key!=QLatin1String("enabled") && (!snapshot.value("coherent").toBool() || !status.value("countKnown").toBool())) || (key==QLatin1String("register") && status.value("count").toInt()>=32) || (key==QLatin1String("delete") && status.value("count").toInt()==0))) { return false; }
    }
    bool pending=false; for (const QVariant& value : operations) { pending=pending || value.toMap().value("pending").toBool(); } if (pending) { for (const QVariant& value : operations) { if (value.toMap().value("editable").toBool()) { return false; } } }
    const QVariantMap write=status.value("lastWrite").toMap(); return write.size()==4 && token(write.value("token")) && text(write.value("operation"),16) && QStringList{"","enabled","register","delete"}.contains(write.value("operation").toString()) && text(write.value("state"),32,false) && QStringList{"none","pending","sent_unverified","observed_consistent","indeterminate","rejected"}.contains(write.value("state").toString()) && text(write.value("reason"),2048);
}
quint64 nextRequest() { static quint64 request=0x5258414d00000000ULL; return ++request; }
}

NativeAutoModeWidget::NativeAutoModeWidget(RadioModel& model,QWidget* parent)
    : QWidget(parent),m_model(model),m_availability(new ControlAvailabilityRegistry(model,this)),
      m_enabled(new QCheckBox(tr("Auto mode enabled draft"),this)),m_mode(new QComboBox(this)),m_frequency(new QDoubleSpinBox(this)),m_rows(new QTableWidget(32,5,this)),
      m_observed(new QLabel(this)),m_status(new QLabel(this)),m_progress(new QLabel(this)),m_writeStatus(new QLabel(this)),
      m_read(new QPushButton(tr("Read auto-mode settings"),this)),m_cancel(new QPushButton(tr("Cancel reads"),this)),m_enabledApply(new QPushButton(tr("Apply auto-mode enabled"),this)),m_register(new QPushButton(tr("Register new division"),this)),m_delete(new QPushButton(tr("Delete selected division"),this))
{
    setObjectName("nativeAutoModeSettings"); auto* layout=new QVBoxLayout(this);
    auto* explanation=new QLabel(tr("Auto mode is stored radio policy shared by its operating contexts. Table labels describe stored policy, not the current effective sideband. Registration and deletion are persistent requests; only subsequent observed reads can show a consistent result, not prove a transaction. Cancel stops reads and cannot undo a sent change."),this); explanation->setWordWrap(true); layout->addWidget(explanation);
    auto* grid=new QGridLayout; layout->addLayout(grid); m_enabled->setObjectName("nativeAutoModeEnabledDraft"); m_enabled->setTristate(true); m_enabled->setCheckState(Qt::PartiallyChecked); m_enabledApply->setObjectName("nativeAutoModeEnabledApply"); grid->addWidget(m_enabled,0,0); grid->addWidget(m_enabledApply,0,1);
    m_frequency->setObjectName("nativeAutoModeFrequencyDraft"); m_frequency->setAccessibleName(tr("New upper frequency boundary draft in Hz")); m_frequency->setDecimals(0); m_frequency->setRange(0,0); m_frequency->setSingleStep(1); m_frequency->setSuffix(tr(" Hz")); m_mode->setObjectName("nativeAutoModeModeDraft"); m_mode->setAccessibleName(tr("New division stored mode policy")); m_register->setObjectName("nativeAutoModeRegister"); grid->addWidget(m_frequency,1,0); grid->addWidget(m_mode,1,1); grid->addWidget(m_register,1,2);
    m_read->setObjectName("nativeAutoModeRead"); m_cancel->setObjectName("nativeAutoModeCancel"); m_delete->setObjectName("nativeAutoModeDelete"); grid->addWidget(m_read,2,0); grid->addWidget(m_cancel,2,1); grid->addWidget(m_delete,2,2);
    for (QPushButton* button : {m_read,m_cancel,m_enabledApply,m_register,m_delete}) { button->setAutoDefault(false); }
    m_rows->setObjectName("nativeAutoModeRows"); m_rows->setAccessibleName(tr("Observed auto-mode frequency divisions")); m_rows->setHorizontalHeaderLabels({tr("Index"),tr("State"),tr("Upper boundary (Hz)"),tr("Stored mode policy"),tr("Reason")}); m_rows->setEditTriggers(QAbstractItemView::NoEditTriggers); m_rows->setSelectionBehavior(QAbstractItemView::SelectRows); m_rows->setSelectionMode(QAbstractItemView::SingleSelection); m_rows->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents); m_rows->horizontalHeader()->setStretchLastSection(true);
    for (int row=0;row<32;++row) { for (int col=0;col<5;++col) { m_rows->setItem(row,col,new QTableWidgetItem(col==0 ? QString::number(row) : QString{})); } } layout->addWidget(m_rows);
    m_observed->setObjectName("nativeAutoModeObserved"); m_status->setObjectName("nativeAutoModeStatus"); m_progress->setObjectName("nativeAutoModeProgress"); m_writeStatus->setObjectName("nativeAutoModeWriteStatus"); for (QLabel* label : {m_observed,m_status,m_progress,m_writeStatus}) { label->setTextFormat(Qt::PlainText); label->setWordWrap(true); label->setSizePolicy(QSizePolicy::Preferred,QSizePolicy::Minimum); layout->addWidget(label); }
    const auto gate=[this](QWidget* widget,const QString& operation) { m_availability->registerWidget(widget,tr("Requires a current radio-authoritative auto-mode context and eligible observed state."),[this,widget,operation](bool connected,const RadioCapabilities& caps) { if (!connected || autoNamespace(caps).isEmpty()) { return false; } if (widget==m_read) { return m_requests.isEmpty() && !m_confirmed.value("refresh").toMap().value("busy").toBool(); } if (widget==m_cancel) { return m_requests.isEmpty() && m_confirmed.value("refresh").toMap().value("busy").toBool(); } if (widget==m_enabledApply) { return enabledAvailable(); } if (widget==m_register) { return registerAvailable(); } if (widget==m_delete) { return deleteAvailable(); } return available(operation); },[this,widget] { return widget==m_enabled && m_confirmed.value("enabledKnown").toBool() && m_confirmed.value("enabled").toBool(); },false); };
    gate(m_read,{}); gate(m_cancel,{}); gate(m_enabled,"enabled"); gate(m_enabledApply,"enabled"); gate(m_frequency,"register"); gate(m_mode,"register"); gate(m_register,"register"); gate(m_delete,"delete");
    connect(m_enabled,&QCheckBox::clicked,this,[this] { editEnabled(); }); connect(m_frequency,&QDoubleSpinBox::valueChanged,this,[this] { editDivision(); }); connect(m_mode,&QComboBox::activated,this,[this] { editDivision(); }); connect(m_rows,&QTableWidget::cellClicked,this,[this](int index,int) { selectDivision(index); }); connect(m_rows,&QTableWidget::itemSelectionChanged,this,[this] { const QList<QTableWidgetItem*> selection=m_rows->selectedItems(); selectDivision(selection.isEmpty() ? -1 : selection.first()->row()); });
    connect(m_read,&QPushButton::clicked,this,[this] { submit(m_confirmed.isEmpty() ? "auto.mode.get" : "auto.mode.read",m_confirmed.isEmpty() ? QVariantMap{} : QVariantMap{{"context",m_confirmed.value("context")}}); });
    connect(m_cancel,&QPushButton::clicked,this,[this] { submit("auto.mode.cancel",{{"context",m_confirmed.value("context")},{"token",m_confirmed.value("refresh").toMap().value("token")}}); });
    connect(m_enabledApply,&QPushButton::clicked,this,[this] { if (enabledAvailable()) { submit("auto.mode.enabled.set",{{"context",m_enabledContext},{"enabledRevision",m_enabledRevision},{"enabled",m_enabled->checkState()==Qt::Checked}}); } });
    connect(m_register,&QPushButton::clicked,this,[this] { if (registerAvailable()) { submit("auto.mode.division.register",{{"context",m_divisionContext},{"snapshotToken",m_divisionSnapshot},{"upperHz",QVariant::fromValue<qint64>(static_cast<qint64>(m_frequency->value()))},{"mode",m_mode->currentData()}}); } });
    connect(m_delete,&QPushButton::clicked,this,[this] { if (deleteAvailable()) { submit("auto.mode.division.delete",{{"context",m_deleteContext},{"snapshotToken",m_deleteSnapshot},{"index",m_selectedIndex},{"rowRevision",m_deleteRevision}}); } });
    connect(&model,&RadioModel::connectionStateChanged,this,&NativeAutoModeWidget::bindBackend); connect(&model,&RadioModel::capabilitiesChanged,this,&NativeAutoModeWidget::bindBackend); bindBackend();
}
NativeAutoModeWidget::~NativeAutoModeWidget() { for (QObject* child : findChildren<QObject*>()) { QObject::disconnect(child,nullptr,this,nullptr); } QObject::disconnect(&m_model,nullptr,this,nullptr); QObject::disconnect(&m_model,nullptr,m_availability,nullptr); for (const QMetaObject::Connection& connection : m_connections) { QObject::disconnect(connection); } }
bool NativeAutoModeWidget::available(const QString& operation) const { const QVariantMap state=m_confirmed.value("operations").toMap().value(operation).toMap(); return m_requests.isEmpty() && state.value("editable").toBool() && !state.value("pending").toBool(); }
bool NativeAutoModeWidget::enabledAvailable() const { return available("enabled") && m_enabledSeeded && !m_enabledRetired && m_enabled->checkState()!=Qt::PartiallyChecked && m_enabledContext==m_confirmed.value("context").toString() && m_enabledRevision==m_confirmed.value("enabledRevision").toString(); }
bool NativeAutoModeWidget::registerAvailable() const { if (!available("register") || m_mode->currentIndex()<0 || m_divisionContext!=m_confirmed.value("context").toString() || m_divisionSnapshot!=m_confirmed.value("snapshot").toMap().value("token").toString() || !std::isfinite(m_frequency->value()) || m_frequency->value()!=std::floor(m_frequency->value())) { return false; } for (const QVariant& raw : m_confirmed.value("rows").toList()) { const QVariantMap row=raw.toMap(); if (row.value("state")==QLatin1String("populated") && row.value("upperHz").toLongLong()==static_cast<qint64>(m_frequency->value())) { return false; } } return true; }
QVariantMap NativeAutoModeWidget::selectedRow() const { const QVariantList rows=m_confirmed.value("rows").toList(); return m_selectedIndex>=0 && m_selectedIndex<rows.size() ? rows[m_selectedIndex].toMap() : QVariantMap{}; }
bool NativeAutoModeWidget::deleteAvailable() const { bool selected=false; for (const QTableWidgetItem* item : m_rows->selectedItems()) { selected=selected || item->row()==m_selectedIndex; } const QVariantMap row=selectedRow(); return selected && available("delete") && row.value("state")==QLatin1String("populated") && m_deleteContext==m_confirmed.value("context").toString() && m_deleteSnapshot==m_confirmed.value("snapshot").toMap().value("token").toString() && m_deleteRevision==row.value("revision").toString(); }
void NativeAutoModeWidget::editEnabled() { if (!available("enabled")) { return; } m_enabledSeeded=true; m_enabledDirty=true; m_enabledRetired=false; m_enabledBaseline=m_confirmed.value("enabled").toBool(); m_enabledContext=m_confirmed.value("context").toString(); m_enabledRevision=m_confirmed.value("enabledRevision").toString(); refreshAvailability(); }
void NativeAutoModeWidget::editDivision() { if (!available("register")) { return; } if (m_mode->currentIndex()>=0) { m_modeDraft=m_mode->currentData().toString(); } m_divisionContext=m_confirmed.value("context").toString(); m_divisionSnapshot=m_confirmed.value("snapshot").toMap().value("token").toString(); refreshAvailability(); }
void NativeAutoModeWidget::selectDivision(int index) { bool selected=false; for (const QTableWidgetItem* item : m_rows->selectedItems()) { selected=selected || item->row()==index; } m_selectedIndex=selected ? index : -1; m_deleteContext=m_confirmed.value("context").toString(); m_deleteSnapshot=m_confirmed.value("snapshot").toMap().value("token").toString(); m_deleteRevision=selectedRow().value("revision").toString(); refreshAvailability(); }
void NativeAutoModeWidget::bindBackend() {
    IRadioBackend* backend=m_model.isConnected() ? m_model.backend() : nullptr; const QString ns=backend ? autoNamespace(m_model.backendCapabilities()) : QString{}; if (backend==m_backend && ns==m_namespace && m_receiveSession==m_model.receiveControlSession()) { render(); return; }
    const quint64 generation=++m_bindingGeneration; m_receiveSession=m_model.receiveControlSession(); const RadioControlSession session=m_model.controlSession(); for (const QMetaObject::Connection& connection : m_connections) { disconnect(connection); }
    m_backend=backend; m_namespace=ns; m_confirmed.clear(); m_requests.clear(); m_statusRevision=0; ++m_statusSerial; m_enabledSeeded=m_enabledDirty=m_enabledRetired=false; m_enabledContext.clear(); m_divisionContext.clear(); m_deleteContext.clear(); m_divisionSnapshot.clear(); m_deleteSnapshot.clear(); m_modeDraft.clear(); m_selectedIndex=-1;
    const QPointer<NativeAutoModeWidget> alive(this); m_status->clear(); if (!alive || generation!=m_bindingGeneration) { return; } render(); if (!alive || generation!=m_bindingGeneration || !backend || ns.isEmpty()) { return; }
    const QPointer<IRadioBackend> source(backend); const auto current=[this,source,generation,session] { return source && generation==m_bindingGeneration && source==m_backend && source==m_model.backend() && m_model.isConnected() && m_model.controlSessionCurrent(session); };
    m_connections[0]=connect(backend,&IRadioBackend::extensionStatus,this,[this,current,ns](const QString& space,const QString& kind,const QVariantMap& status) { if (current() && space==ns && kind=="autoMode") { acceptStatus(status); } });
    m_connections[1]=connect(backend,&IRadioBackend::extensionResult,this,[this,current](quint64 request,const QVariant& result) { if (!current() || !m_requests.contains(request)) { return; } const QString verb=m_requests.take(request); const QPointer<NativeAutoModeWidget> alive(this);
        if (verb=="auto.mode.get" || verb=="auto.mode.read" || verb=="auto.mode.cancel") { if (verb!="auto.mode.get" || m_getSerial==m_statusSerial) { acceptStatus(result.toMap()); } else { render(); } return; }
        const QVariantMap reply=result.toMap(); const bool valid=result.typeId()==QMetaType::QVariantMap && reply.size()==3 && reply.value("sent").typeId()==QMetaType::Bool && reply.value("sent").toBool() && reply.value("verified").typeId()==QMetaType::Bool && !reply.value("verified").toBool() && token(reply.value("token"),false);
        m_status->setText(valid ? tr("Request sent; the radio outcome remains unverified.") : tr("Invalid handoff result; no confirmation inferred.")); if (alive) { render(); }
    });
    m_connections[2]=connect(backend,&IRadioBackend::extensionError,this,[this,current](quint64 request,const QString& error) { if (current() && m_requests.remove(request)) { const QPointer<NativeAutoModeWidget> alive(this); m_status->setText(error); if (alive) { render(); } } }); submit("auto.mode.get");
}
void NativeAutoModeWidget::acceptStatus(const QVariantMap& status) { if (sequence(status.value("statusRevision")) && status.value("statusRevision").toString().toULongLong()<m_statusRevision) { return; } ++m_statusSerial; if (!validStatus(status)) { m_confirmed.clear(); } else { m_statusRevision=status.value("statusRevision").toString().toULongLong(); m_confirmed=status; } render(); }
void NativeAutoModeWidget::render() {
    const QPointer<NativeAutoModeWidget> alive(this); const quint64 generation=m_bindingGeneration,serial=m_statusSerial; const auto current=[this,alive,generation,serial] { return alive && generation==m_bindingGeneration && serial==m_statusSerial; };
    const bool known=m_confirmed.value("enabledKnown").toBool(); const bool observed=m_confirmed.value("enabled").toBool();
    { const QPointer<QCheckBox> editor(m_enabled); QSignalBlocker blocker(editor); if (!m_enabledSeeded && known) { editor->setTristate(false); if (!editor) { blocker.dismiss(); return; } if (!current()) { return; } m_enabledSeeded=true; m_enabledBaseline=observed; m_enabledContext=m_confirmed.value("context").toString(); m_enabledRevision=m_confirmed.value("enabledRevision").toString(); editor->setCheckState(observed ? Qt::Checked : Qt::Unchecked); } else if (known && !m_enabledDirty) { m_enabledBaseline=observed; m_enabledContext=m_confirmed.value("context").toString(); m_enabledRevision=m_confirmed.value("enabledRevision").toString(); editor->setTristate(false); if (!editor) { blocker.dismiss(); return; } if (!current()) { return; } editor->setCheckState(observed ? Qt::Checked : Qt::Unchecked); } else if (!m_enabledSeeded) { editor->setTristate(true); if (!editor) { blocker.dismiss(); return; } if (!current()) { return; } editor->setCheckState(Qt::PartiallyChecked); } if (!editor) { blocker.dismiss(); return; } if (!current()) { return; } }
    if (known && m_enabledSeeded && !m_enabledRetired && m_enabledContext==m_confirmed.value("context").toString()) { if (!m_enabledDirty || observed==m_enabledBaseline || observed==(m_enabled->checkState()==Qt::Checked)) { m_enabledBaseline=observed; m_enabledRevision=m_confirmed.value("enabledRevision").toString(); } else { m_enabledRetired=true; } }
    if (!m_confirmed.isEmpty()) {
        const QVariantMap domain=m_confirmed.value("hzDomain").toMap(); const QPointer<QDoubleSpinBox> editor(m_frequency); QSignalBlocker blocker(editor);
        editor->setRange(static_cast<double>(domain.value("minimum").toLongLong()),static_cast<double>(domain.value("maximum").toLongLong())); if (!editor) { blocker.dismiss(); return; } if (!current()) { return; }
        editor->setSingleStep(static_cast<double>(domain.value("step").toLongLong())); if (!editor) { blocker.dismiss(); return; } if (!current()) { return; }
    }
    { const QPointer<QComboBox> editor(m_mode); QSignalBlocker blocker(editor); const QVariant draft=m_modeDraft; const auto safe=[&] { if (!editor) { blocker.dismiss(); } return editor && current(); }; editor->clear(); if (!safe()) { return; } for (const QVariant& raw : m_confirmed.value("modeChoices").toList()) { const QVariantMap choice=raw.toMap(); editor->addItem(choice.value("label").toString(),choice.value("value")); if (!safe()) { return; } } editor->setCurrentIndex(editor->findData(draft)); if (!safe()) { return; } }
    { const QPointer<QTableWidget> table(m_rows); QSignalBlocker blocker(table); const QVariantList rows=m_confirmed.value("rows").toList(); for (int index=0;index<32;++index) { const QVariantMap row=index<rows.size() ? rows[index].toMap() : QVariantMap{}; const QStringList values{QString::number(index),row.value("state",tr("Unknown")).toString(),row.contains("upperHz") ? QString::number(row.value("upperHz").toLongLong()) : tr("Unknown"),row.value("modeLabel",tr("Unknown")).toString(),row.value("reason").toString()}; for (int col=0;col<5;++col) { table->item(index,col)->setText(values[col]); if (!table) { blocker.dismiss(); return; } if (!current()) { return; } } } }
    const QVariantMap snapshot=m_confirmed.value("snapshot").toMap();
    const QString snapshotState=m_confirmed.isEmpty() ? tr("Unknown") : !snapshot.value("complete").toBool() ? tr("Incomplete") : !snapshot.value("coherent").toBool() ? tr("Inconsistent observations") : tr("Complete observed (not atomic)");
    m_observed->setText(tr("Confirmed auto mode: %1 | Divisions: %2 | Snapshot: %3").arg(known ? observed ? tr("On") : tr("Off") : tr("Unknown"),m_confirmed.value("countKnown").toBool() ? QString::number(m_confirmed.value("count").toInt()) : tr("Unknown"),snapshotState)); if (!current()) { return; }
    QAccessibleValueChangeEvent event(m_observed,m_observed->text()); QAccessible::updateAccessibility(&event); if (!current()) { return; }
    const QVariantMap refresh=m_confirmed.value("refresh").toMap(); m_progress->setText(tr("Read progress: %1/%2; observed %3, failed %4. %5").arg(refresh.value("completed").toInt()).arg(refresh.value("total").toInt()).arg(refresh.value("observed").toInt()).arg(refresh.value("failed").toInt()).arg(refresh.value("reason").toString())); if (!current()) { return; }
    const QVariantMap write=m_confirmed.value("lastWrite").toMap(); m_writeStatus->setText(tr("Last request: %1 — %2").arg(write.value("state",tr("Unknown")).toString(),write.value("reason").toString())); if (current()) { refreshAvailability(); }
}
void NativeAutoModeWidget::refreshAvailability()
{
    const QPointer<NativeAutoModeWidget> alive(this); const quint64 generation=m_bindingGeneration,serial=m_statusSerial;
    const auto current=[this,alive,generation,serial] { return alive && generation==m_bindingGeneration && serial==m_statusSerial; };
    m_availability->refreshEngaged(); if (!current()) { return; }
    const bool connected=m_model.isConnected(); const bool supported=!autoNamespace(m_model.backendCapabilities()).isEmpty();
    const std::array<QWidget*,8> widgets{m_read,m_cancel,m_enabled,m_enabledApply,m_frequency,m_mode,m_register,m_delete};
    const std::array<QString,8> keys{"","","enabled","enabled","register","register","register","delete"};
    for (int index=0;index<8;++index) {
        QWidget* widget=widgets[index]; if (widget->isEnabled()) { continue; } QString reason;
        if (!connected) { reason=tr("Connect to a radio supporting native auto-mode settings."); }
        else if (!supported) { reason=tr("This radio does not support native auto-mode settings."); }
        else if (m_confirmed.isEmpty()) { reason=tr("Read auto-mode settings to obtain a valid radio description."); }
        else if (!keys[index].isEmpty()) { reason=m_confirmed.value("operations").toMap().value(keys[index]).toMap().value("reason").toString(); }
        if (reason.isEmpty()) {
            if (!m_requests.isEmpty()) { reason=tr("Waiting for the current request to finish."); }
            else if (widget==m_read) { reason=tr("Cancel the current read before starting another."); }
            else if (widget==m_cancel) { reason=tr("No read is running."); }
            else if (widget==m_enabledApply) { reason=tr("Radio state changed after this draft. Edit it again before applying."); }
            else if (widget==m_register) {
                if (m_mode->currentIndex()<0) { reason=tr("Choose a stored mode policy for the new division."); }
                else if (!std::isfinite(m_frequency->value()) || m_frequency->value()!=std::floor(m_frequency->value()) || m_frequency->value()<m_frequency->minimum() || m_frequency->value()>m_frequency->maximum()) { reason=tr("Enter a whole upper-boundary frequency within the displayed native range."); }
                else {
                    bool duplicate=false; for (const QVariant& raw : m_confirmed.value("rows").toList()) { const QVariantMap row=raw.toMap(); duplicate=duplicate || (row.value("state")==QLatin1String("populated") && row.value("upperHz").toLongLong()==static_cast<qint64>(m_frequency->value())); }
                    reason=duplicate ? tr("An observed division already uses this upper boundary; replacement is not assumed.") : tr("Edit the new division again to bind it to the current observed table.");
                }
            } else if (widget==m_delete) {
                if (m_selectedIndex<0 || m_rows->selectedItems().isEmpty()) { reason=tr("Select an observed populated division to delete."); }
                else if (selectedRow().value("state")!=QLatin1String("populated")) { reason=tr("The selected division is not populated."); }
                else { reason=tr("Select the division again after the observed table changed."); }
            } else { reason=tr("Read current auto-mode settings before editing this control."); }
        }
        const QPointer<QWidget> target(widget); target->setToolTip(reason); if (!target || !current()) { return; }
        target->setAccessibleDescription(reason); if (!current()) { return; }
    }
}
void NativeAutoModeWidget::submit(const QString& verb,const QVariantMap& arguments) { if (!m_backend || m_backend!=m_model.backend() || !m_model.isConnected() || m_namespace.isEmpty() || !m_requests.isEmpty()) { return; } const quint64 request=nextRequest(); m_requests.insert(request,verb); if (verb=="auto.mode.get") { m_getSerial=m_statusSerial; } const QPointer<NativeAutoModeWidget> alive(this); const quint64 generation=m_bindingGeneration; render(); if (!alive || generation!=m_bindingGeneration || !m_requests.contains(request)) { return; } m_model.invokeBackendExtension(m_namespace,verb,request,arguments); }
} // namespace AetherSDR
