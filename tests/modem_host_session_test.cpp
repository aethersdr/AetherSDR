#include "core/hfgateway/ModemHostSession.h"

#include <cstdio>

using namespace AetherSDR::ModemHost;

namespace {

int g_failed = 0;

void report(const char* name, bool ok)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", name);
    if (!ok) {
        ++g_failed;
    }
}

}  // namespace

int main()
{
    // ── Transmit permission ─────────────────────────────────────────────────
    {
        Session s;
        report("permission starts off", !s.transmitPermitted());
        report("LISTEN ON refused without permission",
               s.request(Request::ListenOn).isEmpty() && s.refusal() == Refusal::NotPermitted);
        report("refused request is not queued", s.outstanding() == 0);
        report("data refused without permission", !s.mayWriteData());
        report("LISTEN OFF always allowed", s.request(Request::ListenOff) == "LISTEN OFF\r");
        report("DISCONNECT always allowed", s.request(Request::Disconnect) == "DISCONNECT\r");
        report("ABORT always allowed", s.request(Request::Abort) == "ABORT\r");
        report("MYCALL allowed", s.request(Request::MyCall, {"N0CALL"}) == "MYCALL N0CALL\r");
        report("VERSION allowed", s.request(Request::Version) == "VERSION\r");
        report("MONITOR allowed without permission", s.request(Request::MonitorOn) == "MONITOR ON\r"
                                                          && s.request(Request::MonitorOff) == "MONITOR OFF\r");
        s.setTransmitPermitted(true);
        report("LISTEN ON allowed with permission", s.request(Request::ListenOn) == "LISTEN ON\r");
        report("data allowed with permission", s.mayWriteData());
        s.setTransmitPermitted(false);
        report("revoking permission refuses LISTEN ON again", s.request(Request::ListenOn).isEmpty());
        report("revoking permission refuses data again", !s.mayWriteData());
    }

    // ── Invalid arguments ───────────────────────────────────────────────────
    {
        Session s;
        report("MYCALL with no calls refused", s.request(Request::MyCall, {}).isEmpty()
                                                   && s.refusal() == Refusal::Invalid && s.outstanding() == 0);
        report("MYCALL with a bad call refused", s.request(Request::MyCall, {"bad call"}).isEmpty());
    }

    // ── Positional reply matching ───────────────────────────────────────────
    {
        Session s;
        s.request(Request::MyCall, {"N0CALL"});
        s.request(Request::ListenOff);
        s.request(Request::Version);
        report("three outstanding", s.outstanding() == 3);

        Session::Event e = s.onLine("OK");
        report("first OK answers MYCALL", e.kind == Session::Event::Kind::Reply && e.request == Request::MyCall && e.ok);
        e = s.onLine("REGISTERED N0CALL");
        report("REGISTERED after OK is a notification",
               e.kind == Session::Event::Kind::Notification
                   && e.notification.type == NotificationType::Registered && s.outstanding() == 2);
        e = s.onLine("WRONG");
        report("WRONG answers LISTEN OFF", e.kind == Session::Event::Kind::Reply && e.request == Request::ListenOff
                                               && !e.ok);
        e = s.onLine("VERSION Data2G 0.5.0");
        report("VERSION line answers VERSION", e.kind == Session::Event::Kind::Reply && e.request == Request::Version
                                                   && e.ok && e.version == "Data2G 0.5.0" && s.outstanding() == 0);
    }
    {
        // Mercury's reply to VERSION doesn't start with VERSION.
        Session s;
        s.request(Request::ListenOff);
        s.request(Request::Version);
        s.request(Request::Disconnect);
        s.onLine("OK");
        const Session::Event v = s.onLine("VARA version 4.9.0 registered");
        report("free-text line answers VERSION", v.kind == Session::Event::Kind::Reply
                                                     && v.request == Request::Version && v.ok
                                                     && v.version == "VARA version 4.9.0 registered");
        const Session::Event m = s.onLine("OK");
        report("the next OK answers the next command, not VERSION", m.kind == Session::Event::Kind::Reply
                                                                       && m.request == Request::Disconnect
                                                                       && s.outstanding() == 0);
    }
    {
        Session s;
        s.request(Request::ListenOff);
        const Session::Event u = s.onLine("SOMETHING NEW");
        report("unknown line while VERSION isn't oldest stays a notification",
               u.kind == Session::Event::Kind::Notification && s.outstanding() == 1);
    }
    {
        Session s;
        s.request(Request::Version);
        const Session::Event e = s.onLine("WRONG");
        report("WRONG answers VERSION too", e.kind == Session::Event::Kind::Reply && e.request == Request::Version
                                                && !e.ok && s.outstanding() == 0);
    }
    {
        Session s;
        s.request(Request::ListenOff);
        const Session::Event v = s.onLine("VERSION Mercury 2.0");
        report("unsolicited VERSION while another command waits is a notification",
               v.kind == Session::Event::Kind::Notification && s.outstanding() == 1);
        const Session::Event b = s.onLine("BUFFER 10");
        report("BUFFER never consumes a reply", b.kind == Session::Event::Kind::Notification
                                                    && b.notification.type == NotificationType::Buffer
                                                    && s.outstanding() == 1);
        const Session::Event p = s.onLine("PTT ON");
        report("PTT is a notification", p.kind == Session::Event::Kind::Notification && p.notification.on);
    }
    {
        Session s;
        const Session::Event e = s.onLine("OK");
        report("OK with nothing outstanding is unmatched", e.kind == Session::Event::Kind::Unmatched);
    }

    // ── Queue cap and reset ─────────────────────────────────────────────────
    {
        Session s;
        for (int i = 0; i < Session::kMaxOutstanding; ++i) {
            s.request(Request::Version);
        }
        report("queue holds kMaxOutstanding", s.outstanding() == Session::kMaxOutstanding);
        report("one more ordinary command is refused", s.request(Request::Version).isEmpty()
                                                           && s.refusal() == Refusal::QueueFull);
        report("ordinary MYCALL refused too", s.request(Request::MyCall, {"N0CALL"}).isEmpty());
        report("stop commands still go out past the soft cap", s.request(Request::ListenOff) == "LISTEN OFF\r"
                                                                    && s.request(Request::Disconnect) == "DISCONNECT\r"
                                                                    && s.request(Request::Abort) == "ABORT\r");
        while (s.outstanding() < Session::kHardMaxOutstanding) {
            s.request(Request::Abort);
        }
        report("stop commands refused at the hard cap", s.request(Request::Abort).isEmpty()
                                                            && s.refusal() == Refusal::QueueFull);
        s.reset();
        report("reset clears the queue", s.outstanding() == 0);
        report("after reset requests work", !s.request(Request::ListenOff).isEmpty());
    }

    if (g_failed != 0) {
        std::printf("\n%d check(s) failed\n", g_failed);
        return 1;
    }
    std::printf("\nall checks passed\n");
    return 0;
}
