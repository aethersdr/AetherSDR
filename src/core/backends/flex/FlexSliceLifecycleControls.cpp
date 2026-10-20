#include "FlexBackend.h"

#include <QRegularExpression>
#include <QStringList>
#include <utility>

namespace AetherSDR {
namespace {
bool lifecycleToken(const QString& text)
{
    static const QRegularExpression token(QStringLiteral("^[A-Za-z0-9_./-]{1,64}$"));
    return token.match(text).hasMatch();
}
}

ReceiveDispatch FlexBackend::requestSliceCreate(const SliceCreateRequest& request,
                                                const SliceCreateCompletion& completion,
                                                const RadioControlFence& current)
{
    static const QRegularExpression pan(QStringLiteral("^0x[0-9a-fA-F]{1,8}$"));
    if (!request.valid() || !pan.match(request.panId).hasMatch() || !m_setupSink
        || (request.mode && !lifecycleToken(*request.mode))
        || (request.rxAntenna && !lifecycleToken(*request.rxAntenna))
        || (current && !current())) {
        return ReceiveDispatch::Unsupported;
    }
    QString command = QStringLiteral("slice create pan=%1 freq=%2")
        .arg(request.panId).arg(request.frequencyHz / 1.0e6, 0, 'f', 6);
    if (request.mode) { command += QStringLiteral(" mode=") + *request.mode; }
    if (request.rxAntenna) { command += QStringLiteral(" rxant=") + *request.rxAntenna; }
    const RadioControlCompletion reply = [completion](const RadioControlReply& result) {
        if (!completion) { return; }
        if (result.kind != RadioControlReply::Kind::Accepted) {
            completion({result, {}});
            return;
        }
        // FlexLib 4.3.16 HandleRequestSliceBlockingReply: ID before debug text.
        bool valid = false;
        const int id = result.message.section(QLatin1Char('|'), 0, 0).trimmed().toInt(&valid);
        if (!valid || id < 0) {
            completion({{RadioControlReply::Kind::Failed, -1,
                         QStringLiteral("Invalid slice-create reply")}, {}});
            return;
        }
        completion({result, id});
    };
    const auto sink = m_setupSink;
    return sink(command, reply) ? ReceiveDispatch::Dispatched : ReceiveDispatch::Unsupported;
}

ReceiveDispatch FlexBackend::requestSliceRemove(int sliceId,
                                                const RadioControlCompletion& completion,
                                                const RadioControlFence& current)
{
    if (sliceId < 0 || !m_setupSink || (current && !current())) {
        return ReceiveDispatch::Unsupported;
    }
    const auto sink = m_setupSink;
    return sink(QStringLiteral("slice remove %1").arg(sliceId), completion)
        ? ReceiveDispatch::Dispatched : ReceiveDispatch::Unsupported;
}

ReceiveDispatch FlexBackend::requestTxSlice(int sliceId, bool enabled,
                                           const RadioControlCompletion& completion,
                                           const RadioControlFence& current)
{
    if (sliceId < 0 || !m_setupSink || (current && !current())) {
        return ReceiveDispatch::Unsupported;
    }
    const auto sink = m_setupSink;
    return sink(QStringLiteral("slice set %1 tx=%2").arg(sliceId).arg(int(enabled)), completion)
        ? ReceiveDispatch::Dispatched : ReceiveDispatch::Unsupported;
}

ReceiveDispatch FlexBackend::requestFullDuplex(bool enabled,
                                              const RadioControlCompletion& completion,
                                              const RadioControlFence& current)
{
    if (!m_setupSink || (current && !current())) { return ReceiveDispatch::Unsupported; }
    const auto sink = m_setupSink;
    return sink(QStringLiteral("radio set full_duplex_enabled=%1").arg(int(enabled)), completion)
        ? ReceiveDispatch::Dispatched : ReceiveDispatch::Unsupported;
}

ReceiveDispatch FlexBackend::requestSliceInventoryRefresh(const RadioControlCompletion& completion,
                                                         const RadioControlFence& current)
{
    if (!m_setupSink || (current && !current())) { return ReceiveDispatch::Unsupported; }
    const auto sink = m_setupSink;
    return sink(QStringLiteral("sub slice all"), completion)
        ? ReceiveDispatch::Dispatched : ReceiveDispatch::Unsupported;
}

} // namespace AetherSDR
