#include "VitaDscpMarker.h"
#include "VitaDscpFlow.h"

#include <QVector>

#include <cstring>

#if defined(Q_OS_WIN)
#include <QNetworkInterface>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <qos2.h>
#elif defined(Q_OS_UNIX)
#include <cerrno>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace AetherSDR {

struct VitaDscpMarker::NativeState {
#if defined(Q_OS_WIN)
    HMODULE library{nullptr};
    HANDLE handle{nullptr};
    QVector<QOS_FLOWID> flows;
    decltype(&QOSCreateHandle) createHandle{nullptr};
    decltype(&QOSCloseHandle) closeHandle{nullptr};
    decltype(&QOSAddSocketToFlow) addSocketToFlow{nullptr};
    decltype(&QOSSetFlow) setFlow{nullptr};
    decltype(&QOSRemoveSocketFromFlow) removeSocketFromFlow{nullptr};

    ~NativeState()
    {
        if (library) {
            FreeLibrary(library);
        }
    }

    bool open(QString& error)
    {
        if (!library) {
            library = LoadLibraryExW(L"qwave.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (!library) {
                error = QStringLiteral("qWAVE unavailable (error %1)").arg(GetLastError());
                return false;
            }
            createHandle = reinterpret_cast<decltype(createHandle)>(GetProcAddress(library, "QOSCreateHandle"));
            closeHandle = reinterpret_cast<decltype(closeHandle)>(GetProcAddress(library, "QOSCloseHandle"));
            addSocketToFlow = reinterpret_cast<decltype(addSocketToFlow)>(GetProcAddress(library, "QOSAddSocketToFlow"));
            setFlow = reinterpret_cast<decltype(setFlow)>(GetProcAddress(library, "QOSSetFlow"));
            removeSocketFromFlow = reinterpret_cast<decltype(removeSocketFromFlow)>(GetProcAddress(library, "QOSRemoveSocketFromFlow"));
        }
        if (!createHandle || !closeHandle || !addSocketToFlow || !setFlow || !removeSocketFromFlow) {
            error = QStringLiteral("qWAVE entry points unavailable");
            return false;
        }
        QOS_VERSION version{1, 0};
        if (!createHandle(&version, &handle)) {
            error = QStringLiteral("QOSCreateHandle failed (error %1)").arg(GetLastError());
            return false;
        }
        return true;
    }

    struct FlowApi {
        using FlowId = QOS_FLOWID;
        NativeState& state;
        SOCKET socket;
        sockaddr_storage destination;
        QString& error;

        bool addBestEffortFlow(FlowId& flow)
        {
            if (!state.addSocketToFlow(state.handle, socket,
                    reinterpret_cast<sockaddr*>(&destination), QOSTrafficTypeBestEffort,
                    QOS_NON_ADAPTIVE_FLOW, &flow)) {
                error = QStringLiteral("QOSAddSocketToFlow failed (error %1)").arg(GetLastError());
                return false;
            }
            return true;
        }

        bool setDscp(FlowId flow, int dscp)
        {
            DWORD value = static_cast<DWORD>(dscp);
            if (!state.setFlow(state.handle, flow, QOSSetOutgoingDSCPValue,
                    sizeof(value), &value, 0, nullptr)) {
                error = QStringLiteral("QOSSetOutgoingDSCPValue failed (error %1; OS policy or privileges may deny EF)")
                            .arg(GetLastError());
                return false;
            }
            return true;
        }

        void removeFlow(FlowId flow)
        {
            state.removeSocketFromFlow(state.handle, 0, flow, 0);
        }
    };
#endif
};

VitaDscpMarker::VitaDscpMarker() : m_native(std::make_unique<NativeState>()) {}

VitaDscpMarker::~VitaDscpMarker()
{
    reset();
}

void VitaDscpMarker::reset()
{
#if defined(Q_OS_WIN)
    if (m_native->handle) {
        for (QOS_FLOWID flow : m_native->flows) {
            m_native->removeSocketFromFlow(m_native->handle, 0, flow, 0);
        }
        m_native->flows.clear();
        m_native->closeHandle(m_native->handle);
        m_native->handle = nullptr;
    }
#endif
}

