#ifdef HAVE_WEBSOCKETS
#include "TciIoWorker.h"
#include "TciAudioHeader.h"
#include "TciProtocol.h"
#include "Resampler.h"
#include "LogManager.h"
#include <QJsonDocument>
#include <QMutexLocker>
#include <QScopeGuard>
#include <QThread>
#include <QWebSocket>
#include <QWebSocketServer>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace AetherSDR {
namespace {
constexpr int kTxChronoSamples = 2048;
constexpr int kTxChronoStereoFrames = kTxChronoSamples / 2;
constexpr qint64 kTxChronoPeriodNs = qint64(kTxChronoStereoFrames) * 1000000000LL / 48000;
constexpr int kTxChronoPollMs = 5;
constexpr qint64 kTxSummaryEveryBlocks = 48;
constexpr qsizetype kMailboxBytes = 4 * 1024 * 1024;
constexpr qsizetype kMailboxItems = 256;
constexpr qsizetype kTextBytes = 64 * 1024;
}

TciIoWorker::TciIoWorker(QObject* parent) : QObject(parent), m_txChronoTimer(this)
{
    m_rxSend = [this](quint64 id, const QByteArray& data) { return sendBinary(id, data); };
    m_rxBacklog = [this](quint64 id) {
        const auto client = m_clients.constFind(id);
        return client != m_clients.cend() && client->socket ? client->socket->bytesToWrite() : -1;
    };
    m_rxClose = [this](quint64 id, QWebSocketProtocol::CloseCode code, const QString& reason) {
        closeClient(id, code, reason);
    };
    m_txChronoTimer.setTimerType(Qt::PreciseTimer);
    m_txChronoTimer.setInterval(kTxChronoPollMs);
    connect(&m_txChronoTimer, &QTimer::timeout, this, [this] {
        if (!m_tciTxContext.permitsDispatch(TxCoordinator::monotonicMs())) {
            stopChrono();
            return;
        }
        const qint64 gap = m_txChronoClock.nsecsElapsed();
        m_txChronoClock.restart();
        m_txChronoAccumNs += gap;
        int sent = 0;
        while (m_txChronoAccumNs >= kTxChronoPeriodNs && m_txClient) {
            sendTxChronoFrame(m_txClient);
            m_txChronoAccumNs -= kTxChronoPeriodNs;
            ++sent;
        }
        noteTxChronoPoll(gap, sent);
        // Coalesce diagnostics: one snapshot per second, not one GUI event
        // per 5 ms tick. The controller never queries this object synchronously.
        if ((m_txChronoPollCount % 200) == 0) { publishTelemetry(); }
    });
    publishTelemetry();
}

void TciIoWorker::publishRxLevel(int channel, float rms)
{
    if (channel < 1 || channel > 8 || m_rxLevelPending[channel - 1]) { return; }
    m_rxLevelPending[channel - 1] = true;
    emit rxLevel(channel, rms);
}

void TciIoWorker::publishTxLevel(float rms)
{
    if (m_txLevelPending) { return; }
    m_txLevelPending = true;
    emit txLevel(rms);
}

void TciIoWorker::acknowledgeLevel(int channel)
{
    if (channel == 0) { m_txLevelPending = false; }
    else if (channel >= 1 && channel <= 8) { m_rxLevelPending[channel - 1] = false; }
}

void TciIoWorker::publishTelemetry()
{
    const QJsonObject snapshot = chronoSnapshot();
    QMutexLocker lock(&m_diagnosticsMutex);
    m_diagnostics = snapshot;
}

QJsonObject TciIoWorker::cachedChronoSnapshot() const
{
    QMutexLocker lock(&m_diagnosticsMutex);
    return m_diagnostics;
}

TciIoWorker::~TciIoWorker() { stop(); }

bool TciIoWorker::start(quint16 requestedPort)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_server) { return m_server->isListening(); }
    { QMutexLocker lock(&m_mailboxMutex); m_accepting = true; }
    m_server = new QWebSocketServer(QStringLiteral("AetherSDR-TCI"), QWebSocketServer::NonSecureMode, this);
    if (!m_server->listen(QHostAddress::Any, requestedPort)) {
        qCWarning(lcCat) << "TciServer: listen failed" << m_server->errorString();
        delete m_server;
        m_server = nullptr;
        return false;
    }
    connect(m_server, &QWebSocketServer::newConnection, this, &TciIoWorker::acceptConnections);
    return true;
}

quint16 TciIoWorker::port() const { return m_server ? m_server->serverPort() : 0; }

