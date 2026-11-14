#include "FlexBackend.h"

namespace AetherSDR {
namespace {
FlexBackend::CommandReply serviceReply(const TransmitServiceCompletion& completion)
{
    return [completion](int code, const QString& body) {
        if (completion) {
            using Outcome = TransmitServiceReply::Outcome;
            completion({code == 0 ? Outcome::Accepted : code == -1 ? Outcome::Unknown
                        : code == 0x50000063 ? Outcome::NotDispatched : Outcome::Rejected,
                        static_cast<uint>(code), body});
        }
    };
}
}

ReceiveDispatch FlexBackend::requestDvk(const DvkRequest& request,
    const TransmitServiceCompletion& completion, const TxCoordinator::Command& fence,
    const RadioControlFence& current)
{
    if (!request.valid()) { return ReceiveDispatch::Unsupported; }
    using Operation = DvkRequest::Operation;
    QString command;
    switch (request.operation) {
    case Operation::RecordStart: command = QStringLiteral("dvk rec_start id=%1").arg(request.slot); break;
    case Operation::RecordStop: command = QStringLiteral("dvk rec_stop"); break;
    case Operation::PreviewStart: command = QStringLiteral("dvk preview_start id=%1").arg(request.slot); break;
    case Operation::PreviewStop: command = QStringLiteral("dvk preview_stop"); break;
    case Operation::PlaybackStart: command = QStringLiteral("dvk playback_start id=%1").arg(request.slot); break;
    case Operation::PlaybackStop: command = QStringLiteral("dvk playback_stop"); break;
    case Operation::Clear: command = QStringLiteral("dvk clear id=%1").arg(request.slot); break;
    case Operation::Remove: command = QStringLiteral("dvk remove id=%1").arg(request.slot); break;
    case Operation::Name:
        command = QStringLiteral("dvk set_name name=\"%1\" id=%2").arg(request.name).arg(request.slot);
        break;
    }
    if (request.operation == Operation::PlaybackStart) {
        const auto sink = m_repliedTxSink;
        return sink && fence.keying && sink(command, fence, serviceReply(completion))
            ? ReceiveDispatch::Dispatched : ReceiveDispatch::Unsupported;
    }
    const auto sink = m_serviceCommandSink;
    return sink && sink(command, current, serviceReply(completion))
        ? ReceiveDispatch::Dispatched : ReceiveDispatch::Unsupported;
}

ReceiveDispatch FlexBackend::requestNavtex(const NavtexRequest& request,
    const TransmitServiceCompletion& completion, const TxCoordinator::Command& fence)
{
    if (!request.valid() || !fence.keying || !m_repliedTxSink) { return ReceiveDispatch::Unsupported; }
    QString escaped = request.text;
    escaped.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    escaped.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    QString command = QStringLiteral("navtex send tx_ident=%1 subject_indicator=%2")
        .arg(request.transmitter).arg(request.subject);
    if (request.serial) { command += QStringLiteral(" serial_num=%1").arg(*request.serial); }
    command += QStringLiteral(" msg_text=\"%1\"").arg(escaped);
    const auto sink = m_repliedTxSink;
    return sink(command, fence, serviceReply(completion))
        ? ReceiveDispatch::Dispatched : ReceiveDispatch::Unsupported;
}
} // namespace AetherSDR
