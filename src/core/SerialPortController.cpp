#include "SerialPortController.h"
#include "AppSettings.h"
#include "LogManager.h"

#ifndef Q_OS_WIN
#ifdef HAVE_SERIALPORT
#include <cerrno>
#endif
#endif

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX  // guard: may already be defined by the build system (#4031)
#endif
#include <windows.h>
#include <QThread>
#include <QMetaObject>
#endif

namespace AetherSDR {

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

void SerialPortController::publishPttInput(bool active, const TxCoordinator::Request& input)
{
    if (active && !m_pttInputHeld) {
        m_pttInput = input.derive();
    }
    m_pttInputHeld = active;
    emit externalPttChanged(active, m_pttInput);
}

void SerialPortController::publishKeyInput(bool down, const TxCoordinator::Request& input)
{
    if (down && !m_keyInputHeld) {
        m_keyInput = input.derive();
    }
    m_keyInputHeld = down;
    emit cwKeyChanged(down, m_keyInput);
}

void SerialPortController::publishPaddleInput(bool dit, bool dah, const TxCoordinator::Request& input)
{
    if ((dit || dah) && !m_paddleInputHeld) {
        m_paddleInput = input;
        m_paddleKeyInput = input.derive();
    }
    m_paddleInputHeld = dit || dah;
    emit cwPaddleChanged(dit, dah, m_paddleInput, m_paddleKeyInput);
}

void SerialPortController::reportOpenFailure(const SerialOpenFailure& failure)
{
    const QString summary = serialOpenFailureSummary(failure);
    qCWarning(lcDevices) << "SerialPortController:" << summary
                         << "kind=" << static_cast<int>(failure.kind)
                         << "group=" << failure.group;
    emit errorOccurred(summary);
    emit connectionStateChanged(false, failure.device, failure.error,
                                failure.nativeCode, static_cast<int>(failure.kind),
                                failure.group);
}

void SerialPortController::retireTxInputs()
{
    // Fence before notifying the owner: even input already queued on another
    // thread must not key after close. Original releases still clear held
    // paddle state/TUNE exclusion (#5422) and straight-key sidetone.
    ++m_inputPortEpoch;
    releaseHeldTxInputs();
}

void SerialPortController::releaseHeldTxInputs()
{
    m_txProducer.discardInputs();
    emit txInputsCancelled();
    if (m_pttInputHeld) {
        publishPttInput(false);
    }
    if (m_keyInputHeld) {
        publishKeyInput(false);
    }
    if (m_paddleInputHeld) {
        publishPaddleInput(false, false);
    }
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
        const DWORD err = ::GetLastError();
        SerialOpenFailure failure;
        failure.kind = classifyWin32SerialOpenFailure(err);
        failure.device = portName;
        failure.error = qt_error_string(static_cast<int>(err)).trimmed();
        failure.nativeCode = static_cast<int>(err);
        reportOpenFailure(failure);
        return false;
    }

