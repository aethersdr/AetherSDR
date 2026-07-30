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

// Read an input device's name from sysfs (e.g. "Ulanzi Dial Keyboard").  The
// sysfs `name` attribute is world-readable, so this works even when we lack
// permission to open the /dev/input/event* node itself — which is exactly how
// we detect a present-but-inaccessible dial.
QString sysfsInputName(const QString& eventNode)
{
    QFile f(QStringLiteral("/sys/class/input/%1/device/name").arg(eventNode));
    if (f.open(QIODevice::ReadOnly))
        return QString::fromUtf8(f.readAll()).trimmed();
    return {};
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
    QString blockedName;
    const QString path = findMatchingDevice(&blockedName);
    if (path.isEmpty()) {
        // A recognized dial is present but we can't open its node — the udev
        // access rule isn't installed.  Surface it once so the UI can offer to
        // install the rule, rather than silently reading as "disconnected".
        if (!blockedName.isEmpty() && !m_accessRequiredEmitted) {
            m_accessRequiredEmitted = true;
            qCWarning(lcDevices)
                << "EvdevEncoderManager:" << blockedName
                << "detected but its /dev/input node is not accessible (EACCES)."
                << "Install the udev access rule (Ulanzi Dial mapper → Grant"
                << "access) or add this user to the 'input' group.";
            emit accessRequired(blockedName);
        }
        return;
    }
    m_accessRequiredEmitted = false;
    if (openAndGrab(path)) {
        qCInfo(lcDevices) << "EvdevEncoderManager: attached"
                          << m_deviceName << "at" << m_devicePath;
        emit connectionChanged(true, m_deviceName);
    }
}

QString EvdevEncoderManager::findMatchingDevice(QString* blockedName) const
{
    QDir dir(QStringLiteral("/dev/input"));
    const auto entries = dir.entryList({QStringLiteral("event*")}, QDir::System);
    for (const QString& name : entries) {
        // Match on the sysfs name first (always readable), so a dial we lack
        // permission to open is still recognized as "present".
        const QString devName = sysfsInputName(name);
        bool supported = false;
        for (const QString& pattern : supportedNames()) {
            if (devName == pattern) { supported = true; break; }
        }
        if (!supported) continue;

        const QString path = dir.filePath(name);
        int fd = ::open(path.toLocal8Bit().constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd >= 0) {
            ::close(fd);
            return path;
        }
        if ((errno == EACCES || errno == EPERM) && blockedName)
            *blockedName = devName;
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
        m_suppressReleaseNonModKey = -1;
        m_suppressReleasePrevsong = false;
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
            if (errno == ENODEV) {
                qCInfo(lcDevices) << "EvdevEncoderManager: device removed";
            } else {
                qCWarning(lcDevices) << "EvdevEncoderManager: read failed on" << m_devicePath
                                     << std::strerror(errno);
            }
            closeFd();
            m_rescanTimer->start();
            return;
        }
        if (n == 0) {
            qCInfo(lcDevices) << "EvdevEncoderManager: EOF on" << m_devicePath;
            closeFd();
            m_rescanTimer->start();
            return;
        }
        size_t count = static_cast<size_t>(n) / sizeof(struct input_event);
        for (size_t i = 0; i < count; ++i) {
            if (ev[i].type != EV_KEY) continue;
            const int keycode = ev[i].code;
            const int value   = ev[i].value;

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
                    // Ctrl released — chord window closing. If a chord key was
                    // held, emit its release event now so listeners don't miss
                    // the release if Ctrl is released before the non-mod key.
                    if (m_lastNonModKey != -1) {
                        emit buttonEvent(chordSignature(m_lastNonModKey, m_prevsongAlongsideCtrl), 0);
                    }
                    m_lastNonModKey = -1;
                    m_prevsongAlongsideCtrl = false;
                }
                continue;
            }

            // PREVIOUSSONG can arrive alongside a Ctrl chord (Mode Cycle
            // emits Ctrl+Y+KEY_PREVIOUSSONG as a compound).
            if (keycode == KEY_PREVIOUSSONG) {
                if (m_ctrlDown) {
                    if (value == 1) {
                        m_prevsongAlongsideCtrl = true;
                        m_suppressReleasePrevsong = true;
                    } else if (value == 0) {
                        m_suppressReleasePrevsong = false;
                    }
                    continue;
                }
                if (m_suppressReleasePrevsong) {
                    if (value == 0) {
                        m_suppressReleasePrevsong = false;
                    } else if (value == 1) {
                        m_suppressReleasePrevsong = false;
                    }
                    if (value == 0) continue; // suppress trailing release from chord
                }
                if (value == 1 || value == 0) {
                    emit buttonEvent(bareKeySignature(keycode), value);
                }
                continue;
            }

            // Bare key press/release (no Ctrl).
            if (!m_ctrlDown) {
                if (keycode == m_suppressReleaseNonModKey) {
                    if (value == 0) {
                        m_suppressReleaseNonModKey = -1;
                    } else if (value == 1) {
                        m_suppressReleaseNonModKey = -1;
                    }
                    if (value == 0) continue; // suppress trailing release from chord
                }
                if (value == 1 || value == 0) {
                    emit buttonEvent(bareKeySignature(keycode), value);
                }
                continue;
            }

            // Ctrl is down — this is a chord.
            if (value == 1) {
                m_lastNonModKey = keycode;
                m_suppressReleaseNonModKey = keycode;
                emit buttonEvent(chordSignature(keycode, m_prevsongAlongsideCtrl), 1);
            } else if (value == 0 && keycode == m_lastNonModKey) {
                emit buttonEvent(chordSignature(keycode, m_prevsongAlongsideCtrl), 0);
                m_lastNonModKey = -1;
                m_prevsongAlongsideCtrl = false;
                m_suppressReleaseNonModKey = -1;
            }
        }
    }
}

} // namespace AetherSDR

#endif // Q_OS_LINUX
