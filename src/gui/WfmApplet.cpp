#include "WfmApplet.h"
#include "ComboStyle.h"
#include "ControlAvailabilityRegistry.h"
#include "GuardedSlider.h"
#include "ModeFilterPresets.h"
#include "WfmLockScope.h"
#include "core/AppSettings.h"
#include "core/ThemeManager.h"
#include "models/RadioModel.h"
#include "models/PanadapterModel.h"
#include "models/SliceModel.h"

#include <QAccessible>
#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QKeyEvent>
#include <QShowEvent>
#include <optional>
#include <QSignalBlocker>
#include <QVBoxLayout>

namespace AetherSDR {
namespace {
QJsonObject preferences()
{
    return QJsonDocument::fromJson(AppSettings::instance()
        .value(QStringLiteral("WfmApplet"), QString{}).toString().toUtf8()).object();
}

// A popup may outlive its receiver binding. Preserve the binding witness
// after hidePopup(): Qt can deliver activated later.
class WfmReceiverComboBox final : public GuardedComboBox {
public:
    using GuardedComboBox::GuardedComboBox;
    void setBinding(quint64 binding)
    {
        m_binding = binding;
        invalidateInteraction();
    }
    void invalidateInteraction()
    {
        ++m_generation;
        hidePopup();
    }
    quint64 takeActivationBinding()
    {
        const Origin origin = m_origin.value_or(Origin{m_binding, m_generation});
        m_origin.reset();
        // Bound controls use nonzero slice generations. Zero cannot authorize
        // an activation whose receiver binding was retired.
        return origin.generation == m_generation ? origin.binding : 0;
    }
    void showPopup() override
    {
        m_origin = Origin{m_binding, m_generation};
        GuardedComboBox::showPopup();
    }
protected:
    void keyPressEvent(QKeyEvent* event) override
    {
        if (!view()->isVisible()) { m_origin = Origin{m_binding, m_generation}; }
        GuardedComboBox::keyPressEvent(event);
    }
    void wheelEvent(QWheelEvent* event) override
    {
        if (!view()->isVisible()) { m_origin = Origin{m_binding, m_generation}; }
        GuardedComboBox::wheelEvent(event);
    }
private:
    struct Origin {
        quint64 binding;
        quint64 generation;
    };
    quint64 m_binding{0};
    quint64 m_generation{0};
    std::optional<Origin> m_origin;
};

void setReadout(QLabel* label, const QString& text, const QString& name)
{
    if (label->text() == text && label->accessibleName() == name) { return; }
    label->setText(text);
    label->setAccessibleName(name);
    QAccessibleEvent event(label, QAccessible::NameChanged);
    QAccessible::updateAccessibility(&event);
}

void announceCombo(QComboBox* combo, const QString& previous)
{
    if (previous != combo->currentText()) {
        QAccessibleValueChangeEvent event(combo, combo->currentText());
        QAccessible::updateAccessibility(&event);
    }
}

void styleButton(QPushButton* button)
{
    ThemeManager::instance().applyStyleSheet(button,
        "QPushButton { background: {{color.background.1}};"
        " border: 1px solid {{color.border.subtle}}; border-radius: 3px;"
        " color: {{color.text.primary}}; font-size: 10px; font-weight: bold; padding: 2px 4px; }"
        "QPushButton:hover { background: {{color.background.2}}; }"
        "QPushButton:disabled { color: {{color.control.unavailable}}; }");
}
} // namespace

WfmApplet::WfmApplet(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("WfmApplet"));
    setAccessibleName(tr("Broadcast FM"));
    theme::setContainer(this, QStringLiteral("applet/wfm"));
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(2, 2, 2, 2);
    root->setSpacing(3);
    m_identity = new QLabel(this);
    m_identity->setObjectName(QStringLiteral("wfmReceiverIdentity"));
    m_identity->setTextFormat(Qt::PlainText);
    m_identity->setWordWrap(true);
    ThemeManager::instance().applyStyleSheet(m_identity,
        "QLabel { color: {{color.text.primary}}; font-size: 11px; }");
    root->addWidget(m_identity);
    m_scope = new WfmLockScope(this);
    m_scope->setObjectName(QStringLiteral("wfmLockScope"));
    root->addWidget(m_scope);