    // Configure baud rate / framing via DCB
    DCB dcb = {};
    dcb.DCBlength = sizeof(DCB);
    if (!::GetCommState(hPort, &dcb)) {
        DWORD err = ::GetLastError();
        ::CloseHandle(hPort);
        SerialOpenFailure failure;
        failure.kind = SerialOpenFailureKind::Other;
        failure.device = portName;
        failure.error = QStringLiteral("Failed to read port state: %1")
                            .arg(qt_error_string(static_cast<int>(err)).trimmed());
        failure.nativeCode = static_cast<int>(err);
        reportOpenFailure(failure);
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
        ::CloseHandle(hPort);
        SerialOpenFailure failure;
        failure.kind = SerialOpenFailureKind::Other;
        failure.device = portName;
        failure.error = QStringLiteral("Failed to configure baud/framing: %1")
                            .arg(qt_error_string(static_cast<int>(err)).trimmed());
        failure.nativeCode = static_cast<int>(err);
        reportOpenFailure(failure);
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
    bool cts = (modemStat & MS_CTS_ON)  != 0;
    bool dsr = (modemStat & MS_DSR_ON)  != 0;
    bool dcd = (modemStat & MS_RLSD_ON) != 0;   // RLSD = Receive Line Signal Detect = DCD
    baselineInputs(cts, dsr, dcd);
    m_debounceTimer.start();

    m_hWin        = static_cast<void*>(hPort);
    m_winPortName = portName;
    m_winBaudRate = baudRate;

    qCDebug(lcDevices) << "SerialPortController: opened" << portName << "at" << baudRate
                       << "(Win32) DSR=" << dsr << "CTS=" << cts << "DCD=" << dcd
                       << "dsrFn=" << static_cast<int>(m_dsrFn)
                       << "ctsFn=" << static_cast<int>(m_ctsFn)
                       << "dcdFn=" << static_cast<int>(m_dcdFn);

    // Start the WaitCommEvent watcher thread
    m_stopWinWatch.store(false);
    m_winWatchThread = QThread::create([this]() { runWinWatcher(); });
    m_winWatchThread->setObjectName("SerialWatcher");
    m_winWatchThread->start();

    emit connectionStateChanged(true, portName, {}, 0,
                                static_cast<int>(SerialOpenFailureKind::None), {});
    return true;
}

void SerialPortController::close()
{
    if (!isOpen()) return;
    retireTxInputs();

    HANDLE hPort = static_cast<HANDLE>(m_hWin);
    const QString closedPort = m_winPortName;

    m_lastCtsActive = false;
    m_lastDsrActive = false;
    m_lastDcdActive = false;
    m_lastDitActive = false;
    m_lastDahActive = false;

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
    emit connectionStateChanged(false, closedPort, {}, 0,
                                static_cast<int>(SerialOpenFailureKind::None), {});
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
    const quint64 portEpoch = m_inputPortEpoch.load(std::memory_order_acquire);

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
        bool dsr = (modemStat & MS_DSR_ON)  != 0;
        bool cts = (modemStat & MS_CTS_ON)  != 0;
        bool dcd = (modemStat & MS_RLSD_ON) != 0;
        const TxCoordinator::Request input = m_txProducer.request();

        qCDebug(lcDevices) << "SerialPortController: WaitCommEvent"
                           << Qt::hex << evtMask
                           << "DSR=" << dsr << "CTS=" << cts << "DCD=" << dcd;

        // Marshal the pin state back to this object's thread
        QMetaObject::invokeMethod(this, [this, dsr, cts, dcd, input, portEpoch]() {
            if (portEpoch == m_inputPortEpoch.load(std::memory_order_acquire)) {
                processWinPinChange(dsr, cts, dcd, input);
            }
        }, Qt::QueuedConnection);
    }

    qCDebug(lcDevices) << "SerialPortController: watcher thread exiting";
}

void SerialPortController::processWinPinChange(bool dsrRaw, bool ctsRaw, bool dcdRaw,
                                              const TxCoordinator::Request& input)
{
    if (!isOpen()) return;
    applyInputSample(ctsRaw, dsrRaw, dcdRaw, input);
}

#else  // ─────────────────────────────────────────────────────────────────────
// Non-Windows implementation — QSerialPort polling
// ─────────────────────────────────────────────────────────────────────────────

