/*
 * Minimal D2XX type/function declarations implemented by aether_serial_compat.c.
 *
 * Copyright (C) 2026 AetherSDR contributors.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
typedef unsigned int DWORD;
typedef unsigned int ULONG;
typedef unsigned short USHORT;
typedef unsigned char UCHAR;
typedef unsigned char BOOL;
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef void* FT_HANDLE;
typedef ULONG FT_STATUS;

enum {
    FT_OK = 0,
    FT_INVALID_HANDLE = 1,
    FT_DEVICE_NOT_FOUND = 2,
    FT_DEVICE_NOT_OPENED = 3,
    FT_IO_ERROR = 4,
    FT_INSUFFICIENT_RESOURCES = 5,
    FT_INVALID_PARAMETER = 6,
    FT_INVALID_BAUD_RATE = 7,
    FT_NOT_SUPPORTED = 17,
    FT_OTHER_ERROR = 18
};

#define FT_OPEN_BY_SERIAL_NUMBER 1
#define FT_BAUD_230400 230400
#define FT_BAUD_460800 460800
#define FT_BITS_8 ((UCHAR)8)
#define FT_STOP_BITS_1 ((UCHAR)0)
#define FT_PARITY_NONE ((UCHAR)0)
#define FT_FLOW_NONE 0x0000
#define FT_FLOW_RTS_CTS 0x0100

typedef struct _ft_device_list_info_node {
    ULONG Flags;
    ULONG Type;
    ULONG ID;
    DWORD LocId;
    char SerialNumber[256];
    char Description[128];
    FT_HANDLE ftHandle;
} FT_DEVICE_LIST_INFO_NODE;

FT_STATUS FT_CreateDeviceInfoList(DWORD* numDevs);
FT_STATUS FT_GetDeviceInfoList(FT_DEVICE_LIST_INFO_NODE* dest, DWORD* numDevs);
FT_STATUS FT_OpenEx(void* arg, DWORD flags, FT_HANDLE* handle);
FT_STATUS FT_Close(FT_HANDLE handle);
FT_STATUS FT_Read(FT_HANDLE handle, void* buffer, DWORD bytesToRead, DWORD* bytesReturned);
FT_STATUS FT_Write(FT_HANDLE handle, void* buffer, DWORD bytesToWrite, DWORD* bytesWritten);
FT_STATUS FT_SetBaudRate(FT_HANDLE handle, ULONG baudRate);
FT_STATUS FT_SetDataCharacteristics(FT_HANDLE handle, UCHAR wordLength, UCHAR stopBits, UCHAR parity);
FT_STATUS FT_SetTimeouts(FT_HANDLE handle, ULONG readTimeout, ULONG writeTimeout);
FT_STATUS FT_SetFlowControl(FT_HANDLE handle, USHORT flowControl, UCHAR xon, UCHAR xoff);
FT_STATUS FT_SetLatencyTimer(FT_HANDLE handle, UCHAR timer);
FT_STATUS FT_GetStatus(FT_HANDLE handle, DWORD* rxBytes, DWORD* txBytes, DWORD* eventWord);

const char* AetherSerial_GetLastError(void);
void AetherSerial_ClearLastError(void);

#ifdef __cplusplus
}
#endif