    auto* statusRow = new QHBoxLayout;
    statusRow->setSpacing(5);
    m_audioMode = new QPushButton(this);
    m_audioMode->setObjectName(QStringLiteral("wfmAudioMode"));
    m_audioMode->setFocusPolicy(Qt::StrongFocus);
    m_audioMode->setMinimumHeight(22);
    styleButton(m_audioMode);
    statusRow->addWidget(m_audioMode);
    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("wfmStereoStatus"));
    m_status->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    ThemeManager::instance().applyStyleSheet(m_status,
        "QLabel { color: {{color.text.secondary}}; font-size: 11px; }");
    statusRow->addWidget(m_status, 1);
    root->addLayout(statusRow);

    auto* settings = new QGridLayout;
    settings->setHorizontalSpacing(5);
    settings->setVerticalSpacing(3);
    m_deemphasis = new WfmReceiverComboBox(this);
    m_deemphasis->setObjectName(QStringLiteral("wfmDeemphasis"));
    m_deemphasis->setAccessibleName(tr("Broadcast FM deemphasis"));
    m_deemphasis->setPlaceholderText(tr("Unknown"));
    m_deemphasis->setFocusPolicy(Qt::StrongFocus);
    applyComboStyle(m_deemphasis);
    m_bandwidth = new WfmReceiverComboBox(this);
    m_bandwidth->setObjectName(QStringLiteral("wfmBandwidth"));
    m_bandwidth->setAccessibleName(tr("Broadcast FM bandwidth"));
    m_bandwidth->setPlaceholderText(tr("Unavailable"));
    m_bandwidth->setFocusPolicy(Qt::StrongFocus);
    applyComboStyle(m_bandwidth);
    auto* deemphasisLabel = new QLabel(tr("De-emphasis"), this);
    deemphasisLabel->setBuddy(m_deemphasis);
    auto* bandwidthLabel = new QLabel(tr("Bandwidth"), this);
    bandwidthLabel->setBuddy(m_bandwidth);
    for (QLabel* label : {deemphasisLabel, bandwidthLabel}) {
        ThemeManager::instance().applyStyleSheet(label,
            "QLabel { color: {{color.text.secondary}}; font-size: 11px; }");
    }
    settings->addWidget(deemphasisLabel, 0, 0);
    settings->addWidget(m_deemphasis, 0, 1);
    settings->addWidget(bandwidthLabel, 1, 0);
    settings->addWidget(m_bandwidth, 1, 1);
    settings->setColumnStretch(1, 1);
    root->addLayout(settings);

    m_settingsToggle = new QPushButton(tr("▸ Settings"), this);
    m_settingsToggle->setObjectName(QStringLiteral("wfmSettingsToggle"));
    m_settingsToggle->setCheckable(true);
    m_settingsToggle->setAccessibleName(tr("Broadcast FM settings"));
    m_settingsToggle->setFocusPolicy(Qt::StrongFocus);
    styleButton(m_settingsToggle);
    root->addWidget(m_settingsToggle);
    m_settingsDrawer = new QFrame(this);
    m_settingsDrawer->setObjectName(QStringLiteral("wfmSettingsDrawer"));
    auto* drawer = new QVBoxLayout(m_settingsDrawer);
    drawer->setContentsMargins(5, 4, 5, 5);
    drawer->setSpacing(3);
    m_showScope = new QCheckBox(tr("Show Lock Scope"), m_settingsDrawer);
    m_showScope->setObjectName(QStringLiteral("wfmShowLockScope"));
    m_showScope->setAccessibleName(tr("Show Broadcast FM lock scope"));
    m_showDiagnostics = new QCheckBox(tr("Show Diagnostics"), m_settingsDrawer);
    m_showDiagnostics->setObjectName(QStringLiteral("wfmShowDiagnostics"));
    m_showDiagnostics->setAccessibleName(tr("Show Broadcast FM diagnostics"));
    const QJsonObject ui = preferences().value(QStringLiteral("ui")).toObject();
    m_showScope->setChecked(ui.value(QStringLiteral("showLockScope")).toBool(true));
    m_showDiagnostics->setChecked(ui.value(QStringLiteral("showDiagnostics")).toBool(false));
    drawer->addWidget(m_showScope);
    drawer->addWidget(m_showDiagnostics);
    root->addWidget(m_settingsDrawer);
    m_diagnostics = new QLabel(this);
    m_diagnostics->setObjectName(QStringLiteral("wfmDiagnostics"));
    m_diagnostics->setWordWrap(true);
    m_diagnostics->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    ThemeManager::instance().applyStyleSheet(m_diagnostics,
        "QLabel { color: {{color.text.secondary}}; font-size: 10px;"
        " background: {{color.background.0}}; border: 1px solid {{color.border.subtle}};"
        " border-radius: 3px; padding: 4px; }");
    root->addWidget(m_diagnostics);

    connect(m_audioMode, &QPushButton::pressed, this, [this] {
        m_audioModeBinding = m_sliceBinding;
    });
    connect(m_audioMode, &QPushButton::clicked, this, [this] {
        if (acceptsControl(m_audioMode, m_audioModeBinding)) {
            m_slice->setWfmForceMono(!m_slice->wfmForceMono());
        }
        refresh(); // The displayed selection remains backend-confirmed.
    });
    connect(m_deemphasis, &QComboBox::activated, this, [this](int index) {
        const quint64 binding = static_cast<WfmReceiverComboBox*>(m_deemphasis)->takeActivationBinding();
        if (acceptsControl(m_deemphasis, binding) && index >= 0 && index < m_deemphasis->count()) {
            m_slice->setWfmDeemphasis(m_deemphasis->itemData(index).toInt());
        }
        refresh();
    });
    connect(m_bandwidth, &QComboBox::activated, this, [this](int index) {
        const quint64 binding = static_cast<WfmReceiverComboBox*>(m_bandwidth)->takeActivationBinding();
        if (acceptsControl(m_bandwidth, binding) && index >= 0 && index < m_bandwidth->count()) {
            const int width = m_bandwidth->itemData(index).toInt();
            const ModeFilters::Edges edges = ModeFilters::edgesForWidth(QStringLiteral("WFM"), width, {});
            const RadioCapabilities caps = m_model->backendCapabilities();
            if (ModeFilters::acceptsFmEdges(QStringLiteral("WFM"),
                    caps.receiveFilterControl ? &*caps.receiveFilterControl : nullptr, edges)) {
                m_slice->setFilterWidth(edges.lo, edges.hi);
            }
        }
        refresh();
    });
    connect(m_settingsToggle, &QPushButton::toggled, this, &WfmApplet::setSettingsExpanded);
    for (QCheckBox* option : {m_showScope, m_showDiagnostics}) {
        connect(option, &QCheckBox::toggled, this, [this] {
            applyUiPreferences();
            saveUiPreferences();
        });
    }
    setSettingsExpanded(false);
    applyUiPreferences();
    refresh();
}

