#include "NetworkAudioLevelsWidget.h"

#include "ControlAvailabilityRegistry.h"
#include "models/RadioModel.h"

#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QMetaType>
#include <QPushButton>
#include <QSpinBox>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include <cmath>

namespace AetherSDR {
namespace {
bool statusRevisionValid(const QVariant& value, quint64& revision)
{
    if (value.typeId() != QMetaType::QString || value.toString().size() > 20) { return false; }
    bool valid = false;
    revision = value.toString().toULongLong(&valid);
    return valid && QString::number(revision) == value.toString();
}

QString levelNamespace(const RadioCapabilities& caps)
{
    for (const QString& ns : caps.extensionNamespaces) {
        const QVariant version = caps.extensions.value(ns).toMap().value(QStringLiteral("networkAudioLevelsVersion"));
        if (version.typeId() == QMetaType::Int && version.toInt() == 1) { return ns; }
    }
    return {};
}
QString levelKey(int direction)
{
    return direction == 0 ? QStringLiteral("inputLevel") : QStringLiteral("outputLevel");
}
quint64 nextLevelRequest()
{
    static quint64 sequence = 0x4e414c0000000000ULL;
    return ++sequence;
}
}

NetworkAudioLevelsWidget::NetworkAudioLevelsWidget(RadioModel& model, QWidget* parent)
    : QWidget(parent), m_model(model), m_availability(new ControlAvailabilityRegistry(model, this)),
      m_levels{new QSpinBox(this), new QSpinBox(this)}, m_readback(new QLabel(this)), m_status(new QLabel(this))
{
    setObjectName(QStringLiteral("networkAudioLevels"));
    auto* layout = new QVBoxLayout(this);
    auto* explanation = new QLabel(tr("Use your computer's microphone and speaker gain first. These are the radio's own VoIP levels, "
        "separate from microphone gain. Enter a requested level, then select Apply. Writes require an administrator login and wait for radio readback."), this);
    explanation->setWordWrap(true);
    layout->addWidget(explanation);
    auto* group = new QGroupBox(tr("Radio VoIP levels"), this);
    auto* form = new QFormLayout(group);
    layout->addWidget(group);
    const QString reason = tr("Requires a connected radio with network audio level control, administrator write permission, and a confirmed level.");
    for (int direction = 0; direction < 2; ++direction) {
        QSpinBox* level = m_levels[direction];
        const bool input = direction == 0;
        level->setObjectName(input ? QStringLiteral("networkAudioInputLevel") : QStringLiteral("networkAudioOutputLevel"));
        level->setAccessibleName(input ? tr("Requested VoIP input level") : tr("Requested VoIP output level"));
        level->setAccessibleDescription(tr("Native radio level, 0 to 100. Enter a value and select Apply. Confirmed levels appear below."));
        level->setRange(0, 100);
        level->setKeyboardTracking(false);
        auto* apply = new QPushButton(input ? tr("Apply input level") : tr("Apply output level"), group);
        apply->setObjectName(input ? QStringLiteral("networkAudioInputApply") : QStringLiteral("networkAudioOutputApply"));
        apply->setAutoDefault(false);
        form->addRow(input ? tr("VoIP input (to radio):") : tr("VoIP output (from radio):"), level);
        form->addRow(apply);
        const auto available = [this, direction](bool connected, const RadioCapabilities& caps) {
            return connected && !levelNamespace(caps).isEmpty()
                && m_confirmed.value(QStringLiteral("writable")).toBool()
                && m_confirmed.contains(levelKey(direction));
        };
        m_availability->registerWidget(level, reason, available, {}, false);
        m_availability->registerWidget(apply, reason, available, {}, false);
        connect(apply, &QPushButton::clicked, this, [this, direction] { submit(direction); });
    }
    m_readback->setObjectName(QStringLiteral("networkAudioLevelReadback"));
    m_readback->setWordWrap(true);
    m_status->setObjectName(QStringLiteral("networkAudioLevelStatus"));
    m_status->setWordWrap(true);
    layout->addWidget(m_readback);
    layout->addWidget(m_status);
    layout->addStretch();
    connect(&model, &RadioModel::connectionStateChanged, this, &NetworkAudioLevelsWidget::bindBackend);
    connect(&model, &RadioModel::capabilitiesChanged, this, &NetworkAudioLevelsWidget::bindBackend);
    bindBackend();
}

void NetworkAudioLevelsWidget::bindBackend()
{
    const QString ns = m_model.isConnected() ? levelNamespace(m_model.backendCapabilities()) : QString{};
    IRadioBackend* backend = m_model.isConnected() ? m_model.backend() : nullptr;
    if (backend == m_backend && ns == m_namespace
        && m_receiveSession == m_model.receiveControlSession()) { render(); return; }
    const quint64 generation = ++m_bindingGeneration;
    m_receiveSession = m_model.receiveControlSession();
    const RadioControlSession session = m_model.controlSession();
    for (const QMetaObject::Connection& connection : m_connections) { disconnect(connection); }
    m_backend = backend;
    m_namespace = ns;
    m_query = m_edit = 0;
    m_statusRevision = 0;
    ++m_statusSerial;
    m_seeded = {false, false};
    m_confirmed.clear();
    const QPointer<NetworkAudioLevelsWidget> alive(this);
    m_status->clear();
    if (!alive || generation != m_bindingGeneration) { return; }
    render();
    if (!alive || generation != m_bindingGeneration || !backend || ns.isEmpty()) { return; }
    const QPointer<IRadioBackend> source = backend;
    const auto current = [this, source, generation, session] {
        return generation == m_bindingGeneration && source && source == m_backend
            && source == m_model.backend() && m_model.isConnected() && m_model.controlSessionCurrent(session);
    };
    m_connections[0] = connect(backend, &IRadioBackend::extensionStatus, this,
        [this, current, ns](const QString& space, const QString& kind, const QVariantMap& status) {
            if (current() && space == ns && kind == QLatin1String("audio.levels")) { acceptStatus(status); }
        });
    m_connections[1] = connect(backend, &IRadioBackend::extensionResult, this,
        [this, current](quint64 id, const QVariant& result) {
            if (!current()) { return; }
            if (id && id == m_query) {
                m_query = 0;
                if (m_getSerial == m_statusSerial) { acceptStatus(result.toMap()); } else { render(); }
            }
            else if (id && id == m_edit) { m_edit = 0; m_status->setText(tr("Request sent. Confirmed levels are shown below.")); }
        });
    m_connections[2] = connect(backend, &IRadioBackend::extensionError, this,
        [this, current](quint64 id, const QString& error) {
            if (current() && id && (id == m_query || id == m_edit)) { m_status->setText(error); }
        });
    m_query = nextLevelRequest();
    m_getSerial = m_statusSerial;
    m_model.invokeBackendExtension(ns, QStringLiteral("audio.levels.get"), m_query);
}

void NetworkAudioLevelsWidget::acceptStatus(const QVariantMap& status)
{
    quint64 revision = 0;
    const bool ordered = statusRevisionValid(status.value(QStringLiteral("statusRevision")), revision);
    if (ordered && revision < m_statusRevision) { return; }
    ++m_statusSerial;
    const QString context = status.value(QStringLiteral("context")).toString();
    bool valid = ordered && status.value(QStringLiteral("context")).typeId() == QMetaType::QString
        && !context.isEmpty() && context.size() <= 128
        && status.value(QStringLiteral("writable")).typeId() == QMetaType::Bool;
    for (int direction = 0; direction < 2; ++direction) {
        const QString key = levelKey(direction);
        if (!status.contains(key)) { continue; }
        const int type = status.value(key).typeId();
        const bool number = type == QMetaType::Int || type == QMetaType::UInt || type == QMetaType::LongLong
            || type == QMetaType::ULongLong || type == QMetaType::Double || type == QMetaType::Float;
        bool numeric = false;
        const double level = status.value(key).toDouble(&numeric);
        valid = valid && number && numeric && std::isfinite(level) && std::trunc(level) == level && level >= 0 && level <= 100;
    }
    if (valid) { m_statusRevision = revision; }
    m_confirmed = valid ? status : QVariantMap{};
    const QPointer<NetworkAudioLevelsWidget> alive(this);
    const quint64 generation = m_bindingGeneration, serial = m_statusSerial;
    if (valid) {
        for (int direction = 0; direction < 2; ++direction) {
            if (!m_seeded[direction] && status.contains(levelKey(direction))) {
                QSignalBlocker blocker(m_levels[direction]);
                m_levels[direction]->setValue(status.value(levelKey(direction)).toInt());
                if (!alive) { blocker.dismiss(); return; }
                if (generation != m_bindingGeneration || serial != m_statusSerial) { return; }
                m_seeded[direction] = true;
            }
        }
    }
    m_status->setText(valid ? QString{} : tr("The radio supplied invalid network audio levels."));
    if (!alive || generation != m_bindingGeneration || serial != m_statusSerial) { return; }
    render();
}

void NetworkAudioLevelsWidget::render()
{
    const QPointer<NetworkAudioLevelsWidget> alive(this);
    const quint64 generation = m_bindingGeneration, serial = m_statusSerial;
    const auto confirmed = [this](int direction) {
        const QString key = levelKey(direction);
        return m_confirmed.contains(key) ? QString::number(m_confirmed.value(key).toInt()) : tr("Unknown");
    };
    m_readback->setText(tr("Confirmed input: %1 · Output: %2").arg(confirmed(0), confirmed(1)));
    if (!alive || generation != m_bindingGeneration || serial != m_statusSerial) { return; }
    m_availability->refreshEngaged();
}

void NetworkAudioLevelsWidget::submit(int direction)
{
    if (!m_levels[direction]->isEnabled() || !m_backend || m_backend != m_model.backend()
        || !m_model.isConnected() || m_namespace.isEmpty()) { return; }
    m_edit = nextLevelRequest();
    m_model.invokeBackendExtension(m_namespace, QStringLiteral("audio.levels.set"), m_edit,
        QVariantMap{{QStringLiteral("direction"), direction == 0 ? QStringLiteral("input") : QStringLiteral("output")},
            {QStringLiteral("level"), m_levels[direction]->value()}, {QStringLiteral("context"), m_confirmed.value(QStringLiteral("context"))}});
}
} // namespace AetherSDR
