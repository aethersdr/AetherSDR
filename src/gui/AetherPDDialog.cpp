#include "AetherPDDialog.h"

#include "CanonIndicators.h"
#include "models/AetherPDInstallationModel.h"
#include "core/TailnetShimTokenStore.h"

#include <QAccessible>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QHBoxLayout>
#include <QTimer>
#include <QVBoxLayout>
#include <optional>
#include <utility>

namespace AetherSDR {
namespace {
QString stateText(aetherpd::State state)
{
    using aetherpd::State;
    switch (state) {
    case State::Unavailable: return QObject::tr("Unavailable");
    case State::Stopped: return QObject::tr("Stopped — session cleanup confirmed");
    case State::CleanupUnconfirmed: return QObject::tr("Session ended — cleanup unconfirmed");
    case State::Starting: return QObject::tr("Starting");
    case State::Disabled: return QObject::tr("Correction disabled");
    case State::BlockedByRadioApd: return QObject::tr("Blocked by radio APD");
    case State::CalibrationArmed: return QObject::tr("Calibration armed — waiting for your transmission");
    case State::RecalibrationRequired: return QObject::tr("Calibration required");
    case State::Idle: return QObject::tr("Idle");
    case State::Correcting: return QObject::tr("Correcting");
    case State::Calibrating: return QObject::tr("Calibrating");
    }
    return QObject::tr("Unknown");
}
void setText(QLabel* label, const QString& text)
{
    const QPointer<QLabel> alive(label);
    if (label->text() != text) {
        label->setText(text);
        if (!alive) { return; }
        label->setAccessibleName(text);
        if (!alive) { return; }
        QAccessibleEvent event(label, QAccessible::NameChanged);
        QAccessible::updateAccessibility(&event);
    }
}
void enable(QPushButton* button, bool allowed, const QString& reason)
{
    const QPointer<QPushButton> alive(button);
    button->setEnabled(allowed);
    if (!alive) { return; }
    button->setToolTip(allowed ? QString{} : reason);
    if (!alive) { return; }
    button->setAccessibleDescription(allowed ? QString{} : reason);
}
} // namespace

AetherPDDialog::AetherPDDialog(RadioModel* radio, QWidget* parent)
    : AetherPDDialog(radio, [](const QString& serial, QObject* context,
                               std::function<void(const QString&)> done) {
        TailnetShimTokenStore::load(serial, context, std::move(done));
    }, parent)
{
}

AetherPDDialog::AetherPDDialog(RadioModel* radio, TokenLoader loadToken, QWidget* parent)
    : CanonWindow(tr("AetherPD"), parent)
    , m_radio(radio)
    , m_session(radio ? radio->controlSession() : RadioControlSession{})
    , m_serial(radio ? radio->serial() : QString{})
    , m_loadToken(std::move(loadToken))
{
    setObjectName(QStringLiteral("AetherPDDialog"));
    setReturnClicksDefault(false);
    setMinimumWidth(520);
    auto* layout = new QVBoxLayout(bodyWidget());
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(14);
    layout->addWidget(makeCanonHeader(tr("AetherPD")));
    auto* hint = new QLabel(tr("Experimental on-radio predistortion using DAX IQ feedback. "
        "Requires an installed AetherPD service and a calibrated feedback path."), bodyWidget());
    hint->setWordWrap(true);
    layout->addWidget(hint);
    const auto label = [this, layout](const char* name) {
        auto* value = new QLabel(bodyWidget());
        value->setObjectName(QString::fromLatin1(name));
        value->setTextFormat(Qt::PlainText);
        value->setWordWrap(true);
        layout->addWidget(value);
        return value;
    };
    m_state = label("pdState");
    m_detail = label("pdDetail");
    m_receipt = label("pdReceipt");
    m_message = label("pdMessage");
    auto* controls = new QWidget(bodyWidget());
    auto* buttons = new QGridLayout(controls);
    buttons->setContentsMargins(0, 0, 0, 0);
    const auto button = [this, buttons](const QString& text, const char* name, int row, int col) {
        auto* control = new QPushButton(text, bodyWidget());
        control->setObjectName(QString::fromLatin1(name));
        control->setAccessibleName(text);
        control->setAutoDefault(false);
        buttons->addWidget(control, row, col);
        return control;
    };
    m_start = button(tr("Start AetherPD"), "pdStart", 0, 0);
    m_stop = button(tr("Stop AetherPD"), "pdStop", 0, 1);
    m_arm = button(tr("Arm calibration"), "pdArm", 1, 0);
    m_cancel = button(tr("Cancel calibration"), "pdCancel", 1, 1);
    m_restore = button(tr("Restore receive"), "pdRestore", 2, 0);
    m_refresh = button(tr("Refresh"), "pdRefresh", 2, 1);
    auto* done = new QPushButton(tr("Done"), bodyWidget());
    done->setObjectName(QStringLiteral("pdDone"));
    done->setAccessibleName(tr("Done"));
    done->setAutoDefault(false);
    layout->addWidget(controls);
    auto* explanation = new QLabel(tr("Arm calibration prepares a bounded calibration signal for your next "
        "deliberate transmission. Use your normal PTT control to transmit. Closing this window "
        "does not stop AetherPD. Restore receive requests the saved receive state after a failed cleanup."), bodyWidget());
    explanation->setWordWrap(true);
    layout->addWidget(explanation);
    auto* installationTitle = new QLabel(tr("AetherPD service update"), bodyWidget());
    layout->addWidget(installationTitle);
    m_installState = label("pdInstallState");
    m_installPlan = new QPlainTextEdit(bodyWidget());
    m_installPlan->setObjectName(QStringLiteral("pdInstallPlan"));
    m_installPlan->setAccessibleName(tr("AetherPD installation plan"));
    m_installPlan->setAccessibleDescription(tr("Read-only target, changes and authenticated package identifiers to review before approving."));
    m_installPlan->setReadOnly(true);
    m_installPlan->setMinimumHeight(140);
    m_installPlan->setMaximumHeight(150);
    layout->addWidget(m_installPlan);
    auto* installControls = new QWidget(bodyWidget());
    auto* installButtons = new QHBoxLayout(installControls);
    installButtons->setContentsMargins(0, 0, 0, 0);
    m_installPrepare = new QPushButton(tr("Prepare update"), bodyWidget());
    m_installPrepare->setObjectName(QStringLiteral("pdInstallPrepare"));
    m_installPrepare->setAccessibleName(tr("Prepare AetherPD development update"));
    m_installPrepare->setAutoDefault(false);
    layout->addWidget(m_installPrepare);
    m_installApprove = new QPushButton(tr("Approve update"), bodyWidget());
    m_installApprove->setObjectName(QStringLiteral("pdInstallApprove"));
    m_installApprove->setAccessibleName(tr("Approve AetherPD update"));
    m_installApprove->setAutoDefault(false);
    m_installCancel = new QPushButton(tr("Cancel update"), bodyWidget());
    m_installCancel->setObjectName(QStringLiteral("pdInstallCancel"));
    m_installCancel->setAccessibleName(tr("Cancel AetherPD update"));
    m_installCancel->setAutoDefault(false);
    installButtons->addWidget(m_installCancel);
    installButtons->addWidget(m_installApprove);
    installButtons->addStretch();
    installButtons->addWidget(done);
    layout->addWidget(installControls);
    connect(m_installPrepare, &QPushButton::clicked, this, &AetherPDDialog::prepareInstallation);
    connect(m_installApprove, &QPushButton::clicked, this, &AetherPDDialog::approveInstallation);
    connect(m_installCancel, &QPushButton::clicked, this, &AetherPDDialog::cancelInstallation);
    m_installRefresh = new QTimer(this);
    m_installRefresh->setSingleShot(true);
    m_installRefresh->setInterval(200);
    connect(m_installRefresh, &QTimer::timeout, this, &AetherPDDialog::renderInstallation);
    if (radio) {
        connect(&radio->aetherPDInstallationModel(), &AetherPDInstallationModel::changed, this, [this] {
            if (!m_radio) { return; }
            if (m_radio->aetherPDInstallationModel().state() == AetherPDInstallationModel::State::Downloading) {
                if (!m_installRefresh->isActive()) {
                    const QPointer<AetherPDDialog> alive(this);
                    render();
                    if (alive) { m_installRefresh->start(); }
                }
            } else { m_installRefresh->stop(); render(); }
        });
        connect(radio, &RadioModel::radioTransmittingChanged, this, &AetherPDDialog::renderInstallation);
    }
    applyCanonSheet(bodyWidget(), [] {
        return QStringLiteral(
            "QLabel { color: {{color.canon.inkSoft}}; background: transparent; }"
            "QLabel#pdState { color: {{color.canon.ink}}; font-weight: bold; font-size: 16px; }"
            "QLabel#pdMessage { color: {{color.accent.warning}}; }"
            "QPlainTextEdit { color: {{color.canon.inkSoft}}; background: {{color.canon.nested}}; border: 1px solid {{color.canon.line}}; }"
            "QPushButton { background: {{color.canon.control}}; color: {{color.canon.cyan}};"
            " border: 1px solid {{color.canon.lineHi}}; border-radius: 4px; padding: 6px 12px; }"
            "QPushButton:hover { background: {{color.canon.nested}}; color: {{color.canon.aqua}}; }"
            "QPushButton:focus { border-color: {{color.canon.aqua}}; }"
            "QPushButton:disabled { color: {{color.canon.muted}}; border-color: {{color.canon.line}}; }");
    });
    // Keep the nested grid from compressing styled buttons below their text.
    for (QPushButton* control : bodyWidget()->findChildren<QPushButton*>()) {
        control->ensurePolished();
        control->setMinimumHeight(control->minimumSizeHint().height());
    }
    buttons->invalidate();
    layout->invalidate();
    connect(done, &QPushButton::clicked, this, &QDialog::close);
    connect(m_refresh, &QPushButton::clicked, this, &AetherPDDialog::refresh);
    connect(m_start, &QPushButton::clicked, this, [this] { request(aetherpd::Operation::Start); });
    connect(m_arm, &QPushButton::clicked, this, [this] { request(aetherpd::Operation::ArmCalibration); });
    connect(m_cancel, &QPushButton::clicked, this, [this] { request(aetherpd::Operation::CancelCalibration); });
    connect(m_stop, &QPushButton::clicked, this, [this] { request(aetherpd::Operation::Stop); });
    connect(m_restore, &QPushButton::clicked, this, [this] { request(aetherpd::Operation::RestoreReceive); });
    if (radio) {
        connect(&radio->aetherPDModel(), &AetherPDModel::changed, this, &AetherPDDialog::render);
        connect(radio, &RadioModel::connectionStateChanged, this, &AetherPDDialog::render);
        connect(radio, &QObject::destroyed, this, &AetherPDDialog::render);
    }
    render();
    QTimer::singleShot(0, this, &AetherPDDialog::refresh);
}

bool AetherPDDialog::current() const
{
    return !m_closed && m_radio && !m_serial.isEmpty()
        && m_radio->controlSessionCurrent(m_session) && m_radio->serial() == m_serial;
}

void AetherPDDialog::refresh()
{
    if (!current()) {
        render();
        return;
    }
    m_notice.clear();
    if (m_radio->aetherPDModel().active()) {
        request(aetherpd::Operation::Status);
        return;
    }
    if (m_loading || !m_loadToken) {
        return;
    }
    m_loading = true;
    const quint64 generation = ++m_loadGeneration;
    const QPointer<AetherPDDialog> self(this);
    const TokenLoader loadToken = m_loadToken;
    const QString serial = m_serial;
    render();
    if (!self || !current()) {
        return;
    }
    loadToken(serial, this, [self, generation](const QString& token) {
        if (!self || generation != self->m_loadGeneration || !self->current()) {
            return;
        }
        ++self->m_loadGeneration;
        self->m_loading = false;
        if (token.isEmpty()) {
            self->m_notice = tr("No credential is stored for this radio. Set up its Remote Access credentials on this computer.");
        } else {
            const bool attached = self->m_radio->attachAetherPD(self->m_session, token);
            if (!self) {
                return;
            }
            if (!attached) {
                self->m_notice = tr("AetherPD could not attach to this radio session. Reopen this window after the connection is ready.");
            }
        }
        if (self) {
            self->render();
        }
    });
}

void AetherPDDialog::request(aetherpd::Operation operation)
{
    if (!current()) {
        render();
        return;
    }
    m_notice.clear();
    const QPointer<AetherPDDialog> self(this);
    const bool admitted = m_radio->requestAetherPD(m_session, operation);
    if (!self) {
        return;
    }
    if (!admitted) {
        m_notice = tr("Request not admitted. Wait for current status and check the connection.");
    }
    render();
}

void AetherPDDialog::render()
{
    const bool valid = current();
    const AetherPDModel* model = valid ? &m_radio->aetherPDModel() : nullptr;
    const bool active = model && model->active();
    const bool installing = valid && m_radio->aetherPDInstallationModel().active();
    const std::optional<aetherpd::Status> status = active ? model->status() : std::nullopt;
    const std::optional<aetherpd::Operation> pending = active ? model->pendingOperation() : std::nullopt;
    using aetherpd::Operation;
    const QString unavailable = tr("A current authenticated radio session is required.");
    QString receipt;
    if (model && model->lastAcknowledgment()) {
        using aetherpd::Acknowledgment;
        switch (*model->lastAcknowledgment()) {
        case Acknowledgment::StartRequested: receipt = tr("Start requested; waiting for observed status."); break;
        case Acknowledgment::ArmAccepted: receipt = tr("Arm request accepted; waiting for observed status."); break;
        case Acknowledgment::CancelAccepted: receipt = tr("Cancel request accepted; waiting for observed status."); break;
        case Acknowledgment::StopRequested:
            receipt = status && status->state == aetherpd::State::Stopped
                ? tr("Stop requested; the session supervisor confirms cleanup completed.")
                : tr("Stop requested; shutdown and unkey are not yet confirmed here.");
            break;
        case Acknowledgment::ReceiveRestored: receipt = tr("Service reports receive restoration completed."); break;
        }
    }
    QString message = m_notice;
    if (message.isEmpty() && model && model->lastFailure()) {
        message = *model->lastFailure() == AetherPDClient::Failure::Unauthorized
            ? tr("The radio rejected this credential. Refresh after updating its Remote Access credentials.")
            : tr("No current status is available. The request may have failed or its reply may have been lost; controls are not retried automatically.");
    }
    const QPointer<AetherPDDialog> alive(this);
    enable(m_refresh, valid && !m_loading && !pending, tr("Wait for the pending request, or reopen this window for the current connection."));
    if (!alive) { return; }
    enable(m_start, active && !installing && (!pending || pending == Operation::Status) && (!status || !status->available),
        installing ? tr("Wait for the service update to finish before starting AetherPD.")
            : tr("Authenticate this radio session and stop any running AetherPD session before starting."));
    if (!alive) { return; }
    const bool arm = status && status->available && status->registered && !status->keyed
        && status->correctionEnabled && status->calibrationControl
        && status->state != aetherpd::State::BlockedByRadioApd
        && status->calibrationState != aetherpd::CalibrationState::Armed
        && status->calibrationState != aetherpd::CalibrationState::Running && !pending;
    enable(m_arm, active && !installing && arm, installing ? tr("Wait for the service update to finish before arming calibration.")
        : tr("Fresh, unkeyed status with calibration available is required. Radio APD must be off."));
    if (!alive) { return; }
    enable(m_cancel, active && (!pending || pending == Operation::Status || pending == Operation::ArmCalibration), unavailable);
    if (!alive) { return; }
    enable(m_stop, active && pending != Operation::Stop, unavailable);
    if (!alive) { return; }
    enable(m_restore, active && !installing && !pending, installing ? tr("Wait for the service update to finish before requesting receive restoration.")
        : tr("Wait for the current request. The service verifies that receive restoration is safe."));
    if (!alive) { return; }
    setText(m_state, !valid ? tr("Connection ended — reopen AetherPD for the current radio")
        : m_loading ? tr("Loading radio credential…") : status ? stateText(status->state) : tr("Status unknown"));
    if (!alive) { return; }
    QString detail;
    if (status && status->available) {
        detail = tr("Dispatch faults: %1 · Send errors: %2 · Rejected input: %3 · Stale IQ: %4\nInput-limited samples: %5")
            .arg(status->faults.dispatchDeadlines).arg(status->faults.sendErrors)
            .arg(status->faults.rejectedInput).arg(status->faults.staleIq).arg(status->inputLimitedSamples);
    } else if (status && status->state == aetherpd::State::Stopped) {
        detail = tr("The owned DSP and session cleanup workers exited cleanly. This reports that session's completion, not the radio's current transmit state.");
    } else if (status && status->state == aetherpd::State::CleanupUnconfirmed) {
        detail = tr("The session supervisor reported an error or cleanup deadline. Shutdown and unkey are not confirmed.");
    }
    setText(m_detail, detail);
    if (!alive) { return; }
    setText(m_receipt, receipt);
    if (!alive) { return; }
    setText(m_message, message);
    if (!alive) { return; }
    renderInstallation();
}

void AetherPDDialog::prepareInstallation()
{
    if (current()) { m_radio->aetherPDInstallationModel().prepareSelectedDevelopment(m_session); }
}
void AetherPDDialog::approveInstallation()
{
    if (!current()) { return; }
    // A click approves the exact preview, even if another producer replaces it.
    m_radio->aetherPDInstallationModel().approve(m_displayedPlan);
}
void AetherPDDialog::cancelInstallation()
{
    if (current()) { m_radio->aetherPDInstallationModel().cancel(); }
}
void AetherPDDialog::renderInstallation()
{
    using Model = AetherPDInstallationModel;
    using State = Model::State;
    using Outcome = Model::Coordinator::Outcome;
    using Phase = Model::Coordinator::State;
    const Model* model = current() ? &m_radio->aetherPDInstallationModel() : nullptr;
    const State state = model ? model->state() : State::Unavailable;
    const auto preview = model ? model->preview() : std::nullopt;
    const QString developmentVersion = model ? model->selectedDevelopmentVersion() : QString{};
    QString detail;
    QByteArray displayed;
    if (preview) {
        displayed = preview->plan;
        detail = tr("Radio: %1\nFirmware: %2\n\nChanges on approval:\n"
            "• Verify and replace the AetherPD payload and service.\n"
            "• Install fixed boot and local-session helpers for reboot and IQ setup.\n"
            "• Restart the shim while retaining its Remote Access identity.\n"
            "• Retire temporary delivery and upgrade workers. Fixed boot/session helpers remain installed.\n"
            "The update does not transmit or arm calibration.\n\n"
            "Temporary executables: pd-accept, pd-carrier, pd-send, pd-deliver, signed upgrade worker, pd-update.\n"
            "Permanent helper: pd-boot.\n"
            "Fixed units: aetherpd-boot.service, aetherpd-start.socket, aetherpd-start@.service, aetherpd-shim.service.\n\n"
            "Previous release (BLAKE2b-256): %3\nNew release (BLAKE2b-256): %4\n"
            "Publisher key (Ed25519): %5\nAttempt (BLAKE2b-256): %6\n"
            "Temporary carrier: %7\nCarrier archive (SHA-256): %8")
            .arg(preview->serial, preview->firmware, preview->previousDigest, preview->candidateDigest,
                 preview->candidateKey, preview->planDigest, preview->carrierName, preview->archiveSha256);
    } else {
        detail = tr("No verified update is prepared. Qualified package selection is not yet available in this development build. "
            "Installing the released Remote Access shim alone does not install AetherPD.");
    }
    if (!developmentVersion.isEmpty()) {
        const QString selection = tr("Development package %1 — not qualified for production.\n").arg(developmentVersion);
        detail = preview ? tr("Development build — not qualified for production.\n") + detail
            : selection + tr("Choose Prepare update to verify the package and review its plan. Preparation does not change the radio.");
    }
    QString status;
    switch (state) {
    case State::Idle: status = tr("No update prepared"); break;
    case State::Downloading:
        status = tr("Downloading verified package: %1 / %2 bytes").arg(model->downloadReceived()).arg(model->downloadTotal()); break;
    case State::Preparing: status = tr("Preparing the installation plan…"); break;
    case State::AwaitingApproval:
        status = m_radio->isRadioTransmitting() ? tr("Review the plan; approval is unavailable while transmitting.")
            : tr("Review the plan, then choose Approve update. Nothing is installed before approval."); break;
    case State::LoadingCredential: status = tr("Loading the saved radio credential…"); break;
    case State::Running:
        switch (model->phase()) {
        case Phase::Idle: case Phase::Requesting: status = tr("Requesting the temporary installer…"); break;
        case Phase::UploadingCarrier: status = tr("Uploading the temporary installer…"); break;
        case Phase::WaitingForCarrier: status = tr("Waiting for the radio to publish the installer…"); break;
        case Phase::UploadingInputs: status = tr("Transferring authenticated package files…"); break;
        case Phase::WaitingForCommit: status = tr("Waiting for the radio to confirm the update…"); break;
        case Phase::Cleaning: status = tr("Removing temporary installer components…"); break;
        case Phase::Finished: status = tr("Finishing update observation…"); break;
        }
        break;
    case State::Canceling: status = tr("Cancellation requested; waiting for installer cleanup. This does not guarantee rollback."); break;
    case State::Finished:
        switch (model->outcome().value_or(Outcome::Unconfirmed)) {
        case Outcome::Committed: status = tr("Radio confirmed the update."); break;
        case Outcome::RolledBack: status = tr("Radio confirmed rollback to the previous payload."); break;
        case Outcome::Canceled: status = model->phase() == Phase::Idle
            ? tr("Update canceled before installation started. No radio changes were requested.")
            : tr("Update canceled locally; an already-started update may still finish."); break;
        case Outcome::Unconfirmed: status = tr("Update outcome unconfirmed. Do not assume it succeeded or rolled back."); break;
        }
        if (model->phase() != Phase::Idle) {
            status += model->cleanupConfirmed() ? tr(" Temporary installer cleanup confirmed.") : tr(" Temporary installer cleanup is unconfirmed.");
        }
        break;
    case State::Unavailable:
        if (!model) { status = tr("Connection ended — reopen AetherPD for the current radio."); break; }
        switch (model->failure()) {
        case Model::Failure::SessionChanged: status = tr("Radio connection changed; prepare a new update for the current connection."); break;
        case Model::Failure::NoCredential: status = tr("No saved Remote Access credential for this radio. Update credentials before preparing another attempt."); break;
        case Model::Failure::CredentialTimeout: status = tr("Credential lookup timed out; no installation was started."); break;
        case Model::Failure::DownloadFailed: status = tr("Package download or verification failed; no installation was started."); break;
        case Model::Failure::PackageRejected: status = tr("Package preparation failed; no installation was started."); break;
        default: status = tr("Update could not start. Check the connection, transmit state and upload availability."); break;
        }
        break;
    }
    const bool approve = model && state == State::AwaitingApproval && preview && !m_radio->isRadioTransmitting();
    const bool cancel = model && (state == State::Downloading || state == State::Preparing || state == State::AwaitingApproval
        || state == State::LoadingCredential || state == State::Running);
    const QPointer<AetherPDDialog> alive(this);
    enable(m_installPrepare, model && !developmentVersion.isEmpty() && !model->active() && state != State::AwaitingApproval,
        tr("A development package compiled for this radio firmware and no pending update are required."));
    if (!alive) { return; }
    enable(m_installApprove, approve, tr("A verified plan for this connection and an idle transmitter are required."));
    if (!alive) { return; }
    enable(m_installCancel, cancel, tr("No pending update can be canceled in this connection."));
    if (!alive) { return; }
    setText(m_installState, status);
    if (!alive) { return; }
    if (m_installPlan->toPlainText() != detail) { m_installPlan->setPlainText(detail); }
    if (!alive) { return; }
    m_displayedPlan = displayed;
    // QLabel wrapping needs more height than the layout's width-free minimum.
    // Reserve the height at the narrowest supported width before Qt caps the
    // initial dialog size to a fraction of the available screen.
    const int wrappedHeight = layout()->totalHeightForWidth(minimumWidth());
    if (wrappedHeight > 0) { setMinimumHeight(wrappedHeight); }
}

void AetherPDDialog::done(int result)
{
    m_closed = true;
    ++m_loadGeneration;
    CanonWindow::done(result);
}

} // namespace AetherSDR
