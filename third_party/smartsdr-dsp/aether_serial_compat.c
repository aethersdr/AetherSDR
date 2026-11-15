/*
 * Minimal FTDI D2XX-compatible serial shim used by the ThumbDV helper.
 *
 * Copyright (C) 2026 AetherSDR contributors.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ftd2xx.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifdef _MSC_VER
static __declspec(thread) char aether_serial_last_error[512];
#else
static _Thread_local char aether_serial_last_error[512];
#endif

static FT_STATUS aether_serial_error(FT_STATUS status, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    vsnprintf(aether_serial_last_error, sizeof(aether_serial_last_error), format, args);
    va_end(args);
    return status;
}

const char* AetherSerial_GetLastError(void)
{
    return aether_serial_last_error;
}

void AetherSerial_ClearLastError(void)
{
    aether_serial_last_error[0] = '\0';
}

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <ctype.h>
#include <stdlib.h>

typedef struct AetherSerialHandle {
    HANDLE handle;
    char path[256];
} AetherSerialHandle;

static FT_STATUS windows_error(const char* operation)
{
    const DWORD error = GetLastError();
    char message[256] = {0};
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   NULL, error, 0, message, (DWORD)sizeof(message), NULL);
    while (message[0] != '\0'
           && (message[strlen(message) - 1U] == '\r'
               || message[strlen(message) - 1U] == '\n')) {
        message[strlen(message) - 1U] = '\0';
    }
    const size_t length = strlen(message);
    const FT_STATUS status = error == ERROR_FILE_NOT_FOUND
            || error == ERROR_PATH_NOT_FOUND
        ? FT_DEVICE_NOT_FOUND
        : (error == ERROR_ACCESS_DENIED || error == ERROR_SHARING_VIOLATION)
            ? FT_DEVICE_NOT_OPENED
            : FT_IO_ERROR;
    return aether_serial_error(status, "%s failed (Windows error %lu: %s)",
                               operation, (unsigned long)error,
                               length > 0U ? message : "unknown error");
}

static const char* serial_path(void)
{
    const char* path = getenv("AETHER_DV_THUMBDV_SERIAL");
    if (path == NULL || path[0] == '\0') {
        path = getenv("THUMBDV_SERIAL");
    }
    return (path != NULL && path[0] != '\0') ? path : NULL;
}

static const char* normalize_serial_path(const char* path, char* buffer, size_t bufferSize)
{
    if (path == NULL || path[0] == '\0') {
        return NULL;
    }
    if (strncmp(path, "\\\\.\\", 4) == 0) {
        return path;
    }
    const size_t length = strlen(path);
    if (length > 3U
            && (path[0] == 'C' || path[0] == 'c')
            && (path[1] == 'O' || path[1] == 'o')
            && (path[2] == 'M' || path[2] == 'm')
            && isdigit((unsigned char)path[3])) {
        for (size_t i = 4U; i < length; ++i) {
            if (!isdigit((unsigned char)path[i])) {
                return path;
            }
        }
        if (length + 4U >= bufferSize) {
            return NULL;
        }
        snprintf(buffer, bufferSize, "\\\\.\\%s", path);
        return buffer;
    }
    return path;
}

static FT_STATUS configure_timeouts(AetherSerialHandle* h, ULONG readTimeout, ULONG writeTimeout)
{
    COMMTIMEOUTS timeouts;
    memset(&timeouts, 0, sizeof(timeouts));
    if (readTimeout == 0) {
        timeouts.ReadIntervalTimeout = MAXDWORD;
        timeouts.ReadTotalTimeoutMultiplier = 0;
        timeouts.ReadTotalTimeoutConstant = 0;
    } else {
        timeouts.ReadIntervalTimeout = readTimeout;
        timeouts.ReadTotalTimeoutConstant = readTimeout;
    }
    timeouts.WriteTotalTimeoutConstant = writeTimeout;
    if (!SetCommTimeouts(h->handle, &timeouts)) {
        return windows_error("SetCommTimeouts");
    }
    return FT_OK;
}

static FT_STATUS configure_baud(AetherSerialHandle* h, ULONG baud)
{
    DCB dcb;
    memset(&dcb, 0, sizeof(dcb));
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(h->handle, &dcb)) {
        return windows_error("GetCommState");
    }
    dcb.BaudRate = baud;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    if (!SetCommState(h->handle, &dcb)) {
        return windows_error("SetCommState");
    }
    if (!PurgeComm(h->handle, PURGE_RXCLEAR | PURGE_TXCLEAR)) {
        return windows_error("PurgeComm");
    }
    return configure_timeouts(h, 0, 0);
}

FT_STATUS FT_CreateDeviceInfoList(DWORD* numDevs)
{
    if (numDevs == NULL) {
        return FT_INVALID_PARAMETER;
    }
    *numDevs = serial_path() ? 1U : 0U;
    return FT_OK;
}

FT_STATUS FT_GetDeviceInfoList(FT_DEVICE_LIST_INFO_NODE* dest, DWORD* numDevs)
{
    const char* path = serial_path();
    if (numDevs == NULL) {
        return FT_INVALID_PARAMETER;
    }
    if (path == NULL) {
        *numDevs = 0;
        return FT_OK;
    }
    if (dest == NULL || *numDevs == 0) {
        *numDevs = 1;
        return FT_INVALID_PARAMETER;
    }
    memset(dest, 0, sizeof(*dest));
    snprintf(dest->SerialNumber, sizeof(dest->SerialNumber), "%s", path);
    snprintf(dest->Description, sizeof(dest->Description), "ThumbDV serial port");
    *numDevs = 1;
    return FT_OK;
}

FT_STATUS FT_OpenEx(void* arg, DWORD flags, FT_HANDLE* handle)
{
    (void)flags;
    AetherSerial_ClearLastError();
    if (handle == NULL) {
        return FT_INVALID_PARAMETER;
    }
    *handle = NULL;
    const char* path = (const char*)arg;
    if (path == NULL || path[0] == '\0') {
        path = serial_path();
    }
    char normalized[256];
    const char* winPath = normalize_serial_path(path, normalized, sizeof(normalized));
    if (winPath == NULL) {
        return aether_serial_error(path == NULL ? FT_DEVICE_NOT_FOUND
                                                : FT_INVALID_PARAMETER,
                                   path == NULL
                                       ? "No ThumbDV serial path was supplied"
                                       : "ThumbDV serial path is too long");
    }
    HANDLE serial = CreateFileA(winPath, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (serial == INVALID_HANDLE_VALUE) {
        return windows_error("CreateFile");
    }
    AetherSerialHandle* h =
        (AetherSerialHandle*)calloc(1, sizeof(*h));
    if (h == NULL) {
        CloseHandle(serial);
        return aether_serial_error(FT_INSUFFICIENT_RESOURCES,
                                   "Unable to allocate a serial handle");
    }
    h->handle = serial;
    snprintf(h->path, sizeof(h->path), "%s", winPath);
    *handle = h;
    return FT_OK;
}

FT_STATUS FT_Close(FT_HANDLE handle)
{
    AetherSerialHandle* h = (AetherSerialHandle*)handle;
    if (h == NULL) {
        return FT_INVALID_HANDLE;
    }
    if (!CloseHandle(h->handle)) {
        const FT_STATUS status = windows_error("CloseHandle");
        free(h);
        return status;
    }
    free(h);
    return FT_OK;
}

FT_STATUS FT_Read(FT_HANDLE handle, void* buffer, DWORD bytesToRead, DWORD* bytesReturned)
{
    AetherSerialHandle* h = (AetherSerialHandle*)handle;
    if (bytesReturned) { *bytesReturned = 0; }
    if (h == NULL || buffer == NULL || bytesReturned == NULL) {
        return FT_INVALID_PARAMETER;
    }
    DWORD readBytes = 0;
    if (!ReadFile(h->handle, buffer, bytesToRead, &readBytes, NULL)) {
        return windows_error("ReadFile");
    }
    *bytesReturned = readBytes;
    return FT_OK;
}

FT_STATUS FT_Write(FT_HANDLE handle, void* buffer, DWORD bytesToWrite, DWORD* bytesWritten)
{
    AetherSerialHandle* h = (AetherSerialHandle*)handle;
    if (bytesWritten) { *bytesWritten = 0; }
    if (h == NULL || buffer == NULL || bytesWritten == NULL) {
        return FT_INVALID_PARAMETER;
    }
    DWORD total = 0;
    const unsigned char* p = (const unsigned char*)buffer;
    while (total < bytesToWrite) {
        DWORD written = 0;
        if (!WriteFile(h->handle, p + total, bytesToWrite - total, &written, NULL)) {
            *bytesWritten = total;
            return windows_error("WriteFile");
        }
        if (written == 0) {
            *bytesWritten = total;
            return aether_serial_error(FT_IO_ERROR,
                                       "WriteFile completed without writing data");
        }
        total += written;
    }
    *bytesWritten = total;
    return FT_OK;
}

FT_STATUS FT_SetBaudRate(FT_HANDLE handle, ULONG baudRate)
{
    AetherSerialHandle* h = (AetherSerialHandle*)handle;
    if (h == NULL) {
        return FT_INVALID_HANDLE;
    }
    return configure_baud(h, baudRate);
}

FT_STATUS FT_SetDataCharacteristics(FT_HANDLE handle, UCHAR wordLength, UCHAR stopBits, UCHAR parity)
{
    AetherSerialHandle* h = (AetherSerialHandle*)handle;
    if (h == NULL) {
        return FT_INVALID_HANDLE;
    }
    if (wordLength != FT_BITS_8 || stopBits != FT_STOP_BITS_1
        || parity != FT_PARITY_NONE) {
        return aether_serial_error(FT_INVALID_PARAMETER,
                                   "Only 8-N-1 serial framing is supported");
    }
    DCB dcb;
    memset(&dcb, 0, sizeof(dcb));
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(h->handle, &dcb)) {
        return windows_error("GetCommState");
    }
    dcb.ByteSize = wordLength;
    dcb.StopBits = stopBits == FT_STOP_BITS_1 ? ONESTOPBIT : TWOSTOPBITS;
    dcb.Parity = parity == FT_PARITY_NONE ? NOPARITY : parity;
    if (!SetCommState(h->handle, &dcb)) {
        return windows_error("SetCommState");
    }
    return FT_OK;
}

FT_STATUS FT_SetTimeouts(FT_HANDLE handle, ULONG readTimeout, ULONG writeTimeout)
{
    AetherSerialHandle* h = (AetherSerialHandle*)handle;
    if (h == NULL) {
        return FT_INVALID_HANDLE;
    }
    return configure_timeouts(h, readTimeout, writeTimeout);
}

FT_STATUS FT_SetFlowControl(FT_HANDLE handle, USHORT flowControl, UCHAR xon, UCHAR xoff)
{
    (void)xon;
    (void)xoff;
    AetherSerialHandle* h = (AetherSerialHandle*)handle;
    if (h == NULL) {
        return FT_INVALID_HANDLE;
    }
    if (flowControl != FT_FLOW_RTS_CTS && flowControl != FT_FLOW_NONE) {
        return aether_serial_error(FT_INVALID_PARAMETER,
                                   "Unsupported serial flow-control mode");
    }
    DCB dcb;
    memset(&dcb, 0, sizeof(dcb));
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(h->handle, &dcb)) {
        return windows_error("GetCommState");
    }
    if (flowControl == FT_FLOW_RTS_CTS) {
        dcb.fOutxCtsFlow = TRUE;
        dcb.fRtsControl = RTS_CONTROL_HANDSHAKE;
    } else {
        dcb.fOutxCtsFlow = FALSE;
        dcb.fRtsControl = RTS_CONTROL_DISABLE;
    }
    if (!SetCommState(h->handle, &dcb)) {
        return windows_error("SetCommState");
    }
    return FT_OK;
}

FT_STATUS FT_SetLatencyTimer(FT_HANDLE handle, UCHAR timer)
{
    if (handle == NULL) {
        return FT_INVALID_HANDLE;
    }
    (void)timer;
    return aether_serial_error(
        FT_NOT_SUPPORTED,
        "FTDI latency timer is unavailable through the Windows COM-port API");
}

FT_STATUS FT_GetStatus(FT_HANDLE handle, DWORD* rxBytes, DWORD* txBytes, DWORD* eventWord)
{
    AetherSerialHandle* h = (AetherSerialHandle*)handle;
    if (rxBytes) { *rxBytes = 0; }
    if (txBytes) { *txBytes = 0; }
    if (eventWord) { *eventWord = 0; }
    if (h == NULL) {
        return FT_INVALID_HANDLE;
    }
    COMSTAT stat;
    DWORD errors = 0;
    if (!ClearCommError(h->handle, &errors, &stat)) {
        return windows_error("ClearCommError");
    }
    if (rxBytes) { *rxBytes = stat.cbInQue; }
    if (txBytes) { *txBytes = stat.cbOutQue; }
    if (eventWord) { *eventWord = errors; }
    return FT_OK;
}

#else

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>
#include <poll.h>
#include <stdint.h>
#include <sys/ioctl.h>
#ifdef __APPLE__
#include <IOKit/serial/ioss.h>
#endif

typedef struct AetherSerialHandle {
    int fd;
    char path[256];
} AetherSerialHandle;

static FT_STATUS posix_error(const char* operation, int error)
{
    const FT_STATUS status = error == ENOENT || error == ENODEV
        ? FT_DEVICE_NOT_FOUND
        : (error == EACCES || error == EPERM || error == EBUSY)
            ? FT_DEVICE_NOT_OPENED
            : FT_IO_ERROR;
    return aether_serial_error(status, "%s failed (%s)", operation, strerror(error));
}

static const char* serial_path(void)
{
    const char* path = getenv("AETHER_DV_THUMBDV_SERIAL");
    if (path == NULL || path[0] == '\0') {
        path = getenv("THUMBDV_SERIAL");
    }
    return (path != NULL && path[0] != '\0') ? path : NULL;
}

static FT_STATUS baud_constant(ULONG baud, speed_t* speed)
{
    switch (baud) {
    case 230400:
#ifdef B230400
        *speed = B230400;
        return FT_OK;
#elif defined(__APPLE__)
        *speed = B9600;
        return FT_OK;
#else
        return aether_serial_error(FT_INVALID_BAUD_RATE,
                                   "230400 baud is not supported by this tty API");
#endif
    case 460800:
#ifdef B460800
        *speed = B460800;
        return FT_OK;
#elif defined(__APPLE__)
        *speed = B9600;
        return FT_OK;
#else
        return aether_serial_error(FT_INVALID_BAUD_RATE,
                                   "460800 baud is not supported by this tty API");
#endif
    default:
        return aether_serial_error(FT_INVALID_BAUD_RATE,
                                   "Unsupported ThumbDV baud rate %lu",
                                   (unsigned long)baud);
    }
}

static FT_STATUS configure_baud(AetherSerialHandle* h, ULONG baud)
{
    struct termios tty;
    if (tcgetattr(h->fd, &tty) != 0) {
        return posix_error("tcgetattr", errno);
    }
    cfmakeraw(&tty);
    tty.c_cflag |= (CLOCAL | CREAD);
#ifdef CRTSCTS
    tty.c_cflag &= ~CRTSCTS;
#endif
    tty.c_cflag &= ~PARENB;
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 0;
    speed_t speed = B9600;
    const FT_STATUS speedStatus = baud_constant(baud, &speed);
    if (speedStatus != FT_OK) {
        return speedStatus;
    }
    if (cfsetispeed(&tty, speed) != 0 || cfsetospeed(&tty, speed) != 0) {
        return posix_error("cfsetspeed", errno);
    }
    if (tcsetattr(h->fd, TCSANOW, &tty) != 0) {
        return posix_error("tcsetattr", errno);
    }
#ifdef __APPLE__
    speed_t custom = (speed_t)baud;
    if (ioctl(h->fd, IOSSIOSPEED, &custom) == -1) {
        return posix_error("IOSSIOSPEED", errno);
    }
#endif
    if (tcflush(h->fd, TCIOFLUSH) != 0) {
        return posix_error("tcflush", errno);
    }
    return FT_OK;
}

FT_STATUS FT_CreateDeviceInfoList(DWORD* numDevs)
{
    if (numDevs == NULL) {
        return FT_INVALID_PARAMETER;
    }
    *numDevs = serial_path() ? 1U : 0U;
    return FT_OK;
}

FT_STATUS FT_GetDeviceInfoList(FT_DEVICE_LIST_INFO_NODE* dest, DWORD* numDevs)
{
    const char* path = serial_path();
    if (numDevs == NULL) {
        return FT_INVALID_PARAMETER;
    }
    if (path == NULL) {
        *numDevs = 0;
        return FT_OK;
    }
    if (dest == NULL || *numDevs == 0) {
        *numDevs = 1;
        return FT_INVALID_PARAMETER;
    }
    memset(dest, 0, sizeof(*dest));
    snprintf(dest->SerialNumber, sizeof(dest->SerialNumber), "%s", path);
    snprintf(dest->Description, sizeof(dest->Description), "ThumbDV serial port");
    *numDevs = 1;
    return FT_OK;
}

FT_STATUS FT_OpenEx(void* arg, DWORD flags, FT_HANDLE* handle)
{
    (void)flags;
    AetherSerial_ClearLastError();
    if (handle == NULL) {
        return FT_INVALID_PARAMETER;
    }
    *handle = NULL;
    const char* path = (const char*)arg;
    if (path == NULL || path[0] == '\0') {
        path = serial_path();
    }
    if (path == NULL) {
        return aether_serial_error(FT_DEVICE_NOT_FOUND,
                                   "No ThumbDV serial path was supplied");
    }
    int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        return posix_error("open", errno);
    }
    AetherSerialHandle* h = calloc(1, sizeof(*h));
    if (h == NULL) {
        close(fd);
        return aether_serial_error(FT_INSUFFICIENT_RESOURCES,
                                   "Unable to allocate a serial handle");
    }
    h->fd = fd;
    snprintf(h->path, sizeof(h->path), "%s", path);
    *handle = h;
    return FT_OK;
}

FT_STATUS FT_Close(FT_HANDLE handle)
{
    AetherSerialHandle* h = (AetherSerialHandle*)handle;
    if (h == NULL) {
        return FT_INVALID_HANDLE;
    }
    if (close(h->fd) != 0) {
        const FT_STATUS status = posix_error("close", errno);
        free(h);
        return status;
    }
    free(h);
    return FT_OK;
}

FT_STATUS FT_Read(FT_HANDLE handle, void* buffer, DWORD bytesToRead, DWORD* bytesReturned)
{
    AetherSerialHandle* h = (AetherSerialHandle*)handle;
    if (bytesReturned) { *bytesReturned = 0; }
    if (h == NULL || buffer == NULL || bytesReturned == NULL) {
        return FT_INVALID_PARAMETER;
    }
    ssize_t n = read(h->fd, buffer, bytesToRead);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return FT_OK;
        }
        return posix_error("read", errno);
    }
    *bytesReturned = (DWORD)n;
    return FT_OK;
}

FT_STATUS FT_Write(FT_HANDLE handle, void* buffer, DWORD bytesToWrite, DWORD* bytesWritten)
{
    AetherSerialHandle* h = (AetherSerialHandle*)handle;
    if (bytesWritten) { *bytesWritten = 0; }
    if (h == NULL || buffer == NULL || bytesWritten == NULL) {
        return FT_INVALID_PARAMETER;
    }

    size_t total = 0;
    const uint8_t* p = (const uint8_t*)buffer;
    while (total < bytesToWrite) {
        ssize_t n = write(h->fd, p + total, (size_t)bytesToWrite - total);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Kernel TX buffer full on the non-blocking fd; wait for it
                // to drain rather than dropping the vocoder frame.
                struct pollfd pfd = { .fd = h->fd, .events = POLLOUT, .revents = 0 };
                int pollResult;
                do {
                    pollResult = poll(&pfd, 1, 1000);
                } while (pollResult < 0 && errno == EINTR);
                if (pollResult <= 0) {
                    return pollResult == 0
                        ? aether_serial_error(FT_IO_ERROR,
                                              "Timed out waiting to write serial data")
                        : posix_error("poll", errno);
                }
                continue;
            }
            return posix_error("write", errno);
        }
        if (n == 0) {
            struct pollfd pfd = { .fd = h->fd, .events = POLLOUT, .revents = 0 };
            int pollResult;
            do {
                pollResult = poll(&pfd, 1, 1000);
            } while (pollResult < 0 && errno == EINTR);
            if (pollResult <= 0) {
                return pollResult == 0
                    ? aether_serial_error(FT_IO_ERROR,
                                          "Timed out waiting to write serial data")
                    : posix_error("poll", errno);
            }
            continue;
        }
        total += (size_t)n;
    }
    *bytesWritten = (DWORD)total;
    return FT_OK;
}

FT_STATUS FT_SetBaudRate(FT_HANDLE handle, ULONG baudRate)
{
    AetherSerialHandle* h = (AetherSerialHandle*)handle;
    if (h == NULL) {
        return FT_INVALID_HANDLE;
    }
    return configure_baud(h, baudRate);
}

FT_STATUS FT_SetDataCharacteristics(FT_HANDLE handle, UCHAR wordLength, UCHAR stopBits, UCHAR parity)
{
    AetherSerialHandle* h = (AetherSerialHandle*)handle;
    if (h == NULL) {
        return FT_INVALID_HANDLE;
    }
    if (wordLength != FT_BITS_8 || stopBits != FT_STOP_BITS_1
        || parity != FT_PARITY_NONE) {
        return aether_serial_error(FT_INVALID_PARAMETER,
                                   "Only 8-N-1 serial framing is supported");
    }
    struct termios tty;
    if (tcgetattr(h->fd, &tty) != 0) {
        return posix_error("tcgetattr", errno);
    }
    tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;
    tty.c_cflag &= ~(PARENB | CSTOPB);
    if (tcsetattr(h->fd, TCSANOW, &tty) != 0) {
        return posix_error("tcsetattr", errno);
    }
    return FT_OK;
}

FT_STATUS FT_SetTimeouts(FT_HANDLE handle, ULONG readTimeout, ULONG writeTimeout)
{
    (void)writeTimeout;
    AetherSerialHandle* h = (AetherSerialHandle*)handle;
    if (h == NULL) {
        return FT_INVALID_HANDLE;
    }
    struct termios tty;
    if (tcgetattr(h->fd, &tty) != 0) {
        return posix_error("tcgetattr", errno);
    }
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = readTimeout == 0U
        ? 0
        : (cc_t)((readTimeout + 99U) / 100U > 255U
                     ? 255U
                     : (readTimeout + 99U) / 100U);
    if (tcsetattr(h->fd, TCSANOW, &tty) != 0) {
        return posix_error("tcsetattr", errno);
    }
    return FT_OK;
}

FT_STATUS FT_SetFlowControl(FT_HANDLE handle, USHORT flowControl, UCHAR xon, UCHAR xoff)
{
    (void)xon; (void)xoff;
    AetherSerialHandle* h = (AetherSerialHandle*)handle;
    if (h == NULL) {
        return FT_INVALID_HANDLE;
    }
    if (flowControl != FT_FLOW_RTS_CTS && flowControl != FT_FLOW_NONE) {
        return aether_serial_error(FT_INVALID_PARAMETER,
                                   "Unsupported serial flow-control mode");
    }
#ifdef CRTSCTS
    struct termios tty;
    if (tcgetattr(h->fd, &tty) != 0) {
        return posix_error("tcgetattr", errno);
    }
    if (flowControl == FT_FLOW_RTS_CTS) {
        tty.c_cflag |= CRTSCTS;
    } else {
        tty.c_cflag &= ~CRTSCTS;
    }
    if (tcsetattr(h->fd, TCSANOW, &tty) != 0) {
        return posix_error("tcsetattr", errno);
    }
#else
    if (flowControl == FT_FLOW_RTS_CTS) {
        return aether_serial_error(
            FT_NOT_SUPPORTED,
            "RTS/CTS flow control is unavailable through this POSIX tty API");
    }
#endif
    return FT_OK;
}

FT_STATUS FT_SetLatencyTimer(FT_HANDLE handle, UCHAR timer)
{
    if (handle == NULL) {
        return FT_INVALID_HANDLE;
    }
    (void)timer;
    return aether_serial_error(
        FT_NOT_SUPPORTED,
        "FTDI latency timer is unavailable through the POSIX tty API");
}

FT_STATUS FT_GetStatus(FT_HANDLE handle, DWORD* rxBytes, DWORD* txBytes, DWORD* eventWord)
{
    AetherSerialHandle* h = (AetherSerialHandle*)handle;
    if (rxBytes) { *rxBytes = 0; }
    if (txBytes) { *txBytes = 0; }
    if (eventWord) { *eventWord = 0; }
    if (h == NULL) {
        return FT_INVALID_HANDLE;
    }
    int available = 0;
    if (ioctl(h->fd, FIONREAD, &available) != 0) {
        return posix_error("ioctl(FIONREAD)", errno);
    }
    if (rxBytes) { *rxBytes = available > 0 ? (DWORD)available : 0; }
    return FT_OK;
}

#endif
