#pragma once

#include <QList>
#include <QString>
#include <QStringList>

namespace AetherSDR::WindowsFirewall {

// Profile bits, as NET_FW_PROFILE_TYPE2 defines them.
inline constexpr int kProfileDomain = 0x1;
inline constexpr int kProfilePrivate = 0x2;
inline constexpr int kProfilePublic = 0x4;

inline constexpr int kProtocolTcp = 6;
inline constexpr int kProtocolUdp = 17;
inline constexpr int kProtocolAny = 256;

struct Rule {
    QString name;
    bool inbound{true};
    bool allow{true};
    bool enabled{true};
    int profiles{0};          // kProfile* bits; 0x7fffffff is "all"
    int protocol{kProtocolAny};
    // The rule names ports, addresses or interfaces, so it does not cover
    // every connection the program makes. Such an allow is not counted as
    // coverage; such a block still counts as blocking.
    bool restricted{false};
};

// One active network profile. Several can be active at once (a VPN on a
// domain network, a second adapter on a public one), and each has its own
// on/off state and outbound default.
struct Profile {
    int bit{0};
    bool enabled{true};
    bool outboundBlockedByDefault{false};
};

// What Windows Defender Firewall says about one program. Reading it needs no
// elevation. Only Windows fills it in; elsewhere inspected stays false.
struct Status {
    bool inspected{false};
    QString error;                    // why inspection failed, when it did
    QString programPath;
    QStringList thirdPartyFirewalls;  // products that have taken over the firewall
    bool packaged{false};             // a Store (MSIX) install: Windows owns its rules
    QList<Profile> activeProfiles;
    QList<Rule> rules;                // rules whose program is programPath
};

enum class Verdict {
    NotInspected,       // not Windows, or the firewall could not be read
    NoNetwork,          // no network profile is active
    ThirdParty,         // another product manages the firewall; we cannot see its rules
    Disabled,           // the firewall is off for every active network
    Blocked,            // an enabled block rule for this program covers an active network
    OutboundBlocked,    // an active profile blocks outbound by default and nothing allows us out
    NoAllowRule,        // nothing blocks, but an active network lacks an inbound allow it needs
    Allowed,            // every active network has the allows it needs
};

struct Assessment {
    Verdict verdict{Verdict::NotInspected};
    QString summary;      // one sentence for the operator
    QStringList details;  // the rules or products behind the verdict
    bool fixable{false};  // Fix resolves it: an inbound problem in Windows Defender Firewall,
                          // on an install that is not a Store package
};

Status inspect(const QString& programPath);

// Inbound needs differ by network: UDP everywhere (radio discovery and the
// radio's streams), TCP only on Domain and Private networks. AetherSDR's TCP
// listeners (TCI, CAT, KISS, transfers) bind every interface without
// authentication, and TCI can key the transmitter, so on a Public network
// incoming TCP stays blocked unless the operator allows it in Windows Defender
// Firewall. An inbound TCP block there is that choice, not a fault: it is not
// reported as Blocked, and the Allowed verdict says TCP is not pre-allowed.
Assessment assess(const Status& status);

// The inbound allow rules Fix and the installer add, with the profiles above.
// fixCommandLine() renders exactly these, and afterFix() applies them.
QList<Rule> fixRules();

// The netsh commands Fix runs, elevated, in one cmd.exe: delete the program's
// INBOUND rules (including a block rule left by a dismissed prompt — block
// beats allow), then add fixRules(). Outbound rules are never touched: an
// administrator's outbound allows on an outbound-blocking PC must survive a
// repair. The rule names match the installer's, so uninstall removes them.
// netsh is called by its full path in systemDirectory when one is given.
QString fixCommandLine(const QString& programPath, const QString& systemDirectory = QString());

// The rule set fixCommandLine() leaves behind: inbound rules replaced by
// fixRules(), everything else kept.
Status afterFix(const Status& status);

// Runs fixCommandLine() through a UAC prompt from the system directory and
// waits for it. Blocking: call it off the GUI thread. Returns false with
// error set when the operator declined, or on any other failure.
bool runFix(const QString& programPath, QString* error);

} // namespace AetherSDR::WindowsFirewall
