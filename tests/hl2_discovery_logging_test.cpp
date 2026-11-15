// What HL2 discovery writes to the log when it cannot find a radio (#6285
// item 3): a failed bind, no usable interface, a probe that could not be sent,
// and a run of unanswered sweeps. A sweep no probe left is not unanswered.
//
// Socket-free. DiscoveryNotices is the policy Hl2Discovery feeds from its
// sweeps; the lines are captured off the real "aether.discovery" category,
// with the product's default filter rule installed, so a line this test sees
// reaches a support bundle (there with the IPv4 address redacted to its last
// octet). NO RADIO AND NO NETWORK ARE NEEDED.

#include "core/backends/hl2/Hl2Discovery.h"
#include "core/LogManager.h"

#include <QCoreApplication>
#include <QHostAddress>
#include <QLoggingCategory>
#include <QStringList>

#include <cstdio>
#include <functional>

using AetherSDR::hl2::DiscoveryNotices;

namespace {

int failures = 0;

void check(bool condition, const char* what)
{
    std::fprintf(stderr, "[%s] %s\n", condition ? " OK " : "FAIL", what);
    if (!condition)
        ++failures;
}

QStringList* g_lines = nullptr;

void capture(QtMsgType type, const QMessageLogContext& ctx, const QString& msg)
{
    if (!g_lines || !ctx.category
        || qstrcmp(ctx.category, "aether.discovery") != 0) {
        return;
    }
    *g_lines << QStringLiteral("%1|%2")
                    .arg(type == QtWarningMsg ? QStringLiteral("W")
                         : type == QtInfoMsg  ? QStringLiteral("I")
                                              : QStringLiteral("other"),
                         msg);
}

// Captures what one step logs, and nothing from the steps around it.
QStringList logged(const std::function<void()>& step)
{
    QStringList lines;
    g_lines = &lines;
    step();
    g_lines = nullptr;
    return lines;
}

const QHostAddress kAddrA(QStringLiteral("192.168.8.20"));
const QHostAddress kAddrB(QStringLiteral("169.254.1.5"));

// One sweep as Hl2Discovery reports it: a probe left kAddrA, then the reply
// window closed.
void sentSweep(DiscoveryNotices& n, bool answered)
{
    n.sendSucceeded(kAddrA);
    n.sweepClosed(answered);
}

// One socket refresh as Hl2Discovery reports it: en0 bound kAddrA, en7 kAddrB.
void bindBoth(DiscoveryNotices& n)
{
    n.bindSucceeded(QStringLiteral("en0"), kAddrA);
    n.bindSucceeded(QStringLiteral("en7"), kAddrB);
    n.endRefresh(2);
}

void bindFailureIsLoggedOncePerCondition()
{
    DiscoveryNotices n;
    const auto refreshWithFailure = [&n] {
        n.bindFailed(QStringLiteral("en0"), kAddrA,
                     QStringLiteral("The address is not available"));
        n.endRefresh(1);
    };

    const QStringList first = logged(refreshWithFailure);
    check(first == QStringList{QStringLiteral(
              "W|HL2 discovery: cannot bind en0 192.168.8.20: "
              "The address is not available")},
          "bind failure: first refresh logs one warning naming interface, address, error");

    check(logged(refreshWithFailure).isEmpty()
              && logged(refreshWithFailure).isEmpty(),
          "bind failure: the same failure on later sweeps logs nothing");

    const QStringList other = logged([&n] {
        n.bindFailed(QStringLiteral("en0"), kAddrA, QStringLiteral("x"));
        n.bindFailed(QStringLiteral("en7"), kAddrB, QStringLiteral("x"));
        n.endRefresh(1);
    });
    check(other.size() == 1 && other.first().contains(QStringLiteral("en7 169.254.1.5")),
          "bind failure: a second address fails -> only that one is logged");

    logged([&n] { n.endRefresh(1); });   // en0 not enumerated
    check(logged(refreshWithFailure).isEmpty(),
          "bind failure: an interface absent for one refresh does not re-log on return");

    logged([&n] {
        n.bindSucceeded(QStringLiteral("en0"), kAddrA);
        n.bindSucceeded(QStringLiteral("en7"), kAddrB);
        n.endRefresh(2);
    });
    check(logged(refreshWithFailure).size() == 1,
          "bind failure: a failure that bound and came back is logged again");
}

void noSocketIsLoggedOncePerCondition()
{
    DiscoveryNotices n;
    const auto emptyRefresh = [&n] { n.endRefresh(0); };

    check(logged([&n] { n.endRefresh(1); }).isEmpty(),
          "no socket: a refresh that bound a socket logs nothing");

    check(logged(emptyRefresh) == QStringList{QStringLiteral(
              "W|HL2 discovery: no usable IPv4 interface; no probe can be sent")},
          "no socket: the first empty refresh logs one warning");
    check(logged(emptyRefresh).isEmpty() && logged(emptyRefresh).isEmpty(),
          "no socket: later empty refreshes log nothing while it persists");

    logged([&n] { n.endRefresh(1); });
    check(logged(emptyRefresh).size() == 1,
          "no socket: logged again after it recovered and recurred");
}

void silenceIsLoggedOnceAfterThreshold()
{
    static_assert(DiscoveryNotices::kSilentSweepsBeforeWarning == 3,
                  "the strings below spell out the threshold");
    DiscoveryNotices n;
    logged([&n] { bindBoth(n); });
    const auto sentFromBoth = [&n] {
        n.sendSucceeded(kAddrB);
        sentSweep(n, false);
    };

    check(logged([&] {
              sentFromBoth();
              sentFromBoth();
          }).isEmpty(),
          "no reply: two silent sweeps log nothing");
    check(logged(sentFromBoth) == QStringList{QStringLiteral(
              "I|HL2 discovery: no Hermes-Lite 2 answered 3 sweeps on 2 interfaces")},
          "no reply: the third silent sweep logs one Info line with the interface count");
    check(logged([&n] {
              for (int i = 0; i < 20; ++i)
                  sentSweep(n, false);
          }).isEmpty(),
          "no reply: continued silence logs nothing more");

    check(logged([&n] {
              sentSweep(n, true);
              sentSweep(n, false);
              sentSweep(n, false);
          }).isEmpty(),
          "no reply: an answer resets the count; two new silent sweeps log nothing");
    logged([&n] { n.endRefresh(1); });
    check(logged([&n] { sentSweep(n, false); }) == QStringList{QStringLiteral(
              "I|HL2 discovery: no Hermes-Lite 2 answered 3 sweeps on 1 interface")},
          "no reply: silence after an answer is logged again (singular interface)");

    check(logged([&n] {
              sentSweep(n, false);
              sentSweep(n, true);
              sentSweep(n, false);
              sentSweep(n, true);
          }).isEmpty(),
          "no reply: an intermittent answer never reaches the threshold");
}

// The silence line means "asked and not answered". A sweep whose every send
// failed asked nothing; the send-failure warning is its whole report.
void sweepWithNoProbeSentIsNotSilence()
{
    const QString silence = QStringLiteral("no Hermes-Lite 2 answered");
    const auto failedSweep = [](DiscoveryNotices& n) {
        n.sendFailed(kAddrA, QStringLiteral("Network unreachable"));
        n.sendFailed(kAddrB, QStringLiteral("Network unreachable"));
        n.sweepClosed(false);
    };

    DiscoveryNotices allFail;
    logged([&allFail] { allFail.endRefresh(2); });
    const QStringList failed = logged([&] {
        for (int i = 0; i < 3; ++i)
            failedSweep(allFail);
    });
    check(failed.filter(QStringLiteral("cannot send a probe from 192.168.8.20")).size() == 1
              && failed.filter(QStringLiteral("cannot send a probe from 169.254.1.5")).size() == 1,
          "no probe sent: three all-failed sweeps log each address's send warning once");
    check(failed.filter(silence).isEmpty() && failed.size() == 2,
          "no probe sent: three all-failed sweeps log no 'no Hermes-Lite 2 answered' line");
    check(logged([&] {
              for (int i = 0; i < 20; ++i)
                  failedSweep(allFail);
          }).isEmpty(),
          "no probe sent: continued all-failed sweeps log nothing more");

    DiscoveryNotices mixed;
    logged([&mixed] { bindBoth(mixed); });
    const QStringList partial = logged([&mixed] {
        for (int i = 0; i < 3; ++i) {
            mixed.sendSucceeded(kAddrA);
            mixed.sendFailed(kAddrB, QStringLiteral("Network unreachable"));
            mixed.sweepClosed(false);
        }
    });
    check(partial.filter(silence).size() == 1 && partial.size() == 2,
          "no probe sent: one socket sent, one failed -> the sweep counts as sent");
    check(partial.filter(silence).value(0).endsWith(
              QStringLiteral("answered 3 sweeps on 1 interface")),
          "no probe sent: two bound, one sent -> the silence line counts the one that sent");

    // The count is interfaces, not addresses.
    DiscoveryNotices twoAddresses;
    logged([&twoAddresses] {
        twoAddresses.bindSucceeded(QStringLiteral("en0"), kAddrA);
        twoAddresses.bindSucceeded(QStringLiteral("en0"), kAddrB);
        twoAddresses.endRefresh(2);
    });
    check(logged([&twoAddresses] {
              for (int i = 0; i < 3; ++i) {
                  twoAddresses.sendSucceeded(kAddrA);
                  twoAddresses.sendSucceeded(kAddrB);
                  twoAddresses.sweepClosed(false);
              }
          }).filter(silence).value(0).endsWith(
              QStringLiteral("answered 3 sweeps on 1 interface")),
          "no probe sent: two addresses of one interface sent -> 'on 1 interface'");

    // The mark belongs to one sweep: a probe that left sweep 1 does not make
    // the failed sweeps after it count.
    DiscoveryNotices stale;
    logged([&stale] { stale.endRefresh(1); });
    check(logged([&stale] {
              stale.sendSucceeded(kAddrA);
              stale.sweepClosed(false);
              for (int i = 0; i < 5; ++i) {
                  stale.sendFailed(kAddrA, QStringLiteral("x"));
                  stale.sweepClosed(false);
              }
          }).filter(silence).isEmpty(),
          "no probe sent: one sent sweep then failed ones never reach the threshold");
    check(logged([&stale] {
              sentSweep(stale, false);
              sentSweep(stale, false);
          }).filter(silence).size() == 1,
          "no probe sent: failed sweeps do not clear the count; two more sent ones log it");
}

void sendFailureIsLoggedOncePerCondition()
{
    DiscoveryNotices n;
    const auto failA = [&n] { n.sendFailed(kAddrA, QStringLiteral("Network unreachable")); };

    check(logged(failA) == QStringList{QStringLiteral(
              "W|HL2 discovery: cannot send a probe from 192.168.8.20: Network unreachable")},
          "send failure: the first failed sweep logs one warning naming address and error");
    check(logged(failA).isEmpty(),
          "send failure: the same address failing on later sweeps logs nothing");

    const QStringList other = logged([&n] { n.sendFailed(kAddrB, QStringLiteral("x")); });
    check(other.size() == 1 && other.first().contains(QStringLiteral("from 169.254.1.5")),
          "send failure: another address is its own condition");

    check(logged([&n] { n.sendSucceeded(kAddrA); }).isEmpty() && logged(failA).size() == 1,
          "send failure: a failure after a probe left that address is logged again");
}

void resetForgetsEverything()
{
    DiscoveryNotices n;
    logged([&n] {
        n.bindFailed(QStringLiteral("en0"), kAddrA, QStringLiteral("x"));
        n.endRefresh(0);
        n.sendFailed(kAddrB, QStringLiteral("x"));
        for (int i = 0; i < 3; ++i)
            sentSweep(n, false);
    });
    n.reset();
    const QStringList again = logged([&n] {
        n.bindFailed(QStringLiteral("en0"), kAddrA, QStringLiteral("x"));
        n.endRefresh(0);
        n.sendFailed(kAddrB, QStringLiteral("x"));
        for (int i = 0; i < 3; ++i)
            sentSweep(n, false);
    });
    check(again.size() == 4,
          "reset (Hl2Discovery::stop): all four conditions are logged again");
}

}  // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    // LogManager::applyFilterRules() suppresses Debug on every aether.*
    // category by default; a warning has to survive that.
    QLoggingCategory::setFilterRules(QStringLiteral("aether.*.debug=false"));
    const QtMessageHandler previous = qInstallMessageHandler(capture);

    // Control: the capture sees the category, and the filter rule is live.
    const QStringList control = logged([] {
        qCDebug(AetherSDR::lcDiscovery) << "control debug";
        qCWarning(AetherSDR::lcDiscovery) << "control warning";
    });
    check(control.size() == 1 && control.first().startsWith(QStringLiteral("W|")),
          "control: an aether.discovery warning is captured, its debug is filtered");

    bindFailureIsLoggedOncePerCondition();
    noSocketIsLoggedOncePerCondition();
    silenceIsLoggedOnceAfterThreshold();
    sweepWithNoProbeSentIsNotSilence();
    sendFailureIsLoggedOncePerCondition();
    resetForgetsEverything();

    qInstallMessageHandler(previous);
    std::fprintf(stderr, "%s: %d failure(s)\n",
                 failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
