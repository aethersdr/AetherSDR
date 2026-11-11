#include "NativeTransmitInputsWidget.h"

#include "ControlAvailabilityRegistry.h"
#include "GuardedSlider.h"
#include "models/RadioModel.h"

#include <QAccessible>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QMetaType>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include <cmath>

namespace AetherSDR {
namespace {
QString inputNamespace(const RadioCapabilities& caps)
{
    for (const QString& ns : caps.extensionNamespaces) {
        const QVariantMap metadata = caps.extensions.value(ns).toMap();
        if (metadata.value(QStringLiteral("transmitInputControlVersion")).toInt() != 1) { continue; }
        const QStringList choices = metadata.value(QStringLiteral("transmitInputChoices")).toStringList();
        if (choices.size() < 2 || choices.size() > 16) { continue; }
        bool valid = true;
        for (const QString& choice : choices) { valid = valid && !choice.isEmpty() && choice.size() <= 128; }
        if (valid) { return ns; }
    }
    return {};
}
QString inputKey(int path)
{
    return path == 0 ? QStringLiteral("send") : QStringLiteral("data");
}
quint64 nextInputRequest()
{
    static quint64 sequence = 0x4e54490000000000ULL;
    return ++sequence;
}
}

NativeTransmitInputsWidget::NativeTransmitInputsWidget(RadioModel& model, QWidget* parent)
    : QWidget(parent), m_model(model), m_availability(new ControlAvailabilityRegistry(model, this)),
      m_inputs{new GuardedComboBox(this), new GuardedComboBox(this)},
      m_readback(new QLabel(this)), m_status(new QLabel(this))
{
    setObjectName(QStringLiteral("nativeTransmitInputs"));
    auto* layout = new QVBoxLayout(this);
    auto* explanation = new QLabel(tr("Select the complete audio input combination for each transmit path. "
        "Changes require confirmed receive operation and wait for radio readback. PC Audio explicitly selects LAN for DATA SEND; "
        "turning PC Audio off preserves the SEND/PTT selection. These controls configure inputs without keying the transmitter."), this);
    explanation->setWordWrap(true);
    layout->addWidget(explanation);
    auto* group = new QGroupBox(tr("Radio transmit audio inputs"), this);
    auto* form = new QFormLayout(group);
    layout->addWidget(group);
    const QString reason = tr("Requires a connected radio with native input control, a confirmed input selection, and confirmed receive operation without a local transmit intent.");
    for (int path = 0; path < 2; ++path) {
        QComboBox* input = m_inputs[path];
        input->setObjectName(path == 0 ? QStringLiteral("nativeSendInput") : QStringLiteral("nativeDataInput"));
        input->setAccessibleName(path == 0 ? tr("SEND/PTT input") : tr("DATA SEND input"));
        input->setAccessibleDescription(tr("Select the complete front and rear audio input combination. The selection changes after radio confirmation."));
        form->addRow(path == 0 ? tr("SEND/PTT input:") : tr("DATA SEND input:"), input);
        m_availability->registerWidget(input, reason, [this, path](bool connected, const RadioCapabilities& caps) {
            return connected && !inputNamespace(caps).isEmpty() && m_confirmed.value(QStringLiteral("editable")).toBool()
                && m_confirmed.contains(inputKey(path));
        }, {}, false);
        connect(input, &QComboBox::activated, this, [this, path](int selection) { submit(path, selection); });
    }
    m_readback->setObjectName(QStringLiteral("nativeTransmitInputReadback"));
    m_readback->setAccessibleName(tr("Confirmed transmit audio inputs"));
    m_readback->setWordWrap(true);
    m_status->setObjectName(QStringLiteral("nativeTransmitInputStatus"));
    m_status->setWordWrap(true);
    layout->addWidget(m_readback);
    layout->addWidget(m_status);
    layout->addStretch();
    connect(&model, &RadioModel::connectionStateChanged, this, &NativeTransmitInputsWidget::bindBackend);
    connect(&model, &RadioModel::capabilitiesChanged, this, &NativeTransmitInputsWidget::bindBackend);
    bindBackend();
}

void NativeTransmitInputsWidget::bindBackend()
{
    const QString ns = m_model.isConnected() ? inputNamespace(m_model.backendCapabilities()) : QString{};
    IRadioBackend* backend = m_model.isConnected() ? m_model.backend() : nullptr;
    const QStringList choices = m_model.backendCapabilities().extensions.value(ns).toMap().value(QStringLiteral("transmitInputChoices")).toStringList();
    if (backend == m_backend && ns == m_namespace && choices == m_choices) { render(); return; }
    const quint64 generation = ++m_bindingGeneration;
    for (const QMetaObject::Connection& connection : m_connections) { disconnect(connection); }
    m_backend = backend;
    m_namespace = ns;
    m_choices = ns.isEmpty() ? QStringList{} : choices;
    m_query = m_edit = 0;
    m_confirmed.clear();
    m_status->clear();
    for (QComboBox* input : m_inputs) {
        const QSignalBlocker blocker(input);
        input->clear();
        input->addItems(m_choices);
    }
    render();
    if (!backend || ns.isEmpty()) { return; }
    const QPointer<IRadioBackend> source = backend;
    const auto current = [this, source, generation] {
        return generation == m_bindingGeneration && source && source == m_backend
            && source == m_model.backend() && m_model.isConnected();
    };
    m_connections[0] = connect(backend, &IRadioBackend::extensionStatus, this,
        [this, ns, current](const QString& space, const QString& kind, const QVariantMap& status) {
            if (current() && space == ns && kind == QLatin1String("inputs")) { acceptStatus(status); }
        });
    m_connections[1] = connect(backend, &IRadioBackend::extensionResult, this,
        [this, current](quint64 id, const QVariant& result) {
            if (!current()) { return; }
            if (id && id == m_query) { m_query = 0; acceptStatus(result.toMap()); }
            else if (id && id == m_edit) { m_edit = 0; m_status->setText(tr("Request sent. Waiting for radio input readback.")); }
        });
    m_connections[2] = connect(backend, &IRadioBackend::extensionError, this,
        [this, current](quint64 id, const QString& error) {
            if (current() && id && (id == m_query || id == m_edit)) { m_status->setText(error); }
        });
    m_query = nextInputRequest();
    m_model.invokeBackendExtension(ns, QStringLiteral("inputs.get"), m_query);
}

void NativeTransmitInputsWidget::acceptStatus(const QVariantMap& status)
{
    const QString context = status.value(QStringLiteral("context")).toString();
    bool valid = !context.isEmpty() && context.size() <= 128
        && status.value(QStringLiteral("editable")).typeId() == QMetaType::Bool;
    for (int path = 0; path < 2; ++path) {
        const QVariant value = status.value(inputKey(path));
        if (!value.isValid()) { continue; }
        const int type = value.typeId();
        const bool number = type == QMetaType::Int || type == QMetaType::UInt || type == QMetaType::LongLong
            || type == QMetaType::ULongLong || type == QMetaType::Double || type == QMetaType::Float;
        bool numeric = false;
        const double selection = value.toDouble(&numeric);
        valid = valid && number && numeric && std::isfinite(selection) && std::trunc(selection) == selection
            && selection >= 0 && selection < m_choices.size();
    }
    m_confirmed = valid ? status : QVariantMap{};
    m_status->setText(valid ? QString{} : tr("The radio supplied invalid transmit input state."));
    render();
}

void NativeTransmitInputsWidget::render()
{
    const auto confirmed = [this](int path) {
        const QString key = inputKey(path);
        return m_confirmed.contains(key) ? m_choices.value(m_confirmed.value(key).toInt()) : tr("Unknown");
    };
    for (int path = 0; path < 2; ++path) {
        const QSignalBlocker blocker(m_inputs[path]);
        m_inputs[path]->setCurrentIndex(m_confirmed.contains(inputKey(path)) ? m_confirmed.value(inputKey(path)).toInt() : -1);
    }
    const QString text = tr("Confirmed SEND/PTT: %1\nConfirmed DATA SEND: %2").arg(confirmed(0), confirmed(1));
    if (m_readback->text() != text) {
        m_readback->setText(text);
        QAccessibleEvent event(m_readback, QAccessible::NameChanged);
        QAccessible::updateAccessibility(&event);
    }
    m_availability->refreshEngaged();
}

void NativeTransmitInputsWidget::submit(int path, int selection)
{
    if (!m_inputs[path]->isEnabled() || selection < 0 || selection >= m_choices.size() || !m_backend
        || m_backend != m_model.backend() || !m_model.isConnected() || m_namespace.isEmpty()) { render(); return; }
    render();
    m_edit = nextInputRequest();
    m_model.invokeBackendExtension(m_namespace, QStringLiteral("inputs.set"), m_edit,
        QVariantMap{{QStringLiteral("path"), inputKey(path)}, {QStringLiteral("selection"), selection},
            {QStringLiteral("context"), m_confirmed.value(QStringLiteral("context"))}});
}
} // namespace AetherSDR
