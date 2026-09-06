#include "TestSettingsProfile.h"
#include "core/RadioDiscovery.h"
#include "core/backends/sim/SimBackend.h"
#include "core/control/ControlResourceStore.h"
#include "core/control/ControlService.h"
#include "core/control/ControlSession.h"
#include "core/control/RadioResourceAdapter.h"
#include "models/RadioModel.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QThread>
#include <QtMath>

#include <cstdio>
#include <functional>
#include <initializer_list>

using namespace AetherSDR;
using namespace AetherSDR::control;

namespace {

bool check(bool condition, const char* message)
{
    if (condition) {
        return true;
    }
    std::fprintf(stderr, "%s\n", message);
    return false;
}

bool hasExactlyKeys(const QJsonObject& object,
                    std::initializer_list<const char*> expected)
{
    QSet<QString> expectedKeys;
    for (const char* key : expected) {
        expectedKeys.insert(QString::fromLatin1(key));
    }
    QSet<QString> actualKeys;
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        actualKeys.insert(it.key());
    }
    return actualKeys == expectedKeys;
}

bool waitUntil(const std::function<bool()>& predicate, int timeoutMs = 3000)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    return predicate();
}

QJsonObject invoke(ControlService* service, ControlSession* session,
                   const QString& id, const QString& method,
                   const QJsonObject& params)
{
    QJsonObject request{{QStringLiteral("v"), 1},
                        {QStringLiteral("id"), id},
                        {QStringLiteral("method"), method},
                        {QStringLiteral("params"), params}};
    if (method != QStringLiteral("hello")) {
        request.insert(QStringLiteral("sessionId"), session->sessionId());
    }
    const QByteArray bytes = QJsonDocument(request).toJson(QJsonDocument::Compact);
    return service->handle(bytes, session).message;
}

bool negotiate(ControlService* service, ControlSession* session)
{
    const QJsonObject reply = invoke(
        service, session, QStringLiteral("hello"), QStringLiteral("hello"),
        {{QStringLiteral("versions"), QJsonArray{1}}});
    return reply.value(QStringLiteral("result")).toObject()
        .value(QStringLiteral("sessionId")).toString() == session->sessionId()
        && session->isNegotiated();
}

QString errorCode(const QJsonObject& response)
{
    return response.value(QStringLiteral("error")).toObject()
        .value(QStringLiteral("code")).toString();
}

// The session hands the transport pre-encoded frames; decode them back so the
// assertions below can keep reading protocol fields by name.
QList<QJsonObject> drain(ControlSession& session)
{
    QList<QJsonObject> messages;
    const QList<QByteArray> frames = session.takePendingFrames();
    messages.reserve(frames.size());
    for (const QByteArray& frame : frames) {
        messages.append(QJsonDocument::fromJson(frame).object());
    }
    return messages;
}

QJsonObject exactResource(const QString& type, const QString& radioSession = {},
                          const QString& id = {})
{
    QJsonObject resource{{QStringLiteral("type"), type}};
    if (!radioSession.isEmpty()) {
        resource.insert(QStringLiteral("radioSession"), radioSession);
    }
    if (!id.isEmpty()) {
        resource.insert(QStringLiteral("id"), id);
    }
    return resource;
}

bool testStoreRevisions()
{
    ControlResourceStore store;
    const ResourceAddress address{QStringLiteral("slice"),
                                  QStringLiteral("radio-1"), QStringLiteral("0")};
    const ResourceAddress otherAddress{QStringLiteral("slice"),
                                       QStringLiteral("radio-1"), QStringLiteral("1")};
    quint64 removedRevision = 0;
    QObject::connect(&store, &ControlResourceStore::resourceRemoved,
                     [&removedRevision](const ResourceAddress&, quint64 revision) {
                         removedRevision = revision;
                     });

    if (!check(store.upsert(address, {{QStringLiteral("frequencyHz"), 100}}),
               "first resource value must create revision 1")
        || !check(!store.upsert(address, {{QStringLiteral("frequencyHz"), 100}}),
                  "an unchanged canonical value must not advance its revision")
        || !check(store.upsert(otherAddress, {{QStringLiteral("frequencyHz"), 700}}),
                  "a second identity must consume the next store revision")
        || !check(store.upsert(address, {{QStringLiteral("frequencyHz"), 200}}),
                  "a changed canonical value must advance its revision")) {
        return false;
    }
    const std::optional<ResourceSnapshot> second = store.get(address);
    if (!check(second && second->revision == 3,
               "store-wide revisions must stay monotonic across identities")
        || !check(store.remove(address) && removedRevision == 4,
                  "removal must consume the next identity revision")
        || !check(store.upsert(address, {{QStringLiteral("frequencyHz"), 300}}),
                  "a removed identity may be recreated")) {
        return false;
    }
    const std::optional<ResourceSnapshot> recreated = store.get(address);
    return check(recreated && recreated->revision == 5,
                 "recreating an identity must not reset its revision");
}

