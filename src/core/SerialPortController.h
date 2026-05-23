#pragma once

#include <QObject>
#include <QString>

#if defined(HAVE_SERIALPORT) || defined(Q_OS_WIN)
#include <QElapsedTimer>
#endif
#ifdef HAVE_SERIALPORT
#ifndef Q_OS_WIN
#include <QSerialPort>
#include <QTimer>
#endif
#endif

#ifdef Q_OS_WIN
#include <atomic>
#include <QThread>
#endif

namespace MasterSDR {

// Controls DTR/RTS lines on a USB-serial adapter for hardware PTT
// and CW keying, and monitors CTS/DSR for external PTT and CW key/paddle input.
//
// Output: Assert DTR/RTS when transmitting (amplifier keying, sequencer)
// Input:  Monitor CTS/DSR for foot switch PTT, straight key, or iambic paddle
//
// On Windows, uses Win32 WaitCommEvent directly — QSerialPort::pinoutSignals()
// (backed by GetCommModemStatus) returns stale data on FTDI and similar drivers
// unless called in the context of a WaitCommEvent completion.
//
// On Linux/macOS, polls QSerialPort::pinoutSignals() on a timer.
//
// Requires Qt6::SerialPort on non-Windows. Compiles to a no-op stub without
// HAVE_SERIALPORT on non-Windows platforms.

class SerialPortController : public QObject {
    Q_OBJECT

public:
    // Output functions (DTR/RTS)
    enum class PinFunction { None, PTT, CwKey, CwPTT };
    // Input functions (CTS/DSR)
    enum class InputFunction { None, PttInput, CwKeyInput, CwDitInput, CwDahInput };

    explicit SerialPortController(QObject* parent = nullptr);
    ~SerialPortController() override;

    bool open(const QString& portName, int baudRate = 9600,
              int dataBits = 8, int parity = 0, int stopBits = 1);
    void close();
    bool isOpen() const;
    QString portName() const;

    // Output pin config (DTR/RTS)
    void setDtrFunction(PinFunction fn) { m_dtrFn = fn; }
    void setRtsFunction(PinFunction fn) { m_rtsFn = fn; }
    void setDtrPolarity(bool activeHigh) { m_dtrActiveHigh = activeHigh; }
    void setRtsPolarity(bool activeHigh) { m_rtsActiveHigh = activeHigh; }

    PinFunction dtrFunction() const { return m_dtrFn; }
    PinFunction rtsFunction() const { return m_rtsFn; }
    bool dtrPolarity() const { return m_dtrActiveHigh; }
    bool rtsPolarity() const { return m_rtsActiveHigh; }

    // Input pin config (CTS/DSR)
    void setCtsFunction(InputFunction fn) { m_ctsFn = fn; updatePolling(); }
    void setDsrFunction(InputFunction fn) { m_dsrFn = fn; updatePolling(); }
    void setCtsPolarity(bool activeHigh) { m_ctsActiveHigh = activeHigh; }
    void setDsrPolarity(bool activeHigh) { m_dsrActiveHigh = activeHigh; }

    InputFunction ctsFunction() const { return m_ctsFn; }
    InputFunction dsrFunction() const { return m_dsrFn; }
    bool ctsPolarity() const { return m_ctsActiveHigh; }
    bool dsrPolarity() const { return m_dsrActiveHigh; }

    // Paddle swap (swap dit/dah without rewiring)
    void setPaddleSwap(bool swap) { m_paddleSwap = swap; }
    bool paddleSwap() const { return m_paddleSwap; }

    // Load/save configuration from AppSettings
    void loadSettings();
    void saveSettings();

public slots:
    void setTransmitting(bool tx);
    void setCwKeyDown(bool down);

signals:
    void externalPttChanged(bool active);
    void cwKeyChanged(bool down);                   // straight key
    void cwPaddleChanged(bool dit, bool dah);       // iambic paddle
    void errorOccurred(const QString& msg);

private:
    void applyPin(PinFunction targetFn, bool active);
    void updatePolling();

    // Output state
    PinFunction m_dtrFn{PinFunction::None};
    PinFunction m_rtsFn{PinFunction::None};
    bool m_dtrActiveHigh{true};
    bool m_rtsActiveHigh{true};

    // Input state
    InputFunction m_ctsFn{InputFunction::None};
    InputFunction m_dsrFn{InputFunction::None};
    bool m_ctsActiveHigh{false};
    bool m_dsrActiveHigh{false};
    bool m_paddleSwap{false};

#if defined(HAVE_SERIALPORT) || defined(Q_OS_WIN)
    // Shared input tracking state (always accessed on this object's thread)
    bool          m_lastCtsActive{false};
    bool          m_lastDsrActive{false};
    bool          m_lastKeyDown{false};
    bool          m_lastDitActive{false};
    bool          m_lastDahActive{false};
    QElapsedTimer m_debounceTimer;
    static constexpr int DEBOUNCE_MS = 20;
#endif

// On Windows, HAVE_SERIALPORT is typically defined (Qt6::SerialPort ships for Windows),
// but the QSerialPort code path below is NOT used — the Win32 WaitCommEvent path takes over.
// The #ifdef HAVE_SERIALPORT guards in dependent files (e.g. MainWindow) are correct.
#ifdef Q_OS_WIN
    // Win32 path: owns the COM port handle exclusively for WaitCommEvent-based detection
    void*         m_hWin{nullptr};      // Win32 HANDLE cast to void*; nullptr = closed
    QString       m_winPortName;
    int           m_winBaudRate{0};
    QThread*      m_winWatchThread{nullptr};
    std::atomic<bool> m_stopWinWatch{false};

    void runWinWatcher();

private slots:
    void processWinPinChange(bool dsrRaw, bool ctsRaw);

#elif defined(HAVE_SERIALPORT)
    // Non-Windows path: poll via QSerialPort timer
    QSerialPort   m_port;
    QTimer        m_pollTimer;
    bool          m_pollLogged{false};
    // Diagnostic state for pollInputPins() logging — kept as members (not statics)
    // so a hypothetical second instance doesn't share state.
    QSerialPort::PinoutSignals m_lastRawPins{};
    bool                       m_lastDebounceLogged{false};
    static constexpr int POLL_INTERVAL_MS = 10;

private slots:
    void pollInputPins();
#endif
};

} // namespace MasterSDR
