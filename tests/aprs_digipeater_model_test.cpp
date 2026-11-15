// Injected frames and a socket-free queue; no DSP, peer, or radio.
#include "models/AprsDigipeaterModel.h"
#include <QCoreApplication>
#include <cstdio>
using namespace AetherSDR;
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    int failures = 0;
    const auto check = [&](bool pass, const char* reason) {
        if (!pass) { std::fprintf(stderr, "FAIL: %s\n", reason); ++failures; }
    };
    AprsDigipeaterModel model;
    const auto frame = ax25::Frame::makeUI(*ax25::Address::parse("APRS"),
        *ax25::Address::parse("K1ABC-9"), {*ax25::Address::parse("WIDE1-1")}, ">test");
    check(!model.isEnabled(), "starts disarmed");
    model.setBaud(1200);
    model.setEnabled(true);
    check(!model.isEnabled(), "invalid callsign refuses arming");
    model.setMyAddress(*ax25::Address::parse("N0CALL-7"));
    model.setBaud(300);
    model.setEnabled(true);
    check(!model.isEnabled(), "HF refuses arming");
    model.receiveFrame(frame.encode());
    check(model.isEmpty(), "disarmed RX does not enqueue");
    model.setBaud(1200);
    model.setEnabled(true);
    model.receiveFrame(frame.encode());
    check(model.size() == 1, "armed fill-in enqueues");
    model.enqueue("terminal");
    model.setManualPosition(35.0, -78.0, true);
    model.setBeaconEnabled(true);
    check(model.sendBeaconNow(), "armed beacon is generated");
    check(model.size() == 3, "beacon shares queue");
    int disarms = 0;
    QObject::connect(&model, &AprsDigipeaterModel::disarmed, [&] { ++disarms; });
    model.setEnabled(false);
    check(disarms == 1, "disarm notifies active TX consumer");
    check(model.size() == 1, "disarm removes repeats and beacons only");
    const auto other = model.dequeue();
    check(other.raw == "terminal" && !other.digi, "unrelated producer preserved");
    check(!model.beaconEnabled() && !model.sendBeaconNow(), "disarm stops beacon");
    model.enqueue("stale digi", true);
    check(model.isEmpty(), "late digi delivery refused");
    model.setEnabled(true);
    for (int i = 0; i < 70; ++i) {
        model.enqueue(QByteArray::number(i), i % 2 == 0);
    }
    check(model.size() == 64, "all producers share capacity bound");
    check(model.dequeue().raw == "6", "overflow discards oldest");
    model.setBaud(300);
    check(!model.isEnabled() && disarms == 2, "profile switch disarms");
    while (!model.isEmpty()) {
        check(!model.dequeue().digi, "profile switch purges only digi");
    }
    model.setBaud(1200);
    check(!model.isEnabled(), "returning to VHF does not rearm");
    model.setEnabled(true);
    model.setMyAddress(*ax25::Address::parse("N0CALL-8"));
    check(!model.isEnabled(), "identity change disarms");
    return failures ? 1 : 0;
}