#ifdef HAVE_SERIALPORT
// Qt's own Unix name→path rule (QSerialPortInfoPrivate::portNameToSystemLocation).
// No bus enumeration, so it also names a device that is not present.
static QString systemLocationFor(const QString& name)
{
    return (name.startsWith(QLatin1Char('/')) || name.startsWith(QLatin1String("./"))
            || name.startsWith(QLatin1String("../")))
        ? name : QStringLiteral("/dev/") + name;
}
#endif

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

    errno = 0;
    if (!m_port.open(QIODevice::ReadWrite)) {
        const int openErrno = errno;  // before anything else can touch it
        const QSerialPort::SerialPortError qtErr = m_port.error();
        const QString qtText = m_port.errorString();
        const QString device = systemLocationFor(portName);

        // Qt maps EACCES, EBUSY and a held lock file all to PermissionError,
        // and errno after open() is whatever Qt last touched. The kind comes
        // from probing the node; errno is reported only when it is provably
        // the one behind Qt's text.
        const SerialDeviceProbe probe = probeSerialDevice(device);
        const int nativeCode =
            (openErrno != 0 && qt_error_string(openErrno) == qtText) ? openErrno : 0;

        SerialOpenFailure failure;
        failure.kind = classifySerialOpenFailure(
            probe, qtErr == QSerialPort::DeviceNotFoundError,
            qtErr == QSerialPort::PermissionError, nativeCode);
        failure.device = device;
        failure.error = qtText;
        failure.nativeCode = nativeCode;
        if (failure.kind == SerialOpenFailureKind::Permission) {
            if (failure.nativeCode == 0)
                failure.nativeCode = probe.accessErrno;
#ifdef Q_OS_LINUX
            // usermod/adduser advice is Linux-only.
            failure.group = serialGroupRemedy(probe);
#endif
        } else if (failure.kind == SerialOpenFailureKind::NotFound
                   && failure.nativeCode == 0) {
            failure.nativeCode = probe.statErrno;
        }
        reportOpenFailure(failure);
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
        bool dcd = pinState.testFlag(QSerialPort::DataCarrierDetectSignal);
        baselineInputs(cts, dsr, dcd);
        m_debounceTimer.start();
        qCDebug(lcDevices) << "SerialPortController: open() initial pin sample —"
                           << "raw=" << Qt::hex << static_cast<int>(pinState)
                           << "DSR=" << dsr << "CTS=" << cts << "DCD=" << dcd
                           << "dsrActiveHigh=" << m_dsrActiveHigh
                           << "ctsActiveHigh=" << m_ctsActiveHigh
                           << "dcdActiveHigh=" << m_dcdActiveHigh
                           << "→ lastDsrActive=" << m_lastDsrActive
                           << "lastCtsActive=" << m_lastCtsActive
                           << "lastDcdActive=" << m_lastDcdActive;
    }
    m_pollLogged = false;

    updatePolling();

    qCDebug(lcDevices) << "SerialPortController: opened" << portName << "at" << baudRate
                       << "dsrFn=" << static_cast<int>(m_dsrFn)
                       << "ctsFn=" << static_cast<int>(m_ctsFn)
                       << "pollActive=" << m_pollTimer.isActive();
    emit connectionStateChanged(true, systemLocationFor(portName), {}, 0,
                                static_cast<int>(SerialOpenFailureKind::None), {});
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
        retireTxInputs();
        m_lastCtsActive = false;
        m_lastDsrActive = false;
        m_lastDcdActive = false;
        m_lastDitActive = false;
        m_lastDahActive = false;

        if (m_dtrFn != PinFunction::None)
            m_port.setDataTerminalReady(!m_dtrActiveHigh);
        if (m_rtsFn != PinFunction::None)
            m_port.setRequestToSend(!m_rtsActiveHigh);
        m_port.close();
        qCDebug(lcDevices) << "SerialPortController: closed";
        emit connectionStateChanged(false, systemLocationFor(m_port.portName()), {}, 0,
                                    static_cast<int>(SerialOpenFailureKind::None), {});
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
    bool needsPoll = (m_ctsFn != InputFunction::None
                    || m_dsrFn != InputFunction::None
                    || m_dcdFn != InputFunction::None);
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
    const TxCoordinator::Request input = m_txProducer.request();

    auto pinState = m_port.pinoutSignals();

    if (m_port.error() != QSerialPort::NoError) {
        qCWarning(lcDevices) << "SerialPortController: pinoutSignals() error:"
                             << m_port.errorString();
        return;
    }

    bool cts = pinState.testFlag(QSerialPort::ClearToSendSignal);
    bool dsr = pinState.testFlag(QSerialPort::DataSetReadySignal);
    bool dcd = pinState.testFlag(QSerialPort::DataCarrierDetectSignal);

    bool ctsActive = m_ctsActiveHigh ? cts : !cts;
    bool dsrActive = m_dsrActiveHigh ? dsr : !dsr;
    bool dcdActive = m_dcdActiveHigh ? dcd : !dcd;

    bool debounceOk = m_debounceTimer.elapsed() >= DEBOUNCE_MS;

    bool hasPttInput = (m_ctsFn == InputFunction::PttInput
                     || m_dsrFn == InputFunction::PttInput
                     || m_dcdFn == InputFunction::PttInput);
    if (hasPttInput) {
        if (pinState != m_lastRawPins || debounceOk != m_lastDebounceLogged || !m_pollLogged) {
            m_lastRawPins = pinState;
            m_lastDebounceLogged = debounceOk;
            qCDebug(lcDevices)
                << "SerialPortController poll:"
                << "raw=" << Qt::hex << static_cast<int>(pinState)
                << "CTS=" << cts << "DSR=" << dsr << "DCD=" << dcd
                << "ctsActive=" << ctsActive << "dsrActive=" << dsrActive << "dcdActive=" << dcdActive
                << "lastCts=" << m_lastCtsActive << "lastDsr=" << m_lastDsrActive << "lastDcd=" << m_lastDcdActive
                << "debounceOk=" << debounceOk
                << "debounceElapsed=" << m_debounceTimer.elapsed() << "ms"
                << "ctsFn=" << static_cast<int>(m_ctsFn)
                << "dsrFn=" << static_cast<int>(m_dsrFn)
                << "dcdFn=" << static_cast<int>(m_dcdFn)
                << "ctsPolActiveHigh=" << m_ctsActiveHigh
                << "dsrPolActiveHigh=" << m_dsrActiveHigh
                << "dcdPolActiveHigh=" << m_dcdActiveHigh;
        }
    }

    if (!m_pollLogged) {
        m_pollLogged = true;
        qCDebug(lcDevices) << "SerialPortController: first poll — raw pinoutSignals ="
                           << Qt::hex << static_cast<int>(pinState)
                           << "DSR=" << dsr << "CTS=" << cts << "DCD=" << dcd;
    }

    applyInputSample(cts, dsr, dcd, input);
}
#endif  // HAVE_SERIALPORT

