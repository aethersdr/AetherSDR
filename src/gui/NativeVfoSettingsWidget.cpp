#include "NativeVfoSettingsWidget.h"

#include "ControlAvailabilityRegistry.h"
#include "GuardedSlider.h"
#include "models/RadioModel.h"

#include <QCheckBox>
#include <QAccessible>
#include <QAccessibleValueChangeEvent>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QMetaType>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <QSpinBox>
#include <QSet>
#include <QtMath>

#include <algorithm>

namespace AetherSDR {
namespace {
bool revisionValid(const QVariant& value, quint64& revision)
{
    if (value.typeId() != QMetaType::QString || value.toString().size() > 20) { return false; }
    bool valid = false;
    revision = value.toString().toULongLong(&valid);
    return valid && QString::number(revision) == value.toString();
}

bool textValid(const QVariant& value, int maximum, bool emptyAllowed = false)
{
    return value.typeId() == QMetaType::QString && value.toString().size() <= maximum
        && (emptyAllowed || !value.toString().isEmpty());
}

bool frequencyValid(const QVariant& value)
{
    if (value.typeId() != QMetaType::Int && value.typeId() != QMetaType::UInt
        && value.typeId() != QMetaType::LongLong && value.typeId() != QMetaType::ULongLong) { return false; }
    bool valid = false;
    const qint64 frequency = value.toLongLong(&valid);
    return valid && frequency > 0;
}

bool vfoStatusValid(const QVariantMap& status)
{
    if (status.value("txActive").typeId() != QMetaType::Bool
        || (status.contains("transmitSelectable") && status.value("transmitSelectable").typeId() != QMetaType::Bool)) { return false; }
    for (const QString& field : {QStringLiteral("receive"), QStringLiteral("transmit")}) {
        if (status.contains(field) && (!textValid(status.value(field), 1)
            || (status.value(field).toString() != QLatin1String("A") && status.value(field).toString() != QLatin1String("B")))) { return false; }
    }
    for (const QString& field : {QStringLiteral("receiveSource"), QStringLiteral("transmitSource")}) {
        if (status.contains(field) && (!textValid(status.value(field), 32) || status.value(field).toString() != QLatin1String("memory"))) { return false; }
    }
    for (const QString& field : {QStringLiteral("modeA"), QStringLiteral("modeB")}) {
        if (status.contains(field) && !textValid(status.value(field), 32)) { return false; }
    }
    for (const QString& field : {QStringLiteral("frequencyAHz"), QStringLiteral("frequencyBHz")}) {
        if (status.contains(field) && !frequencyValid(status.value(field))) { return false; }
    }
    return !status.contains("split") || status.value("split").typeId() == QMetaType::Bool;
}

QString vfoNamespace(const RadioCapabilities& caps)
{
    for (const QString& ns : caps.extensionNamespaces) {
        const QVariant version = caps.extensions.value(ns).toMap().value(QStringLiteral("vfoControlVersion"));
        if (version.typeId() == QMetaType::Int && version.toInt() == 1) {
            return ns;
        }
    }
    return {};
}

QString tuningNamespace(const RadioCapabilities& caps)
{
    for (const QString& ns : caps.extensionNamespaces) {
        const QVariant version = caps.extensions.value(ns).toMap().value(QStringLiteral("nativeTuningControlVersion"));
        if (version.typeId() == QMetaType::Int && version.toInt() == 1) { return ns; }
    }
    return {};
}
const std::array<QString, 3> kTuningFields{QStringLiteral("receiveFine"), QStringLiteral("mhzStep"), QStringLiteral("locked")};

quint64 nextRequest()
{
    static quint64 sequence = 0x56464f0000000000ULL;
    return ++sequence;
}
}

NativeVfoSettingsWidget::NativeVfoSettingsWidget(RadioModel& model, QWidget* parent)
    : QWidget(parent), m_model(model),
      m_availability(new ControlAvailabilityRegistry(model, this)),
      m_tuning{new QCheckBox(this), new QCheckBox(this), new QCheckBox(this)},
      m_tuningReason(new QLabel(this)),
      m_tuningRead(new QPushButton(tr("Read tuning settings"), this)),
      m_receive(new QComboBox(this)), m_transmit(new QComboBox(this)),
      m_frequency{new QDoubleSpinBox(this), new QDoubleSpinBox(this)},
      m_readback(new QLabel(this)), m_status(new QLabel(this))
{
    setObjectName(QStringLiteral("nativeVfoSettings"));
    auto* layout = new QVBoxLayout(this);
    auto* explanation = new QLabel(tr("VFO A and VFO B are tuning memories, not two receivers. "
        "Different receive and transmit VFOs select split operation. Changes wait for radio readback. "
        "VFO changes are unavailable while the radio reports transmitting."), this);
    explanation->setWordWrap(true);
    layout->addWidget(explanation);
    auto* group = new QGroupBox(tr("Radio VFOs"), this);
    auto* form = new QFormLayout(group);
    layout->addWidget(group);
    m_receive->setObjectName(QStringLiteral("nativeReceiveVfo"));
    m_transmit->setObjectName(QStringLiteral("nativeTransmitVfo"));
    m_receive->setAccessibleName(tr("Receive VFO"));
    m_transmit->setAccessibleName(tr("Transmit VFO"));
    for (QComboBox* combo : {m_receive, m_transmit}) { combo->addItems({"A", "B"}); }
    form->addRow(tr("Receive VFO:"), m_receive);
    form->addRow(tr("Transmit VFO:"), m_transmit);
    const auto available = [this](bool connected, const RadioCapabilities& caps) {
        return connected && !vfoNamespace(caps).isEmpty()
            && caps.tuningMinHz > 0 && caps.tuningMaxHz > caps.tuningMinHz
            && (m_confirmed.contains(QStringLiteral("receive")) || m_confirmed.value(QStringLiteral("receiveSource")) == QStringLiteral("memory"))
            && (m_confirmed.contains(QStringLiteral("transmit")) || m_confirmed.value(QStringLiteral("transmitSource")) == QStringLiteral("memory"))
            && !m_confirmed.value(QStringLiteral("txActive"), true).toBool();
    };
    const QString reason = tr("Requires a connected radio with native VFO control, confirmed VFO state, and receive operation.");
    for (QComboBox* combo : {m_receive, m_transmit}) {
        m_availability->registerWidget(combo, reason, available, {}, false);
    }
    m_availability->registerRestriction(m_transmit,
        tr("Select a receive VFO before changing the transmit VFO."),
        [this](bool, const RadioCapabilities&) {
            const QVariant selectable = m_confirmed.value(QStringLiteral("transmitSelectable"));
            return !selectable.isValid() || (selectable.typeId() == QMetaType::Bool && selectable.toBool());
        });
    connect(m_receive, &QComboBox::activated, this, [this](int index) {
        const QPointer<NativeVfoSettingsWidget> alive(this);
        const quint64 generation = m_bindingGeneration, serial = m_vfoStatusSerial;
        render();
        if (alive && generation == m_bindingGeneration && serial == m_vfoStatusSerial) {
            submit(QStringLiteral("vfo.receive"), index == 0 ? "A" : "B");
        }
    });
    connect(m_transmit, &QComboBox::activated, this, [this](int index) {
        const QPointer<NativeVfoSettingsWidget> alive(this);
        const quint64 generation = m_bindingGeneration, serial = m_vfoStatusSerial;
        render();
        if (alive && generation == m_bindingGeneration && serial == m_vfoStatusSerial) {
            submit(QStringLiteral("vfo.transmit"), index == 0 ? "A" : "B");
        }
    });
    for (int index = 0; index < 2; ++index) {
        const QString name = index == 0 ? QStringLiteral("A") : QStringLiteral("B");
        QDoubleSpinBox* frequency = m_frequency[index];
        frequency->setObjectName(QStringLiteral("nativeVfo%1Frequency").arg(name));
        frequency->setAccessibleName(tr("Requested VFO %1 frequency").arg(name));
        frequency->setAccessibleDescription(tr("Enter a frequency, then select Apply VFO %1. The confirmed frequency appears below.").arg(name));
        frequency->setDecimals(6);
        frequency->setRange(0, 0);
        frequency->setSingleStep(0.001);
        frequency->setSuffix(tr(" MHz"));
        frequency->setKeyboardTracking(false);
        auto* apply = new QPushButton(tr("Apply VFO %1").arg(name), group);
        apply->setObjectName(QStringLiteral("nativeVfo%1Apply").arg(name));
        apply->setAutoDefault(false);
        form->addRow(tr("VFO %1 frequency:").arg(name), frequency);
        form->addRow(apply);
        const auto frequencyAvailable = [this, available, index](bool connected, const RadioCapabilities& caps) {
            const QString key = index == 0 ? QStringLiteral("frequencyAHz") : QStringLiteral("frequencyBHz");
            return available(connected, caps) && m_seeded[index] && m_confirmed.contains(key);
        };
        const QString frequencyReason = tr("Requires confirmed physical VFO frequency in the current session and receive operation.");
        m_availability->registerWidget(frequency, frequencyReason, frequencyAvailable, {}, false);
        m_availability->registerWidget(apply, frequencyReason, frequencyAvailable, {}, false);
        connect(apply, &QPushButton::clicked, this, [this, name, frequency] {
            submit(QStringLiteral("vfo.frequency"), QVariantMap{
                {QStringLiteral("vfo"), name},
                {QStringLiteral("frequencyHz"), static_cast<qint64>(qRound64(frequency->value() * 1e6))}});
        });
    }
    auto* tuningGroup = new QGroupBox(tr("Radio tuning"), this);
    auto* tuningForm = new QFormLayout(tuningGroup);
    layout->addWidget(tuningGroup);
    const std::array<QString, 3> tuningLabels{tr("Radio receive FINE"), tr("Radio MHz step"), tr("Radio frequency lock")};
    const std::array<QString, 3> descriptions{
        tr("Radio tuning-control FINE state. This is separate from the application's STEP setting."),
        tr("Radio MULTI/CH MHz-step mode. Its step size is configured on the radio; this does not change the application's wheel step."),
        tr("Radio frequency-lock state. The radio's Lock Function setting determines which physical controls are locked; this is separate from client slice lock.")};
    for (int index = 0; index < 3; ++index) {
        QCheckBox* check = m_tuning[index];
        const QString field = kTuningFields[index];
        check->setObjectName(QStringLiteral("nativeTuning_") + field);
        check->setText(tuningLabels[index]);
        check->setAccessibleName(tuningLabels[index]);
        check->setAccessibleDescription(descriptions[index]);
        check->setToolTip(descriptions[index]);
        check->setProperty("nativeTuningHelp", descriptions[index]);
        tuningForm->addRow(check);
        m_availability->registerWidget(check,
            tr("Requires native radio tuning control and confirmed receive-state readback."),
            [this, field](bool connected, const RadioCapabilities& caps) {
                return connected && !tuningNamespace(caps).isEmpty()
                    && m_tuningConfirmed.value(QStringLiteral("editable")).toBool()
                    && m_tuningConfirmed.value(field).typeId() == QMetaType::Bool
                    && !m_tuningConfirmed.value(QStringLiteral("pending")).toMap().value(field).toBool();
            }, [check] { return check->isChecked(); }, false);
        connect(check, &QCheckBox::clicked, this, [this, field](bool checked) { submitTuning(field, checked); });
    }
    m_tuningRead->setObjectName(QStringLiteral("nativeTuningRead"));
    m_tuningRead->setAccessibleName(tr("Read radio tuning settings"));
    m_tuningRead->setAccessibleDescription(tr("Read current radio FINE, MHz-step and frequency-lock states without changing settings."));
    m_tuningRead->setToolTip(m_tuningRead->accessibleDescription());
    m_tuningRead->setAutoDefault(false);
    tuningForm->addRow(m_tuningRead);
    m_availability->registerWidget(m_tuningRead,
        tr("Requires connected native tuning control, confirmed receive operation and no pending read request."),
        [this](bool connected, const RadioCapabilities& caps) {
            return connected && !tuningNamespace(caps).isEmpty()
                && (m_tuningConfirmed.value(QStringLiteral("editable")).toBool()
                    || !m_tuningConfirmed.contains(QStringLiteral("context"))) && !m_tuningQuery;
        }, {}, false);
    connect(m_tuningRead, &QPushButton::clicked, this, &NativeVfoSettingsWidget::queryTuning);
    m_tuningReason->setObjectName(QStringLiteral("nativeTuningReason"));
    m_tuningReason->setTextFormat(Qt::PlainText);
    m_tuningReason->setWordWrap(true);
    tuningForm->addRow(m_tuningReason);
    m_readback->setObjectName(QStringLiteral("nativeVfoReadback"));
    m_readback->setAccessibleName(tr("Confirmed radio VFO state"));
    m_status->setObjectName(QStringLiteral("nativeVfoStatus"));
    for (QLabel* label : {m_readback, m_status}) {
        label->setTextFormat(Qt::PlainText);
        label->setWordWrap(true);
        layout->addWidget(label);
    }
    setupRelative(layout);
    layout->addStretch();
    connect(&m_model, &RadioModel::connectionStateChanged, this, &NativeVfoSettingsWidget::bindBackend);
    connect(&m_model, &RadioModel::capabilitiesChanged, this, &NativeVfoSettingsWidget::bindBackend);
    connect(&m_model, &RadioModel::connectionStateChanged, this, &NativeVfoSettingsWidget::bindRelative);
    connect(&m_model, &RadioModel::capabilitiesChanged, this, &NativeVfoSettingsWidget::bindRelative);
    bindBackend();
    bindRelative();
}

void NativeVfoSettingsWidget::bindBackend()
{
    const QString ns = m_model.isConnected() ? vfoNamespace(m_model.backendCapabilities()) : QString{};
    const QString tuningNs = m_model.isConnected() ? tuningNamespace(m_model.backendCapabilities()) : QString{};
    IRadioBackend* backend = m_model.isConnected() ? m_model.backend() : nullptr;
    if (backend == m_backend && ns == m_namespace && tuningNs == m_tuningNamespace
        && m_receiveSession == m_model.receiveControlSession()) {
        render();
        return;
    }
    const quint64 generation = ++m_bindingGeneration;
    m_receiveSession = m_model.receiveControlSession();
    const RadioControlSession session = m_model.controlSession();
    disconnect(m_statusConnection);
    disconnect(m_resultConnection);
    disconnect(m_errorConnection);
    m_backend = backend;
    m_namespace = ns;
    m_tuningNamespace = tuningNs;
    m_tuningConfirmed.clear();
    m_tuningEdits.clear();
    m_tuningQuery = 0;
    m_tuningStatusRevision = m_vfoStatusRevision = 0;
    ++m_tuningStatusSerial; ++m_vfoStatusSerial;
    m_query = m_edit = 0;
    m_confirmed.clear();
    m_seeded = {false, false};
    m_status->clear();
    const RadioCapabilities caps = m_model.backendCapabilities();
    const QPointer<NativeVfoSettingsWidget> alive(this);
    for (QDoubleSpinBox* frequency : m_frequency) {
        QSignalBlocker blocker(frequency);
        frequency->setRange(caps.tuningMinHz / 1e6, caps.tuningMaxHz / 1e6);
        if (!alive) { blocker.dismiss(); return; }
        frequency->setValue(frequency->minimum());
        if (!alive) { blocker.dismiss(); return; }
        if (generation != m_bindingGeneration) { return; }
    }
    render();
    if (!alive || generation != m_bindingGeneration || !m_backend || (ns.isEmpty() && tuningNs.isEmpty())) { return; }
    const QPointer<IRadioBackend> source = m_backend;
    m_statusConnection = connect(source, &IRadioBackend::extensionStatus, this,
        [this, source, ns, tuningNs, generation, session](const QString& space, const QString& kind, const QVariantMap& status) {
            if (generation == m_bindingGeneration && m_model.controlSessionCurrent(session) && source && source == m_backend && source == m_model.backend()
                && m_model.isConnected() && space == ns && kind == QLatin1String("vfo")) {
                acceptStatus(status);
            } else if (generation == m_bindingGeneration && m_model.controlSessionCurrent(session) && source && source == m_backend && source == m_model.backend()
                && m_model.isConnected() && !tuningNs.isEmpty() && space == tuningNs && kind == QLatin1String("tuning")) {
                acceptTuningStatus(status);
            }
        });
    m_resultConnection = connect(source, &IRadioBackend::extensionResult, this,
        [this, source, generation, session](quint64 id, const QVariant& result) {
            if (generation != m_bindingGeneration || !m_model.controlSessionCurrent(session) || !source || source != m_backend || source != m_model.backend() || !m_model.isConnected()) { return; }
            if (id == m_tuningQuery && id) {
                m_tuningQuery = 0;
                if (m_tuningGetSerial == m_tuningStatusSerial) { acceptTuningStatus(result.toMap()); } else { renderTuning(); }
            }
            else if (id && m_tuningEdits.contains(id)) { m_tuningEdits.remove(id); renderTuning(); }
            else if (id == m_query && id) {
                m_query = 0;
                if (m_vfoGetSerial == m_vfoStatusSerial) { acceptStatus(result.toMap()); } else { render(); }
            }
            else if (id == m_edit && id) {
                m_edit = 0;
                m_status->setText(tr("Request sent. Confirmed values are shown below."));
            }
        });
    m_errorConnection = connect(source, &IRadioBackend::extensionError, this,
        [this, source, generation, session](quint64 id, const QString& error) {
            if (generation == m_bindingGeneration && m_model.controlSessionCurrent(session) && source && source == m_backend && source == m_model.backend()
                && m_model.isConnected() && id) {
                if (id == m_tuningQuery || m_tuningEdits.contains(id)) {
                    m_tuningEdits.remove(id);
                    if (id == m_tuningQuery) { m_tuningQuery = 0; }
                    const QPointer<NativeVfoSettingsWidget> alive(this);
                    renderTuning();
                    if (alive) { m_tuningReason->setText(error); }
                } else if (id == m_edit || id == m_query) {
                    if (id == m_edit) { m_edit = 0; }
                    if (id == m_query) { m_query = 0; }
                    m_status->setText(error);
                }
            }
        });
    const QPointer<NativeVfoSettingsWidget> widgetGuard(this);
    if (!ns.isEmpty()) {
        m_query = nextRequest();
        m_vfoGetSerial = m_vfoStatusSerial;
        m_model.invokeBackendExtension(ns, QStringLiteral("vfo.get"), m_query);
    }
    if (widgetGuard && generation == widgetGuard->m_bindingGeneration && source && source == widgetGuard->m_backend) {
        widgetGuard->queryTuning();
    }
}

void NativeVfoSettingsWidget::acceptStatus(const QVariantMap& status)
{
    quint64 revision = 0;
    const bool ordered = revisionValid(status.value("statusRevision"), revision);
    if (ordered && revision < m_vfoStatusRevision) { render(); return; }
    ++m_vfoStatusSerial;
    if (!ordered || !vfoStatusValid(status)) {
        m_confirmed.clear();
        m_status->setText(tr("Invalid radio VFO status; waiting for current readback."));
        render(); return;
    }
    m_vfoStatusRevision = revision;
    m_confirmed = status;
    const QPointer<NativeVfoSettingsWidget> alive(this);
    const quint64 generation = m_bindingGeneration, serial = m_vfoStatusSerial;
    for (int index = 0; index < 2; ++index) {
        const QString key = index == 0 ? QStringLiteral("frequencyAHz") : QStringLiteral("frequencyBHz");
        if (!m_seeded[index] && status.contains(key)) {
            QSignalBlocker blocker(m_frequency[index]);
            m_frequency[index]->setValue(status.value(key).toDouble() / 1e6);
            if (!alive) { blocker.dismiss(); return; }
            if (generation != m_bindingGeneration || serial != m_vfoStatusSerial) { return; }
            m_seeded[index] = true;
        }
    }
    m_status->clear();
    render();
}

void NativeVfoSettingsWidget::submit(const QString& verb, const QVariant& value)
{
    if (!m_backend || m_backend != m_model.backend() || !m_model.isConnected()
        || m_namespace.isEmpty() || !m_receive->isEnabled()) { return; }
    if (verb == QLatin1String("vfo.transmit") && !m_transmit->isEnabled()) { return; }
    m_edit = nextRequest();
    m_model.invokeBackendExtension(m_namespace, verb, m_edit, value);
}

void NativeVfoSettingsWidget::render()
{
    const QPointer<NativeVfoSettingsWidget> alive(this);
    const quint64 generation = m_bindingGeneration, serial = m_vfoStatusSerial;
    {
        QSignalBlocker receiveBlock(m_receive), transmitBlock(m_transmit);
        m_receive->setCurrentIndex(m_receive->findText(m_confirmed.value(QStringLiteral("receive")).toString()));
        if (!alive) { receiveBlock.dismiss(); transmitBlock.dismiss(); return; }
        if (generation != m_bindingGeneration || serial != m_vfoStatusSerial) { return; }
        m_transmit->setCurrentIndex(m_transmit->findText(m_confirmed.value(QStringLiteral("transmit")).toString()));
        if (!alive) { receiveBlock.dismiss(); transmitBlock.dismiss(); return; }
        if (generation != m_bindingGeneration || serial != m_vfoStatusSerial) { return; }
    }
    const auto frequency = [this](const QString& key) {
        return m_confirmed.contains(key)
            ? QString::number(m_confirmed.value(key).toDouble() / 1e6, 'f', 6) + tr(" MHz") : tr("Unknown");
    };
    const QString split = m_confirmed.contains(QStringLiteral("split"))
        ? (m_confirmed.value(QStringLiteral("split")).toBool() ? tr("On") : tr("Off")) : tr("Unknown");
    QString readback = tr("Confirmed A: %1 · B: %2 · Split: %3")
        .arg(frequency(QStringLiteral("frequencyAHz")), frequency(QStringLiteral("frequencyBHz")), split);
    if (m_confirmed.value(QStringLiteral("receiveSource")) == QStringLiteral("memory")) {
        readback += tr("\nReceive source: Memory channel (unmapped)");
    }
    if (m_confirmed.value(QStringLiteral("transmitSource")) == QStringLiteral("memory")) {
        readback += tr("\nTransmit source: Memory channel (unmapped)");
    }
    if (m_readback->text() != readback) {
        m_readback->setText(readback);
        QAccessibleValueChangeEvent event(m_readback, readback);
        QAccessible::updateAccessibility(&event);
        if (!alive || generation != m_bindingGeneration || serial != m_vfoStatusSerial) { return; }
    }
    m_availability->refreshEngaged();
    if (!alive || generation != m_bindingGeneration || serial != m_vfoStatusSerial) { return; }
    renderTuning();
}

void NativeVfoSettingsWidget::queryTuning()
{
    if (!m_backend || m_backend != m_model.backend() || !m_model.isConnected()
        || m_tuningNamespace.isEmpty() || m_tuningQuery) { return; }
    m_tuningQuery = nextRequest();
    m_tuningGetSerial = m_tuningStatusSerial;
    const quint64 request = m_tuningQuery, generation = m_bindingGeneration;
    const QPointer<NativeVfoSettingsWidget> alive(this);
    renderTuning();
    if (!alive || generation != m_bindingGeneration || m_tuningQuery != request) { return; }
    m_model.invokeBackendExtension(m_tuningNamespace, QStringLiteral("tuning.get"), m_tuningQuery);
}

void NativeVfoSettingsWidget::acceptTuningStatus(const QVariantMap& status)
{
    quint64 revision = 0;
    const bool ordered = revisionValid(status.value("statusRevision"), revision);
    if (ordered && revision < m_tuningStatusRevision) { renderTuning(); return; }
    ++m_tuningStatusSerial;
    const auto reject = [this] {
        m_tuningConfirmed = {{QStringLiteral("editable"), false},
            {QStringLiteral("reason"), tr("Invalid radio tuning status; waiting for valid current readback.")}};
        m_tuningEdits.clear();
        renderTuning();
    };
    if (!ordered || !textValid(status.value(QStringLiteral("context")), 512)
        || status.value(QStringLiteral("editable")).typeId() != QMetaType::Bool
        || !textValid(status.value(QStringLiteral("reason")), 2048, true)) { reject(); return; }
    for (const QString& field : {QStringLiteral("receiveFine"), QStringLiteral("transmitFine"), QStringLiteral("mhzStep"), QStringLiteral("locked")}) {
        if (status.contains(field) && status.value(field).typeId() != QMetaType::Bool) { reject(); return; }
    }
    const QVariant pending = status.value(QStringLiteral("pending"));
    if (pending.isValid() && pending.typeId() != QMetaType::QVariantMap) { reject(); return; }
    const QVariantMap pendingFields = pending.toMap();
    for (auto it = pendingFields.cbegin(); it != pendingFields.cend(); ++it) {
        if (std::find(kTuningFields.cbegin(), kTuningFields.cend(), it.key()) == kTuningFields.cend() || it.value().typeId() != QMetaType::Bool) { reject(); return; }
    }
    if (m_tuningConfirmed.value(QStringLiteral("context")) != status.value(QStringLiteral("context"))) {
        m_tuningEdits.clear();
    }
    m_tuningStatusRevision = revision;
    m_tuningConfirmed = status;
    renderTuning();
}

void NativeVfoSettingsWidget::submitTuning(const QString& field, bool value)
{
    const QVariantMap state = m_tuningConfirmed;
    const QPointer<NativeVfoSettingsWidget> alive(this);
    const quint64 generation = m_bindingGeneration, serial = m_tuningStatusSerial;
    renderTuning();
    if (!alive || generation != m_bindingGeneration || serial != m_tuningStatusSerial
        || !m_backend || m_backend != m_model.backend() || !m_model.isConnected()
        || m_tuningNamespace.isEmpty() || !state.value(QStringLiteral("editable")).toBool()
        || state.value(field).typeId() != QMetaType::Bool
        || state.value(QStringLiteral("pending")).toMap().value(field).toBool()) { return; }
    const quint64 request = nextRequest();
    m_tuningEdits.insert(request, field);
    m_model.invokeBackendExtension(m_tuningNamespace, QStringLiteral("tuning.set"), request,
        QVariantMap{{QStringLiteral("context"), state.value(QStringLiteral("context"))},
            {QStringLiteral("field"), field}, {QStringLiteral("value"), value}});
}

void NativeVfoSettingsWidget::renderTuning()
{
    const QPointer<NativeVfoSettingsWidget> alive(this);
    const quint64 generation = m_bindingGeneration, serial = m_tuningStatusSerial;
    for (int index = 0; index < 3; ++index) {
        QCheckBox* check = m_tuning[index];
        QSignalBlocker blocker(check);
        const QVariant value = m_tuningConfirmed.value(kTuningFields[index]);
        const bool known = value.typeId() == QMetaType::Bool;
        check->setTristate(!known);
        check->setCheckState(!known ? Qt::PartiallyChecked : value.toBool() ? Qt::Checked : Qt::Unchecked);
        if (!alive) { blocker.dismiss(); return; }
        if (generation != m_bindingGeneration || serial != m_tuningStatusSerial) { return; }
    }
    m_availability->refreshEngaged();
    if (!alive || generation != m_bindingGeneration || serial != m_tuningStatusSerial) { return; }
    const QVariantMap pending = m_tuningConfirmed.value(QStringLiteral("pending")).toMap();
    QString reason = m_tuningConfirmed.value(QStringLiteral("reason")).toString();
    if (m_tuningNamespace.isEmpty() || !m_model.isConnected()) {
        reason = tr("Requires a connected radio with native tuning control.");
    }
    for (int index = 0; index < 3; ++index) {
        QCheckBox* check = m_tuning[index];
        const QString field = kTuningFields[index];
        const bool known = m_tuningConfirmed.value(field).typeId() == QMetaType::Bool;
        QString fieldReason = reason;
        if (fieldReason.isEmpty() && pending.value(field).toBool()) { fieldReason = tr("Waiting for radio readback; last confirmed value retained."); }
        if (fieldReason.isEmpty() && !known) { fieldReason = tr("Waiting for current radio readback."); }
        const QString description = fieldReason.isEmpty() ? check->property("nativeTuningHelp").toString() : fieldReason;
        check->setToolTip(description);
        check->setAccessibleDescription(description);
    }
    m_tuningReason->setText(reason.isEmpty()
        ? tr("Radio controls are separate from client STEP and slice lock. Changes wait for radio readback.") : reason);
}
} // namespace AetherSDR

