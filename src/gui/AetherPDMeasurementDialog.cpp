#include "AetherPDMeasurementDialog.h"
#include "CanonIndicators.h"
#include "core/TxKeyingMarker.h"
#include "models/AetherPDMeasurementModel.h"
#include "models/AetherPDInstallationModel.h"

#include <QAccessible>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QGridLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QVBoxLayout>

namespace AetherSDR {
namespace {
void availability(QPushButton* button, bool enabled, const QString& reason)
{
    const QPointer<QPushButton> alive(button);
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
        "attenuated RXA feedback before arming. The source runs four seconds of alignment noise, then "
        "710/1900 Hz tones, for at most eight seconds. Prepare checks reception; only Transmit keys RF."), bodyWidget());
    hint->setWordWrap(true);
    layout->addWidget(hint);
    m_dummyLoad = new QCheckBox(tr("Dummy load connected; feedback attenuation checked"), bodyWidget());
    m_dummyLoad->setObjectName(QStringLiteral("pdMeasureDummyLoad"));
    m_dummyLoad->setAccessibleName(m_dummyLoad->text());
    layout->addWidget(m_dummyLoad);
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
    auto* grid = new QGridLayout;
    const auto button = [this, grid](const QString& text, const char* name, int row, int col) {
        auto* result = new QPushButton(text, bodyWidget());
        result->setObjectName(QString::fromLatin1(name));
        result->setAccessibleName(text);
        result->setAutoDefault(false);
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
            "QPlainTextEdit,QDoubleSpinBox { color: {{color.canon.inkSoft}}; background: {{color.canon.nested}}; border: 1px solid {{color.canon.line}}; }"
            "QPushButton { background: {{color.canon.control}}; color: {{color.canon.cyan}}; border: 1px solid {{color.canon.lineHi}}; padding: 6px; }"
            "QPushButton:disabled { color: {{color.canon.muted}}; }"
            "QPushButton:hover,QPushButton:focus { border-color: {{color.canon.aqua}}; }") + canonIndicatorRules() + canonSpinArrowRules();
    });
    connect(m_arm, &QPushButton::clicked, this, [this] {
        if (current()) { m_radio->requestAetherPD(m_session, aetherpd::Operation::ArmMeasurement); }
    });
    connect(m_cancelSource, &QPushButton::clicked, this, [this] {
        if (current()) { m_radio->requestAetherPD(m_session, aetherpd::Operation::CancelCalibration); }
    });
    connect(m_prepare, &QPushButton::clicked, this, &AetherPDMeasurementDialog::prepare);
    connect(m_stop, &QPushButton::clicked, this, &AetherPDMeasurementDialog::stop);
    connect(m_export, &QPushButton::clicked, this, &AetherPDMeasurementDialog::exportResult);
    connect(m_dummyLoad, &QCheckBox::toggled, this, &AetherPDMeasurementDialog::render);
    connect(m_limit, &QDoubleSpinBox::valueChanged, this, &AetherPDMeasurementDialog::render);
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
    if (!accepted) { m_notice = tr("Transmission refused. Prepare a fresh receive check before trying again."); }
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
    const bool busy = valid && m_radio->aetherPDModel().pendingOperation().has_value();
    const bool installed = valid && !m_radio->aetherPDInstallationModel().active();
    const bool idle = state == State::Idle;
    const bool readySource = source && source->available && source->registered && source->measurementControl && !source->keyed
        && !m_radio->fullDuplexEnabled() && source->state != aetherpd::State::BlockedByRadioApd;
    availability(m_arm, installed && idle && readySource && !busy && m_dummyLoad->isChecked()
        && source->calibrationState != aetherpd::CalibrationState::Armed && source->calibrationState != aetherpd::CalibrationState::Running,
        tr("Confirm the bench path and wait for a fresh, unkeyed service with measurement support."));
    if (!alive) { return; }
    availability(m_cancelSource, valid && source && source->measurementControl,
        tr("A current authenticated measurement-capable service is required."));
    if (!alive) { return; }
    availability(m_prepare, installed && idle && readySource && !busy && m_dummyLoad->isChecked() && m_limit->value() > 0
        && source->measurement && source->measurement->sourceId && source->sourcePurpose == aetherpd::SourcePurpose::Measurement
        && source->calibrationState == aetherpd::CalibrationState::Armed,
        tr("Arm the measurement source, confirm the bench path and choose a forward-power limit."));
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
        case State::Capturing: message = tr("Transmitting: waiting for guarded tones and collecting four IQ frames…"); break;
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
    if (m_result->toPlainText() != summary) { m_result->setPlainText(summary); }
}
void AetherPDMeasurementDialog::exportResult()
{
    if (!current()) { return; }
    const QByteArray data = m_radio->aetherPDMeasurementModel().exportResult();
    if (data.isEmpty()) { return; }
    auto* dialog = new QFileDialog(this, tr("Export AetherPD measurement"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setAcceptMode(QFileDialog::AcceptSave);
    dialog->setNameFilter(tr("JSON measurement (*.json)"));
    dialog->setDefaultSuffix(QStringLiteral("json"));
    connect(dialog, &QFileDialog::fileSelected, this, [this, data](const QString& path) {
        QSaveFile file(path);
        m_notice = file.open(QIODevice::WriteOnly) && file.write(data) == data.size() && file.commit()
            ? tr("Measurement exported.") : tr("Export failed; the destination was not replaced.");
        render();
    });
    dialog->open();
}
} // namespace AetherSDR
