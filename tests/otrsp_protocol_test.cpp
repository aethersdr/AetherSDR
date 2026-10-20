#include "TestSettingsProfile.h"
#include "core/OtrspProtocol.h"
#include "core/backends/flex/FlexBackend.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"
#include "models/TransmitModel.h"

#include <QCoreApplication>
#include <QSignalSpy>
#include <QStringList>

#include <cstdio>
#include <memory>

using namespace AetherSDR;

namespace {

int failures = 0;

void check(bool ok, const char* message)
{
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

class ConnectedBackend final : public FlexBackend {
public:
    bool connected{true};
    bool isConnected() const override { return connected; }
};

struct Fixture {
    QStringList commands;
    RadioModel radio;
    ConnectedBackend* backend;

    Fixture()
    {
        auto owned = std::make_unique<ConnectedBackend>();
        backend = owned.get();
        backend->setCommandSink([this](const QString& command) { commands.append(command); });
        backend->setSliceCommandSink([this](const QString& command) { commands.append(command); });
        radio.setBackendForTest(std::move(owned), QStringLiteral("otrsp-test"));
        for (int id : {2, 5, 7}) {
            SliceDelta delta;
            delta.inUse = true;
            delta.frequency = 14.2;
            delta.mode = QStringLiteral("USB");
            delta.panId = QStringLiteral("receiver");
            delta.audioGain = 43.0;
            delta.audioMute = id != 2;
            delta.audioPan = 50;
            delta.active = id == 2;
            delta.txSlice = id == 2;
            emit backend->sliceChanged(id, delta);
        }
        commands.clear();
    }

    void observeRx(int focus, bool stereo, bool reverse)
    {
        for (int id : {2, 5}) {
            SliceDelta delta;
            delta.active = id == focus;
            delta.audioMute = !stereo && id != focus;
            delta.audioPan = stereo ? ((id == 2) != reverse ? 0 : 100) : 50;
            emit backend->sliceChanged(id, delta);
        }
    }

    void observeTx(int focus)
    {
        for (int id : {2, 5}) {
            SliceDelta delta;
            delta.txSlice = id == focus;
            emit backend->sliceChanged(id, delta);
        }
    }
};

void receiveModes()
{
    for (const QByteArray& command : {QByteArray("RX1"), QByteArray("RX2"),
                                    QByteArray("RX1S"), QByteArray("RX2S"),
                                    QByteArray("RX1R"), QByteArray("RX2R")}) {
        Fixture f;
        OtrspProtocol protocol(&f.radio, 2, 5);
        const int focus = command.at(2) == '1' ? 2 : 5;
        const int other = focus == 2 ? 5 : 2;
        const bool stereo = command.size() == 4;
        const bool reverse = stereo && command.at(3) == 'R';
        for (int id : {2, 5}) {
            SliceDelta delta;
            delta.active = id != focus;
            delta.audioMute = stereo || id == focus;
            delta.audioPan = stereo ? ((id == 2) != reverse ? 100 : 0) : 0;
            emit f.backend->sliceChanged(id, delta);
        }
        QSignalSpy active(f.radio.slice(focus), &SliceModel::activeChanged);
        QSignalSpy pan(f.radio.slice(2), &SliceModel::audioPanChanged);
        QSignalSpy tx(&f.radio.transmitModel(), &TransmitModel::transmittingChanged);
        check(protocol.processData(command + '\r').isEmpty(), "RX setters have no response");
        check(f.commands.contains(QStringLiteral("slice set %1 active=1").arg(focus)),
              "RX selects the configured slice, not hardcoded slice A/B");
        if (stereo) {
            check(f.commands.contains(QStringLiteral("slice set 2 audio_pan=%1").arg(reverse ? 100 : 0))
                      && f.commands.contains(QStringLiteral("slice set 5 audio_pan=%1").arg(reverse ? 0 : 100))
                      && f.commands.contains(QStringLiteral("slice set 2 audio_mute=0"))
                      && f.commands.contains(QStringLiteral("slice set 5 audio_mute=0")),
                  "stereo and reverse stereo pan and unmute both mapped slices");
        } else {
            check(f.commands.contains(QStringLiteral("slice set %1 audio_pan=50").arg(focus))
                      && f.commands.contains(QStringLiteral("slice set %1 audio_mute=0").arg(focus))
                      && f.commands.contains(QStringLiteral("slice set %1 audio_mute=1").arg(other)),
                  "mono centers the focused slice and mutes the other");
        }
        check(f.radio.slice(7)->audioMute() && f.radio.slice(7)->audioPan() == 50,
              "unmapped slices remain untouched");
        check(tx.isEmpty() && !f.radio.transmitModel().isTransmitting(),
              "RX never keys the transmitter");
        f.observeRx(focus, stereo, reverse);
        check(protocol.processData("?RX\r") == command + '\r', "RX query reports observed mode");
        check(!active.isEmpty(), "observed focus reaches UI notification");
        check(!stereo || !pan.isEmpty(), "observed stereo reaches UI pan notification");
        check(f.radio.slice(2)->audioGain() == 43.0f && f.radio.slice(5)->audioGain() == 43.0f,
              "focus switching preserves operator gain");
    }
}

void transmitSelection()
{
    Fixture f;
    OtrspProtocol protocol(&f.radio, 2, 5);
    QSignalSpy keyed(&f.radio.transmitModel(), &TransmitModel::transmittingChanged);
    for (const QByteArray& command : {QByteArray("TX2"), QByteArray("TX1")}) {
        f.commands.clear();
        check(protocol.processData(command + '\r').isEmpty(), "TX setters have no response");
        const int focus = command.at(2) == '1' ? 2 : 5;
        check(f.commands == QStringList{QStringLiteral("slice set %1 tx=1").arg(focus)},
              "TX emits only a slice-selection command");
        f.observeTx(focus);
        check(protocol.processData("?TX\r") == command + '\r', "TX query follows observed radio state");
    }
    check(keyed.isEmpty() && !f.radio.transmitModel().isTransmitting(), "TX selection never keys RF");
    f.radio.transmitModel().setTransmitting(true);
    f.commands.clear();
    protocol.processData("TX2\r");
    check(f.commands.isEmpty(), "cannot change transmit focus during a transmission");
    check(protocol.processData("?TX\r") == "TX1\r", "refused switch preserves observed TX focus");
    f.radio.transmitModel().setTransmitting(false);
}

void framingAndQueries()
{
    Fixture f;
    OtrspProtocol first(&f.radio, 2, 5);
    OtrspProtocol second(&f.radio, 2, 5);
    check(first.processData("?NA").isEmpty(), "fragmented command waits for CR");
    check(second.processData("?NAME\r?\r?AUX1\r") == "NAMEAetherSDR\r?\r?AUX1\r",
          "identification, ping and unknown query use OTRSP response framing");
    check(first.processData("ME\r\n?TX\r\n") == "NAMEAetherSDR\rTX1\r",
          "client buffers are isolated and CRLF is accepted");
    check(second.processData("?RX\r") == "RX1\r", "new clients query shared state without resetting it");
    first.processData("TX");
    check(f.commands.isEmpty(), "unterminated setters never act");
    first.processData("2\r");
    check(f.commands == QStringList{"slice set 5 tx=1"}, "fragmented setter executes once");
    f.observeTx(5);
    check(second.processData("?TX\r") == "TX2\r", "other clients see the same observed TX focus");
    f.observeRx(5, true, true);
    check(first.processData("?RX\r") == "RX2R\r", "queries reflect external radio/UI changes");
    SliceDelta muted;
    muted.audioMute = true;
    emit f.backend->sliceChanged(2, muted);
    emit f.backend->sliceChanged(5, muted);
    check(first.processData("?RX\r") == "?RX\r", "unrepresentable audio state is not invented");
}

void invalidInput()
{
    Fixture f;
    OtrspProtocol protocol(&f.radio, 2, 5);
    const QByteArray invalid =
        "RX0\rRX3\rRX1X\rRX1SS\rRX01\rTX0\rTX3\rTX1S\rTX01\rrx1\r"
        "AUX125\rPTT1\rCR01\rTX1;\rRX2 junk\r";
    protocol.processData(invalid);
    check(f.commands.isEmpty(), "unsupported and malformed commands produce no radio requests");
    protocol.processData(QByteArray(100000, 'X') + "TX2\r");
    check(f.commands.isEmpty(), "overlong command discards the entire frame, not just its prefix");
    check(protocol.processData("?NAME\r") == "NAMEAetherSDR\r", "parser recovers after an oversized frame");
    protocol.processData(QByteArray("TX2\0", 4) + "\rRX2\n\r");
    check(f.commands.isEmpty(), "NUL and embedded LF cannot turn invalid data into a setter");
    const QByteArray maxQuery = "?12345678901234";
    check(protocol.processData(maxQuery + '\r') == maxQuery + '\r', "15-byte command limit is inclusive");
    check(protocol.processData(maxQuery + "5\r").isEmpty(), "16-byte command exceeds OTRSP limit");
    f.backend->connected = false;
    protocol.processData("RX2S\rTX2\r");
    check(f.commands.isEmpty(), "disconnected radio refuses all focus changes");
    check(protocol.processData("?RX\r?TX\r") == "?RX\r?TX\r", "disconnected queries do not report stale focus");
    check(protocol.processData("?NAME\r") == "NAMEAetherSDR\r", "identification works without a radio");
    f.backend->connected = true;
    OtrspProtocol missing(&f.radio, 2, 99);
    missing.processData("RX1\rRX2S\rTX2\r");
    OtrspProtocol duplicate(&f.radio, 2, 2);
    duplicate.processData("RX1S\rTX1\r");
    OtrspProtocol unassigned(&f.radio, 2, -1);
    unassigned.processData("RX1R\rTX1\r");
    check(f.commands.isEmpty(), "missing, duplicate and unassigned mappings never fall back to another slice");
    OtrspProtocol absent(nullptr, 2, 5);
    check(absent.processData("RX1\rTX2\r?NAME\r") == "NAMEAetherSDR\r", "null model is safe");
}

void sliceRemovedDuringDispatch()
{
    Fixture f;
    OtrspProtocol protocol(&f.radio, 2, 5);
    QObject::connect(f.radio.slice(2), &SliceModel::audioPanChanged, &f.radio,
                     [&](int) { emit f.backend->sliceRemoved(5); });
    protocol.processData("RX1S\r");
    check(f.commands == QStringList{"slice set 2 audio_pan=0"},
          "RX stops if a synchronous notification removes a mapped slice");

    Fixture closing;
    OtrspProtocol closed(&closing.radio, 2, 5);
    QObject::connect(closing.radio.slice(2), &SliceModel::audioPanChanged, &closing.radio,
                     [&](int) { closed.close(); });
    closed.processData("RX1S\rTX2\r");
    check(closing.commands == QStringList{"slice set 2 audio_pan=0"},
          "closing a session cancels the current request and the remaining command burst");
    check(closed.processData("?NAME\r").isEmpty(), "closed sessions stay closed");
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("otrsp-protocol"));
    QCoreApplication app(argc, argv);
    if (!profile.isValid()) {
        return 2;
    }
    receiveModes();
    transmitSelection();
    framingAndQueries();
    invalidInput();
    sliceRemovedDuringDispatch();
    std::printf("OTRSP protocol: %d failures\n", failures);
    return failures ? 1 : 0;
}