bool testServiceSubscriptions()
{
    ControlResourceStore store;
    store.upsert({QStringLiteral("server"), {}, {}},
                 {{QStringLiteral("health"), QStringLiteral("ok")}});
    const ResourceAddress slice{QStringLiteral("slice"),
                                QStringLiteral("radio-1"), QStringLiteral("0")};
    store.upsert(slice, {{QStringLiteral("frequencyHz"), 100}});

    ControlService service(&store);
    ControlSession first(&store, 4096, SessionAuthorization::Observer);
    ControlSession second(&store, 4096, SessionAuthorization::Observer);
    if (!check(negotiate(&service, &first) && negotiate(&service, &second),
               "both clients must negotiate independent sessions")) {
        return false;
    }

    const QJsonObject getReply = invoke(
        &service, &first, QStringLiteral("get-1"), QStringLiteral("resource.get"),
        {{QStringLiteral("resource"),
          exactResource(QStringLiteral("slice"), QStringLiteral("radio-1"),
                        QStringLiteral("0"))}});
    const qint64 initialSliceRevision = getReply.value(QStringLiteral("result")).toObject()
                                            .value(QStringLiteral("revision")).toInteger();
    if (!check(initialSliceRevision > 0,
               "resource.get must return the complete current snapshot")) {
        return false;
    }

    const QJsonArray sliceSelector{
        exactResource(QStringLiteral("slice"), QStringLiteral("radio-1"))};
    const QJsonObject firstSubscribe = invoke(
        &service, &first, QStringLiteral("sub-1"), QStringLiteral("resource.subscribe"),
        {{QStringLiteral("resources"), sliceSelector}});
    const QJsonObject firstBaseline = firstSubscribe.value(QStringLiteral("result")).toObject();
    const QString firstSubscription =
        firstBaseline.value(QStringLiteral("subscription")).toString();
    if (!check(firstBaseline.value(QStringLiteral("sequence")).toInteger() == 0
                   && firstBaseline.value(QStringLiteral("resources")).toArray().size() == 1,
               "subscribe must atomically return a sequence and matching baseline")) {
        return false;
    }

    store.upsert(slice, {{QStringLiteral("frequencyHz"), 200}});
    store.upsert(slice, {{QStringLiteral("frequencyHz"), 300}});
    const QList<QJsonObject> coalesced = drain(first);
    if (!check(coalesced.size() == 1,
               "undrained changes to one resource must coalesce")
        || !check(coalesced.first().value(QStringLiteral("sequence")).toInteger() == 2
                      && coalesced.first().value(QStringLiteral("revision")).toInteger()
                             == initialSliceRevision + 2
                      && coalesced.first().value(QStringLiteral("value")).toObject()
                             .value(QStringLiteral("frequencyHz")).toInteger() == 300,
                  "the coalesced event must retain the newest sequence, revision, and value")) {
        return false;
    }

    const ResourceAddress secondSlice{QStringLiteral("slice"),
                                      QStringLiteral("radio-1"), QStringLiteral("1")};
    store.upsert(secondSlice, {{QStringLiteral("frequencyHz"), 700}});
    store.upsert(slice, {{QStringLiteral("frequencyHz"), 350}});
    const QList<QJsonObject> interleaved = drain(first);
    if (!check(interleaved.size() == 2
                   && interleaved.at(0).value(QStringLiteral("sequence")).toInteger() == 3
                   && interleaved.at(1).value(QStringLiteral("sequence")).toInteger() == 4,
               "coalescing must preserve event-sequence delivery order across resources")) {
        return false;
    }

    invoke(&service, &second, QStringLiteral("sub-2"),
           QStringLiteral("resource.subscribe"),
           {{QStringLiteral("resources"), sliceSelector}});
    store.upsert(slice, {{QStringLiteral("frequencyHz"), 400}});
    const QList<QJsonObject> firstNext = drain(first);
    const QList<QJsonObject> secondNext = drain(second);
    if (!check(firstNext.size() == 1 && secondNext.size() == 1
                   && firstNext.first().value(QStringLiteral("sequence")).toInteger() == 5
                   && secondNext.first().value(QStringLiteral("sequence")).toInteger() == 1,
               "event sequences must be monotonic and local to each client session")) {
        return false;
    }

    store.upsert(slice, {{QStringLiteral("frequencyHz"), 450}});
    const QJsonObject unsubscribe = invoke(
        &service, &first, QStringLiteral("unsub-1"),
        QStringLiteral("resource.unsubscribe"),
        {{QStringLiteral("subscription"), firstSubscription}});
    const QList<QJsonObject> afterUnsubscribe = drain(first);
    store.upsert(slice, {{QStringLiteral("frequencyHz"), 500}});
    if (!check(unsubscribe.value(QStringLiteral("result")).toObject()
                   .value(QStringLiteral("removed")).toBool()
                   && afterUnsubscribe.isEmpty()
                   && drain(first).isEmpty()
                   && drain(second).size() == 1,
               "unsubscribe must discard undrained events no longer observed and isolate clients")) {
        return false;
    }

    const QJsonObject malformed = invoke(
        &service, &first, QStringLiteral("bad-selector"),
        QStringLiteral("resource.get"),
        {{QStringLiteral("resource"),
          QJsonObject{{QStringLiteral("type"), QStringLiteral("slice")},
                      {QStringLiteral("radioSession"), QStringLiteral("radio-1")},
                      {QStringLiteral("typo"), QStringLiteral("0")}}}});
    if (!check(errorCode(malformed) == QStringLiteral("request.invalid_params"),
               "unknown selector fields must fail closed")) {
        return false;
    }

    const QJsonObject unknownSubscription = invoke(
        &service, &first, QStringLiteral("unknown-unsub"),
        QStringLiteral("resource.unsubscribe"),
        {{QStringLiteral("subscription"), firstSubscription}});
    if (!check(errorCode(unknownSubscription) == QStringLiteral("resource.not_found"),
               "unsubscribing an unknown ID must return resource.not_found")) {
        return false;
    }

    QJsonArray tooManySelectors;
    for (int index = 0; index <= 64; ++index) {
        tooManySelectors.append(sliceSelector.first());
    }
    const QJsonObject selectorLimit = invoke(
        &service, &first, QStringLiteral("selector-limit"),
        QStringLiteral("resource.subscribe"),
        {{QStringLiteral("resources"), tooManySelectors}});
    if (!check(errorCode(selectorLimit) == QStringLiteral("request.invalid_params"),
               "a subscription must reject more than 64 selectors")) {
        return false;
    }

    ControlSession limited(&store, 4096, SessionAuthorization::Observer);
    if (!check(negotiate(&service, &limited),
               "subscription-limit client must negotiate")) {
        return false;
    }
    for (int index = 0; index < ControlSession::kMaxSubscriptions; ++index) {
        const QJsonObject accepted = invoke(
            &service, &limited, QStringLiteral("limit-%1").arg(index),
            QStringLiteral("resource.subscribe"),
            {{QStringLiteral("resources"), sliceSelector}});
        if (!check(accepted.value(QStringLiteral("result")).isObject(),
                   "the declared number of subscriptions must be accepted")) {
            return false;
        }
    }
    const QJsonObject subscriptionLimit = invoke(
        &service, &limited, QStringLiteral("limit-overflow"),
        QStringLiteral("resource.subscribe"),
        {{QStringLiteral("resources"), sliceSelector}});
    return check(errorCode(subscriptionLimit)
                     == QStringLiteral("transport.limit_exceeded"),
                 "a client must reject subscription 65");
}

