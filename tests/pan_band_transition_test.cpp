// Unit tests for PanBandTransitions — band changes awaiting their reply, during
// which the adaptive throttle neither restores nor caps the pan (#6791).
//
// The second half replays the review's scenarios against a radio that keeps
// FPS and waterfall rate per band, with its status and replies held back
// until the test delivers them. The client side follows the same rules as
// the wiring in MainWindow and RadioModel:
//   - band change while capped: restore first, unless a band change is in
//     flight;
//   - cap (tier change, or the band change settling): never while in flight;
//   - disconnect, pan close: restore every pan not in flight.
// Run with the in-flight guard disabled, the same steps reproduce the bug:
// the band in between gets the first band's rates.

#include "models/PanBandTransitions.h"

#include <QString>

#include <cstdio>
#include <deque>
#include <map>
#include <set>

using namespace AetherSDR;

namespace {
int g_failed = 0;
int g_total = 0;
void report(const char* label, bool ok)
{
    ++g_total;
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", label);
    if (!ok) {
        ++g_failed;
    }
}

struct Rates {
    int fps{0};
    int wf{0};
    bool operator==(const Rates& o) const { return fps == o.fps && wf == o.wf; }
};

// One pan on a radio that saves the live rates in the band it leaves and
// loads the saved rates of the band it enters.
struct Radio {
    std::map<QString, Rates> saved;
    QString band;
    Rates live;
    void setRates(Rates r) { live = r; }
    Rates switchBand(const QString& to)
    {
        saved[band] = live;
        band = to;
        live = saved[to];
        return live;
    }
};

struct Message {
    bool isReply{false};
    Rates status;       // band status: the new band's own rates
    quint64 token{0};   // reply: settles this band change
};

struct Client {
    bool guard{true};               // false = the pre-fix behaviour
    Radio* radio{nullptr};
    const QString panId{QStringLiteral("0x40000000")};
    PanBandTransitions transitions;
    Rates widget;                   // the restore target
    bool throttled{false};
    int cap{0};
    std::set<int> capsSent;
    std::set<int> wfCapsSent;
    std::deque<Message> inbox;      // held until deliver()

    static int wfForCap(int c) { return c == 4 ? 65 : c == 8 ? 75 : 83; }
    bool inFlight() const { return guard && transitions.inFlight(panId); }

    void sendCap()
    {
        if (inFlight()) return;
        radio->setRates({cap, wfForCap(cap)});
        capsSent.insert(cap);
        wfCapsSent.insert(wfForCap(cap));
    }
    void engage(int c) { throttled = true; cap = c; sendCap(); }
    void restore()
    {
        if (!inFlight()) radio->setRates(widget);
    }
    void bandChange(const QString& to)
    {
        if (throttled) restore();
        const quint64 token = transitions.begin(panId);
        inbox.push_back({false, radio->switchBand(to), 0});
        inbox.push_back({true, {}, token});
    }
    void deliverAll()
    {
        while (!inbox.empty()) {
            const Message m = inbox.front();
            inbox.pop_front();
            if (!m.isReply) {
                if (!throttled || !capsSent.count(m.status.fps)) widget.fps = m.status.fps;
                if (!throttled || !wfCapsSent.count(m.status.wf)) widget.wf = m.status.wf;
            } else if (transitions.settle(panId, m.token) && throttled) {
                sendCap();
            }
        }
    }
    void disconnect()
    {
        if (throttled) restore();
        transitions.clear();
        inbox.clear();
        radio->saved[radio->band] = radio->live;
    }
};

// 20 m 25/80, 40 m 30/90, 80 m 20/70; on 20 m, capped at 8.
Radio freshRadio()
{
    Radio r;
    r.saved = {{QStringLiteral("20"), {25, 80}},
               {QStringLiteral("40"), {30, 90}},
               {QStringLiteral("80"), {20, 70}}};
    r.band = QStringLiteral("20");
    r.live = {25, 80};
    return r;
}

Client clientOn(Radio& r, bool guard)
{
    Client c;
    c.guard = guard;
    c.radio = &r;
    c.widget = r.live;
    c.engage(8);
    return c;
}

bool bandsUntouched(const Radio& r)
{
    return r.saved.at(QStringLiteral("20")) == Rates{25, 80}
        && r.saved.at(QStringLiteral("40")) == Rates{30, 90}
        && r.saved.at(QStringLiteral("80")) == Rates{20, 70};
}
}  // namespace