void TciIoWorker::stop()
{
    Q_ASSERT(QThread::currentThread() == thread());
    {
        QMutexLocker lock(&m_mailboxMutex);
        m_accepting = false;
        m_mailbox.clear();
        m_mailboxBytes = 0;
        m_overloaded = false;
    }
    stopChrono();
    for (const Client& client : std::as_const(m_clients)) {
        client.lifetime->live.store(false, std::memory_order_release);
        client.producer.invalidate();
    }
    // A graceful close may await a peer forever. Abort and destroy every
    // transport child here, before moving the idle engine back to its owner.
    const QList<QWebSocket*> sockets = findChildren<QWebSocket*>(QString(), Qt::FindDirectChildrenOnly);
    for (QWebSocket* socket : sockets) {
        socket->disconnect(this);
        socket->abort();
        delete socket;
    }
    m_clients.clear();
    m_rxClients.clear();
    if (m_server) {
        m_server->close();
        delete m_server;
        m_server = nullptr;
    }
}

bool TciIoWorker::post(std::function<void()> work, qsizetype bytes)
{
    // Before start(), socket-free tests use the same engine in its owner
    // context. Production ingress always enters the bounded mailbox.
    if (QThread::currentThread() == thread()) { work(); return true; }
    QMutexLocker lock(&m_mailboxMutex);
    if (!m_accepting) { return false; }
    const bool fits = bytes >= 0 && bytes <= kMailboxBytes - m_mailboxBytes
        && m_mailbox.size() < kMailboxItems;
    if (fits) {
        m_mailbox.push_back({std::move(work), bytes});
        m_mailboxBytes += bytes;
    } else {
        m_overloaded = true;
    }
    if (!m_drainScheduled) {
        m_drainScheduled = true;
        QMetaObject::invokeMethod(this, &TciIoWorker::drain, Qt::QueuedConnection);
    }
    return fits;
}

void TciIoWorker::drain()
{
    for (int i = 0; i < 32; ++i) {
        Work next;
        bool overloaded = false;
        {
            QMutexLocker lock(&m_mailboxMutex);
            overloaded = std::exchange(m_overloaded, false);
            if (overloaded) { m_mailbox.clear(); m_mailboxBytes = 0; }
            if (m_mailbox.empty()) {
                m_drainScheduled = false;
            } else {
                next = std::move(m_mailbox.front());
                m_mailbox.pop_front();
                m_mailboxBytes -= next.bytes;
            }
        }
        if (overloaded) {
            qCWarning(lcCat) << "TCI: worker mailbox limit; retiring clients";
            const QList<quint64> ids = m_clients.keys();
            for (quint64 id : ids) {
                closeClient(id, QWebSocketProtocol::CloseCodeTooMuchData, QStringLiteral("TCI work backlog"));
            }
        }
        if (!next.run) { return; }
        next.run();
    }
    QMetaObject::invokeMethod(this, &TciIoWorker::drain, Qt::QueuedConnection);
}

void TciIoWorker::acceptConnections()
{
    while (m_server && m_server->hasPendingConnections()) {
        QWebSocket* socket = m_server->nextPendingConnection();
        socket->setParent(this);
        if (m_clients.size() >= 8) {
            socket->close(QWebSocketProtocol::CloseCodeTooMuchData, QStringLiteral("server at max-clients cap"));
            socket->deleteLater();
            continue;
        }
        socket->setMaxAllowedIncomingMessageSize(64 * 1024);
        socket->setMaxAllowedIncomingFrameSize(64 * 1024);
        Client client;
        client.socket = socket;
        client.lifetime = std::make_shared<TciClientLifetime>();
        const quint64 id = client.lifetime->id;
        m_clients.insert(id, client);
        connect(socket, &QWebSocket::textMessageReceived, this, [this, id](const QString& text) {
            auto it = m_clients.find(id);
            if (it == m_clients.end() || !it->lifetime->live.load()) { return; }
            const qsizetype bytes = text.size() * sizeof(QChar);
            if (bytes > kTextBytes - it->textBytes || it->texts.size() >= 64) {
                closeClient(id, QWebSocketProtocol::CloseCodeTooMuchData, QStringLiteral("TCI command backlog"));
                return;
            }
            const QStringList commands = text.split(QLatin1Char(';'), Qt::SkipEmptyParts);
            if (it->texts.size() + commands.size() > 64) {
                closeClient(id, QWebSocketProtocol::CloseCodeTooMuchData, QStringLiteral("TCI command backlog"));
                return;
            }
            for (const QString& command : commands) { it->texts.push_back({command + QLatin1Char(';'), {}}); }
            if (it->activated) { captureTextInputs(*it); }
            it->textBytes += bytes;
            dispatchText(id);
        });
        connect(socket, &QWebSocket::binaryMessageReceived, this, [this, id](const QByteArray& data) {
            receiveBinary(id, data);
        });
        connect(socket, &QWebSocket::errorOccurred, this, [this, id, socket](QAbstractSocket::SocketError error) {
            auto it = m_clients.find(id);
            if (it != m_clients.end()) { it->error = int(error); it->errorText = socket->errorString(); }
        });
        connect(socket, &QWebSocket::disconnected, this, [this, id, socket] {
            auto it = m_clients.find(id);
            if (it == m_clients.end()) { return; }
            it->lifetime->live.store(false, std::memory_order_release);
            it->producer.invalidate();
            const int error = it->error;
            const QString errorText = it->errorText;
            if (m_txClient == id) { stopChrono(); }
            m_rxClients.remove(id);
            // Keep the retired entry against the eight-client cap until the
            // controller acknowledges it. Rapid connect/close churn cannot
            // enqueue an unbounded number of GUI lifecycle notifications.
            emit clientClosed(id, int(socket->closeCode()), error, errorText);
            socket->deleteLater();
        });
        emit clientOpened(client.lifetime, socket->peerAddress(), socket->peerPort());
    }
}