WfmApplet::~WfmApplet() = default;
QSize WfmApplet::sizeHint() const { return {260, layout()->sizeHint().height()}; }
QSize WfmApplet::minimumSizeHint() const { return {220, layout()->minimumSize().height()}; }

bool WfmApplet::ownsWfmSlice() const
{
    return m_model && m_slice && m_model->slice(m_slice->sliceId()) == m_slice
        && m_slice->mode() == QLatin1String("WFM");
}

bool WfmApplet::acceptsControl(const QWidget* control, quint64 binding) const
{
    return binding == m_sliceBinding && m_connected && m_available && ownsWfmSlice()
        && !m_slice->externalReceiveReplacementActive() && control->isEnabled();
}

void WfmApplet::setRadioModel(RadioModel* model)
{
    const quint64 binding = ++m_modelBinding;
    for (const auto& connection : m_modelConnections) { disconnect(connection); }
    m_modelConnections.clear();
    m_availability.reset();
    m_scope->clear();
    m_model = model;
    m_connected = model && model->isConnected();
    m_caps = model ? model->backendCapabilities() : RadioCapabilities{};
    if (model) {
        m_modelConnections.append(connect(model, &RadioModel::capabilitiesChanged, this,
            [this, binding](bool connected, const RadioCapabilities& caps) {
                if (binding != m_modelBinding) { return; }
                const bool connectionChanged = connected != m_connected;
                m_connected = connected;
                m_caps = caps;
                if (connectionChanged) { setSlice(m_slice); }
                refresh();
            }));
        m_modelConnections.append(connect(model, &RadioModel::connectionStateChanged, this,
            [this, binding](bool connected) {
                if (binding != m_modelBinding) { return; }
                const bool connectionChanged = connected != m_connected;
                m_connected = connected;
                m_caps = m_model ? m_model->backendCapabilities() : RadioCapabilities{};
                if (connectionChanged) { setSlice(m_slice); }
                refresh();
            }));
        m_modelConnections.append(connect(model, &RadioModel::sliceRemoved, this,
            [this, binding](int id) {
                if (binding != m_modelBinding) { return; }
                if (m_slice && m_slice->sliceId() == id
                    && (!m_model || m_model->slice(id) != m_slice)) { setSlice(nullptr); }
            }));
        m_modelConnections.append(connect(model, &QObject::destroyed, this, [this, binding] {
            if (binding != m_modelBinding) { return; }
            m_availability.reset();
            m_model = nullptr;
            m_connected = false;
            m_caps = {};
            setSlice(nullptr);
        }));
        registerControls();
    }
    setSlice(m_slice);
}

