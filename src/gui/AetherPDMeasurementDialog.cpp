#include "AetherPDMeasurementDialog.h"
#include "AetherPDMeasurementSpectrum.h"
#include "AetherPDMeasurementExportDialog.h"
#include "CanonIndicators.h"
#include "core/TxKeyingMarker.h"
#include "models/AetherPDMeasurementModel.h"
#include "models/AetherPDInstallationModel.h"

#include <QAccessible>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>

namespace AetherSDR {
namespace {
void availability(QWidget* button, bool enabled, const QString& reason)
{
    const QPointer<QWidget> alive(button);
    button->setEnabled(enabled);
    if (!alive) { return; }
    button->setToolTip(enabled ? QString{} : reason);
    if (!alive) { return; }
    button->setAccessibleDescription(enabled ? QString{} : reason);
}
}

AetherPDMeasurementDialog::AetherPDMeasurementDialog(RadioModel* radio, QWidget* parent)
    : CanonWindow(tr("AetherPD linearity measurement"), parent), m_radio(radio)
    , m_session(radio ? radio->controlSession() : RadioControlSession{})
    , m_controller(std::make_shared<TxController>(radio))
{
    setObjectName(QStringLiteral("AetherPDMeasurementDialog"));
    setReturnClicksDefault(false);
    setMinimumSize(560, 500);
    auto* layout = new QVBoxLayout(bodyWidget());
    layout->setContentsMargins(24, 24, 24, 24);
    layout->addWidget(makeCanonHeader(tr("Linearity measurement")));
    auto* hint = new QLabel(tr("Experimental · local DAX IQ · FDX off. Select your frequency, ANT1 dummy load and "
        "attenuated RXA feedback before arming. Assign an available IQ channel (1–4) to the feedback panadapter in its DAX panel, "
        "select that pan in AetherPD and start it, then select PD mode on your TX slice. The source runs four seconds of alignment noise, then "
        "the selected tones, for at most eight seconds. Prepare checks reception; only Transmit keys RF."), bodyWidget());
    hint->setWordWrap(true);
    layout->addWidget(hint);
    m_dummyLoad = new QCheckBox(tr("Dummy load connected; feedback attenuation checked"), bodyWidget());
    m_dummyLoad->setObjectName(QStringLiteral("pdMeasureDummyLoad"));
    m_dummyLoad->setAccessibleName(m_dummyLoad->text());
    layout->addWidget(m_dummyLoad);
    auto* powerLayout = new QGridLayout;
    auto* powerLabel = new QLabel(tr("Radio power setting (W):"), bodyWidget());
    m_power = new QSpinBox(bodyWidget());
    m_power->setObjectName(QStringLiteral("pdMeasureRadioPower"));
    m_power->setRange(-1, 0);
    m_power->setValue(-1);
    m_power->setSpecialValueText(tr("Unknown"));
    m_power->setAccessibleName(powerLabel->text());
    powerLabel->setBuddy(m_power);
    m_applyPower = new QPushButton(tr("Apply power"), bodyWidget());
    m_applyPower->setObjectName(QStringLiteral("pdMeasureApplyPower"));
    m_powerStatus = new QLabel(bodyWidget());
    m_powerStatus->setObjectName(QStringLiteral("pdMeasurePowerStatus"));
    m_powerStatus->setWordWrap(true);
    powerLayout->addWidget(powerLabel, 0, 0);
    powerLayout->addWidget(m_power, 0, 1);
    powerLayout->addWidget(m_applyPower, 0, 2);
    powerLayout->addWidget(m_powerStatus, 1, 0, 1, 3);
    layout->addLayout(powerLayout);
    connect(m_power, &QSpinBox::valueChanged, this, &AetherPDMeasurementDialog::render);
    connect(m_applyPower, &QPushButton::clicked, this, [this] {
        if (!current()) { return; }
        const QPointer<AetherPDMeasurementDialog> alive(this);
        const bool requested = m_radio->aetherPDMeasurementModel().requestPower(m_session, m_power->value());
        if (!alive) { return; }
        m_notice = requested ? QString{} : tr("Power request refused. Check radio readback and cancel any armed source first.");
        render();
    });
    auto* limitLabel = new QLabel(tr("Measured forward-power abort limit (W):"), bodyWidget());
    m_limit = new QDoubleSpinBox(bodyWidget());
    m_limit->setObjectName(QStringLiteral("pdMeasurePowerLimit"));
    m_limit->setRange(0, 100);
    m_limit->setDecimals(1);
    m_limit->setSpecialValueText(tr("Choose a limit"));
    m_limit->setAccessibleName(limitLabel->text());
    m_limit->setAccessibleDescription(tr("Does not change radio drive. Choose a limit above the nominal radio power setting and within your bench rating."));
    limitLabel->setBuddy(m_limit);
    layout->addWidget(limitLabel);
    layout->addWidget(m_limit);
    auto* analysis = new QGridLayout;
    m_window = new QComboBox(bodyWidget());
    m_window->setObjectName(QStringLiteral("pdMeasureWindow"));
    m_window->setAccessibleName(tr("Analysis window"));
    for (const txlin::WindowKind window : {txlin::WindowKind::BlackmanHarris7, txlin::WindowKind::BlackmanHarris4,
                                         txlin::WindowKind::FlatTop, txlin::WindowKind::Hann}) {
        m_window->addItem(QString::fromStdString(txlin::windowKindLabel(window)), static_cast<int>(window));
    }
    m_averages = new QSpinBox(bodyWidget());
    m_averages->setObjectName(QStringLiteral("pdMeasureAverages"));
    m_averages->setAccessibleName(tr("Averages"));
    m_averages->setRange(1, AetherPDMeasurementSettings::kMaxAverages);
    m_order = new QComboBox(bodyWidget());
    m_order->setObjectName(QStringLiteral("pdMeasureOrder"));
    m_order->setAccessibleName(tr("Highest intermodulation order"));
    m_order->addItem(tr("IMD3"), 3);
    m_order->addItem(tr("IMD3 + 5"), 5);
    m_order->addItem(tr("IMD3 + 5 + 7"), 7);
    m_timeout = new QSpinBox(bodyWidget());
    m_timeout->setObjectName(QStringLiteral("pdMeasureTimeout"));
    m_timeout->setAccessibleName(tr("Transmit timeout in seconds"));
    m_timeout->setRange(AetherPDMeasurementSettings::kMinTimeoutMs / 1000, AetherPDMeasurementSettings::kMaxTimeoutMs / 1000);
    const auto analysisField = [this, analysis](const QString& title, QWidget* field, int column) {
        auto* label = new QLabel(title, bodyWidget());
        label->setBuddy(field);
        analysis->addWidget(label, 0, column);
        analysis->addWidget(field, 1, column);
    };
    analysisField(tr("Analysis window"), m_window, 0);
    analysisField(tr("Averages"), m_averages, 1);
    analysisField(tr("Highest order"), m_order, 2);
    analysisField(tr("Transmit timeout (s)"), m_timeout, 3);
    layout->addLayout(analysis);
    auto* toneLayout = new QGridLayout;
    auto* toneLabel = new QLabel(tr("Tone spacing (Hz):"), bodyWidget());
    m_toneSpacing = new QSpinBox(bodyWidget());
    m_toneSpacing->setObjectName(QStringLiteral("pdMeasureToneSpacing"));
    m_toneSpacing->setRange(aetherpd::kMinToneSpacingHz, aetherpd::kMaxToneSpacingHz);
    m_toneSpacing->setValue(aetherpd::kDefaultToneSpacingHz);
    m_toneSpacing->setAccessibleName(toneLabel->text());
    toneLabel->setBuddy(m_toneSpacing);
    m_toneStatus = new QLabel(bodyWidget());
    m_toneStatus->setObjectName(QStringLiteral("pdMeasureToneStatus"));
    m_toneStatus->setWordWrap(true);
    toneLayout->addWidget(toneLabel, 0, 0);
    toneLayout->addWidget(m_toneSpacing, 0, 1);
    toneLayout->addWidget(m_toneStatus, 1, 0, 1, 2);
    layout->addLayout(toneLayout);
    connect(m_toneSpacing, &QSpinBox::valueChanged, this, &AetherPDMeasurementDialog::render);
    auto* grid = new QGridLayout;
    const auto button = [this, grid](const QString& text, const char* name, int row, int col) {
        auto* result = new QPushButton(text, bodyWidget());
        result->setObjectName(QString::fromLatin1(name));
        result->setAccessibleName(text);
        result->setAutoDefault(false);
        result->setMinimumHeight(result->fontMetrics().height() + 14);
        grid->addWidget(result, row, col);
        return result;
    };
    m_arm = button(tr("Arm measurement source"), "pdMeasureArm", 0, 0);
    m_cancelSource = button(tr("Cancel source"), "pdMeasureCancelSource", 0, 1);
    m_prepare = button(tr("Prepare receive check"), "pdMeasurePrepare", 1, 0);
    m_transmit = button(tr("Transmit measurement"), "pdMeasureTransmit", 1, 1);
    m_stop = button(tr("Stop measurement"), "pdMeasureStop", 2, 0);
    m_export = button(tr("Export result…"), "pdMeasureExport", 2, 1);
    layout->addLayout(grid);
    m_state = new QLabel(bodyWidget());
    m_state->setObjectName(QStringLiteral("pdMeasureState"));
    m_state->setTextFormat(Qt::PlainText);
    m_state->setWordWrap(true);
    layout->addWidget(m_state);
    m_spectrum = new AetherPDMeasurementSpectrum(bodyWidget());
    layout->addWidget(m_spectrum, 2);
    m_result = new QPlainTextEdit(bodyWidget());
    m_result->setObjectName(QStringLiteral("pdMeasureResult"));
    m_result->setAccessibleName(tr("Linearity measurement result"));
    m_result->setReadOnly(true);
    layout->addWidget(m_result, 1);
    auto* closeHint = new QLabel(tr("Closing requests measurement stop; pending release remains owned by the radio session. "
        "It does not stop AetherPD correction. An unused armed source expires after 30 seconds; Cancel source withdraws it immediately."), bodyWidget());
    closeHint->setWordWrap(true);
    layout->addWidget(closeHint);
    applyCanonSheet(bodyWidget(), [] {
        return QStringLiteral(
            "QLabel,QCheckBox { color: {{color.canon.inkSoft}}; background: transparent; }"
            "QPlainTextEdit,QDoubleSpinBox,QSpinBox,QComboBox { color: {{color.canon.inkSoft}}; background: {{color.canon.nested}}; border: 1px solid {{color.canon.line}}; }"
            "QPushButton { background: {{color.canon.control}}; color: {{color.canon.cyan}}; border: 1px solid {{color.canon.lineHi}}; padding: 6px; }"
            "QPushButton:disabled { color: {{color.canon.muted}}; }"
            "QPushButton:hover,QPushButton:focus { border-color: {{color.canon.aqua}}; }") + canonIndicatorRules() + canonSpinArrowRules();
    });
    const auto requestSource = [this](aetherpd::Operation operation) {
        if (!current()) { return; }
        const QPointer<AetherPDMeasurementDialog> alive(this);
        m_notice.clear();
        const bool submitted = operation == aetherpd::Operation::ArmMeasurement
            ? m_radio->aetherPDMeasurementModel().armSource(m_session, m_toneSpacing->value())
            : m_radio->requestAetherPD(m_session, operation);
        if (!alive) { return; }
        if (!submitted && m_notice.isEmpty()) { m_notice = tr("Source request was not submitted. Check the current connection and service status."); }
        render();
    };
    connect(m_arm, &QPushButton::clicked, this, [requestSource] {
        requestSource(aetherpd::Operation::ArmMeasurement);
    });
    connect(m_cancelSource, &QPushButton::clicked, this, [requestSource] {
        requestSource(aetherpd::Operation::CancelCalibration);
    });
    connect(m_prepare, &QPushButton::clicked, this, &AetherPDMeasurementDialog::prepare);
    connect(m_stop, &QPushButton::clicked, this, &AetherPDMeasurementDialog::stop);
    connect(m_export, &QPushButton::clicked, this, &AetherPDMeasurementDialog::exportResult);
    connect(m_dummyLoad, &QCheckBox::toggled, this, &AetherPDMeasurementDialog::render);
    connect(m_limit, &QDoubleSpinBox::valueChanged, this, &AetherPDMeasurementDialog::render);
    const auto saveAnalysis = [this] {
        if (!current()) { return; }
        const QPointer<AetherPDMeasurementDialog> alive(this);
        const AetherPDMeasurementModel::AnalysisOptions options{
            static_cast<txlin::WindowKind>(m_window->currentData().toInt()), m_averages->value(), m_order->currentData().toInt(), m_timeout->value() * 1000};
        m_notice.clear();
        const bool saved = m_radio->aetherPDMeasurementModel().setAnalysisOptions(m_session, options);
        if (!alive) { return; }
        if (!saved) { m_notice = tr("Measurement preferences were not changed: %1").arg(m_radio->aetherPDMeasurementModel().status()); }
        render();
    };
    connect(m_window, &QComboBox::currentIndexChanged, this, saveAnalysis);
    connect(m_averages, &QSpinBox::valueChanged, this, saveAnalysis);
    connect(m_order, &QComboBox::currentIndexChanged, this, saveAnalysis);
    connect(m_timeout, &QSpinBox::valueChanged, this, saveAnalysis);
    markTxKeying(m_transmit);
    connect(m_transmit, &QPushButton::clicked, this, [this] {
        if (!current()) { return; }
        transmit(m_controller, m_controller->captureProgram(TxController::Activity::Mox),
            m_radio->aetherPDMeasurementModel().preparationId());
    });
    registerTxKeyingAction(m_transmit, [this](const std::shared_ptr<TxController>& controller,
        const QString& action, const QString&) -> TxKeyingAction::Prepared {
        if (action != QLatin1String("click") || !current() || !controller || !controller->belongsTo(m_radio)) { return {}; }
        if (m_radio->aetherPDMeasurementModel().state() != AetherPDMeasurementModel::State::Ready) { return {}; }
        const quint64 preparation = m_radio->aetherPDMeasurementModel().preparationId();
        const TxController::Input input = controller->captureProgram(TxController::Activity::Mox);
        const QPointer<AetherPDMeasurementDialog> alive(this);
        return [alive, controller, input, preparation] { if (alive) { alive->transmit(controller, input, preparation); } };
    });
    if (radio) {
        connect(&radio->aetherPDModel(), &AetherPDModel::operationFailed, this,
            [this](aetherpd::Operation operation, AetherPDClient::Failure failure) {
                if (!current() || (operation != aetherpd::Operation::ArmMeasurement
                    && operation != aetherpd::Operation::CancelCalibration)) { return; }
                if (operation == aetherpd::Operation::CancelCalibration) {
                    m_notice = tr("Source cancellation is not confirmed. Check service status; Stop AetherPD ends the owned source.");
                } else if (failure == AetherPDClient::Failure::Refused || failure == AetherPDClient::Failure::InvalidRequest) {
                    m_notice = tr("Measurement source request refused. Check PD mode, the selected feedback pan and IQ channel, fresh DAX IQ and the transmit interlock.");
                } else if (failure == AetherPDClient::Failure::Busy) {
                    m_notice = tr("Measurement source request was not submitted because another request is pending.");
                } else {
                    m_notice = tr("Measurement source arm is not confirmed. Check service status or use Cancel source before trying again.");
                }
                render();
            });
        connect(&radio->aetherPDMeasurementModel(), &AetherPDMeasurementModel::changed, this, &AetherPDMeasurementDialog::render);
        connect(&radio->aetherPDModel(), &AetherPDModel::changed, this, &AetherPDMeasurementDialog::render);
        connect(&radio->aetherPDInstallationModel(), &AetherPDInstallationModel::changed, this, &AetherPDMeasurementDialog::render);
        connect(radio, &RadioModel::connectionStateChanged, this, &AetherPDMeasurementDialog::render);
        connect(radio, &QObject::destroyed, this, &AetherPDMeasurementDialog::render);
    }
    render();
}

AetherPDMeasurementDialog::~AetherPDMeasurementDialog()
{
    m_closed = true;
    if (m_radio) { disconnect(&m_radio->aetherPDMeasurementModel(), nullptr, this, nullptr); }
    stop();
}
bool AetherPDMeasurementDialog::current() const
{
    return !m_closed && m_radio && m_radio->controlSessionCurrent(m_session);
}
void AetherPDMeasurementDialog::prepare()
{
    if (!current()) { return; }
    m_notice.clear();
    const QPointer<AetherPDMeasurementDialog> alive(this);
    const bool accepted = m_radio->aetherPDMeasurementModel().prepareCurrent(m_session, m_limit->value(), m_dummyLoad->isChecked());
    if (!alive) { return; }
    if (!accepted) { m_notice = tr("Preparation refused. Check source readiness, frequency, band plan, radio power and cooldown."); }
    render();
}
void AetherPDMeasurementDialog::transmit(const std::shared_ptr<TxController>& controller, const TxController::Input& input, quint64 preparationId)
{
    if (!current() || !m_transmit->isEnabled() || !controller || !input.belongsTo(m_radio) || !input.valid()
        || preparationId != m_radio->aetherPDMeasurementModel().preparationId()) { return; }
    const QPointer<AetherPDMeasurementDialog> alive(this);
    m_notice.clear();
    const bool accepted = m_radio->aetherPDMeasurementModel().start(m_session, [controller, input] {
        return controller->valid() && input.valid();
    });
    if (!alive) { return; }
    if (!accepted) {
        m_notice = tr("Transmission refused: %1\nPrepare a fresh receive check before trying again.")
            .arg(m_radio->aetherPDMeasurementModel().status());
    }
    render();
}
void AetherPDMeasurementDialog::stop()
{
    if (m_radio) { m_radio->aetherPDMeasurementModel().stop(m_session); }
}
void AetherPDMeasurementDialog::done(int result)
{
    const QPointer<AetherPDMeasurementDialog> alive(this);
    m_closed = true;
    m_controller->invalidate();
    if (!alive) { return; }
    stop();
    if (alive) { CanonWindow::done(result); }
}
void AetherPDMeasurementDialog::render()
{
    using State = AetherPDMeasurementModel::State;
    const QPointer<AetherPDMeasurementDialog> alive(this);
    const bool valid = current();
    AetherPDMeasurementModel* model = valid ? &m_radio->aetherPDMeasurementModel() : nullptr;
    const State state = model ? model->state() : State::Idle;
    const std::optional<aetherpd::Status> source = valid ? m_radio->aetherPDModel().status() : std::nullopt;
    const std::optional<aetherpd::Operation> pending = valid
        ? m_radio->aetherPDModel().pendingOperation() : std::nullopt;
    const bool changingSource = pending && *pending != aetherpd::Operation::Status;
    const bool installed = valid && !m_radio->aetherPDInstallationModel().active();
    const bool idle = state == State::Idle;
    const AetherPDMeasurementModel::PowerControl power = model ? model->powerControl(m_session)
        : AetherPDMeasurementModel::PowerControl{{}, {}, tr("Original radio session is unavailable.")};
    {
        const QSignalBlocker blocker(m_power);
        m_power->setMaximum(power.maximum.value_or(power.reported.value_or(0)));
        if (m_powerReadback != power.reported) {
            m_powerReadback = power.reported;
            m_power->setValue(power.reported.value_or(-1));
        }
    }
    const QString powerHelp = tr("Requests the nominal radio setting without transmitting. Wait for radio readback before arming; actual output is measured separately.");
    availability(m_power, power.unavailableReason.isEmpty(), power.unavailableReason);
    if (!alive) { return; }
    const bool powerEdit = power.reported && m_power->value() >= 0 && m_power->value() != *power.reported;
    availability(m_applyPower, power.unavailableReason.isEmpty() && powerEdit,
        power.unavailableReason.isEmpty() ? tr("Choose a different radio power setting first.") : power.unavailableReason);
    if (!alive) { return; }
    if (power.unavailableReason.isEmpty()) {
        m_power->setAccessibleDescription(powerHelp);
        if (!alive) { return; }
        m_power->setToolTip(powerHelp);
        if (!alive) { return; }
        if (powerEdit) { m_applyPower->setAccessibleDescription(powerHelp); }
        if (!alive) { return; }
    }
    m_powerStatus->setText(power.reported
        ? tr("Radio reports %1 W setting; actual RF output may differ.").arg(*power.reported)
        : tr("Radio power is unknown or awaiting readback."));
    if (!alive) { return; }
    const AetherPDMeasurementSettings::ReadResult preferences = model ? model->analysisOptions()
        : AetherPDMeasurementSettings::ReadResult{false, {}, tr("Original radio is unavailable.")};
    if (idle) {
        const QSignalBlocker windowBlock(m_window), averagesBlock(m_averages), orderBlock(m_order), timeoutBlock(m_timeout);
        m_window->setCurrentIndex(m_window->findData(static_cast<int>(preferences.values.window)));
        m_averages->setValue(preferences.values.averages);
        m_order->setCurrentIndex(m_order->findData(preferences.values.maxOrder));
        m_timeout->setValue(preferences.values.timeoutMs / 1000);
    }
    const QString analysisReason = preferences.usable ? tr("Measurement options are locked until this measurement ends.") : preferences.reason;
    for (QWidget* control : {static_cast<QWidget*>(m_window), static_cast<QWidget*>(m_averages), static_cast<QWidget*>(m_order), static_cast<QWidget*>(m_timeout)}) {
        availability(control, valid && idle && preferences.usable, analysisReason);
        if (!alive) { return; }
    }
    if (idle && preferences.usable) {
        m_window->setAccessibleDescription(tr("Window choice changes leakage and frequency resolution; no window guarantees a fixed measurable IMD depth."));
        if (!alive) { return; }
        m_averages->setAccessibleDescription(tr("One to four frames within the bounded source window. Averaging reduces estimate variance, not the physical noise floor."));
        if (!alive) { return; }
        m_timeout->setAccessibleDescription(tr("Three to ten seconds from Transmit measurement, including startup. Short limits may end before capture completes. Expiry requests stop; confirmed RF release can occur later."));
        if (!alive) { return; }
        m_timeout->setToolTip(m_timeout->accessibleDescription());
        if (!alive) { return; }
        m_order->setAccessibleDescription(tr("Highest odd distortion product to analyze: three, five or seven."));
        if (!alive) { return; }
    }
    const bool readySource = source && source->available && source->registered && source->measurementControl && !source->keyed
        && (!source->feedbackBinding || source->feedbackBinding->valid)
        && !m_radio->fullDuplexEnabled() && source->state != aetherpd::State::BlockedByRadioApd;
    const bool toneCapable = source && source->measurementToneControl && source->measurementToneConfig;
    if (source && source->measurementControl && !toneCapable) {
        const QSignalBlocker blocker(m_toneSpacing);
        m_toneSpacing->setValue(aetherpd::kDefaultToneSpacingHz);
    }
    const QString armReason = model ? model->sourceArmReason(m_session, m_toneSpacing->value())
        : tr("Original radio session is unavailable.");
    const QString selectionReason = model ? model->toneSelectionReason(m_toneSpacing->value()) : armReason;
    const QString editReason = !source ? armReason : !toneCapable
        ? tr("This shim does not advertise selectable tones. The legacy source uses 1190 Hz spacing (710/1900 Hz).")
        : (armReason == selectionReason ? QString{} : armReason);
    availability(m_toneSpacing, toneCapable && editReason.isEmpty(), editReason);
    if (!alive) { return; }
    if (m_toneSpacing->isEnabled()) {
        m_toneSpacing->setAccessibleDescription(tr("200 to 10000 Hz between tones, centered 1305 Hz above the carrier. Editing sends no command; Arm measurement source submits the pair."));
        if (!alive) { return; }
    }
    const aetherpd::MeasurementToneConfig draft{0, static_cast<quint32>(m_toneSpacing->value())};
    QString toneText = tr("Requested offsets: %1 / %2 Hz from carrier.")
        .arg(draft.lowerOffsetHz(), 0, 'f', 1).arg(draft.upperOffsetHz(), 0, 'f', 1);
    if (source && source->measurementToneConfig) {
        const auto& observed = *source->measurementToneConfig;
        toneText += tr(" Reported offsets: %1 / %2 Hz; source %3.")
            .arg(observed.lowerOffsetHz(), 0, 'f', 1).arg(observed.upperOffsetHz(), 0, 'f', 1)
            .arg(observed.sourceId, 16, 16, QLatin1Char('0'));
    } else {
        toneText += tr(" No configured tone profile reported.");
    }
    if (!selectionReason.isEmpty()) { toneText += QLatin1Char(' ') + selectionReason; }
    if (!editReason.isEmpty()) { toneText += QLatin1Char(' ') + editReason; }
    if (m_toneStatus->text() != toneText) {
        m_toneStatus->setText(toneText);
        if (!alive) { return; }
        m_toneStatus->setAccessibleName(toneText);
        if (!alive) { return; }
        QAccessibleEvent event(m_toneStatus, QAccessible::NameChanged);
        QAccessible::updateAccessibility(&event);
        if (!alive) { return; }
    }
    availability(m_arm, installed && idle && armReason.isEmpty() && m_dummyLoad->isChecked(),
        armReason.isEmpty() ? tr("Confirm the dummy load and feedback attenuation before arming.") : armReason);
    if (!alive) { return; }
    availability(m_cancelSource, valid && source && source->measurementControl,
        tr("A current authenticated measurement-capable service is required."));
    if (!alive) { return; }
    const QString observedToneReason = model && source
        ? model->toneSelectionReason(source->measurementToneConfig.value_or(aetherpd::MeasurementToneConfig{}).spacingHz)
        : tr("Wait for service status.");
    availability(m_prepare, observedToneReason.isEmpty() && installed && idle && preferences.usable && readySource && !changingSource && m_dummyLoad->isChecked() && m_limit->value() > 0
        && source->measurement && source->measurement->sourceId && source->sourcePurpose == aetherpd::SourcePurpose::Measurement
        && source->calibrationState == aetherpd::CalibrationState::Armed,
        !observedToneReason.isEmpty() ? observedToneReason
            : (preferences.usable ? tr("Arm the measurement source, confirm the bench path and choose a forward-power limit.") : preferences.reason));
    if (!alive) { return; }
    availability(m_transmit, installed && state == State::Ready && m_dummyLoad->isChecked(),
        tr("Prepare must complete a fresh receive check. Readiness expires after two seconds."));
    if (!alive) { return; }
    availability(m_stop, valid && !idle, tr("No measurement is pending."));
    if (!alive) { return; }
    availability(m_export, model && model->result().has_value(), tr("A completed result is required."));
    if (!alive) { return; }
    m_dummyLoad->setEnabled(idle);
    if (!alive) { return; }
    m_limit->setEnabled(idle);
    if (!alive) { return; }
    QString message = !valid ? tr("Connection ended. Reopen for the current radio.") : (idle ? m_notice : QString{});
    if (message.isEmpty()) {
        switch (state) {
        case State::Idle: message = model->status().isEmpty() ? tr("Idle. Arm the source, then prepare.") : model->status(); break;
        case State::CheckingReceive: message = tr("Checking receive-only DAX IQ…"); break;
        case State::Ready: message = tr("Ready for your Transmit click (valid for two seconds)."); break;
        case State::AwaitingKey: message = tr("Starting independent cleanup helper and waiting for owned TX confirmation…"); break;
        case State::Capturing: message = tr("Transmitting: waiting for guarded tones and collecting the selected IQ frames…"); break;
        case State::Releasing: message = tr("Stop requested; waiting for confirmed release of this transmission."); break;
        case State::Analyzing: message = tr("Transmission released. Analyzing…"); break;
        }
    }
    if (model && !idle) { message += QLatin1Char('\n') + model->preparationSummary(); }
    if (m_state->text() != message) {
        m_state->setText(message);
        if (!alive) { return; }
        m_state->setAccessibleName(message);
        if (!alive) { return; }
        QAccessibleEvent event(m_state, QAccessible::NameChanged);
        QAccessible::updateAccessibility(&event);
        if (!alive) { return; }
    }
    const QString summary = model ? model->resultSummary() : tr("No result for this connection.");
    if (m_result->toPlainText() != summary) {
        m_result->setPlainText(summary);
        if (!alive) { return; }
    }
    const AetherPDMeasurementModel::Result* result = model && model->result() ? &*model->result() : nullptr;
    const QDateTime completed = result ? result->completedAtUtc : QDateTime{};
    const quint64 sourceId = result ? result->sourceId : 0;
    if (completed != m_spectrumCompletedAt || sourceId != m_spectrumSourceId) {
        m_spectrumCompletedAt = completed;
        m_spectrumSourceId = sourceId;
        m_spectrum->setResult(result);
    }
}
void AetherPDMeasurementDialog::exportResult()
{
    if (!current()) { return; }
    const QByteArray data = m_radio->aetherPDMeasurementModel().exportResult();
    const QByteArray csv = m_radio->aetherPDMeasurementModel().exportResultCsv();
    const QByteArray png = m_spectrum->exportPng();
    if (data.isEmpty()) { return; }
    auto* dialog = new AetherPDMeasurementExportDialog(data, csv, png, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &AetherPDMeasurementExportDialog::exportCompleted, this, [this](bool saved) {
        m_notice = saved ? tr("Measurement exported.") : tr("Export failed; the destination was not replaced.");
        render();
    });
    dialog->open();
}
} // namespace AetherSDR
