#include "models/NetworkRadioLookup.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QList>
#include <QMap>
#include <QStringList>
#include <QTimer>

#include <cstdlib>
#include <iostream>
#include <memory>

using AetherSDR::NetworkRadioLookup;
namespace {
void require(bool condition, const char* message)
{
    if (!condition) { std::cerr << message << '\n'; std::exit(1); }
}
struct ResolverHarness {
    int nextId = 0;
    QStringList hosts;
    QList<int> cancelled;
    QMap<int, std::function<void(const QHostInfo&)>> pending;
    NetworkRadioLookup::Resolver resolver()
    {
        return [this](const QString& host, QObject*, std::function<void(const QHostInfo&)> callback) {
            hosts.append(host);
            const int id = ++nextId;
            pending.insert(id, std::move(callback));
            return id;
        };
    }
    NetworkRadioLookup::Canceller canceller() { return [this](int id) { cancelled.append(id); }; }
    void reply(int id, const QList<QHostAddress>& addresses, QHostInfo::HostInfoError error = QHostInfo::NoError)
    {
        QHostInfo info;
        info.setAddresses(addresses);
        info.setError(error);
        pending.value(id)(info);
    }
};
}

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    ResolverHarness harness;
    NetworkRadioLookup lookup(harness.resolver(), harness.canceller(), 10000);
    QList<NetworkRadioLookup::Result> results;
    const auto completed = [&](NetworkRadioLookup::Result result) { results.append(result); };
    lookup.start(QStringLiteral(" radio.local "), completed);
    harness.reply(1, {QHostAddress(QStringLiteral("::1")), QHostAddress(QStringLiteral("192.0.2.1"))});
    require(harness.hosts.last() == "radio.local" && results.size() == 1
        && results.last().error == NetworkRadioLookup::Error::None
        && results.last().address == QHostAddress(QStringLiteral("192.0.2.1")), "select an IPv4 answer from a successful lookup");
    lookup.start(QStringLiteral("old.local"), completed);
    lookup.start(QStringLiteral("new.local"), completed);
    require(harness.cancelled.contains(2), "replacement aborts the previous resolver request");
    harness.reply(2, {QHostAddress(QStringLiteral("192.0.2.2"))});
    require(results.size() == 1, "late replaced answers cannot complete a newer request");
    harness.reply(3, {QHostAddress(QStringLiteral("192.0.2.3"))}, QHostInfo::HostNotFound);
    require(results.size() == 2 && results.last().error == NetworkRadioLookup::Error::NoIpv4Address
        && results.last().address.isNull(), "resolver errors cannot publish attached addresses");
    lookup.start(QStringLiteral("ipv6.local"), completed);
    harness.reply(4, {QHostAddress(QStringLiteral("::1"))});
    require(results.last().error == NetworkRadioLookup::Error::NoIpv4Address, "IPv6-only answer is explicitly unsupported");
    const int calls = harness.hosts.size();
    lookup.start(QString(), completed);
    lookup.start(QString(254, 'a'), completed);
    require(harness.hosts.size() == calls && results.last().error == NetworkRadioLookup::Error::InvalidHost,
        "invalid host bounds are rejected before DNS");
    lookup.start(QStringLiteral("192.0.2.4"), completed);
    require(harness.hosts.size() == calls && results.last().error == NetworkRadioLookup::Error::None,
        "numeric IPv4 bypasses DNS");
    lookup.start(QStringLiteral("::1"), completed);
    require(harness.hosts.size() == calls && results.last().error == NetworkRadioLookup::Error::NoIpv4Address,
        "numeric IPv6 cannot enter the IPv4 connection path");
    lookup.start(QStringLiteral("cancel.local"), completed);
    lookup.cancel();
    const qsizetype beforeCancelReply = results.size();
    harness.reply(5, {QHostAddress(QStringLiteral("192.0.2.5"))});
    require(results.size() == beforeCancelReply && harness.cancelled.contains(5), "cancel discards completion and fences late answers");
    {
        QEventLoop loop;
        NetworkRadioLookup deadline(harness.resolver(), harness.canceller(), 1);
        deadline.start(QStringLiteral("timeout.local"), [&](NetworkRadioLookup::Result result) {
            completed(result);
            loop.quit();
        });
        QTimer::singleShot(1000, &loop, &QEventLoop::quit);
        loop.exec();
        require(results.size() == beforeCancelReply + 1 && results.last().error == NetworkRadioLookup::Error::TimedOut
            && harness.cancelled.contains(6), "deadline completes once and aborts the resolver request");
        harness.reply(6, {QHostAddress(QStringLiteral("192.0.2.6"))});
        require(results.size() == beforeCancelReply + 1, "late success cannot undo a lookup timeout");
    }
    {
        auto mortal = std::make_unique<NetworkRadioLookup>(harness.resolver(), harness.canceller(), 10000);
        mortal->start(QStringLiteral("destroy.local"), completed);
        mortal.reset();
        const qsizetype beforeDestructionReply = results.size();
        harness.reply(7, {QHostAddress(QStringLiteral("192.0.2.7"))});
        require(harness.cancelled.contains(7) && results.size() == beforeDestructionReply,
            "destruction cancels DNS and makes retained resolver callbacks inert");
    }
    {
        auto mortal = std::make_unique<NetworkRadioLookup>(harness.resolver(), harness.canceller(), 10000);
        mortal->start(QStringLiteral("callback.local"), [&](NetworkRadioLookup::Result) { mortal.reset(); });
        harness.reply(8, {QHostAddress(QStringLiteral("192.0.2.8"))});
        require(!mortal, "completion may destroy its lookup owner");
    }
    {
        std::unique_ptr<NetworkRadioLookup> reentrant;
        std::function<void(const QHostInfo&)> second;
        QList<int> cancelled;
        int finished = 0;
        reentrant = std::make_unique<NetworkRadioLookup>(
            [&](const QString& host, QObject*, std::function<void(const QHostInfo&)> callback) {
                if (host == "first.local") {
                    QHostInfo info;
                    info.setAddresses({QHostAddress(QStringLiteral("192.0.2.9"))});
                    callback(info);
                    return 1;
                }
                second = std::move(callback);
                return 2;
            }, [&](int id) { cancelled.append(id); }, 10000);
        reentrant->start(QStringLiteral("first.local"), [&](NetworkRadioLookup::Result) {
            ++finished;
            reentrant->start(QStringLiteral("second.local"), [&](NetworkRadioLookup::Result) { ++finished; });
        });
        QHostInfo info;
        info.setAddresses({QHostAddress(QStringLiteral("192.0.2.10"))});
        second(info);
        require(finished == 2 && !cancelled.contains(2), "synchronous completion cannot overwrite a reentrant request's resolver identity");
    }
    {
        bool reentered = false;
        int newestCompletions = 0;
        int supersededCompletions = 0;
        NetworkRadioLookup* target = nullptr;
        NetworkRadioLookup live(harness.resolver(), [&](int id) {
            harness.cancelled.append(id);
            if (!reentered) {
                reentered = true;
                target->start(QStringLiteral("newest.local"), [&](NetworkRadioLookup::Result) { ++newestCompletions; });
            }
        }, 10000);
        target = &live;
        live.start(QStringLiteral("retire.local"), completed);
        live.start(QStringLiteral("superseded.local"), [&](NetworkRadioLookup::Result) { ++supersededCompletions; });
        require(harness.hosts.last() == "newest.local", "reentrant cancellation keeps the newest start authoritative");
        harness.reply(harness.nextId, {QHostAddress(QStringLiteral("192.0.2.11"))});
        require(newestCompletions == 1 && supersededCompletions == 0,
            "outer cancellation cannot replace the request created by its callback");
    }
    std::cout << "Network radio lookup checks passed\n";
}
