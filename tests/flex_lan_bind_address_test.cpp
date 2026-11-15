// Socket-free coverage of the source selection used by LAN start and rebind.
#include "core/backends/flex/LanBindAddress.h"

#include <QHostAddress>
#include <QString>
#include <cstdio>

using namespace AetherSDR;

int main()
{
    struct Case {
        const char* name;
        const char* radio;
        const char* explicitSource;
        const char* sessionSource;
        const char* tcpSource;
        const char* expectedSource;
        const char* expectedReason;
    };
    const Case cases[] = {
        {"IPv4 explicit precedence", "192.0.2.1", "192.0.2.2", "192.0.2.3", "192.0.2.4", "192.0.2.2", "explicit"},
        {"IPv4 probe precedence", "192.0.2.1", "", "192.0.2.3", "192.0.2.4", "192.0.2.3", "probe-session"},
        {"IPv4 TCP source", "192.0.2.1", "", "", "192.0.2.4", "192.0.2.4", "tcp-local"},
        {"IPv4 wildcard", "192.0.2.1", "", "", "", "0.0.0.0", ""},
        {"unknown target legacy wildcard", "", "", "", "", "0.0.0.0", ""},
        {"IPv6 explicit precedence", "2001:db8::1", "2001:db8::2", "2001:db8::3", "2001:db8::4", "2001:db8::2", "explicit"},
        {"IPv6 probe precedence", "2001:db8::1", "", "2001:db8::3", "2001:db8::4", "2001:db8::3", "probe-session"},
        {"IPv6 TCP source", "2001:db8::1", "", "", "2001:db8::4", "2001:db8::4", "tcp-local"},
        {"IPv6 wildcard", "2001:db8::1", "", "", "", "::", ""},
        {"IPv6 ignores IPv4 candidates", "2001:db8::1", "192.0.2.2", "192.0.2.3", "2001:db8::4", "2001:db8::4", "tcp-local"},
        {"IPv4 ignores IPv6 candidates", "192.0.2.1", "2001:db8::2", "2001:db8::3", "192.0.2.4", "192.0.2.4", "tcp-local"},
        {"IPv6 scoped source retained", "fe80::1%en0", "", "fe80::2%en0", "", "fe80::2%en0", "probe-session"},
        {"mapped IPv6 source retained", "::ffff:192.0.2.1", "", "", "::ffff:192.0.2.4", "::ffff:192.0.2.4", "tcp-local"},
    };
    int failures = 0;
    for (const Case& entry : cases) {
        const LanBindAddress selected = selectLanBindAddress(QHostAddress(QString::fromLatin1(entry.radio)),
            QHostAddress(QString::fromLatin1(entry.explicitSource)),
            QHostAddress(QString::fromLatin1(entry.sessionSource)),
            QHostAddress(QString::fromLatin1(entry.tcpSource)));
        const QHostAddress expected(QString::fromLatin1(entry.expectedSource));
        const bool passed = selected.address == expected
            && selected.address.protocol() == expected.protocol()
            && selected.address.scopeId() == expected.scopeId()
            && selected.reason == QString::fromLatin1(entry.expectedReason);
        std::printf("[%s] %s: source=%s reason=%s\n", passed ? " OK " : "FAIL", entry.name,
            selected.address.toString().toUtf8().constData(), selected.reason.toUtf8().constData());
        if (!passed) {
            ++failures;
        }
    }
    return failures == 0 ? 0 : 1;
}
