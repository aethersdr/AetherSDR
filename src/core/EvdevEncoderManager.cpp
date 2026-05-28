#include <QtGlobal>
#ifdef Q_OS_LINUX

#include "EvdevEncoderManager.h"
#include "core/LogManager.h"

#include <QDir>
#include <QFile>
#include <QFileSystemWatcher>
#include <QSocketNotifier>
#include <QTimer>

#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace AetherSDR {

namespace {

// Name patterns we recognize.  Long-term this becomes a name-regex catalog
// parallel to HidDeviceParser's VID/PID table (#3232).
const QStringList& supportedNames()
{
    static const QStringList names = {
        QStringLiteral("Ulanzi Dial Keyboard"),
    };
    return names;
}

// Map kernel key codes that we treat as the rotary axis.  These are the
// codes we observed Ulanzi Dial firmware emitting per detent.
constexpr int kRotaryCwKey  = KEY_VOLUMEUP;
constexpr int kRotaryCcwKey = KEY_VOLUMEDOWN;

// Friendly signature for a bare (non-chord) key event.  Used both as
// AppSettings binding key and as a label in the Learn-mode UI.
QString bareKeySignature(int keycode)
{
    switch (keycode) {
        case KEY_PLAYPAUSE:    return QStringLiteral("KEY_PLAYPAUSE");
        case KEY_MUTE:         return QStringLiteral("KEY_MUTE");
        case KEY_PREVIOUSSONG: return QStringLiteral("KEY_PREVIOUSSONG");
        case KEY_NEXTSONG:     return QStringLiteral("KEY_NEXTSONG");
        default:
            return QStringLiteral("KEY_%1").arg(keycode);
    }
}

QString chordSignature(int keycode, bool withPreviousSong)
{
    QString letter;
    switch (keycode) {
        case KEY_V: letter = QStringLiteral("V"); break;
        case KEY_C: letter = QStringLiteral("C"); break;
        case KEY_Y: letter = QStringLiteral("Y"); break;
        case KEY_Z: letter = QStringLiteral("Z"); break;
        default:    letter = QStringLiteral("KEY_%1").arg(keycode); break;
    }
    if (withPreviousSong)
        return QStringLiteral("Ctrl+%1+KEY_PREVIOUSSONG").arg(letter);
    return QStringLiteral("Ctrl+%1").arg(letter);
}

} // namespace

EvdevEncoderManager::EvdevEncoderManager(QObject* parent)
    : QObject(parent)
{
    m_rescanTimer = new QTimer(this);
    m_rescanTimer->setSingleShot(true);
    m_rescanTimer->setInterval(250);  // debounce udev burst
    connect(m_rescanTimer, &QTimer::timeout, this, &EvdevEncoderManager::onInputDirChanged);
}

EvdevEncoderManager::~EvdevEncoderManager()
{
    stop();
}

void EvdevEncoderManager::start()
{
    if (!m_watcher) {
        m_watcher = new QFileSystemWatcher(this);
        m_watcher->addPath(QStringLiteral("/dev/input"));
        connect(m_watcher, &QFileSystemWatcher::directoryChanged,
                this, [this](const QString&) { m_rescanTimer->start(); });
    }
    onInputDirChanged();
}

void EvdevEncoderManager::stop()
{
    closeFd();
    if (m_watcher) {
        m_watcher->deleteLater();
        m_watcher = nullptr;
    }
}

void EvdevEncoderManager::onInputDirChanged()
{
    if (m_fd >= 0) return;  // already attached
    const QString path = findMatchingDevice();
    if (path.isEmpty()) return;
    if (openAndGrab(path)) {
        qCInfo(lcDevices) << "EvdevEncoderManager: attached"
                          << m_deviceName << "at" << m_devicePath;
        emit connectionChanged(true, m_deviceName);
    }
}

