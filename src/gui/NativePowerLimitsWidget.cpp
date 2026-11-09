#include "NativePowerLimitsWidget.h"

#include "ControlAvailabilityRegistry.h"
#include "models/RadioModel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QMetaType>
#include <QPushButton>
#include <QSet>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStringList>
#include <QVBoxLayout>

#include <cmath>

namespace AetherSDR {
namespace {
QString powerNamespace(const RadioCapabilities& caps)
{
    for (const QString& ns : caps.extensionNamespaces) {
        if (caps.extensions.value(ns).toMap().value(QStringLiteral("powerLimitControlVersion")).toInt() == 1) { return ns; }
    }
    return {};
}
std::optional<int> integer(const QVariant& value, int minimum, int maximum)
{
    const int type = value.typeId();
    const bool number = type == QMetaType::Int || type == QMetaType::UInt || type == QMetaType::LongLong
        || type == QMetaType::ULongLong || type == QMetaType::Double || type == QMetaType::Float;
    bool converted = false;
    const double result = value.toDouble(&converted);
    if (!number || !converted || !std::isfinite(result) || std::trunc(result) != result
        || result < minimum || result > maximum) { return std::nullopt; }
    return static_cast<int>(result);
}
quint64 nextPowerRequest()
{
    static quint64 sequence = 0x50574c0000000000ULL;
    return ++sequence;
}
bool validNames(const QStringList& names, int maximum)
{
    if (names.isEmpty() || names.size() > maximum) { return false; }
    for (const QString& name : names) { if (name.isEmpty() || name.size() > 64) { return false; } }
    return true;
}
}

NativePowerLimitsWidget::NativePowerLimitsWidget(RadioModel& model, QWidget* parent)
    : QWidget(parent), m_model(model), m_availability(new ControlAvailabilityRegistry(model, this)),
      m_type(new QComboBox(this)), m_band(new QComboBox(this)), m_watts(new QSpinBox(this)),
      m_enabled(new QCheckBox(tr("Enable power limiter"), this)),
      m_readback(new QLabel(this)), m_status(new QLabel(this))
{
    setObjectName(QStringLiteral("nativePowerLimits"));
    auto* layout = new QVBoxLayout(this);
    auto* explanation = new QLabel(tr("Select a configuration band and mode to read its radio-owned power limit. "
        "Selection does not tune the radio. Changes require confirmed receive operation and wait for radio readback. "
        "TX tuning has its own row; configuring it does not enable AetherSDR's TUNE carrier."), this);
    explanation->setWordWrap(true);
    layout->addWidget(explanation);
    auto* group = new QGroupBox(tr("Radio power limits"), this);
    auto* form = new QFormLayout(group);
    layout->addWidget(group);
    m_type->setObjectName(QStringLiteral("nativePowerLimitType"));
    m_type->setAccessibleName(tr("Power limit mode"));
    m_band->setObjectName(QStringLiteral("nativePowerLimitBand"));
    m_band->setAccessibleName(tr("Power limit configuration band"));
    m_watts->setObjectName(QStringLiteral("nativePowerLimitWatts"));
    m_watts->setAccessibleName(tr("Requested native power limit"));
    m_watts->setAccessibleDescription(tr("Native radio limit from 5 to 100 watts. Enter a value, then select Apply limit."));
    m_watts->setRange(5, 100);
    m_watts->setSuffix(tr(" W"));
    m_watts->setKeyboardTracking(false);
    m_enabled->setObjectName(QStringLiteral("nativePowerLimiterEnabled"));
    auto* apply = new QPushButton(tr("Apply limit"), group);
    apply->setObjectName(QStringLiteral("nativePowerLimitApply"));
    apply->setAutoDefault(false);
    auto* read = new QPushButton(tr("Read limit"), group);
    read->setObjectName(QStringLiteral("nativePowerLimitRead"));
    read->setAutoDefault(false);
    form->addRow(tr("Mode:"), m_type);
    form->addRow(tr("Band:"), m_band);
    form->addRow(read);
    form->addRow(tr("Requested limit:"), m_watts);
    form->addRow(apply);
    form->addRow(m_enabled);
    const auto readable = [this](bool connected, const RadioCapabilities& caps) {
        return connected && !powerNamespace(caps).isEmpty() && m_type->count() > 0 && m_band->count() > 0;
    };
    const auto editable = [this, readable](bool connected, const RadioCapabilities& caps) {
        return readable(connected, caps) && m_confirmed.value(QStringLiteral("editable")).toBool() && confirmedWatts().has_value();
    };
    const QString reason = tr("Requires a connected radio, a confirmed limit for this band and mode, and radio-reported receive operation with no pending transmit action.");
    m_availability->registerWidget(m_type, tr("Requires a connected radio with native power-limit configuration."), readable, {}, false);
    m_availability->registerWidget(m_band, tr("Requires a connected radio with native power-limit configuration."), readable, {}, false);
    m_availability->registerWidget(read, tr("Requires a connected radio with native power-limit configuration."), readable, {}, false);
    m_availability->registerWidget(m_watts, reason, editable, {}, false);
    m_availability->registerWidget(apply, reason, editable, {}, false);
    m_availability->registerWidget(m_enabled,
        tr("Requires a connected radio, a confirmed limiter state, and radio-reported receive operation with no pending transmit action."),
        [this, readable](bool connected, const RadioCapabilities& caps) {
            return readable(connected, caps) && m_confirmed.value(QStringLiteral("editable")).toBool()
                && m_confirmed.contains(QStringLiteral("enabled"));
        }, {}, false);
    for (QComboBox* combo : {m_type, m_band}) {
        connect(combo, &QComboBox::activated, this, [this] { m_seeded = false; render(); querySelection(); });
    }
    connect(read, &QPushButton::clicked, this, &NativePowerLimitsWidget::querySelection);
    connect(apply, &QPushButton::clicked, this, [this] {
        if (!m_watts->isEnabled()) { return; }
        submit(QStringLiteral("power.limits.set"), {{QStringLiteral("type"), m_type->currentIndex()},
            {QStringLiteral("band"), m_band->currentIndex()}, {QStringLiteral("watts"), m_watts->value()}});
    });
    connect(m_enabled, &QCheckBox::toggled, this, [this](bool enabled) {
        if (!m_enabled->isEnabled()) { return; }
        render();
        submit(QStringLiteral("power.limiter.set"), {{QStringLiteral("enabled"), enabled}});
    });
    m_readback->setObjectName(QStringLiteral("nativePowerLimitReadback"));
    m_readback->setWordWrap(true);
    m_status->setWordWrap(true);
    layout->addWidget(m_readback);
    layout->addWidget(m_status);
    layout->addStretch();
    connect(&model, &RadioModel::connectionStateChanged, this, &NativePowerLimitsWidget::bindBackend);
    connect(&model, &RadioModel::capabilitiesChanged, this, &NativePowerLimitsWidget::bindBackend);
    bindBackend();
}

void NativePowerLimitsWidget::bindBackend()
{
    const RadioCapabilities caps = m_model.backendCapabilities();
    const QString ns = m_model.isConnected() ? powerNamespace(caps) : QString{};
    IRadioBackend* backend = m_model.isConnected() ? m_model.backend() : nullptr;
    if (backend == m_backend && ns == m_namespace) { render(); return; }
    const quint64 generation = ++m_bindingGeneration;
    for (const QMetaObject::Connection& connection : m_connections) { disconnect(connection); }
    m_backend = backend;
    m_namespace = ns;
    m_getRequest = m_query = m_edit = 0;
    m_seeded = false;
    m_confirmed.clear();
    m_status->clear();
    {
        const QSignalBlocker typeBlock(m_type);
        const QSignalBlocker bandBlock(m_band);
        m_type->clear();
        m_band->clear();
        const QVariantMap metadata = caps.extensions.value(ns).toMap();
        const QStringList types = metadata.value(QStringLiteral("powerLimitTypes")).toStringList();
        const QStringList bands = metadata.value(QStringLiteral("powerLimitBands")).toStringList();
        if (validNames(types, 16) && validNames(bands, 32)) { m_type->addItems(types); m_band->addItems(bands); }
    }
    render();
    if (!backend || ns.isEmpty()) { return; }
    const QPointer<IRadioBackend> source = backend;
    const auto current = [this, source, generation] {
        return generation == m_bindingGeneration && source && source == m_backend
            && source == m_model.backend() && m_model.isConnected();
    };
    m_connections[0] = connect(backend, &IRadioBackend::extensionStatus, this,
        [this, current, ns](const QString& space, const QString& kind, const QVariantMap& state) {
            if (current() && space == ns && kind == QLatin1String("power.limits")) { acceptStatus(state); }
        });
    m_connections[1] = connect(backend, &IRadioBackend::extensionResult, this,
        [this, current](quint64 id, const QVariant& result) {
            if (!current() || !id) { return; }
            if (id == m_getRequest) {
                m_getRequest = 0;
                const QPointer<NativePowerLimitsWidget> guard(this);
                acceptStatus(result.toMap());
                if (guard && m_confirmed.contains(QStringLiteral("context"))) { querySelection(); }
            }
            else if (id == m_query) { m_query = 0; acceptStatus(result.toMap()); }
            else if (id == m_edit) { m_edit = 0; m_status->setText(tr("Request sent. Confirmed settings are shown below.")); }
        });
    m_connections[2] = connect(backend, &IRadioBackend::extensionError, this,
        [this, current](quint64 id, const QString& error) {
            if (current() && id && (id == m_getRequest || id == m_query || id == m_edit)) { m_status->setText(error); }
        });
    m_getRequest = nextPowerRequest();
    m_model.invokeBackendExtension(ns, QStringLiteral("power.limits.get"), m_getRequest);
}

void NativePowerLimitsWidget::acceptStatus(const QVariantMap& state)
{
    const QString context = state.value(QStringLiteral("context")).toString();
    const QVariantList limits = state.value(QStringLiteral("limits")).toList();
    bool valid = !context.isEmpty() && context.size() <= 128 && limits.size() <= 512
        && state.value(QStringLiteral("limits")).typeId() == QMetaType::QVariantList
        && state.value(QStringLiteral("editable")).typeId() == QMetaType::Bool;
    if (state.contains(QStringLiteral("enabled"))) { valid = valid && state.value(QStringLiteral("enabled")).typeId() == QMetaType::Bool; }
    QSet<int> seen;
    for (const QVariant& value : limits) {
        const QVariantMap limit = value.toMap();
        const std::optional<int> type = integer(limit.value(QStringLiteral("type")), 0, m_type->count() - 1);
        const std::optional<int> band = integer(limit.value(QStringLiteral("band")), 0, m_band->count() - 1);
        const std::optional<int> watts = integer(limit.value(QStringLiteral("watts")), 5, 100);
        if (!type || !band || !watts || seen.contains(*type * 32 + *band)) { valid = false; break; }
        seen.insert(*type * 32 + *band);
    }
    m_confirmed = valid ? state : QVariantMap{};
    m_status->setText(valid ? QString{} : tr("The radio supplied invalid power-limit data."));
    render();
}

std::optional<int> NativePowerLimitsWidget::confirmedWatts() const
{
    for (const QVariant& value : m_confirmed.value(QStringLiteral("limits")).toList()) {
        const QVariantMap limit = value.toMap();
        if (limit.value(QStringLiteral("type")).toInt() == m_type->currentIndex()
            && limit.value(QStringLiteral("band")).toInt() == m_band->currentIndex()) { return limit.value(QStringLiteral("watts")).toInt(); }
    }
    return std::nullopt;
}

void NativePowerLimitsWidget::render()
{
    const QSignalBlocker enabledBlock(m_enabled);
    const QSignalBlocker wattsBlock(m_watts);
    m_enabled->setChecked(m_confirmed.value(QStringLiteral("enabled")).toBool());
    const std::optional<int> watts = confirmedWatts();
    if (!m_seeded && watts) { m_watts->setValue(*watts); m_seeded = true; }
    const QString enabled = m_confirmed.contains(QStringLiteral("enabled"))
        ? (m_confirmed.value(QStringLiteral("enabled")).toBool() ? tr("On") : tr("Off")) : tr("Unknown");
    m_readback->setText(tr("Confirmed limit: %1 · Limiter: %2")
        .arg(watts ? tr("%1 W").arg(*watts) : tr("Unknown"), enabled));
    m_availability->refreshEngaged();
}

void NativePowerLimitsWidget::querySelection()
{
    if (!m_backend || m_backend != m_model.backend() || !m_model.isConnected()
        || m_namespace.isEmpty() || !m_type->isEnabled()) { return; }
    if (!m_confirmed.contains(QStringLiteral("context"))) {
        m_getRequest = nextPowerRequest();
        m_model.invokeBackendExtension(m_namespace, QStringLiteral("power.limits.get"), m_getRequest);
        return;
    }
    m_query = nextPowerRequest();
    m_model.invokeBackendExtension(m_namespace, QStringLiteral("power.limits.query"), m_query,
        QVariantMap{{QStringLiteral("type"), m_type->currentIndex()}, {QStringLiteral("band"), m_band->currentIndex()},
            {QStringLiteral("context"), m_confirmed.value(QStringLiteral("context"))}});
}

void NativePowerLimitsWidget::submit(const QString& verb, QVariantMap fields)
{
    if (!m_backend || m_backend != m_model.backend() || !m_model.isConnected()
        || m_namespace.isEmpty() || !m_confirmed.value(QStringLiteral("editable")).toBool()) { return; }
    fields.insert(QStringLiteral("context"), m_confirmed.value(QStringLiteral("context")));
    m_edit = nextPowerRequest();
    m_model.invokeBackendExtension(m_namespace, verb, m_edit, fields);
}
} // namespace AetherSDR