bool testSubscribeSequenceBoundary()
{
    ControlResourceStore store;
    ControlService service(&store);
    ControlSession session(&store, 4096, SessionAuthorization::Observer);
    if (!check(negotiate(&service, &session),
               "sequence-boundary client must negotiate")) {
        return false;
    }

    const ResourceAddress slice{QStringLiteral("slice"),
                                QStringLiteral("radio-1"), QStringLiteral("0")};
    store.upsert(slice, {{QStringLiteral("frequencyHz"), 100}});
    const QJsonArray selectors{
        exactResource(QStringLiteral("slice"), QStringLiteral("radio-1"))};
    invoke(&service, &session, QStringLiteral("sub-initial"),
           QStringLiteral("resource.subscribe"),
           {{QStringLiteral("resources"), selectors}});

    store.upsert(slice, {{QStringLiteral("frequencyHz"), 200}});
    const QJsonObject pendingBaseline = invoke(
        &service, &session, QStringLiteral("sub-pending"),
        QStringLiteral("resource.subscribe"),
        {{QStringLiteral("resources"), selectors}})
                                            .value(QStringLiteral("result")).toObject();
    const QList<QJsonObject> pending = drain(session);
    if (!check(pendingBaseline.value(QStringLiteral("sequence")).toInteger() == 0
                   && pending.size() == 1
                   && pending.first().value(QStringLiteral("sequence")).toInteger() == 1,
               "a baseline must stop at the last drained sequence when an older event is pending")) {
        return false;
    }

    const QJsonObject drainedBaseline = invoke(
        &service, &session, QStringLiteral("sub-drained"),
        QStringLiteral("resource.subscribe"),
        {{QStringLiteral("resources"), selectors}})
                                            .value(QStringLiteral("result")).toObject();
    store.upsert(slice, {{QStringLiteral("frequencyHz"), 300}});
    const QList<QJsonObject> afterBaseline = drain(session);
    return check(drainedBaseline.value(QStringLiteral("sequence")).toInteger() == 1
                     && afterBaseline.size() == 1
                     && afterBaseline.first().value(QStringLiteral("sequence")).toInteger() == 2,
                 "the next delivered event must have a sequence greater than the baseline");
}

