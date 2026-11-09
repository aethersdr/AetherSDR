#include "NativeRadioMessagesWidget.h"
#include "ControlAvailabilityRegistry.h"
#include "models/RadioModel.h"

#include <QAccessible>
#include <QAccessibleValueChangeEvent>
#include <QGridLayout>
#include <QLabel>
#include <QList>
#include <QMetaType>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStringList>
#include <QVBoxLayout>

namespace AetherSDR {
namespace {
bool text(const QVariant& value, int maximum, bool empty = true)
{
    return value.typeId() == QMetaType::QString && value.toString().size() <= maximum
        && (empty || !value.toString().isEmpty());
}
bool sequence(const QVariant& value, quint64& result)
{
    if (!text(value, 20, false)) { return false; }
    bool valid = false; result = value.toString().toULongLong(&valid);
    return valid && QString::number(result) == value.toString();
}
bool integer(const QVariant& value, int minimum, int maximum)
{
    return value.typeId() == QMetaType::Int && value.toInt() >= minimum && value.toInt() <= maximum;
}
QString messagesNamespace(const RadioCapabilities& caps)
{
    for (const QString& ns : caps.extensionNamespaces) {
        const QVariantMap extension = caps.extensions.value(ns).toMap();
        const QVariant version = extension.value("nativeRadioMessagesVersion");
        const QVariant verbs = extension.value("nativeRadioMessagesVerbs");
        if (version.typeId() == QMetaType::Int && version.toInt() == 1 && verbs.typeId() == QMetaType::QStringList
            && verbs.toStringList().contains("radio.messages.get") && verbs.toStringList().contains("radio.beep.set") && verbs.toStringList().contains("radio.beep.reset")) { return ns; }
    }
    return {};
}
bool validStatus(const QVariantMap& status)
{
    if (status.size() != 8 || !text(status.value("context"), 512, false)
        || status.value("connected").typeId() != QMetaType::Bool || !text(status.value("reason"), 2048)
        || !integer(status.value("activeMessage"), 0, 2)) { return false; }
    int active = 0;
    for (int source = 1; source <= 2; ++source) {
        const QVariant raw = status.value(QStringLiteral("message%1").arg(source));
        if (raw.typeId() != QMetaType::QVariantMap) { return false; }
        const QVariantMap message = raw.toMap();
        if (message.value("known").typeId() != QMetaType::Bool) { return false; }
        if (message.size() != (message.value("known").toBool() ? 4 : 1)) { return false; }
        if (message.value("known").toBool()) {
            if (!integer(message.value("id"), 0, 999) || !text(message.value("text"), 4096)
                || message.value("recognized").typeId() != QMetaType::Bool) { return false; }
            if (message.value("id").toInt() != 0 && (source == 2 || (status.value("message2").toMap().value("known").toBool() && status.value("message2").toMap().value("id").toInt() == 0))) { active = source; }
        } else if (message.contains("id") || message.contains("text") || message.contains("recognized")) { return false; }
    }
    if (active != status.value("activeMessage").toInt() || status.value("beep").typeId() != QMetaType::QVariantMap) { return false; }
    const QVariantMap beep = status.value("beep").toMap(); quint64 revision = 0;
    if (beep.value("known").typeId() != QMetaType::Bool || !sequence(beep.value("revision"), revision)
        || beep.value("editable").typeId() != QMetaType::Bool || beep.value("pending").typeId() != QMetaType::Bool
        || !text(beep.value("reason"), 2048)) { return false; }
    if (beep.size() != (beep.value("known").toBool() ? 6 : 5)) { return false; }
    if (beep.value("known").toBool() ? !integer(beep.value("value"), 0, 20) : beep.contains("value")) { return false; }
    return !beep.value("editable").toBool() || (status.value("connected").toBool() && beep.value("known").toBool() && !beep.value("pending").toBool());
}
quint64 nextRequest() { static quint64 request = 0x52584d4500000000ULL; return ++request; }
}

NativeRadioMessagesWidget::NativeRadioMessagesWidget(RadioModel& model, QWidget* parent)
    : QWidget(parent), m_model(model), m_availability(new ControlAvailabilityRegistry(model, this)),
      m_priority(new QLabel(this)), m_observed(new QLabel(this)), m_status(new QLabel(this)),
      m_draft(new QSpinBox(this)), m_apply(new QPushButton(tr("Apply beep volume"), this)),
      m_read(new QPushButton(tr("Read radio messages"), this)), m_reset(new QPushButton(tr("Reset beep volume to radio initial value"), this))
{
    setObjectName("nativeRadioMessagesSettings");
    auto* layout = new QVBoxLayout(this);
    auto* explanation = new QLabel(tr("These are the radio's two independent message sources. A nonzero message from source 2 takes display priority; both sources remain visible. Reading messages does not dismiss or confirm them. Beep volume is stored by the radio; requested values await radio readback."), this);
    explanation->setWordWrap(true); layout->addWidget(explanation);
    for (int index = 0; index < 2; ++index) {
        m_messages[index] = new QLabel(this); m_messages[index]->setObjectName("nativeRadioMessage" + QString::number(index+1));
        m_messages[index]->setAccessibleName(tr("Radio message source %1").arg(index+1)); layout->addWidget(m_messages[index]);
    }
    m_priority->setObjectName("nativeRadioMessagePriority"); layout->addWidget(m_priority);
    auto* grid = new QGridLayout; layout->addLayout(grid);
    grid->addWidget(new QLabel(tr("Radio beep volume"), this), 0, 0); m_observed->setObjectName("nativeRadioBeepObserved"); m_observed->setAccessibleName(tr("Confirmed radio beep volume")); grid->addWidget(m_observed, 0, 1);
    m_draft->setObjectName("nativeRadioBeepDraft"); m_draft->setAccessibleName(tr("Radio beep volume draft, native index, zero is Off")); m_draft->setRange(0,20); m_draft->setSpecialValueText(tr("Off (0)")); m_draft->setValue(0);
    grid->addWidget(m_draft, 0, 2); m_apply->setObjectName("nativeRadioBeepApply"); m_apply->setAutoDefault(false); grid->addWidget(m_apply,0,3);
    m_reset->setObjectName("nativeRadioBeepReset"); m_reset->setAutoDefault(false); layout->addWidget(m_reset);
    m_read->setObjectName("nativeRadioMessagesRead"); m_read->setAutoDefault(false); layout->addWidget(m_read);
    m_status->setObjectName("nativeRadioMessagesStatus"); layout->addWidget(m_status); layout->addStretch();
    for (QLabel* label : {m_messages[0],m_messages[1],m_priority,m_observed,m_status}) { label->setTextFormat(Qt::PlainText); label->setWordWrap(true); }
    for (QWidget* widget : {static_cast<QWidget*>(m_draft),static_cast<QWidget*>(m_apply),static_cast<QWidget*>(m_reset)}) {
        m_availability->registerWidget(widget,tr("Requires confirmed editable beep volume in the current radio session."),
            [this,widget](bool connected,const RadioCapabilities& caps) {
                if (!connected || messagesNamespace(caps).isEmpty() || !writable()) { return false; }
                return widget != m_apply || (m_seeded && !m_draftRetired && m_draftContext == m_confirmed.value("context").toString()
                    && m_draftRevision == m_confirmed.value("beep").toMap().value("revision").toString());
            }, {}, false);
    }
    m_availability->registerWidget(m_read,tr("Requires a connected radio supporting native message queries."),
        [this](bool connected,const RadioCapabilities& caps) { return connected && !messagesNamespace(caps).isEmpty() && m_request==0; }, {}, false);
    connect(m_draft,&QSpinBox::valueChanged,this,[this] { editDraft(); });
    connect(m_apply,&QPushButton::clicked,this,[this] { submit(false); }); connect(m_reset,&QPushButton::clicked,this,[this] { submit(false,true); }); connect(m_read,&QPushButton::clicked,this,[this] { submit(true); });
    connect(&model,&RadioModel::connectionStateChanged,this,&NativeRadioMessagesWidget::bindBackend);
    connect(&model,&RadioModel::capabilitiesChanged,this,&NativeRadioMessagesWidget::bindBackend); bindBackend();
}
NativeRadioMessagesWidget::~NativeRadioMessagesWidget()
{
    for (QObject* child : findChildren<QObject*>()) { QObject::disconnect(child,nullptr,this,nullptr); }
    QObject::disconnect(&m_model,nullptr,this,nullptr); QObject::disconnect(&m_model,nullptr,m_availability,nullptr);
    for (const QMetaObject::Connection& connection : m_connections) { QObject::disconnect(connection); }
}
bool NativeRadioMessagesWidget::writable() const
{
    const QVariantMap beep=m_confirmed.value("beep").toMap();
    return m_request==0 && m_confirmed.value("connected").toBool() && beep.value("known").toBool()
        && beep.value("editable").toBool() && !beep.value("pending").toBool();
}
void NativeRadioMessagesWidget::editDraft()
{
    if (!writable()) { return; }
    m_seeded=true; m_dirty=true; m_draftRetired=false; m_draftBaseline=m_confirmed.value("beep").toMap().value("value").toInt(); m_draftContext=m_confirmed.value("context").toString(); m_draftRevision=m_confirmed.value("beep").toMap().value("revision").toString(); refreshAvailability();
}
void NativeRadioMessagesWidget::bindBackend()
{
    IRadioBackend* backend=m_model.isConnected() ? m_model.backend() : nullptr;
    const QString ns=backend ? messagesNamespace(m_model.backendCapabilities()) : QString{};
    if (backend==m_backend && ns==m_namespace && m_receiveSession==m_model.receiveControlSession()) { render(); return; }
    const quint64 generation=++m_bindingGeneration; m_receiveSession=m_model.receiveControlSession(); const RadioControlSession session=m_model.controlSession();
    for (const QMetaObject::Connection& connection : m_connections) { disconnect(connection); }
    m_backend=backend; m_namespace=ns; m_confirmed.clear(); m_request=0; m_statusRevision=0; ++m_statusSerial;
    m_seeded=false; m_dirty=false; m_draftRetired=false; m_draftBaseline=-1; m_draftContext.clear(); m_draftRevision.clear();
    const QPointer<NativeRadioMessagesWidget> alive(this); render(); if (!alive || generation!=m_bindingGeneration || !backend || ns.isEmpty()) { return; }
    const QPointer<IRadioBackend> source(backend); const auto current=[this,source,generation,session] { return source && generation==m_bindingGeneration && source==m_backend && source==m_model.backend() && m_model.isConnected() && m_model.controlSessionCurrent(session); };
    m_connections[0]=connect(backend,&IRadioBackend::extensionStatus,this,[this,current,ns](const QString& space,const QString& kind,const QVariantMap& status) { if (current() && space==ns && kind==QLatin1String("radioMessages")) { acceptStatus(status); } });
    m_connections[1]=connect(backend,&IRadioBackend::extensionResult,this,[this,current](quint64 request,const QVariant& result) {
        if (!current() || request!=m_request) { return; } m_request=0;
        if (m_readRequest) { if (m_getSerial==m_statusSerial) { acceptStatus(result.toMap()); } else { render(); } return; }
        const QVariantMap outcome=result.toMap();
        const bool valid=result.typeId()==QMetaType::QVariantMap && outcome.size()==2
            && outcome.value("sent").typeId()==QMetaType::Bool && outcome.value("sent").toBool()
            && outcome.value("verified").typeId()==QMetaType::Bool && !outcome.value("verified").toBool();
        const QPointer<NativeRadioMessagesWidget> alive(this);
        m_status->setText(valid ? tr("Request sent; beep volume awaits radio readback.") : tr("Invalid result; beep volume remains unconfirmed.")); if (alive) { render(); }
    });
    m_connections[2]=connect(backend,&IRadioBackend::extensionError,this,[this,current](quint64 request,const QString& error) { if (current() && request==m_request) { m_request=0; m_status->setText(error); render(); } });
    submit(true);
}
void NativeRadioMessagesWidget::acceptStatus(const QVariantMap& status)
{
    quint64 revision=0; const bool ordered=sequence(status.value("statusRevision"),revision); if (ordered && revision<m_statusRevision) { return; }
    ++m_statusSerial; if (!ordered || !validStatus(status)) { m_confirmed.clear(); m_status->setText(tr("Invalid radio message description. Read radio messages to retry.")); }
    else { m_statusRevision=revision; m_confirmed=status; } render();
}
void NativeRadioMessagesWidget::render()
{
    const QPointer<NativeRadioMessagesWidget> alive(this); const quint64 generation=m_bindingGeneration,serial=m_statusSerial;
    const auto current=[this,alive,generation,serial] { return alive && generation==m_bindingGeneration && serial==m_statusSerial; };
    for (int source=1; source<=2; ++source) {
        const QVariantMap message=m_confirmed.value(QStringLiteral("message%1").arg(source)).toMap();
        const QString value=message.value("known").toBool() ? tr("%1: %2").arg(message.value("id").toInt(),3,10,QLatin1Char('0')).arg(message.value("text").toString()) : tr("Unknown");
        m_messages[source-1]->setText(tr("Message source %1 — %2").arg(source).arg(value)); if (!current()) { return; }
        QAccessibleValueChangeEvent event(m_messages[source-1],value); QAccessible::updateAccessibility(&event); if (!current()) { return; }
    }
    const int active=m_confirmed.value("activeMessage").toInt(); const bool bothKnown=m_confirmed.value("message1").toMap().value("known").toBool() && m_confirmed.value("message2").toMap().value("known").toBool();
    m_priority->setText(active ? tr("Display priority: message source %1").arg(active) : bothKnown ? tr("No active radio message.") : tr("Message priority: Unknown")); if (!current()) { return; }
    const QVariantMap beep=m_confirmed.value("beep").toMap(); const bool known=beep.value("known").toBool();
    if (!m_seeded && known) { m_seeded=true; m_draftBaseline=beep.value("value").toInt(); m_draftContext=m_confirmed.value("context").toString(); m_draftRevision=beep.value("revision").toString(); const QPointer<QSpinBox> editor(m_draft); QSignalBlocker blocker(editor); editor->setValue(beep.value("value").toInt()); if (!editor) { blocker.dismiss(); return; } if (!current()) { return; } }
    else if (!m_seeded) { const QPointer<QSpinBox> editor(m_draft); QSignalBlocker blocker(editor); editor->setValue(0); if (!editor) { blocker.dismiss(); return; } if (!current()) { return; } }
    else if (known && m_draftContext==m_confirmed.value("context").toString() && !m_draftRetired) {
        const int observed=beep.value("value").toInt();
        if (!m_dirty || observed==m_draftBaseline || observed==m_draft->value()) {
            m_draftRevision=beep.value("revision").toString(); m_draftBaseline=observed;
        } else { m_draftRetired=true; }
    }
    const QString value=!known ? tr("Unknown") : beep.value("value").toInt()==0 ? tr("Off (0)") : tr("%1 (native index)").arg(beep.value("value").toInt());
    m_observed->setText(value); if (!current()) { return; } QAccessibleValueChangeEvent event(m_observed,value); QAccessible::updateAccessibility(&event); if (current()) { refreshAvailability(); }
}
void NativeRadioMessagesWidget::refreshAvailability()
{
    const QPointer<NativeRadioMessagesWidget> alive(this); const quint64 generation=m_bindingGeneration,serial=m_statusSerial;
    const auto current=[this,alive,generation,serial] { return alive && generation==m_bindingGeneration && serial==m_statusSerial; };
    m_availability->refreshEngaged(); if (!current()) { return; }
    const QString reason=m_confirmed.value("beep").toMap().value("reason").toString();
    for (QWidget* widget : {static_cast<QWidget*>(m_draft),static_cast<QWidget*>(m_apply),static_cast<QWidget*>(m_reset)}) {
        const QString why=widget==m_apply && m_draftRetired ? tr("Radio beep volume changed after this draft. Edit the draft again before applying.") : reason;
        if (!widget->isEnabled() && !why.isEmpty()) { QPointer<QWidget> target(widget); target->setToolTip(why); if (!current() || !target) { return; } target->setAccessibleDescription(why); if (!current()) { return; } }
    }
}
void NativeRadioMessagesWidget::submit(bool read, bool reset)
{
    if (!m_backend || m_backend!=m_model.backend() || !m_model.isConnected() || m_namespace.isEmpty() || m_request) { return; }
    if (!read && (!writable() || (!reset && (!m_seeded || m_draftRetired || m_draft->value()<0 || m_draftContext!=m_confirmed.value("context").toString() || m_draftRevision!=m_confirmed.value("beep").toMap().value("revision").toString())))) { return; }
    QVariantMap arguments=read ? QVariantMap{} : QVariantMap{{"context",m_confirmed.value("context")},{"revision",m_confirmed.value("beep").toMap().value("revision")}};
    if (!read && !reset) { arguments.insert("value",m_draft->value()); }
    const quint64 request=nextRequest(); m_request=request; m_readRequest=read; if (read) { m_getSerial=m_statusSerial; }
    const quint64 generation=m_bindingGeneration; const QPointer<NativeRadioMessagesWidget> alive(this); render(); if (!alive || generation!=m_bindingGeneration || request!=m_request) { return; }
    m_model.invokeBackendExtension(m_namespace,read ? "radio.messages.get" : reset ? "radio.beep.reset" : "radio.beep.set",request,arguments);
}
} // namespace AetherSDR
