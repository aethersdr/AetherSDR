#include "Ctr2FirmwareUpdater.h"

#include "LogManager.h"

#include <QCryptographicHash>
#include <QRegularExpression>
#include <QTimer>

#include <utility>

namespace AetherSDR {

namespace {

using namespace ctr2hid;

constexpr quint8 kImageMagic = 0xE9;
constexpr quint16 kChipEsp32S3 = 9;
constexpr int kAppDescOffset = 32;       // image header 24 + first segment header 8
constexpr quint32 kAppDescMagic = 0xABCD5432;
constexpr int kVersionOffset = kAppDescOffset + 16;
constexpr int kProjectOffset = kAppDescOffset + 48;

quint32 le32(const QByteArray& b, int at)
{
    const auto u = [&](int i) { return static_cast<quint32>(static_cast<quint8>(b[at + i])); };
    return u(0) | (u(1) << 8) | (u(2) << 16) | (u(3) << 24);
}

QString cString(const QByteArray& b, int at, int size)
{
    const QByteArray field = b.mid(at, size);
    const int end = field.indexOf('\0');
    return QString::fromLatin1(end >= 0 ? field.left(end) : field);
}

const QString kKeepsFirmware = QStringLiteral(" The device keeps the firmware it had.");

} // namespace

bool Ctr2FirmwareUpdater::readImage(const QByteArray& file, Image* image, QString* problem)
{
    if (file.size() < kProjectOffset + 32 || static_cast<quint8>(file[0]) != kImageMagic) {
        *problem = tr("This is not an ESP32 firmware image.");
        return false;
    }
    const quint16 chip = static_cast<quint8>(file[12]) | (static_cast<quint8>(file[13]) << 8);
    if (chip != kChipEsp32S3) {
        *problem = tr("This firmware is not for an ESP32-S3.");
        return false;
    }
    if (le32(file, kAppDescOffset) != kAppDescMagic) {
        // A bootloader or a merged whole-flash image starts like an app
        // image but has no app description where an application has it.
        *problem = tr("This is not an application image. Use the device's application "
                      "file, not a merged flash image or a bootloader.");
        return false;
    }
    if (file.size() > kMaxImageBytes) {
        *problem = tr("This image is larger than the device's update slot.");
        return false;
    }
    image->version = cString(file, kVersionOffset, 32);
    // The device reports its version as three bytes it reads from the front
    // of this string (sscanf "%u.%u.%u"); what it will report is what the
    // update is checked against.
    static const QRegularExpression kTriple(QStringLiteral("^(\\d+)\\.(\\d+)\\.(\\d+)"));
    const QRegularExpressionMatch m = kTriple.match(image->version);
    // Each part 0-255, and a number at all: toUInt() gives 0 on overflow.
    const auto part = [&m](int i) {
        bool ok = false;
        const uint v = m.captured(i).toUInt(&ok);
        return ok && v <= 255 ? int(v) : -1;
    };
    if (!m.hasMatch() || part(1) < 0 || part(2) < 0 || part(3) < 0) {
        *problem = tr("This image's version (%1) is not major.minor.patch, so the device could "
                      "not report it.").arg(image->version);
        return false;
    }
    image->bytes = file;
    image->reportedVersion = QStringLiteral("%1.%2.%3").arg(part(1)).arg(part(2)).arg(part(3));
    image->project = cString(file, kProjectOffset, 32);
    return true;
}

Ctr2FirmwareUpdater::Ctr2FirmwareUpdater(DeviceSource devices, PortOpener opener, QObject* parent)
    : QObject(parent)
    , m_devices(std::move(devices))
    , m_opener(std::move(opener))
    , m_timeout(new QTimer(this))
    , m_retry(new QTimer(this))
{
    m_timeout->setSingleShot(true);
    connect(m_timeout, &QTimer::timeout, this, &Ctr2FirmwareUpdater::onTimeout);
    connect(m_retry, &QTimer::timeout, this, [this] {
        if (m_phase == Phase::Calling || m_phase == Phase::Confirming) {
            sendHello();
        } else if (m_phase == Phase::Returning || m_phase == Phase::Verifying) {
            lookForDevice();
        }
    });
}

Ctr2FirmwareUpdater::~Ctr2FirmwareUpdater()
{
    // AetherSDR quitting mid-update: tell the device, so it drops the
    // partial image and leaves its update screen now.
    closePort(sessionEndReports());
}

std::vector<Report> Ctr2FirmwareUpdater::sessionEndReports()
{
    std::vector<Report> out;
    if (!m_port)
        return out;
    if (m_phase == Phase::Begin || m_phase == Phase::Sending || m_phase == Phase::Ending) {
        m_encoder.encodeExtension(MessageType::Firmware, firmware::encodeAbort(), &out);
        m_encoder.encodeControl(MessageType::Closed, &out);
    } else if (m_phase == Phase::Calling) {
        m_encoder.reset();
        m_encoder.encodeControl(MessageType::Closed, &out);
    } else if (m_phase == Phase::Commanding) {
        m_encoder.encodeControl(MessageType::Closed, &out);
    }
    return out;
}

bool Ctr2FirmwareUpdater::restartDevice(const Ctr2HidPort::DeviceInfo& device, Restart target)
{
    return beginSession(device, Image{}, target);
}

bool Ctr2FirmwareUpdater::start(const Ctr2HidPort::DeviceInfo& device, const Image& image)
{
    return beginSession(device, image, std::nullopt);
}

bool Ctr2FirmwareUpdater::beginSession(const Ctr2HidPort::DeviceInfo& device, const Image& image,
                                       std::optional<Restart> restart)
{
    if (isRunning()) {
        return false;
    }
    m_restart = restart;
    m_restartUnanswered = false;
    m_device = device;
    m_image = image;
    m_sha256 = QCryptographicHash::hash(image.bytes, QCryptographicHash::Sha256);
    m_sent = m_stored = 0;
    m_inFlight = 0;
    m_previousVersion.clear();
    QString error;
    setPhase(Phase::Calling, tr("Connecting to the device"));
    if (!openPort(&error)) {
        fail(error);
        return true;
    }
    armTimeout(m_timing.answerTimeoutMs);
    if (restart) {
        qCInfo(lcDevices) << "CTR2 firmware: restarting" << device.label()
                          << (*restart == Restart::Bootloader ? "into download mode"
                                                              : "into its other firmware");
    } else {
        qCInfo(lcDevices) << "CTR2 firmware: updating" << device.label() << "to" << image.project
                          << image.version << image.bytes.size() << "bytes";
    }
    return true;
}

void Ctr2FirmwareUpdater::cancel()
{
    switch (m_phase) {
    case Phase::Calling:
    case Phase::Begin:
    case Phase::Sending:
        fail(tr("Update cancelled.") + kKeepsFirmware);
        break;
    default:
        // After End the device decides; let the update finish.
        break;
    }
}

void Ctr2FirmwareUpdater::setPhase(Phase phase, const QString& step)
{
    m_phase = phase;
    if (!step.isEmpty()) {
        emit stepChanged(step);
    }
}

bool Ctr2FirmwareUpdater::openPort(QString* error)
{
    closePort();
    Ctr2HidPort* port = m_opener ? m_opener(m_device, error) : nullptr;
    if (!port) {
        if (error->isEmpty()) {
            *error = tr("Cannot open the device.");
        }
        return false;
    }
    m_port = port;
    port->setParent(nullptr);
    m_reassembler.reset();
    m_reassembler.setExtensions(capabilityBit(MessageType::Firmware));
    m_reportsQueued = 0;
    m_reportsWritten = 0;
    m_restartQueuedTo = 0;
    connect(port, &Ctr2HidPort::extensionsNegotiated, this, &Ctr2FirmwareUpdater::onExtensions);
    connect(port, &Ctr2HidPort::reportsSent, this, [this](int count) { m_reportsWritten += count; });
    connect(port, &Ctr2HidPort::reportsReceived, this, &Ctr2FirmwareUpdater::onReports);
    connect(port, &Ctr2HidPort::failed, this, &Ctr2FirmwareUpdater::onPortFailed);
    return true;
}

void Ctr2FirmwareUpdater::closePort(const std::vector<Report>& finalReports)
{
    if (m_port) {
        Ctr2HidPort* port = m_port;
        m_port.clear();
        disconnect(port, nullptr, this, nullptr);
        port->shutdown(finalReports);
    }
}

void Ctr2FirmwareUpdater::onExtensions(quint32 extensions)
{
    const std::optional<deviceinfo::Info> info =
        m_port ? m_port->firmwareInfo() : std::optional<deviceinfo::Info>();
    switch (m_phase) {
    case Phase::Calling:
        if (!(extensions & capabilityBit(MessageType::Firmware))) {
            fail(tr("This device cannot be updated over USB (its firmware does not offer it)."));
            return;
        }
        m_previousVersion = info ? info->version() : QString();
        sendHello();
        m_retry->start(m_timing.helloRetryMs);
        break;
    case Phase::Confirming:
        if (!info) {
            fail(tr("The device came back without reporting its firmware version."));
            return;
        }
        if (info->version() != m_image.reportedVersion) {
            fail(tr("The device came back running %1, not %2: it did not keep the update.")
                     .arg(info->version(), m_image.reportedVersion));
            return;
        }
        sendHello();   // the new image keeps itself once it answers
        m_retry->start(m_timing.helloRetryMs);
        armTimeout(m_timing.answerTimeoutMs);
        break;
    case Phase::Verifying:
        if (!info || info->version() != m_image.reportedVersion) {
            fail(tr("The device is not running the new firmware."));
        } else if (info->pending()) {
            fail(tr("The device runs %1 but has not kept it; it will go back to %2 "
                    "when it restarts.").arg(info->version(), m_previousVersion));
        } else {
            closePort();
            succeed(info->version());
        }
        break;
    default:
        break;
    }
}

void Ctr2FirmwareUpdater::sendHello()
{
    if (!m_port) {
        return;
    }
    std::vector<Report> out;
    m_encoder.reset();
    m_encoder.encodeControl(MessageType::Hello, &out);
    m_reportsQueued += qint64(out.size());
    m_port->send(out);
}

bool Ctr2FirmwareUpdater::sendExtension(const QByteArray& payload)
{
    std::vector<Report> out;
    if (!m_port || !m_encoder.encodeExtension(MessageType::Firmware, payload, &out)) {
        return false;
    }
    m_reportsQueued += qint64(out.size());
    m_port->send(out);
    return true;
}

void Ctr2FirmwareUpdater::onReports(const QByteArray& reports)
{
    std::vector<Message> messages;
    for (qsizetype off = 0; off + kReportBytes <= reports.size(); off += kReportBytes) {
        if (!m_reassembler.feed(reinterpret_cast<const std::uint8_t*>(reports.constData() + off),
                                kReportBytes, &messages)) {
            fail(tr("Link error during the update: %1.").arg(m_reassembler.errorText())
                 + (m_phase == Phase::Ending ? QString() : kKeepsFirmware));
            return;
        }
    }
    for (const Message& m : messages) {
        if (!isRunning()) {
            return;
        }
        onMessage(m);
    }
}

void Ctr2FirmwareUpdater::onMessage(const Message& message)
{
    if (message.type == MessageType::Hello) {
        // The device restarted its link mid-session.
        if (m_phase == Phase::Begin || m_phase == Phase::Sending || m_phase == Phase::Ending) {
            fail(tr("The device restarted its link during the update.") + kKeepsFirmware);
        }
        return;
    }
    if (message.type == MessageType::Ready) {
        if (m_phase == Phase::Calling && m_restart) {
            m_retry->stop();
            m_encoder.reset();   // the session opens in place of the host's Ready
            const bool loader = *m_restart == Restart::Bootloader;
            setPhase(Phase::Commanding, loader ? tr("Restarting into download mode")
                                               : tr("Restarting into the other firmware"));
            if (!sendExtension(loader ? firmware::encodeBootloader() : firmware::encodeSwitch())) {
                fail(tr("The restart request could not be sent."));
                return;
            }
            m_restartQueuedTo = m_reportsQueued;
            armTimeout(m_timing.answerTimeoutMs);
        } else if (m_phase == Phase::Calling) {
            m_retry->stop();
            m_encoder.reset();   // the session opens in place of the host's Ready
            setPhase(Phase::Begin, tr("Starting the update"));
            sendExtension(firmware::encodeBegin(static_cast<quint32>(m_image.bytes.size()),
                                                m_sha256));
            armTimeout(m_timing.answerTimeoutMs);
        } else if (m_phase == Phase::Confirming) {
            // The new image has completed a link, so it keeps itself. End
            // the link and read the version again to make sure.
            m_retry->stop();
            std::vector<Report> closed;
            m_encoder.reset();
            m_encoder.encodeControl(MessageType::Closed, &closed);
            closePort(closed);
            setPhase(Phase::Verifying, tr("Checking the new firmware"));
            m_retry->start(m_timing.pollMs);
            armTimeout(m_timing.returnTimeoutMs);
        }
        return;
    }
    firmware::Reply reply;
    if (message.type == MessageType::Firmware && firmware::decodeStatus(message.payload, &reply)) {
        onStatus(reply);
    }
}

void Ctr2FirmwareUpdater::onStatus(const firmware::Reply& reply)
{
    if (m_phase == Phase::Commanding) {
        const bool loader = m_restart && *m_restart == Restart::Bootloader;
        if (reply.op != (loader ? firmware::kBootloader : firmware::kSwitch)) {
            return;
        }
        if (reply.status != firmware::Status::Ok) {
            fail(loader ? tr("This firmware cannot restart into download mode; update it first, "
                             "or use the BOOT and RESET buttons.")
                        : tr("The device has no other firmware to start."));
            return;
        }
        closePort();   // it is leaving the bus
        restarted();
        return;
    }
    if (reply.status != firmware::Status::Ok) {
        if (reply.op == firmware::kEnd) {
            fail(tr("The device refused the new firmware: %1.").arg(firmware::statusText(reply.status))
                 + kKeepsFirmware);
        } else {
            fail(tr("The update stopped: %1.").arg(firmware::statusText(reply.status))
                 + kKeepsFirmware);
        }
        return;
    }
    if (m_phase == Phase::Begin && reply.op == firmware::kBegin) {
        setPhase(Phase::Sending, tr("Sending the firmware"));
        sendChunks();
        armTimeout(m_timing.answerTimeoutMs);
    } else if (m_phase == Phase::Sending && reply.op == firmware::kData) {
        m_inFlight = qMax(0, m_inFlight - 1);
        m_stored = reply.value;
        emit progress(m_stored, m_image.bytes.size());
        if (m_stored >= m_image.bytes.size() && m_inFlight == 0) {
            setPhase(Phase::Ending, tr("The device is checking the firmware"));
            sendExtension(firmware::encodeEnd());
            armTimeout(m_timing.endTimeoutMs);
        } else {
            sendChunks();
            armTimeout(m_timing.answerTimeoutMs);
        }
    } else if (m_phase == Phase::Ending && reply.op == firmware::kEnd) {
        // The device now leaves the bus and restarts on the new image.
        closePort();
        setPhase(Phase::Returning, tr("Restarting the device"));
        QTimer::singleShot(m_timing.firstLookMs, this, [this] {
            if (m_phase == Phase::Returning) {
                lookForDevice();
                m_retry->start(m_timing.pollMs);
            }
        });
        armTimeout(m_timing.returnTimeoutMs);
    }
}

void Ctr2FirmwareUpdater::sendChunks()
{
    while (m_inFlight < m_window && m_sent < m_image.bytes.size()) {
        const QByteArray chunk = m_image.bytes.mid(m_sent, firmware::kChunkBytes);
        sendExtension(firmware::encodeData(static_cast<quint32>(m_sent), chunk));
        m_sent += chunk.size();
        ++m_inFlight;
    }
}

void Ctr2FirmwareUpdater::lookForDevice()
{
    if (m_port || !m_devices) {
        return;
    }
    for (const Ctr2HidPort::DeviceInfo& d : m_devices()) {
        if (d.serial == m_device.serial && d.vendorId == m_device.vendorId
            && d.productId == m_device.productId) {
            m_device = d;   // its path may have changed
            QString error;
            if (!openPort(&error)) {
                return;   // not ready yet; try again on the next poll
            }
            m_retry->stop();
            if (m_phase == Phase::Returning) {
                setPhase(Phase::Confirming, tr("Confirming the new firmware"));
            }
            return;
        }
    }
}

void Ctr2FirmwareUpdater::onPortFailed(const QString& message)
{
    if (m_port) {
        Ctr2HidPort* port = m_port;
        m_port.clear();
        disconnect(port, nullptr, this, nullptr);
        port->shutdown({});
    }
    switch (m_phase) {
    case Phase::Returning:
    case Phase::Verifying:
        // Expected while the device restarts. lookForDevice() stopped the
        // poll when it opened the port: look again.
        m_retry->start(m_timing.pollMs);
        break;
    case Phase::Confirming:
        setPhase(Phase::Returning, QString());
        m_retry->start(m_timing.pollMs);
        break;
    case Phase::Ending:
        fail(tr("The device left during the final check (%1). Reconnect it: it runs "
                "either the new firmware or the old one.").arg(message));
        break;
    case Phase::Commanding:
        // Leaving the bus is what the request asks for. A device that does it
        // after the request was written, before its STATUS arrives or with a
        // STATUS naming another op, has still restarted. A port that failed
        // before writing it (a write error) has not.
        if (m_reportsWritten >= m_restartQueuedTo) {
            restarted();
            break;
        }
        fail(tr("Lost the device: %1.").arg(message) + kKeepsFirmware);
        break;
    default:
        fail(tr("Lost the device: %1.").arg(message) + kKeepsFirmware);
        break;
    }
}

void Ctr2FirmwareUpdater::restarted()
{
    const bool loader = m_restart && *m_restart == Restart::Bootloader;
    m_timeout->stop();
    m_phase = Phase::Idle;
    m_restart.reset();
    emit finished(true, loader ? tr("The device is restarting into download mode.")
                               : tr("The device is restarting into its other firmware."),
                  QString());
}

void Ctr2FirmwareUpdater::onTimeout()
{
    switch (m_phase) {
    case Phase::Calling:
        fail(tr("The device did not answer.") + kKeepsFirmware);
        break;
    case Phase::Begin:
    case Phase::Sending:
        fail(tr("The device stopped answering during the update.") + kKeepsFirmware);
        break;
    case Phase::Ending:
        fail(tr("The device did not finish checking the firmware. Reconnect it: it runs "
                "either the new firmware or the old one."));
        break;
    case Phase::Returning:
    case Phase::Confirming:
    case Phase::Verifying:
        fail(tr("The device did not come back on the new firmware. If it restarts before "
                "it connects, it goes back to %1.").arg(m_previousVersion.isEmpty()
                                                            ? tr("its previous firmware")
                                                            : m_previousVersion));
        break;
    case Phase::Commanding:
        m_restartUnanswered = m_restartQueuedTo > 0 && m_reportsWritten >= m_restartQueuedTo;
        fail(tr("The device did not answer the restart request."));
        break;
    case Phase::Idle:
        break;
    }
}

void Ctr2FirmwareUpdater::armTimeout(int ms)
{
    m_timeout->start(ms);
}

void Ctr2FirmwareUpdater::fail(const QString& message)
{
    if (!isRunning()) {
        return;
    }
    m_timeout->stop();
    m_retry->stop();
    // End the session so the device drops any partial image now and leaves
    // its update screen (also after it refused the image at End).
    closePort(sessionEndReports());
    m_phase = Phase::Idle;
    qCWarning(lcDevices) << "CTR2 firmware: update failed:" << message;
    emit finished(false, message, QString());
}

void Ctr2FirmwareUpdater::succeed(const QString& version)
{
    m_timeout->stop();
    m_retry->stop();
    m_phase = Phase::Idle;
    qCInfo(lcDevices) << "CTR2 firmware: the device runs and has kept" << version;
    emit finished(true, tr("The device now runs %1.").arg(version), version);
}

} // namespace AetherSDR
