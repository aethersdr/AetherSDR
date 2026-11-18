#include "AetherPDInstallUpload.h"
#include "AetherPDInstallRequestClient.h"
#include "AetherPDProtocol.h"
#include "AetherPDTransportPolicy.h"

#include <QCryptographicHash>
#include <QNetworkProxy>
#include <QTcpSocket>
#include <QThread>
#include <QtEndian>

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

extern "C" {
#include "monocypher.h"
}
#ifdef Q_OS_WIN
#include <winsock2.h>
#else
#include <sys/socket.h>
#endif

namespace AetherSDR {
namespace {
constexpr qint64 kUploadMs = 90'000;
constexpr qint64 kDeliveryMs = 190'000;
constexpr qint64 kChunkBytes = 65536;
constexpr qsizetype kMaxReleaseBytes = 18 * 1024 * 1024;
class SocketChannel final : public AetherPDInstallChannel {
public:
    SocketChannel()
    {
        m_socket.setParent(this);
        m_socket.setProxy(QNetworkProxy::NoProxy);
        m_socket.setReadBufferSize(97);
        connect(&m_socket, &QTcpSocket::connected, this, &AetherPDInstallChannel::connected);
        connect(&m_socket, &QIODevice::readyRead, this, &AetherPDInstallChannel::readable);
        connect(&m_socket, &QIODevice::bytesWritten, this, [this] { emit writable(); });
        connect(&m_socket, &QTcpSocket::disconnected, this, &AetherPDInstallChannel::closed);
        connect(&m_socket, &QTcpSocket::errorOccurred, this, [this] { emit closed(); });
    }
    void connectTo(const QHostAddress& address) override { m_socket.connectToHost(address, 48995); }
    qint64 write(const char* bytes, qint64 size) override { return m_socket.write(bytes, size); }
    qint64 pendingBytes() const override { return m_socket.bytesToWrite(); }
    qint64 availableBytes() const override { return m_socket.bytesAvailable(); }
    QByteArray read(qint64 maximum) override { return m_socket.read(maximum); }
    bool finishWrites() override
    {
        const qintptr descriptor = m_socket.socketDescriptor();
        if (descriptor < 0 || m_socket.bytesToWrite() != 0) { return false; }
#ifdef Q_OS_WIN
        return ::shutdown(static_cast<SOCKET>(descriptor), SD_SEND) == 0;
#else
        return ::shutdown(static_cast<int>(descriptor), SHUT_WR) == 0;
#endif
    }
    void close() override { m_socket.abort(); }
private:
    QTcpSocket m_socket;
};
QByteArray digest(const QByteArray& bytes) { return QCryptographicHash::hash(bytes, QCryptographicHash::Blake2b_256); }
}
AetherPDInstallUpload::AetherPDInstallUpload(QObject* parent)
    : QObject(parent), m_ownedChannel(std::make_unique<SocketChannel>()), m_channel(m_ownedChannel.get())
{
    m_ownedChannel->setParent(this);
    initialize();
}
AetherPDInstallUpload::AetherPDInstallUpload(AetherPDInstallChannel& channel, Clock clock, QObject* parent)
    : QObject(parent), m_channel(&channel), m_now(std::move(clock))
{
    initialize();
}
void AetherPDInstallUpload::initialize()
{
    m_timer.setParent(this);
    m_timer.setInterval(25);
    m_clock.start();
    if (!m_now) { m_now = [this] { return m_clock.elapsed(); }; }
    connect(&m_timer, &QTimer::timeout, this, &AetherPDInstallUpload::poll);
    connect(m_channel, &AetherPDInstallChannel::connected, this, [this] {
        if (m_state == State::Connecting && withinDeadline()) { m_state = State::Authenticating; receive(); }
    });
    connect(m_channel, &AetherPDInstallChannel::readable, this, &AetherPDInstallUpload::receive);
    connect(m_channel, &AetherPDInstallChannel::writable, this, &AetherPDInstallUpload::pump);
    connect(m_channel, &AetherPDInstallChannel::closed, this, [this] {
        const QPointer<AetherPDInstallUpload> guard(this);
        receive();
        if (guard && active()) { end(State::Unconfirmed, Reason::Transport); }
    });
    connect(m_channel, &QObject::destroyed, this, [this] {
        m_channel = nullptr;
        if (active()) { end(State::Unconfirmed, Reason::TransportDestroyed); }
    });
}
AetherPDInstallUpload::~AetherPDInstallUpload()
{
    m_timer.stop();
    if (m_channel) { disconnect(m_channel, nullptr, this, nullptr); m_channel->close(); }
    crypto_wipe(m_key.data(), m_key.size());
}
bool AetherPDInstallUpload::active() const
{
    return m_state != State::Idle && m_state != State::Finished && m_state != State::Unconfirmed && m_state != State::Canceled;
}
std::optional<qint64> AetherPDInstallUpload::now()
{
    const qint64 stamp = m_now();
    if (stamp < 0 || stamp < m_lastNow || stamp > (std::numeric_limits<qint64>::max)() - kUploadMs - kDeliveryMs) { return std::nullopt; }
    m_lastNow = stamp;
    return stamp;
}
bool AetherPDInstallUpload::withinDeadline()
{
    const std::optional<qint64> stamp = now();
    if (!stamp) { end(State::Unconfirmed, Reason::InvalidClock); return false; }
    if (*stamp >= m_deadline) { end(State::Unconfirmed, Reason::Deadline); return false; }
    if (!m_channel || m_channel->thread() != thread()) { end(State::Unconfirmed, Reason::TransportDestroyed); return false; }
    return true;
}
bool AetherPDInstallUpload::begin(const QHostAddress& address, const QByteArray& config, const Inputs& inputs)
{
    if (QThread::currentThread() != thread() || m_state != State::Idle || !m_channel
        || m_channel->thread() != thread() || !aetherpd::lanAddress(address)
        || inputs[0].size() <= 184 || inputs[0].size() > 184 + 512 * 1024
        || inputs[1].size() != 352 || inputs[2].size() < 432 || inputs[2].size() > kMaxReleaseBytes
        || inputs[3].size() < 432 || inputs[3].size() > kMaxReleaseBytes
        || !AetherPDInstallRequestClient::publicRecord(config, inputs[1])) { return false; }
    const std::optional<aetherpd::InstallationPlan> plan = aetherpd::parseInstallationPlan(inputs[1]);
    if (!plan) { return false; }
    std::array<QByteArray, 4> hashes;
    for (std::size_t i = 0; i < inputs.size(); ++i) { hashes[i] = digest(inputs[i]); }
    if (hashes[0] != config.mid(56, 32) || hashes[1] != config.mid(88, 32)
        || hashes[2].toHex() != plan->candidateDigest.toLatin1()
        || hashes[3].toHex() != plan->previousDigest.toLatin1()) { return false; }
    const std::optional<qint64> stamp = now();
    if (!stamp) { return false; }
    m_header = QByteArray("PDUPLD01");
    m_header.resize(24);
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        qToBigEndian<quint32>(static_cast<quint32>(inputs[i].size()), m_header.data() + 8 + 4 * i);
        m_header += hashes[i];
        m_output[i + 1] = inputs[i];
    }
    std::memcpy(m_key.data(), config.constData() + 8, m_key.size());
    m_receipt = config.mid(40);
    m_deadline = *stamp + kUploadMs;
    m_state = State::Connecting;
    m_timer.start();
    m_channel->connectTo(address);
    return true;
}
std::array<uint8_t, 32> AetherPDInstallUpload::mac(const char* domain, const QByteArray& extra) const
{
    std::array<uint8_t, 32> result{};
    crypto_blake2b_ctx hash;
    crypto_blake2b_keyed_init(&hash, result.size(), m_key.data(), m_key.size());
    crypto_blake2b_update(&hash, reinterpret_cast<const uint8_t*>(domain), 8);
    crypto_blake2b_update(&hash, reinterpret_cast<const uint8_t*>(m_challenge.constData()), m_challenge.size());
    crypto_blake2b_update(&hash, reinterpret_cast<const uint8_t*>(m_receipt.constData()), m_receipt.size());
    crypto_blake2b_update(&hash, reinterpret_cast<const uint8_t*>(extra.constData()), extra.size());
    crypto_blake2b_final(&hash, result.data());
    return result;
}
bool AetherPDInstallUpload::authenticate(const QByteArray& bytes, qsizetype offset, const char* domain, const QByteArray& extra) const
{
    const std::array<uint8_t, 32> expected = mac(domain, extra);
    return bytes.size() >= offset + 32 && crypto_verify32(expected.data(), reinterpret_cast<const uint8_t*>(bytes.constData() + offset)) == 0;
}
void AetherPDInstallUpload::receive()
{
    if (QThread::currentThread() != thread() || !active() || !withinDeadline()) { return; }
    if (m_channel->availableBytes() < 0 || m_channel->availableBytes() > 96) { end(State::Unconfirmed, Reason::InvalidResponse); return; }
    m_input += m_channel->read(97 - m_input.size());
    if (m_input.size() > 96) { end(State::Unconfirmed, Reason::InvalidResponse); return; }
    if (m_state == State::Connecting || m_state == State::Sending) {
        if (!m_input.isEmpty()) { end(State::Unconfirmed, Reason::InvalidResponse); }
        return;
    }
    if (m_state == State::Authenticating) {
        if (m_input.size() < 72) { return; }
        if (m_input.size() != 72 || !m_input.startsWith("PDUPCH01")) { end(State::Unconfirmed, Reason::InvalidResponse); return; }
        m_challenge = m_input.mid(8, 32);
        if (!authenticate(m_input, 40, "PDUSRV01")) { end(State::Unconfirmed, Reason::Authentication); return; }
        m_input.clear();
        const std::array<uint8_t, 32> signature = mac("PDUCLI01", m_header);
        m_output[0] = m_header + QByteArray(reinterpret_cast<const char*>(signature.data()), signature.size());
        m_state = State::Sending;
        pump();
        return;
    }
    while (m_input.size() >= 48) {
        const QByteArray frame = m_input.first(48);
        m_input.remove(0, 48);
        const quint32 stage = qFromBigEndian<quint32>(frame.constData() + 8);
        const quint32 result = qFromBigEndian<quint32>(frame.constData() + 12);
        const bool first = m_state == State::AwaitingVerification;
        if (!frame.startsWith("PDUPACK1") || stage != (first ? 1U : 2U)
            || (first ? result != 0 : (result != 0 && result != 3 && result != 4 && result != 5 && result != 6))) {
            end(State::Unconfirmed, Reason::InvalidResponse); return;
        }
        if (!authenticate(frame, 16, "PDUACK01", frame.first(16))) { end(State::Unconfirmed, Reason::Authentication); return; }
        if (!first) {
            if (!m_input.isEmpty()) { end(State::Unconfirmed, Reason::InvalidResponse); return; }
            end(State::Finished, Reason::None, static_cast<int>(result)); return;
        }
        m_state = State::AwaitingDelivery;
        m_deadline = m_lastNow + kDeliveryMs;
        const QPointer<AetherPDInstallUpload> guard(this);
        emit inputsVerified();
        if (!guard || !active()) { return; }
    }
}
void AetherPDInstallUpload::pump()
{
    if (QThread::currentThread() != thread() || m_state != State::Sending || m_pumping || !withinDeadline()) { return; }
    m_pumping = true;
    const qint64 queued = m_channel->pendingBytes();
    if (queued < 0 || queued > kChunkBytes) { m_pumping = false; end(State::Unconfirmed, Reason::Transport); return; }
    while (m_part < m_output.size() && m_offset == m_output[m_part].size()) { m_output[m_part].clear(); ++m_part; m_offset = 0; }
    if (m_part == m_output.size()) {
        m_pumping = false;
        if (queued != 0) { return; }
        m_state = State::AwaitingVerification;
        const QPointer<AetherPDInstallUpload> guard(this);
        const bool closed = m_channel->finishWrites();
        if (guard && active() && !closed) { end(State::Unconfirmed, Reason::Transport); }
        return;
    }
    const qint64 size = (std::min)(kChunkBytes - queued, static_cast<qint64>(m_output[m_part].size() - m_offset));
    if (size == 0) { m_pumping = false; return; }
    const QPointer<AetherPDInstallUpload> guard(this);
    const QByteArray part = m_output[m_part];
    const qint64 written = m_channel->write(part.constData() + m_offset, size);
    if (!guard) { return; }
    m_pumping = false;
    if (!active()) { return; }
    if (written < 0 || written > size) { end(State::Unconfirmed, Reason::Transport); return; }
    m_offset += static_cast<qsizetype>(written);
}
void AetherPDInstallUpload::poll()
{
    if (QThread::currentThread() != thread() || !active() || !withinDeadline()) { return; }
    if (m_state == State::Sending) { pump(); }
}
void AetherPDInstallUpload::cancel()
{
    if (QThread::currentThread() == thread() && active()) { end(State::Canceled, Reason::None); }
}
void AetherPDInstallUpload::end(State state, Reason reason, int result)
{
    m_state = state;
    m_timer.stop();
    crypto_wipe(m_key.data(), m_key.size());
    m_output = {};
    m_input.clear();
    const QPointer<AetherPDInstallUpload> guard(this);
    if (m_channel) { m_channel->close(); }
    if (guard) { emit finished(state, reason, result); }
}
} // namespace AetherSDR
