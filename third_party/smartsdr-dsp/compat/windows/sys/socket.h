/*
 * Minimal Winsock-backed POSIX socket compatibility for the ThumbDV helper.
 *
 * Copyright (C) 2026 AetherSDR contributors.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <errno.h>

#ifndef EIO
#define EIO EINVAL
#endif
#ifndef EWOULDBLOCK
#define EWOULDBLOCK EAGAIN
#endif
#ifndef EINPROGRESS
#define EINPROGRESS EAGAIN
#endif
#ifndef EADDRINUSE
#define EADDRINUSE EACCES
#endif
#ifndef ECONNRESET
#define ECONNRESET EIO
#endif
#ifndef ECONNREFUSED
#define ECONNREFUSED EIO
#endif
#ifndef ETIMEDOUT
#define ETIMEDOUT EIO
#endif

typedef SOCKET aether_socket_t;
typedef int socklen_t;

#ifndef AETHER_INVALID_SOCKET
#define AETHER_INVALID_SOCKET INVALID_SOCKET
#endif

static inline int aether_wsa_to_errno(int err)
{
    switch (err) {
    case WSAEWOULDBLOCK: return EWOULDBLOCK;
    case WSAEINTR: return EINTR;
    case WSAEINPROGRESS: return EINPROGRESS;
    case WSAEINVAL: return EINVAL;
    case WSAEADDRINUSE: return EADDRINUSE;
    case WSAECONNRESET: return ECONNRESET;
    case WSAECONNREFUSED: return ECONNREFUSED;
    case WSAETIMEDOUT: return ETIMEDOUT;
    default: return EIO;
    }
}

static inline void aether_socket_set_errno(void)
{
    errno = aether_wsa_to_errno(WSAGetLastError());
}

static inline int aether_socket_init(void)
{
    static volatile LONG initialized = 0;
    if (InterlockedCompareExchange(&initialized, 1, 0) == 0) {
        WSADATA data;
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            initialized = 0;
            errno = EIO;
            return -1;
        }
    }
    return 0;
}

static inline aether_socket_t aether_socket(int af, int type, int protocol)
{
    if (aether_socket_init() != 0) {
        return AETHER_INVALID_SOCKET;
    }
    SOCKET s = socket(af, type, protocol);
    if (s == INVALID_SOCKET) {
        aether_socket_set_errno();
    }
    return s;
}

static inline int aether_bind(aether_socket_t s, const struct sockaddr* name, int namelen)
{
    int result = bind(s, name, namelen);
    if (result == SOCKET_ERROR) {
        aether_socket_set_errno();
        return -1;
    }
    return result;
}

static inline int aether_connect(aether_socket_t s, const struct sockaddr* name, int namelen)
{
    int result = connect(s, name, namelen);
    if (result == SOCKET_ERROR) {
        aether_socket_set_errno();
        return -1;
    }
    return result;
}

static inline int aether_getsockname(aether_socket_t s, struct sockaddr* name,
                                     socklen_t* namelen)
{
    int result = getsockname(s, name, namelen);
    if (result == SOCKET_ERROR) {
        aether_socket_set_errno();
        return -1;
    }
    return result;
}

static inline int aether_setsockopt(aether_socket_t s, int level, int optname,
                                    const void* optval, int optlen)
{
    int result = setsockopt(s, level, optname, (const char*)optval, optlen);
    if (result == SOCKET_ERROR) {
        aether_socket_set_errno();
        return -1;
    }
    return result;
}

static inline int aether_recv(aether_socket_t s, void* buf, int len, int flags)
{
    int result = recv(s, (char*)buf, len, flags);
    if (result == SOCKET_ERROR) {
        aether_socket_set_errno();
        return -1;
    }
    return result;
}

static inline int aether_recvfrom(aether_socket_t s, void* buf, int len, int flags,
                                  struct sockaddr* from, socklen_t* fromlen)
{
    int result = recvfrom(s, (char*)buf, len, flags, from, fromlen);
    if (result == SOCKET_ERROR) {
        aether_socket_set_errno();
        return -1;
    }
    return result;
}

static inline int aether_send(aether_socket_t s, const void* buf, int len, int flags)
{
    int result = send(s, (const char*)buf, len, flags);
    if (result == SOCKET_ERROR) {
        aether_socket_set_errno();
        return -1;
    }
    return result;
}

static inline int aether_sendto(aether_socket_t s, const void* buf, int len, int flags,
                                const struct sockaddr* to, int tolen)
{
    int result = sendto(s, (const char*)buf, len, flags, to, tolen);
    if (result == SOCKET_ERROR) {
        aether_socket_set_errno();
        return -1;
    }
    return result;
}

static inline int aether_closesocket(aether_socket_t s)
{
    int result = closesocket(s);
    if (result == SOCKET_ERROR) {
        aether_socket_set_errno();
        return -1;
    }
    return 0;
}

#define socket(af, type, protocol) aether_socket((af), (type), (protocol))
#define bind(s, name, namelen) aether_bind((s), (name), (int)(namelen))
#define connect(s, name, namelen) aether_connect((s), (name), (int)(namelen))
#define getsockname(s, name, namelen) aether_getsockname((s), (name), (namelen))
#define setsockopt(s, level, optname, optval, optlen) \
    aether_setsockopt((s), (level), (optname), (optval), (int)(optlen))
#define recv(s, buf, len, flags) aether_recv((s), (buf), (int)(len), (flags))
#define recvfrom(s, buf, len, flags, from, fromlen) \
    aether_recvfrom((s), (buf), (int)(len), (flags), (from), (fromlen))
#define send(s, buf, len, flags) aether_send((s), (buf), (int)(len), (flags))
#define sendto(s, buf, len, flags, to, tolen) \
    aether_sendto((s), (buf), (int)(len), (flags), (to), (int)(tolen))
