// Kpa1500Discovery: the sweep-target arithmetic, and a sweep that finds a
// stub KPA1500 answering ^SN/^RV over UDP on loopback.

#include "TestSettingsProfile.h"

#include "core/Kpa1500Discovery.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QHostAddress>
#include <QNetworkDatagram>
#include <QSignalSpy>
#include <QUdpSocket>

#include <cstdio>
#include <functional>

using namespace AetherSDR;

namespace {

int g_failed = 0;

void report(const char* name, bool ok)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", name);
    if (!ok) {
        ++g_failed;
    }
}

bool spin(std::function<bool()> done, int timeoutMs = 8000)
{
    QDeadlineTimer deadline(timeoutMs);
    while (!done() && !deadline.hasExpired()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return done();
}

}  // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("kpa1500-discovery-test"));
    QCoreApplication app(argc, argv);

    // ── Sweep targets ──────────────────────────────────────────────────────
    {
        const QHostAddress self(QStringLiteral("192.168.1.60"));
        const auto t24 = Kpa1500Discovery::sweepTargets(self, 24);
        report("/24: 253 hosts (network, broadcast and self excluded)", t24.size() == 253);
        report("/24: starts at .1 and ends at .254",
               t24.first() == QHostAddress(QStringLiteral("192.168.1.1"))
                   && t24.last() == QHostAddress(QStringLiteral("192.168.1.254")));
        report("/24: own address excluded", !t24.contains(self));
        report("/22: whole subnet, 1021 hosts",
               Kpa1500Discovery::sweepTargets(self, 22).size() == 1021);
        const auto t16 = Kpa1500Discovery::sweepTargets(self, 16);
        report("/16: only the /24 around this computer", t16.size() == 253
               && t16.first() == QHostAddress(QStringLiteral("192.168.1.1")));
        report("loopback is never swept",
               Kpa1500Discovery::sweepTargets(QHostAddress::LocalHost, 8).isEmpty());
        report("IPv6 is never swept",
               Kpa1500Discovery::sweepTargets(QHostAddress::LocalHostIPv6, 64).isEmpty());
        report("/31 and /32 yield nothing",
               Kpa1500Discovery::sweepTargets(self, 31).isEmpty()
                   && Kpa1500Discovery::sweepTargets(self, 32).isEmpty());
    }

    // ── Finds a stub amp ───────────────────────────────────────────────────
    {
        QUdpSocket amp;
        amp.bind(QHostAddress::LocalHost, 0);
        QList<QByteArray> received;
        QObject::connect(&amp, &QUdpSocket::readyRead, [&amp, &received]() {
            while (amp.hasPendingDatagrams()) {
                const QNetworkDatagram d = amp.receiveDatagram();
                received.append(d.data());
                QByteArray reply;
                if (d.data() == "^SN;") { reply = "^SN00022;"; }
                if (d.data() == "^RV;") { reply = "^RV03.10;"; }
                if (!reply.isEmpty()) {
                    amp.writeDatagram(reply, d.senderAddress(), static_cast<quint16>(d.senderPort()));
                }
            }
        });
        // A second, silent "host" that never answers.
        QUdpSocket silent;
        silent.bind(QHostAddress::LocalHost, 0);

        Kpa1500Discovery discovery;
        QSignalSpy foundSpy(&discovery, &Kpa1500Discovery::found);
        QSignalSpy finishedSpy(&discovery, &Kpa1500Discovery::finished);
        discovery.startWithTargets({QHostAddress::LocalHost}, amp.localPort());

        report("sweep finishes", spin([&] { return finishedSpy.count() == 1; }));
        report("sweep reports one amplifier",
               finishedSpy.count() == 1 && finishedSpy.first().first().toInt() == 1
                   && foundSpy.count() == 1);
        const auto results = discovery.results();
        report("result carries the address", !results.isEmpty()
               && results.first().address == QHostAddress(QHostAddress::LocalHost));
        report("result carries the serial number",
               !results.isEmpty() && results.first().serialNumber == QStringLiteral("00022"));
        report("result carries the firmware",
               !results.isEmpty() && results.first().firmware == QStringLiteral("03.10"));
        report("only queries were sent",
               std::all_of(received.cbegin(), received.cend(), [](const QByteArray& f) {
                   return f == "^SN;" || f == "^RV;";
               }));

        // Nothing answering: finishes with zero.
        Kpa1500Discovery none;
        QSignalSpy noneFinished(&none, &Kpa1500Discovery::finished);
        none.startWithTargets({QHostAddress::LocalHost}, silent.localPort());
        report("no amplifier: finishes with zero",
               spin([&] { return noneFinished.count() == 1; })
                   && noneFinished.first().first().toInt() == 0 && none.results().isEmpty());
    }

    if (g_failed == 0) {
        std::printf("\nAll KPA1500 discovery tests passed.\n");
    } else {
        std::printf("\n%d KPA1500 discovery test(s) FAILED.\n", g_failed);
    }
    return g_failed == 0 ? 0 : 1;
}