#endif  // !Q_OS_WIN

// ─────────────────────────────────────────────────────────────────────────────
// Platform-common: shared output control and settings persistence
// ─────────────────────────────────────────────────────────────────────────────

#if defined(HAVE_SERIALPORT) || defined(Q_OS_WIN)
SerialPortController::CwLines SerialPortController::cwLines(bool ctsActive, bool dsrActive,
                                                            bool dcdActive) const
{
    CwLines lines;
    if (m_ctsFn == InputFunction::CwKeyInput) { lines.keyDown = ctsActive; lines.hasKey = true; }
    if (m_dsrFn == InputFunction::CwKeyInput) { lines.keyDown = dsrActive; lines.hasKey = true; }
    if (m_dcdFn == InputFunction::CwKeyInput) { lines.keyDown = dcdActive; lines.hasKey = true; }
    if (m_ctsFn == InputFunction::CwDitInput) { lines.dit = ctsActive; lines.hasPaddle = true; }
    if (m_dsrFn == InputFunction::CwDitInput) { lines.dit = dsrActive; lines.hasPaddle = true; }
    if (m_dcdFn == InputFunction::CwDitInput) { lines.dit = dcdActive; lines.hasPaddle = true; }
    if (m_ctsFn == InputFunction::CwDahInput) { lines.dah = ctsActive; lines.hasPaddle = true; }
    if (m_dsrFn == InputFunction::CwDahInput) { lines.dah = dsrActive; lines.hasPaddle = true; }
    if (m_dcdFn == InputFunction::CwDahInput) { lines.dah = dcdActive; lines.hasPaddle = true; }
    if (m_paddleSwap) {
        std::swap(lines.dit, lines.dah);
    }
    return lines;
}

void SerialPortController::baselineInputs(bool ctsRaw, bool dsrRaw, bool dcdRaw)
{
    m_lastCtsActive = m_ctsActiveHigh ? ctsRaw : !ctsRaw;
    m_lastDsrActive = m_dsrActiveHigh ? dsrRaw : !dsrRaw;
    m_lastDcdActive = m_dcdActiveHigh ? dcdRaw : !dcdRaw;
    const CwLines lines = cwLines(m_lastCtsActive, m_lastDsrActive, m_lastDcdActive);
    m_lastKeyDown = lines.keyDown;
    m_lastDitActive = false;
    m_lastDahActive = false;
    m_ditHeldAtOpen = lines.dit;
    m_dahHeldAtOpen = lines.dah;
    recordBaselineSettings();
}

void SerialPortController::recordBaselineSettings()
{
    m_baseCtsFn = m_ctsFn;
    m_baseDsrFn = m_dsrFn;
    m_baseDcdFn = m_dcdFn;
    m_baseCtsActiveHigh = m_ctsActiveHigh;
    m_baseDsrActiveHigh = m_dsrActiveHigh;
    m_baseDcdActiveHigh = m_dcdActiveHigh;
    m_basePaddleSwap = m_paddleSwap;
}

