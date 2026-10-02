#include "Ctr2HidapiPort.h"

#include "LogManager.h"

#include <QMetaObject>
#include <QThread>
#include <QTimer>

#include <hidapi/hidapi.h>

#include <algorithm>
#include <array>

namespace AetherSDR {

namespace {

constexpr unsigned short kUsagePage = 0xFF00;
constexpr unsigned short kUsage = 0x01;
constexpr int kPollIntervalMs = 1;
constexpr int kMaxReadsPerTick = 64;
constexpr int kMaxWritesPerTick = 16;

QString fromWide(const wchar_t* s)
{
    return s ? QString::fromWCharArray(s) : QString();
}

QString hidError(hid_device* device)
{
    const QString text = fromWide(hid_error(device));
    return text.isEmpty() ? QStringLiteral("HID I/O error") : text;
}

// hid_init() is idempotent. hid_exit() is never called: it is process-wide
// and other HID controllers may still be open.
bool ensureHidInit()
{
    return hid_init() == 0;
}

} // namespace

namespace detail {

// Lives on the port's I/O thread and owns the hid_device there.
class Ctr2HidIoWorker : public QObject {
public:
    Ctr2HidIoWorker(hid_device* device, Ctr2HidapiPort* port)
        : m_device(device)
        , m_port(port)
    {
    }

    void start()
    {
        m_timer = new QTimer(this);
        m_timer->setTimerType(Qt::PreciseTimer);
        m_timer->setInterval(kPollIntervalMs);
        connect(m_timer, &QTimer::timeout, this, [this] { poll(); });
        m_timer->start();
    }

    void enqueue(const QByteArray& reports) { m_outbox.append(reports); }

    // Replaces whatever is queued with finalReports and writes them now.
    void finish(const QByteArray& finalReports)
    {
        m_outbox = finalReports;
        while (m_device && m_outbox.size() >= ctr2hid::kReportBytes) {
            std::array<unsigned char, 1 + ctr2hid::kReportBytes> out{};
            out[0] = ctr2hid::kReportId;
            std::copy(m_outbox.constBegin(), m_outbox.constBegin() + ctr2hid::kReportBytes,
                      out.begin() + 1);
            if (hid_write(m_device, out.data(), out.size()) < 0) {
                break;
            }
            m_outbox.remove(0, ctr2hid::kReportBytes);
        }
        shutdown();
    }

    void shutdown()
    {
        if (m_timer) {
            m_timer->stop();
        }
        if (m_device) {
            hid_close(m_device);
            m_device = nullptr;
        }
    }

private:
    void poll()
    {
        if (!m_device) {
            return;
        }
        QByteArray received;
        std::array<unsigned char, 65> buf{};
        for (int i = 0; i < kMaxReadsPerTick; ++i) {
            const int n = hid_read(m_device, buf.data(), buf.size());
            if (n == 0) {
                break;
            }
            if (n < 0) {
                fail(QStringLiteral("CTR2 USB read failed: %1").arg(hidError(m_device)));
                return;
            }
            // Numbered reports arrive with the report ID first on hidraw,
            // IOHIDManager and Windows; tolerate a backend that strips it.
            if (n == 1 + ctr2hid::kReportBytes && buf[0] == ctr2hid::kReportId) {
                received.append(reinterpret_cast<const char*>(buf.data() + 1), ctr2hid::kReportBytes);
            } else if (n == ctr2hid::kReportBytes) {
                received.append(reinterpret_cast<const char*>(buf.data()), ctr2hid::kReportBytes);
            } else {
                fail(QStringLiteral("Unexpected %1-byte HID report from the CTR2").arg(n));
                return;
            }
        }
        if (!received.isEmpty()) {
            QMetaObject::invokeMethod(m_port, [port = m_port, received] {
                port->deliverReceived(received);
            }, Qt::QueuedConnection);
        }

        int sent = 0;
        while (sent < kMaxWritesPerTick && m_outbox.size() >= ctr2hid::kReportBytes) {
            std::array<unsigned char, 1 + ctr2hid::kReportBytes> out{};
            out[0] = ctr2hid::kReportId;
            std::copy(m_outbox.constBegin(), m_outbox.constBegin() + ctr2hid::kReportBytes,
                      out.begin() + 1);
            if (hid_write(m_device, out.data(), out.size()) < 0) {
                fail(QStringLiteral("CTR2 USB write failed: %1").arg(hidError(m_device)));
                return;
            }
            m_outbox.remove(0, ctr2hid::kReportBytes);
            ++sent;
        }
        if (sent > 0) {
            QMetaObject::invokeMethod(m_port, [port = m_port, sent] {
                port->deliverSent(sent);
            }, Qt::QueuedConnection);
        }
    }

    void fail(const QString& message)
    {
        shutdown();
        m_outbox.clear();
        QMetaObject::invokeMethod(m_port, [port = m_port, message] {
            port->deliverFailure(message);
        }, Qt::QueuedConnection);
    }