void WfmApplet::setSlice(SliceModel* slice)
{
    const quint64 binding = ++m_sliceBinding;
    for (QComboBox* combo : {m_deemphasis, m_bandwidth}) {
        static_cast<WfmReceiverComboBox*>(combo)->setBinding(binding);
    }
    m_audioMode->setDown(false);
    for (const auto& connection : m_sliceConnections) { disconnect(connection); }
    m_sliceConnections.clear();
    m_scope->clear();
    m_slice = slice;
    if (slice) {
        const auto current = [this, binding] {
            return binding == m_sliceBinding && m_slice && m_model
                && m_model->slice(m_slice->sliceId()) == m_slice;
        };
        const auto update = [this, current] { if (current()) { refresh(); } };
        m_sliceConnections.append(connect(slice, &SliceModel::modeChanged, this, update));
        m_sliceConnections.append(connect(slice, &SliceModel::filterChanged, this, update));
        m_sliceConnections.append(connect(slice, &SliceModel::letterChanged, this, update));
        m_sliceConnections.append(connect(slice, &SliceModel::frequencyReported, this, update));
        m_sliceConnections.append(connect(slice, &SliceModel::wfmDeemphasisChanged, this, update));
        m_sliceConnections.append(connect(slice, &SliceModel::wfmForceMonoChanged, this, update));
        m_sliceConnections.append(connect(slice, &SliceModel::wfmStereoStatusChanged, this, update));
        m_sliceConnections.append(connect(slice, &SliceModel::wfmReceptionDiagnosticsChanged, this, [this, current] {
            if (!current()) { return; }
            refreshDiagnostics();
            appendScopeSample();
        }));
        m_sliceConnections.append(connect(slice, &SliceModel::frequencyChanged, this, [this, current] {
            if (!current()) { return; }
            m_scope->clear();
            refreshDiagnostics();
        }));
        m_sliceConnections.append(connect(slice, &SliceModel::inCaptureChanged, this, update));
        m_sliceConnections.append(connect(slice, &SliceModel::externalReceiveReplacementChanged, this, update));
        m_sliceConnections.append(connect(slice, &QObject::destroyed, this, [this, binding] {
            if (binding != m_sliceBinding) { return; }
            setSlice(nullptr);
        }));
    }
    refresh();
}