void TciIoWorker::activate(quint64 id, const TxCoordinator::Producer& producer)
{
    auto it = m_clients.find(id);
    if (it == m_clients.end() || !it->lifetime->live.load()) { producer.invalidate(); return; }
    it->producer = producer;
    it->activated = true;
    captureTextInputs(*it);
    dispatchText(id);
}

void TciIoWorker::captureTextInputs(Client& client)
{
    for (Client::Text& text : client.texts) {
        if (text.captured) { continue; }
        text.captured = true;
        const QString command = text.message.trimmed();
        const int colon = command.indexOf(QLatin1Char(':'));
        if (command.left(colon).trimmed().compare(QLatin1String("trx"), Qt::CaseInsensitive) == 0) {
            QString arguments = command.mid(colon + 1);
            if (arguments.endsWith(QLatin1Char(';'))) { arguments.chop(1); }
            const auto request = TciProtocol::parseTrxRequest(arguments.split(QLatin1Char(',')));
            if (request && !request->transmitting) {
                // An explicit release revokes queued inputs immediately, even
                // while the model owner is stalled. This grants no authority.
                client.producer.discardInputs();
                if (m_txClient == client.lifetime->id) { stopChrono(); }
            } else if (request && request->transmitting) {
                text.input = client.producer.request();
            }
        }
    }
}

void TciIoWorker::dispatchText(quint64 id)
{
    auto it = m_clients.find(id);
    if (it == m_clients.end() || !it->lifetime->live.load() || !it->activated || it->textInFlight || it->texts.empty()) { return; }
    const Client::Text text = std::move(it->texts.front());
    it->texts.pop_front();
    it->textInFlight = true;
    // Includes the in-flight message until acknowledgement; controller backlog
    // remains bounded while its event loop is busy.
    emit textReceived(id, text.message, text.input);
}

void TciIoWorker::acknowledgeText(quint64 id)
{
    auto it = m_clients.find(id);
    if (it == m_clients.end()) { return; }
    it->textInFlight = false;
    it->textBytes = 0;
    for (const Client::Text& text : it->texts) { it->textBytes += text.message.size() * sizeof(QChar); }
    dispatchText(id);
}

void TciIoWorker::sendText(quint64 id, const QString& message)
{
    const auto it = m_clients.constFind(id);
    if (it == m_clients.cend() || !it->socket || !it->lifetime->live.load()) { return; }
    if (it->socket->bytesToWrite() > kMaxRxBacklogBytes - message.size() * 4) {
        closeClient(id, QWebSocketProtocol::CloseCodeTooMuchData, QStringLiteral("TCI output backlog"));
        return;
    }
    it->socket->sendTextMessage(message);
}

qint64 TciIoWorker::sendBinary(quint64 id, const QByteArray& message)
{
    const auto it = m_clients.constFind(id);
    if (it == m_clients.cend() || !it->socket || !it->lifetime->live.load()) { return -1; }
    if (it->socket->bytesToWrite() > kMaxRxBacklogBytes - message.size()) {
        closeClient(id, QWebSocketProtocol::CloseCodeTooMuchData, QStringLiteral("TCI output backlog"));
        return -1;
    }
    return it->socket->sendBinaryMessage(message);
}

void TciIoWorker::closeClient(quint64 id, QWebSocketProtocol::CloseCode code, const QString& reason)
{
    const auto it = m_clients.find(id);
    if (it == m_clients.end()) { return; }
    it->lifetime->live.store(false, std::memory_order_release);
    it->producer.invalidate();
    if (m_txClient == id) { stopChrono(); }
    if (it->socket) { it->socket->close(code, reason); }
}

void TciIoWorker::removeClient(quint64 id)
{
    if (m_txClient == id) { stopChrono(); }
    m_rxClients.remove(id);
    m_clients.remove(id);
}

void TciIoWorker::configureClient(const TciStreamConfig& config)
{
    if (!config.lifetime || !config.lifetime->live.load()) { return; }
    RxClient& client = m_rxClients[config.lifetime->id];
    const TciStreamConfig& old = client.config;
    if (old.generation != config.generation || old.audio != config.audio || old.receiver != config.receiver
        || old.rate != config.rate || old.channels != config.channels || old.format != config.format) {
        client.converters.clear();
    }
    client.config = config;
}