int main()
{
    // ── The tracker itself. ─────────────────────────────────────────────────
    {
        PanBandTransitions t;
        const QString pan = QStringLiteral("0x40000000");
        report("idle pan is not in flight", !t.inFlight(pan));
        const quint64 a = t.begin(pan);
        const quint64 b = t.begin(pan);
        report("two band changes: in flight", t.inFlight(pan));
        report("first reply (superseded, Cancelled) does not settle",
               !t.settle(pan, a) && t.inFlight(pan));
        report("same reply again changes nothing", !t.settle(pan, a) && t.inFlight(pan));
        report("last reply settles", t.settle(pan, b) && !t.inFlight(pan));
        report("settling a settled token is a no-op", !t.settle(pan, b));
    }
    {
        PanBandTransitions t;
        const QString pan = QStringLiteral("0x40000001");
        const quint64 a = t.begin(pan);
        t.forget(pan);
        report("forget: pan no longer in flight", !t.inFlight(pan));
        const quint64 b = t.begin(pan);  // pan ID reused by the next pan
        report("reply for the closed pan does not settle the reused ID",
               !t.settle(pan, a) && t.inFlight(pan));
        t.clear();
        report("clear (disconnect): nothing in flight", !t.inFlight(pan));
        report("late reply after clear does not settle", !t.settle(pan, b));
    }
    {
        PanBandTransitions t;
        const QString p0 = QStringLiteral("0x40000000");
        const QString p1 = QStringLiteral("0x40000001");
        const quint64 a = t.begin(p0);
        t.begin(p1);
        report("pans are tracked separately", t.settle(p0, a) && t.inFlight(p1));
    }

    // ── Review scenario 1: two band changes before the first one's status. ──
    {
        Radio r = freshRadio();
        Client c = clientOn(r, true);
        c.bandChange(QStringLiteral("40"));
        c.bandChange(QStringLiteral("80"));
        report("20 m -> 40 m -> 80 m, status delayed: 20 m keeps 25/80",
               r.saved.at(QStringLiteral("20")) == Rates{25, 80});
        report("... 40 m keeps its own 30/90, not 20 m's rates",
               r.saved.at(QStringLiteral("40")) == Rates{30, 90});
        c.deliverAll();
        report("... once settled, 80 m is capped and its 20/70 is the restore target",
               r.live == Rates{8, 75} && c.widget == Rates{20, 70});
        c.disconnect();
        report("... and a later disconnect saves every band's own rates", bandsUntouched(r));
    }
    {
        Radio r = freshRadio();
        Client c = clientOn(r, false);
        c.bandChange(QStringLiteral("40"));
        c.bandChange(QStringLiteral("80"));
        report("guard disabled: 40 m gets 20 m's 25/80 (the reported bug)",
               r.saved.at(QStringLiteral("40")) == Rates{25, 80});
    }

    // ── Review scenario 2: band change, then disconnect before the status. ──
    {
        Radio r = freshRadio();
        Client c = clientOn(r, true);
        c.bandChange(QStringLiteral("40"));
        c.disconnect();
        report("band change then disconnect: every band keeps its own rates",
               bandsUntouched(r));
    }
    {
        Radio r = freshRadio();
        Client c = clientOn(r, false);
        c.bandChange(QStringLiteral("40"));
        c.disconnect();
        report("guard disabled: the disconnect writes 20 m's rates into 40 m",
               r.saved.at(QStringLiteral("40")) == Rates{25, 80});
    }

    // ── A tier change while the band change is in flight. ───────────────────
    {
        Radio r = freshRadio();
        Client c = clientOn(r, true);
        c.bandChange(QStringLiteral("40"));
        c.engage(4);  // Fair -> Poor before the reply
        report("tier change in flight: the new band is not capped yet",
               r.live == Rates{30, 90});
        c.disconnect();
        report("... so a disconnect then leaves 40 m at its own 30/90",
               r.saved.at(QStringLiteral("40")) == Rates{30, 90});
    }
    {
        Radio r = freshRadio();
        Client c = clientOn(r, true);
        c.bandChange(QStringLiteral("40"));
        c.engage(4);
        c.deliverAll();
        report("tier change in flight: the settle applies the current cap (4)",
               r.live == Rates{4, 65} && c.widget == Rates{30, 90});
    }

    // ── The ordinary path still works. ──────────────────────────────────────
    {
        Radio r = freshRadio();
        Client c = clientOn(r, true);
        c.bandChange(QStringLiteral("40"));
        c.deliverAll();
        c.bandChange(QStringLiteral("20"));
        c.deliverAll();
        c.disconnect();
        report("settled band changes: restore, switch, recap; nothing saved capped",
               bandsUntouched(r));
    }

    std::printf("\n%d/%d passed\n", g_total - g_failed, g_total);
    return g_failed == 0 ? 0 : 1;
}