bool testOverflowRequiresResync()
{
    ControlResourceStore store;
    ControlService service(&store);
    ControlSession session(&store, 360, SessionAuthorization::Observer);
    if (!check(negotiate(&service, &session), "overflow client must negotiate")) {
        return false;
    }
    const QJsonArray selectors{
        exactResource(QStringLiteral("slice"), QStringLiteral("radio-1"))};
    invoke(&service, &session, QStringLiteral("sub"),
           QStringLiteral("resource.subscribe"),
           {{QStringLiteral("resources"), selectors}});

    store.upsert({QStringLiteral("slice"), QStringLiteral("radio-1"), QStringLiteral("0")},
                 {{QStringLiteral("value"), QString(100, QLatin1Char('a'))}});
    store.upsert({QStringLiteral("slice"), QStringLiteral("radio-1"), QStringLiteral("1")},
                 {{QStringLiteral("value"), QString(100, QLatin1Char('b'))}});
    const QList<QJsonObject> overflow = drain(session);
    if (!check(overflow.size() == 1
                   && overflow.first().value(QStringLiteral("event")).toString()
                          == QStringLiteral("resource.resyncRequired")
                   && overflow.first().value(QStringLiteral("subscriptionsInvalidated")).toBool(),
               "queue overflow must invalidate subscriptions and require an explicit resync")) {
        return false;
    }

    store.upsert({QStringLiteral("slice"), QStringLiteral("radio-1"), QStringLiteral("0")},
                 {{QStringLiteral("value"), QStringLiteral("after-overflow")}});
    if (!check(drain(session).isEmpty(),
               "invalidated subscriptions must remain quiet until resubscribed")) {
        return false;
    }
    const QJsonObject resubscribe = invoke(
        &service, &session, QStringLiteral("resub"),
        QStringLiteral("resource.subscribe"),
        {{QStringLiteral("resources"), selectors}});
    if (!check(resubscribe.value(QStringLiteral("result")).toObject()
                   .value(QStringLiteral("resources")).toArray().size() == 2,
               "resubscribe must establish a fresh complete baseline")) {
        return false;
    }

    store.upsert({QStringLiteral("slice"), QStringLiteral("radio-1"), QStringLiteral("0")},
                 {{QStringLiteral("value"), QString(100, QLatin1Char('c'))}});
    store.upsert({QStringLiteral("slice"), QStringLiteral("radio-1"), QStringLiteral("1")},
                 {{QStringLiteral("value"), QString(100, QLatin1Char('d'))}});
    const QJsonObject baselineAfterUndrainedResync = invoke(
        &service, &session, QStringLiteral("resub-undrained"),
        QStringLiteral("resource.subscribe"),
        {{QStringLiteral("resources"), selectors}});
    return check(baselineAfterUndrainedResync.value(QStringLiteral("result")).isObject()
                     && drain(session).isEmpty(),
                 "a fresh baseline must supersede an undrained resync notice");
}

RadioInfo demoInfo()
{
    RadioInfo info;
    info.name = SimBackend::demoModelName();
    info.model = SimBackend::demoModelName();
    info.serial = SimBackend::demoSerial();
    info.family = SimBackend::familyName();
    info.address = QHostAddress(QHostAddress::LocalHost);
    info.port = 4992;
    return info;
}

bool testWireFramingIsCanonical()
{
    // ControlSession now encodes each event once and the transport writes the
    // stored frame verbatim, so the frame it hands over must still be exactly
    // what LocalControlServer::send() would have produced: compact JSON plus
    // the single framing newline, and nothing else.
    ControlResourceStore store;
    ControlService service(&store);
    ControlSession session(&store, 4096, SessionAuthorization::Observer);
    if (!check(negotiate(&service, &session), "framing client must negotiate")) {
        return false;
    }
    const ResourceAddress slice{QStringLiteral("slice"),
                                QStringLiteral("radio-1"), QStringLiteral("0")};
    store.upsert(slice, {{QStringLiteral("frequencyHz"), 100}});
    invoke(&service, &session, QStringLiteral("frame-sub"),
           QStringLiteral("resource.subscribe"),
           {{QStringLiteral("resources"),
             QJsonArray{exactResource(QStringLiteral("slice"),
                                      QStringLiteral("radio-1"))}}});
    store.upsert(slice, {{QStringLiteral("frequencyHz"), 200}});

    const QList<QByteArray> frames = session.takePendingFrames();
    if (!check(frames.size() == 1, "one change must produce one frame")) {
        return false;
    }
    const QByteArray frame = frames.first();
    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(frame, &parseError);
    const QJsonObject message = document.object();
    return check(frame.endsWith('\n') && !frame.chopped(1).contains('\n'),
                 "a frame must be one line terminated by a single newline")
        && check(parseError.error == QJsonParseError::NoError && document.isObject(),
                 "a frame must decode as one JSON object")
        && check(QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n' == frame,
                 "a frame must be compact JSON, byte-identical to the transport encoding")
        && check(message.value(QStringLiteral("event")).toString()
                     == QStringLiteral("resource.changed")
                     && message.value(QStringLiteral("sequence")).toInteger() == 1
                     && message.value(QStringLiteral("sessionId")).toString()
                            == session.sessionId(),
                 "a frame must carry the session-sequenced event envelope");
}

