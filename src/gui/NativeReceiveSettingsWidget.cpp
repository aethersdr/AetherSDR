#include "NativeReceiveSettingsWidget.h"

#include "ControlAvailabilityRegistry.h"
#include "GuardedSlider.h"
#include "models/RadioModel.h"

#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QList>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStringList>
#include <QVBoxLayout>

#include <cmath>
#include <array>
#include <optional>
#include <utility>

namespace AetherSDR {
namespace {
std::optional<int> integer(const QVariant& value);

QString receiveNamespace(const RadioCapabilities& caps)
{
    for (const QString& ns : caps.extensionNamespaces) {
        if (integer(caps.extensions.value(ns).toMap().value(QStringLiteral("nativeReceiveControlsVersion"))) == 1) {
            return ns;
        }
    }
    return {};
}

bool statusRevisionValid(const QVariant& value, quint64& revision)
{
    if (value.typeId() != QMetaType::QString || value.toString().size() > 20) { return false; }
    bool valid = false;
    revision = value.toString().toULongLong(&valid);
    return valid && QString::number(revision) == value.toString();
}

bool textValid(const QVariant& value, int maximum)
{
    return value.typeId() == QMetaType::QString && value.toString().size() <= maximum;
}

quint64 nextReceiveRequest()
{
    static quint64 sequence = 0x5258440000000000ULL;
    return ++sequence;
}

std::optional<int> integer(const QVariant& value)
{
    const int type = value.typeId();
    if (type != QMetaType::Int && type != QMetaType::UInt && type != QMetaType::LongLong
        && type != QMetaType::ULongLong && type != QMetaType::Float && type != QMetaType::Double) { return {}; }
    bool ok = false;
    const double number = value.toDouble(&ok);
    if (!ok || !std::isfinite(number) || std::trunc(number) != number || std::abs(number) > 1000000) { return {}; }
    return static_cast<int>(number);
}

QVariantList choicesFor(const QVariantMap& entry)
{
    if (entry.contains(QStringLiteral("choices"))) { return entry.value(QStringLiteral("choices")).toList(); }
    QVariantList choices;
    const int low = entry.value(QStringLiteral("minimum")).toInt();
    const int high = entry.value(QStringLiteral("maximum")).toInt();
    const int step = entry.value(QStringLiteral("step")).toInt();
    if (step <= 0 || high < low || (high - low) / step > 511) { return choices; }
    const QString unit = entry.value(QStringLiteral("unit")).toString();
    for (int value = low; value <= high; value += step) {
        choices.append(QVariantMap{{QStringLiteral("value"), value},
            {QStringLiteral("label"), QString::number(value) + (unit.isEmpty() ? QString{} : QLatin1Char(' ') + unit)}});
    }
    return choices;
}
}

NativeReceiveSettingsWidget::NativeReceiveSettingsWidget(RadioModel& model, QWidget* parent)
    : QWidget(parent), m_model(model), m_availability(new ControlAvailabilityRegistry(model, this)),
      m_context(new QLabel(this)), m_status(new QLabel(this))
{
    setObjectName(QStringLiteral("nativeReceiveSettings"));
    auto* layout = new QVBoxLayout(this);
    auto* explanation = new QLabel(tr("These controls change the radio's receive processing and monitoring. "
        "Values follow radio readback; computer audio processing is configured separately. "
        "RX monitor opens squelch; DSP monitor temporarily expands the passband and forces APF off. "
        "Monitor choices send explicit On or Off requests; closing this page does not release a monitor."), this);
    explanation->setWordWrap(true);
    explanation->setProperty("rsTone", QStringLiteral("hint"));
    layout->addWidget(explanation);
    m_context->setObjectName(QStringLiteral("nativeReceiveContext"));
    layout->addWidget(m_context);
    auto* refresh = new QPushButton(tr("Read settings"), this);
    refresh->setAutoDefault(false);
    refresh->setObjectName(QStringLiteral("nativeReceiveRefresh"));
    refresh->setAccessibleName(tr("Read native receive DSP settings"));
    refresh->setAccessibleDescription(tr("Read the current receive DSP settings without changing them."));
    layout->addWidget(refresh);
    m_availability->registerWidget(refresh, tr("Requires a connected radio with native receive DSP control."),
        [](bool connected, const RadioCapabilities& caps) { return connected && !receiveNamespace(caps).isEmpty(); }, {}, false);
    connect(refresh, &QPushButton::clicked, this, &NativeReceiveSettingsWidget::readSettings);
    auto* form = new QFormLayout;
    layout->addLayout(form);
    const std::array<std::pair<const char*, QString>, 20> controls{{
        {"apf.enabled", tr("Audio peak filter")}, {"apf.shift", tr("APF shift")},
        {"apf.bandwidth", tr("APF bandwidth")}, {"apf.gain", tr("APF gain")},
        {"notch.enabled", tr("Manual notch")}, {"notch.width", tr("Notch bandwidth")},
        {"notch.position", tr("Notch position")}, {"nr.mode", tr("Noise reduction")},
        {"nr1.level", tr("NR1 level")}, {"nr2.time", tr("NR2 time constant")},
        {"nb1.enabled", tr("Noise blanker 1")}, {"nb1.level", tr("NB1 level")},
        {"nb2.enabled", tr("Noise blanker 2")}, {"nb2.level", tr("NB2 level")},
        {"nb2.type", tr("NB2 type")}, {"nb2.depth", tr("NB2 depth")},
        {"nb2.width", tr("NB2 width")}, {"bc.mode", tr("Beat cancellation")},
        {"monitor.rx", tr("RX monitor (open squelch)")},
        {"monitor.dsp", tr("DSP monitor (expand passband)")},
    }};
    for (const auto& [key, label] : controls) {
        const QString id = QString::fromLatin1(key);
        auto* editor = new GuardedComboBox(this);
        editor->setObjectName(QStringLiteral("nativeReceive_") + id);
        editor->setAccessibleName(label);
        auto* caption = new QLabel(label + QLatin1Char(':'), this);
        caption->setBuddy(editor);
        auto* row = new QWidget(this);
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->addWidget(editor, 1);
        auto* reset = new QPushButton(tr("Reset"), row);
        reset->setAutoDefault(false);
        reset->setObjectName(QStringLiteral("nativeReceiveReset_") + id);
        reset->setAccessibleName(tr("Reset %1 to radio default").arg(label));
        rowLayout->addWidget(reset);
        form->addRow(caption, row);
        m_editors.insert(id, editor);
        m_resetButtons.insert(id, reset);
        m_availability->registerWidget(editor,
            tr("Requires native receive DSP support, confirmed radio settings and receive operation."),
            [this, id](bool connected, const RadioCapabilities& caps) {
                const QVariantMap entry = control(id);
                return connected && !receiveNamespace(caps).isEmpty()
                    && m_confirmed.value(QStringLiteral("editable")).toBool()
                    && entry.value(QStringLiteral("editable")).toBool()
                    && entry.contains(QStringLiteral("value"));
            }, {}, false);
        connect(editor, &QComboBox::activated, this, [this, id](int index) { submit(id, index); });
        m_availability->registerWidget(reset,
            tr("Requires a documented radio default reset and confirmed, editable receive settings."),
            [this, id](bool connected, const RadioCapabilities& caps) {
                const QVariantMap entry = control(id);
                return connected && !receiveNamespace(caps).isEmpty()
                    && m_confirmed.value(QStringLiteral("editable")).toBool()
                    && entry.value(QStringLiteral("editable")).toBool()
                    && entry.value(QStringLiteral("resettable")).toBool()
                    && entry.contains(QStringLiteral("value"));
            }, {}, false);
        connect(reset, &QPushButton::clicked, this, [this, id] { submit(id, -1, true); });
    }
    m_status->setObjectName(QStringLiteral("nativeReceiveStatus"));
    m_status->setWordWrap(true);
    layout->addWidget(m_status);
    layout->addStretch();
    connect(&model, &RadioModel::connectionStateChanged, this, &NativeReceiveSettingsWidget::bindBackend);
    connect(&model, &RadioModel::capabilitiesChanged, this, &NativeReceiveSettingsWidget::bindBackend);
    bindBackend();
}

QVariantMap NativeReceiveSettingsWidget::control(const QString& id) const
{
    for (const QVariant& value : m_confirmed.value(QStringLiteral("controls")).toList()) {
        const QVariantMap entry = value.toMap();
        if (entry.value(QStringLiteral("id")).toString() == id) { return entry; }
    }
    return {};
}

void NativeReceiveSettingsWidget::bindBackend()
{
    const QString ns = m_model.isConnected() ? receiveNamespace(m_model.backendCapabilities()) : QString{};
    IRadioBackend* backend = m_model.isConnected() && !ns.isEmpty() ? m_model.backend() : nullptr;
    if (backend == m_backend && ns == m_namespace && m_receiveSession == m_model.receiveControlSession()) { render(); return; }
    const quint64 generation = ++m_bindingGeneration;
    m_receiveSession = m_model.receiveControlSession();
    const RadioControlSession session = m_model.controlSession();
    for (const QMetaObject::Connection& connection : m_connections) { disconnect(connection); }
    m_backend = backend;
    m_namespace = ns;
    m_query = 0;
    m_statusRevision = 0;
    ++m_statusSerial;
    m_requests.clear();
    m_confirmed.clear();
    const QPointer<NativeReceiveSettingsWidget> alive(this);
    m_status->clear();
    if (!alive || generation != m_bindingGeneration) { return; }
    render();
    if (!alive || generation != m_bindingGeneration || !backend) { return; }
    const QPointer<IRadioBackend> source = backend;
    const auto current = [this, source, generation, session] {
        return generation == m_bindingGeneration && source && source == m_backend
            && source == m_model.backend() && m_model.isConnected() && m_model.controlSessionCurrent(session);
    };
    m_connections[0] = connect(backend, &IRadioBackend::extensionStatus, this,
        [this, current, ns](const QString& space, const QString& kind, const QVariantMap& state) {
            if (current() && space == ns && kind == QLatin1String("receiveControls")) { acceptStatus(state); }
        });
    m_connections[1] = connect(backend, &IRadioBackend::extensionResult, this,
        [this, current](quint64 id, const QVariant& result) {
            if (!current()) { return; }
            if (id && id == m_query) {
                m_query = 0;
                if (m_getSerial == m_statusSerial) { acceptStatus(result.toMap()); } else { render(); }
            }
            else if (m_requests.remove(id)) {
                m_status->setText(tr("Request sent. Values shown follow radio readback."));
            }
        });
    m_connections[2] = connect(backend, &IRadioBackend::extensionError, this,
        [this, current](quint64 id, const QString& error) {
            if (!current()) { return; }
            if (id && (id == m_query || m_requests.remove(id))) {
                if (id == m_query) { m_query = 0; }
                const QPointer<NativeReceiveSettingsWidget> alive(this);
                const quint64 serial = m_statusSerial;
                m_status->setText(error);
                if (!alive || !current() || serial != m_statusSerial) { return; }
                render();
            }
        });
    readSettings();
}

void NativeReceiveSettingsWidget::readSettings()
{
    if (!m_backend || m_backend != m_model.backend() || !m_model.isConnected() || m_namespace.isEmpty()) { return; }
    m_query = nextReceiveRequest();
    m_getSerial = m_statusSerial;
    m_model.invokeBackendExtension(m_namespace, QStringLiteral("receive.controls.get"), m_query);
}

void NativeReceiveSettingsWidget::acceptStatus(const QVariantMap& status)
{
    quint64 producerRevision = 0;
    const bool ordered = statusRevisionValid(status.value(QStringLiteral("statusRevision")), producerRevision);
    if (ordered && producerRevision < m_statusRevision) { return; }
    ++m_statusSerial;
    const QVariantList entries = status.value(QStringLiteral("controls")).toList();
    const QString context = status.value(QStringLiteral("context")).toString();
    bool valid = ordered && status.value(QStringLiteral("controls")).typeId() == QMetaType::QVariantList
        && status.value(QStringLiteral("context")).typeId() == QMetaType::QString
        && !context.isEmpty() && context.size() <= 128 && entries.size() <= 20
        && (!status.contains(QStringLiteral("mode")) || status.value(QStringLiteral("mode")).typeId() == QMetaType::QString)
        && status.value(QStringLiteral("mode")).toString().size() <= 32
        && status.value(QStringLiteral("editable")).typeId() == QMetaType::Bool;
    QStringList ids;
    for (const QVariant& item : entries) {
        const QVariantMap entry = item.toMap();
        const QString id = entry.value(QStringLiteral("id")).toString();
        const std::optional<int> low = integer(entry.value(QStringLiteral("minimum")));
        const std::optional<int> high = integer(entry.value(QStringLiteral("maximum")));
        const std::optional<int> step = integer(entry.value(QStringLiteral("step")));
        valid = valid && item.typeId() == QMetaType::QVariantMap
            && textValid(entry.value(QStringLiteral("id")), 128) && m_editors.contains(id) && !ids.contains(id) && low && high && step
            && *step > 0 && *high >= *low && (*high - *low) / *step <= 511
            && textValid(entry.value(QStringLiteral("label")), 128)
            && textValid(entry.value(QStringLiteral("unit")), 64)
            && textValid(entry.value(QStringLiteral("reason")), 512)
            && (!entry.contains(QStringLiteral("pending"))
                || entry.value(QStringLiteral("pending")).typeId() == QMetaType::Bool)
            && (!entry.contains(QStringLiteral("choices"))
                || entry.value(QStringLiteral("choices")).typeId() == QMetaType::QVariantList)
            && (!entry.contains(QStringLiteral("resettable"))
                || entry.value(QStringLiteral("resettable")).typeId() == QMetaType::Bool)
            && entry.value(QStringLiteral("editable")).typeId() == QMetaType::Bool;
        ids.append(id);
        if (!valid) { break; }
        const QVariantList choices = choicesFor(entry);
        QList<int> values;
        valid = !choices.isEmpty() && choices.size() <= 512;
        for (const QVariant& choice : choices) {
            const QVariantMap option = choice.toMap();
            const std::optional<int> number = integer(option.value(QStringLiteral("value")));
            const QString label = option.value(QStringLiteral("label")).toString();
            valid = valid && choice.typeId() == QMetaType::QVariantMap
                && textValid(option.value(QStringLiteral("label")), 128) && number && *number >= *low && *number <= *high
                && (*number - *low) % *step == 0 && !values.contains(*number)
                && !label.isEmpty() && label.size() <= 128;
            if (number) { values.append(*number); }
        }
        if (entry.contains(QStringLiteral("value"))) {
            const std::optional<int> value = integer(entry.value(QStringLiteral("value")));
            valid = valid && value && values.contains(*value);
        }
        valid = valid && (!entry.value(QStringLiteral("editable")).toBool()
            || (entry.contains(QStringLiteral("value")) && !entry.value(QStringLiteral("pending")).toBool()
                && status.value(QStringLiteral("editable")).toBool()));
        if (!valid) { break; }
    }
    if (valid) { m_statusRevision = producerRevision; }
    m_confirmed = valid ? status : QVariantMap{};
    const QPointer<NativeReceiveSettingsWidget> alive(this);
    const quint64 serial = m_statusSerial;
    m_status->setText(valid ? QString{} : tr("The radio supplied invalid native receive settings."));
    if (!alive || serial != m_statusSerial) { return; }
    render();
}

void NativeReceiveSettingsWidget::render()
{
    const QPointer<NativeReceiveSettingsWidget> alive(this);
    const quint64 generation = m_bindingGeneration;
    const quint64 serial = m_statusSerial;
    const auto current = [&] { return alive && generation == m_bindingGeneration && serial == m_statusSerial; };
    m_context->setText(tr("Current mode: %1").arg(m_confirmed.value(QStringLiteral("mode"), tr("Unknown")).toString()));
    if (!current()) { return; }
    for (auto it = m_editors.cbegin(); it != m_editors.cend(); ++it) {
        QComboBox* editor = it.value();
        const QVariantMap entry = control(it.key());
        const QVariantList choices = entry.isEmpty() ? QVariantList{} : choicesFor(entry);
        QSignalBlocker blocker(editor);
        const QPointer<QComboBox> editorAlive(editor);
        bool same = editor->count() == choices.size();
        for (int index = 0; same && index < choices.size(); ++index) {
            const QVariantMap choice = choices[index].toMap();
            same = editor->itemData(index) == choice.value(QStringLiteral("value"))
                && editor->itemText(index) == choice.value(QStringLiteral("label")).toString();
        }
        if (!same) {
            editor->clear();
            if (!current()) { if (!editorAlive) { blocker.dismiss(); } return; }
            for (const QVariant& choice : choices) {
                const QVariantMap option = choice.toMap();
                editor->addItem(option.value(QStringLiteral("label")).toString(), option.value(QStringLiteral("value")));
                if (!current()) { if (!editorAlive) { blocker.dismiss(); } return; }
            }
        }
        editor->setCurrentIndex(entry.contains(QStringLiteral("value"))
            ? editor->findData(entry.value(QStringLiteral("value"))) : -1);
        if (!current()) { if (!editorAlive) { blocker.dismiss(); } return; }
    }
    m_availability->refreshEngaged();
    if (!current()) { return; }
    for (auto it = m_editors.cbegin(); it != m_editors.cend(); ++it) {
        const QVariantMap entry = control(it.key());
        QString reason = it.value()->isEnabled()
            ? tr("Select a value to request it from the radio. Values change after readback.")
            : entry.value(QStringLiteral("reason"), tr("Requires confirmed native receive DSP settings and receive operation.")).toString();
        if (reason.isEmpty()) { reason = tr("Requires confirmed native receive DSP settings and receive operation."); }
        it.value()->setAccessibleDescription(reason);
        if (!current()) { return; }
        it.value()->setToolTip(reason);
        if (!current()) { return; }
        QPushButton* reset = m_resetButtons.value(it.key());
        const QString resetReason = reset->isEnabled()
            ? tr("Request the radio's documented default for this setting and wait for readback.")
            : entry.value(QStringLiteral("resettable")).toBool() ? reason
            : tr("The radio does not provide a documented default reset for this setting.");
        reset->setAccessibleDescription(resetReason);
        if (!current()) { return; }
        reset->setToolTip(resetReason);
        if (!current()) { return; }
    }
}

void NativeReceiveSettingsWidget::submit(const QString& id, int index, bool reset)
{
    QComboBox* editor = m_editors.value(id);
    if (!editor || !editor->isEnabled() || (reset && !m_resetButtons.value(id)->isEnabled())
        || (!reset && (index < 0 || index >= editor->count()))
        || !m_backend || m_backend != m_model.backend() || !m_model.isConnected()) { return; }
    QVariantMap argument{{QStringLiteral("context"), m_confirmed.value(QStringLiteral("context"))},
        {QStringLiteral("id"), id}};
    if (!reset) { argument.insert(QStringLiteral("value"), editor->itemData(index)); }
    const QPointer<NativeReceiveSettingsWidget> alive(this);
    const quint64 generation = m_bindingGeneration;
    const quint64 serial = m_statusSerial;
    render();
    if (!alive || generation != m_bindingGeneration || serial != m_statusSerial
        || !m_backend || m_backend != m_model.backend() || !m_model.isConnected()) { return; }
    if (m_requests.size() >= 64) { m_status->setText(tr("Too many pending receive requests.")); return; }
    const quint64 request = nextReceiveRequest();
    m_requests.insert(request, id);
    m_model.invokeBackendExtension(m_namespace, reset ? QStringLiteral("receive.controls.reset")
        : QStringLiteral("receive.controls.set"), request, argument);
}
} // namespace AetherSDR
