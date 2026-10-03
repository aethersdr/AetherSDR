#pragma once

#include "Ctr2HidPort.h"

#include <QString>

#include <atomic>
#include <functional>
#include <memory>

class QThread;

namespace AetherSDR {

namespace detail {
class Ctr2HidIoWorker;
}

// Device operations a Ctr2HidThreadPort drives. All are called only on the
// port's I/O thread. read() must not block: it returns the bytes read
// (report ID included when the device numbers its reports), 0 when nothing
// is pending, or <0 on error. write() may block and returns <0 on error.
struct Ctr2HidDeviceIo {
    std::function<int(unsigned char* buffer, int size)> read;
    std::function<int(const unsigned char* buffer, int size)> write;
    std::function<void()> close;
    std::function<QString()> lastError;
};

// Ctr2HidPort over one dedicated I/O thread, because HID writes block.
// Nothing here ever waits for that thread on the owner's thread: queued
// output is cancelled between writes, and shutdown() lets the port delete
// itself once the thread has finished.
class Ctr2HidThreadPort final : public Ctr2HidPort {
    Q_OBJECT

public:
    Ctr2HidThreadPort(Ctr2HidDeviceIo io, const QString& description, QObject* parent = nullptr);
    // Only reached when shutdown() was never called; waits for an idle thread.
    ~Ctr2HidThreadPort() override;

    bool isOpen() const override { return m_open; }
    void send(const std::vector<ctr2hid::Report>& reports) override;
    void discardQueued() override;
    void shutdown(const std::vector<ctr2hid::Report>& finalReports) override;
    QString description() const override { return m_description; }

private:
    friend class detail::Ctr2HidIoWorker;

    void deliverReceived(const QByteArray& reports);
    void deliverSent(int count, quint64 generation);
    void deliverFailure(const QString& message);

    QThread* m_thread{nullptr};
    detail::Ctr2HidIoWorker* m_worker{nullptr};
    // Shared with the worker, which re-reads them between writes.
    std::shared_ptr<std::atomic<quint64>> m_generation;
    std::shared_ptr<std::atomic<bool>> m_cancelled;
    QString m_description;
    bool m_open{true};
    bool m_shuttingDown{false};
};

} // namespace AetherSDR
