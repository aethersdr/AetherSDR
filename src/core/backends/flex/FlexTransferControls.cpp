#include "FlexBackend.h"

#include <QRegularExpression>
#include <QStringList>
#include <QTimer>

namespace AetherSDR {
namespace {

std::optional<quint16> profileTransferPort(const QString& replyBody)
{
    const QString trimmed = replyBody.trimmed();

    static const QRegularExpression keyedRe(
        QStringLiteral(R"((?:^|[\s,;|])(?:port|transfer_port|tcp_port)\s*[=:]?\s*(\d{2,5})(?=$|[^\d]))"),
        QRegularExpression::CaseInsensitiveOption);
    const auto keyed = keyedRe.match(trimmed);
    if (keyed.hasMatch()) {
        bool ok = false;
        const int port = keyed.captured(1).toInt(&ok);
        if (ok && port > 0 && port <= 65535) {
            return static_cast<quint16>(port);
        }
    }

    bool bareOk = false;
    const int barePort = trimmed.toInt(&bareOk);
    if (bareOk && barePort > 0 && barePort <= 65535) {
        return static_cast<quint16>(barePort);
    }

    static const QRegularExpression endpointRe(QStringLiteral(R"(:(\d{2,5})(?=$|[^\d]))"));
    const auto endpoint = endpointRe.match(trimmed);
    if (endpoint.hasMatch()) {
        bool ok = false;
        const int port = endpoint.captured(1).toInt(&ok);
        if (ok && port > 0 && port <= 65535) {
            return static_cast<quint16>(port);
        }
    }

    static const QRegularExpression re(QStringLiteral(R"((?:^|[^\d])(\d{2,5})(?:[^\d]|$))"));
    auto it = re.globalMatch(trimmed);
    std::optional<quint16> lastPort;
    while (it.hasNext()) {
        const auto match = it.next();
        bool ok = false;
        const int port = match.captured(1).toInt(&ok);
        if (ok && port > 0 && port <= 65535) {
            lastPort = static_cast<quint16>(port);
        }
    }
    return lastPort;
}

quint16 transferPort(const QString& body, FileTransferPurpose purpose)
{
    using Purpose = FileTransferPurpose;
    const bool profile = purpose == Purpose::ProfileMetadata || purpose == Purpose::ProfileImport
        || purpose == Purpose::ProfileExport;
    QString portText = body.trimmed();
    if (profile) {
        if (const auto port = profileTransferPort(portText)) { return *port; }
    }
    bool ok = false;
    const uint port = portText.toUInt(&ok);
    if (ok && port > 0 && port <= 65535) { return static_cast<quint16>(port); }
    if (purpose == Purpose::DvkDownload) { return 0; }
    // Legacy firmware may omit a usable port. These defaults match each
    // existing transfer path; a nonzero command refusal never reaches here.
    return purpose == Purpose::Firmware || purpose == Purpose::ProfileMetadata
        || purpose == Purpose::ProfileImport ? 4995 : 42607;
}

struct TransferNegotiation {
    QPointer<FlexBackend> backend;
    FlexBackend::TransferCommandSink sink;
    FileTransferGuard guard;
    RadioControlFence current;
    FileTransferCompletion completion;
    FileTransferPurpose purpose;
    QStringList commands;
    int step{0};
    bool finished{false};

