// What HL2 discovery writes to the log when it cannot find a radio (#6285
// item 3): a failed bind, no usable interface, and a run of unanswered sweeps.
//
// Socket-free. DiscoveryNotices is the policy Hl2Discovery feeds from its
// sweeps; the lines are captured off the real "aether.discovery" category,
// with the product's default filter rule installed, so a line this test sees
// is a line a support bundle gets. NO RADIO AND NO NETWORK ARE NEEDED.

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

void bindFailureIsLoggedOncePerCondition()
{
    DiscoveryNotices n;
    const auto refreshWithFailure = [&n] {
        n.beginRefresh();
        n.bindFailed(QStringLiteral("en0"), kAddrA,
                     QStringLiteral("The address is not available"));
        n.endRefresh(1, 1);
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
        n.beginRefresh();
        n.bindFailed(QStringLiteral("en0"), kAddrA, QStringLiteral("x"));
        n.bindFailed(QStringLiteral("en7"), kAddrB, QStringLiteral("x"));
        n.endRefresh(1, 1);
    });
    check(other.size() == 1 && other.first().contains(QStringLiteral("en7 169.254.1.5")),
          "bind failure: a second address fails -> only that one is logged");

    logged([&n] { n.beginRefresh(); n.endRefresh(2, 2); });   // both bind again
    check(logged(refreshWithFailure).size() == 1,
          "bind failure: a failure that cleared and came back is logged again");
}

void noSocketIsLoggedOncePerCondition()
{
    DiscoveryNotices n;
    const auto emptyRefresh = [&n] { n.beginRefresh(); n.endRefresh(0, 0); };

    check(logged([&n] { n.beginRefresh(); n.endRefresh(1, 1); }).isEmpty(),
          "no socket: a refresh that bound a socket logs nothing");

    check(logged(emptyRefresh) == QStringList{QStringLiteral(
              "W|HL2 discovery: no usable IPv4 interface; no probe can be sent")},
          "no socket: the first empty refresh logs one warning");
    check(logged(emptyRefresh).isEmpty() && logged(emptyRefresh).isEmpty(),
          "no socket: later empty refreshes log nothing while it persists");

    logged([&n] { n.beginRefresh(); n.endRefresh(1, 1); });
    check(logged(emptyRefresh).size() == 1,
          "no socket: logged again after it recovered and recurred");
}

void silenceIsLoggedOnceAfterThreshold()
{
    static_assert(DiscoveryNotices::kSilentSweepsBeforeWarning == 3,
                  "the strings below spell out the threshold");
    DiscoveryNotices n;
    logged([&n] { n.beginRefresh(); n.endRefresh(2, 2); });

    check(logged([&n] { n.sweepClosed(false); n.sweepClosed(false); }).isEmpty(),
          "no reply: two silent sweeps log nothing");
    check(logged([&n] { n.sweepClosed(false); }) == QStringList{QStringLiteral(
              "W|HL2 discovery: no Hermes-Lite 2 answered 3 sweeps on 2 interfaces")},
          "no reply: the third silent sweep logs one warning with the interface count");
    check(logged([&n] {
              for (int i = 0; i < 20; ++i)
                  n.sweepClosed(false);
          }).isEmpty(),
          "no reply: continued silence logs nothing more");

    check(logged([&n] {
              n.sweepClosed(true);
              n.sweepClosed(false);
              n.sweepClosed(false);
          }).isEmpty(),
          "no reply: an answer resets the count; two new silent sweeps log nothing");
    logged([&n] { n.beginRefresh(); n.endRefresh(1, 1); });
    check(logged([&n] { n.sweepClosed(false); }) == QStringList{QStringLiteral(
              "W|HL2 discovery: no Hermes-Lite 2 answered 3 sweeps on 1 interface")},
          "no reply: silence after an answer is logged again (singular interface)");

    check(logged([&n] {
              n.sweepClosed(false);
              n.sweepClosed(true);
              n.sweepClosed(false);
              n.sweepClosed(true);
          }).isEmpty(),
          "no reply: an intermittent answer never reaches the threshold");
}

void resetForgetsEverything()
{
    DiscoveryNotices n;
    logged([&n] {
        n.beginRefresh();
        n.bindFailed(QStringLiteral("en0"), kAddrA, QStringLiteral("x"));
        n.endRefresh(0, 0);
        for (int i = 0; i < 3; ++i)
            n.sweepClosed(false);
    });
    n.reset();
    const QStringList again = logged([&n] {
        n.beginRefresh();
        n.bindFailed(QStringLiteral("en0"), kAddrA, QStringLiteral("x"));
        n.endRefresh(0, 0);
        for (int i = 0; i < 3; ++i)
            n.sweepClosed(false);
    });
    check(again.size() == 3,
          "reset (Hl2Discovery::stop): all three conditions are logged again");
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
    resetForgetsEverything();

    qInstallMessageHandler(previous);
    std::fprintf(stderr, "%s: %d failure(s)\n",
                 failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
