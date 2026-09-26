#include "RtlReceiverSettingsWidget.h"
#include "ControlAvailabilityRegistry.h"
#include "models/RadioModel.h"

#include <QCheckBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>

namespace AetherSDR {
namespace {
quint64 nextRequest()
{
    // All calls run on the GUI thread. Keep IDs distinct across page lifetimes
    // and from the small counters used by other extension clients.
    static quint64 sequence = 0x52544c0000000000ULL;
    return ++sequence;
}
bool supportsSettings(const RadioCapabilities& caps)
{
    return caps.extensionNamespaces.contains(QStringLiteral("rtl"))
        && caps.extensions.value(QStringLiteral("rtl")).toMap()
            .value(QStringLiteral("settingsVersion")).toInt() == 1;
}
}

RtlReceiverSettingsWidget::RtlReceiverSettingsWidget(RadioModel& model, QWidget* parent)
    : QWidget(parent), m_model(model),
      m_availability(new ControlAvailabilityRegistry(model, this)),
      m_controls(new QGroupBox(tr("Receiver corrections"), this)),
      m_ppm(new QSpinBox(m_controls)), m_dc(new QCheckBox(tr("Suppress IQ DC"), m_controls)),
      m_applied(new QLabel(this)), m_status(new QLabel(this)), m_identity(new QLabel(this))
{
    setObjectName(QStringLiteral("rtlReceiverSettings"));
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(m_identity);
    auto* form = new QFormLayout(m_controls);
    m_ppm->setObjectName(QStringLiteral("rtlPpmRequest"));
    m_ppm->setAccessibleName(tr("Requested frequency correction in parts per million"));
    m_ppm->setRange(-1000, 1000);
    m_ppm->setSuffix(tr(" ppm"));
    m_ppm->setKeyboardTracking(false);
    auto* apply = new QPushButton(tr("Apply frequency correction"), m_controls);
    apply->setObjectName(QStringLiteral("rtlPpmApply"));
    form->addRow(tr("Frequency correction:"), m_ppm);
    form->addRow(apply);
    m_dc->setObjectName(QStringLiteral("rtlDcSuppression"));
    form->addRow(m_dc);
    layout->addWidget(m_controls);
    m_applied->setObjectName(QStringLiteral("rtlCorrectionsApplied"));
    m_status->setObjectName(QStringLiteral("rtlCorrectionsStatus"));
    for (QLabel* label : {m_identity, m_applied, m_status}) {
        label->setWordWrap(true);
        label->setTextFormat(Qt::PlainText);
    }
    layout->addWidget(m_applied);
    layout->addWidget(m_status);
    auto* explanation = new QLabel(tr(
        "PPM uses whole numbers. A positive value corrects a fast device clock. "
        "If a known reference appears too low, increase the correction; if it appears too high, decrease it. "
        "Allow the receiver to warm up and use a known stable reference.\n\n"
        "IQ DC suppression is off by default. It removes constant offsets from the received samples "
        "with a 5 Hz high-pass response. Signals at capture center are also affected, especially an AM or CW carrier. "
        "Keep wanted signals away from capture center. After a capture change or enable, allow about 0.3 seconds to settle. "
        "The squelch measurement stays unchanged.\n\n"
        "Accepted corrections are saved per reported device serial. Dongles reporting the same serial share settings; "
        "devices without a serial use corrections for this session only."), this);
    explanation->setWordWrap(true);
    layout->addWidget(explanation);
    layout->addStretch();
    const auto available = [](bool connected, const RadioCapabilities& caps) {
        return connected && supportsSettings(caps);
    };
    for (QWidget* control : {static_cast<QWidget*>(m_ppm), static_cast<QWidget*>(apply), static_cast<QWidget*>(m_dc)}) {
        m_availability->registerWidget(control,
            tr("The connected radio does not provide RTL receiver corrections."), available);
    }
    connect(m_ppm, &QSpinBox::valueChanged, this, [this] { m_ppmEdited = true; });
    connect(apply, &QPushButton::clicked, this, [this] {
        submit(QStringLiteral("ppm.set"), m_ppm->value(), m_ppmRequest);
    });
    connect(m_dc, &QCheckBox::clicked, this, [this](bool requested) {
        const QSignalBlocker block(m_dc);
        m_dc->setChecked(m_confirmed.value(QStringLiteral("dcSuppression")).toBool());
        submit(QStringLiteral("dc_suppression.set"), requested, m_dcRequest);
    });
    connect(&model, &RadioModel::connectionStateChanged, this, [this] { bindBackend(); });
    connect(&model, &RadioModel::capabilitiesChanged, this, [this] {
        if (m_backend != m_model.backend() || !m_haveState
            || !supportsSettings(m_model.backendCapabilities())) { bindBackend(); }
    });
    bindBackend();
}

void RtlReceiverSettingsWidget::bindBackend()
{
    disconnect(m_statusConnection); disconnect(m_resultConnection); disconnect(m_errorConnection);
    m_backend = m_model.backend();
    const quint64 generation = ++m_bindingGeneration;
    m_queryRequest = m_ppmRequest = m_dcRequest = 0;
    m_haveState = false; m_ppmEdited = false;
    m_confirmed.clear(); m_error.clear();
    {
        const QSignalBlocker ppmBlock(m_ppm), dcBlock(m_dc);
        m_ppm->setValue(0); m_dc->setChecked(false);
    }
    renderStatus();
    if (!m_model.isConnected() || !m_backend || !supportsSettings(m_model.backendCapabilities())) { return; }
    const QPointer<IRadioBackend> source = m_backend;
    m_statusConnection = connect(source, &IRadioBackend::extensionStatus, this,
        [this, source, generation](const QString& ns, const QString& kind, const QVariantMap& status) {
            if (generation == m_bindingGeneration && source && source == m_model.backend() && ns == QLatin1String("rtl") && kind == QLatin1String("settings")) {
                acceptStatus(status);
            }
        });
    m_resultConnection = connect(source, &IRadioBackend::extensionResult, this,
        [this, source, generation](quint64 id, const QVariant& value) {
            if (generation != m_bindingGeneration || !source || source != m_model.backend()) { return; }
            if (id == m_queryRequest && id) {
                m_queryRequest = 0; acceptStatus(value.toMap());
            } else if (id && (id == m_ppmRequest || id == m_dcRequest)) {
                if (id == m_ppmRequest) {
                    m_ppmRequest = 0;
                    // Preserve a newer edit that the operator has not submitted.
                    m_ppmEdited = m_ppm->value() != value.toInt();
                }
                if (id == m_dcRequest) { m_dcRequest = 0; }
                queryStatus();
            }
        });
    m_errorConnection = connect(source, &IRadioBackend::extensionError, this,
        [this, source, generation](quint64 id, const QString& reason) {
            if (generation != m_bindingGeneration || !source || source != m_model.backend() || !id
                || (id != m_queryRequest && id != m_ppmRequest && id != m_dcRequest)) { return; }
            const bool queryFailed = id == m_queryRequest;
            if (queryFailed) { m_queryRequest = 0; }
            if (id == m_ppmRequest) { m_ppmRequest = 0; }
            if (id == m_dcRequest) { m_dcRequest = 0; }
            m_error = tr("Request refused: %1").arg(reason);
            if (!queryFailed) { queryStatus(); }
            renderStatus();
        });
    queryStatus();
}

void RtlReceiverSettingsWidget::queryStatus()
{
    if (!m_model.isConnected() || m_backend != m_model.backend()
        || !supportsSettings(m_model.backendCapabilities())) { return; }
    m_queryRequest = nextRequest();
    m_model.invokeBackendExtension(QStringLiteral("rtl"), QStringLiteral("settings.get"), m_queryRequest);
}

void RtlReceiverSettingsWidget::acceptStatus(const QVariantMap& status)
{
    if (!status.value(QStringLiteral("applied")).toBool()) { return; }
    m_haveState = true;
    m_confirmed = status;
    const QSignalBlocker ppmBlock(m_ppm), dcBlock(m_dc);
    if (!m_ppmEdited) { m_ppm->setValue(status.value(QStringLiteral("ppm")).toInt()); }
    m_dc->setChecked(status.value(QStringLiteral("dcSuppression")).toBool());
    renderStatus();
}

void RtlReceiverSettingsWidget::submit(const QString& verb, const QVariant& value, quint64& pending)
{
    if (!m_haveState || !m_model.isConnected() || m_backend != m_model.backend()
        || !supportsSettings(m_model.backendCapabilities())) { return; }
    pending = nextRequest(); m_error.clear();
    renderStatus();
    m_model.invokeBackendExtension(QStringLiteral("rtl"), verb, pending, value);
}

void RtlReceiverSettingsWidget::renderStatus()
{
    const bool usable = m_haveState && m_model.isConnected();
    m_controls->setEnabled(usable);
    m_controls->setAccessibleDescription(usable ? QString() : tr("Connect a supported RTL receiver and wait for confirmed settings."));
    m_identity->setText(tr("Device serial: %1").arg(m_confirmed.value(QStringLiteral("serial"), tr("unavailable")).toString()));
    m_applied->setText(usable ? tr("Applied: %1 ppm; IQ DC suppression %2")
        .arg(m_confirmed.value(QStringLiteral("ppm")).toInt())
        .arg(m_confirmed.value(QStringLiteral("dcSuppression")).toBool() ? tr("on") : tr("off"))
        : tr("Applied corrections unavailable."));
    QStringList lines;
    if (!usable) { lines << tr("Waiting for a connected receiver's confirmed settings."); }
    else {
        if (m_ppmRequest || m_dcRequest || m_confirmed.value(QStringLiteral("pending")).toBool()) {
            lines << tr("Applying request; the applied values above remain in use until confirmation.");
        }
        lines << (m_confirmed.value(QStringLiteral("saved")).toBool()
            ? tr("Applied corrections saved for this device.")
            : m_confirmed.value(QStringLiteral("saveReason"), tr("Applied for this session; not saved.")).toString());
    }
    if (!m_error.isEmpty()) { lines << m_error; }
    m_status->setText(lines.join(QLatin1Char('\n')));
    m_status->setAccessibleDescription(m_status->text());
}
} // namespace AetherSDR