namespace AetherSDR {
void NativeVfoSettingsWidget::setupRelative(QVBoxLayout* layout)
{
    auto* group = new QGroupBox(tr("Native relative tuning"), this);
    auto* form = new QFormLayout(group);
    auto* help = new QLabel(tr("Steps use the radio's current tuning-control step. The radio reports the resulting frequency. These controls do not change the application's STEP setting."), group);
    help->setWordWrap(true); form->addRow(help);
    m_relativeObserved = new QLabel(group); m_relativeObserved->setObjectName("nativeRelativeObserved");
    m_relativeObserved->setTextFormat(Qt::PlainText); m_relativeObserved->setWordWrap(true); form->addRow(m_relativeObserved);
    m_relativeRead = new QPushButton(tr("Read native step context"), group); m_relativeRead->setObjectName("nativeRelativeRead");
    m_relativeCancel = new QPushButton(tr("Cancel native read"), group); m_relativeCancel->setObjectName("nativeRelativeCancel");
    form->addRow(m_relativeRead); form->addRow(m_relativeCancel);
    m_relativeFcDirection = new GuardedComboBox(group); m_relativeFcDirection->setObjectName("nativeRelativeFcDirection");
    m_relativeUdDirection = new GuardedComboBox(group); m_relativeUdDirection->setObjectName("nativeRelativeUdDirection");
    for (QComboBox* direction : {m_relativeFcDirection, m_relativeUdDirection}) {
        direction->addItems({tr("Up"), tr("Down")}); direction->setCurrentIndex(-1);
    }
    m_relativeFcDirection->setAccessibleName(tr("Native multiplier step direction"));
    m_relativeUdDirection->setAccessibleName(tr("Native counted step direction"));
    m_relativeMultiplier = new GuardedComboBox(group); m_relativeMultiplier->setObjectName("nativeRelativeMultiplier");
    m_relativeMultiplier->addItems({tr("×1"), tr("×2"), tr("×5"), tr("×10"), tr("×50"), tr("×100")}); m_relativeMultiplier->setCurrentIndex(-1);
    m_relativeMultiplier->setAccessibleName(tr("Native tuning step multiplier"));
    m_relativeFc = new QPushButton(tr("Apply multiplier step"), group); m_relativeFc->setObjectName("nativeRelativeFcApply");
    form->addRow(tr("Multiplier direction:"), m_relativeFcDirection); form->addRow(tr("Native step multiplier:"), m_relativeMultiplier); form->addRow(m_relativeFc);
    m_relativeSingle = new QCheckBox(tr("Use radio single-step form"), group); m_relativeSingle->setObjectName("nativeRelativeSingle"); m_relativeSingle->setChecked(true);
    m_relativeSteps = new QSpinBox(group); m_relativeSteps->setObjectName("nativeRelativeSteps"); m_relativeSteps->setRange(0, 99); m_relativeSteps->setValue(1);
    m_relativeSteps->setAccessibleName(tr("Number of native tuning steps"));
    m_relativeUd = new QPushButton(tr("Apply counted steps"), group); m_relativeUd->setObjectName("nativeRelativeUdApply");
    form->addRow(tr("Counted direction:"), m_relativeUdDirection); form->addRow(m_relativeSingle); form->addRow(tr("Native steps (0–99):"), m_relativeSteps); form->addRow(m_relativeUd);
    m_relativeReason = new QLabel(group); m_relativeReason->setObjectName("nativeRelativeReason"); m_relativeReason->setWordWrap(true); m_relativeReason->setTextFormat(Qt::PlainText); form->addRow(m_relativeReason);
    for (QPushButton* button : {m_relativeRead, m_relativeCancel, m_relativeFc, m_relativeUd}) { button->setAutoDefault(false); }
    const auto stage = [this] { m_relativeDraft = m_relativeConfirmed.value("revision").toString(); renderRelative(); };
    for (QComboBox* box : {m_relativeFcDirection, m_relativeUdDirection, m_relativeMultiplier}) { connect(box, &QComboBox::activated, this, stage); }
    connect(m_relativeSingle, &QCheckBox::clicked, this, stage);
    connect(m_relativeSteps, &QSpinBox::valueChanged, this, stage);
    connect(m_relativeRead, &QPushButton::clicked, this, [this] { invokeRelative("tuning.relative.read"); });
    connect(m_relativeCancel, &QPushButton::clicked, this, [this] { invokeRelative("tuning.relative.cancel"); });
    connect(m_relativeFc, &QPushButton::clicked, this, [this] { invokeRelative("tuning.relative.fc"); });
    connect(m_relativeUd, &QPushButton::clicked, this, [this] { invokeRelative("tuning.relative.ud"); });
    layout->addWidget(group); renderRelative();
}

void NativeVfoSettingsWidget::bindRelative()
{
    IRadioBackend* backend = m_model.isConnected() ? m_model.backend() : nullptr;
    QString ns;
    const RadioCapabilities caps = m_model.backendCapabilities();
    for (const QString& name : caps.extensionNamespaces) {
        const QVariant version = caps.extensions.value(name).toMap().value("nativeRelativeTuningVersion");
        if (version.typeId() == QMetaType::Int && version.toInt() == 1) { ns = name; break; }
    }
    if (backend == m_relativeBackend && ns == m_relativeNamespace && m_relativeSession == m_model.receiveControlSession()) { renderRelative(); return; }
    for (const QMetaObject::Connection& connection : m_relativeConnections) { disconnect(connection); }
    m_relativeBackend = backend; m_relativeNamespace = ns; m_relativeSession = m_model.receiveControlSession();
    const quint64 generation = ++m_relativeGeneration;
    m_relativeConfirmed.clear(); m_relativeRequest = m_relativeRevision = 0; m_relativeDraft.clear();
    const QPointer<IRadioBackend> source(backend); const RadioControlSession session = m_model.controlSession();
    const QPointer<NativeVfoSettingsWidget> alive(this); renderRelative();
    if (!alive || generation != m_relativeGeneration || !source || source != m_relativeBackend
        || source != m_model.backend() || !m_model.isConnected() || !m_model.controlSessionCurrent(session) || ns.isEmpty()) { return; }
    const auto current = [this, source, generation, session] { return generation == m_relativeGeneration && source && source == m_relativeBackend
        && source == m_model.backend() && m_model.isConnected() && m_model.controlSessionCurrent(session); };
    m_relativeConnections[0] = connect(source, &IRadioBackend::extensionStatus, this,
        [this, ns, current](const QString& space, const QString& kind, const QVariantMap& status) {
            if (current() && space == ns && kind == "relativeTuning") { acceptRelativeStatus(status); }
        });
    m_relativeConnections[1] = connect(source, &IRadioBackend::extensionResult, this,
        [this, current](quint64 id, const QVariant& result) {
            if (!current() || !id || id != m_relativeRequest) { return; }
            m_relativeRequest = 0;
            if (result.typeId() == QMetaType::QVariantMap && result.toMap().contains("schemaVersion")) { acceptRelativeStatus(result.toMap()); }
            else { renderRelative(); }
        });
    m_relativeConnections[2] = connect(source, &IRadioBackend::extensionError, this,
        [this, current](quint64 id, const QString&) { if (current() && id && id == m_relativeRequest) { m_relativeRequest = 0; renderRelative(); } });
    invokeRelative("tuning.relative.get");
}

void NativeVfoSettingsWidget::acceptRelativeStatus(const QVariantMap& status)
{
    const QSet<QString> expected{"schemaVersion", "statusRevision", "context", "revision", "connected", "editable", "receiveVfo", "frequencyHz", "reason", "read", "lastAction"};
    QSet<QString> keys; for (auto it = status.cbegin(); it != status.cend(); ++it) { keys.insert(it.key()); }
    quint64 revision = 0, semantic = 0;
    bool valid = keys == expected && status.value("schemaVersion").typeId() == QMetaType::Int && status.value("schemaVersion").toInt() == 1
        && revisionValid(status.value("statusRevision"), revision) && revisionValid(status.value("revision"), semantic)
        && textValid(status.value("context"), 128) && textValid(status.value("reason"), 2048, true)
        && status.value("connected").typeId() == QMetaType::Bool && status.value("editable").typeId() == QMetaType::Bool
        && status.value("receiveVfo").typeId() == QMetaType::Int && status.value("receiveVfo").toInt() >= -1 && status.value("receiveVfo").toInt() <= 1
        && status.value("frequencyHz").typeId() == QMetaType::LongLong && status.value("frequencyHz").toLongLong() >= 0
        && status.value("read").typeId() == QMetaType::QVariantMap && status.value("lastAction").typeId() == QMetaType::QVariantMap;
    const QVariantMap read = status.value("read").toMap();
    QSet<QString> readKeys; for (auto it = read.cbegin(); it != read.cend(); ++it) { readKeys.insert(it.key()); }
    quint64 token = 0;
    valid = valid && readKeys == QSet<QString>{"token", "busy", "pending", "completed", "total", "reason"}
        && textValid(read.value("token"), 20, true) && (read.value("token").toString().isEmpty() || revisionValid(read.value("token"), token))
        && read.value("busy").typeId() == QMetaType::Bool && textValid(read.value("pending"), 3, true)
        && QStringList{"", "FR", "FT", "TS", "XV", "MV", "QR", "FA", "FB", "SF0", "SF1", "FS", "MH", "LK"}.contains(read.value("pending").toString())
        && (!read.value("busy").toBool() || (token > 0 && !read.value("pending").toString().isEmpty()))
        && (read.value("busy").toBool() || read.value("pending").toString().isEmpty()) && textValid(read.value("reason"), 2048, true)
        && read.value("completed").typeId() == QMetaType::Int && read.value("total").typeId() == QMetaType::Int
        && read.value("completed").toInt() >= 0 && read.value("completed").toInt() <= read.value("total").toInt()
        && (read.value("total").toInt() == 0 || read.value("total").toInt() == 13)
        && (!read.value("busy").toBool() || (read.value("total").toInt() == 13 && read.value("completed").toInt() < 13));
    const QVariantMap action = status.value("lastAction").toMap();
    if (!action.isEmpty()) { valid = valid && action.size() == 2 && textValid(action.value("reason"), 2048, true)
        && QStringList{"pending", "rejected", "indeterminate", "sent_unverified", "observed"}.contains(action.value("state").toString()) && action.value("state").typeId() == QMetaType::QString; }
    valid = valid && (!status.value("editable").toBool() || (status.value("connected").toBool() && !read.value("busy").toBool()
        && status.value("receiveVfo").toInt() >= 0 && status.value("frequencyHz").toLongLong() > 0));
    if (!valid) { m_relativeConfirmed.clear(); renderRelative(); return; }
    if (revision < m_relativeRevision) { return; }
    m_relativeRevision = revision; m_relativeConfirmed = status; renderRelative();
}

void NativeVfoSettingsWidget::renderRelative()
{
    const QPointer<NativeVfoSettingsWidget> alive(this); const quint64 generation = m_relativeGeneration;
    const quint64 serial = ++m_relativeRender;
    const auto current = [alive, generation, serial] { return alive && generation == alive->m_relativeGeneration && serial == alive->m_relativeRender; };
    const bool connected = m_relativeBackend && m_model.isConnected() && !m_relativeNamespace.isEmpty();
    const QVariantMap read = m_relativeConfirmed.value("read").toMap();
    const bool busy = read.value("busy").toBool();
    const bool editable = connected && !m_relativeRequest && !busy && m_relativeConfirmed.value("editable").toBool();
    const bool staged = !m_relativeDraft.isEmpty() && m_relativeDraft == m_relativeConfirmed.value("revision").toString();
    const QList<QPair<QWidget*, bool>> controls{{m_relativeRead, connected && !busy && !m_relativeRequest}, {m_relativeCancel, connected && busy && !m_relativeRequest},
        {m_relativeFcDirection, editable}, {m_relativeUdDirection, editable}, {m_relativeMultiplier, editable}, {m_relativeSingle, editable},
        {m_relativeSteps, editable && !m_relativeSingle->isChecked()},
        {m_relativeFc, editable && staged && m_relativeFcDirection->currentIndex() >= 0 && m_relativeMultiplier->currentIndex() >= 0},
        {m_relativeUd, editable && staged && m_relativeUdDirection->currentIndex() >= 0}};
    QString reason = m_relativeConfirmed.value("reason").toString();
    if (!connected) { reason = tr("Requires connected native relative tuning support."); }
    else if (busy) { reason = read.value("reason").toString(); }
    else if (reason.isEmpty()) { reason = staged ? tr("Apply the selected native step. The radio determines the resulting frequency.") : tr("Choose the native step parameters before Apply."); }
    const QVariantMap action = m_relativeConfirmed.value("lastAction").toMap();
    if (!action.isEmpty()) { reason += tr(" Last action: %1. %2").arg(action.value("state").toString(), action.value("reason").toString()); }
    for (const auto& control : controls) {
        control.first->setEnabled(control.second); if (!current()) { return; }
        control.first->setToolTip(reason); if (!current()) { return; }
        control.first->setAccessibleDescription(reason); if (!current()) { return; }
    }
    const int target = m_relativeConfirmed.value("receiveVfo", -1).toInt();
    const qint64 frequency = m_relativeConfirmed.value("frequencyHz").toLongLong();
    const QString observed = target < 0 ? tr("Receive VFO: Unknown") : frequency <= 0
        ? tr("Receive VFO %1: Frequency unknown").arg(target ? "B" : "A")
        : tr("Raw native VFO %1 register: %2 Hz").arg(target ? "B" : "A").arg(frequency);
    for (const auto& item : QList<QPair<QLabel*, QString>>{{m_relativeObserved, observed}, {m_relativeReason, reason}}) {
        if (item.first->text() != item.second) {
            item.first->setText(item.second); if (!current()) { return; }
            QAccessibleEvent event(item.first, QAccessible::NameChanged); QAccessible::updateAccessibility(&event); if (!current()) { return; }
        }
    }
}

void NativeVfoSettingsWidget::invokeRelative(const QString& verb)
{
    if (!m_relativeBackend || m_relativeBackend != m_model.backend() || !m_model.isConnected() || m_relativeNamespace.isEmpty() || m_relativeRequest) { return; }
    QVariant argument;
    if (verb == "tuning.relative.fc" || verb == "tuning.relative.ud") {
        if ((verb.endsWith("fc") && !m_relativeFc->isEnabled()) || (verb.endsWith("ud") && !m_relativeUd->isEnabled())) { return; }
        QVariantMap args{{"context", m_relativeConfirmed.value("context")}, {"revision", m_relativeDraft}};
        if (verb.endsWith("fc")) { args.insert("direction", m_relativeFcDirection->currentIndex()); args.insert("multiplier", m_relativeMultiplier->currentIndex()); }
        else { args.insert("direction", m_relativeUdDirection->currentIndex()); args.insert("target", m_relativeConfirmed.value("receiveVfo")); if (!m_relativeSingle->isChecked()) { args.insert("steps", m_relativeSteps->value()); } }
        argument = args;
    } else if (verb.endsWith("cancel")) { argument = QVariantMap{{"token", m_relativeConfirmed.value("read").toMap().value("token")}}; }
    const quint64 request = nextRequest(); m_relativeRequest = request;
    const quint64 generation = m_relativeGeneration; const QString ns = m_relativeNamespace;
    const QPointer<NativeVfoSettingsWidget> alive(this); renderRelative();
    if (alive && generation == m_relativeGeneration && request == m_relativeRequest && m_relativeBackend == m_model.backend() && m_model.isConnected()) {
        m_model.invokeBackendExtension(ns, verb, request, argument);
    }
}
} // namespace AetherSDR
