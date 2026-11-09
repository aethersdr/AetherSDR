#include "NativeReceiveAgcWidget.h"

#include "ControlAvailabilityRegistry.h"
#include "models/RadioModel.h"

#include <QAccessible>
#include <QAccessibleValueChangeEvent>
#include <QCheckBox>
#include <QGridLayout>
#include <QLabel>
#include <QMetaType>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStringList>
#include <QVBoxLayout>

namespace AetherSDR {
namespace {
const QStringList kBanks{QStringLiteral("slow"), QStringLiteral("mid"), QStringLiteral("fast")};
QString agcNamespace(const RadioCapabilities& caps)
{
    for (const QString& ns : caps.extensionNamespaces) {
        const QVariant version = caps.extensions.value(ns).toMap().value("nativeReceiveAgcVersion");
        if (version.typeId() == QMetaType::Int && version.toInt() == 1) { return ns; }
    }
    return {};
}
bool integerIn(const QVariant& value, int low, int high)
{
    return value.typeId() == QMetaType::Int && value.toInt() >= low && value.toInt() <= high;
}
bool textValid(const QVariant& value, bool empty = false, int maximum = 512)
{
    return value.typeId() == QMetaType::QString && value.toString().size() <= maximum
        && (empty || !value.toString().isEmpty());
}
bool revisionValid(const QVariant& value, quint64& revision)
{
    if (value.typeId() != QMetaType::QString || value.toString().size() > 20) { return false; }
    bool valid = false;
    revision = value.toString().toULongLong(&valid);
    return valid && QString::number(revision) == value.toString();
}
bool presetsKnown(const QVariantMap& presets)
{
    for (const QString& bank : kBanks) { if (!integerIn(presets.value(bank), 1, 20)) { return false; } }
    return textValid(presets.value("revision"));
}
bool statusValid(const QVariantMap& status)
{
    if (!textValid(status.value("context"), true) || !textValid(status.value("mode"), true)
        || !textValid(status.value("reason"), true, 2048) || !textValid(status.value("restoreRevision"), true)
        || status.value("editable").typeId() != QMetaType::Bool
        || status.value("presets").typeId() != QMetaType::QVariantMap
        || status.value("operations").typeId() != QMetaType::QVariantMap) { return false; }
    const QVariantMap presets = status.value("presets").toMap();
    if (!textValid(presets.value("revision"), true) || presets.value("pending").typeId() != QMetaType::Bool) { return false; }
    int fields = 0;
    for (const QString& bank : kBanks) {
        if (presets.contains(bank)) { if (!integerIn(presets.value(bank), 1, 20)) { return false; } ++fields; }
    }
    if (fields != 0 && (fields != 3 || !presetsKnown(presets))) { return false; }
    for (const QString& field : {QStringLiteral("quickEnabled"), QStringLiteral("quickThreshold"), QStringLiteral("selected")}) {
        const int low = field == QLatin1String("selected") ? 0 : 1;
        const int high = field == QLatin1String("quickEnabled") ? 2 : field == QLatin1String("selected") ? 3 : 10;
        if (status.contains(field) && !integerIn(status.value(field), low, high)) { return false; }
    }
    const QVariantMap operations = status.value("operations").toMap();
    const QStringList names = kBanks + QStringList{"quickEnabled", "quickThreshold", "restore"};
    if (operations.size() != names.size()) { return false; }
    for (const QString& name : names) {
        if (operations.value(name).typeId() != QMetaType::QVariantMap) { return false; }
        const QVariantMap operation = operations.value(name).toMap();
        if (operation.value("editable").typeId() != QMetaType::Bool
            || operation.value("pending").typeId() != QMetaType::Bool
            || !textValid(operation.value("reason"), true, 2048)) { return false; }
        if (!operation.value("editable").toBool()) { continue; }
        if (!status.value("editable").toBool() || !textValid(status.value("context")) || operation.value("pending").toBool()) { return false; }
        if (kBanks.contains(name) && (!presetsKnown(presets) || presets.value("pending").toBool())) { return false; }
        if (name == QLatin1String("restore")) {
            if (!integerIn(status.value("selected"), 0, 0) || !textValid(status.value("restoreRevision"))) { return false; }
        } else if (!kBanks.contains(name) && !status.contains(name)) { return false; }
    }
    return true;
}
quint64 nextRequest()
{
    static quint64 sequence = 0x5258414700000000ULL;
    return ++sequence;
}
}

NativeReceiveAgcWidget::NativeReceiveAgcWidget(RadioModel& model, QWidget* parent)
    : QWidget(parent), m_model(model), m_availability(new ControlAvailabilityRegistry(model, this)),
      m_quick(new QCheckBox(tr("Radio AGC quick recovery"), this)), m_thresholdObserved(new QLabel(this)),
      m_threshold(new QSpinBox(this)), m_thresholdApply(new QPushButton(tr("Apply threshold"), this)),
      m_thresholdReset(new QPushButton(tr("Reset threshold to radio initial value"), this)),
      m_restore(new QPushButton(tr("Restore previous radio AGC selection"), this)),
      m_read(new QPushButton(tr("Read radio AGC"), this)), m_context(new QLabel(this)), m_status(new QLabel(this))
{
    setObjectName("nativeReceiveAgcSettings");
    auto* layout = new QVBoxLayout(this);
    auto* explanation = new QLabel(tr("These are radio receive AGC settings, separate from host audio processing. "
        "Preset indices 1 to 20 run from fastest to slowest; they are not milliseconds. "
        "Drafts are temporary. Confirmed values change only after radio readback."), this);
    explanation->setWordWrap(true); layout->addWidget(explanation);
    for (QLabel* label : {m_context, m_status, m_thresholdObserved}) { label->setTextFormat(Qt::PlainText); label->setWordWrap(true); }
    m_context->setObjectName("nativeReceiveAgcContext"); m_context->setAccessibleName(tr("Confirmed radio AGC context"));
    m_status->setObjectName("nativeReceiveAgcStatus"); m_status->setAccessibleName(tr("Radio AGC request status"));
    layout->addWidget(m_context);
    auto* grid = new QGridLayout;
    layout->addLayout(grid);
    const QStringList labels{tr("Slow"), tr("Mid"), tr("Fast")};
    grid->addWidget(new QLabel(tr("Preset"), this), 0, 0);
    grid->addWidget(new QLabel(tr("Confirmed index"), this), 0, 1);
    grid->addWidget(new QLabel(tr("Draft index"), this), 0, 2);
    const auto gate = [this](QWidget* widget, const QString& operation, bool draftRequired = false, int bank = -1) {
        m_availability->registerWidget(widget, tr("Requires a current confirmed, editable radio AGC value; pending or unavailable values cannot be changed."),
            [this, operation, draftRequired, bank](bool connected, const RadioCapabilities& caps) {
                return connected && !agcNamespace(caps).isEmpty() && available(operation)
                    && (!draftRequired || (bank >= 0 ? m_dirty[bank] : m_thresholdDirty));
            }, [this, operation] {
                return operation == QLatin1String("quickEnabled") && m_confirmed.value("quickEnabled").toInt() == 2;
            }, false);
    };
    for (int bank = 0; bank < 3; ++bank) {
        m_observed[bank] = new QLabel(tr("Unknown"), this);
        m_observed[bank]->setObjectName("nativeReceiveAgcObserved" + kBanks[bank]);
        m_observed[bank]->setAccessibleName(tr("Confirmed %1 AGC preset index").arg(labels[bank]));
        m_draft[bank] = new QSpinBox(this); m_draft[bank]->setRange(1, 20); m_draft[bank]->setKeyboardTracking(false);
        m_draft[bank]->setObjectName("nativeReceiveAgcDraft" + kBanks[bank]);
        m_draft[bank]->setAccessibleName(tr("Draft %1 AGC preset index").arg(labels[bank]));
        m_draft[bank]->setAccessibleDescription(tr("Temporary index, 1 fastest to 20 slowest. Apply stores this preset in the radio."));
        m_apply[bank] = new QPushButton(tr("Apply %1").arg(labels[bank]), this);
        m_apply[bank]->setObjectName("nativeReceiveAgcApply" + kBanks[bank]); m_apply[bank]->setAccessibleName(m_apply[bank]->text()); m_apply[bank]->setAutoDefault(false);
        m_reset[bank] = new QPushButton(tr("Reset %1 to radio initial value").arg(labels[bank]), this);
        m_reset[bank]->setObjectName("nativeReceiveAgcReset" + kBanks[bank]); m_reset[bank]->setAccessibleName(m_reset[bank]->text()); m_reset[bank]->setAutoDefault(false);
        grid->addWidget(new QLabel(labels[bank], this), bank + 1, 0); grid->addWidget(m_observed[bank], bank + 1, 1);
        grid->addWidget(m_draft[bank], bank + 1, 2); grid->addWidget(m_apply[bank], bank + 1, 3); grid->addWidget(m_reset[bank], bank + 1, 4);
        gate(m_draft[bank], kBanks[bank]); gate(m_apply[bank], kBanks[bank], true, bank); gate(m_reset[bank], kBanks[bank]);
        connect(m_draft[bank], &QSpinBox::valueChanged, this, [this, bank](int value) {
            m_dirty[bank] = presetsKnown(m_confirmed.value("presets").toMap())
                && value != m_confirmed.value("presets").toMap().value(kBanks[bank]).toInt();
            m_availability->refreshEngaged();
        });
        connect(m_apply[bank], &QPushButton::clicked, this, [this, bank] { changePreset(bank, false); });
        connect(m_reset[bank], &QPushButton::clicked, this, [this, bank] { changePreset(bank, true); });
    }
    m_quick->setObjectName("nativeReceiveAgcQuickEnabled"); m_quick->setAccessibleName(m_quick->text());
    m_thresholdObserved->setObjectName("nativeReceiveAgcThresholdObserved"); m_thresholdObserved->setAccessibleName(tr("Confirmed AGC quick recovery threshold index"));
    m_threshold->setObjectName("nativeReceiveAgcThresholdDraft"); m_threshold->setRange(1, 10); m_threshold->setKeyboardTracking(false);
    m_threshold->setAccessibleName(tr("Draft AGC quick recovery threshold index"));
    m_threshold->setAccessibleDescription(tr("Temporary native threshold index, 1 to 10; not decibels."));
    m_thresholdApply->setObjectName("nativeReceiveAgcThresholdApply"); m_thresholdReset->setObjectName("nativeReceiveAgcThresholdReset");
    m_restore->setObjectName("nativeReceiveAgcRestore"); m_read->setObjectName("nativeReceiveAgcRead");
    for (QPushButton* button : {m_thresholdApply, m_thresholdReset, m_restore, m_read}) { button->setAccessibleName(button->text()); button->setAutoDefault(false); }
    m_restore->setAccessibleDescription(tr("While radio AGC is confirmed Off, restore its previous Slow, Mid or Fast selection. The selection is confirmed by readback."));
    layout->addWidget(m_quick); layout->addWidget(m_thresholdObserved);
    auto* thresholdRow = new QGridLayout;
    thresholdRow->addWidget(m_threshold, 0, 0); thresholdRow->addWidget(m_thresholdApply, 0, 1); thresholdRow->addWidget(m_thresholdReset, 0, 2);
    layout->addLayout(thresholdRow); layout->addWidget(m_restore); layout->addWidget(m_read); layout->addWidget(m_status); layout->addStretch();
    gate(m_quick, "quickEnabled"); gate(m_threshold, "quickThreshold"); gate(m_thresholdApply, "quickThreshold", true);
    gate(m_thresholdReset, "quickThreshold"); gate(m_restore, "restore");
    m_availability->registerWidget(m_read, tr("Requires a connected radio reporting native AGC control and a legal receive context."),
        [this](bool connected, const RadioCapabilities& caps) {
            return connected && !agcNamespace(caps).isEmpty() && m_requests.isEmpty();
        }, {}, false);
    connect(m_threshold, &QSpinBox::valueChanged, this, [this](int value) {
        m_thresholdDirty = m_confirmed.contains("quickThreshold") && value != m_confirmed.value("quickThreshold").toInt();
        m_availability->refreshEngaged();
    });
    connect(m_quick, &QCheckBox::clicked, this, [this](bool on) {
        const QVariantMap arguments{{"context", m_confirmed.value("context")}, {"value", on ? 2 : 1}};
        const QPointer<NativeReceiveAgcWidget> alive(this); render();
        if (alive) { submit("receive.agc.quick.enabled.set", "quickEnabled", arguments); }
    });
    connect(m_thresholdApply, &QPushButton::clicked, this, [this] { changeThreshold(false); });
    connect(m_thresholdReset, &QPushButton::clicked, this, [this] { changeThreshold(true); });
    connect(m_read, &QPushButton::clicked, this, [this] { submit("receive.agc.get", {}); });
    connect(m_restore, &QPushButton::clicked, this, [this] {
        submit("receive.agc.restore", "restore", {{"context", m_confirmed.value("context")}, {"restoreRevision", m_confirmed.value("restoreRevision")}});
    });
    connect(&model, &RadioModel::connectionStateChanged, this, &NativeReceiveAgcWidget::bindBackend);
    connect(&model, &RadioModel::capabilitiesChanged, this, &NativeReceiveAgcWidget::bindBackend);
    bindBackend();
}

bool NativeReceiveAgcWidget::available(const QString& operation) const
{
    const QVariantMap state = m_confirmed.value("operations").toMap().value(operation).toMap();
    return m_requests.isEmpty() && m_confirmed.value("editable").toBool()
        && state.value("editable").toBool() && !state.value("pending").toBool();
}

void NativeReceiveAgcWidget::bindBackend()
{
    IRadioBackend* backend = m_model.isConnected() ? m_model.backend() : nullptr;
    const QString ns = m_model.isConnected() ? agcNamespace(m_model.backendCapabilities()) : QString{};
    if (backend == m_backend && ns == m_namespace && m_receiveSession == m_model.receiveControlSession()) { render(); return; }
    const quint64 generation = ++m_bindingGeneration;
    m_receiveSession = m_model.receiveControlSession();
    const RadioControlSession session = m_model.controlSession();
    disconnect(m_statusConnection); disconnect(m_resultConnection); disconnect(m_errorConnection);
    m_backend = backend; m_namespace = ns; m_confirmed.clear(); m_requests.clear(); m_statusRevision = 0;
    m_draftContext.clear(); m_draftRevision.clear(); m_dirty.fill(false); m_thresholdDirty = false;
    const QPointer<NativeReceiveAgcWidget> alive(this); render();
    if (!alive || generation != m_bindingGeneration || !backend || ns.isEmpty()) { return; }
    const QPointer<IRadioBackend> source(backend);
    const auto current = [this, source, generation, session] {
        return source && generation == m_bindingGeneration && source == m_backend && source == m_model.backend()
            && m_model.isConnected() && m_model.controlSessionCurrent(session);
    };
    m_statusConnection = connect(backend, &IRadioBackend::extensionStatus, this,
        [this, current, ns](const QString& space, const QString& kind, const QVariantMap& status) {
            if (current() && space == ns && kind == QLatin1String("receiveAgc")) { acceptStatus(status); }
        });
    m_resultConnection = connect(backend, &IRadioBackend::extensionResult, this,
        [this, current](quint64 id, const QVariant& result) {
            if (!current() || !m_requests.contains(id)) { return; }
            const QString verb = m_requests.take(id);
            if (verb == QLatin1String("receive.agc.get")) {
                if (m_getSerial == m_statusSerial) { acceptStatus(result.toMap()); } else { render(); }
                return;
            }
            m_status->setText(result.typeId() == QMetaType::Bool && result.toBool()
                ? tr("Request sent. Confirmed values await radio readback.") : tr("Invalid request result; radio values remain unconfirmed."));
            render();
        });
    m_errorConnection = connect(backend, &IRadioBackend::extensionError, this,
        [this, current](quint64 id, const QString& error) {
            if (current() && m_requests.remove(id)) { m_status->setText(error); render(); }
        });
    submit("receive.agc.get", {});
}

void NativeReceiveAgcWidget::acceptStatus(const QVariantMap& status)
{
    quint64 revision = 0;
    const bool ordered = revisionValid(status.value("statusRevision"), revision);
    if (ordered && revision < m_statusRevision) { return; }
    ++m_statusSerial;
    if (!ordered || !statusValid(status)) {
        m_confirmed.clear(); m_draftContext.clear(); m_draftRevision.clear(); m_dirty.fill(false); m_thresholdDirty = false;
        m_status->setText(tr("Invalid radio AGC description. Reconnect or read radio AGC to retry.")); render(); return;
    }
    m_statusRevision = revision;
    const QVariantMap presets = status.value("presets").toMap();
    const bool contextChanged = status.value("context").toString() != m_draftContext;
    const bool changed = contextChanged || presets.value("revision").toString() != m_draftRevision;
    const bool thresholdChanged = contextChanged || status.value("quickThreshold") != m_confirmed.value("quickThreshold");
    m_confirmed = status;
    if (changed || !presetsKnown(presets)) {
        m_draftContext = status.value("context").toString(); m_draftRevision = presets.value("revision").toString(); m_dirty.fill(false);
        for (int bank = 0; bank < 3; ++bank) { const QSignalBlocker blocker(m_draft[bank]); m_draft[bank]->setValue(presets.value(kBanks[bank], 1).toInt()); }
    }
    if (thresholdChanged || !status.contains("quickThreshold")) {
        m_thresholdDirty = false; const QSignalBlocker blocker(m_threshold); m_threshold->setValue(status.value("quickThreshold", 1).toInt());
    }
    render();
}

void NativeReceiveAgcWidget::render()
{
    const QPointer<NativeReceiveAgcWidget> alive(this);
    const quint64 generation = m_bindingGeneration;
    const quint64 serial = m_statusSerial;
    {
        const QSignalBlocker blocker(m_quick);
        const bool known = m_confirmed.contains("quickEnabled");
        m_quick->setTristate(!known); m_quick->setCheckState(!known ? Qt::PartiallyChecked
            : m_confirmed.value("quickEnabled").toInt() == 2 ? Qt::Checked : Qt::Unchecked);
    }
    const QStringList selected{tr("Off"), tr("Slow"), tr("Mid"), tr("Fast")};
    m_context->setText(tr("Confirmed mode: %1. AGC selection: %2. %3")
        .arg(m_confirmed.value("mode").toString().isEmpty() ? tr("Unknown") : m_confirmed.value("mode").toString(),
            m_confirmed.contains("selected") ? selected.value(m_confirmed.value("selected").toInt()) : tr("Unknown"),
            m_confirmed.value("reason").toString()));
    const QVariantMap presets = m_confirmed.value("presets").toMap();
    for (int bank = 0; bank < 4; ++bank) {
        QLabel* label = bank < 3 ? m_observed[bank] : m_thresholdObserved;
        const QVariant value = bank < 3 ? presets.value(kBanks[bank]) : m_confirmed.value("quickThreshold");
        const QString text = value.isValid() ? tr("Confirmed index: %1").arg(value.toInt()) : tr("Unknown");
        if (label->text() != text) {
            label->setText(text); QAccessibleValueChangeEvent event(label, text); QAccessible::updateAccessibility(&event);
            if (!alive || generation != m_bindingGeneration || serial != m_statusSerial) { return; }
        }
    }
    m_availability->refreshEngaged();
}

void NativeReceiveAgcWidget::submit(const QString& verb, const QString& operation, const QVariantMap& arguments)
{
    if (!m_backend || m_backend != m_model.backend() || !m_model.isConnected() || m_namespace.isEmpty()
        || !m_requests.isEmpty() || (!operation.isEmpty() && !available(operation))) { return; }
    const quint64 request = nextRequest(); m_requests.insert(request, verb);
    if (verb == QLatin1String("receive.agc.get")) { m_getSerial = m_statusSerial; }
    const quint64 generation = m_bindingGeneration; const QPointer<NativeReceiveAgcWidget> alive(this);
    render();
    if (!alive || generation != m_bindingGeneration || !m_requests.contains(request)) { return; }
    m_model.invokeBackendExtension(m_namespace, verb, request, arguments);
}

void NativeReceiveAgcWidget::changePreset(int bank, bool reset)
{
    if (bank < 0 || bank >= 3 || !(reset ? m_reset[bank] : m_apply[bank])->isEnabled() || !available(kBanks[bank])) { return; }
    QVariantMap arguments{{"context", m_draftContext}, {"revision", m_draftRevision}, {"bank", kBanks[bank]}};
    if (!reset) { if (!m_dirty[bank]) { return; } arguments.insert("value", m_draft[bank]->value()); }
    submit(reset ? "receive.agc.preset.reset" : "receive.agc.preset.set", kBanks[bank], arguments);
}

void NativeReceiveAgcWidget::changeThreshold(bool reset)
{
    if (!(reset ? m_thresholdReset : m_thresholdApply)->isEnabled() || !available("quickThreshold")) { return; }
    QVariantMap arguments{{"context", m_confirmed.value("context")}};
    if (!reset) { if (!m_thresholdDirty) { return; } arguments.insert("value", m_threshold->value()); }
    submit(reset ? "receive.agc.quick.threshold.reset" : "receive.agc.quick.threshold.set", "quickThreshold", arguments);
}
} // namespace AetherSDR
