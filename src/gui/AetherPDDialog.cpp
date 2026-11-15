#include "AetherPDDialog.h"

#include "CanonIndicators.h"
#include "core/TailnetShimTokenStore.h"

#include <QAccessible>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
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
    auto* buttons = new QGridLayout;
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
    QPushButton* done = button(tr("Done"), "pdDone", 3, 1);
    layout->addLayout(buttons);
    auto* explanation = new QLabel(tr("Arm calibration prepares a bounded calibration signal for your next "
        "deliberate transmission. Use your normal PTT control to transmit. Closing this window "
        "does not stop AetherPD. Restore receive requests the saved receive state after a failed cleanup."), bodyWidget());
    explanation->setWordWrap(true);
    layout->addWidget(explanation);
    applyCanonSheet(bodyWidget(), [] {
        return QStringLiteral(
            "QLabel { color: {{color.canon.inkSoft}}; background: transparent; }"
            "QLabel#pdState { color: {{color.canon.ink}}; font-weight: bold; font-size: 16px; }"
            "QLabel#pdMessage { color: {{color.accent.warning}}; }"
            "QPushButton { background: {{color.canon.control}}; color: {{color.canon.cyan}};"
            " border: 1px solid {{color.canon.lineHi}}; border-radius: 4px; padding: 6px 12px; }"
            "QPushButton:hover { background: {{color.canon.nested}}; color: {{color.canon.aqua}}; }"
            "QPushButton:focus { border-color: {{color.canon.aqua}}; }"
            "QPushButton:disabled { color: {{color.canon.muted}}; border-color: {{color.canon.line}}; }");
    });
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
        case Acknowledgment::StopRequested: receipt = tr("Stop requested; shutdown and unkey are not yet confirmed here."); break;
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
    enable(m_start, active && (!pending || pending == Operation::Status) && (!status || !status->available),
        tr("Authenticate this radio session and stop any running AetherPD session before starting."));
    if (!alive) { return; }
    const bool arm = status && status->available && status->registered && !status->keyed
        && status->correctionEnabled && status->calibrationControl
        && status->state != aetherpd::State::BlockedByRadioApd
        && status->calibrationState != aetherpd::CalibrationState::Armed
        && status->calibrationState != aetherpd::CalibrationState::Running && !pending;
    enable(m_arm, active && arm, tr("Fresh, unkeyed status with calibration available is required. Radio APD must be off."));
    if (!alive) { return; }
    enable(m_cancel, active && (!pending || pending == Operation::Status || pending == Operation::ArmCalibration), unavailable);
    if (!alive) { return; }
    enable(m_stop, active && pending != Operation::Stop, unavailable);
    if (!alive) { return; }
    enable(m_restore, active && !pending, tr("Wait for the current request. The service verifies that receive restoration is safe."));
    if (!alive) { return; }
    setText(m_state, !valid ? tr("Connection ended — reopen AetherPD for the current radio")
        : m_loading ? tr("Loading radio credential…") : status ? stateText(status->state) : tr("Status unknown"));
    if (!alive) { return; }
    setText(m_detail, status ? tr("Dispatch faults: %1 · Send errors: %2 · Rejected input: %3 · Stale IQ: %4\nInput-limited samples: %5")
        .arg(status->faults.dispatchDeadlines).arg(status->faults.sendErrors)
        .arg(status->faults.rejectedInput).arg(status->faults.staleIq).arg(status->inputLimitedSamples) : QString{});
    if (!alive) { return; }
    setText(m_receipt, receipt);
    if (!alive) { return; }
    setText(m_message, message);
}

void AetherPDDialog::done(int result)
{
    m_closed = true;
    ++m_loadGeneration;
    CanonWindow::done(result);
}

} // namespace AetherSDR
