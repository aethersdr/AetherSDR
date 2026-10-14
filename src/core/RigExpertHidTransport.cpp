#include "RigExpertHidTransport.h"

#include <QCryptographicHash>
#include <QDir>
#include <QLockFile>
#include <QThread>
#include <QTimer>

#include <array>
#include <atomic>
#include <memory>

#ifdef HAVE_HIDAPI
#include <hidapi/hidapi.h>
#endif

namespace AetherSDR {

bool RigExpertHidTransport::available()
{
#ifdef HAVE_HIDAPI
    return true;
#else
    return false;
#endif
}

QList<RigExpertUsbDevice> RigExpertHidTransport::enumerate()
{
    QList<RigExpertUsbDevice> devices;
#ifdef HAVE_HIDAPI
    if (hid_init() != 0) { return devices; }
    hid_device_info* list = hid_enumerate(0x0483, 0xa1de);
    for (const hid_device_info* d = list; d; d = d->next) {
        const QString serial = d->serial_number ? QString::fromWCharArray(d->serial_number) : QString();
        // This PoC deliberately excludes other RigExpert models and bootloaders.
        if (!serial.startsWith(QLatin1String("4600")) && !serial.startsWith(QLatin1String("4601"))) {
            continue;
        }
        devices.append({QString::fromUtf8(d->path), serial,
                        d->product_string ? QString::fromWCharArray(d->product_string) : QString()});
    }
    hid_free_enumeration(list);
#endif
    return devices;
}

#ifdef HAVE_HIDAPI
namespace {

class HidWorker final : public QObject {
    Q_OBJECT
public:
    HidWorker(std::shared_ptr<std::atomic<bool>> cancelled,
              std::shared_ptr<std::atomic<int>> pendingReceives)
        : m_cancelled(std::move(cancelled)), m_pendingReceives(std::move(pendingReceives)) {}

    void open(const QString& path)
    {
        const QByteArray key = QCryptographicHash::hash(path.toUtf8(), QCryptographicHash::Sha256).toHex();
        m_lock = std::make_unique<QLockFile>(QDir::temp().filePath(
            QStringLiteral("aethersdr-rigexpert-%1.lock").arg(QString::fromLatin1(key))));
        if (!m_lock->tryLock(0)) { fail(QStringLiteral("Stick Pro is claimed by another AetherSDR instance")); return; }
        if (m_cancelled->load()) { finish({}); return; }
        if (hid_init() != 0) { fail(QStringLiteral("HID initialization failed")); return; }
        m_handle = hid_open_path(path.toUtf8().constData());
        if (!m_handle) { fail(QStringLiteral("Cannot open Stick Pro: %1").arg(error())); return; }
        if (hid_set_nonblocking(m_handle, 1) != 0) { fail(QStringLiteral("Cannot enable nonblocking HID reads")); return; }
        // Discard bytes left by an earlier host before requesting a new identity.
        std::array<unsigned char, 64> buffer{};
        for (int i = 0; i < 64; ++i) {
            const int n = hid_read(m_handle, buffer.data(), buffer.size());
            if (n < 0) { fail(QStringLiteral("Initial HID drain failed: %1").arg(error())); return; }
            if (n == 0) { break; }
            if (i == 63) { fail(QStringLiteral("Stick Pro is still streaming; stop AntScope and reconnect")); return; }
        }
        m_timer = new QTimer(this);
        m_timer->setInterval(5);
        connect(m_timer, &QTimer::timeout, this, &HidWorker::poll);
        m_timer->start();
        emit opened();
    }

    void send(const QByteArray& report)
    {
        if (m_handle && !m_cancelled->load()) { write(report); }
    }