bool testPureSeamReconnectRepublishesSlice()
{
    ControlResourceStore store;
    RadioModel radio;
    RadioResourceAdapter adapter(&radio, &store, QStringLiteral("radio-1"));
    radio.connectToRadio(demoInfo());

    const ResourceAddress sliceAddress{QStringLiteral("slice"),
                                       QStringLiteral("radio-1"), QStringLiteral("0")};
    if (!check(waitUntil([&] { return store.get(sliceAddress).has_value(); }),
               "the seam-reconnect fixture must start with a published slice")) {
        radio.disconnectFromRadio();
        return false;
    }
    SliceModel* originalSlice = radio.slice(0);
    if (!check(originalSlice != nullptr,
               "the seam-reconnect fixture must retain its SliceModel")) {
        radio.disconnectFromRadio();
        return false;
    }

    radio.disconnectFromRadio();
    if (!check(waitUntil([&] {
                   return !radio.isConnected()
                       && !store.get(sliceAddress).has_value();
               }),
               "disconnect must remove the published slice before reclaim")) {
        return false;
    }

    radio.stageSessionModelsForReconnectForTest();
    radio.connectionStateChanged(true);
    int occupancySignals = 0;
    QObject::connect(&radio, &RadioModel::slotOccupancyChanged,
                     [&occupancySignals](int sliceId) {
                         if (sliceId == 0) {
                             ++occupancySignals;
                         }
                     });
    SliceDelta delta;
    delta.frequency = originalSlice->frequency();
    delta.mode = originalSlice->mode();
    delta.panId = originalSlice->panId();
    radio.emitBackendSliceChangedForTest(0, delta);

    const std::optional<ResourceSnapshot> reclaimed = store.get(sliceAddress);
    const bool result = check(radio.slice(0) == originalSlice,
                              "the normalized backend seam must reclaim the existing SliceModel")
        && check(occupancySignals == 1,
                 "non-Flex slice reclaim must publish an occupancy edge")
        && check(reclaimed.has_value()
                     && reclaimed->value.value(QStringLiteral("owned")).toBool(),
                 "the occupancy edge must reattach and republish the slice resource");
    radio.connectionStateChanged(false);
    return result;
}

bool testExternalReceiveAudioRepublishesAgcAndSquelch()
{
    ControlResourceStore store;
    RadioModel radio;
    RadioResourceAdapter adapter(&radio, &store, QStringLiteral("radio-1"));
    radio.connectToRadio(demoInfo());

    const ResourceAddress sliceAddress{QStringLiteral("slice"),
                                       QStringLiteral("radio-1"), QStringLiteral("0")};
    if (!check(waitUntil([&] { return store.get(sliceAddress).has_value(); }),
               "the external-receive fixture must start with a published slice")) {
        radio.disconnectFromRadio();
        return false;
    }
    SliceModel* slice = radio.slice(0);
    if (!check(slice != nullptr, "the external-receive fixture must retain its SliceModel")) {
        radio.disconnectFromRadio();
        return false;
    }

    // While an external receive-audio source replaces the radio's audio, the
    // AGC and squelch setters emit only their externalReceive* signals but the
    // accessors the adapter reads switch to the external values.
    slice->setExternalReceiveAudioReplacementMute(true);
    slice->setAgcMode(QStringLiteral("fast"));
    slice->setAgcThreshold(-40);
    slice->setSquelch(true, 42);

    const auto receiveOf = [&store, &sliceAddress] {
        return store.get(sliceAddress)->value.value(QStringLiteral("receive")).toObject();
    };
    const QJsonObject receive = receiveOf();
    const QJsonObject agc = receive.value(QStringLiteral("agc")).toObject();
    const QJsonObject squelch = receive.value(QStringLiteral("squelch")).toObject();
    const bool result =
        check(agc.value(QStringLiteral("mode")).toString() == slice->receiveAgcMode()
                  && agc.value(QStringLiteral("mode")).toString()
                         == QStringLiteral("fast"),
              "external receive AGC mode must reach the published resource")
        && check(agc.value(QStringLiteral("threshold")).toInt()
                     == slice->receiveAgcThreshold(),
                 "external receive AGC threshold must reach the published resource")
        && check(squelch.value(QStringLiteral("enabled")).toBool()
                     == slice->receiveSquelchOn()
                     && squelch.value(QStringLiteral("level")).toInt()
                            == slice->receiveSquelchLevel()
                     && squelch.value(QStringLiteral("level")).toInt() == 42,
                 "external receive squelch must reach the published resource");
    radio.disconnectFromRadio();
    waitUntil([&radio] { return !radio.isConnected(); });
    return result;
}