void TciIoWorker::setRxGain(int channel, float gain)
{
    if (channel >= 1 && channel <= 8) { m_rxChannelGain[channel-1] = gain; }
}

void TciIoWorker::setTxGain(float gain, int mode)
{
    m_txGain = gain;
    m_overflowMode = static_cast<OverflowMode>(std::clamp(mode, 0, 2));
}

void TciIoWorker::setRxBindings(const QHash<quint64, TciRxBinding>& bindings)
{
    for (auto it = m_bindings.cbegin(); it != m_bindings.cend(); ++it) {
        if (!bindings.contains(it.key()) || bindings.value(it.key()).alive != it->alive) {
            resetRxRoute(it.key());
            auto route = m_rxRoutes.find(it.key());
            if (route != m_rxRoutes.end() && (it.key() >> 32) == 0 && !bindings.contains(it.key())) { route->retired = true; }
        }
    }
    m_bindings = bindings;
}

void TciIoWorker::retireRxRoute(quint64 key)
{
    resetRxRoute(key);
    auto route = m_rxRoutes.find(key);
    if (route != m_rxRoutes.end()) { route->retired = true; }
}

void TciIoWorker::resetRxRoute(quint64 key)
{
    for (RxClient& client : m_rxClients) { client.converters.remove(key); }
}

bool TciIoWorker::rxDeliveryCurrent(quint64 id, quint64 generation, quint64 key,
    const PcmFrame& input, const std::shared_ptr<TciRxConverter>& converter) const
{
    const auto binding = m_bindings.constFind(key);
    const auto route = m_rxRoutes.constFind(key);
    const auto client = m_rxClients.constFind(id);
    return input.current() && binding != m_bindings.cend() && binding->current()
        && route != m_rxRoutes.cend() && !route->retired && route->pin
        && route->pin->stream() == input.stream() && route->owner == binding->alive
        && client != m_rxClients.cend() && client->config.lifetime->live.load()
        && client->config.audio && client->config.generation == generation
        && (client->config.receiver < 0 || client->config.receiver == binding->trx)
        && client->converters.value(key) == converter;
}

void TciIoWorker::receivePcm(quint64 key, const PcmFrame& frame)
{
    const PcmFrame input = frame;
    if (m_rxProcessing || !input.current()) { return; }
    const auto found = m_bindings.constFind(key);
    if (found == m_bindings.cend() || !found->current()) { return; }
    const TciRxBinding binding = *found;
    const bool dax = (key >> 32) != 0;
    if (dax ? (input.stream().purpose != PcmPurpose::Auxiliary || input.stream().format != PcmFormat{})
            : (input.stream().purpose != PcmPurpose::Slice || input.stream().sliceId != binding.sliceId)) { return; }
    m_rxProcessing = true;
    QPointer<TciIoWorker> self(this);
    const auto guard = qScopeGuard([self] { if (self) { self->m_rxProcessing = false; } });
    for (auto it = m_rxRoutes.begin(); it != m_rxRoutes.end();) {
        if (it->retired && (!it->pin || !it->pin->current())) { it = m_rxRoutes.erase(it); }
        else { ++it; }
    }
    auto route = m_rxRoutes.find(key);
    if (route != m_rxRoutes.end()) {
        const bool live = route->pin && route->pin->current();
        if (live && (route->pin->stream() != input.stream() || route->retired)) { return; }
    } else if (m_rxRoutes.size() >= qsizetype(PcmFrameGate::kMaxStreams)) { return; }
    if (!m_rxGate.accept(input)) { return; }
    if (route == m_rxRoutes.end()) { route = m_rxRoutes.insert(key, RxRoute{}); }
    if (!route->pin || route->pin->stream() != input.stream() || route->owner != binding.alive
        || route->nextSample != input.firstSample() || input.discontinuity()) { resetRxRoute(key); }
    route->pin = input;
    route->owner = binding.alive;
    route->nextSample = input.firstSample() + input.frameCount();
    route->retired = false;
    const float gain = binding.gainChannel >= 1 && binding.gainChannel <= 8
        ? m_rxChannelGain[binding.gainChannel-1] : 1;
    QList<std::pair<quint64, quint64>> recipients;
    for (auto it = m_rxClients.cbegin(); it != m_rxClients.cend(); ++it) {
        if (it->config.audio && (it->config.receiver < 0 || it->config.receiver == binding.trx)) {
            recipients.append({it.key(), it->config.generation});
        }
    }
    if (recipients.isEmpty()) { return; }
    double sum = 0;
    for (float sample : input.samples()) { sum += double(sample) * sample; }
    publishRxLevel(binding.gainChannel, float(std::sqrt(sum / input.samples().size()) * gain));
    if (!self || !input.current() || !binding.current()) { return; }
    for (const auto& [id, generation] : recipients) {
        auto client = m_rxClients.find(id);
        if (client == m_rxClients.end() || client->config.generation != generation) { continue; }
        const TciStreamConfig config = client->config;
        std::shared_ptr<TciRxConverter> converter = client->converters.value(key);
        if (!converter) {
            converter = std::make_shared<TciRxConverter>(input.stream().format, config.rate);
            if (!converter->valid()) { continue; }
            client->converters.insert(key, converter);
        }
        const auto send = m_rxSend;
        const auto backlog = m_rxBacklog;
        const bool processed = converter->process(std::span<const float>(input.samples().constData(), input.samples().size()),
            [self, id, generation, key, input, converter, config, binding, gain, send, backlog](QVector<float> stereo) {
                if (!self || !self->rxDeliveryCurrent(id, generation, key, input, converter)) { return false; }
                const QByteArray packet = encodeRxAudio(binding.trx, config.rate, config.channels, config.format, stereo, gain);
                const qint64 pending = backlog(id);
                if (!self || !self->rxDeliveryCurrent(id, generation, key, input, converter)) { return false; }
                if (pending < 0 || pending > kMaxRxBacklogBytes - packet.size()) {
                    self->m_rxClients[id].config.audio = false;
                    self->m_rxClients[id].converters.clear();
                    emit self->audioStopped(id, generation);
                    if (!self) { return false; }
                    qCWarning(lcCat) << "TCI: RX audio stopped:" << (pending < 0 ? "invalid backlog" : "backlog limit")
                                    << "client=" << id << "pending_bytes=" << pending << "packet_bytes=" << packet.size();
                    self->m_rxClose(id, QWebSocketProtocol::CloseCodeTooMuchData, QStringLiteral("RX audio backlog limit"));
                    return false;
                }
                const qint64 sent = send(id, packet);
                if (!self) { return false; }
                if (sent != packet.size()) {
                    auto current = self->m_rxClients.find(id);
                    if (current != self->m_rxClients.end()) { current->config.audio = false; current->converters.clear(); }
                    emit self->audioStopped(id, generation);
                    if (!self) { return false; }
                    qCWarning(lcCat) << "TCI: RX audio stopped:" << (sent < 0 ? "send failed" : "short send")
                                    << "client=" << id << "pending_bytes=" << pending << "packet_bytes=" << packet.size()
                                    << "sent_bytes=" << sent;
                    return false;
                }
                self->m_rxAudioFramesSent += stereo.size()/2;
                return self->rxDeliveryCurrent(id, generation, key, input, converter);
            });
        if (!self) { return; }
        if (!processed) {
            auto current = m_rxClients.find(id);
            if (current != m_rxClients.end() && current->converters.value(key) == converter) { current->converters.remove(key); }
        }
    }
}

