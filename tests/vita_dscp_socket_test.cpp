#include "core/backends/flex/VitaDscpMarker.h"
#include "core/backends/flex/VitaDscpFlow.h"

#include <QCoreApplication>
#include <QNetworkDatagram>
#include <QUdpSocket>
#include <QVariant>
#include <cstdio>

using namespace AetherSDR;

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QUdpSocket sink;
    QUdpSocket sender;
    if (!sink.bind(QHostAddress::LocalHost, 0)) {
        std::fprintf(stderr, "SKIP: cannot bind the loopback UDP sink\n");
        return 77;
    }
    VitaDscpMarker marker;
    for (int cycle = 0; cycle < 2; ++cycle) {
        if (!sender.bind(QHostAddress::LocalHost, 0)) {
            std::fprintf(stderr, "SKIP: cannot bind the loopback UDP sender\n");
            return 77;
        }
        const VitaDscpResult result = marker.apply(sender.socketDescriptor(), QHostAddress::LocalHost,
                                                   {sink.localPort()});
        std::printf("cycle %d: EF %s: %s\n", cycle, result.applied ? "configured" : "unavailable",
                    qPrintable(result.detail));
        // POSIX native acceptance/readback is the assertion of this test; denial
        // remains a failure. Injected denial is covered by vita_dscp_stream_test.
#ifndef Q_OS_WIN
        if (!result.applied || sender.socketOption(QAbstractSocket::TypeOfServiceOption).toInt() != kVitaIpTosEf) {
            std::fprintf(stderr, "FAIL: native IPv4 EF marking/readback\n");
            return 1;
        }
#endif
        const QByteArray payload = QByteArrayLiteral("local QoS probe") + QByteArray::number(cycle);
        if (sender.writeDatagram(payload, QHostAddress::LocalHost, sink.localPort()) != payload.size()
            || !sink.waitForReadyRead(1000) || sink.receiveDatagram().data() != payload) {
            std::fprintf(stderr, "FAIL: socket must still carry data regardless of QoS outcome\n");
            return 1;
        }
        marker.reset();
        sender.close();
    }
    const VitaDscpResult invalid = marker.apply(-1, QHostAddress::LocalHost, {sink.localPort()});
    if (invalid.applied || invalid.detail.isEmpty()) {
        std::fprintf(stderr, "FAIL: absent descriptor must report an explained best-effort failure\n");
        return 1;
    }
    return 0;
}