    void finish(const QByteArray& report)
    {
        if (m_finished) { return; }
        m_finished = true;
        if (m_timer) { m_timer->stop(); }
        if (m_handle) {
            if (!report.isEmpty()) { write(report); }
            if (m_handle) { hid_close(m_handle); m_handle = nullptr; }
        }
        m_lock.reset();
        emit closed();
        QThread::currentThread()->quit();
    }

signals:
    void opened();
    void reportReceived(const QByteArray& report);
    void reportWritten(const QByteArray& report);
    void failed(const QString& error);
    void closed();

private:
    QString error() const
    {
        const wchar_t* message = hid_error(m_handle);
        return message ? QString::fromWCharArray(message) : QStringLiteral("HID I/O error");
    }
    bool write(const QByteArray& report)
    {
        if (report.size() != 64) { fail(QStringLiteral("Refusing a non-64-byte HID output report")); return false; }
        const int n = hid_write(m_handle, reinterpret_cast<const unsigned char*>(report.constData()), report.size());
        if (n != report.size()) { fail(QStringLiteral("HID write failed or short (%1/64): %2").arg(n).arg(error())); return false; }
        emit reportWritten(report);
        return true;
    }
    void poll()
    {
        if (!m_handle || m_cancelled->load()) { return; }
        std::array<unsigned char, 64> buffer{};
        for (int i = 0; i < 8; ++i) {
            const int n = hid_read(m_handle, buffer.data(), buffer.size());
            if (n == 0) { return; }
            if (n < 0) { fail(QStringLiteral("HID read failed: %1").arg(error())); return; }
            if (m_pendingReceives->load() >= 64) {
                fail(QStringLiteral("HID receive backlog exceeded 64 reports")); return;
            }
            m_pendingReceives->fetch_add(1);
            emit reportReceived(QByteArray(reinterpret_cast<const char*>(buffer.data()), n));
        }
    }
    void fail(const QString& message)
    {
        emit failed(message);
        QByteArray stop(64, '\0');
        stop[0] = 0x07;
        stop[1] = 4;
        stop.replace(2, 4, "off\r");
        finish(stop);
    }
    std::shared_ptr<std::atomic<bool>> m_cancelled;
    std::shared_ptr<std::atomic<int>> m_pendingReceives;
    std::unique_ptr<QLockFile> m_lock;
    hid_device* m_handle{nullptr};
    QTimer* m_timer{nullptr};
    bool m_finished{false};
};

class HidTransport final : public RigExpertHidTransport {
    Q_OBJECT
public:
    explicit HidTransport(QObject* parent) : RigExpertHidTransport(parent) {}
    ~HidTransport() override { finish({}); }

    void start(const QString& path) override
    {
        if (m_started) { return; }
        m_started = true;
        auto* ioThread = new QThread;
        auto* worker = new HidWorker(m_cancelled, m_pendingReceives);
        worker->moveToThread(ioThread);
        connect(ioThread, &QThread::started, worker, [worker, path] { worker->open(path); });
        connect(ioThread, &QThread::finished, worker, &QObject::deleteLater);
        connect(ioThread, &QThread::finished, ioThread, &QObject::deleteLater);
        connect(this, &HidTransport::sendRequested, worker, &HidWorker::send, Qt::QueuedConnection);
        connect(this, &HidTransport::finishRequested, worker, &HidWorker::finish, Qt::QueuedConnection);
        connect(worker, &HidWorker::opened, this, &RigExpertHidTransport::opened);
        connect(worker, &HidWorker::reportReceived, this, [this](const QByteArray& report) {
            m_pendingReceives->fetch_sub(1);
            emit reportReceived(report);
        });
        connect(worker, &HidWorker::reportWritten, this, &RigExpertHidTransport::reportWritten);
        connect(worker, &HidWorker::failed, this, [this](const QString& error) {
            m_closing = true;
            emit failed(error);
        });
        connect(worker, &HidWorker::closed, this, [this] {
            m_closing = true;
            emit closed();
        });
        ioThread->start();
    }
    void send(const QByteArray& report) override
    {
        if (m_started && !m_closing) { emit sendRequested(report); }
    }
    void finish(const QByteArray& report) override
    {
        if (!m_started || m_closing) { return; }
        m_closing = true;
        m_cancelled->store(true);
        emit finishRequested(report);
    }
signals:
    void sendRequested(const QByteArray& report);
    void finishRequested(const QByteArray& report);
private:
    std::shared_ptr<std::atomic<bool>> m_cancelled{std::make_shared<std::atomic<bool>>(false)};
    std::shared_ptr<std::atomic<int>> m_pendingReceives{std::make_shared<std::atomic<int>>(0)};
    bool m_started{false};
    bool m_closing{false};
};

} // namespace
#endif

RigExpertHidTransport* RigExpertHidTransport::create(QObject* parent)
{
#ifdef HAVE_HIDAPI
    return new HidTransport(parent);
#else
    Q_UNUSED(parent);
    return nullptr;
#endif
}

} // namespace AetherSDR

#ifdef HAVE_HIDAPI
#include "RigExpertHidTransport.moc"
#endif