void SerialPortController::rebaselineInPlace(bool ctsRaw, bool dsrRaw, bool dcdRaw)
{
    // Only a pin whose role or polarity changed (or the paddle swap) is acted
    // on: a reload that changed nothing on a pin leaves its held input keyed.
    // The port stays open, so the epoch is kept: the Win32 watcher captured it
    // at start.
    const bool ctsChanged = m_ctsFn != m_baseCtsFn || m_ctsActiveHigh != m_baseCtsActiveHigh;
    const bool dsrChanged = m_dsrFn != m_baseDsrFn || m_dsrActiveHigh != m_baseDsrActiveHigh;
    const bool dcdChanged = m_dcdFn != m_baseDcdFn || m_dcdActiveHigh != m_baseDcdActiveHigh;
    const bool swapChanged = m_paddleSwap != m_basePaddleSwap;
    if (!ctsChanged && !dsrChanged && !dcdChanged && !swapChanged) {
        return;
    }
    // A role is touched when a changed pin had it before the reload or has it now.
    const auto touched = [&](InputFunction a, InputFunction b) {
        const auto has = [&](bool changed, InputFunction before, InputFunction now) {
            return changed && (before == a || before == b || now == a || now == b);
        };
        return has(ctsChanged, m_baseCtsFn, m_ctsFn) || has(dsrChanged, m_baseDsrFn, m_dsrFn)
            || has(dcdChanged, m_baseDcdFn, m_dcdFn);
    };
    const bool pttTouched = touched(InputFunction::PttInput, InputFunction::PttInput);
    const bool keyTouched = touched(InputFunction::CwKeyInput, InputFunction::CwKeyInput);
    const bool paddleTouched =
        swapChanged || touched(InputFunction::CwDitInput, InputFunction::CwDahInput);

    // Release what a touched role holds; a baseline alone would drop it without
    // publishing the release and leave the radio keyed. When every held role is
    // touched, cancel the producer's inputs too, as close() does.
    const bool keepsAHold = (m_pttInputHeld && !pttTouched) || (m_keyInputHeld && !keyTouched)
                         || (m_paddleInputHeld && !paddleTouched);
    if (!keepsAHold) {
        releaseHeldTxInputs();
    } else {
        if (pttTouched && m_pttInputHeld) {
            publishPttInput(false);
        }
        if (keyTouched && m_keyInputHeld) {
            publishKeyInput(false);
        }
        if (paddleTouched && m_paddleInputHeld) {
            publishPaddleInput(false, false);
        }
    }

    // Rebaseline the changed pins under their new polarity; an input active
    // now keys nothing until it is released and pressed again.
    if (ctsChanged) { m_lastCtsActive = m_ctsActiveHigh ? ctsRaw : !ctsRaw; }
    if (dsrChanged) { m_lastDsrActive = m_dsrActiveHigh ? dsrRaw : !dsrRaw; }
    if (dcdChanged) { m_lastDcdActive = m_dcdActiveHigh ? dcdRaw : !dcdRaw; }
    const CwLines lines = cwLines(m_ctsActiveHigh ? ctsRaw : !ctsRaw,
                                  m_dsrActiveHigh ? dsrRaw : !dsrRaw,
                                  m_dcdActiveHigh ? dcdRaw : !dcdRaw);
    if (keyTouched) {
        m_lastKeyDown = lines.keyDown;
    }
    if (paddleTouched) {
        m_lastDitActive = false;
        m_lastDahActive = false;
        m_ditHeldAtOpen = lines.dit;
        m_dahHeldAtOpen = lines.dah;
    }
    recordBaselineSettings();
}

