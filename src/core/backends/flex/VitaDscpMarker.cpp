#include "VitaDscpMarker.h"
#include "VitaDscpFlow.h"

#include <QVector>

#if defined(Q_OS_WIN)
#include <winsock2.h>
#include <windows.h>
#include <qos2.h>
#elif defined(Q_OS_UNIX)
#include <cerrno>
#include <cstring>
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
        sockaddr_in destination;
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
    if (!ipv4 || ports.size() == 0) {
        return {false, QStringLiteral("IPv4 destination and UDP port required")};
    }
    QString error;
    if (!m_native->open(error)) {
        return {false, error};
    }
    for (quint16 port : ports) {
        NativeState::FlowApi api{*m_native, static_cast<SOCKET>(descriptor), {}, error};
        api.destination.sin_family = AF_INET;
        api.destination.sin_addr.s_addr = htonl(address);
        api.destination.sin_port = htons(port);
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
    Q_UNUSED(destination);
    Q_UNUSED(ports);
    const int tos = kVitaIpTosEf;
    if (::setsockopt(static_cast<int>(descriptor), IPPROTO_IP, IP_TOS, &tos, sizeof(tos)) != 0) {
        return {false, QString::fromLocal8Bit(std::strerror(errno))};
    }
    int granted = 0;
    socklen_t length = sizeof(granted);
    if (::getsockopt(static_cast<int>(descriptor), IPPROTO_IP, IP_TOS, &granted, &length) != 0) {
        return {false, QString::fromLocal8Bit(std::strerror(errno))};
    }
    if (granted != tos) {
        return {false, QStringLiteral("IP_TOS readback %1 differs from requested %2").arg(granted).arg(tos)};
    }
    return {true, QStringLiteral("IP_TOS readback 0xB8")};
#else
    Q_UNUSED(destination);
    Q_UNUSED(ports);
    return {false, QStringLiteral("DSCP marking unsupported on this platform")};
#endif
}

} // namespace AetherSDR
