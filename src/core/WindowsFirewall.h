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
    QList<Profile> activeProfiles;
    QList<Rule> rules;                // rules whose program is programPath
};

enum class Verdict {
    NotInspected,       // not Windows, or the firewall could not be read
    ThirdParty,         // another product manages the firewall; we cannot see its rules
    Disabled,           // the firewall is off for every active network
    Blocked,            // an enabled block rule for this program covers an active network
    OutboundBlocked,    // an active profile blocks outbound by default and nothing allows us out
    NoAllowRule,        // nothing blocks, but an active network lacks an inbound TCP+UDP allow
    Allowed,            // every active network has the TCP and UDP allows it needs
};

struct Assessment {
    Verdict verdict{Verdict::NotInspected};
    QString summary;      // one sentence for the operator
    QStringList details;  // the rules or products behind the verdict
    bool fixable{false};  // Fix resolves it: an inbound problem in Windows Defender Firewall
};

Status inspect(const QString& programPath);
Assessment assess(const Status& status);

// The netsh commands Fix runs, elevated, in one cmd.exe: delete the program's
// INBOUND rules (including a block rule left by a dismissed prompt — block
// beats allow) and add inbound TCP and UDP allow rules on all profiles.
// Outbound rules are never touched: an administrator's outbound allows on an
// outbound-blocking PC must survive a repair. The rule names match the
// installer's, so uninstall removes them.
QString fixCommandLine(const QString& programPath);

// The rule set fixCommandLine() leaves behind, for checking a repair before
// running it: inbound rules replaced, everything else kept.
Status afterFix(const Status& status);

// Runs fixCommandLine() through a UAC prompt and waits for it. Blocking: call
// it off the GUI thread. Returns false with error set when the operator
// declined, or on any other failure.
bool runFix(const QString& programPath, QString* error);

} // namespace AetherSDR::WindowsFirewall