void SerialPortController::applyInputSample(bool ctsRaw, bool dsrRaw, bool dcdRaw,
                                            const TxCoordinator::Request& input)
{
    const bool ctsActive = m_ctsActiveHigh ? ctsRaw : !ctsRaw;
    const bool dsrActive = m_dsrActiveHigh ? dsrRaw : !dsrRaw;
    const bool dcdActive = m_dcdActiveHigh ? dcdRaw : !dcdRaw;
    const bool debounceOk = m_debounceTimer.elapsed() >= DEBOUNCE_MS;

    // ── PTT input ────────────────────────────────────────────────────────
    if (m_ctsFn == InputFunction::PttInput && ctsActive != m_lastCtsActive && debounceOk) {
        qCDebug(lcDevices) << "SerialPortController: CTS PTT edge →" << ctsActive;
        m_lastCtsActive = ctsActive;
        m_debounceTimer.restart();
        if (ctsActive || m_pttInputHeld) {
            publishPttInput(ctsActive, input);
        }
    }
    if (m_dsrFn == InputFunction::PttInput && dsrActive != m_lastDsrActive && debounceOk) {
        qCDebug(lcDevices) << "SerialPortController: DSR PTT edge →" << dsrActive;
        m_lastDsrActive = dsrActive;
        m_debounceTimer.restart();
        if (dsrActive || m_pttInputHeld) {
            publishPttInput(dsrActive, input);
        }
    }
    if (m_dcdFn == InputFunction::PttInput && dcdActive != m_lastDcdActive && debounceOk) {
        qCDebug(lcDevices) << "SerialPortController: DCD PTT edge →" << dcdActive;
        m_lastDcdActive = dcdActive;
        m_debounceTimer.restart();
        if (dcdActive || m_pttInputHeld) {
            publishPttInput(dcdActive, input);
        }
    }

    const CwLines lines = cwLines(ctsActive, dsrActive, dcdActive);
    // ── CW straight key ──────────────────────────────────────────────────
    if (lines.hasKey && lines.keyDown != m_lastKeyDown) {
        m_lastKeyDown = lines.keyDown;
        // A release of a key held at open was never published as down.
        if (lines.keyDown || m_keyInputHeld) {
            publishKeyInput(lines.keyDown, input);
        }
    }

    // Track DCD for the other roles so close-time stuck-PTT detection sees
    // fresh state. As the PTT input its baseline moves only with a published
    // edge, or a release inside the debounce window would be lost for good.
    if (m_dcdFn != InputFunction::PttInput) {
        m_lastDcdActive = dcdActive;
    }

    // ── CW paddle (dit/dah) ──────────────────────────────────────────────
    // A line active at open counts as open until it is released, so a paddle
    // resting on one contact keys nothing when the other paddle is pressed.
    if (!lines.dit) { m_ditHeldAtOpen = false; }
    if (!lines.dah) { m_dahHeldAtOpen = false; }
    const bool dit = lines.dit && !m_ditHeldAtOpen;
    const bool dah = lines.dah && !m_dahHeldAtOpen;
    if (lines.hasPaddle && (dit != m_lastDitActive || dah != m_lastDahActive)) {
        m_lastDitActive = dit;
        m_lastDahActive = dah;
        publishPaddleInput(dit, dah, input);
    }
}
#endif

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
    m_dcdFn = strToInputFn(s.value("SerialDcdFunction", "None").toString());
    m_ctsActiveHigh = s.value("SerialCtsPolarity", "ActiveHigh").toString() == "ActiveHigh";
    m_dsrActiveHigh = s.value("SerialDsrPolarity", "ActiveHigh").toString() == "ActiveHigh";
    m_dcdActiveHigh = s.value("SerialDcdPolarity", "ActiveHigh").toString() == "ActiveHigh";
    m_paddleSwap = s.value("SerialPaddleSwap", "False").toString() == "True";

    // SerialPortOpen alone: the operator's Open/Close for this session.
    // SerialAutoOpen only seeds it at startup, so Close always closes (#6972).
    const bool shouldOpen = s.value("SerialPortOpen", "False").toString() == "True";

    qCDebug(lcDevices) << "SerialPortController::loadSettings:"
                       << "port=" << port
                       << "shouldOpen=" << shouldOpen
                       << "isOpen=" << isOpen()
                       << "dsrFn=" << static_cast<int>(m_dsrFn)
                       << "ctsFn=" << static_cast<int>(m_ctsFn)
                       << "dcdFn=" << static_cast<int>(m_dcdFn)
                       << "dsrActiveHigh=" << m_dsrActiveHigh
                       << "ctsActiveHigh=" << m_ctsActiveHigh
                       << "dcdActiveHigh=" << m_dcdActiveHigh;

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
            // rebaselineInPlace() acts only on pins whose role or polarity changed
            // (or the paddle swap), so the next poll sees no phantom edge.
            qCDebug(lcDevices) << "SerialPortController::loadSettings: in-place update (no reopen)";
#ifdef Q_OS_WIN
            {
                DWORD modemStat = 0;
                ::GetCommModemStatus(static_cast<HANDLE>(m_hWin), &modemStat);
                bool cts = (modemStat & MS_CTS_ON)  != 0;
                bool dsr = (modemStat & MS_DSR_ON)  != 0;
                bool dcd = (modemStat & MS_RLSD_ON) != 0;
                rebaselineInPlace(cts, dsr, dcd);
            }
#else
#  ifdef HAVE_SERIALPORT
            {
                auto pinState = m_port.pinoutSignals();
                bool cts = pinState.testFlag(QSerialPort::ClearToSendSignal);
                bool dsr = pinState.testFlag(QSerialPort::DataSetReadySignal);
                bool dcd = pinState.testFlag(QSerialPort::DataCarrierDetectSignal);
                rebaselineInPlace(cts, dsr, dcd);
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

} // namespace AetherSDR
