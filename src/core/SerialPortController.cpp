#include "SerialPortController.h"
#include "AppSettings.h"
#include "LogManager.h"

#ifndef Q_OS_WIN
#ifdef HAVE_SERIALPORT
#include <QSerialPortInfo>
#endif
#endif

#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <QThread>
#include <QMetaObject>
#endif

namespace MasterSDR {

SerialPortController::SerialPortController(QObject* parent)
    : QObject(parent)
{
#if defined(HAVE_SERIALPORT) && !defined(Q_OS_WIN)
    // Set this as parent so moveToThread() moves them with us.
    // Without this, m_port and m_pollTimer stay on the creating thread,
    // causing cross-thread QObject access that silently fails on macOS.
    m_port.setParent(this);
    m_pollTimer.setParent(this);
    connect(&m_pollTimer, &QTimer::timeout, this, &SerialPortController::pollInputPins);
#endif
}

SerialPortController::~SerialPortController()
{
    close();
}

// ─────────────────────────────────────────────────────────────────────────────
// Windows implementation — Win32 WaitCommEvent
// ─────────────────────────────────────────────────────────────────────────────
#ifdef Q_OS_WIN

bool SerialPortController::open(const QString& portName, int baudRate,
                                 int dataBits, int parity, int stopBits)
{
    if (isOpen()) close();

    // Prepend "\\.\" so COMxx > COM9 open correctly
    QString devPath = portName.startsWith("COM", Qt::CaseInsensitive)
                    ? "\\\\.\\" + portName : portName;

    // Non-overlapped: WaitCommEvent is called synchronously in the watcher thread,
    // which blocks until a pin change occurs. SetCommMask(0) aborts it on close.
    // Overlapped mode causes silent WaitCommEvent failures on some FTDI VCP drivers.
    HANDLE hPort = ::CreateFileW(
        reinterpret_cast<LPCWSTR>(devPath.utf16()),
        GENERIC_READ | GENERIC_WRITE,
        0,               // no sharing — COM ports are exclusive
        nullptr,
        OPEN_EXISTING,
        0,               // synchronous I/O
        nullptr
    );
    if (hPort == INVALID_HANDLE_VALUE) {
        DWORD err = ::GetLastError();
        qCWarning(lcDevices) << "SerialPortController: failed to open" << portName
                             << "Win32 error" << err;
        emit errorOccurred(QString("Failed to open %1 (error %2)").arg(portName).arg(err));
        return false;
    }

    // Configure baud rate / framing via DCB
    DCB dcb = {};
    dcb.DCBlength = sizeof(DCB);
    if (!::GetCommState(hPort, &dcb)) {
        DWORD err = ::GetLastError();
        qCWarning(lcDevices) << "SerialPortController: GetCommState failed on" << portName
                             << "Win32 error" << err;
        emit errorOccurred(QString("Failed to read port state for %1 (error %2)").arg(portName).arg(err));
        ::CloseHandle(hPort);
        return false;
    }
    dcb.BaudRate        = static_cast<DWORD>(baudRate);
    dcb.ByteSize        = static_cast<BYTE>(dataBits);
    dcb.Parity          = static_cast<BYTE>(parity);
    dcb.StopBits        = (stopBits == 2) ? TWOSTOPBITS : ONESTOPBIT;
    dcb.fBinary         = TRUE;
    dcb.fParity         = (parity != NOPARITY);
    // Software-controlled outputs, start HIGH (Windows default on open)
    dcb.fDtrControl     = DTR_CONTROL_ENABLE;
    dcb.fRtsControl     = RTS_CONTROL_ENABLE;
    dcb.fOutxCtsFlow    = FALSE;
    dcb.fOutxDsrFlow    = FALSE;
    dcb.fDsrSensitivity = FALSE;
    if (!::SetCommState(hPort, &dcb)) {
        DWORD err = ::GetLastError();
        qCWarning(lcDevices) << "SerialPortController: SetCommState failed on" << portName
                             << "Win32 error" << err;
        emit errorOccurred(QString("Failed to configure %1 (baud/framing, error %2)").arg(portName).arg(err));
        ::CloseHandle(hPort);
        return false;
    }

    // Monitor DSR and CTS changes — required for FTDI drivers where
    // GetCommModemStatus only refreshes in a WaitCommEvent completion context.
    ::SetCommMask(hPort, EV_DSR | EV_CTS | EV_RLSD);

    // Deassert PTT/CW output pins that have a function assigned.
    // Pins with no function stay HIGH (OS default), providing source voltage
    // for foot switch circuits that loop an output pin back to DSR/CTS.
    if (m_dtrFn != PinFunction::None)
        ::EscapeCommFunction(hPort, m_dtrActiveHigh ? CLRDTR : SETDTR);
    if (m_rtsFn != PinFunction::None)
        ::EscapeCommFunction(hPort, m_rtsActiveHigh ? CLRRTS : SETRTS);

    // Sample initial pin state
    DWORD modemStat = 0;
    ::GetCommModemStatus(hPort, &modemStat);
    bool cts = (modemStat & MS_CTS_ON) != 0;
    bool dsr = (modemStat & MS_DSR_ON) != 0;
    m_lastCtsActive = m_ctsActiveHigh ? cts : !cts;
    m_lastDsrActive = m_dsrActiveHigh ? dsr : !dsr;
    m_debounceTimer.start();

    m_hWin        = static_cast<void*>(hPort);
    m_winPortName = portName;
    m_winBaudRate = baudRate;

    qCDebug(lcDevices) << "SerialPortController: opened" << portName << "at" << baudRate
                       << "(Win32) DSR=" << dsr << "CTS=" << cts
                       << "dsrFn=" << static_cast<int>(m_dsrFn)
                       << "ctsFn=" << static_cast<int>(m_ctsFn);

    // Start the WaitCommEvent watcher thread
    m_stopWinWatch.store(false);
    m_winWatchThread = QThread::create([this]() { runWinWatcher(); });
    m_winWatchThread->setObjectName("SerialWatcher");
    m_winWatchThread->start();

    return true;
}

void SerialPortController::close()
{
    if (!isOpen()) return;

    HANDLE hPort = static_cast<HANDLE>(m_hWin);

    // Emit PTT release before closing to avoid stuck TX
    bool pttWasActive = (m_ctsFn == InputFunction::PttInput && m_lastCtsActive)
                     || (m_dsrFn == InputFunction::PttInput && m_lastDsrActive);
    if (pttWasActive)
        emit externalPttChanged(false);

    m_lastCtsActive = false;
    m_lastDsrActive = false;

    // Deassert configured output pins
    if (m_dtrFn != PinFunction::None)
        ::EscapeCommFunction(hPort, m_dtrActiveHigh ? CLRDTR : SETDTR);
    if (m_rtsFn != PinFunction::None)
        ::EscapeCommFunction(hPort, m_rtsActiveHigh ? CLRRTS : SETRTS);

    // Stop watcher thread: flag it, then abort the pending WaitCommEvent
    m_stopWinWatch.store(true);
    ::SetCommMask(hPort, 0);    // causes pending WaitCommEvent to return immediately

    if (m_winWatchThread) {
        m_winWatchThread->wait(3000);
        delete m_winWatchThread;
        m_winWatchThread = nullptr;
    }

    ::CloseHandle(hPort);
    m_hWin        = nullptr;
    m_winPortName.clear();
    m_winBaudRate = 0;

    qCDebug(lcDevices) << "SerialPortController: closed (Win32)";
}

bool SerialPortController::isOpen() const
{
    return m_hWin != nullptr;
}

QString SerialPortController::portName() const
{
    return m_winPortName;
}

void SerialPortController::applyPin(PinFunction targetFn, bool active)
{
    if (!isOpen()) return;
    HANDLE hPort = static_cast<HANDLE>(m_hWin);
    if (m_dtrFn == targetFn) {
        bool level = m_dtrActiveHigh ? active : !active;
        ::EscapeCommFunction(hPort, level ? SETDTR : CLRDTR);
    }
    if (m_rtsFn == targetFn) {
        bool level = m_rtsActiveHigh ? active : !active;
        ::EscapeCommFunction(hPort, level ? SETRTS : CLRRTS);
    }
}

void SerialPortController::updatePolling()
{
    // No-op on Windows: detection is event-driven via runWinWatcher()
}

void SerialPortController::runWinWatcher()
{
    HANDLE hPort = static_cast<HANDLE>(m_hWin);

    qCDebug(lcDevices) << "SerialPortController: watcher thread started";

    while (!m_stopWinWatch.load()) {
        DWORD evtMask = 0;
        // Synchronous blocking call — returns when a pin changes or SetCommMask(0) aborts it
        if (!::WaitCommEvent(hPort, &evtMask, nullptr)) {
            DWORD err = ::GetLastError();
            if (err == ERROR_OPERATION_ABORTED || err == ERROR_INVALID_HANDLE) break;
            qCWarning(lcDevices) << "SerialPortController: WaitCommEvent error" << err;
            break;
        }

        if (m_stopWinWatch.load()) break;

        // Read pin state immediately after the event — FTDI only refreshes
        // GetCommModemStatus in the context of a WaitCommEvent completion.
        DWORD modemStat = 0;
        ::GetCommModemStatus(hPort, &modemStat);
        bool dsr = (modemStat & MS_DSR_ON) != 0;
        bool cts = (modemStat & MS_CTS_ON) != 0;

        qCDebug(lcDevices) << "SerialPortController: WaitCommEvent"
                           << Qt::hex << evtMask << "DSR=" << dsr << "CTS=" << cts;

        // Marshal the pin state back to this object's thread
        QMetaObject::invokeMethod(this, [this, dsr, cts]() {
            processWinPinChange(dsr, cts);
        }, Qt::QueuedConnection);
    }

    qCDebug(lcDevices) << "SerialPortController: watcher thread exiting";
}

void SerialPortController::processWinPinChange(bool dsrRaw, bool ctsRaw)
{
    if (!isOpen()) return;
    bool ctsActive = m_ctsActiveHigh ? ctsRaw : !ctsRaw;
    bool dsrActive = m_dsrActiveHigh ? dsrRaw : !dsrRaw;

    bool debounceOk = m_debounceTimer.elapsed() >= DEBOUNCE_MS;

    // ── PTT input ────────────────────────────────────────────────────────
    if (m_ctsFn == InputFunction::PttInput && ctsActive != m_lastCtsActive && debounceOk) {
        qCDebug(lcDevices) << "SerialPortController: CTS PTT edge →" << ctsActive;
        m_lastCtsActive = ctsActive;
        m_debounceTimer.restart();
        emit externalPttChanged(ctsActive);
    }
    if (m_dsrFn == InputFunction::PttInput && dsrActive != m_lastDsrActive && debounceOk) {
        qCDebug(lcDevices) << "SerialPortController: DSR PTT edge →" << dsrActive;
        m_lastDsrActive = dsrActive;
        m_debounceTimer.restart();
        emit externalPttChanged(dsrActive);
    }

    // ── CW straight key ──────────────────────────────────────────────────
    bool keyDown = false;
    bool hasKey = false;
    if (m_ctsFn == InputFunction::CwKeyInput) { keyDown = ctsActive; hasKey = true; }
    if (m_dsrFn == InputFunction::CwKeyInput) { keyDown = dsrActive; hasKey = true; }
    if (hasKey && keyDown != m_lastKeyDown) {
        m_lastKeyDown = keyDown;
        emit cwKeyChanged(keyDown);
    }

    // ── CW paddle (dit/dah) ──────────────────────────────────────────────
    bool ditActive = false, dahActive = false;
    bool hasPaddle = false;
    if (m_ctsFn == InputFunction::CwDitInput) { ditActive = ctsActive; hasPaddle = true; }
    if (m_dsrFn == InputFunction::CwDitInput) { ditActive = dsrActive; hasPaddle = true; }
    if (m_ctsFn == InputFunction::CwDahInput) { dahActive = ctsActive; hasPaddle = true; }
    if (m_dsrFn == InputFunction::CwDahInput) { dahActive = dsrActive; hasPaddle = true; }

    if (m_paddleSwap) std::swap(ditActive, dahActive);

    if (hasPaddle && (ditActive != m_lastDitActive || dahActive != m_lastDahActive)) {
        m_lastDitActive = ditActive;
        m_lastDahActive = dahActive;
        emit cwPaddleChanged(ditActive, dahActive);
    }
}

#else  // ─────────────────────────────────────────────────────────────────────
// Non-Windows implementation — QSerialPort polling
// ─────────────────────────────────────────────────────────────────────────────

bool SerialPortController::open(const QString& portName, int baudRate,
                                 int dataBits, int parity, int stopBits)
{
#ifdef HAVE_SERIALPORT
    if (m_port.isOpen()) close();

    m_port.setPortName(portName);
    m_port.setBaudRate(baudRate);
    m_port.setDataBits(static_cast<QSerialPort::DataBits>(dataBits));
    m_port.setParity(static_cast<QSerialPort::Parity>(parity));
    m_port.setStopBits(static_cast<QSerialPort::StopBits>(stopBits));
    m_port.setFlowControl(QSerialPort::NoFlowControl);

    if (!m_port.open(QIODevice::ReadWrite)) {
        qCWarning(lcDevices) << "SerialPortController: failed to open" << portName
                         << m_port.errorString();
        emit errorOccurred(m_port.errorString());
        return false;
    }

    // Deassert output pins that have a PTT/CW function assigned — prevents spurious TX
    // on open. Pins with no function are left at the OS default (HIGH), which provides
    // the source voltage for foot switch circuits that loop an output back to DSR/CTS.
    if (m_dtrFn != PinFunction::None)
        m_port.setDataTerminalReady(!m_dtrActiveHigh);
    if (m_rtsFn != PinFunction::None)
        m_port.setRequestToSend(!m_rtsActiveHigh);

    // Sample actual pin state so the first poll doesn't fire a spurious PTT event
    {
        auto pinState = m_port.pinoutSignals();
        bool cts = pinState.testFlag(QSerialPort::ClearToSendSignal);
        bool dsr = pinState.testFlag(QSerialPort::DataSetReadySignal);
        m_lastCtsActive = m_ctsActiveHigh ? cts : !cts;
        m_lastDsrActive = m_dsrActiveHigh ? dsr : !dsr;
        qCDebug(lcDevices) << "SerialPortController: open() initial pin sample —"
                           << "raw=" << Qt::hex << static_cast<int>(pinState)
                           << "DSR=" << dsr << "CTS=" << cts
                           << "dsrActiveHigh=" << m_dsrActiveHigh
                           << "ctsActiveHigh=" << m_ctsActiveHigh
                           << "→ lastDsrActive=" << m_lastDsrActive
                           << "lastCtsActive=" << m_lastCtsActive;
    }
    m_pollLogged = false;
    m_debounceTimer.start();

    updatePolling();

    qCDebug(lcDevices) << "SerialPortController: opened" << portName << "at" << baudRate
                       << "dsrFn=" << static_cast<int>(m_dsrFn)
                       << "ctsFn=" << static_cast<int>(m_ctsFn)
                       << "pollActive=" << m_pollTimer.isActive();
    return true;
#else
    Q_UNUSED(portName); Q_UNUSED(baudRate);
    Q_UNUSED(dataBits); Q_UNUSED(parity); Q_UNUSED(stopBits);
    return false;
#endif
}

void SerialPortController::close()
{
#ifdef HAVE_SERIALPORT
    m_pollTimer.stop();
    if (m_port.isOpen()) {
        bool pttWasActive = (m_ctsFn == InputFunction::PttInput && m_lastCtsActive)
                         || (m_dsrFn == InputFunction::PttInput && m_lastDsrActive);
        if (pttWasActive)
            emit externalPttChanged(false);

        m_lastCtsActive = false;
        m_lastDsrActive = false;

        if (m_dtrFn != PinFunction::None)
            m_port.setDataTerminalReady(!m_dtrActiveHigh);
        if (m_rtsFn != PinFunction::None)
            m_port.setRequestToSend(!m_rtsActiveHigh);
        m_port.close();
        qCDebug(lcDevices) << "SerialPortController: closed";
    }
#endif
}

bool SerialPortController::isOpen() const
{
#ifdef HAVE_SERIALPORT
    return m_port.isOpen();
#else
    return false;
#endif
}

QString SerialPortController::portName() const
{
#ifdef HAVE_SERIALPORT
    return m_port.portName();
#else
    return {};
#endif
}

void SerialPortController::applyPin(PinFunction targetFn, bool active)
{
#ifdef HAVE_SERIALPORT
    if (!m_port.isOpen()) return;

    if (m_dtrFn == targetFn) {
        bool level = m_dtrActiveHigh ? active : !active;
        m_port.setDataTerminalReady(level);
    }
    if (m_rtsFn == targetFn) {
        bool level = m_rtsActiveHigh ? active : !active;
        m_port.setRequestToSend(level);
    }
#else
    Q_UNUSED(targetFn); Q_UNUSED(active);
#endif
}

void SerialPortController::updatePolling()
{
#ifdef HAVE_SERIALPORT
    bool needsPoll = (m_ctsFn != InputFunction::None || m_dsrFn != InputFunction::None);
    if (needsPoll && m_port.isOpen() && !m_pollTimer.isActive())
        m_pollTimer.start(POLL_INTERVAL_MS);
    else if (!needsPoll && m_pollTimer.isActive())
        m_pollTimer.stop();
#endif
}

#ifdef HAVE_SERIALPORT
void SerialPortController::pollInputPins()
{
    if (!m_port.isOpen()) return;

    auto pinState = m_port.pinoutSignals();

    if (m_port.error() != QSerialPort::NoError) {
        qCWarning(lcDevices) << "SerialPortController: pinoutSignals() error:"
                             << m_port.errorString();
        return;
    }

    bool cts = pinState.testFlag(QSerialPort::ClearToSendSignal);
    bool dsr = pinState.testFlag(QSerialPort::DataSetReadySignal);

    bool ctsActive = m_ctsActiveHigh ? cts : !cts;
    bool dsrActive = m_dsrActiveHigh ? dsr : !dsr;

    bool debounceOk = m_debounceTimer.elapsed() >= DEBOUNCE_MS;

    bool hasPttInput = (m_ctsFn == InputFunction::PttInput || m_dsrFn == InputFunction::PttInput);
    if (hasPttInput) {
        if (pinState != m_lastRawPins || debounceOk != m_lastDebounceLogged || !m_pollLogged) {
            m_lastRawPins = pinState;
            m_lastDebounceLogged = debounceOk;
            qCDebug(lcDevices)
                << "SerialPortController poll:"
                << "raw=" << Qt::hex << static_cast<int>(pinState)
                << "CTS=" << cts << "DSR=" << dsr
                << "ctsActive=" << ctsActive << "dsrActive=" << dsrActive
                << "lastCts=" << m_lastCtsActive << "lastDsr=" << m_lastDsrActive
                << "debounceOk=" << debounceOk
                << "debounceElapsed=" << m_debounceTimer.elapsed() << "ms"
                << "ctsFn=" << static_cast<int>(m_ctsFn)
                << "dsrFn=" << static_cast<int>(m_dsrFn)
                << "ctsPolActiveHigh=" << m_ctsActiveHigh
                << "dsrPolActiveHigh=" << m_dsrActiveHigh;
        }
    }

    if (!m_pollLogged) {
        m_pollLogged = true;
        qCDebug(lcDevices) << "SerialPortController: first poll — raw pinoutSignals ="
                           << Qt::hex << static_cast<int>(pinState)
                           << "DSR=" << dsr << "CTS=" << cts;
    }

    // ── PTT input ────────────────────────────────────────────────────────
    if (m_ctsFn == InputFunction::PttInput && ctsActive != m_lastCtsActive && debounceOk) {
        qCDebug(lcDevices) << "SerialPortController: CTS PTT edge →" << ctsActive;
        m_lastCtsActive = ctsActive;
        m_debounceTimer.restart();
        emit externalPttChanged(ctsActive);
    }
    if (m_dsrFn == InputFunction::PttInput && dsrActive != m_lastDsrActive && debounceOk) {
        qCDebug(lcDevices) << "SerialPortController: DSR PTT edge →" << dsrActive;
        m_lastDsrActive = dsrActive;
        m_debounceTimer.restart();
        emit externalPttChanged(dsrActive);
    }

    // ── CW straight key ──────────────────────────────────────────────────
    bool keyDown = false;
    bool hasKey = false;
    if (m_ctsFn == InputFunction::CwKeyInput) { keyDown = ctsActive; hasKey = true; }
    if (m_dsrFn == InputFunction::CwKeyInput) { keyDown = dsrActive; hasKey = true; }
    if (hasKey && keyDown != m_lastKeyDown) {
        m_lastKeyDown = keyDown;
        emit cwKeyChanged(keyDown);
    }

    // ── CW paddle (dit/dah) ──────────────────────────────────────────────
    bool ditActive = false, dahActive = false;
    bool hasPaddle = false;
    if (m_ctsFn == InputFunction::CwDitInput) { ditActive = ctsActive; hasPaddle = true; }
    if (m_dsrFn == InputFunction::CwDitInput) { ditActive = dsrActive; hasPaddle = true; }
    if (m_ctsFn == InputFunction::CwDahInput) { dahActive = ctsActive; hasPaddle = true; }
    if (m_dsrFn == InputFunction::CwDahInput) { dahActive = dsrActive; hasPaddle = true; }

    if (m_paddleSwap) std::swap(ditActive, dahActive);

    if (hasPaddle && (ditActive != m_lastDitActive || dahActive != m_lastDahActive)) {
        m_lastDitActive = ditActive;
        m_lastDahActive = dahActive;
        emit cwPaddleChanged(ditActive, dahActive);
    }
}
#endif  // HAVE_SERIALPORT

#endif  // !Q_OS_WIN

// ─────────────────────────────────────────────────────────────────────────────
// Platform-common: shared output control and settings persistence
// ─────────────────────────────────────────────────────────────────────────────

void SerialPortController::setTransmitting(bool tx)
{
    applyPin(PinFunction::PTT, tx);
    applyPin(PinFunction::CwPTT, tx);
}

void SerialPortController::setCwKeyDown(bool down)
{
    applyPin(PinFunction::CwKey, down);
}

void SerialPortController::loadSettings()
{
    auto& s = AppSettings::instance();
    QString port = s.value("SerialPortName", "").toString();

    auto strToFn = [](const QString& str) -> PinFunction {
        if (str == "PTT") return PinFunction::PTT;
        if (str == "CwKey") return PinFunction::CwKey;
        if (str == "CwPTT") return PinFunction::CwPTT;
        return PinFunction::None;
    };

    auto strToInputFn = [](const QString& str) -> InputFunction {
        if (str == "PttInput") return InputFunction::PttInput;
        if (str == "CwKeyInput") return InputFunction::CwKeyInput;
        if (str == "CwDitInput") return InputFunction::CwDitInput;
        if (str == "CwDahInput") return InputFunction::CwDahInput;
        return InputFunction::None;
    };

    m_dtrFn = strToFn(s.value("SerialDtrFunction", "None").toString());
    m_rtsFn = strToFn(s.value("SerialRtsFunction", "None").toString());
    m_dtrActiveHigh = s.value("SerialDtrPolarity", "ActiveHigh").toString() == "ActiveHigh";
    m_rtsActiveHigh = s.value("SerialRtsPolarity", "ActiveHigh").toString() == "ActiveHigh";

    m_ctsFn = strToInputFn(s.value("SerialCtsFunction", "None").toString());
    m_dsrFn = strToInputFn(s.value("SerialDsrFunction", "None").toString());
    m_ctsActiveHigh = s.value("SerialCtsPolarity", "ActiveHigh").toString() == "ActiveHigh";
    m_dsrActiveHigh = s.value("SerialDsrPolarity", "ActiveHigh").toString() == "ActiveHigh";
    m_paddleSwap = s.value("SerialPaddleSwap", "False").toString() == "True";

    bool shouldOpen = s.value("SerialAutoOpen", "False").toString() == "True"
                   || s.value("SerialPortOpen", "False").toString() == "True";

    qCDebug(lcDevices) << "SerialPortController::loadSettings:"
                       << "port=" << port
                       << "shouldOpen=" << shouldOpen
                       << "isOpen=" << isOpen()
                       << "dsrFn=" << static_cast<int>(m_dsrFn)
                       << "ctsFn=" << static_cast<int>(m_ctsFn)
                       << "dsrActiveHigh=" << m_dsrActiveHigh
                       << "ctsActiveHigh=" << m_ctsActiveHigh;

    if (!port.isEmpty() && shouldOpen) {
        int baud = s.value("SerialBaudRate", "9600").toInt();
        int data = s.value("SerialDataBits", "8").toInt();
        int par  = s.value("SerialParity", "0").toInt();
        int stop = s.value("SerialStopBits", "1").toInt();

#ifdef Q_OS_WIN
        bool samePort = isOpen() && m_winPortName == port && m_winBaudRate == baud;
#else
        bool samePort = false;
#  ifdef HAVE_SERIALPORT
        samePort = isOpen() && m_port.portName() == port && m_port.baudRate() == baud;
#  endif
#endif

        if (samePort) {
            // Port already open on the same port/baud — update settings in-place.
            // Re-sample pin state with new polarity so next event/poll doesn't
            // see a phantom edge from the polarity flip.
            qCDebug(lcDevices) << "SerialPortController::loadSettings: in-place update (no reopen)";
#ifdef Q_OS_WIN
            {
                DWORD modemStat = 0;
                ::GetCommModemStatus(static_cast<HANDLE>(m_hWin), &modemStat);
                bool cts = (modemStat & MS_CTS_ON) != 0;
                bool dsr = (modemStat & MS_DSR_ON) != 0;
                m_lastCtsActive = m_ctsActiveHigh ? cts : !cts;
                m_lastDsrActive = m_dsrActiveHigh ? dsr : !dsr;
            }
#else
#  ifdef HAVE_SERIALPORT
            {
                auto pinState = m_port.pinoutSignals();
                bool cts = pinState.testFlag(QSerialPort::ClearToSendSignal);
                bool dsr = pinState.testFlag(QSerialPort::DataSetReadySignal);
                m_lastCtsActive = m_ctsActiveHigh ? cts : !cts;
                m_lastDsrActive = m_dsrActiveHigh ? dsr : !dsr;
            }
            updatePolling();
#  endif
#endif
        } else {
            open(port, baud, data, par, stop);
        }
    } else if (!shouldOpen && isOpen()) {
        close();
    } else {
        updatePolling();
    }
}

void SerialPortController::saveSettings()
{
    auto& s = AppSettings::instance();

    auto fnToStr = [](PinFunction fn) -> QString {
        switch (fn) {
        case PinFunction::PTT:   return "PTT";
        case PinFunction::CwKey: return "CwKey";
        case PinFunction::CwPTT: return "CwPTT";
        default:                 return "None";
        }
    };

    auto inputFnToStr = [](InputFunction fn) -> QString {
        switch (fn) {
        case InputFunction::PttInput:   return "PttInput";
        case InputFunction::CwKeyInput: return "CwKeyInput";
        case InputFunction::CwDitInput: return "CwDitInput";
        case InputFunction::CwDahInput: return "CwDahInput";
        default:                        return "None";
        }
    };

    s.setValue("SerialPortName", portName());
    s.setValue("SerialDtrFunction", fnToStr(m_dtrFn));
    s.setValue("SerialRtsFunction", fnToStr(m_rtsFn));
    s.setValue("SerialDtrPolarity", m_dtrActiveHigh ? "ActiveHigh" : "ActiveLow");
    s.setValue("SerialRtsPolarity", m_rtsActiveHigh ? "ActiveHigh" : "ActiveLow");
    s.setValue("SerialCtsFunction", inputFnToStr(m_ctsFn));
    s.setValue("SerialDsrFunction", inputFnToStr(m_dsrFn));
    s.setValue("SerialCtsPolarity", m_ctsActiveHigh ? "ActiveHigh" : "ActiveLow");
    s.setValue("SerialDsrPolarity", m_dsrActiveHigh ? "ActiveHigh" : "ActiveLow");
    s.setValue("SerialPaddleSwap", m_paddleSwap ? "True" : "False");
    s.setValue("SerialAutoOpen", isOpen() ? "True" : "False");
    s.setValue("SerialPortOpen", isOpen() ? "True" : "False");
    s.save();
}

} // namespace MasterSDR
