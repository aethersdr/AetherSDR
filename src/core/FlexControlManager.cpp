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

    m_port.setPortName(portName);
    m_port.setBaudRate(9600);
    m_port.setDataBits(QSerialPort::Data8);
    m_port.setParity(QSerialPort::NoParity);
    m_port.setStopBits(QSerialPort::OneStop);
    m_port.setFlowControl(QSerialPort::NoFlowControl);

    if (!m_port.open(QIODevice::ReadWrite)) {
        qCWarning(lcDevices) << "FlexControlManager: failed to open" << portName
                   << m_port.errorString();
        return false;
    }

    m_buffer.clear();
    qCDebug(lcDevices) << "FlexControlManager: opened" << portName;
    writeLedState();
    emit connectionChanged(true);
    return true;
}

void FlexControlManager::close()
{
    if (!m_port.isOpen()) {
        return;
    }
    writeLedCommand(0);
    m_port.waitForBytesWritten(50);
    m_port.close();
    m_buffer.clear();
    qCDebug(lcDevices) << "FlexControlManager: closed";
    emit connectionChanged(false);
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
        // Init/reset — log and ignore
        qCDebug(lcDevices) << "FlexControlManager: device reset" << cmd;
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
        return;
    }
    qCDebug(lcDevices) << "FlexControlManager: LED command" << cmd;
    m_port.flush();
}

} // namespace AetherSDR

#endif // HAVE_SERIALPORT