    void finish(const RadioControlReply& reply)
    {
        if (finished) { return; }
        finished = true;
        if (!completion) { return; }
        if (!backend || !guard.current() || (current && !current())) {
            completion({{RadioControlReply::Kind::Cancelled, -1,
                QStringLiteral("Original transfer ended.")}, 0});
            return;
        }
        const quint16 port = reply.kind == RadioControlReply::Kind::Accepted
            ? transferPort(reply.message, purpose) : 0;
        if (reply.kind == RadioControlReply::Kind::Accepted && port == 0) {
            completion({{RadioControlReply::Kind::Failed, -1,
                QStringLiteral("Radio returned an invalid transfer port.")}, 0});
        } else {
            completion({reply, port});
        }
    }
};

bool sendTransferStep(const std::shared_ptr<TransferNegotiation>& operation)
{
    if (!operation->backend || !operation->guard.current()
        || (operation->current && !operation->current())) {
        operation->finish({RadioControlReply::Kind::Cancelled, -1,
                           QStringLiteral("Original transfer ended.")});
        return false;
    }
    const int step = ++operation->step;
    const bool firmwareUpload = operation->purpose == FileTransferPurpose::Firmware
        && step == operation->commands.size();
    if (firmwareUpload) { operation->guard.setFirmwarePending(true); }
    const RadioControlCompletion reply = [operation, step, firmwareUpload](const RadioControlReply& result) {
        if (operation->finished || operation->step != step) { return; }
        // A definitive radio refusal permits retry; cancellation or a missing
        // reply cannot establish whether the update request reached the radio.
        if (firmwareUpload && result.kind == RadioControlReply::Kind::Failed && result.code != -1) {
            operation->guard.setFirmwarePending(false);
        }
        if (result.kind != RadioControlReply::Kind::Accepted
            || step == operation->commands.size()) {
            operation->finish(result);
        } else {
            sendTransferStep(operation);
        }
    };
    QTimer::singleShot(10000, operation->backend, [operation, step] {
        if (!operation->finished && operation->step == step) {
            operation->finish({RadioControlReply::Kind::Failed, -1,
                QStringLiteral("Radio did not answer transfer setup.")});
        }
    });
    if (!operation->sink(operation->commands.at(step - 1), operation->guard, reply)) {
        if (firmwareUpload) { operation->guard.setFirmwarePending(false); }
        reply({RadioControlReply::Kind::Refused, -1,
               QStringLiteral("Transfer setup was not dispatched.")});
        return false;
    }
    return true;
}

} // namespace

ReceiveDispatch FlexBackend::requestWaveform(const WaveformRequest& request,
    const RadioControlCompletion& completion, const FileTransferGuard& guard,
    const RadioControlFence& current)
{
    const auto control = capabilities().waveformControl;
    if (!request.valid() || !control || !control->supports(request.action)
        || !m_transferSink || !guard.current() || (current && !current())) {
        return ReceiveDispatch::Unsupported;
    }
    const QString action = request.action == WaveformAction::Uninstall ? QStringLiteral("uninstall")
        : request.action == WaveformAction::RemoveContainer ? QStringLiteral("remove_container")
        : QStringLiteral("restart");
    const TransferCommandSink sink = m_transferSink;
    return sink(QStringLiteral("waveform %1 %2").arg(action, request.name), guard, completion)
        ? ReceiveDispatch::Dispatched : ReceiveDispatch::Unsupported;
}

ReceiveDispatch FlexBackend::requestFileTransfer(const FileTransferRequest& request,
    const FileTransferCompletion& completion, const FileTransferGuard& guard,
    const RadioControlFence& current)
{
    const auto control = capabilities().fileTransferControl;
    if (!request.valid() || !control || !control->supports(request.purpose)
        || !m_transferSink || !guard.current() || (current && !current())) {
        return ReceiveDispatch::Unsupported;
    }
    using Purpose = FileTransferPurpose;
    auto operation = std::make_shared<TransferNegotiation>();
    operation->backend = this;
    operation->sink = m_transferSink;
    operation->guard = guard;
    operation->current = current;
    operation->completion = completion;
    operation->purpose = request.purpose;
    QString kind;
    switch (request.purpose) {
    case Purpose::Firmware: case Purpose::LegacyWaveform:
        operation->commands.append(QStringLiteral("file filename %1").arg(request.fileName));
        kind = request.purpose == Purpose::Firmware ? QStringLiteral("update") : QStringLiteral("new_waveform");
        break;
    case Purpose::DockerWaveform: kind = QStringLiteral("waveform_docker_image"); break;
    case Purpose::ProfileMetadata: kind = QStringLiteral("db_meta_subset"); break;
    case Purpose::ProfileImport: kind = QStringLiteral("db_import"); break;
    case Purpose::ProfileExport:
        operation->commands.append(QStringLiteral("file download db_package"));
        break;
    case Purpose::DvkUpload:
        operation->commands.append(QStringLiteral("dvk upload id=%1").arg(request.slot));
        kind = QStringLiteral("dvk_recording");
        break;
    case Purpose::DvkDownload:
        operation->commands.append(QStringLiteral("dvk download id=%1").arg(request.slot));
        break;
    }
    if (!kind.isEmpty()) {
        QString command = QStringLiteral("file upload %1 %2").arg(request.size).arg(kind);
        if (request.purpose == Purpose::DockerWaveform) { command += QLatin1Char(' ') + request.fileName; }
        operation->commands.append(command);
    }
    return sendTransferStep(operation) ? ReceiveDispatch::Dispatched : ReceiveDispatch::Unsupported;
}

} // namespace AetherSDR
