#include "RigctlPty.h"
#include "LogManager.h"
#include "RigctlProtocol.h"
#include "models/RadioModel.h"

#include <QSocketNotifier>

#ifndef _WIN32
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>

#ifdef __APPLE__
#include <util.h>       // openpty() on macOS
#else
#include <pty.h>        // openpty() on Linux
#endif
#endif // !_WIN32

namespace MasterSDR {

RigctlPty::RigctlPty(RadioModel* model, QObject* parent)
    : QObject(parent)
    , m_model(model)
{}

RigctlPty::~RigctlPty()
{
    stop();
}

bool RigctlPty::start()
{
#ifdef _WIN32
    qCWarning(lcCat) << "RigctlPty: PTY not supported on Windows";
    return false;
#else
    if (m_masterFd >= 0)
        return true;  // already running

    char slaveName[256] = {};
    if (openpty(&m_masterFd, &m_slaveFd, slaveName, nullptr, nullptr) != 0) {
        qCWarning(lcCat) << "RigctlPty: openpty() failed";
        return false;
    }

    m_slavePath = QString::fromLocal8Bit(slaveName);

    // Set master FD to non-blocking
    int flags = fcntl(m_masterFd, F_GETFL);
    fcntl(m_masterFd, F_SETFL, flags | O_NONBLOCK);

    // Configure slave terminal: raw mode, no echo
    struct termios tio;
    if (tcgetattr(m_slaveFd, &tio) == 0) {
        cfmakeraw(&tio);
        tio.c_cc[VMIN] = 1;
        tio.c_cc[VTIME] = 0;
        tcsetattr(m_slaveFd, TCSANOW, &tio);
    }

    // Create symlink for convenience
    ::unlink(m_symlinkPath.toLocal8Bit().constData());
    if (::symlink(slaveName, m_symlinkPath.toLocal8Bit().constData()) != 0) {
        qCWarning(lcCat) << "RigctlPty: symlink failed:" << m_symlinkPath;
    }

    // Set up protocol handler
    m_protocol = new RigctlProtocol(m_model);
    m_protocol->setSliceIndex(m_sliceIndex);

    // Watch for data on the master FD
    m_notifier = new QSocketNotifier(m_masterFd, QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated, this, &RigctlPty::onDataReady);

    qCInfo(lcCat) << "RigctlPty: started on" << m_slavePath << "symlink:" << m_symlinkPath;
    emit started(m_symlinkPath);
    return true;
#endif
}

void RigctlPty::stop()
{
#ifdef _WIN32
    return;
#else
    if (m_masterFd < 0)
        return;

    delete m_notifier;
    m_notifier = nullptr;

    delete m_protocol;
    m_protocol = nullptr;

    ::close(m_masterFd);
    ::close(m_slaveFd);
    m_masterFd = -1;
    m_slaveFd = -1;
    m_buffer.clear();

    // Remove symlink
    ::unlink(m_symlinkPath.toLocal8Bit().constData());

    qCInfo(lcCat) << "RigctlPty: stopped";
    emit stopped();
#endif
}

void RigctlPty::onDataReady()
{
#ifdef _WIN32
    return;
#else
    char buf[4096];
    ssize_t n = ::read(m_masterFd, buf, sizeof(buf));
    if (n <= 0) {
        if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
            // PTY closed by the other end — keep it open for reconnection
        }
        return;
    }

    m_buffer.append(buf, static_cast<int>(n));

    // Process complete lines
    while (true) {
        int nlPos = m_buffer.indexOf('\n');
        if (nlPos < 0) {
            // Also try \r as line terminator (some serial software uses \r only)
            nlPos = m_buffer.indexOf('\r');
            if (nlPos < 0) break;
        }

        QString line = QString::fromUtf8(m_buffer.left(nlPos));
        m_buffer.remove(0, nlPos + 1);

        // Skip empty lines
        if (line.trimmed().isEmpty())
            continue;

        QString response = m_protocol->handleLine(line);
        if (!response.isEmpty()) {
            QByteArray data = response.toUtf8();
            ::write(m_masterFd, data.constData(), data.size());
        }
    }
#endif
}

} // namespace MasterSDR
