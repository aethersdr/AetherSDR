#include "WindowsFirewall.h"

#include <QDir>

#ifdef Q_OS_WIN
#include <windows.h>
#include <netfw.h>
#include <shellapi.h>
#endif

namespace AetherSDR::WindowsFirewall {

namespace {

constexpr int kProtocolTcp = 6;
constexpr int kProtocolUdp = 17;
constexpr int kProtocolAny = 256;

QString profileNames(int profiles)
{
    if ((profiles & 0x7) == 0x7) {
        return QStringLiteral("all networks");
    }
    QStringList names;
    if (profiles & kProfileDomain) names << QStringLiteral("domain");
    if (profiles & kProfilePrivate) names << QStringLiteral("private");
    if (profiles & kProfilePublic) names << QStringLiteral("public");
    return names.isEmpty() ? QStringLiteral("no networks") : names.join(QStringLiteral(", "));
}

QString protocolName(int protocol)
{
    switch (protocol) {
    case kProtocolTcp: return QStringLiteral("TCP");
    case kProtocolUdp: return QStringLiteral("UDP");
    case kProtocolAny: return QStringLiteral("any protocol");
    default: return QStringLiteral("protocol %1").arg(protocol);
    }
}

QString describe(const Rule& r)
{
    return QStringLiteral("\"%1\": %2 %3 %4, %5%6")
        .arg(r.name,
             r.allow ? QStringLiteral("allow") : QStringLiteral("block"),
             r.inbound ? QStringLiteral("inbound") : QStringLiteral("outbound"),
             protocolName(r.protocol),
             profileNames(r.profiles),
             r.enabled ? QString() : QStringLiteral(" (disabled)"));
}

bool covers(const Rule& r, int profiles)
{
    return r.enabled && (r.profiles & profiles) != 0;
}

bool coversProtocol(const Rule& r, int protocol)
{
    return r.protocol == kProtocolAny || r.protocol == protocol;
}

} // namespace

Assessment assess(const Status& status)
{
    Assessment a;
    if (!status.inspected) {
        a.summary = status.error.isEmpty()
            ? QStringLiteral("Windows Firewall status is not available on this system.")
            : QStringLiteral("Windows Firewall could not be read: %1").arg(status.error);
        return a;
    }
    if (!status.thirdPartyFirewalls.isEmpty()) {
        a.verdict = Verdict::ThirdParty;
        a.summary = QStringLiteral(
            "%1 manages the firewall on this PC. AetherSDR cannot read its rules; if "
            "external services or radio discovery fail, allow AetherSDR in %1.")
            .arg(status.thirdPartyFirewalls.join(QStringLiteral(" and ")));
        a.details = status.thirdPartyFirewalls;
        return a;
    }
    if (!status.enabledOnCurrent) {
        a.verdict = Verdict::Disabled;
        a.summary = QStringLiteral("Windows Defender Firewall is off for the active network.");
        return a;
    }
    const int active = status.currentProfiles;
    for (const Rule& r : status.rules) {
        if (!r.allow && covers(r, active)) {
            a.details << describe(r);
        }
    }
    if (!a.details.isEmpty()) {
        a.verdict = Verdict::Blocked;
        a.fixable = true;
        a.summary = QStringLiteral(
            "Windows Defender Firewall is blocking AetherSDR on the active network. "
            "This usually follows a dismissed firewall prompt after an update.");
        return a;
    }
    bool outboundAllow = false;
    bool inTcp = false;
    bool inUdp = false;
    for (const Rule& r : status.rules) {
        if (!r.allow || !covers(r, active)) {
            continue;
        }
        if (!r.inbound) {
            outboundAllow = true;
        } else {
            inTcp |= coversProtocol(r, kProtocolTcp);
            inUdp |= coversProtocol(r, kProtocolUdp);
        }
    }
    if (status.outboundBlockedByDefault && !outboundAllow) {
        a.verdict = Verdict::OutboundBlocked;
        a.summary = QStringLiteral(
            "The active network's firewall profile blocks outgoing connections by default, "
            "and no rule allows AetherSDR out. Ask whoever manages this PC to allow it.");
        return a;
    }
    for (const Rule& r : status.rules) {
        a.details << describe(r);
    }
    if (!inTcp || !inUdp) {
        a.verdict = Verdict::NoAllowRule;
        a.fixable = true;
        a.summary = QStringLiteral(
            "No firewall rule allows AetherSDR in on the active network yet, so Windows "
            "will ask the first time it listens. Fix adds the rules now.");
        return a;
    }
    a.verdict = Verdict::Allowed;
    a.summary = QStringLiteral("Windows Defender Firewall allows AetherSDR on the active network.");
    return a;
}

QString fixCommandLine(const QString& programPath)
{
    const QString program = QDir::toNativeSeparators(programPath);
    const QString add = QStringLiteral(
        "netsh advfirewall firewall add rule name=\"AetherSDR (%1-In)\" dir=in action=allow "
        "program=\"%2\" enable=yes profile=any protocol=%1");
    return QStringLiteral("/D /C \"netsh advfirewall firewall delete rule name=all program=\"%1\" "
                          ">NUL 2>&1 & %2 & %3\"")
        .arg(program, add.arg(QStringLiteral("TCP"), program),
             add.arg(QStringLiteral("UDP"), program));
}

#ifdef Q_OS_WIN

namespace {

template <typename T>
struct ComPtr {
    T* p{nullptr};
    ~ComPtr() { if (p) p->Release(); }
    T** out() { return &p; }
    T* operator->() const { return p; }
};

struct ComScope {
    bool release{false};
    ComScope()
    {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        // RPC_E_CHANGED_MODE: the thread already has COM in another mode,
        // which is still usable; only a successful init needs a matching uninit.
        release = SUCCEEDED(hr);
    }
    ~ComScope() { if (release) CoUninitialize(); }
};

QString fromBstr(BSTR b)
{
    return b ? QString::fromWCharArray(b, int(SysStringLen(b))) : QString();
}

QString normalisePath(const QString& path)
{
    return QDir::toNativeSeparators(QDir::cleanPath(path)).toCaseFolded();
}

QString hresultText(HRESULT hr)
{
    return QStringLiteral("0x%1").arg(quint32(hr), 8, 16, QLatin1Char('0'));
}

void readThirdParty(Status& status)
{
    ComPtr<INetFwProducts> products;
    if (FAILED(CoCreateInstance(__uuidof(NetFwProducts), nullptr, CLSCTX_INPROC_SERVER,
                                __uuidof(INetFwProducts), reinterpret_cast<void**>(products.out())))) {
        return;
    }
    long count = 0;
    if (FAILED(products->get_Count(&count))) {
        return;
    }
    for (long i = 0; i < count; ++i) {
        ComPtr<INetFwProduct> product;
        if (FAILED(products->Item(i, product.out())) || !product.p) {
            continue;
        }
        BSTR name = nullptr;
        if (SUCCEEDED(product->get_DisplayName(&name))) {
            const QString n = fromBstr(name).trimmed();
            if (!n.isEmpty()) {
                status.thirdPartyFirewalls << n;
            }
        }
        SysFreeString(name);
    }
}

} // namespace

Status inspect(const QString& programPath)
{
    Status status;
    status.programPath = QDir::toNativeSeparators(programPath);
    ComScope com;

    ComPtr<INetFwPolicy2> policy;
    HRESULT hr = CoCreateInstance(__uuidof(NetFwPolicy2), nullptr, CLSCTX_INPROC_SERVER,
                                  __uuidof(INetFwPolicy2), reinterpret_cast<void**>(policy.out()));
    if (FAILED(hr)) {
        status.error = QStringLiteral("firewall policy unavailable (%1)").arg(hresultText(hr));
        return status;
    }

    long profiles = 0;
    if (FAILED(policy->get_CurrentProfileTypes(&profiles))) {
        status.error = QStringLiteral("active network profile unavailable");
        return status;
    }
    status.currentProfiles = int(profiles);
    for (const int bit : {kProfileDomain, kProfilePrivate, kProfilePublic}) {
        if (!(profiles & bit)) {
            continue;
        }
        const auto type = static_cast<NET_FW_PROFILE_TYPE2>(bit);
        VARIANT_BOOL on = VARIANT_FALSE;
        if (SUCCEEDED(policy->get_FirewallEnabled(type, &on)) && on == VARIANT_TRUE) {
            status.enabledOnCurrent = true;
        }
        NET_FW_ACTION action = NET_FW_ACTION_ALLOW;
        if (SUCCEEDED(policy->get_DefaultOutboundAction(type, &action))
            && action == NET_FW_ACTION_BLOCK) {
            status.outboundBlockedByDefault = true;
        }
    }

    readThirdParty(status);

    ComPtr<INetFwRules> rules;
    if (FAILED(policy->get_Rules(rules.out()))) {
        status.error = QStringLiteral("firewall rules unavailable");
        return status;
    }
    ComPtr<IUnknown> enumUnknown;
    ComPtr<IEnumVARIANT> rulesEnum;
    if (FAILED(rules->get__NewEnum(enumUnknown.out()))
        || FAILED(enumUnknown->QueryInterface(__uuidof(IEnumVARIANT),
                                              reinterpret_cast<void**>(rulesEnum.out())))) {
        status.error = QStringLiteral("firewall rules could not be enumerated");
        return status;
    }

    const QString wanted = normalisePath(programPath);
    VARIANT item;
    VariantInit(&item);
    ULONG fetched = 0;
    while (rulesEnum->Next(1, &item, &fetched) == S_OK && fetched == 1) {
        ComPtr<INetFwRule> rule;
        if (item.vt == VT_DISPATCH && item.pdispVal
            && SUCCEEDED(item.pdispVal->QueryInterface(__uuidof(INetFwRule),
                                                        reinterpret_cast<void**>(rule.out())))) {
            BSTR app = nullptr;
            if (SUCCEEDED(rule->get_ApplicationName(&app)) && app
                && normalisePath(fromBstr(app)) == wanted) {
                Rule r;
                BSTR name = nullptr;
                rule->get_Name(&name);
                r.name = fromBstr(name);
                SysFreeString(name);
                NET_FW_RULE_DIRECTION dir = NET_FW_RULE_DIR_IN;
                rule->get_Direction(&dir);
                r.inbound = dir == NET_FW_RULE_DIR_IN;
                NET_FW_ACTION action = NET_FW_ACTION_ALLOW;
                rule->get_Action(&action);
                r.allow = action == NET_FW_ACTION_ALLOW;
                VARIANT_BOOL enabled = VARIANT_FALSE;
                rule->get_Enabled(&enabled);
                r.enabled = enabled == VARIANT_TRUE;
                long ruleProfiles = 0;
                rule->get_Profiles(&ruleProfiles);
                r.profiles = int(ruleProfiles);
                long protocol = kProtocolAny;
                rule->get_Protocol(&protocol);
                r.protocol = int(protocol);
                status.rules << r;
            }
            SysFreeString(app);
        }
        VariantClear(&item);
    }
    status.inspected = true;
    return status;
}

bool runFix(const QString& programPath, QString* error)
{
    wchar_t systemDir[MAX_PATH] = {};
    GetSystemDirectoryW(systemDir, MAX_PATH);
    const QString cmd = QString::fromWCharArray(systemDir) + QStringLiteral("\\cmd.exe");
    const std::wstring file = cmd.toStdWString();
    const std::wstring params = fixCommandLine(programPath).toStdWString();

    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    info.lpVerb = L"runas";
    info.lpFile = file.c_str();
    info.lpParameters = params.c_str();
    info.nShow = SW_HIDE;
    if (!ShellExecuteExW(&info)) {
        if (error) {
            *error = GetLastError() == ERROR_CANCELLED
                ? QStringLiteral("The Windows permission prompt was declined.")
                : QStringLiteral("Windows could not start the firewall update (error %1).")
                      .arg(GetLastError());
        }
        return false;
    }
    if (info.hProcess) {
        const DWORD waited = WaitForSingleObject(info.hProcess, 60000);
        DWORD code = 0;
        GetExitCodeProcess(info.hProcess, &code);
        CloseHandle(info.hProcess);
        if (waited != WAIT_OBJECT_0) {
            if (error) {
                *error = QStringLiteral("The firewall update did not finish within a minute.");
            }
            return false;
        }
        if (code != 0) {
            if (error) {
                *error = QStringLiteral("netsh reported an error (exit code %1).").arg(code);
            }
            return false;
        }
    }
    return true;
}

#else

Status inspect(const QString& programPath)
{
    Status status;
    status.programPath = programPath;
    return status;
}

bool runFix(const QString&, QString* error)
{
    if (error) {
        *error = QStringLiteral("Windows Firewall is only available on Windows.");
    }
    return false;
}

#endif

} // namespace AetherSDR::WindowsFirewall