void TciIoWorker::prepareTx(quint64 id, const TxCoordinator::Context& context)
{
    if (!context.permitsDispatch(TxCoordinator::monotonicMs())) { return; }
    stopChrono();
    m_txClient = id;
    m_tciTxContext = context;
    m_txResampler = std::make_unique<Resampler>(48000., 24000., 4096);
    m_txChronoRequestedFrames = 0;
    resetTxChronoStallStats();
    m_txAudioBlocks = m_txInputFrames = m_txOutputFrames = m_txClipSamples = m_txAudioSampleCount = 0;
    m_txAudioSumSq = 0;
    m_txAudioPeak = 0;
    m_txSawDuplicatedStereo = false;
}

void TciIoWorker::startChrono(quint64 id, int trx, const TxCoordinator::Context& context)
{
    if (m_txClient != id) { prepareTx(id, context); }
    if (!m_txClient || !context.permitsDispatch(TxCoordinator::monotonicMs()) || m_txChronoTimer.isActive()) { return; }
    m_txChronoTrx = trx;
    m_txChronoAccumNs = 0;
    m_txChronoClock.start();
    m_txChronoSessionClock.start();
    resetTxChronoStallStats();
    m_txChronoTimer.start();
    sendTxChronoFrame(id);
    publishTelemetry();
}

void TciIoWorker::stopChrono()
{
    if (m_txClient) { logTxAudioSummary("stop"); }
    m_txChronoTimer.stop();
    m_txClient = 0;
    m_tciTxContext = {};
    m_txResampler.reset();
    m_txChronoAccumNs = 0;
    publishTelemetry();
}


