#ifdef HAVE_SERIALPORT

#include "FlexControlManager.h"
#include "LogManager.h"

#include <QSerialPortInfo>
#include <QDebug>

namespace AetherSDR {

FlexControlManager::FlexControlManager(QObject* parent)
    : QObject(parent)
{
    // Set this as parent so moveToThread() moves m_port with us.
    // Without this, m_port stays on the creating thread, causing
    // cross-thread QObject access that silently fails on macOS.
    m_port.setParent(this);
    connect(&m_port, &QSerialPort::readyRead, this, &FlexControlManager::onReadyRead);
    // Without this the driver had NO error path at all: a port that dropped
    // stayed "open" from Qt's point of view, readyRead simply never fired
    // again, and nothing retried — the knob was dead until the process (or on
    // Windows, the machine) restarted. Every other device driver in the tree
    // handles this; see AcomConnection's serial branch and MidiControlManager's
    // hotplug timer. (#4574)
    connect(&m_port, &QSerialPort::errorOccurred,
            this, &FlexControlManager::handlePortError);

    m_reconnectTimer.setInterval(kReconnectIntervalMs);
    connect(&m_reconnectTimer, &QTimer::timeout, this, [this] {
        if (m_wantedPort.isEmpty()) {          // nothing wanted — stop retrying
            m_reconnectTimer.stop();
            return;
        }
        if (m_port.isOpen()) {                 // recovered by another path
            m_reconnectTimer.stop();
            return;
        }
        // Re-detect rather than reusing the old name: a USB re-enumeration can
        // hand the device a different COM port than it had before.
        const QString port = detectPort();
        if (port.isEmpty()) return;            // device still absent; keep trying
        qCDebug(lcDevices) << "FlexControlManager: retrying" << port;
        if (open(port)) {
            m_reconnectTimer.stop();
            qCInfo(lcDevices) << "FlexControlManager: reconnected on" << port;
        }
    });
}

FlexControlManager::~FlexControlManager()
{
    close();
}

QString FlexControlManager::detectPort()
{
    for (const auto& info : QSerialPortInfo::availablePorts()) {
        if (info.vendorIdentifier() == VendorId &&
            info.productIdentifier() == ProductId)
            return info.portName();
    }
    return {};
}

bool FlexControlManager::open(const QString& portName)
{
    if (m_port.isOpen()) close();

    // Recorded BEFORE the open attempt: a connection is now wanted, so a
    // failure here arms the retry rather than giving up. close() clears it.
    m_wantedPort = portName;
    m_deliberateDisconnect = false;
    // A previous error can leave the port latched; clear it or Qt may refuse
    // to reopen the same object.
    m_port.clearError();

    m_port.setPortName(portName);
    m_port.setBaudRate(9600);
    m_port.setDataBits(QSerialPort::Data8);
    m_port.setParity(QSerialPort::NoParity);
    m_port.setStopBits(QSerialPort::OneStop);
    m_port.setFlowControl(QSerialPort::NoFlowControl);

    if (!m_port.open(QIODevice::ReadWrite)) {
        qCWarning(lcDevices) << "FlexControlManager: failed to open" << portName
                   << m_port.errorString();
        // Keep trying: the device may be mid-enumeration, or another process
        // may still be releasing it.
        armReconnect();
        return false;
    }

    m_reconnectTimer.stop();   // connected — stop retrying
    m_buffer.clear();
    qCDebug(lcDevices) << "FlexControlManager: opened" << portName;
    writeLedState();
    emit connectionChanged(true);
    return true;
}

void FlexControlManager::handlePortError(QSerialPort::SerialPortError error)
{
    if (error == QSerialPort::NoError) return;
    // ResourceError / DeviceNotFound are the USB-went-away cases; the rest
    // (permission, framing, timeout) are treated the same way because the
    // recovery is identical and re-detecting a healthy device is cheap.
    qCWarning(lcDevices) << "FlexControlManager: serial error" << error
                         << m_port.errorString() << "— releasing port and retrying";
    releasePort();
    armReconnect();
}

void FlexControlManager::releasePort()
{
    const bool wasOpen = m_port.isOpen();
    // clearError() first: on an errored port Qt can refuse further operations
    // until the error state is reset, which is exactly how the handle used to
    // be leaked.
    m_port.clearError();
    if (wasOpen) m_port.close();
    m_buffer.clear();
    if (wasOpen) emit connectionChanged(false);
}

void FlexControlManager::armReconnect()
{
    if (m_wantedPort.isEmpty()) return;      // no connection wanted
    if (m_deliberateDisconnect) return;      // operator asked for this
    if (!m_reconnectTimer.isActive()) m_reconnectTimer.start();
}

void FlexControlManager::close()
{
    // Operator-initiated: stop wanting a connection, so the retry timer does
    // not immediately undo this.
    m_deliberateDisconnect = true;
    m_wantedPort.clear();
    m_reconnectTimer.stop();

    // NOT an early return on !isOpen().
    //
    // The old code returned immediately when the port did not report itself
    // open, which is the state an errored port can be left in — so m_port.close()
    // never ran and the OS handle was never released. On Windows that meant a
    // relaunched AetherSDR could not reopen a port the previous process still
    // held, which is a plausible reason a reporter needs a full reboot rather
    // than an app restart. (#4574)
    if (!m_port.isOpen()) {
        // Still clear any latched error so a later open() on this object is not
        // refused by Qt, and drop any partial frame.
        m_port.clearError();
        m_buffer.clear();
        m_deliberateDisconnect = false;
        return;
    }
    // If this write fails it calls releasePort(), which closes the port and
    // emits connectionChanged(false) itself; the guard below then avoids a
    // second, duplicate emission. armReconnect() is a no-op on this path
    // because m_wantedPort is already cleared and m_deliberateDisconnect is set.
    writeLedCommand(0);
    if (m_port.isOpen()) {
        m_port.waitForBytesWritten(50);
        m_port.close();
        m_port.clearError();
        m_buffer.clear();
        qCDebug(lcDevices) << "FlexControlManager: closed";
        emit connectionChanged(false);
    }
    m_deliberateDisconnect = false;
}

void FlexControlManager::setActiveLedButton(int button)
{
    if (button < 1 || button > 3) {
        button = 0;
    }
    if (m_activeLedButton == button) {
        return;
    }
    m_activeLedButton = button;
    writeLedState();
}

void FlexControlManager::onReadyRead()
{
    m_buffer.append(m_port.readAll());

    // Process all complete commands (semicolon-delimited)
    int idx;
    while ((idx = m_buffer.indexOf(';')) >= 0) {
        QByteArray cmd = m_buffer.left(idx).trimmed();
        m_buffer.remove(0, idx + 1);
        if (!cmd.isEmpty())
            processCommand(cmd);
    }

    // Cap buffer to prevent runaway growth from garbage data
    if (m_buffer.size() > 256)
        m_buffer.clear();
}

void FlexControlManager::processCommand(const QByteArray& cmd)
{
    if (cmd.startsWith('D')) {
        // Clockwise rotation: D (1 step), D02–D06 (accelerated)
        int accel = 1;
        if (cmd.size() > 1)
            accel = std::max(1, cmd.mid(1).toInt());
        emit tuneSteps(m_invertDirection ? accel : -accel);

    } else if (cmd.startsWith('U')) {
        // Counter-clockwise rotation: U (1 step), U02–U06 (accelerated)
        int accel = 1;
        if (cmd.size() > 1)
            accel = std::max(1, cmd.mid(1).toInt());
        emit tuneSteps(m_invertDirection ? -accel : accel);

    } else if (cmd.startsWith('X') && cmd.size() >= 3) {
        // Side-button press: X<button><action>
        //   button: 1, 2, 3 (side buttons)
        //   action: S=tap(0), C=double-tap(1), L=hold(2)
        int button = cmd.at(1) - '0';
        if (button < 1 || button > 4) return;
        char action = cmd.at(2);
        int actionId = (action == 'S') ? 0 : (action == 'C') ? 1 : 2;
        emit buttonPressed(button, actionId);

    } else if (cmd.size() == 1 && (cmd == "S" || cmd == "C" || cmd == "L")) {
        // Knob press: bare S/C/L token (no X-prefix). Confirmed via FlexControl
        // hardware capture (#2263). The knob is button 4 in our action-dropdown
        // layout, matching the pre-existing X4S/X4C/X4L slot.
        int actionId = (cmd == "S") ? 0 : (cmd == "C") ? 1 : 2;
        emit buttonPressed(4, actionId);

    } else if (cmd.startsWith('F')) {
        // Init/reset (e.g. F0304;) — device just cleared its hardware state,
        // including the Aux LEDs. Re-issue our cached LED state so the
        // hardware matches the application's active wheel-mode button. This
        // covers race-windows where our open()'s writeLedState() arrives at
        // the device before its own power-on reset completes (#2908).
        qCDebug(lcDevices) << "FlexControlManager: device reset" << cmd
                           << "— restoring LED state";
        writeLedState();
    }
}

void FlexControlManager::writeLedState()
{
    writeLedCommand(m_activeLedButton);
}

void FlexControlManager::writeLedCommand(int button)
{
    if (!m_port.isOpen() || !m_port.isWritable()) {
        return;
    }

    QByteArray cmd("I000;");
    if (button >= 1 && button <= 3) {
        cmd[button] = '1';
    }

    const qint64 written = m_port.write(cmd);
    if (written != cmd.size()) {
        qCWarning(lcDevices) << "FlexControlManager: failed to write LED command"
                             << cmd << m_port.errorString();
        // A short write means the port is already gone. This used to only warn,
        // so a device that died during a write stayed dead. Route it into the
        // same recovery as a read error. (#4574)
        //
        // Not called when close() is writing the LEDs off on its way out —
        // m_deliberateDisconnect suppresses the retry in armReconnect().
        releasePort();
        armReconnect();
        return;
    }
    qCDebug(lcDevices) << "FlexControlManager: LED command" << cmd;
    m_port.flush();
}

} // namespace AetherSDR

#endif // HAVE_SERIALPORT
