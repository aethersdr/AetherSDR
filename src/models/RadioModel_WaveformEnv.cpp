#include "models/RadioModel.h"

#include <QPointer>
#include <QSet>
#include <QThread>
#include <QTimer>
#include <memory>

namespace AetherSDR {

ReceiveDispatch RadioModel::requestWaveformEnv(const WaveformEnvRequest& request,
                                              QObject* replyContext,
                                              const RadioControlCompletion& completion)
{
    if (QThread::currentThread() != thread()) { return ReceiveDispatch::Unsupported; }
    const QPointer<RadioModel> alive(this);
    const RadioControlSession session = controlSession();
    const RadioControlCompletion reply = fencedSetupCompletion(replyContext, completion, true);
    const auto refuse = [&] {
        if (reply) {
            reply({RadioControlReply::Kind::Refused, -1,
                   QStringLiteral("Waveform environment request not dispatched.")});
        }
        if (alive) {
            reportReceiveDispatch(ReceiveDispatch::Unsupported, QStringLiteral("waveform environment"));
        }
        return ReceiveDispatch::Unsupported;
    };
    const RadioCapabilities caps = backendCapabilities();
    if (!m_backend || !request.valid() || !caps.hasWaveforms || !caps.waveformEnvControl.has_value()
        || !controlSessionCurrent(session)) {
        return refuse();
    }
    const RadioControlFence current = [alive, session] {
        return alive && alive->controlSessionCurrent(session);
    };
    const ReceiveDispatch result = m_backend->requestWaveformEnv(request, reply, current);
    if (result == ReceiveDispatch::Unsupported && alive) { return refuse(); }
    return result;
}

std::optional<QMap<QString, QString>> RadioModel::waveformContainerEnvironment(const QString& name) const
{
    for (const FlexWaveformEntry& entry : m_flexWaveformModel.waveforms()) {
        if (entry.isContainer && entry.name == name) { return entry.environment; }
    }
    return std::nullopt;
}

namespace {

QString stepLabel(const WaveformEnvRequest& step)
{
    switch (step.action) {
    case WaveformEnvRequest::Action::Clear: return QStringLiteral("clearing the staged environment");
    case WaveformEnvRequest::Action::Stage: return QStringLiteral("variable %1").arg(step.key);
    case WaveformEnvRequest::Action::Recreate: return QStringLiteral("the recreate");
    }
    return {};
}

} // namespace

bool RadioModel::applyWaveformEnvironment(const QString& name,
                                          const WaveformEnvironment& environment,
                                          QObject* replyContext,
                                          std::function<void(const WaveformEnvApplyResult&)> completion)
{
    using Action = WaveformEnvRequest::Action;
    using Outcome = WaveformEnvApplyResult::Outcome;
    const RadioCapabilities caps = backendCapabilities();
    if (QThread::currentThread() != thread() || m_waveformEnvApplyActive
        || !WaveformEnvRequest::nameValid(name) || !caps.hasWaveforms
        || !caps.waveformEnvControl.has_value() || !controlSessionCurrent(controlSession())) {
        return false;
    }
    QList<WaveformEnvRequest> steps{{Action::Clear, {}, {}, {}}};
    QSet<QString> keys;
    for (const auto& [key, value] : environment) {
        if (!WaveformEnvRequest::keyValid(key) || !WaveformEnvRequest::valueValid(value)
            || keys.contains(key)) {
            return false;
        }
        keys.insert(key);
        steps.append({Action::Stage, key, value, {}});
    }
    steps.append({Action::Recreate, {}, {}, name});

    struct State {
        QList<WaveformEnvRequest> steps;
        QString name;
        RadioControlSession session;
        QPointer<QObject> owner;
        bool hasOwner{false};
        std::function<void(const WaveformEnvApplyResult&)> completion;
        bool done{false};
        QMetaObject::Connection restarted;
        QMetaObject::Connection sessionEnded;
    };
    const auto state = std::make_shared<State>();
    state->steps = steps;
    state->name = name;
    state->session = controlSession();
    state->owner = replyContext;
    state->hasOwner = replyContext != nullptr;
    state->completion = std::move(completion);
    m_waveformEnvApplyActive = true;

    const QPointer<RadioModel> alive(this);
    // Staged variables also apply to the next deploy (FlexLib Radio.cs:8841), so
    // every ending clears them once the radio is done with them.
    const auto clearStage = [alive] {
        if (alive) { alive->requestWaveformEnv({Action::Clear, {}, {}, {}}); }
    };
    const auto finish = [alive, state](Outcome outcome, const QString& message) {
        if (state->done) { return; }
        state->done = true;
        QObject::disconnect(state->restarted);
        QObject::disconnect(state->sessionEnded);
        if (alive) { alive->m_waveformEnvApplyActive = false; }
        if (state->completion && (!state->hasOwner || state->owner)) {
            state->completion({outcome, message});
        }
    };

    state->sessionEnded = connect(this, &RadioModel::connectionStateChanged, this,
        [alive, state, finish] {
            if (alive && !alive->controlSessionCurrent(state->session)) {
                finish(Outcome::Cancelled, QStringLiteral("the radio session ended"));
            }
        });

    // Each pending reply owns the next step; the step function holds itself only
    // weakly, so the chain is freed once the last reply has run.
    const auto send = std::make_shared<std::function<void(qsizetype)>>();
    const std::weak_ptr<std::function<void(qsizetype)>> weakSend = send;
    *send = [alive, state, finish, clearStage, weakSend](qsizetype index) {
        const auto send = weakSend.lock();
        if (!alive || state->done || !send) { return; }
        const WaveformEnvRequest step = state->steps.at(index);
        const bool recreate = step.action == Action::Recreate;
        if (recreate) {
            // Listen before sending: the status can beat the command reply.
            state->restarted = connect(&alive->m_flexWaveformModel, &FlexWaveformModel::restarted,
                alive, [state, finish, clearStage](const QString& name, bool ok) {
                    if (state->done || name != state->name) { return; }
                    clearStage();
                    finish(ok ? Outcome::Recreated : Outcome::RecreateFailed,
                           ok ? QString() : QStringLiteral("the radio reported an error"));
                });
        }
        alive->requestWaveformEnv(step, alive,
            [alive, state, finish, clearStage, send, index, step, recreate](const RadioControlReply& reply) {
                if (!alive || state->done) { return; }
                if (reply.kind == RadioControlReply::Kind::Cancelled) {
                    finish(Outcome::Cancelled, reply.message);
                    return;
                }
                if (reply.kind != RadioControlReply::Kind::Accepted) {
                    clearStage();
                    const QString why = reply.message.trimmed().isEmpty()
                        ? QStringLiteral("no reason given") : reply.message.trimmed();
                    finish(Outcome::Refused,
                           QStringLiteral("the radio refused %1: %2").arg(stepLabel(step), why));
                    return;
                }
                if (!recreate) {
                    (*send)(index + 1);
                    return;
                }
                QTimer::singleShot(alive->m_waveformEnvConfirmTimeoutMs, alive,
                    [state, finish, clearStage] {
                        if (state->done) { return; }
                        clearStage();
                        finish(Outcome::NoConfirmation,
                               QStringLiteral("the radio did not report the restart"));
                    });
            });
    };
    (*send)(0);
    return true;
}

} // namespace AetherSDR
