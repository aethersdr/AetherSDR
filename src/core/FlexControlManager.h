#pragma once

#ifdef HAVE_SERIALPORT

#include <QObject>
#include <QSerialPort>
#include <QByteArray>

namespace MasterSDR {

// FlexControl USB tuning knob driver.
//
// The FlexControl is a USB serial device (VID 0x2192, PID 0x0010) at
// 9600 8N1 that sends semicolon-delimited ASCII commands:
//   D;      — rotate CW (tune up 1 step)
//   D02;–D06; — rotate CW with acceleration (2–6 steps)
//   U;      — rotate CCW (tune down 1 step)
//   U02;–U06; — rotate CCW with acceleration
//   X1S;    — button 1 tap (S=tap, C=double, L=hold)
//   X2S;    — button 2 tap
//   X3S;    — button 3 tap
//   X4S;    — knob button tap
//   F0304;  — device init/reset
class FlexControlManager : public QObject {
    Q_OBJECT

public:
    explicit FlexControlManager(QObject* parent = nullptr);
    ~FlexControlManager() override;

    bool open(const QString& portName);
    void close();
    bool isOpen() const { return m_port.isOpen(); }
    QString portName() const { return m_port.portName(); }

    // Scan QSerialPortInfo for VID 0x2192, PID 0x0010
    static QString detectPort();

    void setInvertDirection(bool invert) { m_invertDirection = invert; }

    static constexpr quint16 VendorId  = 0x2192;
    static constexpr quint16 ProductId = 0x0010;

signals:
    // +N = tune up N steps, -N = tune down N steps
    void tuneSteps(int steps);
    // button 1-4, action: 0=tap, 1=double-tap, 2=hold
    void buttonPressed(int button, int action);
    void connectionChanged(bool connected);

private slots:
    void onReadyRead();

private:
    void processCommand(const QByteArray& cmd);

    QSerialPort m_port;
    QByteArray  m_buffer;
    bool        m_invertDirection{false};
};

} // namespace MasterSDR

#endif // HAVE_SERIALPORT