bool testSliceRemovalMatchesTheLiveObject()
{
    ControlResourceStore store;
    RadioModel radio;
    RadioResourceAdapter adapter(&radio, &store, QStringLiteral("radio-1"));
    radio.connectToRadio(demoInfo());

    const ResourceAddress sliceAddress{QStringLiteral("slice"),
                                       QStringLiteral("radio-1"), QStringLiteral("0")};
    if (!check(waitUntil([&] { return store.get(sliceAddress).has_value(); }),
               "the slice-removal fixture must start with a published slice")) {
        radio.disconnectFromRadio();
        return false;
    }

    // pruneStaleSessionModels() emits sliceRemoved for a *stale* SliceModel whose
    // id a live one may already have reclaimed. An id-only match would unbind the
    // live slice and publish a removal for a resource that still exists.
    radio.sliceRemoved(0);
    if (!check(store.get(sliceAddress).has_value(),
               "a removal for an id that still resolves to a live slice must keep it")) {
        radio.disconnectFromRadio();
        return false;
    }
    const quint64 revision = store.get(sliceAddress)->revision;
    SliceModel* slice = radio.slice(0);
    if (!check(slice != nullptr, "the live slice must survive the stale removal")) {
        radio.disconnectFromRadio();
        return false;
    }
    slice->setLocked(!slice->isLocked());
    if (!check(store.get(sliceAddress)
                   && store.get(sliceAddress)->revision > revision,
               "the live slice must stay attached after a stale removal")) {
        radio.disconnectFromRadio();
        return false;
    }

    // Once the model no longer serves the id, the same signal must drop it.
    radio.stageSessionModelsForReconnectForTest();
    radio.sliceRemoved(0);
    const bool result = check(!store.get(sliceAddress).has_value(),
                              "a removal for an id the model no longer serves must drop it");
    radio.disconnectFromRadio();
    waitUntil([&radio] { return !radio.isConnected(); });
    return result;
}

