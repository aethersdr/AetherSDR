#include "core/backends/flex/FlexBackend.h"

namespace AetherSDR {

// FlexLib v4.3.16 Radio.cs:8841-8861: StageWaveformEnv, ClearWaveformEnv and
// RecreateDockerWaveform.
ReceiveDispatch FlexBackend::requestWaveformEnv(const WaveformEnvRequest& request,
                                               const RadioControlCompletion& completion,
                                               const RadioControlFence& current)
{
    using Action = WaveformEnvRequest::Action;
    if (!request.valid() || !m_setupSink || (current && !current())
        || !capabilities().waveformEnvControl.has_value()) {
        return ReceiveDispatch::Unsupported;
    }
    QString command;
    switch (request.action) {
    case Action::Clear: command = QStringLiteral("waveform env clear"); break;
    case Action::Stage:
        command = QStringLiteral("waveform env %1=%2").arg(request.key, request.value);
        break;
    case Action::Recreate: command = QStringLiteral("waveform recreate %1").arg(request.name); break;
    }
    const auto sink = m_setupSink;
    return sink(command, completion) ? ReceiveDispatch::Dispatched : ReceiveDispatch::Unsupported;
}

} // namespace AetherSDR
