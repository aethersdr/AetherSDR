#include "Ctr2HidThreadPort.h"

#include "LogManager.h"

#include <QMetaObject>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <array>
#include <utility>

namespace AetherSDR {

namespace {

constexpr int kPollIntervalMs = 1;
constexpr int kMaxReadsPerTick = 64;
constexpr int kMaxWritesPerTick = 16;

QByteArray toBytes(const std::vector<ctr2hid::Report>& reports)
{
    QByteArray bytes;
    bytes.reserve(static_cast<qsizetype>(reports.size()) * ctr2hid::kReportBytes);
    for (const ctr2hid::Report& r : reports) {
        bytes.append(reinterpret_cast<const char*>(r.data()), ctr2hid::kReportBytes);
    }
    return bytes;
}

} // namespace

namespace detail {

// Lives on the port's I/O thread and owns the device there.
class Ctr2HidIoWorker : public QObject {
public:
    Ctr2HidIoWorker(Ctr2HidDeviceIo io, Ctr2HidThreadPort* port,
                    std::shared_ptr<std::atomic<quint64>> generation,
                    std::shared_ptr<std::atomic<bool>> cancelled)
        : m_io(std::move(io))
        , m_port(port)
        , m_sharedGeneration(std::move(generation))
        , m_cancelled(std::move(cancelled))
        , m_generation(m_sharedGeneration->load())
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

    void enqueue(const QByteArray& reports, quint64 generation)
    {
        if (generation == m_generation) {
            m_outbox.append(reports);
        }
    }

    void discard(quint64 generation)
    {
        m_generation = generation;
        m_outbox.clear();
    }

    // Final reports are written even though the port is cancelled; then the
    // device is closed and this thread stops.
    void finish(const QByteArray& finalReports)
    {
        if (m_timer) {
            m_timer->stop();
        }
        m_outbox.clear();
        for (qsizetype off = 0; m_open && off + ctr2hid::kReportBytes <= finalReports.size();
             off += ctr2hid::kReportBytes) {
            if (!writeReport(finalReports.constData() + off)) {
                break;
            }
        }
        closeDevice();
        QThread::currentThread()->quit();
    }

private:
    bool writeReport(const char* report)
    {
        std::array<unsigned char, 1 + ctr2hid::kReportBytes> out{};
        out[0] = ctr2hid::kReportId;
        std::copy(report, report + ctr2hid::kReportBytes, out.begin() + 1);
        return m_io.write(out.data(), static_cast<int>(out.size())) >= 0;
    }

