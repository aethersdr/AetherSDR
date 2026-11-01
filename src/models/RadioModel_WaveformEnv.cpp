#include "models/RadioModel.h"

#include <QPointer>
#include <QSet>
#include <QThread>
#include <QTimer>
#include <memory>
#include <optional>

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

WaveformEnvApplyResult applyResult(WaveformEnvApplyResult::Outcome outcome, const QString& message)
{
    WaveformEnvApplyResult result;
    result.outcome = outcome;
    result.message = message;
    return result;
}

} // namespace

void RadioModel::setWaveformEnvironmentStagingState(bool active, bool uncertain)
{
    if (active == m_waveformEnvApplyActive && uncertain == m_waveformEnvStagingUncertain) { return; }
    m_waveformEnvApplyActive = active;
    m_waveformEnvStagingUncertain = uncertain;
    emit waveformEnvironmentStagingChanged();
}

bool RadioModel::clearWaveformEnvironmentStaging(QObject* replyContext,
                                                 const RadioControlCompletion& completion)
{
    if (m_waveformEnvApplyActive) { return false; }
    const QPointer<RadioModel> alive(this);
    return requestWaveformEnv({WaveformEnvRequest::Action::Clear, {}, {}, {}}, replyContext,
        [alive, completion](const RadioControlReply& reply) {
            if (alive && !alive->m_waveformEnvApplyActive
                && reply.kind == RadioControlReply::Kind::Accepted) {
                alive->setWaveformEnvironmentStagingState(false, false);
            }
            if (completion) { completion(reply); }
        }) == ReceiveDispatch::Dispatched;
}