QByteArray TciIoWorker::encodeRxAudio(int trx, int rate, int channels, int format,
                                   const QVector<float>& stereo, float gain)
{
    const int frames = static_cast<int>(stereo.size() / 2);
    const int scalars = frames * channels;
    const int sampleBytes = format == 3 ? sizeof(float) : sizeof(qint16);
    QByteArray packet(sizeof(TciAudioHeader) + scalars * sampleBytes, Qt::Uninitialized);
    TciAudioHeader header{};
    header.receiver = static_cast<quint32>(trx);
    header.sampleRate = static_cast<quint32>(rate);
    header.format = static_cast<quint32>(format);
    header.length = static_cast<quint32>(scalars);
    header.type = 1;
    header.channels = static_cast<quint32>(channels);
    std::memcpy(packet.data(), &header, sizeof(header));
    for (int i = 0; i < scalars; ++i) {
        // Average in double precision so valid finite peaks do not overflow
        // before int16 saturation or float32 encoding.
        const double value = (channels == 2 ? static_cast<double>(stereo[i])
            : (static_cast<double>(stereo[2*i]) + stereo[2*i+1]) * 0.5) * gain;
        char* destination = packet.data() + sizeof(header) + i * sampleBytes;
        if (format == 3) {
            const float sample = static_cast<float>(value);
            std::memcpy(destination, &sample, sizeof(sample));
        } else {
            const qint16 sample = static_cast<qint16>(std::clamp(value * 32768.0, -32768.0, 32767.0));
            std::memcpy(destination, &sample, sizeof(sample));
        }
    }
    return packet;
}

void TciIoWorker::resetTxChronoStallStats()
{
    m_txChronoPollCount = 0;
    m_txChronoMaxPollGapNs = 0;
    m_txChronoLatePolls = 0;
    m_txChronoCatchUpBursts = 0;
    m_txChronoCatchUpFrames = 0;
    m_txChronoMaxCatchUp = 0;
}

void TciIoWorker::noteTxChronoPoll(qint64 gapNs, int framesSent)
{
    ++m_txChronoPollCount;
    if (gapNs > m_txChronoMaxPollGapNs) {
        m_txChronoMaxPollGapNs = gapNs;
    }
    // Two chrono periods (~42.7 ms) is a missed TX_CHRONO slot before catch-up.
    if (gapNs >= 2 * kTxChronoPeriodNs) {
        ++m_txChronoLatePolls;
    }
    if (framesSent > m_txChronoMaxCatchUp) {
        m_txChronoMaxCatchUp = framesSent;
    }
    if (framesSent > 1) {
        ++m_txChronoCatchUpBursts;
        m_txChronoCatchUpFrames += (framesSent - 1);
    }
}

QJsonObject TciIoWorker::chronoSnapshot() const
{
    return QJsonObject{
        {QStringLiteral("active"), m_txChronoTimer.isActive()},
        {QStringLiteral("polls"), static_cast<qint64>(m_txChronoPollCount)},
        {QStringLiteral("maxGapMs"),
            static_cast<double>(m_txChronoMaxPollGapNs) / 1.0e6},
        {QStringLiteral("latePolls"), static_cast<qint64>(m_txChronoLatePolls)},
        {QStringLiteral("catchUpBursts"),
            static_cast<qint64>(m_txChronoCatchUpBursts)},
        {QStringLiteral("catchUpFrames"),
            static_cast<qint64>(m_txChronoCatchUpFrames)},
        {QStringLiteral("maxCatchUp"), m_txChronoMaxCatchUp},
        {QStringLiteral("periodMs"),
            static_cast<double>(kTxChronoPeriodNs) / 1.0e6},
        {QStringLiteral("pollMs"), kTxChronoPollMs},
    };
}

void TciIoWorker::logTxAudioSummary(const char* reason)
{
    if (m_txChronoRequestedFrames <= 0 && m_txAudioBlocks <= 0)
        return;

    const double elapsedSec = m_txChronoSessionClock.isValid()
        ? static_cast<double>(m_txChronoSessionClock.nsecsElapsed()) / 1.0e9
        : 0.0;
    const double effectiveRate48k = elapsedSec > 0.0
        ? static_cast<double>(m_txChronoRequestedFrames) / elapsedSec
        : 0.0;
    const double rms = m_txAudioSampleCount > 0
        ? std::sqrt(m_txAudioSumSq / static_cast<double>(m_txAudioSampleCount))
        : 0.0;

    qCInfo(lcCat).nospace()
        << "TCI TX summary reason=" << reason
        << " trx=" << m_txChronoTrx
        << " route=" << "radio-dax"
        << " gain=" << m_txGain
        << " blocks=" << m_txAudioBlocks
        << " requested48k=" << m_txChronoRequestedFrames
        << " inputFramesSrc=" << m_txInputFrames
        << " output24k=" << m_txOutputFrames
        << " effective48k=" << effectiveRate48k
        << " peak=" << m_txAudioPeak
        << " rms=" << rms
        << " clips=" << m_txClipSamples
        << " layout=" << (m_txSawDuplicatedStereo ? "duplicated-stereo" : "mono-or-stereo");

    // Cadence evenness — diagnostic only. Catch-up keeps requested48k honest
    // across a stall, so this line is what answers "did the TCI thread miss
    // slots during a window drag?". One line per summary, never per poll.
    qCDebug(lcCat).nospace()
        << "TCI TX chrono stall reason=" << reason
        << " polls=" << m_txChronoPollCount
        << " maxGapMs=" << (static_cast<double>(m_txChronoMaxPollGapNs) / 1.0e6)
        << " latePolls=" << m_txChronoLatePolls
        << " catchUpBursts=" << m_txChronoCatchUpBursts
        << " catchUpFrames=" << m_txChronoCatchUpFrames
        << " maxCatchUp=" << m_txChronoMaxCatchUp
        << " periodMs=" << (static_cast<double>(kTxChronoPeriodNs) / 1.0e6);
}

