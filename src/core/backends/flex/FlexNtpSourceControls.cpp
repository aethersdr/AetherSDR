#include "core/backends/flex/FlexBackend.h"

#include <QStringList>

namespace AetherSDR {

// FlexLib v4.3.16: Radio.cs:14546 RequestNtpSource, NtpSource.cs:104 Remove and
// :301 Enabled, Radio.cs:2348 for the subscription.
ReceiveDispatch FlexBackend::requestNtpSource(const NtpSourceRequest& request,
                                             const RadioControlCompletion& completion,
                                             const RadioControlFence& current)
{
    using Action = NtpSourceRequest::Action;
    if (!request.valid() || !m_setupSink || (current && !current())
        || !capabilities().ntpSourceControl.has_value()) {
        return ReceiveDispatch::Unsupported;
    }
    const NtpSourceDefinition& d = request.definition;
    QString command;
    switch (request.action) {
    case Action::Subscribe: command = QStringLiteral("sub ntp_sources all"); break;
    case Action::Create:
        command = QStringLiteral("ntp_sources create id=%1 address=%2 directive=%3")
                      .arg(d.id, d.address, d.directive);
        if (!d.description.isEmpty()) {
            command += QStringLiteral(" description=\"%1\"").arg(d.description);
        }
        if (d.prefer) { command += QStringLiteral(" prefer=1"); }
        break;
    case Action::Remove: command = QStringLiteral("ntp_sources remove id=%1").arg(d.id); break;
    case Action::Enable: command = QStringLiteral("ntp_sources enable id=%1").arg(d.id); break;
    case Action::Disable: command = QStringLiteral("ntp_sources disable id=%1").arg(d.id); break;
    }
    const auto sink = m_setupSink;
    return sink(command, completion) ? ReceiveDispatch::Dispatched : ReceiveDispatch::Unsupported;
}



} // namespace AetherSDR