bool RadioModel::applyWaveformEnvironment(const QString& name,
                                          const WaveformEnvironment& environment,
                                          QObject* replyContext,
                                          std::function<void(const WaveformEnvApplyResult&)> completion)
{
    using Action = WaveformEnvRequest::Action;
    using Outcome = WaveformEnvApplyResult::Outcome;
    const RadioCapabilities caps = backendCapabilities();
    // Nothing staged keeps the container's current environment (measured), so an
    // empty apply would recreate it unchanged while looking like a success.
    if (QThread::currentThread() != thread() || m_waveformEnvApplyActive || environment.isEmpty()
        || !WaveformEnvRequest::nameValid(name) || !caps.hasWaveforms
        || !caps.waveformEnvControl.has_value() || !controlSessionCurrent(controlSession())) {
        return false;
    }
    QList<WaveformEnvRequest> steps{{Action::Clear, {}, {}, {}}};
    QMap<QString, QString> requested;
    for (const auto& [key, value] : environment) {
        if (!WaveformEnvRequest::keyValid(key) || !WaveformEnvRequest::valueValid(value)
            || requested.contains(key)) {
            return false;
        }
        requested.insert(key, value);
        steps.append({Action::Stage, key, value, {}});
    }
    steps.append({Action::Recreate, {}, {}, name});

    struct State {
        QList<WaveformEnvRequest> steps;
        QString name;
        QMap<QString, QString> requested;
        RadioControlSession session;
        QPointer<QObject> owner;
        bool hasOwner{false};
        std::function<void(const WaveformEnvApplyResult&)> completion;
        bool done{false};
        // Set once the ending is known; the result is delivered when the
        // acknowledged cleanup (and, after a recreate, the new environment) is in.
        std::optional<WaveformEnvApplyResult> pending;
        bool cleanupDone{false};
        bool awaitingEnvironment{false};
        quint64 envWait{0};
        QMetaObject::Connection restarted;
        QMetaObject::Connection environment;
        QMetaObject::Connection sessionEnded;
    };
    const auto state = std::make_shared<State>();
    state->steps = steps;
    state->name = name;
    state->requested = requested;
    state->session = controlSession();
    state->owner = replyContext;
    state->hasOwner = replyContext != nullptr;
    state->completion = std::move(completion);
    // From the first stage on, the radio may hold variables a later deploy
    // would pick up, until an acknowledged clear says otherwise.
    setWaveformEnvironmentStagingState(true, true);

    const QPointer<RadioModel> alive(this);
    const auto deliver = [alive, state] {
        if (state->done || !state->pending || !state->cleanupDone || state->awaitingEnvironment) {
            return;
        }
        state->done = true;
        QObject::disconnect(state->restarted);
        QObject::disconnect(state->environment);
        QObject::disconnect(state->sessionEnded);
        if (alive) {
            alive->setWaveformEnvironmentStagingState(false, !state->pending->stagingCleared);
        }
        if (state->completion && (!state->hasOwner || state->owner)) {
            state->completion(*state->pending);
        }
    };
    // Staged variables also apply to the next deploy (FlexLib Radio.cs:8841), so
    // every ending clears them, and the clear is a step whose reply counts.
    const auto cleanUp = [alive, state, deliver] {
        if (!alive) { return; }
        alive->requestWaveformEnv({Action::Clear, {}, {}, {}}, alive,
            [state, deliver](const RadioControlReply& reply) {
                if (state->done || !state->pending) { return; }
                state->cleanupDone = true;
                state->pending->stagingCleared = reply.kind == RadioControlReply::Kind::Accepted;
                if (!state->pending->stagingCleared) {
                    state->pending->stagingMessage = reply.message.trimmed().isEmpty()
                        ? QStringLiteral("no reason given") : reply.message.trimmed();
                }
                deliver();
            });
    };
    const auto end = [state, cleanUp](WaveformEnvApplyResult result) {
        if (state->done || state->pending) { return; }
        state->pending = result;
        cleanUp();
    };
    const auto checkEnvironment = [alive, state, deliver] {
        if (!alive || !state->awaitingEnvironment) { return; }
        if (alive->waveformContainerEnvironment(state->name) == state->requested) {
            state->awaitingEnvironment = false;
            if (state->pending) { state->pending->environmentConfirmed = true; }
            deliver();
        }
    };

    state->sessionEnded = connect(this, &RadioModel::connectionStateChanged, this,
        [alive, state] {
            if (!alive || state->done || alive->controlSessionCurrent(state->session)) { return; }
            // No clear can reach the radio now, so its staging stays uncertain.
            state->done = true;
            QObject::disconnect(state->restarted);
            QObject::disconnect(state->environment);
            QObject::disconnect(state->sessionEnded);
            alive->setWaveformEnvironmentStagingState(false, true);
            WaveformEnvApplyResult result = state->pending.value_or(
                applyResult(Outcome::Cancelled, QStringLiteral("the radio session ended")));
            result.stagingCleared = false;
            result.stagingMessage = QStringLiteral("the radio session ended");
            if (state->completion && (!state->hasOwner || state->owner)) {
                state->completion(result);
            }
        });

    // Each pending reply owns the next step; the step function holds itself only
    // weakly, so the chain is freed once the last reply has run.
    const auto send = std::make_shared<std::function<void(qsizetype)>>();
    const std::weak_ptr<std::function<void(qsizetype)>> weakSend = send;
    *send = [alive, state, end, deliver, checkEnvironment, weakSend](qsizetype index) {
        const auto send = weakSend.lock();
        if (!alive || state->done || state->pending || !send) { return; }
        const WaveformEnvRequest step = state->steps.at(index);
        const bool recreate = step.action == Action::Recreate;
        if (recreate) {
            // Listen before sending: the status can beat the command reply.
            state->restarted = connect(&alive->m_flexWaveformModel, &FlexWaveformModel::restarted,
                alive, [alive, state, end, deliver, checkEnvironment](const QString& name, bool ok) {
                    if (state->done || state->pending || name != state->name) { return; }
                    if (!ok) {
                        end(applyResult(Outcome::RecreateFailed, QStringLiteral("the radio reported an error")));
                        return;
                    }
                    // The status right after `restarted` still carries the old
                    // environment; wait for one carrying the requested set.
                    state->awaitingEnvironment = true;
                    state->environment = connect(alive, &RadioModel::waveformContainersChanged,
                                                 alive, [checkEnvironment] { checkEnvironment(); });
                    const quint64 wait = ++state->envWait;
                    QTimer::singleShot(alive->m_waveformEnvConfirmTimeoutMs, alive,
                        [state, deliver, wait] {
                            if (state->done || state->envWait != wait || !state->awaitingEnvironment) {
                                return;
                            }
                            state->awaitingEnvironment = false;
                            deliver();
                        });
                    end(applyResult(Outcome::Recreated, {}));
                    checkEnvironment();
                });
        }
        alive->requestWaveformEnv(step, alive,
            [alive, state, end, send, index, step, recreate](const RadioControlReply& reply) {
                if (!alive || state->done || state->pending) { return; }
                if (reply.kind != RadioControlReply::Kind::Accepted) {
                    const QString why = reply.message.trimmed().isEmpty()
                        ? QStringLiteral("no reason given") : reply.message.trimmed();
                    end(applyResult(reply.kind == RadioControlReply::Kind::Cancelled ? Outcome::Cancelled
                                                                                     : Outcome::Refused,
                                    QStringLiteral("the radio refused %1: %2").arg(stepLabel(step), why)));
                    return;
                }
                if (!recreate) {
                    (*send)(index + 1);
                    return;
                }
                QTimer::singleShot(alive->m_waveformEnvConfirmTimeoutMs, alive, [state, end] {
                    end(applyResult(Outcome::NoConfirmation, QStringLiteral("the radio did not report the restart")));
                });
            });
    };
    (*send)(0);
    return true;
}

} // namespace AetherSDR