void WfmApplet::registerControls()
{
    m_availability = std::make_unique<ControlAvailabilityRegistry>(*m_model, this);
    const auto supported = [this](bool connected, const RadioCapabilities& caps) {
        return connected && caps.broadcastFmReceive && ownsWfmSlice()
            && !m_slice->externalReceiveReplacementActive();
    };
    m_availability->registerWidget(m_audioMode,
        tr("Mono selection is unavailable for this receiver or replacement audio source"),
        [supported](bool connected, const RadioCapabilities& caps) {
            return supported(connected, caps) && caps.broadcastFmReceive->forceMonoControl;
        }, [] { return true; }, false);
    m_availability->registerWidget(m_deemphasis,
        tr("De-emphasis is unavailable for this receiver or replacement audio source"),
        [supported](bool connected, const RadioCapabilities& caps) {
            return supported(connected, caps) && !caps.broadcastFmReceive->deemphasisUs.isEmpty();
        }, [] { return true; }, false);
    m_availability->registerWidget(m_bandwidth,
        tr("Adjustable WFM bandwidth is unavailable for this receiver or replacement audio source"),
        [supported](bool connected, const RadioCapabilities& caps) {
            return supported(connected, caps) && !ModeFilters::widthsForMode(QStringLiteral("WFM"),
                caps.receiveFilterControl ? &*caps.receiveFilterControl : nullptr).isEmpty();
        }, [] { return true; }, false);
    for (QCheckBox* control : {m_showScope, m_showDiagnostics}) {
        m_availability->registerWidget(control, tr("This receiver does not provide pilot measurements"),
            [this](bool connected, const RadioCapabilities& caps) {
                return connected && caps.broadcastFmReceive && caps.broadcastFmReceive->receptionDiagnostics
                    && ownsWfmSlice();
            }, [] { return true; }, false);
    }
}

void WfmApplet::refresh()
{
    const bool available = m_connected && m_caps.broadcastFmReceive && ownsWfmSlice();
    if (available != m_available) {
        m_available = available;
        if (!available) { m_scope->clear(); }
        emit availabilityChanged(available);
    }
    const QString identity = m_available
        ? tr("Slice %1 · %2").arg(m_slice->letter(), m_slice->frequencyReportedKnown()
            ? tr("%1 MHz").arg(m_slice->reportedFrequency(), 0, 'f', 6) : tr("Frequency unavailable"))
        : tr("No selected WFM receiver");
    setReadout(m_identity, identity, tr("Broadcast FM receiver: %1").arg(identity));
    if (m_availability) {
        m_availability->refreshEngaged();
    } else {
        for (QWidget* control : {static_cast<QWidget*>(m_audioMode), static_cast<QWidget*>(m_deemphasis),
                                static_cast<QWidget*>(m_bandwidth), static_cast<QWidget*>(m_showScope),
                                static_cast<QWidget*>(m_showDiagnostics)}) {
            control->setEnabled(false);
            control->setAccessibleDescription(tr("Connect a broadcast FM receiver and select WFM"));
        }
    }
    const bool forceMono = m_available && m_slice->wfmForceMono();
    const QString modeText = forceMono ? tr("Mono") : tr("Auto Stereo");
    if (m_audioMode->text() != modeText) {
        m_audioMode->setText(modeText);
        m_audioMode->setAccessibleName(tr("Broadcast FM audio mode: %1").arg(modeText));
        QAccessibleEvent event(m_audioMode, QAccessible::NameChanged);
        QAccessible::updateAccessibility(&event);
    }
    if (m_audioMode->isEnabled()) {
        m_audioMode->setAccessibleDescription(tr("Cycle between forced Mono and Auto Stereo. Auto Stereo falls back to mono when no stereo pilot is detected."));
        m_audioMode->setToolTip(m_audioMode->accessibleDescription());
    }
    const QString oldDeemphasis = m_deemphasis->currentText();
    const QString oldBandwidth = m_bandwidth->currentText();
    {
        const QSignalBlocker blocker(m_deemphasis);
        m_deemphasis->clear();
        if (m_caps.broadcastFmReceive) {
            for (int value : m_caps.broadcastFmReceive->deemphasisUs) {
                m_deemphasis->addItem(tr("%1 µs").arg(value), value);
            }
        }
        m_deemphasis->setCurrentIndex(m_available ? m_deemphasis->findData(m_slice->wfmDeemphasisUs()) : -1);
    }
    {
        const QSignalBlocker blocker(m_bandwidth);
        m_bandwidth->clear();
        const QVector<int> widths = ModeFilters::widthsForMode(QStringLiteral("WFM"),
            m_caps.receiveFilterControl ? &*m_caps.receiveFilterControl : nullptr);
        for (int width : widths) { m_bandwidth->addItem(tr("%1 kHz").arg(width / 1000), width); }
        int selected = -1;
        if (m_available) {
            const int low = m_slice->filterLow();
            const int high = m_slice->filterHigh();
            const int width = high - low;
            if (low == -width / 2 && high == width / 2) { selected = m_bandwidth->findData(width); }
            if (selected < 0) {
                // Accepted custom edges remain exact; a display refresh must
                // never round them to a nearby symmetric preset or send intent.
                m_bandwidth->addItem(tr("%1 kHz (custom)").arg(width / 1000.0, 0, 'g', 5), 0);
                selected = m_bandwidth->count() - 1;
            }
        }
        m_bandwidth->setCurrentIndex(selected);
    }
    announceCombo(m_deemphasis, oldDeemphasis);
    announceCombo(m_bandwidth, oldBandwidth);
    refreshDiagnostics();
}

