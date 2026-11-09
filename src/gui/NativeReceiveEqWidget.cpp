#include "NativeReceiveEqWidget.h"

#include "ControlAvailabilityRegistry.h"
#include "models/RadioModel.h"

#include <QCheckBox>
#include <QAccessible>
#include <QAccessibleValueChangeEvent>
#include <QComboBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QMetaType>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVariantList>
#include <QVBoxLayout>

namespace AetherSDR {
namespace {
QString eqNamespace(const RadioCapabilities& caps)
{
    for (const QString& ns : caps.extensionNamespaces) {
        const QVariant version = caps.extensions.value(ns).toMap().value(QStringLiteral("nativeReceiveEqControlVersion"));
        if (version.typeId() == QMetaType::Int && version.toInt() == 1) { return ns; }
    }
    return {};
}
bool integerIn(const QVariant& value, int minimum, int maximum)
{
    return value.typeId() == QMetaType::Int && value.toInt() >= minimum && value.toInt() <= maximum;
}
bool tokenValid(const QVariant& value, bool emptyAllowed = false)
{
    return value.typeId() == QMetaType::QString && value.toString().size() <= 512
        && (emptyAllowed || !value.toString().isEmpty());
}
bool statusRevisionValid(const QVariant& value, quint64& revision)
{
    if (value.typeId() != QMetaType::QString || value.toString().size() > 20) { return false; }
    bool valid = false;
    revision = value.toString().toULongLong(&valid);
    return valid && QString::number(revision) == value.toString();
}
bool curveValid(const QVariant& value)
{
    if (value.typeId() != QMetaType::QVariantList || value.toList().size() != 18) { return false; }
    for (const QVariant& band : value.toList()) {
        if (!integerIn(band, 0, 30)) { return false; }
    }
    return true;
}
bool statusValid(const QVariantMap& status)
{
    if (!tokenValid(status.value("context"), true) || !tokenValid(status.value("mode"), true)
        || !tokenValid(status.value("curveRevision"), true)
        || status.value("radioStoredCurve").typeId() != QMetaType::Bool
        || !status.value("radioStoredCurve").toBool()
        || status.value("operations").typeId() != QMetaType::QVariantMap) { return false; }
    if (status.contains("enabled") && status.value("enabled").typeId() != QMetaType::Bool) { return false; }
    if (status.contains("effect") && !integerIn(status.value("effect"), 0, 8)) { return false; }
    if (status.contains("curveRaw") && (!curveValid(status.value("curveRaw"))
        || !status.contains("effect") || !tokenValid(status.value("curveRevision")))) { return false; }
    const QVariantMap operations = status.value("operations").toMap();
    if (operations.size() != 5) { return false; }
    for (const QString& field : {QStringLiteral("enabled"), QStringLiteral("effect"), QStringLiteral("curve"),
                                 QStringLiteral("reset"), QStringLiteral("copy")}) {
        if (operations.value(field).typeId() != QMetaType::QVariantMap) { return false; }
        const QVariantMap operation = operations.value(field).toMap();
        if (operation.value("editable").typeId() != QMetaType::Bool
            || operation.value("pending").typeId() != QMetaType::Bool
            || operation.value("reason").typeId() != QMetaType::QString
            || operation.value("reason").toString().size() > 2048
            || (operation.value("editable").toBool() && !tokenValid(status.value("context")))) { return false; }
        if (operation.value("editable").toBool()) {
            if (field == QLatin1String("enabled") && !status.contains("enabled")) { return false; }
            if (field == QLatin1String("effect") && !status.contains("effect")) { return false; }
            if (field != QLatin1String("enabled") && field != QLatin1String("effect")
                && (!curveValid(status.value("curveRaw")) || !status.contains("effect")
                    || !tokenValid(status.value("curveRevision")))) { return false; }
        }
    }
    if (status.contains("copyRecord")) {
        if (status.value("copyRecord").typeId() != QMetaType::QVariantMap) { return false; }
        const QVariantMap copy = status.value("copyRecord").toMap();
        if (!tokenValid(copy.value("token")) || !integerIn(copy.value("sourceEffect"), 0, 8)
            || !integerIn(copy.value("destination"), 0, 2) || !curveValid(copy.value("sourceCurveRaw"))
            || !tokenValid(copy.value("state")) || copy.value("reason").typeId() != QMetaType::QString
            || copy.value("reason").toString().size() > 2048) { return false; }
    }
    return true;
}
quint64 nextEqRequest()
{
    static quint64 sequence = 0x5258455100000000ULL;
    return ++sequence;
}
}

NativeReceiveEqWidget::NativeReceiveEqWidget(RadioModel& model, QWidget* parent)
    : QWidget(parent), m_model(model), m_availability(new ControlAvailabilityRegistry(model, this)),
      m_enabled(new QCheckBox(tr("Radio receive EQ enabled"), this)), m_effect(new QComboBox(this)),
      m_read(new QPushButton(tr("Read radio EQ"), this)), m_apply(new QPushButton(tr("Apply curve to radio"), this)),
      m_resetSelected(new QPushButton(tr("Reset selected bands to radio initial values"), this)),
      m_resetAll(new QPushButton(tr("Reset all bands to radio initial values"), this)),
      m_destination(new QComboBox(this)), m_copy(new QPushButton(tr("Copy selected effect to destination"), this)),
      m_selectDestination(new QPushButton(tr("Select copied destination effect"), this)),
      m_inspect(new QPushButton(tr("Inspect copied destination"), this)),
      m_context(new QLabel(this)), m_status(new QLabel(this))
{
    setObjectName(QStringLiteral("nativeReceiveEqSettings"));
    auto* layout = new QVBoxLayout(this);
    auto* explanation = new QLabel(tr("This is the radio's receive equalizer, separate from host audio EQ. "
        "Apply, reset and copy change stored radio effects, including named presets. Effects may be shared across receive modes. "
        "Draft values are temporary; confirmed values change only after radio readback."), this);
    explanation->setWordWrap(true);
    layout->addWidget(explanation);
    for (QLabel* label : {m_context, m_status}) { label->setWordWrap(true); label->setTextFormat(Qt::PlainText); }
    m_context->setObjectName(QStringLiteral("nativeReceiveEqContext"));
    m_status->setObjectName(QStringLiteral("nativeReceiveEqStatus"));
    m_status->setAccessibleName(tr("Radio receive EQ request status"));
    layout->addWidget(m_context);
    auto* controls = new QFormLayout;
    layout->addLayout(controls);
    m_enabled->setObjectName(QStringLiteral("nativeReceiveEqEnabled"));
    m_enabled->setAccessibleName(m_enabled->text());
    m_effect->setObjectName(QStringLiteral("nativeReceiveEqEffect"));
    m_effect->setAccessibleName(tr("Radio receive EQ effect"));
    m_effect->addItems({tr("High Boost 1"), tr("High Boost 2"), tr("Formant Pass"), tr("Bass Boost 1"),
                       tr("Bass Boost 2"), tr("Flat"), tr("User 1"), tr("User 2"), tr("User 3")});
    controls->addRow(m_enabled);
    controls->addRow(tr("Selected effect:"), m_effect);
    controls->addRow(m_read);
    auto* group = new QGroupBox(tr("Confirmed curve and temporary draft"), this);
    auto* grid = new QGridLayout(group);
    grid->addWidget(new QLabel(tr("Band"), group), 0, 0);
    grid->addWidget(new QLabel(tr("Confirmed"), group), 0, 1);
    grid->addWidget(new QLabel(tr("Draft"), group), 0, 2);
    grid->addWidget(new QLabel(tr("Reset selection"), group), 0, 3);
    layout->addWidget(group);
    for (int band = 0; band < 18; ++band) {
        const QString name = tr("%1 Hz").arg(band * 300);
        m_observed[band] = new QLabel(tr("Unknown"), group);
        m_observed[band]->setObjectName(QStringLiteral("nativeReceiveEqObserved%1").arg(band));
        m_observed[band]->setAccessibleName(tr("Confirmed EQ %1").arg(name));
        m_draft[band] = new QSpinBox(group);
        m_draft[band]->setObjectName(QStringLiteral("nativeReceiveEqDraft%1").arg(band));
        m_draft[band]->setAccessibleName(tr("Draft EQ %1").arg(name));
        m_draft[band]->setAccessibleDescription(tr("Temporary draft, minus 24 to plus 6 dB. Apply curve to radio stores all 18 bands."));
        m_draft[band]->setRange(-24, 6);
        m_draft[band]->setSuffix(tr(" dB"));
        m_draft[band]->setKeyboardTracking(false);
        m_resetSelection[band] = new QCheckBox(group);
        m_resetSelection[band]->setObjectName(QStringLiteral("nativeReceiveEqResetBand%1").arg(band));
        m_resetSelection[band]->setAccessibleName(tr("Select EQ %1 for radio initial reset").arg(name));
        grid->addWidget(new QLabel(name, group), band + 1, 0);
        grid->addWidget(m_observed[band], band + 1, 1);
        grid->addWidget(m_draft[band], band + 1, 2);
        grid->addWidget(m_resetSelection[band], band + 1, 3);
        connect(m_draft[band], &QSpinBox::valueChanged, this, [this] { m_draftDirty = true; render(); });
        connect(m_resetSelection[band], &QCheckBox::toggled, this, [this] { render(); });
    }
    for (QPushButton* button : {m_apply, m_resetSelected, m_resetAll}) { layout->addWidget(button); }
    auto* copyGroup = new QGroupBox(tr("Copy stored effect"), this);
    auto* copyForm = new QFormLayout(copyGroup);
    m_destination->setObjectName(QStringLiteral("nativeReceiveEqDestination"));
    m_destination->setAccessibleName(tr("Receive EQ copy destination"));
    m_destination->addItems({tr("User 1"), tr("User 2"), tr("User 3")});
    copyForm->addRow(tr("Overwrite destination:"), m_destination);
    copyForm->addRow(m_copy);
    auto* copyHelp = new QLabel(tr("A sent copy is unverified. Selecting its destination changes the active receive effect. "
        "Then read radio EQ and inspect the result. A difference can also reflect concurrent radio changes. "
        "Return to the previous effect using Selected effect."), copyGroup);
    copyHelp->setWordWrap(true);
    copyForm->addRow(copyHelp);
    copyForm->addRow(m_selectDestination);
    copyForm->addRow(m_inspect);
    layout->addWidget(copyGroup);
    layout->addWidget(m_status);
    layout->addStretch();
    const auto registerControl = [this](QWidget* control, const QString& operation) {
        m_availability->registerWidget(control, tr("Requires confirmed radio EQ settings and an available receive operation."),
            [this, operation](bool connected, const RadioCapabilities& caps) {
                return connected && !eqNamespace(caps).isEmpty() && operationAvailable(operation);
            }, {}, false);
    };
    registerControl(m_enabled, "enabled"); registerControl(m_effect, "effect");
    for (int band = 0; band < 18; ++band) {
        registerControl(m_draft[band], "curve"); registerControl(m_resetSelection[band], "reset");
    }
    registerControl(m_resetAll, "reset");
    registerControl(m_destination, "copy"); registerControl(m_copy, "copy");
    for (QPushButton* button : {m_read, m_apply, m_resetSelected, m_resetAll, m_copy, m_selectDestination, m_inspect}) {
        button->setAutoDefault(false);
        button->setAccessibleName(button->text());
    }
    m_read->setObjectName(QStringLiteral("nativeReceiveEqRead"));
    m_apply->setObjectName(QStringLiteral("nativeReceiveEqApply"));
    m_resetSelected->setObjectName(QStringLiteral("nativeReceiveEqResetSelected"));
    m_resetAll->setObjectName(QStringLiteral("nativeReceiveEqResetAll"));
    m_copy->setObjectName(QStringLiteral("nativeReceiveEqCopy"));
    m_selectDestination->setObjectName(QStringLiteral("nativeReceiveEqSelectDestination"));
    m_inspect->setObjectName(QStringLiteral("nativeReceiveEqInspect"));
    m_availability->registerWidget(m_read, tr("Requires a connected radio with native receive EQ support."),
        [this](bool connected, const RadioCapabilities& caps) {
            return connected && !eqNamespace(caps).isEmpty() && m_requests.isEmpty();
        }, {}, false);
    m_availability->registerWidget(m_inspect, tr("Requires a current copied effect record and confirmed destination readback."),
        [this](bool connected, const RadioCapabilities& caps) {
            const QVariantMap copy = m_confirmed.value("copyRecord").toMap();
            const QVariantMap operations = m_confirmed.value("operations").toMap();
            return connected && !eqNamespace(caps).isEmpty() && m_requests.isEmpty() && !copy.isEmpty()
                && copy.value("state").toString() != QLatin1String("stale")
                && !operations.value("effect").toMap().value("pending").toBool()
                && !operations.value("curve").toMap().value("pending").toBool()
                && m_confirmed.value("effect").toInt() == copy.value("destination").toInt() + 6
                && curveValid(m_confirmed.value("curveRaw"));
        }, {}, false);
    m_availability->registerWidget(m_apply, tr("Requires a changed draft and a fresh editable radio curve."),
        [this](bool connected, const RadioCapabilities& caps) {
            return connected && !eqNamespace(caps).isEmpty() && m_draftDirty && operationAvailable("curve");
        }, {}, false);
    m_availability->registerWidget(m_resetSelected, tr("Requires selected bands and a fresh editable radio curve."),
        [this](bool connected, const RadioCapabilities& caps) {
            if (!connected || eqNamespace(caps).isEmpty() || !operationAvailable("reset")) { return false; }
            for (QCheckBox* box : m_resetSelection) { if (box->isChecked()) { return true; } }
            return false;
        }, {}, false);
    m_availability->registerWidget(m_selectDestination, tr("Requires a current copy record and editable effect selection; this changes the active effect."),
        [this](bool connected, const RadioCapabilities& caps) {
            return connected && !eqNamespace(caps).isEmpty() && operationAvailable("effect")
                && !m_confirmed.value("copyRecord").toMap().isEmpty();
        }, {}, false);
    connect(m_enabled, &QCheckBox::clicked, this, [this](bool value) {
        const QVariantMap arguments{{"context", m_confirmed.value("context")}, {"enabled", value}};
        const QPointer<NativeReceiveEqWidget> alive(this);
        render();
        if (alive) { submit("receive.eq.enabled.set", arguments); }
    });
    connect(m_effect, &QComboBox::activated, this, [this](int value) {
        const QVariantMap arguments{{"context", m_confirmed.value("context")}, {"effect", value}};
        const QPointer<NativeReceiveEqWidget> alive(this);
        render();
        if (alive && value >= 0 && value <= 8) { submit("receive.eq.effect.set", arguments); }
    });
    connect(m_read, &QPushButton::clicked, this, [this] { submit("receive.eq.get"); });
    connect(m_apply, &QPushButton::clicked, this, &NativeReceiveEqWidget::applyCurve);
    connect(m_resetSelected, &QPushButton::clicked, this, [this] { resetBands(false); });
    connect(m_resetAll, &QPushButton::clicked, this, [this] { resetBands(true); });
    connect(m_copy, &QPushButton::clicked, this, [this] {
        if (!m_copy->isEnabled() || m_destination->currentIndex() < 0 || m_destination->currentIndex() > 2) { return; }
        submit("receive.eq.copy", {{"context", m_confirmed.value("context")}, {"curveRevision", m_confirmed.value("curveRevision")},
                                  {"destination", m_destination->currentIndex()}});
    });
    connect(m_selectDestination, &QPushButton::clicked, this, &NativeReceiveEqWidget::selectCopyDestination);
    connect(m_inspect, &QPushButton::clicked, this, [this] {
        if (m_inspect->isEnabled()) { submit("receive.eq.inspection.get", {{"copyToken", m_confirmed.value("copyRecord").toMap().value("token")}}); }
    });
    connect(&model, &RadioModel::connectionStateChanged, this, &NativeReceiveEqWidget::bindBackend);
    connect(&model, &RadioModel::capabilitiesChanged, this, &NativeReceiveEqWidget::bindBackend);
    bindBackend();
}

bool NativeReceiveEqWidget::operationAvailable(const QString& operation) const
{
    const QVariantMap state = m_confirmed.value("operations").toMap().value(operation).toMap();
    if (!m_requests.isEmpty() || !state.value("editable").toBool() || state.value("pending").toBool()) { return false; }
    if (operation == QLatin1String("curve") || operation == QLatin1String("reset") || operation == QLatin1String("copy")) {
        return curveValid(m_confirmed.value("curveRaw")) && m_draftContext == m_confirmed.value("context").toString()
            && m_draftRevision == m_confirmed.value("curveRevision").toString();
    }
    return true;
}

void NativeReceiveEqWidget::bindBackend()
{
    IRadioBackend* backend = m_model.isConnected() ? m_model.backend() : nullptr;
    const QString ns = m_model.isConnected() ? eqNamespace(m_model.backendCapabilities()) : QString{};
    if (backend == m_backend && ns == m_namespace && m_receiveSession == m_model.receiveControlSession()) { render(); return; }
    const quint64 generation = ++m_bindingGeneration;
    m_receiveSession = m_model.receiveControlSession();
    const RadioControlSession session = m_model.controlSession();
    disconnect(m_statusConnection); disconnect(m_resultConnection); disconnect(m_errorConnection);
    m_backend = backend; m_namespace = ns; m_requests.clear(); m_confirmed.clear(); m_statusRevision = 0;
    m_draftContext.clear(); m_draftRevision.clear(); m_inspectionToken.clear(); m_draftDirty = false; m_status->clear();
    const QPointer<NativeReceiveEqWidget> alive(this);
    render();
    if (!alive || generation != m_bindingGeneration) { return; }
    if (!backend || ns.isEmpty()) { return; }
    const QPointer<IRadioBackend> source(backend);
    const auto current = [this, source, generation, session] {
        return generation == m_bindingGeneration && source && source == m_backend && source == m_model.backend()
            && m_model.isConnected() && m_model.controlSessionCurrent(session);
    };
    m_statusConnection = connect(backend, &IRadioBackend::extensionStatus, this,
        [this, current, ns](const QString& space, const QString& kind, const QVariantMap& status) {
            if (current() && space == ns && kind == QLatin1String("receiveEq")) { acceptStatus(status); }
        });
    m_resultConnection = connect(backend, &IRadioBackend::extensionResult, this,
        [this, current](quint64 id, const QVariant& result) {
            if (!current() || !id || !m_requests.contains(id)) { return; }
            const QString verb = m_requests.take(id);
            if (verb == QLatin1String("receive.eq.get")) {
                if (m_getSerial == m_statusSerial) { acceptStatus(result.toMap()); } else { render(); }
                return;
            }
            if (verb == QLatin1String("receive.eq.inspection.get")) {
                const QVariantMap inspection = result.toMap();
                const QString state = inspection.value("state").toString();
                const QStringList states{QStringLiteral("matched"), QStringLiteral("differs"), QStringLiteral("awaiting"), QStringLiteral("stale")};
                if (inspection.value("copyToken").typeId() != QMetaType::QString
                    || inspection.value("copyToken").toString() != m_inspectionToken
                    || m_confirmed.value("copyRecord").toMap().value("token").toString() != m_inspectionToken
                    || inspection.value("state").typeId() != QMetaType::QString || !states.contains(state)
                    || inspection.value("reason").typeId() != QMetaType::QString) {
                    m_status->setText(tr("Destination inspection is unavailable or no longer current."));
                } else { m_status->setText(tr("Destination inspection: %1. %2").arg(state, inspection.value("reason").toString())); }
                m_inspectionToken.clear();
            } else if (verb == QLatin1String("receive.eq.copy")) {
                const QVariantMap copy = result.toMap();
                const bool sent = copy.value("sent").typeId() == QMetaType::Bool && copy.value("sent").toBool()
                    && copy.value("verified").typeId() == QMetaType::Bool && !copy.value("verified").toBool()
                    && tokenValid(copy.value("copyToken"));
                m_status->setText(sent ? tr("Copy request sent; destination not yet verified.") : tr("Copy result is invalid; destination remains unverified."));
            } else if (result.typeId() == QMetaType::Bool && result.toBool()) {
                m_status->setText(tr("Request sent. Values await radio readback."));
            } else { m_status->setText(tr("Radio EQ result is invalid; values remain unconfirmed.")); }
            render();
        });
    m_errorConnection = connect(backend, &IRadioBackend::extensionError, this,
        [this, current](quint64 id, const QString& error) {
            if (!current() || !id || !m_requests.remove(id)) { return; }
            m_status->setText(error); render();
        });
    submit("receive.eq.get");
}

void NativeReceiveEqWidget::acceptStatus(const QVariantMap& status)
{
    quint64 producerRevision = 0;
    const bool ordered = statusRevisionValid(status.value("statusRevision"), producerRevision);
    if (ordered && producerRevision < m_statusRevision) { return; }
    ++m_statusSerial;
    if (!ordered || !statusValid(status)) {
        m_confirmed.clear(); m_draftContext.clear(); m_draftRevision.clear(); m_draftDirty = false;
        m_status->setText(tr("Radio EQ description is invalid. Read radio EQ to retry.")); render(); return;
    }
    m_statusRevision = producerRevision;
    const QString context = status.value("context").toString();
    const QString revision = status.value("curveRevision").toString();
    const bool replaceDraft = context != m_draftContext || revision != m_draftRevision
        || !curveValid(status.value("curveRaw"));
    m_confirmed = status;
    if (replaceDraft) {
        m_draftContext = context; m_draftRevision = revision; m_draftDirty = false;
        const QVariantList curve = status.value("curveRaw").toList();
        for (int band = 0; band < 18; ++band) {
            const QSignalBlocker blocker(m_draft[band]);
            m_draft[band]->setValue(curve.size() == 18 ? 6 - curve[band].toInt() : 0);
            const QSignalBlocker selectionBlocker(m_resetSelection[band]);
            m_resetSelection[band]->setChecked(false);
        }
    }
    render();
}

void NativeReceiveEqWidget::render()
{
    const QPointer<NativeReceiveEqWidget> alive(this);
    const quint64 generation = m_bindingGeneration;
    const quint64 serial = m_statusSerial;
    {
        const QSignalBlocker enabledBlocker(m_enabled), effectBlocker(m_effect);
        const bool known = m_confirmed.value("enabled").typeId() == QMetaType::Bool;
        m_enabled->setTristate(!known);
        m_enabled->setCheckState(!known ? Qt::PartiallyChecked : m_confirmed.value("enabled").toBool() ? Qt::Checked : Qt::Unchecked);
        m_effect->setCurrentIndex(m_confirmed.contains("effect") ? m_confirmed.value("effect").toInt() : -1);
    }
    m_context->setText(tr("Confirmed receive mode: %1. Stored selected radio effect; no local preset replay.")
                          .arg(m_confirmed.value("mode").toString().isEmpty() ? tr("Unknown") : m_confirmed.value("mode").toString()));
    const QVariantList curve = m_confirmed.value("curveRaw").toList();
    for (int band = 0; band < 18; ++band) {
        const QString value = curve.size() == 18 ? tr("%1 dB").arg(6 - curve[band].toInt()) : tr("Unknown");
        if (m_observed[band]->text() != value) {
            m_observed[band]->setText(value);
            QAccessibleValueChangeEvent event(m_observed[band], value);
            QAccessible::updateAccessibility(&event);
            if (!alive || generation != m_bindingGeneration || serial != m_statusSerial) { return; }
        }
    }
    m_availability->refreshEngaged();
}

void NativeReceiveEqWidget::submit(const QString& verb, const QVariantMap& arguments)
{
    if (!m_backend || m_backend != m_model.backend() || !m_model.isConnected() || m_namespace.isEmpty() || !m_requests.isEmpty()) { return; }
    QString operation;
    if (verb == QLatin1String("receive.eq.enabled.set")) { operation = "enabled"; }
    else if (verb == QLatin1String("receive.eq.effect.set")) { operation = "effect"; }
    else if (verb == QLatin1String("receive.eq.curve.set")) { operation = "curve"; }
    else if (verb == QLatin1String("receive.eq.curve.reset")) { operation = "reset"; }
    else if (verb == QLatin1String("receive.eq.copy")) { operation = "copy"; }
    if (!operation.isEmpty() && !operationAvailable(operation)) { return; }
    const quint64 request = nextEqRequest();
    if (verb == QLatin1String("receive.eq.inspection.get")) { m_inspectionToken = arguments.value("copyToken").toString(); }
    m_requests.insert(request, verb);
    if (verb == QLatin1String("receive.eq.get")) { m_getSerial = m_statusSerial; }
    const QPointer<NativeReceiveEqWidget> alive(this);
    const quint64 generation = m_bindingGeneration;
    render();
    if (!alive || generation != m_bindingGeneration || !m_requests.contains(request)) { return; }
    // The invocation may synchronously destroy this page. No access follows it.
    m_model.invokeBackendExtension(m_namespace, verb, request, arguments);
}

void NativeReceiveEqWidget::applyCurve()
{
    if (!m_apply->isEnabled() || !m_draftDirty || !operationAvailable("curve")) { return; }
    QVariantList values;
    for (QSpinBox* spin : m_draft) { values.append(6 - spin->value()); }
    submit("receive.eq.curve.set", {{"context", m_draftContext}, {"curveRevision", m_draftRevision}, {"curveRaw", values}});
}

void NativeReceiveEqWidget::resetBands(bool all)
{
    if (!(all ? m_resetAll : m_resetSelected)->isEnabled() || !operationAvailable("reset")) { return; }
    QVariantList bands;
    for (int band = 0; band < 18; ++band) { if (all || m_resetSelection[band]->isChecked()) { bands.append(band); } }
    if (bands.isEmpty()) { return; }
    submit("receive.eq.curve.reset", {{"context", m_draftContext}, {"curveRevision", m_draftRevision}, {"bands", bands}});
}

void NativeReceiveEqWidget::selectCopyDestination()
{
    if (!m_selectDestination->isEnabled()) { return; }
    const QVariantMap copy = m_confirmed.value("copyRecord").toMap();
    if (!integerIn(copy.value("destination"), 0, 2)) { return; }
    submit("receive.eq.effect.set", {{"context", m_confirmed.value("context")}, {"effect", copy.value("destination").toInt() + 6}});
}
} // namespace AetherSDR