bool testSimBackendEndToEnd()
{
    ControlResourceStore store;
    ControlService service(&store);
    ControlSession client(&store, 1024 * 1024, SessionAuthorization::Observer);
    RadioModel radio;
    RadioResourceAdapter adapter(&radio, &store, QStringLiteral("radio-1"));
    if (!check(negotiate(&service, &client), "sim observer must negotiate")) {
        return false;
    }

    const QJsonArray selectors{
        exactResource(QStringLiteral("radioSession")),
        exactResource(QStringLiteral("slice"), QStringLiteral("radio-1")),
        exactResource(QStringLiteral("panadapter"), QStringLiteral("radio-1"))};
    const QJsonObject baselineReply = invoke(
        &service, &client, QStringLiteral("sim-sub"),
        QStringLiteral("resource.subscribe"),
        {{QStringLiteral("resources"), selectors}});
    if (!check(baselineReply.value(QStringLiteral("result")).toObject()
                   .value(QStringLiteral("resources")).toArray().size() == 1,
               "the disconnected model must begin as one radioSession resource")) {
        return false;
    }

    radio.connectToRadio(demoInfo());
    const ResourceAddress radioAddress{QStringLiteral("radioSession"), {},
                                       QStringLiteral("radio-1")};
    const ResourceAddress sliceAddress{QStringLiteral("slice"),
                                       QStringLiteral("radio-1"), QStringLiteral("0")};
    const ResourceAddress panAddress{QStringLiteral("panadapter"),
                                     QStringLiteral("radio-1"),
                                     QStringLiteral("0x40000000")};
    const bool converged = waitUntil([&] {
        const std::optional<ResourceSnapshot> radioSnapshot = store.get(radioAddress);
        return radioSnapshot
            && radioSnapshot->value.value(QStringLiteral("connected")).toBool()
            && store.get(sliceAddress).has_value()
            && store.get(panAddress).has_value();
    });
    if (!check(converged,
               "SimBackend must drive connected radio, slice, and panadapter resources")) {
        radio.disconnectFromRadio();
        return false;
    }

    const QJsonObject sliceReply = invoke(
        &service, &client, QStringLiteral("sim-get"),
        QStringLiteral("resource.get"),
        {{QStringLiteral("resource"),
          exactResource(QStringLiteral("slice"), QStringLiteral("radio-1"),
                        QStringLiteral("0"))}});
    const QJsonObject sliceValue = sliceReply.value(QStringLiteral("result")).toObject()
                                       .value(QStringLiteral("value")).toObject();
    const QJsonObject radioValue = store.get(radioAddress)->value;
    const QJsonObject panValue = store.get(panAddress)->value;
    const QJsonObject identity = radioValue.value(QStringLiteral("identity")).toObject();
    const QJsonObject capabilities =
        radioValue.value(QStringLiteral("capabilities")).toObject();
    const QJsonObject receive = sliceValue.value(QStringLiteral("receive")).toObject();
    const QJsonObject displayCadence =
        panValue.value(QStringLiteral("displayCadence")).toObject();
    if (!check(hasExactlyKeys(radioValue,
                              {"id", "connected", "family", "identity", "capabilities"})
                   && hasExactlyKeys(identity,
                                     {"name", "model", "serial", "version", "manufacturer"})
                   && hasExactlyKeys(capabilities,
                                     {"maxSlices", "maxPanadapters", "sampleRatesHz",
                                      "tuningRangeHz", "declaredBands", "canTransmit",
                                      "maximumTransmitWatts", "hasTuner", "hasAmplifier",
                                      "extensions"})
                   && hasExactlyKeys(sliceValue,
                                     {"id", "letter", "panadapterId", "owned",
                                      "frequencyHz", "mode", "filter", "active",
                                      "txSlice", "locked", "audio", "receive"})
                   && hasExactlyKeys(sliceValue.value(QStringLiteral("filter")).toObject(),
                                     {"lowHz", "highHz"})
                   && hasExactlyKeys(sliceValue.value(QStringLiteral("audio")).toObject(),
                                     {"gain", "pan", "muted"})
                   && hasExactlyKeys(receive,
                                     {"antenna", "rfGain", "agc", "squelch"})
                   && hasExactlyKeys(receive.value(QStringLiteral("agc")).toObject(),
                                     {"mode", "threshold", "offLevel"})
                   && hasExactlyKeys(receive.value(QStringLiteral("squelch")).toObject(),
                                     {"enabled", "level"})
                   && hasExactlyKeys(panValue,
                                     {"id", "centerHz", "centerKnown",
                                      "bandwidthHz", "dbmRange", "bandwidthLimitsHz",
                                      "receive", "displayCadence"})
                   && hasExactlyKeys(panValue.value(QStringLiteral("dbmRange")).toObject(),
                                     {"minimum", "maximum"})
                   && hasExactlyKeys(
                       panValue.value(QStringLiteral("bandwidthLimitsHz")).toObject(),
                       {"minimum", "maximum"})
                   && hasExactlyKeys(panValue.value(QStringLiteral("receive")).toObject(),
                                     {"antenna", "rfGain"})
                   && hasExactlyKeys(displayCadence,
                                     {"fps", "averageFrames", "weightedAverage",
                                      "weightedAverageKnown", "waterfallRate"}),
               "SimBackend resources must match the complete documented v1 schemas")) {
        radio.disconnectFromRadio();
        return false;
    }

    QList<QJsonObject> events = drain(client);
    PanadapterModel* modelPan = radio.panadapter(QStringLiteral("0x40000000"));
    if (!check(modelPan && !modelPan->weightedAverageKnown(),
               "weighted averaging must begin unknown before its first report")) {
        radio.disconnectFromRadio();
        return false;
    }
    const quint64 weightedRevision = store.get(panAddress)->revision;
    modelPan->applyStateExtension(
        {{QStringLiteral("weighted_average"), QStringLiteral("0")}});
    const std::optional<ResourceSnapshot> reportedWeighted = store.get(panAddress);
    const QList<QJsonObject> weightedEvents = drain(client);
    events.append(weightedEvents);
    if (!check(reportedWeighted && reportedWeighted->revision > weightedRevision
                   && reportedWeighted->value.value(QStringLiteral("displayCadence"))
                          .toObject().value(QStringLiteral("weightedAverageKnown")).toBool()
                   && !reportedWeighted->value.value(QStringLiteral("displayCadence"))
                           .toObject().value(QStringLiteral("weightedAverage")).toBool()
                   && weightedEvents.size() == 1
                   && weightedEvents.first().value(QStringLiteral("event")).toString()
                          == QStringLiteral("resource.changed"),
               "the first known-false weighted-average report must publish an event")) {
        radio.disconnectFromRadio();
        return false;
    }
    const quint64 knownWeightedRevision = reportedWeighted->revision;
    modelPan->applyStateExtension(
        {{QStringLiteral("weighted_average"), QStringLiteral("0")}});
    if (!check(store.get(panAddress)->revision == knownWeightedRevision
                   && drain(client).isEmpty(),
               "an identical known weighted-average report must be deduplicated")) {
        radio.disconnectFromRadio();
        return false;
    }

    const quint64 sliceRevision = store.get(sliceAddress)->revision;
    QJsonObject staleSliceValue = sliceValue;
    staleSliceValue.insert(QStringLiteral("owned"), false);
    store.upsert(sliceAddress, staleSliceValue);
    radio.slotOccupancyChanged(0);
    const std::optional<ResourceSnapshot> refreshedSlice = store.get(sliceAddress);
    if (!check(refreshedSlice && refreshedSlice->revision > sliceRevision
                   && refreshedSlice->value.value(QStringLiteral("owned")).toBool(),
               "slot occupancy changes must republish RadioModel-derived ownership")) {
        radio.disconnectFromRadio();
        return false;
    }

    const quint64 panRevision = store.get(panAddress)->revision;
    QJsonObject stalePanValue = panValue;
    stalePanValue.insert(QStringLiteral("centerKnown"),
                         !panValue.value(QStringLiteral("centerKnown")).toBool());
    store.upsert(panAddress, stalePanValue);
    radio.panadapterReclaimed(radio.panadapter(QStringLiteral("0x40000000")));
    const std::optional<ResourceSnapshot> refreshedPan = store.get(panAddress);
    if (!check(refreshedPan && refreshedPan->revision > panRevision
                   && refreshedPan->value.value(QStringLiteral("centerKnown"))
                          == panValue.value(QStringLiteral("centerKnown")),
               "panadapter reclaim must republish canonical model state")) {
        radio.disconnectFromRadio();
        return false;
    }
    events.append(drain(client));
    bool sawRadio = false;
    bool sawSlice = false;
    bool sawPan = false;
    qint64 previousSequence = 0;
    for (const QJsonObject& event : events) {
        const qint64 sequence = event.value(QStringLiteral("sequence")).toInteger();
        if (sequence <= previousSequence) {
            radio.disconnectFromRadio();
            return check(false, "SimBackend events must remain ordered after coalescing");
        }
        previousSequence = sequence;
        const QString type = event.value(QStringLiteral("resource")).toObject()
                                 .value(QStringLiteral("type")).toString();
        sawRadio = sawRadio || type == QStringLiteral("radioSession");
        sawSlice = sawSlice || type == QStringLiteral("slice");
        sawPan = sawPan || type == QStringLiteral("panadapter");
    }

    const SliceModel* modelSlice = radio.slice(0);
    const bool result =
        check(modelSlice
                  && sliceValue.value(QStringLiteral("frequencyHz")).toInteger()
                         == qRound64(modelSlice->frequency() * 1'000'000.0)
                  && sliceValue.value(QStringLiteral("mode")).toString()
                         == modelSlice->mode(),
              "resource.get must expose SimBackend's authoritative slice state")
        && check(sawRadio && sawSlice && sawPan,
                 "the subscription must deliver each SimBackend resource family")
        && check(!radio.backendCapabilities().canTransmit,
                 "the end-to-end simulator proof must remain RX-only");
    radio.disconnectFromRadio();
    const bool removed = waitUntil([&] {
        return !radio.isConnected()
            && !store.get(sliceAddress).has_value()
            && !store.get(panAddress).has_value();
    });
    if (!result
        || !check(removed,
                  "disconnect must remove dynamic resources and update the radio session")) {
        return false;
    }

    radio.connectToRadio(demoInfo());
    const bool reclaimed = waitUntil([&] {
        return radio.isConnected()
            && store.get(sliceAddress).has_value()
            && store.get(panAddress).has_value();
    });
    radio.disconnectFromRadio();
    waitUntil([&radio] { return !radio.isConnected(); });
    return check(reclaimed,
                 "reconnect must republish reclaimed slice and panadapter resources");
}

} // namespace

int main(int argc, char* argv[])
{
    TestSettingsProfile settingsProfile(
        QStringLiteral("aether-control-resource-service-test"));
    if (!check(settingsProfile.isValid(),
               "isolated settings profile must be available")) {
        return 1;
    }
    QCoreApplication app(argc, argv);
    return testStoreRevisions()
        && testServiceSubscriptions()
        && testSubscribeSequenceBoundary()
        && testOverflowRequiresResync()
        && testWireFramingIsCanonical()
        && testPureSeamReconnectRepublishesSlice()
        && testExternalReceiveAudioRepublishesAgcAndSquelch()
        && testSliceRemovalMatchesTheLiveObject()
        && testSimBackendEndToEnd() ? 0 : 1;
}
