#include "WindowsFirewall.h"

#include <QDir>

#include <utility>

#ifdef Q_OS_WIN
#include <windows.h>
#include <netfw.h>
#include <shellapi.h>
#endif

namespace AetherSDR::WindowsFirewall {

namespace {

constexpr int kAllProfiles = kProfileDomain | kProfilePrivate | kProfilePublic;

QString profileNames(int profiles)
{
    if ((profiles & kAllProfiles) == kAllProfiles) {
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
    return QStringLiteral("\"%1\": %2 %3 %4, %5%6%7")
        .arg(r.name,
             r.allow ? QStringLiteral("allow") : QStringLiteral("block"),
             r.inbound ? QStringLiteral("inbound") : QStringLiteral("outbound"),
             protocolName(r.protocol),
             profileNames(r.profiles),
             r.restricted ? QStringLiteral(", limited to some ports or addresses") : QString(),
             r.enabled ? QString() : QStringLiteral(" (disabled)"));
}

bool appliesTo(const Rule& r, int profile)
{
    return r.enabled && (r.profiles & profile) != 0;
}

bool carries(const Rule& r, int protocol)
{
    return r.protocol == kProtocolAny || r.protocol == protocol;
}

// An enabled, unrestricted allow in this direction covers this profile and protocol.
bool allowed(const QList<Rule>& rules, bool inbound, int profile, int protocol)
{
    for (const Rule& r : rules) {
        if (r.allow && !r.restricted && r.inbound == inbound && appliesTo(r, profile)
            && carries(r, protocol)) {
            return true;
        }
    }
    return false;
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

    QList<Profile> enabled;
    for (const Profile& p : status.activeProfiles) {
        if (p.enabled) enabled << p;
    }
    if (enabled.isEmpty()) {
        a.verdict = Verdict::Disabled;
        a.summary = QStringLiteral("Windows Defender Firewall is off for the active networks.");
        return a;
    }

    bool outboundBlockRule = false;
    for (const Rule& r : status.rules) {
        if (r.allow) continue;
        for (const Profile& p : enabled) {
            if (appliesTo(r, p.bit)) {
                a.details << describe(r);
                outboundBlockRule |= !r.inbound;
                break;
            }
        }
    }
    if (!a.details.isEmpty()) {
        a.verdict = Verdict::Blocked;
        a.fixable = !outboundBlockRule;
        a.summary = outboundBlockRule
            ? QStringLiteral("A Windows Defender Firewall rule blocks AetherSDR's outgoing "
                             "connections on an active network. Ask whoever manages this PC to "
                             "remove it; Fix only repairs incoming rules.")
            : QStringLiteral("Windows Defender Firewall is blocking AetherSDR on an active "
                             "network. This usually follows a dismissed firewall prompt after "
                             "an update.");
        return a;
    }

    for (const Profile& p : enabled) {
        if (p.outboundBlockedByDefault
            && (!allowed(status.rules, false, p.bit, kProtocolTcp)
                || !allowed(status.rules, false, p.bit, kProtocolUdp))) {
            a.details << QStringLiteral("%1 network: outgoing connections blocked by default, "
                                        "no rule allows AetherSDR out over both TCP and UDP")
                             .arg(profileNames(p.bit));
        }
    }
    if (!a.details.isEmpty()) {
        a.verdict = Verdict::OutboundBlocked;
        a.summary = QStringLiteral(
            "An active network's firewall profile blocks outgoing connections by default, and "
            "AetherSDR is not allowed out. Ask whoever manages this PC to allow it.");
        return a;
    }

    for (const Profile& p : enabled) {
        QStringList missing;
        if (!allowed(status.rules, true, p.bit, kProtocolTcp)) missing << QStringLiteral("TCP");
        if (!allowed(status.rules, true, p.bit, kProtocolUdp)) missing << QStringLiteral("UDP");
        if (!missing.isEmpty()) {
            a.details << QStringLiteral("%1 network: no rule allows incoming %2")
                             .arg(profileNames(p.bit), missing.join(QStringLiteral(" or ")));
        }
    }
    if (!a.details.isEmpty()) {
        a.verdict = Verdict::NoAllowRule;
        a.fixable = true;
        a.summary = QStringLiteral(
            "No firewall rule allows AetherSDR in on every active network yet, so Windows "
            "will ask the first time it listens. Fix adds the rules now.");
        return a;
    }

    a.verdict = Verdict::Allowed;
    for (const Rule& r : status.rules) {
        a.details << describe(r);
    }
    a.summary = QStringLiteral(
        "AetherSDR's own Windows Defender Firewall rules allow it on every active network. "
        "Rules for ports or services rather than programs are not checked.");
    return a;
}

QString fixCommandLine(const QString& programPath)
{
    const QString program = QDir::toNativeSeparators(programPath);
    const QString add = QStringLiteral(
        "netsh advfirewall firewall add rule name=\"AetherSDR (%1-In)\" dir=in action=allow "
        "program=\"%2\" enable=yes profile=any protocol=%1");
    return QStringLiteral("/D /C \"netsh advfirewall firewall delete rule name=all dir=in "
                          "program=\"%1\" >NUL 2>&1 & %2 & %3\"")
        .arg(program, add.arg(QStringLiteral("TCP"), program),
             add.arg(QStringLiteral("UDP"), program));
}

Status afterFix(const Status& status)
{
    Status out = status;
    out.rules.clear();
    for (const Rule& r : status.rules) {
        if (!r.inbound) out.rules << r;
    }
    for (const auto& [name, protocol] : {std::pair{QStringLiteral("AetherSDR (TCP-In)"), kProtocolTcp},
                                         std::pair{QStringLiteral("AetherSDR (UDP-In)"), kProtocolUdp}}) {
        Rule r;
        r.name = name;
        r.profiles = 0x7fffffff;
        r.protocol = protocol;
        out.rules << r;
    }
    return out;
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

// A rule limited to some ports, addresses or interface types does not cover
// every connection the program makes.
bool isRestricted(INetFwRule* rule)
{
    auto limited = [](HRESULT hr, BSTR value, const wchar_t* wildcard) {
        const QString v = SUCCEEDED(hr) ? fromBstr(value).trimmed() : QString();
        SysFreeString(value);
        return !v.isEmpty() && v.compare(QString::fromWCharArray(wildcard), Qt::CaseInsensitive) != 0;
    };
    BSTR value = nullptr;
    HRESULT hr = rule->get_LocalPorts(&value);
    if (limited(hr, value, L"*")) return true;
    value = nullptr;
    hr = rule->get_RemotePorts(&value);
    if (limited(hr, value, L"*")) return true;
    value = nullptr;
    hr = rule->get_LocalAddresses(&value);
    if (limited(hr, value, L"*")) return true;
    value = nullptr;
    hr = rule->get_RemoteAddresses(&value);
    if (limited(hr, value, L"*")) return true;
    value = nullptr;
    hr = rule->get_InterfaceTypes(&value);
    if (limited(hr, value, L"All")) return true;
    VARIANT interfaces;
    VariantInit(&interfaces);
    const bool namedInterfaces = SUCCEEDED(rule->get_Interfaces(&interfaces))
        && interfaces.vt != VT_EMPTY && interfaces.vt != VT_NULL;
    VariantClear(&interfaces);
    return namedInterfaces;
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
    for (const int bit : {kProfileDomain, kProfilePrivate, kProfilePublic}) {
        if (!(profiles & bit)) {
            continue;
        }
        const auto type = static_cast<NET_FW_PROFILE_TYPE2>(bit);
        Profile p;
        p.bit = bit;
        VARIANT_BOOL on = VARIANT_TRUE;
        if (SUCCEEDED(policy->get_FirewallEnabled(type, &on))) {
            p.enabled = on == VARIANT_TRUE;
        }
        NET_FW_ACTION action = NET_FW_ACTION_ALLOW;
        if (SUCCEEDED(policy->get_DefaultOutboundAction(type, &action))) {
            p.outboundBlockedByDefault = action == NET_FW_ACTION_BLOCK;
        }
        status.activeProfiles << p;
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
                r.restricted = isRestricted(rule.p);
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