void TciIoWorker::sendTxChronoFrame(quint64 client)
{
    if (!client) return;

    // TX_CHRONO: header-only, no payload (matches Thetis).
    QByteArray frame(sizeof(TciAudioHeader), '\0');
    TciAudioHeader hdr{};
    hdr.receiver   = static_cast<quint32>(m_txChronoTrx);
    hdr.sampleRate = 48000;
    hdr.format     = 3;                // float32
    hdr.length     = kTxChronoSamples; // matches audio_stream_samples
    hdr.type       = 3;                // TX_CHRONO
    hdr.channels   = 2;
    std::memcpy(frame.data(), &hdr, sizeof(hdr));
    sendBinary(client, frame);
    m_txChronoRequestedFrames += kTxChronoStereoFrames;
}

void TciIoWorker::receiveBinary(quint64 id, const QByteArray& data)
{
    const TxCoordinator::Context context = m_tciTxContext;
    if (id != m_txClient || !context.permitsDispatch(TxCoordinator::monotonicMs())) { return; }
    if (data.size() < static_cast<int>(sizeof(TciAudioHeader))) return;

    // Parse header
    TciAudioHeader hdr;
    std::memcpy(&hdr, data.constData(), sizeof(hdr));

    // Only accept TX_AUDIO_STREAM (type 2)
    if (hdr.type != 2) return;

    const int payloadBytes = data.size() - static_cast<int>(sizeof(TciAudioHeader));
    if (payloadBytes <= 0) return;

    const char* payload = data.constData() + sizeof(TciAudioHeader);

    // ── Convert TX audio to float32 stereo ─────────────────────────────────
    // WSJT-X channels field is garbage (FIFO reuse). readAudioData() writes
    // hdr.length floats to data[0..length-1]. Take the first hdr.length floats.
    QByteArray pcm;

    if (hdr.format == 3) {
        int validFloats = static_cast<int>(hdr.length);
        int availFloats = payloadBytes / static_cast<int>(sizeof(float));
        if (validFloats > availFloats) validFloats = availFloats;
        if (validFloats <= 0) return;

        pcm = QByteArray(payload,
                         validFloats * static_cast<int>(sizeof(float)));
    } else if (hdr.format == 0) {
        int validSamples = static_cast<int>(hdr.length);
        int availSamples = payloadBytes / static_cast<int>(sizeof(qint16));
        if (validSamples > availSamples) validSamples = availSamples;
        if (validSamples <= 0) return;

        auto* src = reinterpret_cast<const qint16*>(payload);
        pcm.resize(validSamples * static_cast<int>(sizeof(float)));
        auto* dst = reinterpret_cast<float*>(pcm.data());
        for (int i = 0; i < validSamples; ++i)
            dst[i] = src[i] / 32768.0f;
    }

    if (pcm.isEmpty()) return;

    int inputFramesSrcRate = 0;   // input frames at the client-declared rate (#3914)
    bool duplicatedStereo = false;

    // ─── TX resampling: client-declared rate → 24kHz (radio native DAX) ──
    // Resample from the rate the client declared in THIS frame (hdr.sampleRate),
    // not a hardcoded 48k. WSJT-X sends 48 kHz — the common path, unchanged — but
    // a client that negotiated 8/12/24 kHz (audio_samplerate) sends at that rate
    // and must be resampled from it, or every tone is mis-pitched and digital
    // decodes fail (#3306). Rebuild the per-session resampler only if the
    // declared rate changes (rare, mid-stream); a 24 kHz client gets a 1:1
    // resampler so the mono/stereo canonicalization below still runs.
    {
        const int declaredRate = static_cast<int>(hdr.sampleRate);
        const int txSrcRate = (declaredRate == 8000 || declaredRate == 12000
                               || declaredRate == 24000 || declaredRate == 48000)
                                  ? declaredRate
                                  : 48000;   // default/garbage -> WSJT-X-compatible 48k
        if (!m_txResampler
            || static_cast<int>(m_txResampler->srcRate()) != txSrcRate) {
            m_txResampler = std::make_unique<Resampler>(
                static_cast<double>(txSrcRate), 24000.0, 4096);
        }
    }

    // Detect mono vs stereo from payload layout.
    //
    // WSJT-X's TCI modulator writes the first `hdr.length` floats as duplicated
    // stereo pairs (L=R), even though the payload buffer it allocates is larger.
    // Treating those `hdr.length` floats as true mono doubles the apparent
    // duration of every block and destroys digital-mode tones.
    if (m_txResampler) {
        int totalFloats = pcm.size() / static_cast<int>(sizeof(float));
        int declaredSamples = static_cast<int>(hdr.length);
        const auto* fSrc = reinterpret_cast<const float*>(pcm.constData());

        if (hdr.format == 3 && totalFloats >= 2 && (totalFloats % 2) == 0) {
            const int pairsToCheck = std::min(totalFloats / 2, 128);
            int duplicatedPairs = 0;
            for (int i = 0; i < pairsToCheck; ++i) {
                if (std::fabs(fSrc[i * 2] - fSrc[i * 2 + 1]) < 1.0e-6f)
                    ++duplicatedPairs;
            }
            duplicatedStereo = duplicatedPairs >= (pairsToCheck * 9) / 10;
        }

        if (duplicatedStereo) {
            // WSJT-X fills `length` floats as stereo pairs in-place.
            int stereoFrames = totalFloats / 2;
            inputFramesSrcRate = stereoFrames;
            pcm = m_txResampler->processStereoToStereo(fSrc, stereoFrames);
        } else if (totalFloats <= declaredSamples) {
            // True mono: upmix to stereo then resample.
            int monoFrames = totalFloats;
            inputFramesSrcRate = monoFrames;
            pcm = m_txResampler->processMonoToStereo(fSrc, monoFrames);
        } else {
            // Explicit stereo: resample directly.
            int stereoFrames = totalFloats / 2;
            inputFramesSrcRate = stereoFrames;
            pcm = m_txResampler->processStereoToStereo(fSrc, stereoFrames);
        }
        if (pcm.isEmpty()) return;
    }

    auto* dst = reinterpret_cast<float*>(pcm.data());
    const int outputStereoFrames = pcm.size() / (2 * static_cast<int>(sizeof(float)));
    const int outputSamples = pcm.size() / static_cast<int>(sizeof(float));
    double sumSq = 0.0;
    float peak = 0.0f;
    qint64 clipSamples = 0;
    // Three overflow regimes selectable via right-click on the TCI TX slider:
    //   Clip     — saturating clamp at ±1.0; cheap defensive limiter,
    //              introduces harmonics on overshoots but protects the
    //              radio float→int16 stage from out-of-range input.
    //   NaNGuard — pass everything except NaN/Inf (which the radio can't
    //              digest); preserves bit-exact tones for well-formed
    //              digital clients, accepts that a malformed >1.0 client
    //              will reach the radio.
    //   Measure  — pure bypass: count overshoots for telemetry but never
    //              touch sample data.  100% client-side passthrough.
    switch (m_overflowMode) {
    case OverflowMode::Clip:
        for (int i = 0; i < outputSamples; ++i) {
            float v = dst[i] * m_txGain;
            if (v > 1.0f) { v = 1.0f; ++clipSamples; }
            else if (v < -1.0f) { v = -1.0f; ++clipSamples; }
            dst[i] = v;
            peak = std::max(peak, std::abs(v));
            sumSq += static_cast<double>(v) * static_cast<double>(v);
        }
        break;
    case OverflowMode::NaNGuard:
        for (int i = 0; i < outputSamples; ++i) {
            float v = dst[i] * m_txGain;
            if (!std::isfinite(v)) { v = 0.0f; ++clipSamples; }
            else if (std::abs(v) > 1.0f) ++clipSamples;
            dst[i] = v;
            peak = std::max(peak, std::abs(v));
            sumSq += static_cast<double>(v) * static_cast<double>(v);
        }
        break;
    case OverflowMode::Measure:
        for (int i = 0; i < outputSamples; ++i) {
            const float v = dst[i] * m_txGain;
            dst[i] = v;
            if (!std::isfinite(v) || std::abs(v) > 1.0f) ++clipSamples;
            const float absV = std::isfinite(v) ? std::abs(v) : 0.0f;
            peak = std::max(peak, absV);
            sumSq += std::isfinite(v)
                       ? static_cast<double>(v) * static_cast<double>(v)
                       : 0.0;
        }
        break;
    }

    ++m_txAudioBlocks;
    m_txInputFrames += inputFramesSrcRate;
    m_txOutputFrames += outputStereoFrames;
    m_txClipSamples += clipSamples;
    m_txAudioSampleCount += outputSamples;
    m_txAudioSumSq += sumSq;
    m_txAudioPeak = std::max(m_txAudioPeak, peak);
    m_txSawDuplicatedStereo = m_txSawDuplicatedStereo || duplicatedStereo;

    if (outputSamples > 0) {
        publishTxLevel(std::sqrt(static_cast<float>(sumSq / outputSamples)));
    }

    if ((m_txAudioBlocks % kTxSummaryEveryBlocks) == 0)
        logTxAudioSummary("running");

    if (context.permitsDispatch(TxCoordinator::monotonicMs())) {
        emit txPcmReady(pcm, context);
    }
}

} // namespace AetherSDR
#endif
