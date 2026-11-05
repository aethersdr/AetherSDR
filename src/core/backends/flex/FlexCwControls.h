#pragma once

#include "core/backends/CwControlRequest.h"
#include "core/backends/RadioControlTypes.h"
#include "core/backends/ReceiveCommand.h"
#include "core/backends/SessionCommandGuard.h"

#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <functional>
#include <utility>

namespace AetherSDR {
class PanadapterStream;
class FlexCwControlsTestAccess;
class TxOperationIntegrationTestAccess;

// Owner-thread encoding and scheduling. Workers receive immutable packets and
// operation fences; they never read the next session's state.
class FlexCwControls final : public QObject {
public:
    using ControlSink = std::function<bool(const QString&, const SessionCommandGuard&,
                                           const RadioControlCompletion&)>;
    using TxSink = std::function<void(const QString&, const TxCoordinator::Command&)>;
    explicit FlexCwControls(PanadapterStream* stream, QObject* parent = nullptr);
    ~FlexCwControls() override;
    void setControlSink(ControlSink sink) { m_controlSink = std::move(sink); }
    void setTxSink(TxSink sink) { m_txSink = std::move(sink); }
    void prepare(quint32 handle, const RadioControlFence& current);
    void retire();
    ReceiveDispatch request(const CwControlRequest& request, const TxCoordinator::Command& command);
    ReceiveDispatch requestLocalPtt(const RadioControlCompletion& completion,
                                    const RadioControlFence& current);
    void observeStreamRemoved(quint32 streamId);
private:
    QByteArray packet(const QByteArray& payload);
    QString decorate(const QString& command, const CwControlRequest& request);
    QPointer<PanadapterStream> m_stream;
    ControlSink m_controlSink;
    TxSink m_txSink;
    SessionCommandGuard m_session{SessionCommandGuard::create()};
    SessionCommandGuard m_streamGuard{SessionCommandGuard::create()};
    RadioControlFence m_current;
    quint32 m_handle{0};
    quint32 m_streamId{0};
    quint32 m_index{1};
    quint8 m_packetCount{0};
    struct Timestamp { quint16 value; qint64 backdate; bool fallback; };
    Timestamp timestamp(qint64 nowMs, qint64 scheduleAgeMs);
    qint64 m_clockStartMs{-1};
    qint64 m_lastSendMs{-1};
    bool m_prepared{false};
    QTimer m_createDeadline;
    QTimer m_pttDeadline;
    RadioControlCompletion m_pttCompletion;
    SessionCommandGuard m_createGuard;
    SessionCommandGuard m_pttGuard;
    void finishLocalPtt(const RadioControlReply& reply);
    friend class FlexCwControlsTestAccess;
    friend class TxOperationIntegrationTestAccess;
};
} // namespace AetherSDR
