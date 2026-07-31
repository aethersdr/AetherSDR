#pragma once

#ifdef HAVE_SERIALPORT

#include <QObject>
#include <QSerialPort>
#include <QByteArray>
#include <QTimer>

#include <atomic>

namespace AetherSDR {

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
//   I100;   — host sets Aux1 LED on (Ixyz; = Aux1/Aux2/Aux3)
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

    // True while the driver is retrying a connection it still wants. Exposed so
    // the recovery behaviour is observable without a FlexControl attached —
    // "is it still trying?" is otherwise indistinguishable from "it gave up",
    // which is precisely the bug in #4574.
    //
    // Mirrored into an atomic rather than reading m_reconnectTimer.isActive():
    // the manager lives on the ExtControllers worker thread, so a GUI-thread
    // caller (a "Reconnecting…" indicator, say) would otherwise be racing
    // QTimer's internals.
    bool isReconnecting() const { return m_reconnecting.load(std::memory_order_relaxed); }

    void setInvertDirection(bool invert) { m_invertDirection = invert; }
    void setActiveLedButton(int button);

    static constexpr quint16 VendorId  = 0x2192;
    static constexpr quint16 ProductId = 0x0010;

    // Retry cadence after the port drops. 2 s is between AcomConnection's
    // reconnect and MidiControlManager's 5 s hotplug scan: the knob is a
    // hands-on control, so a slow recovery is felt, and re-detection is just a
    // QSerialPortInfo enumeration.
    static constexpr int kReconnectIntervalMs = 2000;
    // ...but a device that is present and permanently un-openable (Linux user
    // not in `dialout`, port held by another process) would otherwise re-scan
    // and log forever at that rate. Back off towards a slow poll so the
    // steady-state cost is bounded; the first few retries stay fast, which is
    // the case that matters for a replug.
    static constexpr int kMaxReconnectIntervalMs = 30000;

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
    void writeLedState();
    void writeLedCommand(int button);

    // A serial error means the port is gone (USB drop, driver reset, the host
    // reclaiming the device). Tear the handle down and start retrying so the
    // knob comes back by itself. (#4574)
    void handlePortError(QSerialPort::SerialPortError error);
    // Release the OS handle without the LED write / connectionChanged that
    // close() performs — used when the port is already in an error state and
    // writing to it would be pointless.
    void releasePort();
    void armReconnect();
    void stopReconnect();
    void retryOnce();
    void backOff();

    QSerialPort m_port;
    QByteArray  m_buffer;
    bool        m_invertDirection{false};
    int         m_activeLedButton{0};

    // Reconnect state. Mirrors AcomConnection's serial recovery: a repeating
    // timer re-detects the device while a connection is WANTED, and an
    // operator-initiated close() suppresses it so Disconnect stays disconnected.
    //
    // m_reconnectTimer is parented to `this` in the constructor for the same
    // reason m_port is: MainWindow moves this object to the ExtControllers
    // thread, and an unparented QTimer member would stay behind on the GUI
    // thread, where start() from the worker is silently refused. (#4574)
    QTimer  m_reconnectTimer;
    QString m_wantedPort;              // empty = no connection wanted
    int     m_retryIntervalMs{kReconnectIntervalMs};
    bool    m_retryFailureLogged{false};   // warn once per outage, not per tick
    bool    m_releasing{false};            // releasePort() re-entrancy guard
    std::atomic<bool> m_reconnecting{false};
};

} // namespace AetherSDR

#endif // HAVE_SERIALPORT
