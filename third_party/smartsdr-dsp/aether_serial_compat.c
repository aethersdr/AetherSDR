/*
 * Minimal FTDI D2XX-compatible serial shim used by the ThumbDV helper.
 *
 * Copyright (C) 2026 AetherSDR contributors.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ftd2xx.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#ifdef __APPLE__
#include <IOKit/serial/ioss.h>
#endif

typedef struct AetherSerialHandle {
    int fd;
    char path[256];
} AetherSerialHandle;

static const char* serial_path(void)
{
    const char* path = getenv("AETHER_DSTAR_THUMBDV_SERIAL");
    if (path == NULL || path[0] == '\0') {
        path = getenv("THUMBDV_SERIAL");
    }
    return (path != NULL && path[0] != '\0') ? path : NULL;
}

static speed_t baud_constant(ULONG baud)
{
    switch (baud) {
    case 230400:
#ifdef B230400
        return B230400;
#else
        return B115200;
#endif
    case 460800:
#ifdef B460800
        return B460800;
#else
        return B230400;
#endif
    default:
        return B115200;
    }
}

static FT_STATUS configure_baud(AetherSerialHandle* h, ULONG baud)
{
    struct termios tty;
    if (tcgetattr(h->fd, &tty) != 0) {
        return FT_IO_ERROR;
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
    speed_t speed = baud_constant(baud);
    cfsetispeed(&tty, speed);
    cfsetospeed(&tty, speed);
    if (tcsetattr(h->fd, TCSANOW, &tty) != 0) {
        return FT_IO_ERROR;
    }
#ifdef __APPLE__
    speed_t custom = (speed_t)baud;
    if (ioctl(h->fd, IOSSIOSPEED, &custom) == -1 && baud == 460800) {
        return FT_IO_ERROR;
    }
#endif
    tcflush(h->fd, TCIOFLUSH);
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
    if (handle == NULL) {
        return FT_INVALID_PARAMETER;
    }
    *handle = NULL;
    const char* path = (const char*)arg;
    if (path == NULL || path[0] == '\0') {
        path = serial_path();
    }
    if (path == NULL) {
        return FT_DEVICE_NOT_FOUND;
    }
    int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        return FT_DEVICE_NOT_FOUND;
    }
    AetherSerialHandle* h = calloc(1, sizeof(*h));
    if (h == NULL) {
        close(fd);
        return FT_INSUFFICIENT_RESOURCES;
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
    close(h->fd);
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
        return FT_IO_ERROR;
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
    ssize_t n = write(h->fd, buffer, bytesToWrite);
    if (n < 0) {
        return FT_IO_ERROR;
    }
    *bytesWritten = (DWORD)n;
    return (n == (ssize_t)bytesToWrite) ? FT_OK : FT_IO_ERROR;
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
    (void)handle; (void)wordLength; (void)stopBits; (void)parity;
    return FT_OK;
}

FT_STATUS FT_SetTimeouts(FT_HANDLE handle, ULONG readTimeout, ULONG writeTimeout)
{
    (void)handle; (void)readTimeout; (void)writeTimeout;
    return FT_OK;
}

FT_STATUS FT_SetFlowControl(FT_HANDLE handle, USHORT flowControl, UCHAR xon, UCHAR xoff)
{
    (void)xon; (void)xoff;
    AetherSerialHandle* h = (AetherSerialHandle*)handle;
    if (h == NULL) {
        return FT_INVALID_HANDLE;
    }
#ifdef CRTSCTS
    struct termios tty;
    if (tcgetattr(h->fd, &tty) != 0) {
        return FT_IO_ERROR;
    }
    if (flowControl == FT_FLOW_RTS_CTS) {
        tty.c_cflag |= CRTSCTS;
    } else {
        tty.c_cflag &= ~CRTSCTS;
    }
    if (tcsetattr(h->fd, TCSANOW, &tty) != 0) {
        return FT_IO_ERROR;
    }
#else
    (void)flowControl;
#endif
    return FT_OK;
}

FT_STATUS FT_SetLatencyTimer(FT_HANDLE handle, UCHAR timer)
{
    (void)handle; (void)timer;
    return FT_OK;
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
        return FT_IO_ERROR;
    }
    if (rxBytes) { *rxBytes = available > 0 ? (DWORD)available : 0; }
    return FT_OK;
}