    void poll()
    {
        if (!m_open) {
            return;
        }
        QByteArray received;
        std::array<unsigned char, 65> buf{};
        for (int i = 0; i < kMaxReadsPerTick; ++i) {
            const int n = m_io.read(buf.data(), static_cast<int>(buf.size()));
            if (n == 0) {
                break;
            }
            if (n < 0) {
                fail(QStringLiteral("CTR2 USB read failed: %1").arg(errorText()));
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
        const quint64 generation = m_generation;
        while (sent < kMaxWritesPerTick && m_outbox.size() >= ctr2hid::kReportBytes) {
            // A fence or shutdown requested mid-batch stops before the next write.
            if (m_cancelled->load() || m_sharedGeneration->load() != generation) {
                break;
            }
            if (!writeReport(m_outbox.constData())) {
                fail(QStringLiteral("CTR2 USB write failed: %1").arg(errorText()));
                return;
            }
            m_outbox.remove(0, ctr2hid::kReportBytes);
            ++sent;
        }
        if (sent > 0) {
            QMetaObject::invokeMethod(m_port, [port = m_port, sent, generation] {
                port->deliverSent(sent, generation);
            }, Qt::QueuedConnection);
        }
    }

    QString errorText() const
    {
        const QString text = m_io.lastError ? m_io.lastError() : QString();
        return text.isEmpty() ? QStringLiteral("HID I/O error") : text;
    }

    void closeDevice()
    {
        if (m_open) {
            m_open = false;
            if (m_io.close) {
                m_io.close();
            }
        }
    }

    void fail(const QString& message)
    {
        if (m_timer) {
            m_timer->stop();
        }
        m_outbox.clear();
        closeDevice();
        QMetaObject::invokeMethod(m_port, [port = m_port, message] {
            port->deliverFailure(message);
        }, Qt::QueuedConnection);
    }

    Ctr2HidDeviceIo m_io;
    Ctr2HidThreadPort* m_port;
    std::shared_ptr<std::atomic<quint64>> m_sharedGeneration;
    std::shared_ptr<std::atomic<bool>> m_cancelled;
    quint64 m_generation;
    QTimer* m_timer{nullptr};
    QByteArray m_outbox;
    bool m_open{true};
};

} // namespace detail

Ctr2HidThreadPort::Ctr2HidThreadPort(Ctr2HidDeviceIo io, const QString& description, QObject* parent)
    : Ctr2HidPort(parent)
    , m_generation(std::make_shared<std::atomic<quint64>>(0))
    , m_cancelled(std::make_shared<std::atomic<bool>>(false))
    , m_description(description)
{
    m_thread = new QThread(this);
    m_thread->setObjectName(QStringLiteral("Ctr2HidIo"));
    m_worker = new detail::Ctr2HidIoWorker(std::move(io), this, m_generation, m_cancelled);
    m_worker->moveToThread(m_thread);
    connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    m_thread->start();
    QMetaObject::invokeMethod(m_worker, [w = m_worker] { w->start(); }, Qt::QueuedConnection);
}

Ctr2HidThreadPort::~Ctr2HidThreadPort()
{
    if (m_thread->isRunning()) {
        m_cancelled->store(true);
        QMetaObject::invokeMethod(m_worker, [w = m_worker] { w->finish({}); },
                                  Qt::BlockingQueuedConnection);
        m_thread->wait();
    }
}

void Ctr2HidThreadPort::send(const std::vector<ctr2hid::Report>& reports)
{
    if (!m_open || m_shuttingDown || reports.empty()) {
        return;
    }
    const QByteArray bytes = toBytes(reports);
    const quint64 generation = m_generation->load();
    QMetaObject::invokeMethod(m_worker, [w = m_worker, bytes, generation] {
        w->enqueue(bytes, generation);
    }, Qt::QueuedConnection);
}

void Ctr2HidThreadPort::discardQueued()
{
    if (m_shuttingDown) {
        return;
    }
    const quint64 generation = ++(*m_generation);
    QMetaObject::invokeMethod(m_worker, [w = m_worker, generation] { w->discard(generation); },
                              Qt::QueuedConnection);
}

void Ctr2HidThreadPort::shutdown(const std::vector<ctr2hid::Report>& finalReports)
{
    if (m_shuttingDown) {
        return;
    }
    m_shuttingDown = true;
    m_open = false;
    setParent(nullptr);
    m_cancelled->store(true);
    ++(*m_generation);
    connect(m_thread, &QThread::finished, this, &QObject::deleteLater);
    if (!m_thread->isRunning()) {
        deleteLater();
        return;
    }
    const QByteArray bytes = toBytes(finalReports);
    QMetaObject::invokeMethod(m_worker, [w = m_worker, bytes] { w->finish(bytes); },
                              Qt::QueuedConnection);
}

void Ctr2HidThreadPort::deliverReceived(const QByteArray& reports)
{
    if (m_open) {
        emit reportsReceived(reports);
    }
}

void Ctr2HidThreadPort::deliverSent(int count, quint64 generation)
{
    if (m_open && generation == m_generation->load()) {
        emit reportsSent(count);
    }
}

void Ctr2HidThreadPort::deliverFailure(const QString& message)
{
    if (!m_open) {
        return;
    }
    m_open = false;
    qCWarning(lcDevices) << "CTR2 USB:" << message;
    emit failed(message);
}

} // namespace AetherSDR
