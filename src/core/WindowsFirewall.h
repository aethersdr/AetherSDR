#pragma once

#include <QList>
#include <QString>
#include <QStringList>

namespace AetherSDR::WindowsFirewall {

// Profile bits, as NET_FW_PROFILE_TYPE2 defines them.
inline constexpr int kProfileDomain = 0x1;
inline constexpr int kProfilePrivate = 0x2;
inline constexpr int kProfilePublic = 0x4;

struct Rule {
    QString name;
    bool inbound{true};
    bool allow{true};
    bool enabled{true};
    int profiles{0};      // kProfile* bits; 0x7fffffff is "all"
    int protocol{256};    // 6 TCP, 17 UDP, 256 any
};

// What Windows Defender Firewall says about one program. Reading it needs no
// elevation. Only Windows fills it in; elsewhere inspected stays false.
struct Status {
    bool inspected{false};
    QString error;                    // why inspection failed, when it did
    QString programPath;
    QStringList thirdPartyFirewalls;  // products that have taken over the firewall
    int currentProfiles{0};           // kProfile* bits of the active networks
    bool enabledOnCurrent{false};     // the firewall is on for an active profile
    bool outboundBlockedByDefault{false};
    QList<Rule> rules;                // rules whose program is programPath
};

enum class Verdict {
    NotInspected,       // not Windows, or the firewall could not be read
    ThirdParty,         // another product manages the firewall; we cannot see its rules
    Disabled,           // the firewall is off for the active networks
    Blocked,            // an enabled block rule for this program covers an active network
    OutboundBlocked,    // the active profile blocks outbound by default, no allow rule
    NoAllowRule,        // nothing blocks yet, but Windows will prompt on first listen
    Allowed,            // enabled inbound allow rules cover TCP and UDP on the active networks
};

struct Assessment {
    Verdict verdict{Verdict::NotInspected};
    QString summary;      // one sentence for the operator
    QStringList details;  // the rules or products behind the verdict
    bool fixable{false};  // Fix can resolve it (Windows Defender Firewall only)
};

Status inspect(const QString& programPath);
Assessment assess(const Status& status);

// The netsh commands Fix runs, elevated, in one cmd.exe: delete every rule for
// the program (including a block rule left by a dismissed prompt — block beats
// allow) and add inbound TCP and UDP allow rules on all profiles. The rule
// names match the installer's, so uninstall removes them.
QString fixCommandLine(const QString& programPath);

// Runs fixCommandLine() through a UAC prompt and waits for it. Blocking: call
// it off the GUI thread. Returns false with error set when the operator
// declined, or on any other failure.
bool runFix(const QString& programPath, QString* error);

} // namespace AetherSDR::WindowsFirewall