void WfmApplet::refreshDiagnostics()
{
    const bool receiving = m_available && m_slice->inCapture()
        && !m_slice->externalReceiveReplacementActive();
    const WfmReceptionDiagnostics diagnostics = receiving && m_caps.broadcastFmReceive->receptionDiagnostics
        ? m_slice->wfmReceptionDiagnostics() : WfmReceptionDiagnostics{};
    const bool forceMono = m_available && m_slice->wfmForceMono();
    const WfmStereoStatus status = receiving ? m_slice->wfmStereoStatus() : WfmStereoStatus::Unavailable;
    QString text = tr("Unavailable");
    QString token = QStringLiteral("color.text.secondary");
    if (diagnostics.valid) {
        if (forceMono) {
            text = diagnostics.pilotLocked ? tr("Pilot detected") : tr("No pilot");
        } else if (status == WfmStereoStatus::Stereo && diagnostics.pilotLocked) {
            text = tr("Stereo");
            if (!forceMono) { token = QStringLiteral("color.accent.success"); }
        } else if (status == WfmStereoStatus::Acquiring) {
            text = tr("Acquiring");
            if (!forceMono) { token = QStringLiteral("color.accent.warning"); }
        } else if (status == WfmStereoStatus::Mono) {
            text = tr("Mono fallback");
        }
        if (!forceMono && diagnostics.lockLossCount > 0 && diagnostics.stableDurationMs < 5000) {
            text += tr(" · Unstable");
            token = QStringLiteral("color.accent.warning");
        }
    } else {
        m_scope->clear();
        if (receiving && status == WfmStereoStatus::Acquiring) {
            text = tr("Acquiring");
            if (!forceMono) { token = QStringLiteral("color.accent.warning"); }
        }
    }
    setReadout(m_status, text, tr("Broadcast FM observed pilot status: %1").arg(text));
    m_status->setAccessibleDescription(tr("Measured stereo pilot indicator, independent of the selected audio mode."));
    ThemeManager::instance().setWidgetForegroundToken(m_status, token);
    QString details = tr("No current receive measurements");
    if (diagnostics.valid) {
        details = tr("Pilot indicator: %1\nPilot magnitude: %2 relative\nAcquire / release: %3 / %4 relative\nHigh blocks: %5 / %6 · Low blocks: %7 / %8\nLock duration: %9 s\nLock losses: %10 · Reacquisitions: %11\nObservation: %12 s\nPilot state unchanged for %13 s (5 s window)")
            .arg(diagnostics.pilotLocked ? tr("Detected") : tr("Not detected"))
            .arg(diagnostics.pilotMagnitude, 0, 'g', 4)
            .arg(diagnostics.pilotEngageThreshold, 0, 'g', 4)
            .arg(diagnostics.pilotReleaseThreshold, 0, 'g', 4)
            .arg(diagnostics.consecutiveHighBlocks).arg(diagnostics.engageBlocks)
            .arg(diagnostics.consecutiveLowBlocks).arg(diagnostics.releaseBlocks)
            .arg(diagnostics.lockDurationMs / 1000.0, 0, 'f', 1)
            .arg(diagnostics.lockLossCount).arg(diagnostics.reacquisitionCount)
            .arg(diagnostics.observationDurationMs / 1000.0, 0, 'f', 1)
            .arg(diagnostics.stableDurationMs / 1000.0, 0, 'f', 1);
    }
    if (m_available) {
        const QString frequency = m_slice->frequencyReportedKnown()
            ? tr("%1 MHz").arg(m_slice->reportedFrequency(), 0, 'f', 6) : tr("Unavailable");
        details += tr("\nFrequency: %1\nFilter: %2 to %3 kHz\nDe-emphasis: %4 µs")
            .arg(frequency).arg(m_slice->filterLow() / 1000.0, 0, 'g', 5)
            .arg(m_slice->filterHigh() / 1000.0, 0, 'g', 5).arg(m_slice->wfmDeemphasisUs());
        if (const PanadapterModel* pan = m_model->panadapter(m_slice->panId())) {
            details += tr("\nRF gain: %1%2").arg(pan->rfGain()).arg(pan->rfGainUnitSuffix());
        }
    }
    setReadout(m_diagnostics, details, tr("Broadcast FM diagnostics: %1").arg(details));
    m_diagnostics->setAccessibleDescription(tr("Pilot magnitude uses relative discriminator units. The pilot indicator is not PLL lock, SNR, or an audio quality score. Measurements reset with the decoder and clear when stale."));
    m_diagnostics->setToolTip(m_diagnostics->accessibleDescription());
}

