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

    // Parent the timer for exactly the reason m_port is parented above, and it
    // matters more here: MainWindow does
    //     m_flexControl = new FlexControlManager;
    //     m_flexControl->moveToThread(m_extCtrlThread);
    // (MainWindow_Controllers.cpp). An unparented QTimer member is not a child,
    // so moveToThread() leaves it on the GUI thread — and QTimer::start() from
    // the worker thread is then silently refused, so the whole recovery below
    // never runs. MidiControlManager avoids this by allocating its hotplug
    // timer as `new QTimer(this)`. (#4574)
    m_reconnectTimer.setParent(this);
    m_reconnectTimer.setInterval(m_retryIntervalMs);
    connect(&m_reconnectTimer, &QTimer::timeout, this, &FlexControlManager::retryOnce);
}

void FlexControlManager::retryOnce()
{
    if (m_wantedPort.isEmpty()) {          // nothing wanted — stop retrying
        stopReconnect();
        return;
    }
    if (m_port.isOpen()) {                 // recovered by another path
        stopReconnect();
        return;
    }
    // Re-detect rather than reusing the old name: a USB re-enumeration can
    // hand the device a different COM port than it had before.
    const QString port = detectPort();
    if (port.isEmpty()) {                  // device still absent; keep trying
        backOff();
        return;
    }
    qCDebug(lcDevices) << "FlexControlManager: retrying" << port;
    if (open(port))
        qCInfo(lcDevices) << "FlexControlManager: reconnected on" << port;
    else
        backOff();
}

// Widen the retry interval towards kMaxReconnectIntervalMs. The first retries
// stay at 2 s because that is the case worth being fast for (a replug); a
// device that never opens settles into a 30 s poll instead of scanning and
// warning every 2 s for the rest of the session.
void FlexControlManager::backOff()
{
    if (m_retryIntervalMs >= kMaxReconnectIntervalMs)
        return;
    m_retryIntervalMs = qMin(m_retryIntervalMs * 2, kMaxReconnectIntervalMs);
    m_reconnectTimer.setInterval(m_retryIntervalMs);
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
        // Warn once per outage. handlePortError() has already logged the real
        // errorString() (it runs re-entrantly from inside m_port.open()), and
        // repeating it on every retry tick is what turns a missing `dialout`
        // group into a log full of the same line.
        if (!m_retryFailureLogged) {
            qCWarning(lcDevices) << "FlexControlManager: failed to open" << portName
                       << "— will keep retrying";
            m_retryFailureLogged = true;
        }
        // Keep trying: the device may be mid-enumeration, or another process
        // may still be releasing it.
        armReconnect();
        return false;
    }

    m_buffer.clear();
    qCDebug(lcDevices) << "FlexControlManager: opened" << portName;
    writeLedState();
    // writeLedState() -> writeLedCommand() routes a short write into
    // releasePort(), which closes the port and emits connectionChanged(false).
    // Don't then announce a connection we no longer have: a device that is
    // present but wedged would otherwise leave AetherControl showing
    // "Connected" with no open port, and hand the caller a bogus true.
    if (!m_port.isOpen())
        return false;

    stopReconnect();   // connected — stop retrying, reset the backoff
    emit connectionChanged(true);
    return true;
}

void FlexControlManager::handlePortError(QSerialPort::SerialPortError error)
{
    if (error == QSerialPort::NoError) return;
    // ResourceError / DeviceNotFound are the USB-went-away cases; the rest
    // (permission, framing, timeout) are treated the same way because the
    // recovery is identical and re-detecting a healthy device is cheap.
    //
    // Logged once per outage rather than once per retry: this also fires
    // re-entrantly from inside m_port.open(), so an un-openable device would
    // otherwise emit this line on every tick for the rest of the session.
    if (!m_retryFailureLogged) {
        qCWarning(lcDevices) << "FlexControlManager: serial error" << error
                             << m_port.errorString() << "— releasing port and retrying";
        m_retryFailureLogged = true;
    }
    releasePort();
    armReconnect();
}

void FlexControlManager::releasePort()
{
    // Re-entrancy guard: m_port.close() can itself set an error, which lands
    // back here through errorOccurred while we are still inside this function.
    if (m_releasing) return;
    m_releasing = true;

    const bool wasOpen = m_port.isOpen();
    // clearError() first: on an errored port Qt can refuse further operations
    // until the error state is reset.
    m_port.clearError();
    if (wasOpen) m_port.close();
    m_buffer.clear();

    m_releasing = false;
    if (wasOpen) emit connectionChanged(false);
}

void FlexControlManager::armReconnect()
{
    // m_wantedPort is the single source of truth for "a connection is wanted".
    // close() clears it, which is what makes a deliberate Disconnect stick.
    if (m_wantedPort.isEmpty()) return;
    if (!m_reconnectTimer.isActive()) m_reconnectTimer.start();
    // Mirror the timer's ACTUAL state, not the intent to start it. If start()
    // is ever refused — a QTimer left on the wrong thread is exactly how — then
    // isReconnecting() must report false rather than claim a recovery that is
    // not running. Storing `true` here unconditionally would make the
    // thread-affinity case below untestable. (#4574)
    m_reconnecting.store(m_reconnectTimer.isActive(), std::memory_order_relaxed);
}

void FlexControlManager::stopReconnect()
{
    m_reconnectTimer.stop();
    m_reconnecting.store(false, std::memory_order_relaxed);
    m_retryIntervalMs = kReconnectIntervalMs;
    m_reconnectTimer.setInterval(m_retryIntervalMs);
    m_retryFailureLogged = false;
}

void FlexControlManager::close()
{
    // Operator-initiated: stop wanting a connection, so the retry timer does
    // not immediately undo this. Callers must reach this unconditionally —
    // gating on isOpen() would make Disconnect unreachable after a port drop
    // and leave m_wantedPort set, so the driver would reclaim a device the
    // operator had switched off. (#4574)
    m_wantedPort.clear();
    stopReconnect();

    if (!m_port.isOpen()) {
        // Nothing to release: QSerialPort has already given the OS handle back
        // — isOpen() only goes false once QSerialPortPrivate::close() has run.
        // Still clear any latched error so a later open() on this object is not
        // refused by Qt, and drop any partial frame.
        m_port.clearError();
        m_buffer.clear();
        return;
    }
    // If this write fails it calls releasePort(), which closes the port and
    // emits connectionChanged(false) itself; the guard below then avoids a
    // second, duplicate emission. armReconnect() is a no-op on this path
    // because m_wantedPort has already been cleared.
    writeLedCommand(0);
    if (m_port.isOpen()) {
        m_port.waitForBytesWritten(50);
        m_port.close();
        m_port.clearError();
        m_buffer.clear();
        qCDebug(lcDevices) << "FlexControlManager: closed";
        emit connectionChanged(false);
    }
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
        // Harmless when close() is writing the LEDs off on its way out:
        // m_wantedPort is already cleared by then, so armReconnect() no-ops.
        releasePort();
        armReconnect();
        return;
    }
    qCDebug(lcDevices) << "FlexControlManager: LED command" << cmd;
    m_port.flush();
}

} // namespace AetherSDR

#endif // HAVE_SERIALPORT