    hid_device* m_device;
    Ctr2HidapiPort* m_port;
    QTimer* m_timer{nullptr};
    QByteArray m_outbox;
};

} // namespace detail

QList<Ctr2HidPort::DeviceInfo> Ctr2HidapiPort::enumerate()
{
    QList<DeviceInfo> out;
    if (!ensureHidInit()) {
        return out;
    }
    hid_device_info* list = hid_enumerate(0, 0);
    for (hid_device_info* d = list; d; d = d->next) {
        if (d->usage_page != kUsagePage || d->usage != kUsage) {
            continue;
        }
        DeviceInfo info;
        info.path = QString::fromUtf8(d->path);
        info.vendorId = d->vendor_id;
        info.productId = d->product_id;
        info.manufacturer = fromWide(d->manufacturer_string);
        info.product = fromWide(d->product_string);
        info.serial = fromWide(d->serial_number);
        out.append(info);
    }
    hid_free_enumeration(list);
    // Recognised CTR2 boards first; otherwise keep enumeration order.
    std::stable_sort(out.begin(), out.end(), [](const DeviceInfo& a, const DeviceInfo& b) {
        return !a.ctr2Model().isEmpty() && b.ctr2Model().isEmpty();
    });
    return out;
}

Ctr2HidapiPort* Ctr2HidapiPort::open(const DeviceInfo& device, QString* error, QObject* parent)
{
    if (!ensureHidInit()) {
        *error = QStringLiteral("USB HID support failed to initialize");
        return nullptr;
    }
    hid_device* handle = hid_open_path(device.path.toUtf8().constData());
    if (!handle) {
        *error = QStringLiteral("Cannot open %1: %2").arg(device.label(), hidError(nullptr));
        return nullptr;
    }
    hid_set_nonblocking(handle, 1);
    return new Ctr2HidapiPort(handle, device.label(), parent);
}

Ctr2HidapiPort::Ctr2HidapiPort(void* device, const QString& description, QObject* parent)
    : Ctr2HidPort(parent)
    , m_description(description)
    , m_open(true)
{
    m_thread = new QThread(this);
    m_thread->setObjectName(QStringLiteral("Ctr2HidIo"));
    m_worker = new detail::Ctr2HidIoWorker(static_cast<hid_device*>(device), this);
    m_worker->moveToThread(m_thread);
    connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    m_thread->start();
    QMetaObject::invokeMethod(m_worker, [w = m_worker] { w->start(); }, Qt::QueuedConnection);
}

Ctr2HidapiPort::~Ctr2HidapiPort()
{
    close();
}

void Ctr2HidapiPort::send(const std::vector<ctr2hid::Report>& reports)
{
    if (!m_open || reports.empty()) {
        return;
    }
    QByteArray bytes;
    bytes.reserve(static_cast<qsizetype>(reports.size()) * ctr2hid::kReportBytes);
    for (const ctr2hid::Report& r : reports) {
        bytes.append(reinterpret_cast<const char*>(r.data()), ctr2hid::kReportBytes);
    }
    QMetaObject::invokeMethod(m_worker, [w = m_worker, bytes] { w->enqueue(bytes); },
                              Qt::QueuedConnection);
}

void Ctr2HidapiPort::close()
{
    if (!m_thread) {
        return;
    }
    m_open = false;
    // Waits for any in-flight hid_write, which the OS bounds (a few seconds).
    if (m_thread->isRunning()) {
        QMetaObject::invokeMethod(m_worker, [w = m_worker] { w->shutdown(); },
                                  Qt::BlockingQueuedConnection);
        m_thread->quit();
        m_thread->wait();
    }
    m_thread = nullptr;
    m_worker = nullptr;
}

void Ctr2HidapiPort::closeAfter(const std::vector<ctr2hid::Report>& finalReports)
{
    if (m_thread && m_thread->isRunning() && m_open) {
        QByteArray bytes;
        for (const ctr2hid::Report& r : finalReports) {
            bytes.append(reinterpret_cast<const char*>(r.data()), ctr2hid::kReportBytes);
        }
        QMetaObject::invokeMethod(m_worker, [w = m_worker, bytes] { w->finish(bytes); },
                                  Qt::BlockingQueuedConnection);
    }
    close();
}

void Ctr2HidapiPort::deliverReceived(const QByteArray& reports)
{
    if (m_open) {
        emit reportsReceived(reports);
    }
}

void Ctr2HidapiPort::deliverSent(int count)
{
    if (m_open) {
        emit reportsSent(count);
    }
}

void Ctr2HidapiPort::deliverFailure(const QString& message)
{
    if (!m_open) {
        return;
    }
    qCWarning(lcDevices) << "CTR2 USB:" << message;
    close();
    emit failed(message);
}

} // namespace AetherSDR
