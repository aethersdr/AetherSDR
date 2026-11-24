#pragma once

#include "core/backends/flex/FlexBackend.h"
#include "core/backends/flex/FlexCommandSession.h"
#include "core/backends/flex/RadioConnection.h"
#include "core/backends/flex/WanConnection.h"
#include "models/RadioModel.h"

#include <QPointer>
#include <algorithm>

namespace AetherSDR {

// Inert fixtures use the production LAN adapter and reply ledger. Policy tests
// can retain an old completion deliberately to challenge their own fences.
class FlexCommandTestAccess {
public:
    using Reply = std::function<void(int, const QString&)>;
    static FlexCommandSession& session(RadioModel& radio)
    {
        if (radio.m_wanConn) { return radio.m_wanConn->commands(); }
        if (auto* backend = dynamic_cast<FlexBackend*>(radio.m_backend.get());
            backend && backend->connection() == radio.m_connection) {
            FlexCommandSession& commands = backend->commandSession();
            if (!radio.m_connection->lineEpoch()) { radio.m_connection->setLineEpoch(1); }
            if (!commands.active() && radio.m_connection->isConnected()) {
                const quint64 epoch = std::max(radio.m_connection->lineEpoch(), commands.epoch() + 1);
                radio.m_connection->setLineEpoch(epoch);
                commands.begin(epoch);
            }
            return commands;
        }
        QObject* existing = radio.findChild<QObject*>(QStringLiteral("testFlexCommands"), Qt::FindDirectChildrenOnly);
        if (existing) { return *static_cast<FlexCommandSession*>(existing); }
        auto* commands = new FlexCommandSession(&radio);
        commands->setObjectName(QStringLiteral("testFlexCommands"));
        if (radio.m_connection) {
            quint64 epoch = radio.m_connection->lineEpoch();
            if (!epoch) { epoch = 1; radio.m_connection->setLineEpoch(epoch); }
            commands->bindLan(radio.m_connection);
            commands->begin(epoch);
        }
        if (radio.m_backend) {
            QObject::connect(radio.m_backend.get(), &QObject::destroyed, commands,
                [commands] { commands->retire(QStringLiteral("Backend retired.")); });
            QObject::connect(radio.m_backend.get(), &IRadioBackend::disconnected, commands,
                [commands] { commands->retire(QStringLiteral("Disconnected.")); });
        }
        return *commands;
    }
    static void bind(RadioModel& radio, FlexBackend& encoder)
    {
        encoder.setCommandSessionForTest(&session(radio));
        // Inert model fixtures have an identity transport, not a LAN handshake
        // snapshot. Production gets this value from ConnectionSession.
        encoder.setDaxTxClientHandleProviderForTest([model = QPointer<RadioModel>(&radio)] {
            return model ? model->ourClientHandle() : 0;
        });
    }
    static void bindSynchronous(RadioModel& radio, FlexBackend& encoder)
    {
        bind(radio, encoder);
        FlexCommandSession& commands = session(radio);
        commands.setWriter([target = QPointer<FlexCommandSession>(&commands), connection = radio.m_connection]
                           (const FlexCommandSession::Write& write) {
            const auto result = write.run([&] { return connection->dispatchCommand(write.sequence, write.command); });
            if (target) { target->writeFinished(write.epoch, write.sequence, result); }
            return true;
        });
    }
    static void record(FlexBackend& backend,
                       std::function<void(const QString&, Reply)> writer)
    {
        FlexCommandSession& commands = backend.commandSession();
        commands.begin(1);
        backend.setCommandSessionForTest(&commands);
        commands.setWriter([target = QPointer<FlexCommandSession>(&commands), writer](const FlexCommandSession::Write& write) {
            const auto result = write.run([&] {
                writer(write.command, [target, epoch = write.epoch, sequence = write.sequence](int code, const QString& body) {
                    if (target) { target->answer(epoch, sequence, code, body); }
                });
                return FlexCommandSession::WriteResult::Written;
            });
            if (target) { target->writeFinished(write.epoch, write.sequence, result); }
            return true;
        });
    }
    static void expireAll(FlexCommandSession& commands)
    {
        for (auto& pending : commands.m_pending) { pending.deadline = QDeadlineTimer(0); }
        commands.expire();
    }
    static FlexCommandSession::Completion completion(Reply reply)
    {
        if (!reply) { return {}; }
        return [reply = std::move(reply)](const FlexCommandSession::Reply& result) {
            reply(result.kind == FlexCommandSession::ReplyKind::NotDispatched ? 0x50000063 : result.code,
                  result.body);
        };
    }
    static quint32 send(RadioModel& radio, const QString& command, Reply reply = {},
                        const FlexCommandSession::Options& options = {})
    { return session(radio).submit(command, options, completion(std::move(reply))); }
    static bool tx(RadioModel& radio, const QString& text, const TxCoordinator::Command& command,
                   Reply reply = {}, const RadioControlFence& current = {})
    {
        FlexCommandSession::Options options;
        options.tx = true;
        options.operation = command.operation;
        options.keying = command.keying;
        options.cleanup = !command.keying;
        if (current) { options.permitsWrite = [current] { return current(); }; }
        options.consumed = [completion = command.completion] { completion.finish(); };
        const bool accepted = send(radio, text, std::move(reply), options) != 0;
        if (!accepted) { command.completion.finish(); }
        return accepted;
    }
    static qsizetype pending(RadioModel& radio)
    {
        qsizetype count = 0;
        for (const auto& pending : session(radio).m_pending) { if (pending.completion) { ++count; } }
        return count;
    }
    static quint32 firstSequence(RadioModel& radio)
    {
        quint32 first = 0;
        for (auto it = session(radio).m_pending.cbegin(); it != session(radio).m_pending.cend(); ++it) {
            if (it->completion && (!first || it.key() < first)) { first = it.key(); }
        }
        return first;
    }
    static Reply reply(FlexCommandSession& commands, quint32 sequence, bool take = false)
    {
        auto it = commands.m_pending.find(sequence);
        if (it == commands.m_pending.end() || !it->completion) { return {}; }
        const auto completion = it->completion;
        if (take) { commands.m_pending.erase(it); }
        return [completion](int code, const QString& body) {
            completion({code == 0x50000063 ? FlexCommandSession::ReplyKind::NotDispatched
                                         : FlexCommandSession::ReplyKind::RadioReply, code, body});
        };
    }
    static Reply takeFirstReply(RadioModel& radio)
    { return reply(session(radio), firstSequence(radio), true); }
    static QList<Reply> replies(RadioModel& radio, bool take = false)
    {
        QList<Reply> result;
        QList<quint32> sequences = session(radio).m_pending.keys();
        std::sort(sequences.begin(), sequences.end());
        for (quint32 sequence : sequences) {
            Reply callback = reply(session(radio), sequence, take);
            if (callback) { result.append(std::move(callback)); }
        }
        return result;
    }
    static void insertReply(FlexCommandSession& commands, quint32 sequence, Reply reply)
    {
        if (!commands.active()) { commands.begin(1); }
        FlexCommandSession::Write write{commands.epoch(), sequence, QStringLiteral("injected reply"),
            commands.m_session, commands.m_session.request(), {},
            std::make_shared<std::atomic<FlexCommandSession::Write::State>>(FlexCommandSession::Write::State::Written)};
        commands.m_pending.insert(sequence, {write, completion(std::move(reply)), QDeadlineTimer(30000), true, {}});
        commands.m_sequence.store(std::max<quint64>(commands.m_sequence.load(), quint64(sequence) + 1));
    }
    static void prepareWan(WanConnection& wan)
    {
        if (!wan.m_sessionGeneration) { wan.resetSessionState(); }
        wan.m_connected = true;
    }
    static void insertReply(WanConnection& wan, quint32 sequence, Reply reply)
    { prepareWan(wan); insertReply(wan.commands(), sequence, std::move(reply)); }
    static bool contains(FlexCommandSession& commands, quint32 sequence)
    { return commands.m_pending.contains(sequence); }
};

} // namespace AetherSDR