QString EvdevEncoderManager::findMatchingDevice() const
{
    QDir dir(QStringLiteral("/dev/input"));
    const auto entries = dir.entryList({QStringLiteral("event*")}, QDir::System);
    for (const QString& name : entries) {
        const QString path = dir.filePath(name);
        int fd = ::open(path.toLocal8Bit().constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        char devName[256] = {};
        if (ioctl(fd, EVIOCGNAME(sizeof(devName) - 1), devName) >= 0) {
            const QString s = QString::fromUtf8(devName);
            for (const QString& pattern : supportedNames()) {
                if (s == pattern) {
                    ::close(fd);
                    return path;
                }
            }
        }
        ::close(fd);
    }
    return {};
}

bool EvdevEncoderManager::openAndGrab(const QString& path)
{
    int fd = ::open(path.toLocal8Bit().constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        qCWarning(lcDevices) << "EvdevEncoderManager: open failed"
                             << path << std::strerror(errno);
        return false;
    }
    char devName[256] = {};
    ioctl(fd, EVIOCGNAME(sizeof(devName) - 1), devName);
    m_deviceName = QString::fromUtf8(devName);
    m_devicePath = path;

    int grab = 1;
    if (ioctl(fd, EVIOCGRAB, &grab) < 0) {
        // Not fatal: events still flow, they just also reach the focused
        // window.  Worth a warning so users know the global-hotkey
        // pollution will be present.
        qCWarning(lcDevices) << "EvdevEncoderManager: EVIOCGRAB failed —"
                             << "events will leak to focused window:"
                             << std::strerror(errno);
    }

    m_fd = fd;
    m_notifier = new QSocketNotifier(fd, QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated,
            this, &EvdevEncoderManager::onReadable);
    return true;
}

void EvdevEncoderManager::closeFd()
{
    if (m_notifier) {
        m_notifier->setEnabled(false);
        m_notifier->deleteLater();
        m_notifier = nullptr;
    }
    if (m_fd >= 0) {
        int grab = 0;
        ioctl(m_fd, EVIOCGRAB, &grab);
        ::close(m_fd);
        m_fd = -1;
        const QString name = m_deviceName;
        m_deviceName.clear();
        m_devicePath.clear();
        m_ctrlDown = false;
        m_lastNonModKey = -1;
        m_prevsongAlongsideCtrl = false;
        if (!name.isEmpty())
            emit connectionChanged(false, name);
    }
}

void EvdevEncoderManager::onReadable()
{
    if (m_fd < 0) return;
    struct input_event ev[32];
    while (true) {
        ssize_t n = ::read(m_fd, ev, sizeof(ev));
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            // ENODEV typically means the BT dial disconnected.
            if (errno == ENODEV) {
                qCInfo(lcDevices) << "EvdevEncoderManager: device removed";
                closeFd();
                m_rescanTimer->start();  // wait for hot-plug re-add
                return;
            }
            qCWarning(lcDevices) << "EvdevEncoderManager: read err"
                                 << std::strerror(errno);
            closeFd();
            return;
        }
        if (n == 0) break;
        const int count = static_cast<int>(n / sizeof(input_event));
        for (int i = 0; i < count; ++i) {
            const struct input_event& e = ev[i];
            if (e.type != EV_KEY) continue;

            const int keycode = e.code;
            const int value   = e.value;  // 0 = release, 1 = press, 2 = autorepeat

            // Rotary: emit a tick per press or autorepeat tick.  The
            // ~30 Hz autorepeat rate from the dial firmware gives a
            // natural acceleration feel under continuous rotation.
            if (keycode == kRotaryCwKey) {
                if (value == 1 || value == 2) emit tuneSteps(+1);
                continue;
            }
            if (keycode == kRotaryCcwKey) {
                if (value == 1 || value == 2) emit tuneSteps(-1);
                continue;
            }

            // Modifier state for chord assembly.
            if (keycode == KEY_LEFTCTRL || keycode == KEY_RIGHTCTRL) {
                m_ctrlDown = (value != 0);
                if (!m_ctrlDown) {
                    // Ctrl released — chord window closing.  Reset
                    // compound flags so the next chord starts clean.
                    m_lastNonModKey = -1;
                    m_prevsongAlongsideCtrl = false;
                }
                continue;
            }

            // PREVIOUSSONG can arrive alongside a Ctrl chord (Mode Cycle
            // emits Ctrl+Y+KEY_PREVIOUSSONG as a compound).  Track that
            // it was present during a chord window so we can emit the
            // compound signature.
            if (keycode == KEY_PREVIOUSSONG && m_ctrlDown && value == 1) {
                m_prevsongAlongsideCtrl = true;
                continue;
            }
            if (keycode == KEY_PREVIOUSSONG && m_ctrlDown && value == 0) {
                // chord-window release; handled when Ctrl + non-mod release
                continue;
            }

            // Bare key press/release (no Ctrl).
            if (!m_ctrlDown) {
                if (value == 1 || value == 0) {
                    emit buttonEvent(bareKeySignature(keycode), value);
                }
                continue;
            }

            // Ctrl is down — this is a chord.
            if (value == 1) {
                m_lastNonModKey = keycode;
                emit buttonEvent(chordSignature(keycode, m_prevsongAlongsideCtrl), 1);
            } else if (value == 0 && keycode == m_lastNonModKey) {
                emit buttonEvent(chordSignature(keycode, m_prevsongAlongsideCtrl), 0);
                m_lastNonModKey = -1;
                m_prevsongAlongsideCtrl = false;
            }
        }
    }
}

} // namespace AetherSDR

#endif // Q_OS_LINUX