void WfmApplet::appendScopeSample()
{
    if (!m_available || !m_slice || !m_slice->inCapture() || m_slice->externalReceiveReplacementActive()
        || !m_caps.broadcastFmReceive->receptionDiagnostics || !m_slice->wfmReceptionDiagnostics().valid) {
        m_scope->clear();
        return;
    }
    if (!m_showScope->isChecked() || !m_scope->isVisible()) { return; }
    const WfmReceptionDiagnostics& diagnostics = m_slice->wfmReceptionDiagnostics();
    m_scope->appendSample(diagnostics.pilotMagnitude, diagnostics.pilotEngageThreshold,
        diagnostics.pilotReleaseThreshold, m_slice->wfmStereoStatus(), m_slice->wfmForceMono());
}

void WfmApplet::setSettingsExpanded(bool expanded)
{
    m_settingsDrawer->setVisible(expanded);
    m_settingsToggle->setText(expanded ? tr("▾ Settings") : tr("▸ Settings"));
    m_settingsToggle->setAccessibleDescription(expanded ? tr("Expanded") : tr("Collapsed"));
    setMinimumHeight(minimumSizeHint().height());
    updateGeometry();
    if (QWidget* parent = parentWidget()) { parent->updateGeometry(); }
}

void WfmApplet::applyUiPreferences()
{
    m_scope->setVisible(m_showScope->isChecked());
    if (!m_showScope->isChecked()) { m_scope->clear(); }
    m_diagnostics->setVisible(m_showDiagnostics->isChecked());
    setMinimumHeight(minimumSizeHint().height());
    updateGeometry();
    if (QWidget* parent = parentWidget()) { parent->updateGeometry(); }
}

void WfmApplet::saveUiPreferences()
{
    // Client presentation only. Accepted receiver controls persist through
    // their model/backend path, never through this UI preference object.
    QJsonObject document = preferences();
    QJsonObject ui = document.value(QStringLiteral("ui")).toObject();
    ui.insert(QStringLiteral("showLockScope"), m_showScope->isChecked());
    ui.insert(QStringLiteral("showDiagnostics"), m_showDiagnostics->isChecked());
    document.insert(QStringLiteral("ui"), ui);
    AppSettings::instance().setValue(QStringLiteral("WfmApplet"),
        QString::fromUtf8(QJsonDocument(document).toJson(QJsonDocument::Compact)));
    AppSettings::instance().save();
}
} // namespace AetherSDR
