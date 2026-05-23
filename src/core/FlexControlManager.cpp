#ifdef HAVE_SERIALPORT

#include "FlexControlManager.h"
#include "LogManager.h"

#include <QSerialPortInfo>
#include <QDebug>

namespace MasterSDR {

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

    if (!m_port.open(QIODevice::ReadOnly)) {
        qCWarning(lcDevices) << "FlexControlManager: failed to open" << portName
                   << m_port.errorString();
        return false;
    }

    m_buffer.clear();
    qCDebug(lcDevices) << "FlexControlManager: opened" << portName;
    emit connectionChanged(true);
    return true;
}

void FlexControlManager::close()
{
    if (!m_port.isOpen()) return;
    m_port.close();
    m_buffer.clear();
    qCDebug(lcDevices) << "FlexControlManager: closed";
    emit connectionChanged(false);
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

} // namespace MasterSDR

#endif // HAVE_SERIALPORT
