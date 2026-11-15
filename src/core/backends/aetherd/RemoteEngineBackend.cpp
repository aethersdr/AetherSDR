#include "RemoteEngineBackend.h"
#include "RemoteEngineMapping.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLoggingCategory>
#include <QNetworkInterface>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslError>
#include <QUrl>
#include <QWebSocket>

#include <climits>
#include <cmath>

Q_LOGGING_CATEGORY(lcRemoteEngine, "aether.backend.aetherd")

namespace AetherSDR::aetherd {

namespace {

const QString kRadioSession = QStringLiteral("radio-1");

QString upInterfaceHolding(const QHostAddress& address)
{
    // Not "interface": that is a macro once windows.h is in the translation unit.
    for (const QNetworkInterface& candidate : QNetworkInterface::allInterfaces()) {
        if (!candidate.flags().testFlag(QNetworkInterface::IsUp)) { continue; }
        for (const QNetworkAddressEntry& entry : candidate.addressEntries()) {
            if (entry.ip().isEqual(address, QHostAddress::ConvertV4MappedToIPv4)) {
                return candidate.name();
            }
        }
    }
    return {};
}

// The certificate problems a self-made, pinned certificate always has. Any
// other error (an expired certificate, a broken chain signature) is not ours
// to wave through even when the digest matches.
bool pinnableError(QSslError::SslError error)
{
    switch (error) {
    case QSslError::SelfSignedCertificate:
    case QSslError::SelfSignedCertificateInChain:
    case QSslError::HostNameMismatch:
    case QSslError::CertificateUntrusted:
    case QSslError::UnableToGetLocalIssuerCertificate:
    case QSslError::UnableToVerifyFirstCertificate:
        return true;
    default:
        return false;
    }
}

int priority(const QString& type)
{
    if (type == QLatin1String("radioSession")) { return 0; }
    if (type == QLatin1String("panadapter")) { return 1; }
    return 2;
}

} // namespace

RemoteEngineBackend::RemoteEngineBackend(QObject* parent)
    : RemoteEngineBackend(Options{}, parent)
{
}

RemoteEngineBackend::Options RemoteEngineBackend::withDefaults(Options options)
{
    if (!options.clockMs) {
        auto timer = std::make_shared<QElapsedTimer>();
        timer->start();
        options.clockMs = [timer] { return timer->elapsed(); };
    }
    if (!options.interfaceForLocalAddress) {
        options.interfaceForLocalAddress = upInterfaceHolding;
    }
    return options;
}

RemoteEngineBackend::RemoteEngineBackend(Options options, QObject* parent)
    : IRadioBackend(parent), m_options(withDefaults(std::move(options))),
      m_scheduler(m_options.clockMs)
{
    m_pumpTimer.setSingleShot(true);
    connect(&m_pumpTimer, &QTimer::timeout, this, [this] { pump(); });
}

RemoteEngineBackend::~RemoteEngineBackend()
{
    // Rule 5: no signal after this point; the socket is torn down quietly.
    ++m_generation;
    if (m_socket) {
        m_socket->disconnect(this);
        m_socket->abort();
        m_socket.reset();
    }
}

std::optional<RemoteEngineRecord> RemoteEngineBackend::recordFor(const RadioConnectRequest& request)
{
    const std::optional<RemoteEngineRecord> record = RemoteEngineCredentials::session(request.serial);
    if (!record || !record->usable()) { return std::nullopt; }
    return record;
}

bool RemoteEngineBackend::tailnetPathAdmitted(const QHostAddress& localAddress,
    const std::function<QString(const QHostAddress&)>& interfaceFor)
{
    if (localAddress.isNull() || !interfaceFor) { return false; }
    const QString name = interfaceFor(localAddress);
    return name.startsWith(QLatin1String("tailscale")) || name.startsWith(QLatin1String("wg"));
}

QString RemoteEngineBackend::fingerprintOf(const QByteArray& certificateDer)
{
    return QString::fromLatin1(QCryptographicHash::hash(certificateDer, QCryptographicHash::Sha256).toHex());
}

RadioCapabilities RemoteEngineBackend::capabilities() const
{
    const auto session = m_cache.constFind(resourceKey(QStringLiteral("radioSession"), {}, kRadioSession));
    return capabilitiesFrom(session == m_cache.constEnd() ? QJsonObject{} : session->value, m_agcMode);
}

void RemoteEngineBackend::connectRadio(const RadioConnectRequest& request)
{
    if (m_phase != Phase::Idle) { finish(); }
    const std::optional<RemoteEngineRecord> record = recordFor(request);
    if (!record) {
        emit connectionError(tr("This remote engine is not paired on this machine. Pair it again "
                                "from the Remote engine section."));
        return;
    }
    // The token goes only to the address it was paired at, or to a tailnet
    // address stored in the same engine's record.
    bool tailnet = false;
    if (!record->tailnetUrl.isEmpty()) {
        const QUrl url(record->tailnetUrl);
        tailnet = QHostAddress(url.host()) == QHostAddress(request.host)
            && (request.port == 0 || url.port() == request.port);
    }
    openSocket(*record, tailnet);
}

void RemoteEngineBackend::openSocket(const RemoteEngineRecord& record, bool tailnet)
{
    const QUrl url(tailnet ? record.tailnetUrl : record.lanUrl);
    if (!url.isValid() || url.scheme() != (tailnet ? QLatin1String("ws") : QLatin1String("wss"))) {
        emit connectionError(tailnet
            ? tr("The tailnet address must be a ws:// address.")
            : tr("The paired address must be a wss:// address."));
        return;
    }
    const quint64 generation = ++m_generation;
    m_tailnet = tailnet;
    m_pin = record.engineId;
    m_token = record.token;
    m_sessionId.clear();
    m_lastSequence = -1;
    m_snapshotReady = false;
    m_pending.clear();
    m_cache.clear();
    m_sentMethods.clear();
    m_helloSent = false;
    m_agcMode.clear();
    m_scheduler.clear();
    m_scheduler.setLimits({});
    m_socket = std::make_unique<QWebSocket>(QStringLiteral("aethersdr"), QWebSocketProtocol::VersionLatest);
    QWebSocket* socket = m_socket.get();
    connect(socket, &QWebSocket::connected, this, [this, generation] {
        if (generation == m_generation) { onSocketConnected(); }
    });
    connect(socket, &QWebSocket::textMessageReceived, this, [this, generation](const QString& text) {
        if (generation == m_generation) { onText(text); }
    });
    connect(socket, &QWebSocket::disconnected, this, [this, generation] {
        if (generation == m_generation) { finish(); }
    });
    connect(socket, &QWebSocket::errorOccurred, this, [this, generation, socket](QAbstractSocket::SocketError) {
        if (generation == m_generation && m_phase != Phase::Live) { fail(socket->errorString()); }
    });
    connect(socket, &QWebSocket::sslErrors, this, [this, generation, socket](const QList<QSslError>& errors) {
        if (generation != m_generation) { return; }
        const QString seen = fingerprintOf(socket->sslConfiguration().peerCertificate().toDer());
        if (seen != m_pin) {
            fail(tr("The engine's certificate changed (paired %1…, now %2…). Nothing was sent. "
                    "Pair again on the radio's LAN.").arg(m_pin.left(12), seen.left(12)));
            return;
        }
        for (const QSslError& error : errors) {
            if (!pinnableError(error.error())) {
                fail(tr("The engine's certificate is unusable: %1").arg(error.errorString()));
                return;
            }
        }
        socket->ignoreSslErrors(errors);
    });
    m_phase = Phase::Opening;
    socket->open(url);
}

void RemoteEngineBackend::onSocketConnected()
{
    if (m_tailnet) {
        if (!tailnetPathAdmitted(m_socket->localAddress(), m_options.interfaceForLocalAddress)) {
            fail(tr("A ws:// tailnet address is used only over a WireGuard (Tailscale) interface; "
                    "this connection is not. Nothing was sent."));
            return;
        }
    } else {
        // A certificate a CA happens to vouch for raises no sslErrors: pin here too.
        const QString seen = fingerprintOf(m_socket->sslConfiguration().peerCertificate().toDer());
        if (seen != m_pin) {
            fail(tr("The engine's certificate changed (paired %1…, now %2…). Nothing was sent. "
                    "Pair again on the radio's LAN.").arg(m_pin.left(12), seen.left(12)));
            return;
        }
    }
    m_phase = Phase::Hello;
    sendNow(Pending::Kind::Hello, QStringLiteral("hello"), QJsonObject{
        {QStringLiteral("versions"), QJsonArray{1}},
        {QStringLiteral("auth"), QJsonObject{{QStringLiteral("scheme"), QStringLiteral("bearer")},
                                             {QStringLiteral("token"), m_token}}}});
}

void RemoteEngineBackend::sendNow(Pending::Kind kind, const QString& method, const QJsonObject& params,
                                  const QString& resourceKey, bool retried)
{
    if (!m_socket) { return; }
    const QString id = QString::number(++m_nextId);
    QJsonObject request{{QStringLiteral("v"), 1}, {QStringLiteral("id"), id},
                        {QStringLiteral("method"), method}, {QStringLiteral("params"), params}};
    if (kind != Pending::Kind::Hello) {
        request.insert(QStringLiteral("sessionId"), m_sessionId);
        m_sentMethods.append(method);
    } else {
        m_helloSent = true;
    }
    m_pending.insert(id, Pending{kind, resourceKey, method, params, retried});
    m_socket->sendTextMessage(QString::fromUtf8(QJsonDocument(request).toJson(QJsonDocument::Compact)));
}

void RemoteEngineBackend::schedulePump()
{
    if (m_phase != Phase::Live) { return; }
    const std::optional<qint64> wait = m_scheduler.msUntilReady();
    if (!wait) { return; }
    if (*wait == 0) {
        pump();
        return;
    }
    if (!m_pumpTimer.isActive()) { m_pumpTimer.start(int(std::min<qint64>(*wait, 1000))); }
}

void RemoteEngineBackend::pump()
{
    if (m_phase != Phase::Live) { return; }
    while (std::optional<RemoteEngineScheduler::Frame> frame = m_scheduler.takeNext()) {
        Pending::Kind kind = Pending::Kind::Intent;
        QString key = frame->resourceKey;
        if (frame->method == QLatin1String("resource.subscribe")) {
            kind = Pending::Kind::Subscribe;
        } else if (frame->method == QLatin1String("resource.get")) {
            kind = Pending::Kind::Get;
            key = resourceKey(frame->params.value(QStringLiteral("resource")).toObject());
        } else {
            // The revision is read now, not at submit: a reply to our previous
            // request may have advanced it in the meantime.
            const auto cachedResource = m_cache.constFind(key);
            if (cachedResource == m_cache.constEnd()) {
                m_scheduler.completed(key);
                continue;
            }
            frame->params.insert(QStringLiteral("expectedRevision"), qint64(cachedResource->revision));
        }
        sendNow(kind, frame->method, frame->params, key, frame->retried);
    }
    schedulePump();
}

void RemoteEngineBackend::subscribe()
{
    m_scheduler.enqueuePlain(QStringLiteral("resource.subscribe"), QJsonObject{
        {QStringLiteral("resources"), QJsonArray{
             QJsonObject{{QStringLiteral("type"), QStringLiteral("radioSession")}},
             QJsonObject{{QStringLiteral("type"), QStringLiteral("slice")},
                         {QStringLiteral("radioSession"), kRadioSession}},
             QJsonObject{{QStringLiteral("type"), QStringLiteral("panadapter")},
                         {QStringLiteral("radioSession"), kRadioSession}}}}});
    schedulePump();
}

void RemoteEngineBackend::onText(const QString& text)
{
    const QJsonObject message = QJsonDocument::fromJson(text.toUtf8()).object();
    if (message.isEmpty()) { return; }
    if (message.contains(QStringLiteral("event"))) {
        onEvent(message);
    } else if (message.contains(QStringLiteral("id"))) {
        onResponse(message);
    }
}

void RemoteEngineBackend::onResponse(const QJsonObject& message)
{
    const auto found = m_pending.find(message.value(QStringLiteral("id")).toString());
    if (found == m_pending.end()) { return; }
    const Pending pending = *found;
    m_pending.erase(found);
    const QJsonObject error = message.value(QStringLiteral("error")).toObject();
    const QString code = error.value(QStringLiteral("code")).toString();
    const QJsonObject result = message.value(QStringLiteral("result")).toObject();

    switch (pending.kind) {
    case Pending::Kind::Hello:
        if (!code.isEmpty()) {
            if (code.startsWith(QLatin1String("auth."))) {
                fail(tr("The engine no longer accepts this machine's pairing (%1). "
                        "Pair again on the radio's screen.").arg(code));
            } else if (code == QLatin1String("transport.limit_exceeded")) {
                fail(tr("The engine is already serving its maximum number of remote clients."));
            } else {
                fail(tr("The engine refused the connection (%1).").arg(code));
            }
            return;
        }
        m_sessionId = result.value(QStringLiteral("sessionId")).toString();
        m_scheduler.setLimits(RemoteEngineScheduler::clientLimits(
            result.value(QStringLiteral("limits")).toObject()));
        m_phase = Phase::Live;
        m_connected = true;
        emit connected();
        subscribe();
        return;
    case Pending::Kind::Subscribe:
        if (!code.isEmpty()) {
            fail(tr("The engine refused the subscription (%1).").arg(code));
            return;
        }
        m_lastSequence = std::max(m_lastSequence,
                                  qint64(result.value(QStringLiteral("sequence")).toDouble(-1)));
        applyBaseline(result.value(QStringLiteral("resources")).toArray(), true);
        m_snapshotReady = true;
        refreshCapabilities();
        schedulePump();
        return;
    case Pending::Kind::Get: {
        if (code.isEmpty()) {
            applyResource(result.value(QStringLiteral("resource")).toObject(),
                          quint64(result.value(QStringLiteral("revision")).toDouble()),
                          result.value(QStringLiteral("value")).toObject());
        }
        // The conflicted intent this re-read serves is retried once, unless a
        // newer intent for the same method is already queued (latest wins).
        const std::pair<QString, QJsonObject> retry = m_retryIntents.take(pending.resourceKey);
        m_scheduler.completed(pending.resourceKey);
        if (code.isEmpty() && !retry.first.isEmpty()
            && !m_scheduler.hasQueuedIntent(pending.resourceKey, retry.first)) {
            m_scheduler.submitIntent(pending.resourceKey, retry.first, retry.second, true);
        }
        break;
    }
    case Pending::Kind::Intent:
        if (code == QLatin1String("request.conflict") && !pending.retried) {
            // Hold the resource while it is re-read; the Get's reply retries.
            QJsonObject params = pending.params;
            params.remove(QStringLiteral("expectedRevision"));
            const auto cachedResource = m_cache.constFind(pending.resourceKey);
            if (cachedResource == m_cache.constEnd()) {
                m_scheduler.completed(pending.resourceKey);
                break;
            }
            const QJsonObject getParams{{QStringLiteral("resource"), cachedResource->address}};
            m_scheduler.enqueuePlain(QStringLiteral("resource.get"), getParams);
            // Remember which intent the re-read serves.
            m_retryIntents.insert(pending.resourceKey, {pending.method, params});
            break;
        }
        if (!code.isEmpty()) {
            qCInfo(lcRemoteEngine) << "remote engine refused" << pending.method << code
                                   << error.value(QStringLiteral("message")).toString();
        }
        m_scheduler.completed(pending.resourceKey);
        break;
    }
    // A settled resource shows the engine's value again, including any clamp.
    if (!pending.resourceKey.isEmpty()) {
        const QStringList parts = pending.resourceKey.split(QLatin1Char('/'));
        if (parts.size() == 3) {
            if (const Cached* resource = cached(parts.at(0), parts.at(2))) {
                const Cached copy = *resource;
                if (parts.at(0) == QLatin1String("slice")) { publishSlice(copy); }
                else if (parts.at(0) == QLatin1String("panadapter")) { publishPan(copy); }
                else if (parts.at(0) == QLatin1String("radioSession")) { publishSession(copy); }
            }
        }
    }
    schedulePump();
}

void RemoteEngineBackend::onEvent(const QJsonObject& message)
{
    const qint64 sequence = qint64(message.value(QStringLiteral("sequence")).toDouble(-1));
    // Sequences rise but skip numbers: the engine coalesces a resource's queued
    // events to the newest complete value. Only an out-of-order one is stale.
    if (sequence <= m_lastSequence) { return; }
    m_lastSequence = sequence;
    const QString event = message.value(QStringLiteral("event")).toString();
    if (event == QLatin1String("resource.changed")) {
        applyResource(message.value(QStringLiteral("resource")).toObject(),
                      quint64(message.value(QStringLiteral("revision")).toDouble()),
                      message.value(QStringLiteral("value")).toObject());
    } else if (event == QLatin1String("resource.removed")) {
        removeResource(message.value(QStringLiteral("resource")).toObject());
    } else if (event == QLatin1String("resource.resyncRequired")) {
        subscribe();
    }
}

void RemoteEngineBackend::applyBaseline(const QJsonArray& resources, bool replaceAll)
{
    QList<QJsonObject> ordered;
    for (const QJsonValue& entry : resources) { ordered.append(entry.toObject()); }
    std::stable_sort(ordered.begin(), ordered.end(), [](const QJsonObject& a, const QJsonObject& b) {
        return priority(a.value(QStringLiteral("resource")).toObject().value(QStringLiteral("type")).toString())
            < priority(b.value(QStringLiteral("resource")).toObject().value(QStringLiteral("type")).toString());
    });
    if (replaceAll) {
        QSet<QString> present;
        for (const QJsonObject& entry : ordered) {
            present.insert(resourceKey(entry.value(QStringLiteral("resource")).toObject()));
        }
        const QList<QString> keys = m_cache.keys();
        for (const QString& key : keys) {
            if (!present.contains(key)) { removeResource(m_cache.value(key).address); }
        }
    }
    for (const QJsonObject& entry : ordered) {
        const QJsonObject address = entry.value(QStringLiteral("resource")).toObject();
        // A baseline is authoritative even at an equal revision.
        m_cache.remove(resourceKey(address));
        applyResource(address, quint64(entry.value(QStringLiteral("revision")).toDouble()),
                      entry.value(QStringLiteral("value")).toObject());
    }
}

void RemoteEngineBackend::applyResource(const QJsonObject& address, quint64 revision,
                                        const QJsonObject& value)
{
    const QString key = resourceKey(address);
    const auto existing = m_cache.constFind(key);
    if (existing != m_cache.constEnd() && existing->revision >= revision) { return; }
    const Cached entry{address, revision, value};
    m_cache.insert(key, entry);
    const QString type = address.value(QStringLiteral("type")).toString();
    if (type == QLatin1String("slice")) { publishSlice(entry); }
    else if (type == QLatin1String("panadapter")) { publishPan(entry); }
    else if (type == QLatin1String("radioSession")) { publishSession(entry); }
}

void RemoteEngineBackend::removeResource(const QJsonObject& address)
{
    const QString key = resourceKey(address);
    if (!m_cache.remove(key)) { return; }
    m_scheduler.completed(key);
    const QString type = address.value(QStringLiteral("type")).toString();
    const QString id = address.value(QStringLiteral("id")).toString();
    if (type == QLatin1String("slice")) {
        if (const std::optional<int> slice = canonicalSliceId(id)) { emit sliceRemoved(*slice); }
    } else if (type == QLatin1String("panadapter")) {
        emit panRemoved(id);
    }
}

bool RemoteEngineBackend::outstanding(const QString& key, const QString& method) const
{
    return m_scheduler.intentOutstanding(key, method) || m_retryIntents.value(key).first == method;
}

void RemoteEngineBackend::publishSlice(const Cached& slice)
{
    const QString id = slice.address.value(QStringLiteral("id")).toString();
    const std::optional<int> sliceId = canonicalSliceId(id);
    if (!sliceId) { return; }
    const QString key = resourceKey(slice.address);
    const RadioCapabilities caps = capabilities();
    SliceDelta delta = sliceDeltaFrom(slice.value,
        caps.receiveModeControl ? caps.receiveModeControl->modes : QStringList{});
    // Our own intent is still in flight: keep the operator's value on screen
    // rather than the engine's older one (it is shown again once settled).
    if (outstanding(key, QStringLiteral("slice.setFrequency"))) { delta.frequency.reset(); }
    if (outstanding(key, QStringLiteral("slice.setMode"))) { delta.mode.reset(); }
    if (outstanding(key, QStringLiteral("slice.setFilter"))) { delta.filterLow.reset(); delta.filterHigh.reset(); }
    if (outstanding(key, QStringLiteral("slice.setAgcThreshold"))) { delta.agcThreshold.reset(); }
    if (outstanding(key, QStringLiteral("slice.setAudioGain"))) { delta.audioGain.reset(); }
    if (outstanding(key, QStringLiteral("slice.setAudioMute"))) { delta.audioMute.reset(); }
    if (outstanding(key, QStringLiteral("slice.setNoiseBlanker"))) { delta.nb.reset(); }
    if (outstanding(key, QStringLiteral("slice.setNoiseReduction"))) { delta.nr.reset(); }
    if (outstanding(key, QStringLiteral("slice.setRit"))) { delta.ritOn.reset(); delta.ritFreq.reset(); }
    emit sliceChanged(*sliceId, delta);
    // The AGC selectors offer only the running mode (v1 has no AGC-mode method).
    if (delta.agcMode && (slice.value.value(QStringLiteral("active")).toBool() || m_agcMode.isEmpty())
        && *delta.agcMode != m_agcMode) {
        m_agcMode = *delta.agcMode;
        if (m_snapshotReady) { emit capabilitiesChanged(); }
    }
}

void RemoteEngineBackend::publishPan(const Cached& pan)
{
    const std::optional<PanState> state = panStateFrom(pan.value);
    if (!state) { return; }
    const QString panId = pan.address.value(QStringLiteral("id")).toString();
    const QString key = resourceKey(pan.address);
    if (state->minimumBandwidthMhz && state->maximumBandwidthMhz) {
        emit panBandwidthLimitsChanged(panId, *state->minimumBandwidthMhz, *state->maximumBandwidthMhz);
    }
    if (!outstanding(key, QStringLiteral("panadapter.setCenter"))
        && !outstanding(key, QStringLiteral("panadapter.setBandwidth"))) {
        emit panCenterBandwidthChanged(panId, state->centerMhz, state->bandwidthMhz);
    }
    if (state->rfGain && !outstanding(key, QStringLiteral("panadapter.setRfGain"))) {
        emit panRfGainChanged(panId, *state->rfGain);
    }
}

void RemoteEngineBackend::publishSession(const Cached& session)
{
    RadioDelta delta;
    delta.model = session.value.value(QStringLiteral("identity")).toObject()
                      .value(QStringLiteral("model")).toString();
    emit radioChanged(delta);
    const QJsonObject speaker = session.value.value(QStringLiteral("receiveObservation")).toObject()
                                    .value(QStringLiteral("radioSpeaker")).toObject();
    if (speaker.value(QStringLiteral("known")).toBool() && speaker.value(QStringLiteral("enabled")).isBool()
        && !outstanding(resourceKey(session.address), QStringLiteral("radioSession.setRadioSpeaker"))) {
        emit radioSpeakerAudioReported(speaker.value(QStringLiteral("enabled")).toBool());
    }
    if (m_snapshotReady) { refreshCapabilities(); }
}

void RemoteEngineBackend::refreshCapabilities()
{
    const auto session = m_cache.constFind(resourceKey(QStringLiteral("radioSession"), {}, kRadioSession));
    const QJsonObject caps = session == m_cache.constEnd()
        ? QJsonObject{} : session->value.value(QStringLiteral("capabilities")).toObject();
    if (caps == m_capabilityJson && m_snapshotReady && !m_capabilityJson.isEmpty()) { return; }
    m_capabilityJson = caps;
    emit capabilitiesChanged();
}

const RemoteEngineBackend::Cached* RemoteEngineBackend::cached(const QString& type, const QString& id) const
{
    const auto it = m_cache.constFind(resourceKey(type, type == QLatin1String("radioSession") ? QString() : kRadioSession, id));
    return it == m_cache.constEnd() ? nullptr : &it.value();
}

bool RemoteEngineBackend::submit(const QString& type, const QString& id, const QString& method, QJsonObject params)
{
    // Rule 1: nothing goes to the engine before its state has been seen once;
    // a verb the model pushes while connecting is dropped, never replayed.
    if (m_phase != Phase::Live || !m_snapshotReady) { return false; }
    const Cached* resource = cached(type, id);
    if (!resource) { return false; }
    params.insert(QStringLiteral("radioSession"), kRadioSession);
    if (type == QLatin1String("slice")) { params.insert(QStringLiteral("slice"), id); }
    else if (type == QLatin1String("panadapter")) { params.insert(QStringLiteral("panadapter"), id); }
    m_scheduler.submitIntent(resourceKey(resource->address), method, params);
    schedulePump();
    return true;
}

void RemoteEngineBackend::setSliceFrequency(int sliceId, double hz)
{
    if (!std::isfinite(hz) || hz < 1.0) { return; }
    submit(QStringLiteral("slice"), QString::number(sliceId), QStringLiteral("slice.setFrequency"),
           {{QStringLiteral("hz"), qint64(std::llround(hz))}});
}

void RemoteEngineBackend::setSliceMode(int sliceId, const QString& mode)
{
    submit(QStringLiteral("slice"), QString::number(sliceId), QStringLiteral("slice.setMode"),
           {{QStringLiteral("mode"), mode.toUpper()}});
}

void RemoteEngineBackend::setSliceFilter(int sliceId, int lowHz, int highHz)
{
    submit(QStringLiteral("slice"), QString::number(sliceId), QStringLiteral("slice.setFilter"),
           {{QStringLiteral("lowHz"), lowHz}, {QStringLiteral("highHz"), highHz}});
}

void RemoteEngineBackend::setSliceAgc(int sliceId, const QString& mode, int thresholdDb)
{
    const Cached* slice = cached(QStringLiteral("slice"), QString::number(sliceId));
    if (!slice) { return; }
    const QJsonObject agc = slice->value.value(QStringLiteral("receive")).toObject()
                                .value(QStringLiteral("agc")).toObject();
    const QString current = agc.value(QStringLiteral("mode")).toString();
    if (!mode.isEmpty() && mode.compare(current, Qt::CaseInsensitive) != 0) {
        // v1 has no AGC-mode method; the selectors never offer another mode.
        qCInfo(lcRemoteEngine) << "remote engine: AGC mode" << mode << "dropped; the engine runs" << current;
        return;
    }
    if (agc.value(QStringLiteral("threshold")).toInt(INT_MIN) == thresholdDb
        && !outstanding(resourceKey(slice->address), QStringLiteral("slice.setAgcThreshold"))) {
        return;
    }
    submit(QStringLiteral("slice"), QString::number(sliceId), QStringLiteral("slice.setAgcThreshold"),
           {{QStringLiteral("threshold"), thresholdDb}});
}

ReceiveDispatch RemoteEngineBackend::requestSliceDsp(int sliceId, const SliceDspRequest& request)
{
    if (request.field != SliceDspRequest::Field::Enabled) { return ReceiveDispatch::Unsupported; }
    QString method;
    if (request.feature == SliceDspRequest::Feature::Nb) { method = QStringLiteral("slice.setNoiseBlanker"); }
    else if (request.feature == SliceDspRequest::Feature::Nr) { method = QStringLiteral("slice.setNoiseReduction"); }
    else { return ReceiveDispatch::Unsupported; }
    return submit(QStringLiteral("slice"), QString::number(sliceId), method,
                  {{QStringLiteral("enabled"), request.enabled}})
        ? ReceiveDispatch::Dispatched : ReceiveDispatch::Unsupported;
}

ReceiveDispatch RemoteEngineBackend::requestSliceAudio(int sliceId, const SliceAudioRequest& request)
{
    if (!request.valid() || request.origin != SliceAudioRequest::Origin::Operator) {
        return ReceiveDispatch::Unsupported;
    }
    bool sent = false;
    switch (request.field) {
    case SliceAudioRequest::Field::Gain:
        sent = submit(QStringLiteral("slice"), QString::number(sliceId), QStringLiteral("slice.setAudioGain"),
                      {{QStringLiteral("gain"), request.value}});
        break;
    case SliceAudioRequest::Field::Mute:
        sent = submit(QStringLiteral("slice"), QString::number(sliceId), QStringLiteral("slice.setAudioMute"),
                      {{QStringLiteral("muted"), request.value != 0}});
        break;
    case SliceAudioRequest::Field::Pan:
        break;
    }
    return sent ? ReceiveDispatch::Dispatched : ReceiveDispatch::Unsupported;
}

ReceiveDispatch RemoteEngineBackend::requestSliceControl(int sliceId, const SliceControlRequest& request)
{
    if (request.valid() && request.field == SliceControlRequest::Field::Rit) {
        const IncrementalTuning tuning = std::get<IncrementalTuning>(request.value);
        return submit(QStringLiteral("slice"), QString::number(sliceId), QStringLiteral("slice.setRit"),
                      {{QStringLiteral("enabled"), tuning.enabled}, {QStringLiteral("offsetHz"), tuning.hz}})
            ? ReceiveDispatch::Dispatched : ReceiveDispatch::Unsupported;
    }
    return IRadioBackend::requestSliceControl(sliceId, request);
}

ReceiveDispatch RemoteEngineBackend::requestRadioSpeakerAudio(bool on)
{
    return submit(QStringLiteral("radioSession"), kRadioSession, QStringLiteral("radioSession.setRadioSpeaker"),
                  {{QStringLiteral("enabled"), on}})
        ? ReceiveDispatch::Dispatched : ReceiveDispatch::Unsupported;
}

void RemoteEngineBackend::setCwPitch(int hz)
{
    submit(QStringLiteral("radioSession"), kRadioSession, QStringLiteral("radioSession.setCwPitch"),
           {{QStringLiteral("hz"), hz}});
}

void RemoteEngineBackend::setPanCenter(const QString& panId, double hz, PanCenterIntent intent)
{
    Q_UNUSED(intent);
    if (!std::isfinite(hz) || hz < 1.0) { return; }
    submit(QStringLiteral("panadapter"), panId, QStringLiteral("panadapter.setCenter"),
           {{QStringLiteral("hz"), qint64(std::llround(hz))}});
}

void RemoteEngineBackend::setPanBandwidth(const QString& panId, double hz)
{
    if (!std::isfinite(hz) || hz < 1.0) { return; }
    submit(QStringLiteral("panadapter"), panId, QStringLiteral("panadapter.setBandwidth"),
           {{QStringLiteral("hz"), qint64(std::llround(hz))}});
}

void RemoteEngineBackend::setPanRfGain(const QString& panId, int gainDb)
{
    submit(QStringLiteral("panadapter"), panId, QStringLiteral("panadapter.setRfGain"),
           {{QStringLiteral("gainDb"), gainDb}});
}

void RemoteEngineBackend::setKeying(bool key, const AetherSDR::TxCoordinator::Operation& operation,
                                    const AetherSDR::TxCoordinator::Completion& completion)
{
    Q_UNUSED(operation);
    Q_UNUSED(completion);
    // Receive-only by construction: no transmit capability is declared and no
    // protocol TX method exists for a network session. Reaching here keyed
    // means the TX guard above the seam was bypassed.
    if (key) {
        qCWarning(lcRemoteEngine) << "setKeying(true) on the receive-only remote-engine backend; refused";
    }
}

void RemoteEngineBackend::invokeExtension(const QString& ns, const QString& verb,
                                          quint64 requestId, const QVariant& arg)
{
    Q_UNUSED(arg);
    if (requestId != 0) {
        emit extensionError(requestId, QStringLiteral("aetherd: no extension '%1.%2'").arg(ns, verb));
    }
}

void RemoteEngineBackend::fail(const QString& reason)
{
    if (m_phase == Phase::Idle) { return; }
    emit connectionError(reason);
    finish();
}

void RemoteEngineBackend::finish()
{
    if (m_phase == Phase::Idle) { return; }
    // Rule 2: closing the socket is all a disconnect does. Nothing is sent.
    ++m_generation;
    m_phase = Phase::Idle;
    m_pumpTimer.stop();
    m_scheduler.clear();
    m_pending.clear();
    m_retryIntents.clear();
    m_snapshotReady = false;
    if (m_socket) {
        m_socket->disconnect(this);
        m_socket->abort();
        m_socket.release()->deleteLater();
    }
    const bool wasConnected = m_connected;
    m_connected = false;
    if (wasConnected) { emit disconnected(); }
}

void RemoteEngineBackend::disconnectRadio()
{
    finish();
}

} // namespace AetherSDR::aetherd