VitaDscpResult VitaDscpMarker::apply(qintptr descriptor, const QHostAddress& destination,
                                   std::initializer_list<quint16> ports)
{
    reset();
    if (descriptor == -1) {
        return {false, QStringLiteral("UDP socket has no native descriptor")};
    }
#if defined(Q_OS_WIN)
    bool ipv4 = false;
    const quint32 address = destination.toIPv4Address(&ipv4);
    const bool ipv6 = !ipv4 && destination.protocol() == QAbstractSocket::IPv6Protocol;
    if ((!ipv4 && !ipv6) || ports.size() == 0) {
        return {false, QStringLiteral("IPv4 or IPv6 destination and UDP port required")};
    }
    QString error;
    if (!m_native->open(error)) {
        return {false, error};
    }
    for (quint16 port : ports) {
        NativeState::FlowApi api{*m_native, static_cast<SOCKET>(descriptor), {}, error};
        if (ipv6) {
            auto* in6 = reinterpret_cast<sockaddr_in6*>(&api.destination);
            const Q_IPV6ADDR bytes = destination.toIPv6Address();
            in6->sin6_family = AF_INET6;
            std::memcpy(&in6->sin6_addr, bytes.c, sizeof(bytes.c));
            const QString scope = destination.scopeId();
            bool numeric = false;
            const ulong scopeId = scope.toULong(&numeric);
            in6->sin6_scope_id = numeric ? scopeId
                                         : QNetworkInterface::interfaceIndexFromName(scope);
            in6->sin6_port = htons(port);
        } else {
            auto* in4 = reinterpret_cast<sockaddr_in*>(&api.destination);
            in4->sin_family = AF_INET;
            in4->sin_addr.s_addr = htonl(address);
            in4->sin_port = htons(port);
        }
        const std::optional<QOS_FLOWID> flow = createVitaEfFlow(api);
        if (!flow) {
            // Roll back earlier destinations too; retain no partially
            // configured set of qWAVE flows on this socket.
            reset();
            return {false, error};
        }
        m_native->flows.append(*flow);
    }
    return {true, QStringLiteral("qWAVE accepted EF for outgoing UDP endpoints")};
#elif defined(Q_OS_UNIX)
    Q_UNUSED(ports);
    const int fd = static_cast<int>(descriptor);
    // Native IPv6 packets take their class from IPV6_TCLASS; IP_TOS on an
    // IPv6 socket reads back fine but marks only IPv4-mapped traffic, which a
    // dual-stack socket sends to an IPv4 destination.
    sockaddr_storage local{};
    socklen_t localLength = sizeof(local);
    const bool ipv6Socket = ::getsockname(fd, reinterpret_cast<sockaddr*>(&local), &localLength) == 0
        && local.ss_family == AF_INET6;
    bool ipv4Destination = false;
    destination.toIPv4Address(&ipv4Destination);
    const bool ipv6 = ipv6Socket && !ipv4Destination;
    const int level = ipv6 ? IPPROTO_IPV6 : IPPROTO_IP;
    const int option = ipv6 ? IPV6_TCLASS : IP_TOS;
    const QString name = ipv6 ? QStringLiteral("IPV6_TCLASS") : QStringLiteral("IP_TOS");
    const int tos = kVitaIpTosEf;
    if (::setsockopt(fd, level, option, &tos, sizeof(tos)) != 0) {
        return {false, QString::fromLocal8Bit(std::strerror(errno))};
    }
    int granted = 0;
    socklen_t length = sizeof(granted);
    if (::getsockopt(fd, level, option, &granted, &length) != 0) {
        return {false, QString::fromLocal8Bit(std::strerror(errno))};
    }
    if (granted != tos) {
        return {false, QStringLiteral("%1 readback %2 differs from requested %3")
                           .arg(name).arg(granted).arg(tos)};
    }
    return {true, QStringLiteral("%1 readback 0xB8").arg(name)};
#else
    Q_UNUSED(destination);
    Q_UNUSED(ports);
    return {false, QStringLiteral("DSCP marking unsupported on this platform")};
#endif
}

} // namespace AetherSDR
