#include "LocalCredentialHandshake.h"
#include "ControlCredentials.h"

#include <QJsonArray>
#include <QScopeGuard>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <aclapi.h>
#include <array>
#elif defined(Q_OS_UNIX)
#include <sys/socket.h>
#include <unistd.h>
#include <limits>
#endif

namespace AetherSDR::control {

bool localServerIsCurrentUser(qintptr descriptor)
{
    if (descriptor == -1) {
        return false;
    }
#ifdef Q_OS_WIN
    const HANDLE pipe = reinterpret_cast<HANDLE>(descriptor);
    ULONG serverPid = 0;
    if (!GetNamedPipeServerProcessId(pipe, &serverPid) || serverPid == 0) {
        return false;
    }
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, serverPid);
    if (!process) {
        return false;
    }
    const auto closeProcess = qScopeGuard([process] { CloseHandle(process); });
    HANDLE currentToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &currentToken)) {
        return false;
    }
    const auto closeCurrentToken = qScopeGuard([currentToken] { CloseHandle(currentToken); });
    HANDLE serverToken = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &serverToken)) {
        return false;
    }
    const auto closeServerToken = qScopeGuard([serverToken] { CloseHandle(serverToken); });
    alignas(TOKEN_USER) std::array<unsigned char, sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE> currentBytes{};
    alignas(TOKEN_USER) std::array<unsigned char, sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE> serverBytes{};
    DWORD returned = 0;
    if (!GetTokenInformation(currentToken, TokenUser, currentBytes.data(),
                             static_cast<DWORD>(currentBytes.size()), &returned)
        || !GetTokenInformation(serverToken, TokenUser, serverBytes.data(),
                                static_cast<DWORD>(serverBytes.size()), &returned)) {
        return false;
    }
    const PSID currentSid = reinterpret_cast<const TOKEN_USER*>(currentBytes.data())->User.Sid;
    const PSID serverSid = reinterpret_cast<const TOKEN_USER*>(serverBytes.data())->User.Sid;
    if (!IsValidSid(currentSid) || !IsValidSid(serverSid) || !EqualSid(currentSid, serverSid)) {
        return false;
    }
    // Check the connected object too, avoiding reliance on a PID/name lookup
    // alone. Qt's UserAccessOption explicitly sets the pipe owner to TokenUser.
    // This rejects foreign-owner pipes even if their DACL admits our account.
    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR security = nullptr;
    const DWORD result = GetSecurityInfo(pipe, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION,
                                         &owner, nullptr, nullptr, nullptr, &security);
    const auto freeSecurity = qScopeGuard([security] { if (security) { LocalFree(security); } });
    return result == ERROR_SUCCESS && owner && IsValidSid(owner) && EqualSid(currentSid, owner);
#elif defined(Q_OS_MAC) || defined(Q_OS_FREEBSD)
    if (descriptor < 0 || descriptor > std::numeric_limits<int>::max()) {
        return false;
    }
    uid_t uid = 0;
    gid_t gid = 0;
    return getpeereid(static_cast<int>(descriptor), &uid, &gid) == 0 && uid == geteuid();
#elif defined(Q_OS_LINUX)
    if (descriptor < 0 || descriptor > std::numeric_limits<int>::max()) {
        return false;
    }
    struct ucred peer {};
    socklen_t size = sizeof(peer);
    return getsockopt(static_cast<int>(descriptor), SOL_SOCKET, SO_PEERCRED, &peer, &size) == 0
        && size == sizeof(peer) && peer.uid == geteuid();
#else
    return false;
#endif
}

std::optional<QJsonObject> exchangeCredentialHello(
    const QByteArray& secret, const LocalPeerCheck& verifyPeer, const LocalHelloExchange& exchange)
{
    if (secret.size() != ControlCredentials::kSecretBytes || !verifyPeer || !exchange || !verifyPeer()) {
        return {};
    }
    return exchange({{QStringLiteral("v"), 1}, {QStringLiteral("id"), QStringLiteral("admin-hello")},
        {QStringLiteral("method"), QStringLiteral("hello")}, {QStringLiteral("params"), QJsonObject{
            {QStringLiteral("versions"), QJsonArray{1}}, {QStringLiteral("auth"), QJsonObject{
                {QStringLiteral("scheme"), QStringLiteral("bearer")},
                {QStringLiteral("token"), QString::fromLatin1(secret.toHex())}}}}}});
}

} // namespace AetherSDR::control
