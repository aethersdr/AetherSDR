// A directed (unicast) discovery target finds an HL2 a broadcast cannot reach.
//
// A fake HL2 listens on 127.0.0.1:1024. The loopback is never swept by
// Hl2Discovery, so the fake can be found only by the unicast probe sent to it
// from a real interface. Without a configured target nothing is found. Needs
// one up non-loopback IPv4 interface; skips (77) without one. No radio is used.

#include "core/backends/hl2/Hl2Discovery.h"
#include "core/backends/hl2/MetisProtocol.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QUdpSocket>

#include <cstdio>

using AetherSDR::hl2::Hl2Discovery;

namespace {

int failures = 0;

void check(bool condition, const char* what)
{
    std::fprintf(stderr, "[%s] %s\n", condition ? " OK " : "FAIL", what);
    if (!condition)
        ++failures;
}

bool haveLanInterface()
{
    for (const QNetworkInterface& iface : QNetworkInterface::allInterfaces()) {
        const auto flags = iface.flags();
        if (!flags.testFlag(QNetworkInterface::IsUp) || !flags.testFlag(QNetworkInterface::IsRunning)
            || flags.testFlag(QNetworkInterface::IsLoopBack)) {
            continue;
        }
        for (const QNetworkAddressEntry& entry : iface.addressEntries()) {
            if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol)
                return true;
        }
    }
    return false;
}

// Runs one sweep and returns how many times the fake HL2 was discovered.
int sweepFinds(QUdpSocket& fake, const QList<QHostAddress>& targets)
{
    Hl2Discovery discovery;
    discovery.setDirectedTargets(targets);
    int found = 0;
    // Only the fake counts: a real HL2 on the LAN may answer the broadcast too.
    const QString fakeSerial = Hl2Discovery::macToSerial({0x00, 0x1c, 0xc0, 0xde, 0xad, 0x01});
    QObject::connect(&discovery, &Hl2Discovery::radioDiscovered, [&](const AetherSDR::RadioInfo& info) {
        if (info.serial == fakeSerial)
            ++found;
    });
    discovery.sweepNow();
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < 400) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        while (fake.hasPendingDatagrams()) {
            const QNetworkDatagram dg = fake.receiveDatagram();
            if (dg.data().size() >= 3 && quint8(dg.data()[0]) == 0xEF && quint8(dg.data()[1]) == 0xFE
                && quint8(dg.data()[2]) == 0x02) {
                QByteArray reply(60, '\0');
                reply[0] = char(0xEF);
                reply[1] = char(0xFE);
                reply[2] = char(0x02);
                const unsigned char mac[6] = {0x00, 0x1c, 0xc0, 0xde, 0xad, 0x01};
                for (int i = 0; i < 6; ++i)
                    reply[3 + i] = char(mac[i]);
                reply[9] = char(74);
                reply[10] = char(6); // Hermes-Lite 2 board id
                fake.writeDatagram(reply, dg.senderAddress(), dg.senderPort());
            }
        }
    }
    discovery.stop();
    return found;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (!haveLanInterface()) {
        std::fprintf(stderr, "skip: no up non-loopback IPv4 interface\n");
        return 77;
    }
    QUdpSocket fake;
    if (!fake.bind(QHostAddress::LocalHost, AetherSDR::hl2::kMetisPort)) {
        std::fprintf(stderr, "skip: cannot bind the fake HL2 (%s)\n", qPrintable(fake.errorString()));
        return 77;
    }

    check(sweepFinds(fake, {}) == 0, "no directed target: the loopback HL2 is not found by broadcast");
    check(sweepFinds(fake, {QHostAddress(QStringLiteral("not an address")), QHostAddress::AnyIPv6}) == 0,
          "invalid and non-IPv4 targets are ignored");
    check(sweepFinds(fake, {QHostAddress::LocalHost}) == 1, "a directed target finds the HL2");
    check(sweepFinds(fake, {QHostAddress::LocalHost, QHostAddress::LocalHost}) == 1,
          "a repeated target still yields one radio");
    return failures == 0 ? 0 : 1;
}
